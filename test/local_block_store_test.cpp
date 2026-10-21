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
