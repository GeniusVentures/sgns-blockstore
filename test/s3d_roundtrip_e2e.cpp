/**
 * @file s3d_roundtrip_e2e.cpp
 * @brief S3D-02/SC4: the milestone's end-to-end story as a TWO-RUN
 *        fresh-process proof - run A (S3dE2E.UploadRun) drives
 *        LocalBlockStore.Put -> DrainPendingUploads -> s3d and asserts
 *        the journal reached the Accepted level; run B
 *        (S3dE2E.VerifyRun) performs the honest cold retrieval - NO
 *        local store anywhere - through a fresh S3BlockStore, CID-
 *        verified against the recorded digest.
 *
 * TWO INVOCATIONS ARE THE PROOF: this binary is designed to run as
 *
 *   s3d_roundtrip_e2e.exe --gtest_filter=S3dE2E.UploadRun
 *   s3d_roundtrip_e2e.exe --gtest_filter=S3dE2E.VerifyRun
 *
 * (two separate process invocations - running it once as one ctest
 * pass is NOT the proof). The run-pair handoff rides a plain-text
 * run-state file whose path arrives via SGNS_BLOCKSTORE_TEST_S3D_RUN_
 * STATE (cid-string, derived object key, size, timestamp, hex payload
 * digest).
 *
 * Tier: S3D-TIER - dev-gated live tier, NEVER in CI (D-08/D-09): the
 * binary ONLY CONNECTS to the env-provided endpoint (it never spawns
 * or manages s3d - the Sia account is dev-owned state) and contains no
 * child-process machinery of any kind. No fixed sleeps - connect or
 * fail fast. With any env var unset both cases loud-skip
 * [SGNS_BLOCKSTORE][S3D-TIER] naming what is missing.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <sgns-blockstore/LocalBlockStore.hpp>
#include <sgns-blockstore/S3BlockStore.hpp>
#include <sgns-blockstore/UploadDrainer.hpp>

// Internal adapter impl - src/ include privilege (test-surface-only).
#include "MinioS3Client.hpp"

#include <libp2p/crypto/sha/sha256.hpp>
#include <libp2p/multi/content_identifier_codec.hpp>

namespace
{
    using sgns::blockstore::BlockStore;
    using sgns::blockstore::Cid;
    using sgns::blockstore::CidOf;
    using sgns::blockstore::DrainPendingUploads;
    using sgns::blockstore::LocalBlockStore;
    using sgns::blockstore::LocalBlockStoreOptions;
    using sgns::blockstore::MinioS3Client;
    using sgns::blockstore::S3BlockStore;
    using sgns::blockstore::S3ClientConfig;
    using sgns::blockstore::UploadState;
    using libp2p::multi::MulticodecType;

    /// @brief The env gate for BOTH runs: the S3D endpoint quartet plus
    ///        the run-state handoff path. Names every missing var.
    struct S3dE2EEnv
    {
        std::string endpoint;
        uint16_t port = 0;
        std::string access_key;
        std::string secret_key;
        std::string bucket;
        std::string run_state_path;
        bool use_tls = false;
        bool ok = false;
    };

    S3dE2EEnv ReadEnv()
    {
        S3dE2EEnv env;
        const char* endpoint =
            std::getenv("SGNS_BLOCKSTORE_TEST_S3D_ENDPOINT");
        const char* access =
            std::getenv("SGNS_BLOCKSTORE_TEST_S3D_ACCESS_KEY");
        const char* secret =
            std::getenv("SGNS_BLOCKSTORE_TEST_S3D_SECRET_KEY");
        const char* bucket =
            std::getenv("SGNS_BLOCKSTORE_TEST_S3D_BUCKET");
        const char* run_state =
            std::getenv("SGNS_BLOCKSTORE_TEST_S3D_RUN_STATE");
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
        if (run_state == nullptr || *run_state == '\0')
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_RUN_STATE";
        }
        if (!missing.empty())
        {
            std::cout << "[SGNS_BLOCKSTORE][S3D-TIER] dev-gated live E2E "
                         "skipped - unset:"
                      << missing << std::endl;
            return env;
        }
        const std::string raw(endpoint);
        const auto colon = raw.rfind(':');
        if (colon != std::string::npos && colon > 0)
        {
            env.endpoint = raw.substr(0, colon);
            env.port = static_cast<uint16_t>(
                std::stoul(raw.substr(colon + 1)));
        }
        else
        {
            env.endpoint = raw;
        }
        env.access_key = access;
        env.secret_key = secret;
        env.bucket = bucket;
        env.run_state_path = run_state;
        env.use_tls = use_tls != nullptr && std::string(use_tls) == "1";
        env.ok = true;
        return env;
    }

    S3ClientConfig MakeConfig(const S3dE2EEnv& env)
    {
        S3ClientConfig config;
        config.endpoint = env.endpoint;
        config.port = env.port;
        config.region = "us-east-1";
        config.useTls = env.use_tls;
        config.tlsVerify = true; // default; plain-HTTP loopback lane
        config.pathStyle = true;
        config.accessKey = env.access_key;
        config.secretKey = env.secret_key;
        config.bucket = env.bucket;
        return config;
    }

    std::unique_ptr<S3BlockStore> OpenRemoteStore(const S3dE2EEnv& env)
    {
        auto client = MinioS3Client::Open(MakeConfig(env));
        if (!client)
        {
            ADD_FAILURE() << "MinioS3Client::Open failed";
            return nullptr;
        }
        auto store = S3BlockStore::Open(
            std::shared_ptr<sgns::blockstore::S3Client>(
                std::move(client.value())));
        if (!store)
        {
            ADD_FAILURE() << "S3BlockStore::Open failed";
            return nullptr;
        }
        return std::move(store.value());
    }

    std::string IsoTimestamp()
    {
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm tm_buffer{};
#ifdef _WIN32
        localtime_s(&tm_buffer, &now);
#else
        localtime_r(&now, &tm_buffer);
#endif
        std::ostringstream stream;
        stream << std::put_time(&tm_buffer, "%Y-%m-%dT%H:%M:%S");
        return stream.str();
    }

    std::string Hex(gsl::span<const uint8_t> bytes)
    {
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (const auto b : bytes)
        {
            out += kHex[b >> 4];
            out += kHex[b & 0x0F];
        }
        return out;
    }

    /// @brief The store's key derivation, recomputed for the run-state
    ///        record (evidence artifact - the key the drain produced).
    std::string DerivedKey(const Cid& cid)
    {
        auto cid_string =
            libp2p::multi::ContentIdentifierCodec::toString(cid);
        if (!cid_string)
        {
            return {};
        }
        const auto digest = cid.content_address.getHash();
        static const char* kHex = "0123456789abcdef";
        const std::string fanout{
            kHex[digest[0] >> 4], kHex[digest[0] & 0x0F]};
        return "v1/blocks/raw/" + fanout + "/" + cid_string.value();
    }
} // namespace

/**
 * @given a fresh local store (temp dir), a unique-per-run payload, and
 *        the live s3d endpoint
 * @when the block is Put locally (journal born pending), the pending
 *        set drains through DrainPendingUploads into S3BlockStore, and
 *        the journal is asserted
 * @then every drain item is a success slot, GetUploadState reports
 *        UploadedAccepted (D-04: Accepted-level ONLY - never a
 *        durability claim), the pending set is empty, and the run-state
 *        file (cid/key/size/timestamp/digest) is written for run B
 */
TEST(S3dE2E, UploadRun)
{
    const auto env = ReadEnv();
    if (!env.ok)
    {
        GTEST_SKIP() << "[SGNS_BLOCKSTORE][S3D-TIER] env gate unset";
    }

    // Fresh local store in a short build-tree-relative dir (MAX_PATH).
    const std::string dir = "test-data/s3d-e2e/upload-"
        + std::to_string(std::chrono::steady_clock::now()
                             .time_since_epoch()
                             .count());
    auto local_opened =
        LocalBlockStore::Open(LocalBlockStoreOptions{ dir });
    ASSERT_TRUE(local_opened) << local_opened.error().message;
    auto local = std::move(local_opened.value());

    // Unique-per-run payload: fixed prefix + ISO timestamp + nonce.
    std::random_device random;
    std::ostringstream payload_stream;
    payload_stream << "sgns-s3d-e2e " << IsoTimestamp() << " nonce="
                   << (random() ^ random());
    const std::string payload = payload_stream.str();
    const std::vector<uint8_t> bytes(payload.begin(), payload.end());
    const auto cid =
        CidOf(bytes, MulticodecType::Code::RAW).value();

    ASSERT_TRUE(local->Put(cid, bytes))
        << "local Put failed (journal record born pending)";

    auto remote = OpenRemoteStore(env);
    ASSERT_NE(remote, nullptr);
    auto drained = DrainPendingUploads(*local, *remote);
    ASSERT_TRUE(drained) << drained.error().message;
    ASSERT_EQ(drained.value().size(), 1u);
    ASSERT_TRUE(drained.value()[0].upload)
        << "drain upload failed"; // success slot - achieved (first put)
    ASSERT_TRUE(drained.value()[0].journal)
        << "journal mark failed";

    auto state = local->GetUploadState(cid);
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_EQ(state.value(), UploadState::UploadedAccepted);
    auto pending = local->GetPendingUploads();
    ASSERT_TRUE(pending) << pending.error().message;
    EXPECT_TRUE(pending.value().empty());

    // Run-state handoff + evidence banners (run B and the session log
    // consume these).
    const std::string cid_string =
        libp2p::multi::ContentIdentifierCodec::toString(cid).value();
    const std::string key = DerivedKey(cid);
    auto digest = libp2p::crypto::sha256(bytes);
    ASSERT_TRUE(digest);
    const std::string digest_hex = Hex(digest.value());
    std::ofstream state_file(env.run_state_path, std::ios::trunc);
    ASSERT_TRUE(state_file.is_open())
        << "cannot write run-state file: " << env.run_state_path;
    state_file << "cid=" << cid_string << "\n";
    state_file << "key=" << key << "\n";
    state_file << "size=" << bytes.size() << "\n";
    state_file << "timestamp=" << IsoTimestamp() << "\n";
    state_file << "digest=" << digest_hex << "\n";
    state_file.close();

    std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][E2E-A] run A complete: "
                 "journal=UploadedAccepted pending=0"
              << std::endl;
    std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][E2E-A] cid=" << cid_string
              << std::endl;
    std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][E2E-A] key=" << key
              << std::endl;
    std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][E2E-A] size=" << bytes.size()
              << " digest=" << digest_hex << std::endl;
}

/**
 * @given run A's recorded state (cid/key/size/digest) and the live
 *        endpoint - and NOTHING else: this run constructs zero local
 *        state of any kind (the honest cold-retrieval proof, D-09)
 * @when a fresh remote store Gets the recorded CID
 * @then the fetch succeeds with CID verification on, the bytes match
 *        the recorded size AND the recorded sha-256 digest (byte
 *        identity across two fresh processes)
 */
TEST(S3dE2E, VerifyRun)
{
    const auto env = ReadEnv();
    if (!env.ok)
    {
        GTEST_SKIP() << "[SGNS_BLOCKSTORE][S3D-TIER] env gate unset";
    }

    // Read run A's handoff.
    std::ifstream state_file(env.run_state_path);
    ASSERT_TRUE(state_file.is_open())
        << "run-state file missing (run A has not run): "
        << env.run_state_path;
    std::string cid_string;
    std::string recorded_key;
    uint64_t recorded_size = 0;
    std::string recorded_digest;
    std::string line;
    while (std::getline(state_file, line))
    {
        if (line.rfind("cid=", 0) == 0)
        {
            cid_string = line.substr(4);
        }
        else if (line.rfind("key=", 0) == 0)
        {
            recorded_key = line.substr(4);
        }
        else if (line.rfind("size=", 0) == 0)
        {
            recorded_size = std::stoull(line.substr(5));
        }
        else if (line.rfind("digest=", 0) == 0)
        {
            recorded_digest = line.substr(7);
        }
    }
    ASSERT_FALSE(cid_string.empty()) << "run-state lacks cid";
    ASSERT_FALSE(recorded_digest.empty()) << "run-state lacks digest";

    auto cid_parsed =
        libp2p::multi::ContentIdentifierCodec::fromString(cid_string);
    ASSERT_TRUE(cid_parsed) << "recorded cid does not parse";

    // Fresh remote composition; retrieval only.
    auto remote = OpenRemoteStore(env);
    ASSERT_NE(remote, nullptr);
    auto got = remote->Get(cid_parsed.value());
    ASSERT_TRUE(got) << got.error().message; // CID-verified by the store
    EXPECT_EQ(got.value().bytes.size(), recorded_size);
    auto digest = libp2p::crypto::sha256(got.value().bytes);
    ASSERT_TRUE(digest);
    EXPECT_EQ(Hex(digest.value()), recorded_digest);

    std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][E2E-B] run B complete: "
                 "cold retrieval CID-verified, byte-identical"
              << std::endl;
    std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][E2E-B] cid=" << cid_string
              << " size=" << recorded_size << std::endl;
    std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][E2E-B] key=" << recorded_key
              << std::endl;
    std::cout << "[SGNS_BLOCKSTORE][S3D-TIER][E2E-B] verdict=IDENTITY_"
                 "PROVEN_TWO_FRESH_PROCESSES"
              << std::endl;
}
