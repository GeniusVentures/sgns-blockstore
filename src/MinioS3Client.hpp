/**
 * @file MinioS3Client.hpp
 * @brief minio-cpp-backed implementation of the internal S3Client seam
 *        (BACK-06, decisions D-01/D-02; SC2/SC4).
 *
 * Pimpl (LocalBlockStore precedent): ZERO SDK types in this header -
 * every minio-cpp namespace token lives in MinioS3Client.cpp, so no
 * consumer of this internal header can ever be broken by SDK churn, and
 * the installed-header surface stays SDK-free by construction (SC2).
 *
 * Failure taxonomy (SC4): every SDK failure string is classified in
 * exactly ONE function inside the .cpp (MapMinioFailure). The default
 * bucket is TemporarilyUnavailable with the message preserved - an
 * unrecognized failure NEVER reads as NotFound (the CRDT-corruption
 * trap). Both precondition-evidence shapes report through the SUCCESS
 * slot with alreadyPresent=true BEFORE the mapper is consulted (D-02).
 */

#ifndef SGNS_BLOCKSTORE_MINIOS3CLIENT_HPP
#define SGNS_BLOCKSTORE_MINIOS3CLIENT_HPP

#include <memory>

#include <sgns-blockstore/S3Client.hpp>

namespace sgns::blockstore
{
    /**
     * @brief The minio-cpp-backed S3Client (impl header stays internal;
     *     the S3Client interface it implements is installed since Phase 4).
     *
     * Constructed via Open(). Synchronous and blocking like the whole
     * Phase-3 seam - no background threads; concurrent-call guarantees
     * are out of the Phase-3 contract.
     */
    class MinioS3Client final : public S3Client
    {
    public:
        /**
         * @brief Factory mirroring LocalBlockStore::Open.
         * @param config endpoint/credentials/bucket; tlsVerify is the ONLY
         *        TLS-verification control (default true, T-3-02)
         * @throws std::invalid_argument on programmer error (empty
         *         endpoint or bucket), exactly as the Phase-2 empty-path
         *         check does
         */
        static result<std::unique_ptr<MinioS3Client>, BlockStatus> Open(
            const S3ClientConfig& config);

        ~MinioS3Client() override;

        MinioS3Client(const MinioS3Client&) = delete;
        MinioS3Client& operator=(const MinioS3Client&) = delete;

        result<S3PutResult, BlockStatus> Put(
            const std::string& key,
            const std::vector<uint8_t>& bytes,
            const PutObjectOptions& options = {}) override;

        result<S3Object, BlockStatus> Get(const std::string& key) override;

        result<S3HeadResult, BlockStatus> Head(const std::string& key) override;

        result<void, BlockStatus> Delete(const std::string& key) override;

    private:
        MinioS3Client();

        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace sgns::blockstore

#endif // SGNS_BLOCKSTORE_MINIOS3CLIENT_HPP
