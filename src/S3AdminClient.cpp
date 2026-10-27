/**
 * @file S3AdminClient.cpp
 * @brief cpp-httplib-backed implementation of the s3d admin facade
 *        (DUR-01, Phase 10 D-05/D-06).
 *
 * Pimpl (MinioS3Client precedent): ZERO httplib/nlohmann tokens in any
 * header - every cpp-httplib and nlohmann_json namespace token lives in
 * this TU, so no consumer can be broken by SDK churn and the installed
 * surface stays dependency-free by construction (the PRIVATE links in
 * src/CMakeLists.txt enforce it at link time).
 *
 * Failure taxonomy (10-RESEARCH 1.5): every failure string/class is
 * classified in exactly ONE function below (AdminUnavailable). The
 * default - and only - bucket is TemporarilyUnavailable with the context
 * preserved: an admin-API failure is never an authoritative statement of
 * object absence, so a conservative failure keeps journal records in
 * their previous state (fail-safe advancement).
 *
 * Basic auth shape (spike evidence, runbook README.md:75): the admin
 * listener authenticates with HTTP Basic where the USERNAME is ignored -
 * the wire form is base64(":" + adminPassword) with an EMPTY username
 * before the colon. The password is sent on the Authorization header
 * only; it never reaches an error string, a status payload, or any other
 * output channel (redaction by omission).
 */

#include <sgns-blockstore/S3AdminClient.hpp>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace sgns::blockstore
{
    namespace
    {
        /// @brief The ONE failure-classifier: every admin failure mints
        ///        its BlockStatus here. Conservative by construction -
        ///        outage/timeout/non-200/parse failures all read as
        ///        TemporarilyUnavailable so no advancement ever keys off
        ///        a missing observation.
        BlockStatus AdminUnavailable( std::string message )
        {
            return BlockStatus{ StatusClass::TemporarilyUnavailable,
                                BlockSource::Remote,
                                std::move( message ) };
        }

        /// @brief Minimal standard base64 encode (RFC 4648) for the
        ///        Basic auth header - no external dependency.
        std::string Base64Encode( const std::string &in )
        {
            static const char *kAlphabet =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve( ( in.size() + 2 ) / 3 * 4 );
            size_t i = 0;
            while ( i + 2 < in.size() )
            {
                const uint32_t triple = ( static_cast<uint8_t>( in[i] ) << 16 )
                                      | ( static_cast<uint8_t>( in[i + 1] ) << 8 )
                                      | static_cast<uint8_t>( in[i + 2] );
                out.push_back( kAlphabet[( triple >> 18 ) & 0x3F] );
                out.push_back( kAlphabet[( triple >> 12 ) & 0x3F] );
                out.push_back( kAlphabet[( triple >> 6 ) & 0x3F] );
                out.push_back( kAlphabet[triple & 0x3F] );
                i += 3;
            }
            const size_t rem = in.size() - i;
            if ( rem == 1 )
            {
                const uint32_t triple = static_cast<uint8_t>( in[i] ) << 16;
                out.push_back( kAlphabet[( triple >> 18 ) & 0x3F] );
                out.push_back( kAlphabet[( triple >> 12 ) & 0x3F] );
                out.push_back( '=' );
                out.push_back( '=' );
            }
            else if ( rem == 2 )
            {
                const uint32_t triple = ( static_cast<uint8_t>( in[i] ) << 16 )
                                      | ( static_cast<uint8_t>( in[i + 1] ) << 8 );
                out.push_back( kAlphabet[( triple >> 18 ) & 0x3F] );
                out.push_back( kAlphabet[( triple >> 12 ) & 0x3F] );
                out.push_back( kAlphabet[( triple >> 6 ) & 0x3F] );
                out.push_back( '=' );
            }
            return out;
        }

        /// @brief Strict unsigned-field extraction: the field MUST be
        ///        present and an unsigned integer. Unknown extra fields
        ///        are tolerated (forward compatibility); missing or
        ///        mistyped REQUIRED fields fail the whole parse.
        bool FieldU64( const nlohmann::json &j, const char *name, uint64_t *out )
        {
            const auto it = j.find( name );
            if ( it == j.end() || !it->is_number_unsigned() )
            {
                return false;
            }
            *out = it->get<uint64_t>();
            return true;
        }

        /// @brief Parse /stats/uploads into UploadStats with strict
        ///        presence + integer-type checks on every consumed field.
        bool ParseUploadStats( const std::string &body, UploadStats *out )
        {
            nlohmann::json parsed;
            try
            {
                parsed = nlohmann::json::parse( body );
            }
            catch ( const nlohmann::json::exception & )
            {
                return false;
            }
            if ( !parsed.is_object() )
            {
                return false;
            }
            UploadStats stats;
            if ( !FieldU64( parsed, "pendingObjects", &stats.pendingObjects )
                 || !FieldU64( parsed, "pendingSize", &stats.pendingSize )
                 || !FieldU64( parsed, "uploadedObjects", &stats.uploadedObjects )
                 || !FieldU64( parsed, "uploadedSize", &stats.uploadedSize )
                 || !FieldU64( parsed, "unpinnedObjects", &stats.unpinnedObjects )
                 || !FieldU64( parsed, "failedUploads", &stats.failedUploads )
                 || !FieldU64( parsed, "orphanedObjects", &stats.orphanedObjects )
                 || !FieldU64( parsed, "multipartUploads", &stats.multipartUploads )
                 || !FieldU64( parsed, "bufferUsed", &stats.bufferUsed )
                 || !FieldU64( parsed, "bufferLimit", &stats.bufferLimit )
                 || !FieldU64( parsed, "ingressActive", &stats.ingressActive )
                 || !FieldU64( parsed, "ingressBytes", &stats.ingressBytes )
                 || !FieldU64( parsed, "ingressRate", &stats.ingressRate )
                 || !FieldU64( parsed, "uploadBytes", &stats.uploadBytes )
                 || !FieldU64( parsed, "uploadRate", &stats.uploadRate ) )
            {
                return false;
            }
            *out = stats;
            return true;
        }
    } // namespace

    /// @brief Pimpl: the concrete httplib-backed admin client.
    class S3dAdminClientImpl final : public S3AdminClient
    {
    public:
        static result<std::unique_ptr<S3dAdminClientImpl>, BlockStatus> Open(
            const S3AdminClientConfig &config )
        {
            if ( config.endpoint.empty() )
            {
                throw std::invalid_argument( "OpenS3dAdminClient: endpoint is empty" );
            }
            auto impl = std::make_unique<S3dAdminClientImpl>();
            if ( !impl->Init( config ) )
            {
                return AdminUnavailable( "admin client: unsupported endpoint URL: " + config.endpoint );
            }
            return std::move( impl );
        }

        result<void, BlockStatus> Flush( std::chrono::milliseconds read_timeout_budget ) override
        {
            if ( read_timeout_budget <= std::chrono::milliseconds::zero() )
            {
                return AdminUnavailable( "admin flush: read budget must be positive" );
            }
            SetTimeouts( read_timeout_budget );
            const auto res = client_->Post( "/objects/flush" );
            if ( res == nullptr )
            {
                return AdminUnavailable( "admin flush: transport failure (connection refused/timeout)" );
            }
            if ( res->status != 200 )
            {
                return AdminUnavailable(
                    "admin flush: non-200 response (status=" + std::to_string( res->status ) + ")" );
            }
            return outcome::success();
        }

        result<UploadStats, BlockStatus> GetUploadStats() override
        {
            SetTimeouts( std::chrono::milliseconds( config_.stats_read_timeout_ms ) );
            const auto res = client_->Get( "/stats/uploads" );
            if ( res == nullptr )
            {
                return AdminUnavailable( "admin stats: transport failure (connection refused/timeout)" );
            }
            if ( res->status != 200 )
            {
                return AdminUnavailable(
                    "admin stats: non-200 response (status=" + std::to_string( res->status ) + ")" );
            }
            UploadStats stats;
            if ( !ParseUploadStats( res->body, &stats ) )
            {
                return AdminUnavailable(
                    "admin stats: body is not valid JSON or a required counter field is "
                    "missing/mistyped" );
            }
            return stats;
        }

    private:
        /// @brief Parse scheme://host[:port] and construct the matching
        ///        httplib client. Returns false on an unparseable URL.
        bool Init( const S3AdminClientConfig &config )
        {
            config_ = config;
            std::string url = config.endpoint;
            bool        tls = false;
            if ( url.rfind( "https://", 0 ) == 0 )
            {
                tls = true;
                url.erase( 0, 8 );
            }
            else if ( url.rfind( "http://", 0 ) == 0 )
            {
                url.erase( 0, 7 );
            }
            else
            {
                return false;
            }
            // Drop any trailing path component - the admin routes are
            // fixed ("/objects/flush", "/stats/uploads").
            const auto slash = url.find( '/' );
            if ( slash != std::string::npos )
            {
                url.erase( slash );
            }
            const auto colon = url.rfind( ':' );
            std::string host = url;
            int          port;
            if ( colon != std::string::npos )
            {
                host                 = url.substr( 0, colon );
                const auto port_text = url.substr( colon + 1 );
                // Manual digits-only range-checked parse (review MA-01):
                // std::stoi throws out_of_range on huge ports (escaping
                // the outcome contract through the embedder) and silently
                // accepts trailing junk / negatives - none of that here.
                uint32_t parsed = 0;
                bool     ok     = !port_text.empty();
                for ( const char c : port_text )
                {
                    if ( c < '0' || c > '9' )
                    {
                        ok = false;
                        break;
                    }
                    parsed = parsed * 10 + static_cast<uint32_t>( c - '0' );
                    if ( parsed > 65535 )
                    {
                        ok = false;
                        break;
                    }
                }
                if ( !ok )
                {
                    return false;
                }
                port = static_cast<int>( parsed );
            }
            else
            {
                port = tls ? 443 : 80;
            }
            if ( host.empty() )
            {
                return false;
            }

            // cpp-httplib 0.14 Client is a pimpl wrapper: the
            // scheme_host_port ctor selects the plain/SSL impl (an
            // SSLClient is NOT-a-Client here - it derives ClientImpl),
            // and the wrapper forwards the verification control.
            const std::string base_url = ( tls ? "https://" : "http://" ) + host + ":" + std::to_string( port );
            client_ = std::make_unique<httplib::Client>( base_url );
            // tlsVerify NEVER defaults off (T-3-02 posture) - only an
            // explicit opt-out disables server certificate
            // verification (loopback test lanes).
            client_->enable_server_certificate_verification( config.tlsVerify );
            // Empty username, password only (spike evidence: the admin
            // listener ignores the username field of the Basic pair).
            const std::string basic = ":" + config.adminPassword;
            auth_header_            = "Basic " + Base64Encode( basic );
            client_->set_default_headers( { { "Authorization", auth_header_ } } );
            return true;
        }

        /// @brief Bound both the connect and read phases by the caller's
        ///        budget - s3d has no server-side deadline, so this is
        ///        the only bound on any call (Pitfall 4).
        void SetTimeouts( std::chrono::milliseconds budget )
        {
            client_->set_connection_timeout( budget );
            client_->set_read_timeout( budget );
        }

        S3AdminClientConfig         config_;
        std::unique_ptr<httplib::Client> client_;
        std::string                 auth_header_;
    };

    result<std::shared_ptr<S3AdminClient>, BlockStatus> OpenS3dAdminClient(
        const S3AdminClientConfig &config )
    {
        auto opened = S3dAdminClientImpl::Open( config );
        if ( !opened.has_value() )
        {
            return opened.error();
        }
        std::unique_ptr<S3dAdminClientImpl> impl = std::move( opened.value() );
        return std::shared_ptr<S3AdminClient>( std::move( impl ) );
    }
} // namespace sgns::blockstore
