/**
 * @file BlockStore.hpp
 * @brief The frozen v1.0.0 BlockStore interface - List-free, delete-free,
 *        eviction-safe (CORE-05/CORE-06, D-01/D-03/D-04/D-13/D-14).
 */

#ifndef SGNS_BLOCKSTORE_BLOCKSTORE_HPP
#define SGNS_BLOCKSTORE_BLOCKSTORE_HPP

#include <gsl/span>
#include <vector>

#include <sgns-blockstore/Block.hpp>
#include <sgns-blockstore/BlockStatus.hpp>
#include <sgns-blockstore/Cid.hpp>
#include <sgns-blockstore/Durability.hpp>
#include <sgns-blockstore/Options.hpp>

namespace sgns::blockstore
{
    /**
     * @brief The content-addressed block store interface (D-01).
     *
     * Every public operation returns outcome::result<T, BlockStatus> -
     * the rich taxonomy (D-02) rides the failure slot. An authoritative
     * miss is a FAILURE value with code == NotFound (D-03): one uniform
     * pattern, no optional-in-success second pattern.
     *
     * Eviction invariant (quoted verbatim from
     * supergenius-sharded-ipld-cache-architecture.md):
     * "Eviction removes only local cached bytes. It does not delete the
     * object from Sia, remove the path-to-CID mapping, or imply a CRDT
     * deletion."
     * Local eviction is therefore a CONCRETE method on backend types
     * (MemoryBlockStore::EvictLocal now, LocalBlockStore::EvictLocal in
     * Phase 2) and NEVER a member of this interface (D-16).
     *
     * @note CORE-05/CORE-06 permanence: no Remove/Delete/Erase member and
     *       no List/Enumerate member will EVER exist on this interface -
     *       not in any phase, not on any future facade or decorator over
     *       it. Enumeration lives only on the separate
     *       BlockStoreTooling surface.
     *
     * @note PutMany contract (D-13/D-14): returns one result per input
     *       element where results[i] corresponds to blocks[i]; there is
     *       explicitly NO all-or-nothing guarantee - a failed item never
     *       rolls back its siblings. The outer outcome envelope is
     *       reserved for wholesale backend malfunction that prevents
     *       per-block reporting at all (planner resolution of research
     *       open question Q4).
     *
     * @note Programmer-error contract (D-08): a CID whose multihash type
     *       is not sha256 is rejected at API entry by THROWING
     *       std::invalid_argument BEFORE any write - it is a programmer
     *       error outside the 5-value runtime taxonomy, never a
     *       BlockStatus. Boundary: the repo's CRDT CBOR path
     *       (sgns::common::getCidOf) produces blake2b-256 CIDs, which
     *       this v1 API therefore does NOT accept - a documented v1
     *       non-goal; widening to more hash functions later is additive.
     */
    class BlockStore
    {
    public:
        virtual ~BlockStore() = default;

        /**
         * @brief Store a block after verifying its bytes against its CID.
         * @param cid the claimed content identifier (sha-256 multihash)
         * @param bytes the payload; must hash (sha-256, cid.content_type) to cid
         * @param options write options (deadline; ignored by never-blocking backends)
         * @return PutOutcome with the VERIFIED durability level, or
         *         BlockStatus failure (Corrupt+Caller on verification
         *         mismatch)
         */
        virtual result<PutOutcome, BlockStatus> Put(
            const Cid& cid, gsl::span<const uint8_t> bytes,
            const WriteOptions& options = {})
            = 0;

        /**
         * @brief Retrieve a block, re-verifying bytes against the CID
         *        unless ReadOptions::verify == false.
         * @param cid the content identifier to fetch
         * @param options read options (deadline, verify toggle)
         * @return the verified Block, or BlockStatus failure (NotFound is
         *         an authoritative miss ONLY)
         */
        virtual result<Block, BlockStatus> Get(
            const Cid& cid, const ReadOptions& options = {})
            = 0;

        /**
         * @brief Check presence of a CID.
         * @param cid the content identifier to check
         * @return true/false as SUCCESS values; under a non-authoritative
         *         failure a BlockStatus FAILURE is returned - NEVER the
         *         value false (CORE-02 non-conflation)
         */
        virtual result<bool, BlockStatus> Has(const Cid& cid) = 0;

        /**
         * @brief Report the stored byte count of a CID.
         * @param cid the content identifier to measure
         * @return the exact stored byte count (no bytes are returned, so
         *         no verification applies), or BlockStatus failure
         */
        virtual result<uint64_t, BlockStatus> GetSize(const Cid& cid) = 0;

        /**
         * @brief Store a batch of blocks with per-element results.
         * @param blocks the batch; results[i] corresponds to blocks[i]
         * @param options write options applied to every element
         * @return a vector of per-element results (inner failures do not
         *         roll back siblings - D-14), or an outer BlockStatus
         *         failure for wholesale malfunction that prevents
         *         per-block reporting
         */
        virtual result<std::vector<result<PutOutcome, BlockStatus>>,
                       BlockStatus>
            PutMany(gsl::span<const BlockInput> blocks,
                    const WriteOptions& options = {})
            = 0;
    };
}

#endif  // SGNS_BLOCKSTORE_BLOCKSTORE_HPP
