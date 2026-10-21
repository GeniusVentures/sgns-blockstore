/**
 * @file BlockStoreTooling.hpp
 * @brief The tooling/audit-only enumeration surface (CORE-06).
 */

#ifndef SGNS_BLOCKSTORE_BLOCKSTORETOOLING_HPP
#define SGNS_BLOCKSTORE_BLOCKSTORETOOLING_HPP

#include <functional>
#include <vector>

#include <gsl/span>

#include <sgns-blockstore/BlockStatus.hpp>
#include <sgns-blockstore/Cid.hpp>

namespace sgns::blockstore
{
    /**
     * @brief The enumeration surface for tooling and audit - NEVER part of
     *        BlockStore (CORE-06).
     *
     * The BlockStore interface itself exposes no List/Enumerate/ForEach
     * member and never will; enumeration lives on this separate
     * opt-in interface so operational tooling can audit store contents
     * without the published data-path API ever growing a listing
     * capability.
     *
     * @note v1 implementer: LocalBlockStore (Phase 2) implements this
     *       interface; the header-level separation predates it. Future
     *       implementers MUST honor the ordering contract below.
     * @note Ordering contract: iteration proceeds in lexicographic order
     *       over the ENCODED CID bytes (ContentIdentifierCodec::encode),
     *       over a SNAPSHOT of the store taken at iteration start -
     *       concurrent mutations during iteration do not affect the
     *       visited set (CORE-06 ordering edge, resolved as an explicit
     *       documented contract).
     */
    class BlockStoreTooling
    {
    public:
        virtual ~BlockStoreTooling() = default;

        /**
         * @brief Visit every block in the store, in documented order.
         * @param visitor called once per block with its CID and bytes;
         *                may not re-enter the store
         * @return success when iteration completed, or the BlockStatus
         *         failure that aborted it
         */
        virtual result<void, BlockStatus> ForEach(
            const std::function<void(const Cid&, gsl::span<const uint8_t>)>& visitor)
            = 0;

        /**
         * @brief Enumerate the pending-upload set for restart
         *        reconciliation (D-05).
         *
         * Returns the CIDs whose upload-journal state is PendingUpload -
         * blocks committed locally whose remote upload has never been
         * acknowledged. Placement follows D-05: enumeration NEVER lives
         * on BlockStore (CORE-06); this tooling surface is its only
         * home. No callback machinery in v1: the scan is synchronous and
         * the pending set is available immediately after a store Open.
         *
         * @note Ordering contract: the returned CIDs appear in the same
         *       order as ForEach iteration - lexicographic over the
         *       ENCODED CID bytes, over a snapshot of the journal taken
         *       at call start.
         *
         * @return the pending-upload CID set in documented order, or the
         *         BlockStatus failure that aborted the scan
         */
        virtual result<std::vector<Cid>, BlockStatus> GetPendingUploads() = 0;
    };
}

#endif  // SGNS_BLOCKSTORE_BLOCKSTORETOOLING_HPP
