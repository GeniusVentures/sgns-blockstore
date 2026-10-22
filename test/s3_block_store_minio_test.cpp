/**
 * @file s3_block_store_minio_test.cpp
 * @brief S3D-01/SC3: the REAL MinioS3Client -> S3BlockStore composition
 *        proven against the env-gated local MinIO binary (RELEASE.2025-
 *        04-22 community build via SGNS_BLOCKSTORE_TEST_MINIO_BIN).
 *
 * Tier: opt-in MINIO-TIER (runtime env gate, never a CMake gate, no
 * Docker, no auto-download). The harness spawns the binary on loopback
 * with plaintext HTTP (TLS was proven live in Phase 3; store semantics
 * add nothing TLS-shaped). With the env unset every case loud-skips
 * [SGNS_BLOCKSTORE][MINIO-TIER] - CI-safe by construction.
 *
 * Never use the license-gated W:\gnus\minio.exe (AIStor build) - this
 * tier runs exclusively on the community binary.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <sgns-blockstore/S3BlockStore.hpp>

// Internal adapter impl - src/ include privilege (test-surface-only).
#include "MinioS3Client.hpp"

#include "MinioServerHarness.hpp"

#include <libp2p/multi/content_identifier_codec.hpp>

namespace
{
    using sgns::blockstore::BlockStore;
    using sgns::blockstore::Cid;
    using sgns::blockstore::CidOf;
    using sgns::blockstore::MinioS3Client;
    using sgns::blockstore::S3BlockStore;
    using sgns::blockstore::S3ClientConfig;
    using libp2p::multi::MulticodecType;

    gsl::span<const uint8_t> SpanOf(const std::string& s)
    {
        return gsl::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    Cid MakeCid(const std::string& payload)
    {
        return CidOf(SpanOf(payload), MulticodecType::Code::RAW).value();
    }

    /// @brief The exact store key derivation, recomputed in-test (never a
    ///        pasted literal here - only the hermetic suite pins the
    ///        known vector; this tier DERIVES and observes at the real
    ///        endpoint, proving the layout survives a real server).
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

    /// @brief One live fixture: harness + bucket + two independently
    ///        constructed stores over the SAME real endpoint (no shared
    ///        client state between the put side and the get side).
    class S3BlockStoreMinioTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            if (!sgns::blockstore::testsupport::MinioServerHarness::
                    Available())
            {
                GTEST_SKIP() << "[SGNS_BLOCKSTORE][MINIO-TIER] binary not "
                                "provided (set SGNS_BLOCKSTORE_TEST_MINIO_"
                                "BIN; community build - never the "
                                "license-gated minio.exe)";
            }
            std::string error;
            if (!harness_.Start(/*tls=*/false, &error))
            {
                GTEST_SKIP() << "[SGNS_BLOCKSTORE][MINIO-TIER] fixture "
                                "unavailable: "
                             << error;
            }
            ASSERT_TRUE(harness_.MakeBucket("store-bucket", &error))
                << error;
            put_store_ = OpenStore();
            get_store_ = OpenStore();
        }

        std::unique_ptr<S3BlockStore> OpenStore()
        {
            S3ClientConfig config;
            config.endpoint = "127.0.0.1";
            config.port = harness_.Port();
            config.region = "us-east-1";
            config.useTls = false;
            config.tlsVerify = true; // default; plaintext lane unaffected
            config.pathStyle = true;
            config.accessKey = harness_.AccessKey();
            config.secretKey = harness_.SecretKey();
            config.bucket = "store-bucket";
            auto client = MinioS3Client::Open(config);
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

        sgns::blockstore::testsupport::MinioServerHarness harness_;
        std::unique_ptr<S3BlockStore> put_store_;
        std::unique_ptr<S3BlockStore> get_store_;
    };
} // namespace

/**
 * @given a payload and two store instances over independent clients
 * @when one Puts and the OTHER Gets
 * @then the round trip is byte-identical with CID verification on -
 *        no in-process caching shortcut is possible
 */
TEST_F(S3BlockStoreMinioTest, PutGetRoundtripCidVerified)
{
    const std::string payload = "minio-tier roundtrip payload";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(put_store_->Put(cid, SpanOf(payload)));
    auto got = get_store_->Get(cid);
    ASSERT_TRUE(got) << got.error().message;
    EXPECT_EQ(got.value().bytes,
              std::vector<uint8_t>(payload.begin(), payload.end()));
}

/**
 * @given a stored block
 * @when the identical pair is Put again through a second store
 * @then alreadyPresent=true in the SUCCESS slot - the live 412
 *        absorption through the real mapper at a real server
 */
TEST_F(S3BlockStoreMinioTest, ConditionalRePutIdempotent)
{
    const std::string payload = "minio-tier idempotency payload";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(put_store_->Put(cid, SpanOf(payload)));
    auto re_put = get_store_->Put(cid, SpanOf(payload));
    ASSERT_TRUE(re_put) << re_put.error().message;
    EXPECT_TRUE(re_put.value().alreadyPresent);
}

/**
 * @given a stored block and the derived key recomputed in-test
 * @when a FRESH client Heads the derived key string directly
 * @then the object exists at exactly that key - the BACK-07 layout
 *        survives a real S3-compatible server (charset and case
 *        sensitivity observable at this tier, A1)
 */
TEST_F(S3BlockStoreMinioTest, KeyLayoutObservable)
{
    const std::string payload = "minio-tier key layout payload";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(put_store_->Put(cid, SpanOf(payload)));

    const std::string key = DerivedKey(cid);
    ASSERT_FALSE(key.empty());
    S3ClientConfig config;
    config.endpoint = "127.0.0.1";
    config.port = harness_.Port();
    config.region = "us-east-1";
    config.pathStyle = true;
    config.accessKey = harness_.AccessKey();
    config.secretKey = harness_.SecretKey();
    config.bucket = "store-bucket";
    auto client = MinioS3Client::Open(config);
    ASSERT_TRUE(client);
    auto head = client.value()->Head(key);
    ASSERT_TRUE(head) << head.error().message;
    EXPECT_TRUE(head.value().exists);
    EXPECT_EQ(head.value().size,
              static_cast<uint64_t>(payload.size()));
}

/**
 * @given a stored block and a never-put CID
 * @when Has/GetSize probe both
 * @then Has true + exact size for the stored CID; Has false for the
 *        never-put CID (authoritative absence at the real server)
 */
TEST_F(S3BlockStoreMinioTest, HasAndGetSize)
{
    const std::string payload = "minio-tier has/getsize payload";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(put_store_->Put(cid, SpanOf(payload)));

    auto has = get_store_->Has(cid);
    ASSERT_TRUE(has) << has.error().message;
    EXPECT_TRUE(has.value());

    auto size = get_store_->GetSize(cid);
    ASSERT_TRUE(size) << size.error().message;
    EXPECT_EQ(size.value(), static_cast<uint64_t>(payload.size()));

    const auto missing = MakeCid("minio-tier never-put cid");
    auto has_missing = get_store_->Has(missing);
    ASSERT_TRUE(has_missing) << has_missing.error().message;
    EXPECT_FALSE(has_missing.value());
}
