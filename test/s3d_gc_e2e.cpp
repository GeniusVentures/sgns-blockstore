/**
 * @file s3d_gc_e2e.cpp
 * @brief TEST-03 (v1.1 Phase 11 plan 11-04): the live s3d GC vertical as
 *        a TWO-RUN fresh-process proof - run 1 (S3dGcE2E.SpillRun) drives
 *        LocalBlockStore.Put -> DrainPendingUploads -> s3d -> admin flush
 *        + pendingObjects==0 -> MarkUploadedDurableMany; run 2
 *        (S3dGcE2E.EvictRun) reopens the SAME store in a fresh process,
 *        backdates one Durable record's spill timestamp (the age policy
 *        itself is hermetically proven - the live leg proves the
 *        destructive + retrieval mechanics against the real backend),
 *        evicts it via EvictLocalAndMarkDisposed, and asserts the local
 *        copy is GONE while the S3 tier still serves the bytes
 *        CID-verified against run 1's recorded digest.
 *
 * TWO INVOCATIONS ARE THE PROOF:
 *   s3d_gc_e2e.exe --gtest_filter=S3dGcE2E.SpillRun
 *   s3d_gc_e2e.exe --gtest_filter=S3dGcE2E.EvictRun
 *
 * The run-pair handoff rides a plain-text run-state file whose path
 * arrives via SGNS_BLOCKSTORE_TEST_S3D_RUN_STATE (cid list, digests,
 * sizes, the local store dir).
 *
 * Tier: TIER3 - dev-gated live tier, NEVER in CI: the binary ONLY
 * CONNECTS to the env-provided endpoint (never spawns or manages s3d -
 * the Sia account is dev-owned state). No fixed sleeps - connect or
 * fail fast. With any env var unset both cases loud-skip
 * [SGNS_BLOCKSTORE][s3d-gc-e2e] naming EVERY missing var.
 */

#include <gtest/gtest.h>

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
#include <sgns-blockstore/S3AdminClient.hpp>
#include <sgns-blockstore/S3BlockStore.hpp>
#include <sgns-blockstore/UploadDrainer.hpp>

#include "MinioS3Client.hpp"

#include <libp2p/crypto/sha/sha256.hpp>
#include <libp2p/multi/content_identifier_codec.hpp>

namespace
{
    using sgns::blockstore::Cid;
    using sgns::blockstore::CidOf;
    using sgns::blockstore::DrainPendingUploads;
    using sgns::blockstore::LocalBlockStore;
    using sgns::blockstore::LocalBlockStoreOptions;
    using sgns::blockstore::MinioS3Client;
    using sgns::blockstore::S3AdminClientConfig;
    using sgns::blockstore::S3BlockStore;
    using sgns::blockstore::S3ClientConfig;
    using sgns::blockstore::UploadState;
    using libp2p::multi::MulticodecType;

    /// @brief The env gate for BOTH runs: the S3D quartet + the admin
    ///        password + the run-state handoff path. Names every
    ///        missing var in the loud skip line.
    struct S3dGcE2EEnv
    {
        std::string endpoint;
        uint16_t port = 0;
        std::string access_key;
        std::string secret_key;
        std::string bucket;
        std::string admin_password;
        std::string admin_endpoint; // optional - documented s3d default
        std::string run_state_path;
        /// Checkpoint-ruled mode (documented ceiling): skip waiting for
        /// flush evidence and drive the durable marks directly - s3d's
        /// /objects/flush BLOCKS until Sia slot progression completes
        /// (Phase-10's accepted-is-max-provable finding).
        bool skip_flush_evidence = false;
        bool ok = false;
    };

    S3dGcE2EEnv ReadEnv()
    {
        S3dGcE2EEnv env;
        const char *endpoint   = std::getenv( "SGNS_BLOCKSTORE_TEST_S3D_ENDPOINT" );
        const char *access     = std::getenv( "SGNS_BLOCKSTORE_TEST_S3D_ACCESS_KEY" );
        const char *secret     = std::getenv( "SGNS_BLOCKSTORE_TEST_S3D_SECRET_KEY" );
        const char *bucket     = std::getenv( "SGNS_BLOCKSTORE_TEST_S3D_BUCKET" );
        const char *admin_pw   = std::getenv( "SGNS_BLOCKSTORE_TEST_S3D_ADMIN_PASSWORD" );
        const char *admin_ep   = std::getenv( "SGNS_BLOCKSTORE_TEST_S3D_ADMIN_ENDPOINT" );
        const char *run_state  = std::getenv( "SGNS_BLOCKSTORE_TEST_S3D_RUN_STATE" );
        const char *skip_flush = std::getenv( "SGNS_BLOCKSTORE_TEST_S3D_SKIP_FLUSH_EVIDENCE" );

        std::string missing;
        if ( endpoint == nullptr || *endpoint == '\0' )
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_ENDPOINT";
        }
        if ( access == nullptr || *access == '\0' )
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_ACCESS_KEY";
        }
        if ( secret == nullptr || *secret == '\0' )
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_SECRET_KEY";
        }
        if ( bucket == nullptr || *bucket == '\0' )
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_BUCKET";
        }
        if ( admin_pw == nullptr || *admin_pw == '\0' )
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_ADMIN_PASSWORD";
        }
        if ( run_state == nullptr || *run_state == '\0' )
        {
            missing += " SGNS_BLOCKSTORE_TEST_S3D_RUN_STATE";
        }
        if ( !missing.empty() )
        {
            std::cout << "[SGNS_BLOCKSTORE][s3d-gc-e2e] dev-gated live GC E2E skipped - unset:" << missing
                      << std::endl;
            return env;
        }
        const std::string raw( endpoint );
        const auto colon = raw.rfind( ':' );
        if ( colon != std::string::npos && colon > 0 )
        {
            env.endpoint = raw.substr( 0, colon );
            env.port     = static_cast<uint16_t>( std::stoul( raw.substr( colon + 1 ) ) );
        }
        else
        {
            env.endpoint = raw;
        }
        env.access_key      = access;
        env.secret_key      = secret;
        env.bucket          = bucket;
        env.admin_password  = admin_pw;
        env.admin_endpoint  = ( admin_ep != nullptr && *admin_ep != '\0' ) ? admin_ep : "http://127.0.0.1:8001";
        env.run_state_path  = run_state;
        env.skip_flush_evidence = ( skip_flush != nullptr && std::string( skip_flush ) == "1" );
        env.ok              = true;
        return env;
    }

    S3ClientConfig MakeConfig( const S3dGcE2EEnv &env )
    {
        S3ClientConfig config;
        config.endpoint   = env.endpoint;
        config.port       = env.port;
        config.region     = "us-east-1";
        config.pathStyle  = true;
        config.accessKey  = env.access_key;
        config.secretKey  = env.secret_key;
        config.bucket     = env.bucket;
        return config;
    }

    std::unique_ptr<S3BlockStore> OpenRemoteStore( const S3dGcE2EEnv &env )
    {
        auto client = MinioS3Client::Open( MakeConfig( env ) );
        if ( !client )
        {
            ADD_FAILURE() << "MinioS3Client::Open failed";
            return nullptr;
        }
        auto store = S3BlockStore::Open(
            std::shared_ptr<sgns::blockstore::S3Client>( std::move( client.value() ) ) );
        if ( !store )
        {
            ADD_FAILURE() << "S3BlockStore::Open failed";
            return nullptr;
        }
        return std::move( store.value() );
    }

    std::string IsoTimestamp()
    {
        const auto now = std::chrono::system_clock::to_time_t( std::chrono::system_clock::now() );
        std::tm tm_buffer{};
#ifdef _WIN32
        localtime_s( &tm_buffer, &now );
#else
        localtime_r( &now, &tm_buffer );
#endif
        std::ostringstream stream;
        stream << std::put_time( &tm_buffer, "%Y-%m-%dT%H:%M:%S" );
        return stream.str();
    }

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
} // namespace

/**
 * @given a fresh local store (temp dir), N=3 unique-per-run blocks, and
 *        the live s3d endpoint + admin listener
 * @when the blocks are Put locally (journals born pending), drained
 *        into s3d, and the flush-gated advancement runs (admin Flush +
 *        GetUploadStats until pendingObjects == 0, then the batched
 *        durable mark)
 * @then every journal record reads UploadedDurable, and the run-state
 *        file (cids, digests, sizes, local dir) is written for run 2
 */
TEST( S3dGcE2E, SpillRun )
{
    const auto env = ReadEnv();
    if ( !env.ok )
    {
        GTEST_SKIP() << "[SGNS_BLOCKSTORE][s3d-gc-e2e] env gate unset";
    }

    const std::string dir = "test-data/s3d-gc-e2e/spill-"
                          + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );
    auto local_opened = LocalBlockStore::Open( LocalBlockStoreOptions{ dir } );
    ASSERT_TRUE( local_opened ) << local_opened.error().message;
    auto local = std::move( local_opened.value() );

    std::random_device random;
    constexpr size_t kBlocks = 3;
    std::vector<Cid> cids;
    std::vector<std::string> digests;
    std::vector<uint64_t> sizes;
    for ( size_t i = 0; i < kBlocks; ++i )
    {
        std::ostringstream stream;
        stream << "sgns-s3d-gc-e2e " << IsoTimestamp() << " idx=" << i << " nonce=" << ( random() ^ random() );
        const std::string payload = stream.str();
        const std::vector<uint8_t> bytes( payload.begin(), payload.end() );
        const auto cid = CidOf( bytes, MulticodecType::Code::RAW ).value();
        ASSERT_TRUE( local->Put( cid, bytes ) );
        cids.push_back( cid );
        digests.push_back( Hex( libp2p::crypto::sha256( bytes ).value() ) );
        sizes.push_back( bytes.size() );
    }

    auto remote = OpenRemoteStore( env );
    ASSERT_NE( remote, nullptr );
    auto drained = DrainPendingUploads( *local, *remote );
    ASSERT_TRUE( drained ) << drained.error().message;
    ASSERT_EQ( drained.value().size(), kBlocks );

    // Flush-gated advancement (the Phase-10 evidence chain, live): flush
    // + stats until pendingObjects == 0, then the batched durable mark.
    // CHECKPOINT-RULED MODE (SGNS_BLOCKSTORE_TEST_S3D_SKIP_FLUSH_EVIDENCE
    // =1): s3d's /objects/flush BLOCKS until Sia slot progression
    // completes - when the session cannot observe the evidence (Phase-10's
    // accepted-is-max-provable ceiling), the durable marks are driven
    // directly and the capture records the mode. The E2E's claim (evict
    // + CID-verified retrieval against the real backend) is unaffected
    // by how Durable was reached.
    const int  kFlushAttempts = env.skip_flush_evidence ? 2 : 30;
    const auto kFlushBudget   = std::chrono::milliseconds( env.skip_flush_evidence ? 5000 : 30000 );
    bool       evidence       = false;
    {
        sgns::blockstore::S3AdminClientConfig admin_config;
        admin_config.endpoint              = env.admin_endpoint;
        admin_config.adminPassword         = env.admin_password;
        admin_config.tlsVerify             = false; // dev loopback listener, plain HTTP
        admin_config.stats_read_timeout_ms = 10000;
        auto admin = sgns::blockstore::OpenS3dAdminClient( admin_config );
        ASSERT_TRUE( admin ) << "OpenS3dAdminClient failed";
        for ( int attempt = 0; attempt < kFlushAttempts && !evidence; ++attempt )
        {
            auto flushed = admin.value()->Flush( kFlushBudget );
            if ( !flushed )
            {
                continue;
            }
            auto stats = admin.value()->GetUploadStats();
            if ( stats && stats.value().pendingObjects == 0 )
            {
                evidence = true;
            }
        }
    }
    if ( !evidence && !env.skip_flush_evidence )
    {
        FAIL() << "flush evidence (pendingObjects == 0) not observed - set "
                  "SGNS_BLOCKSTORE_TEST_S3D_SKIP_FLUSH_EVIDENCE=1 for the documented "
                  "accepted-is-max-provable ceiling mode (checkpoint-ruled)";
    }
    ASSERT_TRUE( local->MarkUploadedDurableMany( gsl::span<const Cid>( cids.data(), cids.size() ) ) );
    for ( const auto &cid : cids )
    {
        auto state = local->GetUploadState( cid );
        ASSERT_TRUE( state );
        EXPECT_EQ( state.value(), UploadState::UploadedDurable );
    }

    std::ofstream state_file( env.run_state_path, std::ios::trunc );
    ASSERT_TRUE( state_file.is_open() ) << "cannot write run-state: " << env.run_state_path;
    state_file << "local_dir=" << dir << "\n";
    state_file << "count=" << kBlocks << "\n";
    for ( size_t i = 0; i < kBlocks; ++i )
    {
        state_file << "cid" << i << "=" << libp2p::multi::ContentIdentifierCodec::toString( cids[i] ).value()
                   << "\n";
        state_file << "digest" << i << "=" << digests[i] << "\n";
        state_file << "size" << i << "=" << sizes[i] << "\n";
    }
    state_file.close();

    std::cout << "[SGNS_BLOCKSTORE][s3d-gc-e2e][RUN-1] spill complete: blocks=" << kBlocks
              << " journal=UploadedDurable flush_evidence="
              << ( evidence ? "pendingObjects0" : "SKIP_MODE_CEILING_DOCUMENTED" ) << std::endl;
}

/**
 * @given run 1's recorded state (cids/digests/local dir) and the live
 *        endpoint - a FRESH process over the SAME store directory
 * @when one Durable record's spill timestamp is backdated (the age
 *        policy is hermetically proven; the live leg proves the
 *        mechanics) and EvictLocalAndMarkDisposed runs
 * @then the local copy is GONE (local Get NotFound, journal gone,
 * IsDisposed true) while the S3 tier STILL serves the bytes CID-verified
 * with run 1's recorded digest - the spill -> GC -> retrieve vertical
 * against the real backend.
 */
TEST( S3dGcE2E, EvictRun )
{
    const auto env = ReadEnv();
    if ( !env.ok )
    {
        GTEST_SKIP() << "[SGNS_BLOCKSTORE][s3d-gc-e2e] env gate unset";
    }

    std::ifstream state_file( env.run_state_path );
    ASSERT_TRUE( state_file.is_open() ) << "run-state missing (run 1 has not run): " << env.run_state_path;
    std::string local_dir;
    size_t count = 0;
    std::vector<std::string> cid_strings;
    std::vector<std::string> digests;
    std::vector<uint64_t> sizes;
    std::string line;
    while ( std::getline( state_file, line ) )
    {
        if ( line.rfind( "local_dir=", 0 ) == 0 )
        {
            local_dir = line.substr( 10 );
        }
        else if ( line.rfind( "count=", 0 ) == 0 )
        {
            count = std::stoull( line.substr( 6 ) );
        }
        else if ( line.rfind( "cid", 0 ) == 0 && line.find( '=', 3 ) != std::string::npos )
        {
            cid_strings.push_back( line.substr( line.find( '=', 3 ) + 1 ) );
        }
        else if ( line.rfind( "digest", 0 ) == 0 && line.find( '=', 6 ) != std::string::npos )
        {
            digests.push_back( line.substr( line.find( '=', 6 ) + 1 ) );
        }
        else if ( line.rfind( "size", 0 ) == 0 && line.find( '=', 4 ) != std::string::npos )
        {
            sizes.push_back( std::stoull( line.substr( line.find( '=', 4 ) + 1 ) ) );
        }
    }
    ASSERT_FALSE( local_dir.empty() );
    ASSERT_EQ( count, cid_strings.size() );
    ASSERT_EQ( count, digests.size() );

    // Fresh process, SAME store directory (the reopen proof).
    auto local_opened = LocalBlockStore::Open( LocalBlockStoreOptions{ local_dir } );
    ASSERT_TRUE( local_opened ) << local_opened.error().message;
    auto local = std::move( local_opened.value() );

    const auto victim_parsed = libp2p::multi::ContentIdentifierCodec::fromString( cid_strings[0] );
    ASSERT_TRUE( victim_parsed );
    const Cid victim( victim_parsed.value() );

    // Backdate the age evidence (the retention threshold itself is
    // hermetically proven; here it is simulated to trigger the primitive).
    const auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch() )
            .count() );
    ASSERT_TRUE( local->SetSpilledAt( victim, now_ms - 600000 ) );

    // The destructive primitive, live.
    sgns::blockstore::DisposedRecord record;
    record.disposed_at_unix_ms = now_ms;
    record.reclaimed_bytes     = sizes[0];
    record.original_size       = sizes[0];
    record.reason              = 1;
    ASSERT_TRUE( local->EvictLocalAndMarkDisposed( victim, record ) );

    // The local copy is GONE.
    auto local_get = local->Get( victim );
    ASSERT_FALSE( local_get );
    auto state = local->GetUploadState( victim );
    ASSERT_FALSE( state );
    auto disposed = local->IsDisposed( victim );
    ASSERT_TRUE( disposed );
    EXPECT_TRUE( disposed.value() );

    // The S3 tier still serves it, CID-verified with run 1's digest.
    auto remote = OpenRemoteStore( env );
    ASSERT_NE( remote, nullptr );
    auto got = remote->Get( victim );
    ASSERT_TRUE( got ) << got.error().message;
    EXPECT_EQ( got.value().bytes.size(), sizes[0] );
    EXPECT_EQ( Hex( libp2p::crypto::sha256( got.value().bytes ).value() ), digests[0] );

    std::cout << "[SGNS_BLOCKSTORE][s3d-gc-e2e][RUN-2] eviction complete: local=absent marker=present"
              << std::endl;
    std::cout << "[SGNS_BLOCKSTORE][s3d-gc-e2e][RUN-2] blocks=" << count << " reclaimed_count=1"
              << " reclaimed_bytes=" << sizes[0] << std::endl;
    std::cout << "[SGNS_BLOCKSTORE][s3d-gc-e2e][RUN-2] retrieval_digest=" << digests[0] << std::endl;
    std::cout << "[SGNS_BLOCKSTORE][s3d-gc-e2e][RUN-2] verdict=SPILL_GC_RETRIEVE_PROVEN_LIVE" << std::endl;
}
