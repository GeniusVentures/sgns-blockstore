/**
 * @file local_block_store_crash_test.cpp
 * @brief Crash-safety suite for the durable RocksDB cold-tier backend
 *        (SC 3 + SC 4, BACK-03/BACK-04): the D-08 crash-window pair, the
 *        D-09 evict-and-surface corruption self-heal, and journal
 *        reconciliation across restart.
 *
 * CRASH MODEL (read before touching anything here):
 *  - In-process close-without-flush + reopen exercises RocksDB WAL
 *    recovery - the REAL recovery path a process crash takes. These
 *    tests prove WAL-replay atomicity semantics, NOT fsync/OS-level
 *    durability: the fsync claim rides WriteOptions::sync=true in the
 *    production Put path, and OS-level durability is Phase 5's
 *    subprocess-kill matrix (RESEARCH Pitfall 1).
 *  - There is deliberately NO mid-DB::Write fault point: a WriteBatch
 *    serializes as ONE atomic WAL record, so an interrupted Write leaves
 *    the whole batch or none of it (kPointInTimeRecovery applies complete
 *    records and stops at an incomplete tail). Before/after DB::Write is
 *    therefore the COMPLETE D-08 pair (RESEARCH State-of-the-Art note).
 *  - No test calls Flush() or SyncWAL() anywhere: with
 *    avoid_flush_during_shutdown=true the close-then-reopen path is
 *    crash-faithful, and any flush/sync would silently convert these
 *    proofs into no-ops (RESEARCH Pitfall 2).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <sgns-blockstore/Cid.hpp>
#include <sgns-blockstore/FaultController.hpp>
#include <sgns-blockstore/LocalBlockStore.hpp>

#include <libp2p/multi/content_identifier_codec.hpp>
#include <libp2p/multi/multicodec_type.hpp>

namespace
{
    using sgns::blockstore::BlockSource;
    using sgns::blockstore::BlockStatus;
    using sgns::blockstore::Cid;
    using sgns::blockstore::CidOf;
    using sgns::blockstore::CrashPoint;
    using sgns::blockstore::FaultKind;
    using sgns::blockstore::LocalBlockStore;
    using sgns::blockstore::LocalBlockStoreOptions;
    using sgns::blockstore::StatusClass;
    using sgns::blockstore::UploadState;
    using libp2p::multi::MulticodecType;

    gsl::span<const uint8_t> SpanOf(const std::string& s)
    {
        return gsl::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    Cid MakeCid(const std::string& payload)
    {
        return CidOf(SpanOf(payload), MulticodecType::Code::RAW).value();
    }

    // SHORT relative path under the build tree (Windows MAX_PATH); a
    // monotonic counter gives every case its own directory.
    std::string NextUniqueDir()
    {
        static std::atomic<uint64_t> next{0};
        return "test-data/local-blockstore-crash/case-"
             + std::to_string(next.fetch_add(1));
    }

    std::shared_ptr<LocalBlockStore> ReopenStore(const std::string& dir)
    {
        auto opened = LocalBlockStore::Open(LocalBlockStoreOptions{dir});
        if (!opened)
        {
            ADD_FAILURE() << "LocalBlockStore::Open failed for " << dir
                          << ": " << opened.error().message;
            return nullptr;
        }
        return std::shared_ptr<LocalBlockStore>(std::move(opened.value()));
    }

    std::shared_ptr<LocalBlockStore> MakeStore(const std::string& dir)
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return ReopenStore(dir);
    }

    // NOTE: ContentIdentifier has no default constructor, so the CID
    // members carry member-initializers.
    class LocalBlockStoreCrashTest : public ::testing::Test
    {
    public:
        std::string payload_a_ = "crash suite payload a";
        std::string payload_b_ = "crash suite payload b - different length";
        Cid cid_a_ = MakeCid(payload_a_);
        Cid cid_b_ = MakeCid(payload_b_);
    };
}

/**
 * @given a committed block whose stored bytes were corrupted in place
 *        by the D-09 corrupt-after-commit hook (the store's OWN handle,
 *        API-only - no second RocksDB handle, no CF-name coupling)
 * @when the reopened store Gets it twice, then the caller re-Puts
 * @then the FIRST Get fails Corrupt+Local (the detecting call surfaces
 *        the corruption), the SECOND Get fails NotFound - the self-heal
 *        atomically evicted block AND journal entries (D-09: Get is
 *        write-capable only on this path) - Has false, GetUploadState
 *        NotFound, pending set empty, and a re-Put of the good bytes
 *        repairs with alreadyPresent == false
 */
TEST_F(LocalBlockStoreCrashTest, StoredCorruptionSelfHealsEvictAndSurface)
{
    const std::string dir = NextUniqueDir();
    {
        auto store = MakeStore(dir);
        ASSERT_NE(store, nullptr);
        // D-09 hook: the Put commits successfully, then the commit tail
        // corrupts the stored bytes in place (sync write - survives the
        // no-flush close+reopen below).
        store->SetCorruptAfterCommitForTesting(true);
        ASSERT_TRUE(store->Put(cid_a_, SpanOf(payload_a_)));
        store->SetCorruptAfterCommitForTesting(false);
    } // destruction: no-flush close

    auto reopened = ReopenStore(dir);
    ASSERT_NE(reopened, nullptr);
    auto detecting = reopened->Get(cid_a_);
    ASSERT_FALSE(detecting);
    EXPECT_EQ(detecting.error().code, StatusClass::Corrupt);
    EXPECT_EQ(detecting.error().source, BlockSource::Local);

    auto after = reopened->Get(cid_a_);
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().code, StatusClass::NotFound); // evicted

    auto has = reopened->Has(cid_a_);
    ASSERT_TRUE(has);
    EXPECT_FALSE(has.value());
    auto state = reopened->GetUploadState(cid_a_);
    ASSERT_FALSE(state);
    EXPECT_EQ(state.error().code, StatusClass::NotFound); // co-removed
    auto pending = reopened->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty());

    auto repair = reopened->Put(cid_a_, SpanOf(payload_a_));
    ASSERT_TRUE(repair);
    EXPECT_FALSE(repair.value().alreadyPresent);
}

/**
 * @given a stored block and the CorruptData fault armed
 * @when Get fails Corrupt and the fault is cleared
 * @then Get SUCCEEDS with the original bytes and the journal state is
 *        intact - fault-induced corruption corrupted only the returned
 *        COPY, and the D-09 eviction branch (gated on
 *        fault_ != CorruptData) NEVER fired (RESEARCH Pitfall 3; the
 *        kit's non-sticky contract preserved by regression)
 */
TEST_F(LocalBlockStoreCrashTest, FaultCorruptionDoesNotEvict)
{
    auto store = MakeStore(NextUniqueDir());
    ASSERT_NE(store, nullptr);
    ASSERT_TRUE(store->Put(cid_a_, SpanOf(payload_a_)));
    store->SetFault(FaultKind::CorruptData);
    auto corrupted_view = store->Get(cid_a_);
    ASSERT_FALSE(corrupted_view);
    EXPECT_EQ(corrupted_view.error().code, StatusClass::Corrupt);
    EXPECT_EQ(corrupted_view.error().source, BlockSource::Local);

    store->SetFault(FaultKind::None);
    auto clean = store->Get(cid_a_);
    ASSERT_TRUE(clean); // block intact: no eviction fired on the fault path
    EXPECT_EQ(clean.value().bytes,
              std::vector<uint8_t>(payload_a_.begin(), payload_a_.end()));
    auto state = store->GetUploadState(cid_a_);
    ASSERT_TRUE(state); // journal intact too
    EXPECT_EQ(state.value(), UploadState::PendingUpload);
}

/**
 * @given the BeforeCommit crash point armed
 * @when Put runs
 * @then it fails TemporarilyUnavailable - the caller never saw success;
 *        the "process died" before the atomic DB::Write commit, so
 *        NOTHING was written - and after no-flush close and reopen, Get
 *        is NotFound, Has is false, and the pending set is EMPTY: the
 *        interrupted write left no torn block and no orphan journal
 *        record (SC 3 first half; D-07/D-08)
 */
TEST_F(LocalBlockStoreCrashTest, CrashBeforeCommitLeavesNothing)
{
    const std::string dir = NextUniqueDir();
    {
        auto store = MakeStore(dir);
        ASSERT_NE(store, nullptr);
        store->SetCrashPointForTesting(CrashPoint::BeforeCommit);
        auto put = store->Put(cid_a_, SpanOf(payload_a_));
        ASSERT_FALSE(put);
        EXPECT_EQ(put.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_EQ(put.error().source, BlockSource::Local);
        store->SetCrashPointForTesting(CrashPoint::None);
    } // destruction: no-flush close

    auto reopened = ReopenStore(dir); // WAL replay runs
    ASSERT_NE(reopened, nullptr);
    auto got = reopened->Get(cid_a_);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, StatusClass::NotFound); // never torn
    auto has = reopened->Has(cid_a_);
    ASSERT_TRUE(has);
    EXPECT_FALSE(has.value());
    auto pending = reopened->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty()); // journal reconciled: EMPTY
}

// ===================================================================
// Reconciliation suite (Plan 02-03 Task 2, SC 3 second half + SC 4)
// ===================================================================

/**
 * @given a committed Put (sync=true) followed by a no-flush close
 * @when the same directory reopens
 * @then BOTH the CID-verified block AND its PendingUpload journal
 *        record recover: the block and journal ride ONE WAL record, so
 *        they recover together or not at all - there is no intermediate
 *        state, and that absence IS SC 3's no-torn-block claim (D-08)
 */
TEST_F(LocalBlockStoreCrashTest, CrashAfterCommitRecoversBlockAndPending)
{
    const std::string dir = NextUniqueDir();
    {
        auto store = MakeStore(dir);
        ASSERT_NE(store, nullptr);
        ASSERT_TRUE(store->Put(cid_a_, SpanOf(payload_a_))); // sync commit
    } // destruction: no-flush close - the committed batch lives in the WAL

    auto reopened = ReopenStore(dir); // WAL replay runs
    ASSERT_NE(reopened, nullptr);
    auto got = reopened->Get(cid_a_);
    ASSERT_TRUE(got);
    EXPECT_EQ(got.value().bytes,
              std::vector<uint8_t>(payload_a_.begin(), payload_a_.end()));
    auto pending = reopened->GetPendingUploads();
    ASSERT_TRUE(pending);
    ASSERT_EQ(pending.value().size(), 1u);
    EXPECT_EQ(pending.value()[0], cid_a_);
}

/**
 * @given journal transitions recorded before a crash (cid_a durable,
 *        cid_b pending)
 * @when the store reopens
 * @then reconciliation restores the EXACT last-committed journal
 *        state: GetUploadState(cid_a) == UploadedDurable,
 *        GetPendingUploads() == {cid_b}, both blocks retrievable -
 *        D-05 synchronous enumeration at Open, no callback machinery
 *        (SC 4)
 */
TEST_F(LocalBlockStoreCrashTest, JournalTransitionsReconcileAcrossRestart)
{
    const std::string dir = NextUniqueDir();
    {
        auto store = MakeStore(dir);
        ASSERT_NE(store, nullptr);
        ASSERT_TRUE(store->Put(cid_a_, SpanOf(payload_a_)));
        ASSERT_TRUE(store->Put(cid_b_, SpanOf(payload_b_)));
        ASSERT_TRUE(store->MarkUploadedDurable(cid_a_));
    }
    auto reopened = ReopenStore(dir);
    ASSERT_NE(reopened, nullptr);
    auto state = reopened->GetUploadState(cid_a_);
    ASSERT_TRUE(state);
    EXPECT_EQ(state.value(), UploadState::UploadedDurable);
    auto pending = reopened->GetPendingUploads();
    ASSERT_TRUE(pending);
    ASSERT_EQ(pending.value().size(), 1u);
    EXPECT_EQ(pending.value()[0], cid_b_);
    EXPECT_TRUE(reopened->Get(cid_a_));
    EXPECT_TRUE(reopened->Get(cid_b_));
}

/**
 * @given the D-02 mapping documented in LocalBlockStore.hpp: no temp
 *        files exist by design, so ROADMAP Phase-2 criterion 4's
 *        "sweeps stale temp files" is VACUOUS - startup sweep IS
 *        journal reconciliation only
 * @when a new block commits across a restart cycle in a fresh directory
 * @then reconciliation needs no sweep step: after reopen the pending
 *        set is exactly the committed blocks, and a further Put+restart
 *        extends it exactly - no artifacts, no manual recovery state
 */
TEST_F(LocalBlockStoreCrashTest, VacuousSweepDocumentationHolds)
{
    const std::string dir = NextUniqueDir();
    const std::string payload_c = "crash suite payload c - reconciliation";
    const Cid cid_c = MakeCid(payload_c);
    {
        auto store = MakeStore(dir);
        ASSERT_NE(store, nullptr);
        ASSERT_TRUE(store->Put(cid_a_, SpanOf(payload_a_)));
    }
    {
        auto reopened = ReopenStore(dir);
        ASSERT_NE(reopened, nullptr);
        ASSERT_TRUE(reopened->Put(cid_c, SpanOf(payload_c)));
    }
    auto reopened2 = ReopenStore(dir);
    ASSERT_NE(reopened2, nullptr);
    auto pending = reopened2->GetPendingUploads();
    ASSERT_TRUE(pending);
    ASSERT_EQ(pending.value().size(), 2u); // exactly the committed set
    EXPECT_NE(std::find(pending.value().begin(), pending.value().end(), cid_a_),
              pending.value().end());
    EXPECT_NE(std::find(pending.value().begin(), pending.value().end(), cid_c),
              pending.value().end());
    EXPECT_TRUE(reopened2->Get(cid_a_));
    EXPECT_TRUE(reopened2->Get(cid_c));
}
