/**
 * @file MemoryBlockStore.hpp
 * @brief The hermetic in-memory reference backend (BACK-01).
 */

#ifndef SGNS_BLOCKSTORE_MEMORYBLOCKSTORE_HPP
#define SGNS_BLOCKSTORE_MEMORYBLOCKSTORE_HPP

#include <map>
#include <vector>

#include <sgns-blockstore/Block.hpp>
#include <sgns-blockstore/BlockStore.hpp>
#include <sgns-blockstore/FaultController.hpp>

namespace sgns::blockstore
{
    /**
     * @brief Hermetic reference backend: a std::map of verified blocks.
     *
     * The verify-at-facade discipline lives here (S5 lesson): every Put
     * computes CidOf(bytes, cid.content_type) and rejects a mismatch as
     * Corrupt+Caller; every Get re-verifies the returned bytes against
     * the CID unless ReadOptions::verify == false (CORE-04). The
     * verification lives in the backend's shared path - never
     * per-call-site, never compiled out.
     *
     * Publicly inherits BOTH BlockStore and FaultController: the dual
     * base is what makes std::dynamic_pointer_cast&lt;FaultController&gt;(store)
     * succeed, so the BlockStoreConformanceTests kit gets fault
     * conformance for free (D-10).
     *
     * @note Thread-safety contract (v1, CORE-03/CORE-05 concurrency
     *       edges): single-threaded use - no internal synchronization is
     *       promised. This documented contract also covers
     *       EvictLocal/Get interleaving.
     */
    class MemoryBlockStore : public BlockStore, public FaultController
    {
    public:
        ~MemoryBlockStore() override = default;

        /** @copydoc BlockStore::Put() */
        result<PutOutcome, BlockStatus> Put(
            const Cid& cid, gsl::span<const uint8_t> bytes,
            const WriteOptions& options = {}) override;

        /** @copydoc BlockStore::Get() */
        result<Block, BlockStatus> Get(
            const Cid& cid, const ReadOptions& options = {}) override;

        /** @copydoc BlockStore::Has() */
        result<bool, BlockStatus> Has(const Cid& cid) override;

        /** @copydoc BlockStore::GetSize() */
        result<uint64_t, BlockStatus> GetSize(const Cid& cid) override;

        /** @copydoc BlockStore::PutMany() */
        result<std::vector<result<PutOutcome, BlockStatus>>, BlockStatus>
        PutMany(gsl::span<const BlockInput> blocks,
                const WriteOptions& options = {}) override;

        /**
         * @brief Remove this store's LOCAL copy of a block (D-16).
         *
         * Concrete method on the backend type ONLY - never on BlockStore.
         * Idempotent: success whenever no local bytes remain, whether or
         * not the CID was present. Local eviction is not a CRDT deletion
         * and never touches any remote tier.
         *
         * @param cid the block whose local bytes to drop
         * @return success once the post-state (no local bytes) holds
         */
        result<void, BlockStatus> EvictLocal(const Cid& cid);

        /** @copydoc FaultController::SetFault() */
        void SetFault(FaultKind kind) override;

        /** @copydoc FaultController::GetFault() */
        FaultKind GetFault() const override;

    private:
        std::map<Cid, std::vector<uint8_t>> storage_; ///< operator<-keyed (no std::hash on ContentIdentifier - research Pitfall 4)
        FaultKind fault_ = FaultKind::None; ///< conformance seam state (never read by production paths)
    };
}

#endif  // SGNS_BLOCKSTORE_MEMORYBLOCKSTORE_HPP
