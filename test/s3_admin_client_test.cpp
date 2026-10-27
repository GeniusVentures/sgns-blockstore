/**
 * @file s3_admin_client_test.cpp
 * @brief S3AdminClient wire suite - the real cpp-httplib client against
 *        the raw-socket LoopbackHttpStub (DUR-01, Phase 10 plan 10-01
 *        Task 2; the minio_s3_client_test D-08 lane shape).
 *
 * READ BEFORE TOUCHING: the stub serves RAW wire bytes - a scripted
 * response is consumed by ONE request (FIFO), and a bare 200 OK is
 * served when the queue is empty. Every failure shape here must surface
 * TemporarilyUnavailable through the REAL client (the never-NotFound
 * taxonomy asserted per shape), and the Authorization header is asserted
 * from the CAPTURED REQUEST bytes - genuine wire evidence, not a
 * re-implementation of the encoder (":pw" -> base64 -> "OnB3").
 *
 * The bounded-read proof (Pitfall 4) uses a local black-hole listener
 * (accept, never respond): Flush must return within its per-call budget
 * mapped to TemporarilyUnavailable - s3d has no server-side deadline,
 * so the client budget is the only bound.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "LoopbackHttpStub.hpp"
#include <sgns-blockstore/S3AdminClient.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace
{
    using sgns::blockstore::BlockSource;
    using sgns::blockstore::S3AdminClient;
    using sgns::blockstore::S3AdminClientConfig;
    using sgns::blockstore::StatusClass;
    using sgns::blockstore::UploadStats;
    using sgns::blockstore::testsupport::LoopbackHttpStub;

    /// @brief A 200 response carrying a JSON body.
    std::string Ok200Json( const std::string &body )
    {
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: "
               + std::to_string( body.size() )
               + "\r\n"
                 "Connection: close\r\n"
                 "\r\n"
               + body;
    }

    std::string Bare500()
    {
        return "HTTP/1.1 500 Internal Server Error\r\n"
               "Content-Length: 0\r\n"
               "Connection: close\r\n"
               "\r\n";
    }

    /// The complete well-formed stats body (every consumed counter,
    /// bufferLimit deliberately 0 - the unbounded pass-through).
    const char *kFullStatsBody =
        R"({"pendingObjects":2,"pendingSize":76,"uploadedObjects":5,)"
        R"("uploadedSize":190,"unpinnedObjects":1,"failedUploads":3,)"
        R"("orphanedObjects":4,"multipartUploads":6,"bufferUsed":38,)"
        R"("bufferLimit":0,"ingressActive":7,"ingressBytes":1234,)"
        R"("ingressRate":56,"uploadBytes":789,"uploadRate":10})";

    S3AdminClientConfig MakeConfig( uint16_t port )
    {
        S3AdminClientConfig config;
        config.endpoint            = "http://127.0.0.1:" + std::to_string( port );
        config.adminPassword       = "pw";
        config.tlsVerify           = true; // default, passed through verbatim
        config.stats_read_timeout_ms = 1000;
        return config;
    }

    std::shared_ptr<S3AdminClient> MakeClient( uint16_t port )
    {
        auto opened = sgns::blockstore::OpenS3dAdminClient( MakeConfig( port ) );
        return std::shared_ptr<S3AdminClient>( std::move( opened.value() ) );
    }

    //
    // Black-hole listener for the bounded-read proof: accepts one
    // connection and never responds (WinSock, 127.0.0.1 only).
    //
    class BlackholeListener
    {
    public:
        ~BlackholeListener()
        {
            Stop();
        }

        bool Start()
        {
#ifdef _WIN32
            WSADATA wsa;
            if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 )
            {
                return false;
            }
            socket_ = ::socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
            if ( socket_ == INVALID_SOCKET )
            {
                return false;
            }
            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
            addr.sin_port        = 0;
            if ( bind( socket_, reinterpret_cast<sockaddr *>( &addr ), sizeof( addr ) ) != 0
                 || listen( socket_, 1 ) != 0 )
            {
                return false;
            }
            sockaddr_in bound{};
            int          bound_len = sizeof( bound );
            if ( getsockname( socket_, reinterpret_cast<sockaddr *>( &bound ), &bound_len ) != 0 )
            {
                return false;
            }
            port_   = ntohs( bound.sin_port );
            thread_ = std::thread( [this] { AcceptLoop(); } );
            return true;
#else
            return false;
#endif
        }

        uint16_t Port() const
        {
            return port_;
        }

        void Stop()
        {
#ifdef _WIN32
            if ( socket_ != INVALID_SOCKET )
            {
                closesocket( socket_ );
                socket_ = INVALID_SOCKET;
            }
            if ( thread_.joinable() )
            {
                thread_.join();
            }
#endif
        }

    private:
        void AcceptLoop()
        {
#ifdef _WIN32
            const SOCKET accepted = accept( socket_, nullptr, nullptr );
            if ( accepted != INVALID_SOCKET )
            {
                // Hold the connection open WITHOUT ever responding -
                // the client's read budget is the only escape.
                std::this_thread::sleep_for( std::chrono::seconds( 3 ) );
                closesocket( accepted );
            }
#endif
        }

#ifdef _WIN32
        SOCKET       socket_ = INVALID_SOCKET;
#endif
        uint16_t     port_ = 0;
        std::thread  thread_;
    };
} // namespace

/**
 * @given the real admin client against the loopback stub scripted with
 *        a 200 flush response
 * @when  Flush runs
 * @then  it succeeds, and the CAPTURED wire request carries the admin
 *        Basic auth with an EMPTY username byte before the colon
 *        (":pw" -> "Basic OnB3") and the process-global flush route.
 */
TEST( S3AdminClientWire, FlushCarriesEmptyUsernameBasicAuth )
{
    LoopbackHttpStub stub;
    ASSERT_TRUE( stub.Start() );
    stub.EnqueueOk200();

    auto client = MakeClient( stub.Port() );
    auto flushed = client->Flush( std::chrono::milliseconds( 1000 ) );
    ASSERT_TRUE( flushed.has_value() );

    ASSERT_GE( stub.RequestCount(), 1u );
    const auto captured = stub.CapturedRequest( 0 );
    EXPECT_NE( captured.find( "POST /objects/flush" ), std::string::npos );
    EXPECT_NE( captured.find( "Authorization: Basic OnB3" ), std::string::npos )
        << "wire request must carry base64(\":pw\") with the EMPTY username";
}

/**
 * @given a scripted bare 500 flush response
 * @when  Flush runs
 * @then  the failure is TemporarilyUnavailable - never NotFound (an
 *        admin failure is not an authoritative absence statement).
 */
TEST( S3AdminClientWire, Flush500MapsTemporarilyUnavailable )
{
    LoopbackHttpStub stub;
    ASSERT_TRUE( stub.Start() );
    stub.EnqueueResponse( Bare500() );

    auto client  = MakeClient( stub.Port() );
    auto flushed = client->Flush( std::chrono::milliseconds( 1000 ) );
    ASSERT_FALSE( flushed.has_value() );
    EXPECT_EQ( flushed.error().code, StatusClass::TemporarilyUnavailable );
    EXPECT_EQ( flushed.error().source, BlockSource::Remote );
}

/**
 * @given a complete, well-formed stats body (bufferLimit 0 = unbounded)
 * @when  GetUploadStats runs
 * @then  every counter parses into UploadStats verbatim.
 */
TEST( S3AdminClientWire, StatsParseEveryCounterIncludingZeroLimit )
{
    LoopbackHttpStub stub;
    ASSERT_TRUE( stub.Start() );
    stub.EnqueueResponse( Ok200Json( kFullStatsBody ) );

    auto client = MakeClient( stub.Port() );
    auto stats  = client->GetUploadStats();
    ASSERT_TRUE( stats.has_value() );
    EXPECT_EQ( stats.value().pendingObjects, 2u );
    EXPECT_EQ( stats.value().pendingSize, 76u );
    EXPECT_EQ( stats.value().uploadedObjects, 5u );
    EXPECT_EQ( stats.value().uploadedSize, 190u );
    EXPECT_EQ( stats.value().unpinnedObjects, 1u );
    EXPECT_EQ( stats.value().failedUploads, 3u );
    EXPECT_EQ( stats.value().orphanedObjects, 4u );
    EXPECT_EQ( stats.value().multipartUploads, 6u );
    EXPECT_EQ( stats.value().bufferUsed, 38u );
    EXPECT_EQ( stats.value().bufferLimit, 0u ); // pass-through, never defaulted
    EXPECT_EQ( stats.value().ingressActive, 7u );
    EXPECT_EQ( stats.value().ingressBytes, 1234u );
    EXPECT_EQ( stats.value().ingressRate, 56u );
    EXPECT_EQ( stats.value().uploadBytes, 789u );
    EXPECT_EQ( stats.value().uploadRate, 10u );

    ASSERT_GE( stub.RequestCount(), 1u );
    EXPECT_NE( stub.CapturedRequest( 0 ).find( "GET /stats/uploads" ), std::string::npos );
}

/**
 * @given a 200 stats response whose body is not JSON
 * @when  GetUploadStats runs
 * @then  the parse failure maps TemporarilyUnavailable (strict parse -
 *        never a partially-filled struct).
 */
TEST( S3AdminClientWire, StatsGarbageBodyMapsTemporarilyUnavailable )
{
    LoopbackHttpStub stub;
    ASSERT_TRUE( stub.Start() );
    stub.EnqueueResponse( Ok200Json( "this is { not json" ) );

    auto client = MakeClient( stub.Port() );
    auto stats  = client->GetUploadStats();
    ASSERT_FALSE( stats.has_value() );
    EXPECT_EQ( stats.error().code, StatusClass::TemporarilyUnavailable );
}

/**
 * @given a 200 stats response whose JSON omits pendingObjects
 * @when  GetUploadStats runs
 * @then  the missing REQUIRED evidence field fails the whole parse as
 *        TemporarilyUnavailable - the durability evidence can never be
 *        partially observed.
 */
TEST( S3AdminClientWire, StatsMissingPendingObjectsMapsTemporarilyUnavailable )
{
    LoopbackHttpStub stub;
    ASSERT_TRUE( stub.Start() );
    stub.EnqueueResponse( Ok200Json( R"({"uploadedObjects":5,"bufferLimit":10})" ) );

    auto client = MakeClient( stub.Port() );
    auto stats  = client->GetUploadStats();
    ASSERT_FALSE( stats.has_value() );
    EXPECT_EQ( stats.error().code, StatusClass::TemporarilyUnavailable );
}

/**
 * @given a listener that accepts the connection and never responds
 * @when  Flush runs with a 200 ms per-call read budget
 * @then  it returns TemporarilyUnavailable WITHIN the bound (Pitfall 4:
 *        s3d has no server-side deadline - the client budget is the
 *        only thing keeping the drainer bounded).
 */
TEST( S3AdminClientWire, FlushReadBudgetBoundsABlockedResponse )
{
    BlackholeListener blackhole;
    ASSERT_TRUE( blackhole.Start() );

    auto config   = MakeConfig( blackhole.Port() );
    auto opened   = sgns::blockstore::OpenS3dAdminClient( config );
    auto client   = std::shared_ptr<S3AdminClient>( std::move( opened.value() ) );
    const auto t0 = std::chrono::steady_clock::now();
    auto flushed  = client->Flush( std::chrono::milliseconds( 200 ) );
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0 );
    ASSERT_FALSE( flushed.has_value() );
    EXPECT_EQ( flushed.error().code, StatusClass::TemporarilyUnavailable );
    EXPECT_LT( elapsed.count(), 5000 ) << "Flush must be bounded by its read budget";
}
