/**
 * @file LocalBlockStore.hpp
 * @brief The durable local RocksDB cold-tier backend (BACK-03/BACK-04).
 */

#ifndef SGNS_BLOCKSTORE_LOCALBLOCKSTORE_HPP
#define SGNS_BLOCKSTORE_LOCALBLOCKSTORE_HPP

#include <memory>
#include <optional>
#include <string>
#include <utility>
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

    /// Column-family name holding the spill timestamps ("spill_times",
    /// v1.1 Phase 11 D-01). Published so audit tooling and tests can
    /// address the store's column families.
    static constexpr const char* kSpillTimesColumnFamilyName = "spill_times";

    /// Column-family name holding the disposed-CID markers
    /// ("disposed_cids", v1.1 Phase 11 D-09/D-10). Published so audit
    /// tooling and tests can address the store's column families.
    static constexpr const char* kDisposedColumnFamilyName = "disposed_cids";

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
     * @brief One Durable journal record paired with its spill-timestamp
     *        age evidence (D-01, v1.1 Phase 11).
     *
     * spilled_at_unix_ms == 0 means NO spill_times entry exists for the
     * record (a pre-Phase-11 store, or a record whose side-CF write was
     * never born): the caller backfills via SetSpilledAt and treats the
     * record as young for one full retention period (the under-eviction
     * direction).
     */
    struct SpillEntry
    {
        Cid     cid;                   ///< the Durable journal record's CID
        uint64_t spilled_at_unix_ms{0}; ///< wall-clock unix-ms spill time (0 = absent)
    };
    /**
     * @brief The terminal disposed-CID marker record (D-09, v1.1
     *        Phase 11).
     *
     * Written ONLY by EvictLocalAndMarkDisposed's single batch and read
     * by IsDisposed/GetDisposed/TrimDisposed; the bounded re-delivery
     * dedup state (D-10/D-11). was_sealed RECORDS the hot seal state at
     * eviction for a future phase - NO v1.1 code path restores it
     * (rehydrate stays bytes-only, Phase-9 SC4).
     */
    struct DisposedRecord
    {
        uint64_t disposed_at_unix_ms{0}; ///< wall-clock eviction time
        uint64_t reclaimed_bytes{0};     ///< local bytes freed by the eviction
        uint64_t original_size{0};       ///< the block's stored size at eviction
        uint8_t reason{0};               ///< eviction reason code (policy-internal)
        bool was_sealed{false};          ///< hot seal-marker state at eviction

        /// Fixed-layout codec (26 bytes, little-endian): [u64 disposed_at]
        /// [u64 reclaimed][u64 original_size][u8 reason][u8 was_sealed].
        /// Library-owned: records are never assembled from caller strings.
        static std::string Encode(const DisposedRecord& record);
        static std::optional<DisposedRecord> Decode(const std::string& value);
    };

    /// Default disposed-marker count cap (D-10) for TrimDisposed.
    static constexpr size_t kDefaultDisposedMarkerCap{100000};
    /// Default disposed-marker max age (30 days in unix-ms, D-10).
    static constexpr uint64_t kDefaultDisposedMarkerMaxAgeMs{2592000000ull};
    /**
     * @brief TEST-ONLY crash-point seam (D-07) - never read by
     *        production paths beyond the Put short-circuit.
     *
     * BeforeCommit models process death BEFORE the atomic DB::Write
     * commit: Put returns TemporarilyUnavailable WITHOUT writing, so
     * the caller never saw success. There is deliberately NO
     * mid-commit point: a WriteBatch serializes as ONE atomic WAL
     * record, so an interrupted DB::Write leaves the whole batch or
     * none of it - before/after DB::Write is the COMPLETE crash-window
     * pair. AfterCommit is implicit: any committed Put plus an
     * immediate no-flush close IS crash-after-commit (covered by the
     * restart and reconciliation tests).
     */
    enum class CrashPoint
    {
        None,         ///< normal operation
        BeforeCommit, ///< Put short-circuits before DB::Write (simulated death)
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
     * IS the atomic-durable commit. There are NO temp files by design,
     * so ROADMAP Phase-2 criterion 4's "sweeps stale temp files" is
     * VACUOUS by design - the startup sweep IS journal reconciliation
     * (GetPendingUploads) only.
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

        //
        // Concrete backend-only surface (D-06/D-10) - these methods live
        // on LocalBlockStore ONLY, never on BlockStore or any facade:
        // Phase 4's uploader composes over exactly this seam.
        //

        /**
         * @brief Record the remote store's accepted-but-not-yet-durable
         *        acknowledgement in ONE atomic journal update (D-06).
         *
         * Moves the CID's journal record to UploadedAccepted. Transitions
         * move Put-born records only: a CID with no journal record
         * returns a NotFound failure WITHOUT writing - upload state is
         * never fabricated for a block that was never Put.
         *
         * @param cid the block whose upload was acknowledged
         * @return success, or NotFound+Local when no record exists, or
         *         the mapped RocksDB failure
         */
        result<void, BlockStatus> MarkUploadedAccepted(const Cid& cid);

        /**
         * @brief Record confirmed remote durability in ONE atomic journal
         *        update (D-06).
         *
         * Moves the CID's journal record to UploadedDurable; the block
         * leaves the pending set. Same no-fabrication rule as
         * MarkUploadedAccepted.
         *
         * @param cid the block whose remote storage proved durable
         * @return success, or NotFound+Local when no record exists, or
         *         the mapped RocksDB failure
         */
        result<void, BlockStatus> MarkUploadedDurable(const Cid& cid);

        /**
         * @brief Record confirmed remote durability for a BATCH of CIDs
         *        in ONE atomic journal update (DUR-01, Phase 10).
         *
         * Per-item read-gate loop first (record existence; skip-and-
         * continue on a no-record CID - per-item no-fabrication
         * isolation, the batch never aborts on a missing record), then
         * ONE rocksdb::WriteBatch with a Put per VERIFIED key committed
         * by ONE sync Write: a single fsync and an all-or-nothing crash
         * window over the verified set (tighter than N single-CID
         * transitions - the flush-gated advancement marks exactly its
         * pre-flush snapshot this way).
         *
         * @param cids the blocks whose remote storage proved durable
         * @return success once the verified subset (possibly empty - an
         *         empty span is a success no-op) is durably marked;
         *         already-Durable records stay Durable
         */
        result<void, BlockStatus> MarkUploadedDurableMany(
            gsl::span<const Cid> const& cids);

        /**
         * @brief Read the CID's journal-record state (D-06 accessor).
         * @param cid the block whose journal state to read
         * @return the UploadState, or a NotFound+Local failure when the
         *         CID has no journal record (never a fabricated default
         *         state value); an undecodable record reads back
         *         conservatively as PendingUpload
         */
        result<UploadState, BlockStatus> GetUploadState(const Cid& cid);

        /**
         * @brief Enumerate the CIDs whose journal record is in the
         *        requested state (DUR-01, the D-03 snapshot source).
         *
         * The flush-gated durability advancement's Accepted snapshot IS
         * this enumeration: journal-derived (crash-re-derivable by
         * construction - a restart re-runs it and re-derives the same
         * set), snapshot-at-call-start semantics over a plain CF
         * iterator, and byte-lexicographic ordering over the encoded
         * CID bytes (the frozen ordering contract, satisfied naturally
         * by the default BytewiseComparator). Undecodable records are
         * skipped - an audit surface, never a scan crash.
         *
         * @param state the upload state to filter by
         * @return the matching CIDs in encoded-byte lexicographic order,
         *         or the mapped RocksDB scan failure
         */
        result<std::vector<Cid>, BlockStatus> GetUploadsInState(UploadState state);

        /**
         * @brief Read the CID's spill timestamp (D-01 age evidence, v1.1
         *        Phase 11).
         * @param cid the block whose spill time to read
         * @return the uint64 unix-ms timestamp, or a NotFound+Local
         *         failure when no spill_times entry exists (never a
         *         fabricated 0); IO/lock failures map to
         *         TemporarilyUnavailable via MapStatus (the
         *         absent-vs-unreadable distinction MI-02 depends on)
         */
        result<uint64_t, BlockStatus> GetSpilledAt(const Cid& cid);

        /**
         * @brief Write or overwrite the CID's spill timestamp (D-01).
         *
         * The backfill surface for pre-Phase-11 records (spilled_at == 0
         * from GetDurableWithSpillTime) and the TEST seam for age-gate
         * fixtures. ONE sync journal write - never called on the Put path
         * (birth timestamps ride Put's own atomic batch).
         * @param cid the block whose spill time to write
         * @param unix_ms the wall-clock unix-ms timestamp
         * @return success, or the mapped RocksDB failure
         */
        result<void, BlockStatus> SetSpilledAt(const Cid& cid, uint64_t unix_ms);

        /**
         * @brief Enumerate the UploadedDurable journal records WITH their
         *        spill timestamps (GC-01, the retention candidate source).
         *
         * ONE scan of the journal column family filtering UploadedDurable
         * with point reads in spill_times - the journal is the truth
         * (MI-01: never the advisory status counters). Entries come back
         * in the journal CF's encoded-byte lexicographic order over the
         * CID keys (the frozen ordering contract, deterministic and
         * restart-stable); a missing side-CF entry yields
         * spilled_at_unix_ms == 0 (absent, never fabricated). Undecodable
         * journal records are skipped - an audit surface, never a scan
         * crash.
         * @return the Durable SpillEntry set in encoded-byte order, or
         *         the mapped RocksDB scan failure
         */
        result<std::vector<SpillEntry>, BlockStatus> GetDurableWithSpillTime();

        /**
         * @brief The D-16 HALF-1 destructive primitive: remove this
         *        store's local copy of a block AND record the terminal
         *        disposed marker in ONE atomic WriteBatch (v1.1 Phase
         *        11).
         *
         * ONE sync Write removes the block bytes, the journal record,
         * AND the spill timestamp, and writes the disposed marker -
         * three Deletes and one Put in a single all-or-nothing batch
         * (the EvictLocal composition). Idempotent by post-state:
         * deleting already-absent keys succeeds and the marker Put is
         * last-writer-wins, so a replay over an already-evicted CID
         * leaves exactly the same state. The caller owns the eviction
         * policy (the durability gate lives ABOVE this primitive);
         * the hot-store cleanup half NEVER runs here (the two halves
         * never share a store - Pitfall 3).
         *
         * @warning extends EvictLocal's D-10 caveat: policy callers must
         *          gate on EXACTLY UploadedDurable before evicting - this
         *          primitive never consults durability itself.
         * @param cid the block whose local state becomes "disposed"
         * @param record the marker content (library codec, never caller
         *        strings)
         * @return success once the post-state (no local bytes, marker
         *         present) holds
         */
        result<void, BlockStatus> EvictLocalAndMarkDisposed(const Cid& cid,
                                                            const DisposedRecord& record);

        /**
         * @brief Disposed-marker point lookup (D-11).
         * @param cid the block whose marker to check
         * @return true when the marker exists; FALSE on absence AND on
         *         any non-authoritative failure (the bool contract the
         *         freeze-sensitive pubsub-path callers need - never
         *         blocks, never throws)
         */
        result<bool, BlockStatus> IsDisposed(const Cid& cid);

        /**
         * @brief Enumerate the disposed markers (D-10, the trim +
         *        orphan-re-scan source).
         *
         * Snapshot-consistent CF iterator, encoded-byte lexicographic
         * order over the CID keys, undecodable values skipped - the
         * GetUploadsInState discipline.
         * @return the (CID, DisposedRecord) pairs in encoded-byte order,
         *         or the mapped RocksDB scan failure
         */
        result<std::vector<std::pair<Cid, DisposedRecord>>, BlockStatus> GetDisposed();

        /**
         * @brief Enforce BOTH disposed-marker bounds in ONE batch
         *        (D-10): drop entries older than max_age_unix_ms, then
         *        oldest-first until at most count_cap remain (whichever
         *        bound trims first; count_cap 0 or max_age 0 disables
         *        that bound).
         *
         * A trimmed CID that re-arrives is re-learned through the
         *        cold chain (re-journal -> re-spill -> re-drain; the
         *        remote copy makes this safe).
         * @param count_cap the maximum markers to keep (0 = no cap)
         * @param max_age_unix_ms drop markers with disposed_at STRICTLY
         *        BELOW this bound (0 = no age bound)
         * @return success once the post-state holds
         */
        result<void, BlockStatus> TrimDisposed(size_t count_cap,
                                               uint64_t max_age_unix_ms);

        /**
         * @brief Remove this store's LOCAL copy of a block, unconditionally
         *        (D-10).
         *
         * ONE sync WriteBatch removes the block bytes AND the journal
         * record regardless of journal state - the caller owns eviction
         * policy; this method never consults durability. Idempotent:
         * success whenever no local bytes remain, whether or not the CID
         * was present. Local eviction is not a CRDT deletion and never
         * touches any remote tier.
         *
         * @warning D-10 caveat: evicting a block whose journal state is
         *          not UploadedDurable may discard the ONLY copy of the
         *          data - the caller owns that decision. Durability-gated
         *          eviction is deferred future work per REQUIREMENTS.
         *
         * @param cid the block whose local bytes and journal record to drop
         * @return success once the post-state (no local bytes) holds
         */
        result<void, BlockStatus> EvictLocal(const Cid& cid);

        /**
         * @brief Arm the TEST-ONLY crash-point seam (D-07).
         *
         * Concrete method on the backend type ONLY - the frozen
         * FaultKind enum and FaultController surface are untouched.
         * Defaults to CrashPoint::None; BeforeCommit makes subsequent
         * Puts return TemporarilyUnavailable before any DB::Write call
         * without mutating stored state.
         *
         * @param point the crash point to arm (None clears)
         */
        void SetCrashPointForTesting(CrashPoint point);

        /**
         * @brief Arm the TEST-ONLY corrupt-after-commit hook (D-09).
         *
         * Concrete method on the backend type ONLY - the frozen
         * FaultController surface is untouched (the kit's CorruptData
         * fault corrupts only the returned copy; this hook corrupts REAL
         * stored bytes). When armed, the NEXT successful Put of a NEW
         * block returns success as usual while the committed block bytes
         * are corrupted in place after the WriteBatch lands: the store's
         * OWN handles read the just-committed value back, flip its first
         * byte, and write it back with sync=true - so the corruption
         * survives the no-flush close+reopen the crash suite uses as its
         * crash oracle. Never fires on the already_present re-Put path,
         * never touches the journal column family, and is never routed
         * through the fault_/FaultGate seam. PRODUCTION CODE MUST NEVER
         * CALL THIS - arming it on real stored data is the catastrophic
         * misuse.
         *
         * @param armed true arms the hook for the next new-block Put
         *        (false disarms)
         */
        void SetCorruptAfterCommitForTesting(bool armed);

    private:
        LocalBlockStore(); // construction goes through Open()

        /** @brief Shared gate+read+one-batch-write body of both journal
         *         transitions (D-06). */
        result<void, BlockStatus> TransitionJournal(const Cid& cid,
                                                    UploadState new_state);

        class Impl; // RocksDB state - defined in LocalBlockStore.cpp
        std::unique_ptr<Impl> impl_;
        FaultKind fault_ = FaultKind::None; ///< conformance seam state (never read by production paths)
        CrashPoint crash_point_ = CrashPoint::None; ///< test-only crash seam state (D-07)
        bool corrupt_after_commit_ = false; ///< test-only corrupt-after-commit seam state (D-09)
    };
}

#endif  // SGNS_BLOCKSTORE_LOCALBLOCKSTORE_HPP
