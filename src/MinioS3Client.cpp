/**
 * @file MinioS3Client.cpp
 * @brief Implementation of the minio-cpp-backed S3Client adapter
 *        (BACK-06/SC2/SC4) - ALL minio::/miniocpp:: usage lives in this
 *        TU (the LocalBlockStore/rocksdb discipline).
 */

#include "MinioS3Client.hpp"

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <miniocpp/client.h>
#include <miniocpp/providers.h>

namespace sgns::blockstore
{
    namespace
    {
        /**
         * @brief Precondition-evidence classification for BOTH (all
         *        three) 412 shapes observed at minio-cpp v1.0.0.
         *
         * Shapes (03-01 spike findings):
         *  - XML body with Content-Type: application/xml parses to the
         *    "PreconditionFailed: ..." string prefix;
         *  - an empty/non-XML body yields the SDK status-default
         *    "server failed with HTTP status code 412" text;
         *  - an unparseable body yields
         *    "invalid response received; status code: 412; ...".
         *
         * Callers convert this to the D-02 SUCCESS slot
         * (alreadyPresent=true) BEFORE MapMinioFailure is consulted.
         */
        bool IsPreconditionFailure(const std::string& error_string)
        {
            return error_string.find("PreconditionFailed") != std::string::npos
                || error_string.find("HTTP status code 412")
                    != std::string::npos
                || error_string.find("status code: 412") != std::string::npos;
        }

        /// @brief Authoritative-absence evidence in an SDK error string.
        bool IsAuthoritativeAbsence(const std::string& error_string)
        {
            return error_string.rfind("NoSuchKey:", 0) == 0
                || error_string.rfind("NoSuchBucket:", 0) == 0;
        }

        /**
         * @brief The ONE-place SDK-error-string -> BlockStatus mapper
         *        (SC4; the LocalBlockStore MapStatus discipline).
         *
         * Classification order:
         *  - NoSuchKey:/NoSuchBucket: prefixes -> NotFound (the ONLY
         *    authoritative-absence strings);
         *  - AccessDenied: prefix -> Unauthorized;
         *  - everything else (5xx server-failed text, transport text with
         *    no code prefix, ANY unrecognized string) ->
         *    TemporarilyUnavailable with the original message preserved.
         *
         * The default bucket is TemporarilyUnavailable and NEVER NotFound
         * - an unrecognized failure reading as authoritative absence is
         * the CRDT-corruption trap this taxonomy exists to prevent.
         *
         * @note Message composition copies SDK response text only - no
         *       S3ClientConfig field (credentials!) is ever concatenated
         *       into a message (threat T-3-03).
         */
        BlockStatus MapMinioFailure(const std::string& error_string,
                                    const char* op)
        {
            if (error_string.rfind("NoSuchKey:", 0) == 0
                || error_string.rfind("NoSuchBucket:", 0) == 0)
            {
                return BlockStatus{StatusClass::NotFound, BlockSource::Remote,
                                   std::string(op) + ": " + error_string};
            }
            if (error_string.rfind("AccessDenied:", 0) == 0)
            {
                return BlockStatus{StatusClass::Unauthorized,
                                   BlockSource::Remote,
                                   std::string(op) + ": " + error_string};
            }
            return BlockStatus{StatusClass::TemporarilyUnavailable,
                               BlockSource::Remote,
                               std::string(op) + ": " + error_string};
        }
    } // namespace

    /**
     * @brief Holds every SDK member - the pimpl core (SC2).
     * @note Member order is load-bearing: base_url_ and provider_ must
     *       outlive client_ (the SDK stores the references).
     */
    class MinioS3Client::Impl
    {
    public:
        Impl(const S3ClientConfig& config)
            : base_url_(config.endpoint,
                        config.useTls,
                        config.region.empty() ? std::string("us-east-1")
                                              : config.region),
              provider_(config.accessKey, config.secretKey),
              client_(base_url_, &provider_),
              config_(config)
        {
            base_url_.port = config.port;
            // pathStyle true (the default config) == virtual_style false.
            base_url_.virtual_style = !config.pathStyle;
            // The ONLY TLS-verification control path (T-3-02): default
            // tlsVerify=true means certificate checking stays ON.
            client_.IgnoreCertCheck(!config.tlsVerify);
        }

        minio::s3::BaseUrl base_url_;
        minio::creds::StaticProvider provider_;
        minio::s3::Client client_;
        S3ClientConfig config_;
    };

    result<std::unique_ptr<MinioS3Client>, BlockStatus> MinioS3Client::Open(
        const S3ClientConfig& config)
    {
        if (config.endpoint.empty() || config.bucket.empty())
        {
            throw std::invalid_argument(
                "sgns::blockstore: S3ClientConfig requires a non-empty "
                "endpoint and bucket");
        }
        auto client = std::unique_ptr<MinioS3Client>(new MinioS3Client());
        client->impl_ = std::make_unique<Impl>(config);
        return client;
    }

    MinioS3Client::MinioS3Client() = default;

    MinioS3Client::~MinioS3Client() = default;

    result<S3PutResult, BlockStatus> MinioS3Client::Put(
        const std::string& key, const std::vector<uint8_t>& bytes,
        const PutObjectOptions& options)
    {
        // Read-before-write for the plain path's alreadyPresent report
        // (the createOnly path gets it for free via the 412). A failed
        // pre-check never blocks the Put - the Put itself reports the
        // real failure if the backend is broken.
        bool already_present = false;
        if (!options.createOnly)
        {
            auto head = Head(key);
            already_present = head.has_value() && head.value().exists;
        }

        // Binary-safe istream over the bytes (std::string carries binary).
        const std::string data(bytes.begin(), bytes.end());
        std::istringstream stream(data);
        minio::s3::PutObjectArgs args(
            stream, std::optional<uint64_t>(bytes.size()), /*part_size=*/0);
        args.bucket = impl_->config_.bucket;
        args.region = impl_->config_.region.empty()
            ? std::string("us-east-1")
            : impl_->config_.region;
        args.object = key;
        args.content_type = "application/octet-stream";
        if (options.createOnly)
        {
            // THE conditional field: the ObjectWriteArgs headers Multimap
            // - verified on the wire in the 03-01 spike. NEVER
            // extra_headers: the pinned tag's single-part PUT path
            // silently drops that member (03-RESEARCH pitfall 2).
            args.headers.Add("If-None-Match", "*");
        }

        auto result = impl_->client_.PutObject(args);
        if (!result)
        {
            // D-02: both precondition-evidence shapes are NOT failures -
            // the success slot reports the duplicate before the mapper
            // is ever consulted.
            const std::string error = result.error().String();
            if (IsPreconditionFailure(error))
            {
                return S3PutResult{ /*achieved=*/true,
                                    /*alreadyPresent=*/true };
            }
            return MapMinioFailure(error, "PutObject");
        }
        return S3PutResult{ /*achieved=*/true, already_present };
    }

    result<S3Object, BlockStatus> MinioS3Client::Get(const std::string& key)
    {
        std::string buffer;
        minio::s3::GetObjectArgs args;
        args.bucket = impl_->config_.bucket;
        args.region = impl_->config_.region.empty()
            ? std::string("us-east-1")
            : impl_->config_.region;
        args.object = key;
        args.datafunc = [&buffer](minio::http::DataFunctionArgs chunk) {
            buffer.append(chunk.datachunk);
            return true;
        };

        auto result = impl_->client_.GetObject(args);
        if (!result)
        {
            return MapMinioFailure(result.error().String(), "GetObject");
        }
        return S3Object{ key,
                         std::vector<uint8_t>(buffer.begin(), buffer.end()) };
    }

    result<S3HeadResult, BlockStatus> MinioS3Client::Head(
        const std::string& key)
    {
        minio::s3::StatObjectArgs args;
        args.bucket = impl_->config_.bucket;
        args.region = impl_->config_.region.empty()
            ? std::string("us-east-1")
            : impl_->config_.region;
        args.object = key;

        auto result = impl_->client_.StatObject(args);
        if (!result)
        {
            const std::string error = result.error().String();
            // Authoritative absence: the SUCCESS slot exists=false (the
            // 03-02 interface contract) - never an error there.
            if (IsAuthoritativeAbsence(error))
            {
                return S3HeadResult{ /*exists=*/false, /*size=*/0 };
            }
            return MapMinioFailure(error, "StatObject");
        }
        return S3HeadResult{ /*exists=*/true,
                             static_cast<uint64_t>(result.value().size) };
    }

    result<void, BlockStatus> MinioS3Client::Delete(const std::string& key)
    {
        minio::s3::RemoveObjectArgs args;
        args.bucket = impl_->config_.bucket;
        args.region = impl_->config_.region.empty()
            ? std::string("us-east-1")
            : impl_->config_.region;
        args.object = key;

        auto result = impl_->client_.RemoveObject(args);
        if (!result)
        {
            return MapMinioFailure(result.error().String(), "RemoveObject");
        }
        return outcome::success(); // idempotent (S3 RemoveObject semantics)
    }
} // namespace sgns::blockstore
