/**
 * @file UploadDrainer.hpp
 * @brief One-shot drain of a LocalBlockStore's pending-upload set into
 *        a remote BlockStore (Phase 4, D-01..D-04) - CORE-06's tooling
 *        pending-set enumeration surface gets its first production
 *        consumer.
 *
 * Composition (D-01): snapshot the local pending set
 * (BlockStoreTooling::GetPendingUploads), then per CID fetch the bytes
 * locally, Put them to the remote store, and advance the local journal
 * to the Accepted level when - and ONLY when - the remote outcome is
 * remote-present. The remote parameter is the generic BlockStore (the
 * intended S3 tier, any store by type); the local parameter is the
 * concrete LocalBlockStore because the journal surfaces are its own.
 *
 * Per-item contract (D-03): a failing item - local Get miss (e.g. the
 * bytes were evicted), taxonomy failure, or journal-mark failure - is
 * RECORDED in its DrainItem and the drain continues; a failed item
 * never rolls back or aborts its siblings. The outer envelope is
 * reserved for wholesale malfunction that prevents per-item reporting
 * at all (the GetPendingUploads failure; PutMany outer-precedent).
 *
 * Journal discipline (D-04): the journal advances to the Accepted
 * level ONLY on remote-present outcomes - the remote Put's SUCCESS
 * slot (achieved OR alreadyPresent; a re-drain re-put of a block the
 * remote already holds returns alreadyPresent in the success slot, so
 * interrupted drains converge idempotently). The Durable-level
 * transition is deliberately OUT of the drainer: it is caller/tooling
 * territory until durability polling ships - the drainer never
 * invokes it.
 */

#ifndef SGNS_BLOCKSTORE_UPLOADDRAINER_HPP
#define SGNS_BLOCKSTORE_UPLOADDRAINER_HPP

#include <vector>

#include <sgns-blockstore/BlockStore.hpp>
#include <sgns-blockstore/BlockStatus.hpp>
#include <sgns-blockstore/Cid.hpp>
#include <sgns-blockstore/Durability.hpp>
#include <sgns-blockstore/LocalBlockStore.hpp>
#include <sgns-blockstore/Options.hpp>

namespace sgns::blockstore
{
    /**
     * @brief The per-item drain outcome: BOTH the remote upload result
     *        and the journal-mark result.
     *
     * When the upload failed, no mark ran - `journal` then carries its
     * default success (an outcome of "no transition attempted", never
     * a fabricated failure). When the upload succeeded, `journal`
     * carries the Accepted-level transition's own outcome (a mark
     * failure never rolls back the upload; the CID stays pending and
     * the mark retries on the next drain).
     */
    struct DrainItem
    {
        Cid cid;                                ///< the drained block
        result<PutOutcome, BlockStatus> upload; ///< the remote Put outcome
        /// The Accepted-level journal transition's outcome (default
        /// success when no mark ran - upload failed).
        result<void, BlockStatus> journal = outcome::success();
    };

    /**
     * @brief Drain every pending-upload block of `local` into `remote`.
     *
     * Synchronous one-shot semantics (D-02): iterates the pending-set
     * snapshot in documented order (lexicographic over encoded CID
     * bytes), at most one remote Put per pending CID per drain. No
     * RocksDB access and no journal writes except through the
     * LocalBlockStore methods - the Phase-2 surfaces exist for exactly
     * this composition.
     *
     * @param local the local store whose pending set drains and whose
     *              journal advances
     * @param remote the remote BlockStore receiving the bytes (the S3
     *               tier in practice; generic by type, D-01)
     * @param options write options forwarded to every remote Put
     * @return one DrainItem per pending CID (snapshot order), or the
     *         wholesale BlockStatus failure that aborted the drain
     *         before any item ran (pending-set enumeration failure)
     *
     * @note Thread-safety contract (v1, CORE-03): single-threaded use
     *       - both stores are touched synchronously on the caller's
     *       thread; no internal synchronization is promised.
     */
    result<std::vector<DrainItem>, BlockStatus> DrainPendingUploads(
        LocalBlockStore& local, BlockStore& remote,
        const WriteOptions& options = {});
} // namespace sgns::blockstore

#endif // SGNS_BLOCKSTORE_UPLOADDRAINER_HPP
