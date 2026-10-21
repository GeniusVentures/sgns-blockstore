/**
 * @file MinioServerHarness.hpp
 * @brief Env-gated MinIO server child-process harness (D-06/D-07/D-09, Q2).
 *
 * Locate-without-download contract: the harness NEVER downloads a MinIO
 * binary and NEVER contacts the public MinIO playground or any
 * live-internet endpoint (threat T-3-04 posture). The binary arrives
 * exclusively via the SGNS_BLOCKSTORE_TEST_MINIO_BIN env var pointing at a
 * pre-installed server binary; consuming tests emit a loud labelled skip
 * ([SGNS_BLOCKSTORE][MINIO-TIER]) when the gate is unset.
 *
 * Lifecycle discipline mirrors LocalBlockStore::Close teardown ordering:
 * terminate child -> wait for exit -> remove temp dir, all idempotent
 * (threat T-3-06). Credentials are randomly generated per run (password
 * at least 8 chars, threat T-3-01) and are never logged.
 */

#ifndef SGNS_BLOCKSTORE_TEST_MINIOSERVERHARNESS_HPP
#define SGNS_BLOCKSTORE_TEST_MINIOSERVERHARNESS_HPP

#include <cstdint>
#include <string>

namespace sgns::blockstore::testsupport
{
    /**
     * @brief Opt-in MinIO server tier for live-endpoint evidence (SC4a).
     * @note The header deliberately carries no miniocpp types; the minio-cpp
     *       client used by MakeBucket() lives in the implementation TU only.
     */
    class MinioServerHarness
    {
    public:
        /// @return true when SGNS_BLOCKSTORE_TEST_MINIO_BIN is set and points
        ///         at an existing file (never downloads anything).
        static bool Available();

        /// @return the env-var binary path ("" when unset).
        static std::string BinaryPath();

        MinioServerHarness() = default;
        ~MinioServerHarness(); ///< Stop() if still running (idempotent).

        MinioServerHarness(const MinioServerHarness&) = delete;
        MinioServerHarness& operator=(const MinioServerHarness&) = delete;

        /**
         * @brief Spawn `<bin> server <tempdir> --address 127.0.0.1:<port>`
         *        with per-run random root credentials and poll
         *        /minio/health/live until ready (deadline-bounded, never a
         *        fixed sleep).
         * @param tls when true, generate a self-signed certificate (openssl
         *            resolved via SGNS_BLOCKSTORE_TEST_OPENSSL_BIN, else the
         *            superbuild openssl install prefix) and start MinIO with
         *            --certs-dir; @p error carries the labelled reason when
         *            no openssl is resolvable (caller skips loudly)
         * @param error optional failure text
         * @return true when the health endpoint answered 200
         */
        bool Start(bool tls, std::string* error = nullptr);

        /// @brief Terminate -> wait -> remove temp dir (idempotent, T-3-06).
        void Stop();

        /// @return the port the server listens on (valid after Start()).
        uint16_t Port() const
        {
            return port_;
        }

        /// @return whether the server runs with TLS on.
        bool UseTls() const
        {
            return tls_;
        }

        /// @return per-run generated root credentials (never logged).
        const std::string& AccessKey() const
        {
            return access_key_;
        }
        const std::string& SecretKey() const
        {
            return secret_key_;
        }

        /**
         * @brief Create a bucket via minio-cpp against this harness's
         *        endpoint (fixture-setup convenience for consuming tests).
         * @return true on success; @p error carries the SDK error string
         */
        bool MakeBucket(const std::string& name, std::string* error = nullptr);

    private:
        std::string data_dir_;
        std::string certs_dir_;
        std::string access_key_;
        std::string secret_key_;
        uint16_t port_ = 0;
        bool tls_ = false;
        bool running_ = false;
        void* child_ = nullptr; ///< OS process handle (void* keeps header portable)
    };
} // namespace sgns::blockstore::testsupport

#endif // SGNS_BLOCKSTORE_TEST_MINIOSERVERHARNESS_HPP
