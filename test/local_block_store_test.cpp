/**
 * @file local_block_store_test.cpp
 * @brief Behavior suite for the durable RocksDB cold-tier backend
 *        (BACK-03/BACK-04): the restart-durability tracer proof (SC 1)
 *        - close-without-flush + reopen replays the WAL and recovers
 *        BOTH the block bytes and the day-one PendingUpload journal
 *        record - plus the basic put/get/mismatch behaviors.
 *
 * Plain GTest discipline (D-11): EXPECT_TRUE on results + manual
 * .value()/.error() extraction, no outcome macros. NEVER .value() on a
 * possibly-failed envelope (policy::terminate would terminate the
 * binary): Open results are checked through MakeStore/ReopenStore which
 * ADD_FAILURE and return nullptr instead.
 *
 * NOTE: the BlockStoreConformanceTests kit instantiation lands with
 * Plan 02-02, mirroring memory_block_store_test.cpp's tail.
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

#include "BlockStoreConformanceTests.hpp"

namespace
{
    using sgns::blockstore::BlockSource;
    using sgns::blockstore::BlockStatus;
    using sgns::blockstore::Cid;
    using sgns::blockstore::CidOf;
    using sgns::blockstore::DurabilityLevel;
    using sgns::blockstore::FaultKind;
    using sgns::blockstore::LocalBlockStore;
    using sgns::blockstore::LocalBlockStoreOptions;
    using sgns::blockstore::StatusClass;
    using sgns::blockstore::UploadState;
    using libp2p::multi::HashType;
    using libp2p::multi::MulticodecType;
    using libp2p::multi::Multihash;

    gsl::span<const uint8_t> SpanOf(const std::string& s)
    {
        return gsl::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    Cid MakeCid(const std::string& payload)
    {
        return CidOf(SpanOf(payload), MulticodecType::Code::RAW).value();
    }

    Cid Blake2bCid()
    {
        // 32 arbitrary bytes as a blake2b-256 digest - a no-payload gate
        // probe after the Phase-6 widening (the pair is accepted; a fake
        // digest can never round-trip, so real vectors use CidOf below).
        static const uint8_t digest[32] = {
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
            0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
            0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
        };
        return Cid(Cid::Version::V1, MulticodecType::Code::RAW,
                   Multihash::create(HashType::blake2b_256,
                                     gsl::span<const uint8_t>(digest, 32))
                       .value());
    }

    Cid Sha512Cid()
    {
        // 64 arbitrary bytes as a sha-512 digest - the out-of-set
        // rejection exemplar after the Phase-6 widening (D-01/D-03).
        static const uint8_t digest[64] = {
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
            0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
            0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
            0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
            0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
        };
        return Cid(Cid::Version::V1, MulticodecType::Code::RAW,
                   Multihash::create(HashType::sha512,
                                     gsl::span<const uint8_t>(digest, 64))
                       .value());
    }

    // SHORT relative path under the build tree (Windows MAX_PATH, A3);
    // a monotonic counter gives every case its own directory (Pitfall 4).
    std::string NextUniqueDir()
    {
        static std::atomic<uint64_t> next{0};
        return "test-data/local-blockstore/case-"
             + std::to_string(next.fetch_add(1));
    }

    // Open WITHOUT touching the directory - the reopen path of the
    // restart-durability proof must see exactly what the previous
    // instance left behind.
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

    // Fresh case directory: stale dirs from prior runs must not leak
    // state into this one (remove_all first, A3/Pitfall 4).
    std::shared_ptr<LocalBlockStore> MakeStore(const std::string& dir)
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return ReopenStore(dir);
    }

    // NOTE: ContentIdentifier has no default constructor, so the CID
    // members carry member-initializers (fixture must stay default-
    // constructible for TEST_F).
    class LocalBlockStoreTest : public ::testing::Test
    {
    public:
        std::string dir_ = NextUniqueDir();
        std::shared_ptr<LocalBlockStore> store_ = MakeStore(dir_);
        std::string payload_a_ = "block a payload";
        std::string payload_b_ = "block b payload - different length";
        Cid cid_a_ = MakeCid(payload_a_);
        Cid cid_b_ = MakeCid(payload_b_);
    };
}

/**
 * @given an empty store and two payloads of different lengths
 * @when both are Put and read back
 * @then Put reports LocalCommitted with alreadyPresent == false, Get
 *        returns byte-identical bytes (CID-verified), Has reports true,
 *        and GetSize reports the exact byte counts
 */
TEST_F(LocalBlockStoreTest, PutGetRoundTrip)
{
    ASSERT_NE(store_, nullptr);
    auto put_a = store_->Put(cid_a_, SpanOf(payload_a_));
    ASSERT_TRUE(put_a);
    EXPECT_EQ(put_a.value().achieved, DurabilityLevel::LocalCommitted);
    EXPECT_FALSE(put_a.value().alreadyPresent);
    ASSERT_TRUE(store_->Put(cid_b_, SpanOf(payload_b_)));

    auto got_a = store_->Get(cid_a_);
    ASSERT_TRUE(got_a);
    EXPECT_EQ(got_a.value().bytes,
              std::vector<uint8_t>(payload_a_.begin(), payload_a_.end()));
    auto got_b = store_->Get(cid_b_);
    ASSERT_TRUE(got_b);
    EXPECT_EQ(got_b.value().bytes,
              std::vector<uint8_t>(payload_b_.begin(), payload_b_.end()));

    auto has_a = store_->Has(cid_a_);
    ASSERT_TRUE(has_a);
    EXPECT_TRUE(has_a.value());
    auto size_a = store_->GetSize(cid_a_);
    ASSERT_TRUE(size_a);
    EXPECT_EQ(size_a.value(), static_cast<uint64_t>(payload_a_.size()));
}

/**
 * @given an open store holding two committed blocks
 * @when the store is destroyed (close WITHOUT flush - the crash-faithful
 *        path: committed batches live only in the WAL) and the SAME
 *        directory is reopened
 * @then WAL replay recovers both blocks byte-identical and CID-verified,
 *        and GetPendingUploads returns exactly the two CIDs - the
 *        journal record was committed atomically with the block bytes
 *        and survives the restart (SC 1, BACK-03/BACK-04)
 */
TEST_F(LocalBlockStoreTest, RestartDurabilityPreservesBlocksAndJournal)
{
    const std::string dir = NextUniqueDir();
    {
        auto store = MakeStore(dir);
        ASSERT_NE(store, nullptr);
        ASSERT_TRUE(store->Put(cid_a_, SpanOf(payload_a_)));
        ASSERT_TRUE(store->Put(cid_b_, SpanOf(payload_b_)));
    } // destruction: CF handles destroyed -> Close() -> delete db_.
      // avoid_flush_during_shutdown=true: NO flush happens here.

    auto reopened = ReopenStore(dir); // RocksDB WAL replay runs now
    ASSERT_NE(reopened, nullptr);

    auto got_a = reopened->Get(cid_a_);
    ASSERT_TRUE(got_a);
    EXPECT_EQ(got_a.value().bytes,
              std::vector<uint8_t>(payload_a_.begin(), payload_a_.end()));
    auto got_b = reopened->Get(cid_b_);
    ASSERT_TRUE(got_b);
    EXPECT_EQ(got_b.value().bytes,
              std::vector<uint8_t>(payload_b_.begin(), payload_b_.end()));

    auto pending = reopened->GetPendingUploads();
    ASSERT_TRUE(pending);
    ASSERT_EQ(pending.value().size(), 2u);
    const auto& cids = pending.value();
    const bool has_a = std::find(cids.begin(), cids.end(), cid_a_) != cids.end();
    const bool has_b = std::find(cids.begin(), cids.end(), cid_b_) != cids.end();
    EXPECT_TRUE(has_a);
    EXPECT_TRUE(has_b);
}

/**
 * @given an empty store
 * @when Get fetches a missing CID
 * @then failure BlockStatus{NotFound, BlockSource::Local, message} -
 *        an authoritative miss only
 */
TEST_F(LocalBlockStoreTest, GetMissingIsNotFound)
{
    ASSERT_NE(store_, nullptr);
    auto res = store_->Get(cid_a_);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().code, StatusClass::NotFound);
    EXPECT_EQ(res.error().source, BlockSource::Local);
    EXPECT_FALSE(res.error().message.empty());
}

/**
 * @given a payload with one flipped byte versus its claimed CID
 * @when Put is called
 * @then failure BlockStatus{Corrupt, BlockSource::Caller, message} and
 *        nothing is stored - and no journal record leaked either, since
 *        block and journal commit in ONE batch or neither exists
 */
TEST_F(LocalBlockStoreTest, PutMismatchedBytesRejectedCorruptCaller)
{
    ASSERT_NE(store_, nullptr);
    auto corrupted = payload_a_;
    corrupted[0] = static_cast<char>(~corrupted[0]);
    auto res = store_->Put(cid_a_, SpanOf(corrupted));
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().code, StatusClass::Corrupt);
    EXPECT_EQ(res.error().source, BlockSource::Caller);
    EXPECT_FALSE(res.error().message.empty());
    auto has = store_->Has(cid_a_);
    ASSERT_TRUE(has);
    EXPECT_FALSE(has.value()); // nothing stored
    auto pending = store_->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty()); // no orphan journal record
}

/**
 * @given fault Unavailable injected
 * @when Put and Get run
 * @then both FAIL TemporarilyUnavailable with source Local - the fault
 *        gate returns BEFORE RocksDB is touched (the gate precedes every
 *        db_ access, so no directory content change is needed to prove
 *        the ordering) - and after SetFault(None) the identical calls
 *        succeed (no sticky negative cache)
 */
TEST_F(LocalBlockStoreTest, FaultUnavailableSurfacesTemporarilyUnavailableBeforeStorage)
{
    ASSERT_NE(store_, nullptr);
    store_->SetFault(FaultKind::Unavailable);
    auto put = store_->Put(cid_a_, SpanOf(payload_a_));
    ASSERT_FALSE(put);
    EXPECT_EQ(put.error().code, StatusClass::TemporarilyUnavailable);
    EXPECT_EQ(put.error().source, BlockSource::Local);
    auto got = store_->Get(cid_a_);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
    EXPECT_EQ(got.error().source, BlockSource::Local);

    store_->SetFault(FaultKind::None);
    EXPECT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_))); // same CID succeeds
    EXPECT_TRUE(store_->Get(cid_a_));
}

/**
 * @given fault Unauthorized injected
 * @when Put and Get run
 * @then both FAIL Unauthorized with source Local - never NotFound,
 *        never TemporarilyUnavailable (the taxonomy's fourth class is
 *        distinct) - and after SetFault(None) the identical calls
 *        succeed (non-sticky)
 */
TEST_F(LocalBlockStoreTest, FaultUnauthorizedSurfacesUnauthorized)
{
    ASSERT_NE(store_, nullptr);
    store_->SetFault(FaultKind::Unauthorized);
    auto put = store_->Put(cid_a_, SpanOf(payload_a_));
    ASSERT_FALSE(put);
    EXPECT_EQ(put.error().code, StatusClass::Unauthorized);
    EXPECT_EQ(put.error().source, BlockSource::Local);
    auto got = store_->Get(cid_a_);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, StatusClass::Unauthorized);
    EXPECT_EQ(got.error().source, BlockSource::Local);
    EXPECT_NE(got.error().code, StatusClass::NotFound);
    EXPECT_NE(got.error().code, StatusClass::TemporarilyUnavailable);

    store_->SetFault(FaultKind::None);
    EXPECT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    EXPECT_TRUE(store_->Get(cid_a_));
}

/**
 * @given a stored block and fault CorruptData injected
 * @when Get fetches it, then the fault is cleared and Get runs again
 * @then the first Get FAILS Corrupt with source Local (the seam flipped
 *        a byte on the returned COPY only), and the second Get SUCCEEDS
 *        with the ORIGINAL bytes - the stored bytes were never touched
 *        and NOTHING was evicted (non-sticky, exactly the kit case that
 *        would break if eviction fired on the fault path)
 */
TEST_F(LocalBlockStoreTest, FaultCorruptDataCorruptsReturnedCopyNotStorage)
{
    ASSERT_NE(store_, nullptr);
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    store_->SetFault(FaultKind::CorruptData);
    auto res = store_->Get(cid_a_);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().code, StatusClass::Corrupt);
    EXPECT_EQ(res.error().source, BlockSource::Local);
    EXPECT_FALSE(res.error().message.empty());

    store_->SetFault(FaultKind::None);
    auto clean = store_->Get(cid_a_);
    ASSERT_TRUE(clean); // cleared fault -> intact: no eviction ever fired
    EXPECT_EQ(clean.value().bytes,
              std::vector<uint8_t>(payload_a_.begin(), payload_a_.end()));
    auto pending = store_->GetPendingUploads();
    ASSERT_TRUE(pending);
    ASSERT_EQ(pending.value().size(), 1u); // journal record untouched too
}

// ===================================================================
// Journal lifecycle (Plan 02-02): explicit transitions, state reader,
// restart persistence, re-Put no-regression (D-03/D-06, BACK-04).
// ===================================================================

/**
 * @given a block Put into the store
 * @when its journal state is read
 * @then GetUploadState == PendingUpload (the day-one birth state from
 *        the 02-01 one-batch commit) and GetPendingUploads lists exactly
 *        that CID
 */
TEST_F(LocalBlockStoreTest, JournalBirthStateIsPendingUpload)
{
    ASSERT_NE(store_, nullptr);
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    auto state = store_->GetUploadState(cid_a_);
    ASSERT_TRUE(state);
    EXPECT_EQ(state.value(), UploadState::PendingUpload);
    auto pending = store_->GetPendingUploads();
    ASSERT_TRUE(pending);
    ASSERT_EQ(pending.value().size(), 1u);
    EXPECT_EQ(pending.value()[0], cid_a_);
}

/**
 * @given a pending block
 * @when MarkUploadedAccepted transitions it
 * @then GetUploadState == UploadedAccepted, the pending set is EMPTY
 *        (accepted is not pending), and Get still returns the block
 *        byte-identical - a journal move never touches block bytes
 */
TEST_F(LocalBlockStoreTest, MarkUploadedAcceptedMovesStateAndShrinksPending)
{
    ASSERT_NE(store_, nullptr);
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    ASSERT_TRUE(store_->MarkUploadedAccepted(cid_a_));
    auto state = store_->GetUploadState(cid_a_);
    ASSERT_TRUE(state);
    EXPECT_EQ(state.value(), UploadState::UploadedAccepted);
    auto pending = store_->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty());
    auto got = store_->Get(cid_a_);
    ASSERT_TRUE(got);
    EXPECT_EQ(got.value().bytes,
              std::vector<uint8_t>(payload_a_.begin(), payload_a_.end()));
}

/**
 * @given an accepted block
 * @when MarkUploadedDurable transitions it
 * @then GetUploadState == UploadedDurable, the pending set is still
 *        empty, and the block remains retrievable byte-identical
 */
TEST_F(LocalBlockStoreTest, MarkUploadedDurableMovesStateBlockRetrievable)
{
    ASSERT_NE(store_, nullptr);
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    ASSERT_TRUE(store_->MarkUploadedAccepted(cid_a_));
    ASSERT_TRUE(store_->MarkUploadedDurable(cid_a_));
    auto state = store_->GetUploadState(cid_a_);
    ASSERT_TRUE(state);
    EXPECT_EQ(state.value(), UploadState::UploadedDurable);
    auto pending = store_->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty());
    EXPECT_TRUE(store_->Get(cid_a_));
}

/**
 * @given a store with cid_a marked UploadedDurable and cid_b pending
 * @when the store is destroyed (no-flush close) and the same directory
 *        reopened
 * @then WAL replay recovers the journal: GetUploadState(cid_a) is still
 *        UploadedDurable, GetPendingUploads() == {cid_b} exactly, and
 *        both blocks Get byte-identical (BACK-04: journal durable +
 *        recoverable, restart reconciliation separates the sets)
 */
TEST_F(LocalBlockStoreTest, JournalStateSurvivesRestart)
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
 * @given a CID that was never Put (no journal record exists)
 * @when MarkUploadedDurable or GetUploadState addresses it
 * @then both return a NotFound failure with source Local - transitions
 *        move Put-born records only and NEVER fabricate upload state
 *        (T-02-08), and the state reader reports absence rather than a
 *        default state value
 */
TEST_F(LocalBlockStoreTest, TransitionOnUnknownCidIsNotFound)
{
    ASSERT_NE(store_, nullptr);
    auto marked = store_->MarkUploadedDurable(cid_a_);
    ASSERT_FALSE(marked);
    EXPECT_EQ(marked.error().code, StatusClass::NotFound);
    EXPECT_EQ(marked.error().source, BlockSource::Local);
    auto state = store_->GetUploadState(cid_a_);
    ASSERT_FALSE(state);
    EXPECT_EQ(state.error().code, StatusClass::NotFound);
    EXPECT_EQ(state.error().source, BlockSource::Local);
    // no record was invented by the failed transition
    auto pending = store_->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty());
}

/**
 * @given a block whose journal record is UploadedDurable
 * @when the identical (cid, bytes) pair is Put again
 * @then the re-Put succeeds with alreadyPresent == true and the journal
 *        state is STILL UploadedDurable with an empty pending set - the
 *        conditional journal write means a re-Put never regresses upload
 *        state (Pitfall 6; redundant-re-upload hazard for Phase 4)
 */
TEST_F(LocalBlockStoreTest, RePutDoesNotRegressJournalState)
{
    ASSERT_NE(store_, nullptr);
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    ASSERT_TRUE(store_->MarkUploadedDurable(cid_a_));
    auto re_put = store_->Put(cid_a_, SpanOf(payload_a_));
    ASSERT_TRUE(re_put);
    EXPECT_TRUE(re_put.value().alreadyPresent);
    auto state = store_->GetUploadState(cid_a_);
    ASSERT_TRUE(state);
    EXPECT_EQ(state.value(), UploadState::UploadedDurable);
    auto pending = store_->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty());
}

/**
 * @given the journal transition surface
 * @when a non-sha256 CID crosses it, or the Unavailable fault is active
 * @then the non-sha256 CID THROWS std::invalid_argument before any
 *        storage access (D-08), and under the fault MarkUploadedDurable
 *        fails TemporarilyUnavailable with source Local BEFORE RocksDB
 *        is touched - then clears non-stickily
 */
TEST_F(LocalBlockStoreTest, TransitionsRespectEntryGates)
{
    ASSERT_NE(store_, nullptr);
    // Phase-6 widening: journal tooling rides the widened store gate -
    // the out-of-set exemplar (sha-512) still throws, while blake2b-256
    // (the CRDT shape) is accepted end-to-end (D-01).
    EXPECT_THROW((void)store_->MarkUploadedDurable(Sha512Cid()),
                 std::invalid_argument);
    EXPECT_THROW((void)store_->GetUploadState(Sha512Cid()),
                 std::invalid_argument);
    const std::string cbor_payload("\xa0", 1);
    const Cid blake2b_cid =
        CidOf(SpanOf(cbor_payload), MulticodecType::Code::DAG_CBOR,
              HashType::blake2b_256).value();
    ASSERT_TRUE(store_->Put(blake2b_cid, SpanOf(cbor_payload)));
    ASSERT_TRUE(store_->MarkUploadedDurable(blake2b_cid));
    auto upload_state = store_->GetUploadState(blake2b_cid);
    ASSERT_TRUE(upload_state);

    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    store_->SetFault(FaultKind::Unavailable);
    auto marked = store_->MarkUploadedDurable(cid_a_);
    ASSERT_FALSE(marked);
    EXPECT_EQ(marked.error().code, StatusClass::TemporarilyUnavailable);
    EXPECT_EQ(marked.error().source, BlockSource::Local);
    store_->SetFault(FaultKind::None);
    ASSERT_TRUE(store_->MarkUploadedDurable(cid_a_)); // clears non-stickily
}

// ===================================================================
// EvictLocal (Plan 02-02, D-10): unconditional, idempotent, caller-owned
// local removal of block bytes + journal record.
// ===================================================================

/**
 * @given a pending block
 * @when EvictLocal removes it
 * @then Has is false (success value), Get is NotFound, GetUploadState is
 *        NotFound (journal co-removed), and the pending set is empty -
 *        ONE batch removed both CF entries
 */
TEST_F(LocalBlockStoreTest, EvictLocalRemovesBlockAndJournalRecord)
{
    ASSERT_NE(store_, nullptr);
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    ASSERT_TRUE(store_->EvictLocal(cid_a_));
    auto has = store_->Has(cid_a_);
    ASSERT_TRUE(has);
    EXPECT_FALSE(has.value());
    auto got = store_->Get(cid_a_);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, StatusClass::NotFound);
    auto state = store_->GetUploadState(cid_a_);
    ASSERT_FALSE(state);
    EXPECT_EQ(state.error().code, StatusClass::NotFound);
    auto pending = store_->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty());
}

/**
 * @given a CID already evicted (no local bytes remain)
 * @when EvictLocal runs again
 * @then it SUCCEEDS - idempotent: the desired post-state already holds
 */
TEST_F(LocalBlockStoreTest, EvictLocalIsIdempotent)
{
    ASSERT_NE(store_, nullptr);
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    ASSERT_TRUE(store_->EvictLocal(cid_a_));
    EXPECT_TRUE(store_->EvictLocal(cid_a_)); // idempotent
}

/**
 * @given a block marked UploadedDurable
 * @when EvictLocal removes it
 * @then BOTH the block bytes and the journal record are gone regardless
 *        of journal state - eviction is unconditional (D-10): the
 *        caller owns eviction policy, the method never consults
 *        durability
 */
TEST_F(LocalBlockStoreTest, EvictLocalIgnoresJournalState)
{
    ASSERT_NE(store_, nullptr);
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    ASSERT_TRUE(store_->MarkUploadedDurable(cid_a_));
    ASSERT_TRUE(store_->EvictLocal(cid_a_));
    auto got = store_->Get(cid_a_);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, StatusClass::NotFound);
    auto state = store_->GetUploadState(cid_a_);
    ASSERT_FALSE(state);
    EXPECT_EQ(state.error().code, StatusClass::NotFound);
}

/**
 * @given an evicted CID
 * @when the same block is Put again
 * @then the re-Put succeeds with alreadyPresent == false - eviction
 *        genuinely cleared both CF entries, and the journal record is
 *        re-born as PendingUpload in the same batch
 */
TEST_F(LocalBlockStoreTest, RePutAfterEvictRepairs)
{
    ASSERT_NE(store_, nullptr);
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    ASSERT_TRUE(store_->MarkUploadedDurable(cid_a_));
    ASSERT_TRUE(store_->EvictLocal(cid_a_));
    auto re_put = store_->Put(cid_a_, SpanOf(payload_a_));
    ASSERT_TRUE(re_put);
    EXPECT_FALSE(re_put.value().alreadyPresent);
    auto state = store_->GetUploadState(cid_a_);
    ASSERT_TRUE(state);
    EXPECT_EQ(state.value(), UploadState::PendingUpload); // re-born
    auto got = store_->Get(cid_a_);
    ASSERT_TRUE(got);
}

// Phase-1 conformance kit instantiation (SC 2, BACK-02/D-09): the
// factory runs PER CASE, so every case gets a FRESH unique directory
// (Pitfall 4 - LOCK contention and cross-case leakage are impossible).
// Open failure is ADD_FAILURE + nullptr, NEVER .value() on the
// terminate-policy envelope.
std::shared_ptr<sgns::blockstore::BlockStore> MakeFreshLocalStore()
{
    static std::atomic<uint64_t> next{0};
    const auto dir = "test-data/local-blockstore/kit-case-"
                   + std::to_string(next.fetch_add(1));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    auto opened = sgns::blockstore::LocalBlockStore::Open(
        sgns::blockstore::LocalBlockStoreOptions{dir});
    if (!opened)
    {
        ADD_FAILURE() << "LocalBlockStore::Open failed for " << dir
                      << ": " << opened.error().message;
        return nullptr;
    }
    return std::shared_ptr<sgns::blockstore::LocalBlockStore>(
        std::move(opened.value()));
}

SGNS_BLOCKSTORE_CONFORMANCE_TESTS(LocalBlockStoreConformance,
                                  MakeFreshLocalStore)

//
// DUR-01 (v1.1 Phase 10 plan 10-01 Task 2): state-filtered enumeration
// and the batched durable-mark composition surface.
//

/// @brief Encoded-CID bytes as the ordering key (byte-lexicographic
///        contract of the enumeration).
std::vector<uint8_t> Encoded(const Cid& cid)
{
    auto encoded =
        libp2p::multi::ContentIdentifierCodec::encode(cid);
    return encoded.value();
}

/**
 * @given a journal holding records in all three upload states
 * @when  GetUploadsInState enumerates each state
 * @then  each call returns EXACTLY that state's CIDs in encoded-byte
 *        lexicographic order (the frozen ordering contract - the D-03
 *        snapshot the flush-gated advancement marks against).
 */
TEST(LocalBlockStoreJournal, GetUploadsInStateFiltersAndSorts)
{
    auto store = MakeStore("test-data/local-blockstore/state-enum-case");
    ASSERT_NE(store, nullptr);

    std::vector<Cid> pending;
    std::vector<Cid> accepted;
    std::vector<Cid> durable;
    for (int i = 0; i < 6; ++i)
    {
        const auto payload = "state-enum payload " + std::to_string(i);
        Cid cid = MakeCid(payload);
        ASSERT_TRUE(store->Put(cid, SpanOf(payload)).has_value());
        switch (i % 3)
        {
            case 0:
                pending.push_back(cid);
                break;
            case 1:
                ASSERT_TRUE(store->MarkUploadedAccepted(cid).has_value());
                accepted.push_back(cid);
                break;
            default:
                ASSERT_TRUE(store->MarkUploadedAccepted(cid).has_value());
                ASSERT_TRUE(store->MarkUploadedDurable(cid).has_value());
                durable.push_back(cid);
                break;
        }
    }

    auto sort_by_encoded = [](std::vector<Cid> cids)
    {
        std::sort(cids.begin(), cids.end(), [](const Cid& a, const Cid& b)
                  { return Encoded(a) < Encoded(b); });
        return cids;
    };

    auto in_pending = store->GetUploadsInState(UploadState::PendingUpload);
    ASSERT_TRUE(in_pending.has_value());
    auto expected_pending = sort_by_encoded(pending);
    ASSERT_EQ(in_pending.value().size(), expected_pending.size());
    EXPECT_EQ(in_pending.value(), expected_pending);

    auto in_accepted = store->GetUploadsInState(UploadState::UploadedAccepted);
    ASSERT_TRUE(in_accepted.has_value());
    auto expected_accepted = sort_by_encoded(accepted);
    ASSERT_EQ(in_accepted.value().size(), expected_accepted.size());
    EXPECT_EQ(in_accepted.value(), expected_accepted);

    auto in_durable = store->GetUploadsInState(UploadState::UploadedDurable);
    ASSERT_TRUE(in_durable.has_value());
    auto expected_durable = sort_by_encoded(durable);
    ASSERT_EQ(in_durable.value().size(), expected_durable.size());
    EXPECT_EQ(in_durable.value(), expected_durable);
}

/**
 * @given Accepted journal records plus one CID with NO record at all
 * @when  MarkUploadedDurableMany runs over the mixed span
 * @then  every existing record is Durable after the ONE call, the
 *        no-record CID is skipped-and-continued (per-item isolation -
 *        upload state is never fabricated, the batch never aborts), and
 *        an empty span is a success no-op. Already-Durable records stay
 *        Durable (idempotent replay is a no-op advancement).
 */
TEST(LocalBlockStoreJournal, MarkUploadedDurableManySkipsAndContinues)
{
    auto store = MakeStore("test-data/local-blockstore/batch-mark-case");
    ASSERT_NE(store, nullptr);

    std::vector<Cid> accepted;
    for (int i = 0; i < 4; ++i)
    {
        const auto payload = "batch-mark payload " + std::to_string(i);
        Cid cid = MakeCid(payload);
        ASSERT_TRUE(store->Put(cid, SpanOf(payload)).has_value());
        ASSERT_TRUE(store->MarkUploadedAccepted(cid).has_value());
        accepted.push_back(cid);
    }
    const Cid no_record = MakeCid("never journaled payload");

    // Empty span: success no-op.
    ASSERT_TRUE(store
                    ->MarkUploadedDurableMany(
                        gsl::span<const Cid>{})
                    .has_value());

    // Mixed span: existing records marked, no-record CID skipped.
    std::vector<Cid> mixed = accepted;
    mixed.insert(mixed.begin() + 2, no_record);
    ASSERT_TRUE(store
                    ->MarkUploadedDurableMany(
                        gsl::span<const Cid>(mixed.data(), mixed.size()))
                    .has_value());

    for (const auto& cid : accepted)
    {
        auto state = store->GetUploadState(cid);
        ASSERT_TRUE(state.has_value());
        EXPECT_EQ(state.value(), UploadState::UploadedDurable);
    }
    auto no_record_state = store->GetUploadState(no_record);
    ASSERT_FALSE(no_record_state.has_value()); // still no record - not fabricated

    // Idempotent replay over Durable records stays Durable.
    ASSERT_TRUE(store
                    ->MarkUploadedDurableMany(
                        gsl::span<const Cid>(accepted.data(), accepted.size()))
                    .has_value());
    for (const auto& cid : accepted)
    {
        auto state = store->GetUploadState(cid);
        ASSERT_TRUE(state.has_value());
        EXPECT_EQ(state.value(), UploadState::UploadedDurable);
    }
}

/**
 * @given a payload and its CIDv0 dag-pb/sha-256 claim (the shape every
 *        CRDT delta node carries - IPLDNodeImpl blocks are V0 by
 *        construction) while CidOf recomputes the CIDv1 form
 * @when  Put and verified Get run against the V0 claim
 * @then  both ACCEPT: CIDv0 and CIDv1 with the same codec, hash type,
 *        and multihash are the same content address (the IPLD spec
 *        equivalence, surfaced by Phase 11's tracer - a strict
 *        operator== rejected every real delta block at the decorator
 *        seam); wrong bytes under a V0 claim still fail Corrupt.
 */
TEST(LocalBlockStoreJournal, CidV0DagPbClaimAcceptedAndStillVerifies)
{
    auto store = MakeStore("test-data/local-blockstore/v0-equivalence-case");
    ASSERT_NE(store, nullptr);

    const std::string payload = "cidv0 dag-pb equivalence payload";
    auto computed = CidOf(SpanOf(payload), MulticodecType::Code::DAG_PB);
    ASSERT_TRUE(computed.has_value());
    const Cid claimed_v0(Cid::Version::V0, MulticodecType::Code::DAG_PB,
                         computed.value().content_address);

    // The equivalence claim: V0 in, bytes hashed to the V1 twin.
    auto put = store->Put(claimed_v0, SpanOf(payload));
    ASSERT_TRUE(put.has_value()) << "the V0 dag-pb claim must be accepted";

    // Verified read-back through the same equivalence.
    sgns::blockstore::ReadOptions verify_read;
    verify_read.verify = true;
    auto got = store->Get(claimed_v0, verify_read);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got.value().bytes,
              std::vector<uint8_t>(payload.begin(), payload.end()));

    // The security property is intact: wrong bytes under the V0 claim
    // still fail Corrupt (the digest mismatch stands).
    auto bad = store->Put(claimed_v0, SpanOf("cidv0 wrong payload"));
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, StatusClass::Corrupt);
}
