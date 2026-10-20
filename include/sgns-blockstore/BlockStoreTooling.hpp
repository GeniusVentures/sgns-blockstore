/**
 * @file BlockStoreTooling.hpp
 * @brief The tooling/audit-only enumeration surface (CORE-06).
 */

#ifndef SGNS_BLOCKSTORE_BLOCKSTORETOOLING_HPP
#define SGNS_BLOCKSTORE_BLOCKSTORETOOLING_HPP

#include <functional>

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
     * @note v1 ships the header-level separation and this documented
     *       contract only - no backend implements it yet. Future
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
    };
}

#endif  // SGNS_BLOCKSTORE_BLOCKSTORETOOLING_HPP
