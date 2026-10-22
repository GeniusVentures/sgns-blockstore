/**
 * @file s3d_spike_test.cpp
 * @brief D-06 live spike trio, C++ half (S3D-03 evidence instrument):
 *        the conditional re-PUT shape and the HEAD-missing shape at a
 *        dev-managed s3d v0.2.1 endpoint.
 *
 * Tier: S3D-TIER - dev-gated LIVE tier, NEVER in CI (TEST-01/D-08).
 * The test ONLY CONNECTS to the env-provided endpoint; it never
 * spawns, stops, or otherwise manages s3d (the Sia account is
 * dev-owned state - fundamentally heavier than the MinIO child
 * process, D-08). With any of the S3D env vars unset every case
 * loud-skips with the [SGNS_BLOCKSTORE][S3D-TIER] label naming what is
 * missing - a runtime skip, never a CMake gate.
 *
 * Evidence contract (D-13): the greppable banner lines printed here
 * are transcribed into docs/evidence/s3d-spike/sdk-error-shapes.txt by
 * the Task-2 live session (which mapper shape fired for the 412, which
 * absence path fired for HEAD-missing). The adapter absorbs the 412
 * into its SUCCESS slot before the raw SDK string is observable, so
 * ConditionalRePutShape ALSO drives one raw minio-cpp conditional PUT
 * (the same If-None-Match wire mechanism MinioS3Client uses, verified
 * in the 03-01 spike) to print the wire-path error text.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sgns-blockstore/S3Client.hpp>

// Internal adapter impl header - src/ include privilege
// (test-surface-only; installed headers stay SDK-free).
#include "MinioS3Client.hpp"

#include <miniocpp/client.h>
#include <miniocpp/providers.h>

namespace
{
    using sgns::blockstore::MinioS3Client;
    using sgns::blockstore::S3ClientConfig;
    using sgns::blockstore::S3HeadResult;
    using sgns::blockstore::S3PutResult;

    /// @brief The S3D-tier env gate: every required var must be set,
    ///        else the case loud-skips naming exactly what is missing.
    struct S3dEnv
    {
        std::string endpoint;  ///< host only (e.g. 127.0.0.1)
        uint16_t port = 0;     ///< 0 = scheme default
        std::string access_key;
        std::string secret_key;
        std::string bucket;
        bool use_tls = false;
        bool ok = false;
    };

    S3dEnv ReadS3dEnv()
    {
        S3dEnv env;
        const char* endpoint =
            std::getenv("SGNS_BLOCKSTORE_TEST_S3D_ENDPOINT");
        const char* access =
            std::getenv("SGNS_BLOCKSTORE_TEST_S3D_ACCESS_KEY");
        const char* secret =
            std::getenv("SGNS_BLOCKSTORE_TEST_S3D_SECRET_KEY");
        const char* bucket =
            std::getenv("SGNS_BLOCKSTORE_TEST_S3D_BUCKET");
        const char* use_tls =
            std::getenv("SGNS_BLOCKSTORE_TEST_S3D_USE_TLS");

        std::string missing;
        if (endpoint == nullptr || *endpoint == '\0')
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_ENDPOINT";
        }
        if (access == nullptr || *access == '\0')
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_ACCESS_KEY";
        }
        if (secret == nullptr || *secret == '\0')
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_SECRET_KEY";
        }
        if (bucket == nullptr || *bucket == '\0')
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_BUCKET";
        }
        if (!missing.empty())
        {
            std::cout << "[SGNS_BLOCKSTORE][S3D-TIER] dev-gated live tier "
                         "skipped - unset:"
                      << missing << std::endl;
            return env;
        }
        env.endpoint = endpoint;
        // The env carries host:port (e.g. 127.0.0.1:8000); split on
        // the last ':' so the adapter config gets both fields (the
        // SDK's BaseUrl takes the port separately in this path).
        const std::string raw_endpoint(endpoint);
        const auto colon = raw_endpoint.rfind(':');
        if (colon != std::string::npos && colon > 0)
        {
            env.endpoint = raw_endpoint.substr(0, colon);
            env.port = static_cast<uint16_t>(
                std::stoul(raw_endpoint.substr(colon + 1)));
        }
        env.access_key = access;
        env.secret_key = secret;
        env.bucket = bucket;
        // Plain-HTTP loopback is the lane; tlsVerify is left at its
        // default-true (an opt-off is only ever for explicit fixtures).
        env.use_tls = use_tls != nullptr && std::string(use_tls) == "1";
        env.ok = true;
        return env;
    }

    S3ClientConfig MakeConfig(const S3dEnv& env)
    {
        S3ClientConfig config;
        config.endpoint = env.endpoint;
        config.port = env.port;
        config.region = "us-east-1";
        config.useTls = env.use_tls;
        config.tlsVerify = true; // default; never off for this tier
        config.pathStyle = true;
        config.accessKey = env.access_key;
        config.secretKey = env.secret_key;
        config.bucket = env.bucket;
        return config;
    }

    /// @brief Unique-per-run spike key (so a reused dev bucket still
    ///        exercises the achieved-first path on every run).
    std::string UniqueSpikeKey()
    {
        const auto now =
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm_buffer{};
        localtime_s(&tm_buffer, &now);
        std::ostringstream stream;
        stream << "sgns-s3d-spike/" << std::put_time(&tm_buffer, "%Y%m%dT%H%M%S")
               << "-" << std::chrono::steady_clock::now().time_since_epoch().count();
        return stream.str();
    }

    const std::string kSpikePayload = "s3d spike payload - conditional create";
} // namespace

/**
 * @given a live dev-managed s3d endpoint and a unique spike key
 * @when the MinioS3Client Puts conditionally (createOnly) three times,
 *        and a raw minio-cpp conditional PUT probes the wire path
 * @then the first Put achieves, the second and third return the
 *        SUCCESS slot with alreadyPresent=true (the three-shape 412
 *        classifier absorbing s3d's precondition evidence -
 *        idempotent at the live endpoint), and the raw probe's error
 *        string is printed under the evidence banner for
 *        sdk-error-shapes.txt transcription.
 */
TEST(S3dSpike, ConditionalRePutShape)
{
    const auto env = ReadS3dEnv();
    if (!env.ok)
    {
        GTEST_SKIP() << "[SGNS_BLOCKSTORE][S3D-TIER] env gate unset";
    }
    auto client = MinioS3Client::Open(MakeConfig(env));
    ASSERT_TRUE(client);
    const std::string key = UniqueSpikeKey();
    const std::vector<uint8_t> bytes(kSpikePayload.begin(),
                                     kSpikePayload.end());

    auto first = client.value()->Put(
        key, bytes, sgns::blockstore::PutObjectOptions{ /*createOnly=*/true });
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first.value().achieved);
    EXPECT_FALSE(first.value().alreadyPresent);

    auto second = client.value()->Put(
        key, bytes, sgns::blockstore::PutObjectOptions{ /*createOnly=*/true });
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_TRUE(second.value().alreadyPresent); // 412 absorbed -> success

    auto third = client.value()->Put(
        key, bytes, sgns::blockstore::PutObjectOptions{ /*createOnly=*/true });
    ASSERT_TRUE(third) << third.error().message;
    EXPECT_TRUE(third.value().alreadyPresent); // idempotent at the endpoint

    // Wire-path evidence: the SAME conditional mechanism through the
    // raw SDK, against the now-existing key - prints the error string
    // shape s3d renders (which of the three classifier shapes fires).
    minio::s3::BaseUrl base_url(env.endpoint, env.use_tls, "us-east-1");
    base_url.port = env.port;
    base_url.virtual_style = false; // path-style, matching the adapter
    minio::creds::StaticProvider provider(env.access_key, env.secret_key);
    minio::s3::Client raw_client(base_url, &provider);
    std::istringstream stream(kSpikePayload);
    minio::s3::PutObjectArgs args(
        stream, std::optional<uint64_t>(bytes.size()), /*part_size=*/0);
    args.bucket = env.bucket;
    args.region = "us-east-1";
    args.object = key;
    args.content_type = "application/octet-stream";
    args.headers.Add("If-None-Match", "*"); // the conditional field
    auto raw = raw_client.PutObject(args);
    ASSERT_FALSE(raw); // the key exists - the precondition MUST fail
    std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][EVIDENCE] "
                 "conditional-reput raw SDK error: "
              << raw.error().String() << std::endl;
    std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][EVIDENCE] "
                 "adapter classification: alreadyPresent=true (success slot)"
              << std::endl;
}

/**
 * @given a live dev-managed s3d endpoint and a never-put key
 * @when the MinioS3Client Heads it
 * @then the SUCCESS slot reports exists=false when s3d's missing-key
 *        string classifies as authoritative absence; if it does NOT
 *        classify, the case FAILS printing the captured string and
 *        naming the D-05 contingency (extend IsAuthoritativeAbsence
 *        with exactly that string) - never softened to tolerate the
 *        mismatch silently.
 */
TEST(S3dSpike, HeadMissingShape)
{
    const auto env = ReadS3dEnv();
    if (!env.ok)
    {
        GTEST_SKIP() << "[SGNS_BLOCKSTORE][S3D-TIER] env gate unset";
    }
    auto client = MinioS3Client::Open(MakeConfig(env));
    ASSERT_TRUE(client);
    const std::string key = UniqueSpikeKey() + "/never-put";

    auto head = client.value()->Head(key);
    if (head)
    {
        EXPECT_FALSE(head.value().exists);
        std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][EVIDENCE] "
                     "head-missing classified: authoritative absence "
                     "(success slot, exists=false)"
                  << std::endl;
    }
    else
    {
        std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][EVIDENCE] "
                     "head-missing UNCLASSIFIED raw SDK error: "
                  << head.error().message << std::endl;
        FAIL() << "D-05 CONTINGENCY: s3d's HEAD-missing string did not "
                  "classify as authoritative absence (got error slot "
                  "code="
               << static_cast<int>(head.error().code)
               << "). Extend IsAuthoritativeAbsence in "
                  "src/MinioS3Client.cpp with exactly the captured "
                  "string (one line, one place) and record the decision "
                  "in docs/evidence/s3d-spike/sdk-error-shapes.txt";
    }
}
