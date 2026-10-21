/**
 * @file InMemoryS3Client.hpp
 * @brief Stateful hermetic fake of the S3Client seam (D-05, SC3 tier).
 *
 * A test-only implementation: a plain object map with createOnly
 * enforcement and per-op fault scripting. It is NEVER read by production
 * paths - the fault table exists so the taxonomy classes can be induced
 * deterministically (FaultController discipline) and so Phase 5's fault
 * matrix composes on the same surface.
 *
 * Faults map to adapter-level taxonomy OUTCOMES, not raw wire bytes - the
 * wire-level fault matrix (truncated/garbage responses) is the
 * LoopbackHttpStub tier against MinioS3Client (03-03, D-08).
 */

#ifndef SGNS_BLOCKSTORE_INMEMORYS3CLIENT_HPP
#define SGNS_BLOCKSTORE_INMEMORYS3CLIENT_HPP

#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "S3Client.hpp"

namespace sgns::blockstore
{
    /**
     * @brief The in-memory S3Client fake (D-05).
     * @note Single-threaded contract like the rest of the Phase-3 seam;
     *       the mutex only makes the no-data-race statement cheap.
     */
    class InMemoryS3Client final : public S3Client
    {
    public:
        InMemoryS3Client() = default;
        ~InMemoryS3Client() override = default;

        /**
         * @brief Script a fault for one op kind (FIFO-free: persists until
         *        cleared or overwritten for that op).
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        void SetFault(S3Op op, S3FaultKind kind);

        /**
         * @brief Clear every scripted fault.
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        void ClearFaults();

        result<S3PutResult, BlockStatus> Put(
            const std::string& key,
            const std::vector<uint8_t>& bytes,
            const PutObjectOptions& options = {}) override;

        result<S3Object, BlockStatus> Get(const std::string& key) override;

        result<S3HeadResult, BlockStatus> Head(const std::string& key) override;

        result<void, BlockStatus> Delete(const std::string& key) override;

    private:
        /// @brief Taxonomy outcome for the scripted fault, or nullopt when
        ///        the op is unfaulted (FaultGate philosophy).
        std::optional<BlockStatus> FaultGate(S3Op op) const;

        mutable std::mutex mutex_;
        std::map<std::string, std::vector<uint8_t>> objects_;
        std::array<S3FaultKind, 4> faults_ = {
            S3FaultKind::None, S3FaultKind::None,
            S3FaultKind::None, S3FaultKind::None,
        };
    };
} // namespace sgns::blockstore

#endif // SGNS_BLOCKSTORE_INMEMORYS3CLIENT_HPP
