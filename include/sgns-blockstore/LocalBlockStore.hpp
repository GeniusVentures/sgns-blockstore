/**
 * @file LocalBlockStore.hpp
 * @brief The durable local RocksDB cold-tier backend (BACK-03/BACK-04).
 */

#ifndef SGNS_BLOCKSTORE_LOCALBLOCKSTORE_HPP
#define SGNS_BLOCKSTORE_LOCALBLOCKSTORE_HPP

#include <memory>
#include <string>
#include <vector>

#include <gsl/span>

#include <sgns-blockstore/Block.hpp>
#include <sgns-blockstore/BlockStatus.hpp>
#include <sgns-blockstore/BlockStore.hpp>
#include <sgns-blockstore/BlockStoreTooling.hpp>
#include <sgns-blockstore/Cid.hpp>
#include <sgns-blockstore/Durability.hpp>
#include <sgns-blockstore/FaultController.hpp>

namespace sgns::blockstore
{
    /// Column-family name holding the block bytes ("blocks"). Published so
    /// audit tooling and tests can address the store's column families.
    static constexpr const char* kBlocksColumnFamilyName = "blocks";

    /// Column-family name holding the upload journal ("upload_journal").
    /// Published so audit tooling and tests can address the store's column
    /// families.
    static constexpr const char* kUploadJournalColumnFamilyName = "upload_journal";

    /**
     * @brief The lifecycle state of a block's remote-upload journal record.
     *
     * Phase 2 commits a PendingUpload record in the SAME WriteBatch as the
     * block bytes (BACK-04 birth semantics, D-03); the transition methods
     * Phase 4 composes over (MarkUploadedAccepted/MarkUploadedDurable plus
     * the state accessor) land with Plan 02-02.
     */
    enum class UploadState
    {
        PendingUpload,    ///< committed locally, remote upload not yet acknowledged
        UploadedAccepted, ///< object store acknowledged receipt (durability unproven)
        UploadedDurable,  ///< object store proved durable storage
    };

    /**
     * @brief Options for opening a LocalBlockStore.
     */
    struct LocalBlockStoreOptions
    {
        /**
         * @brief Directory of the store's OWN RocksDB instance.
         *
         * NEVER the GlobalDB handle and never a directory shared with a
         * CRDT/GlobalDB instance (D-01): LocalBlockStore always opens its
         * own RocksDB database in this directory. An empty path is
         * programmer error and throws std::invalid_argument from Open.
         *
         * v1 fixes avoid_flush_during_shutdown=true at open: Close() never
         * flushes memtables, so a reopen ALWAYS replays the unflushed WAL
         * tail - the deterministic, crash-faithful recovery path.
         */
        std::string path;
    };

    /**
     * @brief Durable local backend: block bytes plus a day-one
     *        PendingUpload journal record committed in ONE atomic
     *        WriteBatch to the store's own RocksDB instance
     *        (BACK-03/BACK-04).
     *
     * Durability mapping (D-02/D-10 duty): BACK-03's
     * "(temp + fsync + rename)" is satisfied by RocksDB's own WAL commit
     * machinery - one WriteBatch committed with WriteOptions::sync=true
     * IS the atomic-durable commit. There are NO temp files by design, so
     * a startup temp-file sweep is vacuous; startup reconciliation is
     * journal enumeration (GetPendingUploads) only.
     *
     * Pimpl (Phase-2 Pattern 1): zero RocksDB types in this header - all
     * rocksdb::* usage lives in LocalBlockStore.cpp, so a consumer's
     * compile can never be broken by RocksDB version differences
     * (dual-compat 9.4/11.1 by construction).
     *
     * Publicly inherits BlockStore, FaultController AND
     * BlockStoreTooling: the fault seam (D-10) and the tooling surface
     * (CORE-06) are both probeable from separately-compiled test TUs.
     *
     * @note Thread-safety contract (v1, CORE-03/CORE-05 concurrency
     *       edges): single-threaded use - no internal synchronization is
     *       promised (same contract as MemoryBlockStore).
     */
    class LocalBlockStore final : public BlockStore,
                                  public FaultController,
                                  public BlockStoreTooling
    {
    public:
        ~LocalBlockStore() override; // out-of-line: Impl is incomplete here

        /**
         * @brief Open (or create) the store's own RocksDB instance.
         * @param options the store directory
         * @return the opened store, or a mapped BlockStatus failure
         *         (lock held / IO / corruption surface as
         *         TemporarilyUnavailable or Corrupt - never NotFound)
         * @throws std::invalid_argument when options.path is empty
         *         (programmer error, Phase-1 D-08 precedent)
         */
        static result<std::unique_ptr<LocalBlockStore>, BlockStatus> Open(
            const LocalBlockStoreOptions& options);

        /**
         * @brief Close the store (idempotent).
         *
         * Teardown order (Phase-2 Pitfall 7): destroy the blocks/journal
         * column-family handles (never the default-CF handle) ->
         * DB::Close() (status mapped, never propagated as a failure that
         * skips the frees) -> delete the DB. This exact sequence releases
         * the Windows LOCK so a same-process reopen of the same directory
         * succeeds.
         *
         * @return success, or the mapped Close status
         */
        result<void, BlockStatus> Close();

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

        /** @copydoc BlockStoreTooling::GetPendingUploads() */
        result<std::vector<Cid>, BlockStatus> GetPendingUploads() override;

        /** @copydoc BlockStoreTooling::ForEach() */
        result<void, BlockStatus> ForEach(
            const std::function<void(const Cid&, gsl::span<const uint8_t>)>& visitor)
            override;

        /** @copydoc FaultController::SetFault() */
        void SetFault(FaultKind kind) override;

        /** @copydoc FaultController::GetFault() */
        FaultKind GetFault() const override;

    private:
        LocalBlockStore(); // construction goes through Open()

        class Impl; // RocksDB state - defined in LocalBlockStore.cpp
        std::unique_ptr<Impl> impl_;
        FaultKind fault_ = FaultKind::None; ///< conformance seam state (never read by production paths)
    };
}

#endif  // SGNS_BLOCKSTORE_LOCALBLOCKSTORE_HPP
