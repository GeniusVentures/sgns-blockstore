/**
 * @file gc_minio_roundtrip_test.cpp
 * @brief TEST-03 (v1.1 Phase 11 plan 11-04): the MinIO-binary opt-in GC
 *        roundtrip - spill -> durable (HERMETIC admin semantics - A7:
 *        real MinIO has no s3d admin flush, so the flush-gated evidence
 *        rides the InMemoryS3AdminClient fake while the S3 tier is the
 *        REAL community MinIO binary) -> evict -> retrieve, CID-verified.
 *
 * Tier: TIER2 - dev-gated opt-in via SGNS_BLOCKSTORE_TEST_MINIO_BIN
 * (community build; NEVER the license-gated minio.exe), NEVER in CI.
 * Loud labelled skip when the binary is absent.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <sgns-blockstore/LocalBlockStore.hpp>
#include <sgns-blockstore/S3AdminClient.hpp>
#include <sgns-blockstore/S3BlockStore.hpp>
#include <sgns-blockstore/UploadDrainer.hpp>

#include "InMemoryS3AdminClient.hpp"
#include "MinioS3Client.hpp"
#include "MinioServerHarness.hpp"

#include <libp2p/crypto/sha/sha256.hpp>
#include <libp2p/multi/content_identifier_codec.hpp>

namespace
{
    using sgns::blockstore::Cid;
    using sgns::blockstore::CidOf;
    using sgns::blockstore::DrainPendingUploads;
    using sgns::blockstore::InMemoryS3AdminClient;
    using sgns::blockstore::LocalBlockStore;
    using sgns::blockstore::LocalBlockStoreOptions;
    using sgns::blockstore::MinioS3Client;
    using sgns::blockstore::S3BlockStore;
    using sgns::blockstore::S3ClientConfig;
    using sgns::blockstore::UploadState;
    using sgns::blockstore::testsupport::MinioServerHarness;

    std::string Hex( gsl::span<const uint8_t> bytes )
    {
        static const char *kHex = "0123456789abcdef";
        std::string out;
        out.reserve( bytes.size() * 2 );
        for ( const auto b : bytes )
        {
            out += kHex[b >> 4];
            out += kHex[b & 0x0F];
        }
        return out;
    }

    /// One live fixture over the REAL MinIO binary; the admin tier is
    /// the hermetic fake (A7 - real MinIO has no s3d admin listener).
    class GcMinioRoundtripTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            if ( !MinioServerHarness::Available() )
            {
                GTEST_SKIP() << "[SGNS_BLOCKSTORE][MINIO-GC] binary not provided (set "
                                "SGNS_BLOCKSTORE_TEST_MINIO_BIN; community build)";
            }
            std::string error;
            if ( !harness_.Start( /*tls=*/false, &error ) )
            {
                GTEST_SKIP() << "[SGNS_BLOCKSTORE][MINIO-GC] fixture unavailable: " << error;
            }
            ASSERT_TRUE( harness_.MakeBucket( "gc-bucket", &error ) ) << error;

            auto client = MinioS3Client::Open( MakeConfig() );
            ASSERT_TRUE( client );
            auto store = S3BlockStore::Open(
                std::shared_ptr<sgns::blockstore::S3Client>( std::move( client.value() ) ) );
            ASSERT_TRUE( store );
            remote_ = std::move( store.value() );
            fake_admin_ = std::make_shared<InMemoryS3AdminClient>();
        }

        void TearDown() override
        {
            harness_.Stop();
        }

        S3ClientConfig MakeConfig()
        {
            S3ClientConfig config;
            config.endpoint   = "127.0.0.1";
            config.port       = harness_.Port();
            config.region     = "us-east-1";
            config.pathStyle  = true;
            config.accessKey  = harness_.AccessKey();
            config.secretKey  = harness_.SecretKey();
            config.bucket     = "gc-bucket";
            return config;
        }

        MinioServerHarness                    harness_;
        std::unique_ptr<S3BlockStore>         remote_;
        std::shared_ptr<InMemoryS3AdminClient> fake_admin_;
    };
} // namespace

/**
 * @given a real MinIO-backed S3 tier and a fresh local store
 * @when a block is spilled, drained, flushed to Durable under the
 *        hermetic admin semantics (A7), backdated, evicted via
 *        EvictLocalAndMarkDisposed, and retrieved from the remote tier
 * @then the local copy is gone (bytes, journal; marker present) while
 *        the REAL MinIO tier serves the bytes CID-verified with the
 *        recorded digest - the GC roundtrip over a live S3 backend.
 */
TEST_F( GcMinioRoundtripTest, SpillDurableEvictRetrieveRoundtrip )
{
    const std::string dir = "test-data/gc-minio/case-"
                          + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );
    auto local_opened = LocalBlockStore::Open( LocalBlockStoreOptions{ dir } );
    ASSERT_TRUE( local_opened );
    auto local = std::move( local_opened.value() );

    const std::string payload = "gc minio roundtrip payload";
    const std::vector<uint8_t> bytes( payload.begin(), payload.end() );
    const auto cid = CidOf( bytes, libp2p::multi::MulticodecType::Code::RAW ).value();
    const auto recorded_digest = Hex( libp2p::crypto::sha256( bytes ).value() );

    // Spill + drain over the REAL MinIO tier.
    ASSERT_TRUE( local->Put( cid, bytes ) );
    auto drained = DrainPendingUploads( *local, *remote_ );
    ASSERT_TRUE( drained );

    // Hermetic flush evidence (A7), then the durable mark.
    auto flushed = fake_admin_->Flush( std::chrono::milliseconds( 1000 ) );
    ASSERT_TRUE( flushed );
    auto stats = fake_admin_->GetUploadStats();
    ASSERT_TRUE( stats );
    ASSERT_EQ( stats.value().pendingObjects, 0u );
    ASSERT_TRUE( local->MarkUploadedDurableMany( gsl::span<const Cid>( &cid, 1 ) ) );

    // Backdate + evict.
    const auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch() )
            .count() );
    ASSERT_TRUE( local->SetSpilledAt( cid, now_ms - 600000 ) );
    sgns::blockstore::DisposedRecord record;
    record.disposed_at_unix_ms = now_ms;
    record.reclaimed_bytes     = bytes.size();
    record.original_size       = bytes.size();
    ASSERT_TRUE( local->EvictLocalAndMarkDisposed( cid, record ) );

    // Local gone, marker present.
    ASSERT_FALSE( local->Get( cid ) );
    auto disposed = local->IsDisposed( cid );
    ASSERT_TRUE( disposed );
    EXPECT_TRUE( disposed.value() );

    // The REAL MinIO tier still serves it, CID-verified.
    auto got = remote_->Get( cid );
    ASSERT_TRUE( got ) << got.error().message;
    EXPECT_EQ( Hex( got.value().bytes ), recorded_digest );

    std::cout << "[SGNS_BLOCKSTORE][MINIO-GC] roundtrip proven: spill -> durable -> evict -> "
                 "retrieve (CID-verified)"
              << std::endl;
}
