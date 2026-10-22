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
#include <set>
#include <string>
#include <vector>

#include <sgns-blockstore/S3Client.hpp>

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
         * @brief Script a fault for ONE key + op kind - the key-scoped
         *        overlay consulted BEFORE the op-level table: a keyed
         *        fault wins over the op-wide setting for that key while
         *        every other key keeps the op-wide behavior (D-05(c):
         *        "exactly one item fails inside a single drain" becomes
         *        mechanically expressible). kind == None clears the
   *        entry; ClearFaults clears BOTH tables.
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        void SetFaultForKey(const std::string& key, S3Op op,
                            S3FaultKind kind);

        /**
         * @brief Serve one key's Get from the SUCCESS slot with its
         *        first stored byte XOR-flipped (skipped when the stored
         *        value is empty). The ADAPTER succeeds - S3BlockStore's
         *        CID verification is the detector (the D-08 layer split;
         *        this is deliberately NOT an S3FaultKind: CorruptData
         *        returns a Corrupt FAILURE from the adapter and stays
         *        exactly as-is).
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        void SetFlipBytesForKey(const std::string& key, bool enabled);

        /**
         * @brief Clear every scripted fault (BOTH tables: the op-level
         *        table and the key-scoped overlay; the flip set clears
         *        per key via SetFlipBytesForKey(key, false)).
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        void ClearFaults();

        /**
         * @brief The sorted set of stored object keys (SC1 key
         *        observability).
         * @return every key currently in the fake, sorted - the probe the
         *         key-layout tests assert the BACK-07 layout against
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        std::vector<std::string> ObjectKeys() const;

        result<S3PutResult, BlockStatus> Put(
            const std::string& key,
            const std::vector<uint8_t>& bytes,
            const PutObjectOptions& options = {}) override;

        result<S3Object, BlockStatus> Get(const std::string& key) override;

        result<S3HeadResult, BlockStatus> Head(const std::string& key) override;

        result<void, BlockStatus> Delete(const std::string& key) override;

    private:
        /// @brief Taxonomy outcome for the scripted fault - the KEY-scoped
        ///        overlay first (key-scoped precedence), then the op-level
        ///        table - or nullopt when unfaulted (FaultGate philosophy).
        std::optional<BlockStatus> FaultGate(const std::string& key,
                                             S3Op op) const;

        /// @brief Map a scripted S3FaultKind to its taxonomy outcome.
        static std::optional<BlockStatus> FaultFor(S3FaultKind kind);

        mutable std::mutex mutex_;
        std::map<std::string, std::vector<uint8_t>> objects_;
        /// @brief Key-scoped fault overlay, consulted BEFORE faults_.
        std::map<std::string, std::array<S3FaultKind, 4>> key_faults_;
        /// @brief Keys whose Get success slot serves a flipped copy.
        std::set<std::string> flip_keys_;
        std::array<S3FaultKind, 4> faults_ = {
            S3FaultKind::None, S3FaultKind::None,
            S3FaultKind::None, S3FaultKind::None,
        };
    };
} // namespace sgns::blockstore

#endif // SGNS_BLOCKSTORE_INMEMORYS3CLIENT_HPP
