/**
 * @file memory_block_store_test.cpp
 * @brief Behavior suite for the MemoryBlockStore reference backend
 *        (BACK-01) - the executable proof of the frozen taxonomy
 *        (CORE-02), facade verification with source attribution
 *        (CORE-04), the D-08 programmer-error gate, EvictLocal placement
 *        (D-16), and PutMany ordering/no-rollback (D-14).
 *
 * Plain GTest discipline (D-11): EXPECT_TRUE on results + manual
 * .value()/.error() extraction, no outcome macros.
 *
 * NOTE (Plan-03 marker): the BlockStoreConformanceTests kit instantiation
 * // SGNS_BLOCKSTORE_CONFORMANCE_TESTS(MemoryBlockStoreTest, ...) lands
 * // here with the conformance-kit plan.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <sgns-blockstore/Cid.hpp>
#include <sgns-blockstore/FaultController.hpp>
#include <sgns-blockstore/MemoryBlockStore.hpp>

#include <libp2p/multi/multihash.hpp>

#include "BlockStoreConformanceTests.hpp"

namespace
{
    using sgns::blockstore::Block;
    using sgns::blockstore::BlockInput;
    using sgns::blockstore::BlockSource;
    using sgns::blockstore::BlockStatus;
    using sgns::blockstore::Cid;
    using sgns::blockstore::CidOf;
    using sgns::blockstore::FaultController;
    using sgns::blockstore::FaultKind;
    using sgns::blockstore::MemoryBlockStore;
    using sgns::blockstore::PutOutcome;
    using sgns::blockstore::ReadOptions;
    using sgns::blockstore::StatusClass;
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

    // NOTE: ContentIdentifier has no default constructor, so the CID
    // members carry member-initializers (fixture must stay default-
    // constructible for TEST_F).
    class MemoryBlockStoreTest : public ::testing::Test
    {
    public:
        std::shared_ptr<MemoryBlockStore> store_{std::make_shared<MemoryBlockStore>()};
        std::string payload_a_ = "block a payload";
        std::string payload_b_ = "block b payload - different length";
        Cid cid_a_ = MakeCid(payload_a_);
        Cid cid_b_ = MakeCid(payload_b_);
    };

    Cid Blake2bCid()
    {
        // 32 arbitrary bytes as a blake2b-256 digest - the CRDT CBOR
        // boundary shape that v1 must reject at API entry (D-08).
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
}

/**
 * @given an empty store and a payload whose CID is computed via CidOf
 * @when Put stores it
 * @then success with PutOutcome{LocalCommitted, alreadyPresent == false}
 */
TEST_F(MemoryBlockStoreTest, PutSucceedsWithLocalCommitted)
{
    auto res = store_->Put(cid_a_, SpanOf(payload_a_));
    ASSERT_TRUE(res);
    EXPECT_EQ(res.value().achieved, sgns::blockstore::DurabilityLevel::LocalCommitted);
    EXPECT_FALSE(res.value().alreadyPresent);
}

/**
 * @given a store already holding (cid_a, payload_a)
 * @when the identical (cid, bytes) pair is Put again
 * @then success with alreadyPresent == true and Get still returns the
 *        original bytes (identical re-put merges, never duplicates)
 */
TEST_F(MemoryBlockStoreTest, IdenticalRePutMerges)
{
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    auto res = store_->Put(cid_a_, SpanOf(payload_a_));
    ASSERT_TRUE(res);
    EXPECT_TRUE(res.value().alreadyPresent);
    auto got = store_->Get(cid_a_);
    ASSERT_TRUE(got);
    EXPECT_EQ(got.value().bytes,
              std::vector<uint8_t>(payload_a_.begin(), payload_a_.end()));
}

/**
 * @given a payload with one flipped byte versus its claimed CID
 * @when Put is called
 * @then failure BlockStatus{Corrupt, BlockSource::Caller, message} and
 *        nothing is stored
 */
TEST_F(MemoryBlockStoreTest, PutMismatchedBytesIsCorruptFromCaller)
{
    auto corrupted = payload_a_;
    corrupted[0] = static_cast<char>(~corrupted[0]);
    auto res = store_->Put(cid_a_, SpanOf(corrupted));
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().code, StatusClass::Corrupt);
    EXPECT_EQ(res.error().source, BlockSource::Caller);
    EXPECT_FALSE(res.error().message.empty());
    EXPECT_FALSE(store_->Has(cid_a_).value()); // nothing stored
}

/**
 * @given an empty store
 * @when Get fetches a missing CID
 * @then failure BlockStatus{NotFound, BlockSource::Memory, message}
 */
TEST_F(MemoryBlockStoreTest, GetMissingIsNotFoundFailure)
{
    auto res = store_->Get(cid_a_);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().code, StatusClass::NotFound);
    EXPECT_EQ(res.error().source, BlockSource::Memory);
    EXPECT_FALSE(res.error().message.empty());
}

/**
 * @given a store holding block A but not block B
 * @when Has checks both
 * @then success true for A and success false for B (false is a VALUE,
 *        not a failure)
 */
TEST_F(MemoryBlockStoreTest, HasReportsPresenceAsValues)
{
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    auto has_a = store_->Has(cid_a_);
    ASSERT_TRUE(has_a);
    EXPECT_TRUE(has_a.value());
    auto has_b = store_->Has(cid_b_);
    ASSERT_TRUE(has_b);
    EXPECT_FALSE(has_b.value());
}

/**
 * @given a store holding a block
 * @when GetSize measures it
 * @then success with the exact stored byte count; an empty payload also
 *        stores and reports size 0
 */
TEST_F(MemoryBlockStoreTest, GetSizeReportsExactByteCount)
{
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    auto size = store_->GetSize(cid_a_);
    ASSERT_TRUE(size);
    EXPECT_EQ(size.value(), static_cast<uint64_t>(payload_a_.size()));

    const Cid empty_cid = CidOf(gsl::span<const uint8_t>(), MulticodecType::Code::RAW).value();
    ASSERT_TRUE(store_->Put(empty_cid, gsl::span<const uint8_t>()));
    auto empty_size = store_->GetSize(empty_cid);
    ASSERT_TRUE(empty_size);
    EXPECT_EQ(empty_size.value(), 0u);
}

/**
 * @given a blake2b-256 CID (the CRDT CBOR boundary shape)
 * @when Put, Get, or PutMany is called with it
 * @then std::invalid_argument is THROWN (D-08 programmer error, before
 *        any write) and nothing is stored
 */
TEST_F(MemoryBlockStoreTest, Blake2bCidThrowsInvalidArgument)
{
    const Cid bad = Blake2bCid();
    EXPECT_THROW((void)store_->Put(bad, SpanOf(payload_a_)), std::invalid_argument);
    EXPECT_THROW((void)store_->Get(bad), std::invalid_argument);
    EXPECT_THROW((void)store_->Has(bad), std::invalid_argument);
    const BlockInput batch[] = {{bad, SpanOf(payload_a_)}};
    EXPECT_THROW((void)store_->PutMany(batch), std::invalid_argument);
}

/**
 * @given fault Unavailable injected
 * @when Put/Get/Has/GetSize run
 * @then every op FAILS TemporarilyUnavailable - Has NEVER returns the
 *        value false - and after SetFault(None) the identical subsequent
 *        calls succeed (no sticky negative cache)
 */
TEST_F(MemoryBlockStoreTest, UnavailableFaultIsTemporaryFailureNotSticky)
{
    store_->SetFault(FaultKind::Unavailable);
    auto put = store_->Put(cid_a_, SpanOf(payload_a_));
    ASSERT_FALSE(put);
    EXPECT_EQ(put.error().code, StatusClass::TemporarilyUnavailable);
    auto got = store_->Get(cid_a_);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
    auto has = store_->Has(cid_a_);
    ASSERT_FALSE(has); // failure, NOT the value false
    EXPECT_EQ(has.error().code, StatusClass::TemporarilyUnavailable);
    auto size = store_->GetSize(cid_a_);
    ASSERT_FALSE(size);
    EXPECT_EQ(size.error().code, StatusClass::TemporarilyUnavailable);

    store_->SetFault(FaultKind::None);
    EXPECT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_))); // same CID succeeds
    auto got_after = store_->Get(cid_a_);
    ASSERT_TRUE(got_after);
    EXPECT_TRUE(store_->Has(cid_a_).value());
}

/**
 * @given fault CorruptData injected on a store holding a valid block
 * @when Get fetches it
 * @then the backend-corrupted bytes fail re-verification and the failure
 *        is BlockStatus{Corrupt, BlockSource::Memory, message}
 */
TEST_F(MemoryBlockStoreTest, CorruptDataFaultIsCorruptFromMemory)
{
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    store_->SetFault(FaultKind::CorruptData);
    auto res = store_->Get(cid_a_);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().code, StatusClass::Corrupt);
    EXPECT_EQ(res.error().source, BlockSource::Memory);
    EXPECT_FALSE(res.error().message.empty());
    store_->SetFault(FaultKind::None);
    EXPECT_TRUE(store_->Get(cid_a_)); // cleared -> intact
}

/**
 * @given fault Unauthorized injected
 * @when Put/Get/Has/GetSize run
 * @then every op FAILS Unauthorized - never NotFound, never
 *        TemporarilyUnavailable - and after SetFault(None) the identical
 *        subsequent calls succeed (auth-failure class distinct and
 *        non-sticky)
 */
TEST_F(MemoryBlockStoreTest, UnauthorizedFaultIsDistinctAndNotSticky)
{
    store_->SetFault(FaultKind::Unauthorized);
    auto put = store_->Put(cid_a_, SpanOf(payload_a_));
    ASSERT_FALSE(put);
    EXPECT_EQ(put.error().code, StatusClass::Unauthorized);
    auto got = store_->Get(cid_a_);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, StatusClass::Unauthorized);
    auto has = store_->Has(cid_a_);
    ASSERT_FALSE(has);
    EXPECT_EQ(has.error().code, StatusClass::Unauthorized);
    EXPECT_NE(has.error().code, StatusClass::NotFound);
    EXPECT_NE(has.error().code, StatusClass::TemporarilyUnavailable);
    auto size = store_->GetSize(cid_a_);
    ASSERT_FALSE(size);
    EXPECT_EQ(size.error().code, StatusClass::Unauthorized);

    store_->SetFault(FaultKind::None);
    EXPECT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    EXPECT_TRUE(store_->Get(cid_a_));
}

/**
 * @given a store holding a block
 * @when EvictLocal runs twice
 * @then both calls succeed (idempotent - the desired post-state of no
 *        local bytes holds either way) and Get reports NotFound between
 *        them; EvictLocal is a concrete backend method, never on
 *        BlockStore (D-16)
 */
TEST_F(MemoryBlockStoreTest, EvictLocalIsIdempotentBackendOnly)
{
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    EXPECT_TRUE(store_->EvictLocal(cid_a_));
    auto gone = store_->Get(cid_a_);
    ASSERT_FALSE(gone);
    EXPECT_EQ(gone.error().code, StatusClass::NotFound);
    EXPECT_TRUE(store_->EvictLocal(cid_a_)); // idempotent
}

/**
 * @given a batch [good, mismatched, good]
 * @when PutMany stores it
 * @then outer success, vector size 3, results[1] is Corrupt+Caller while
 *        results[0]/results[2] succeeded and remain retrievable (no
 *        rollback, order preserved - D-14)
 */
TEST_F(MemoryBlockStoreTest, PutManyPreservesOrderAndNeverRollsBack)
{
    auto corrupted = payload_a_;
    corrupted[1] = static_cast<char>(~corrupted[1]);
    const BlockInput batch[] = {
        {cid_a_, SpanOf(payload_a_)},
        {cid_a_, SpanOf(corrupted)},
        {cid_b_, SpanOf(payload_b_)},
    };
    auto res = store_->PutMany(batch);
    ASSERT_TRUE(res);
    ASSERT_EQ(res.value().size(), 3u);
    EXPECT_TRUE(res.value()[0]);
    ASSERT_FALSE(res.value()[1]);
    EXPECT_EQ(res.value()[1].error().code, StatusClass::Corrupt);
    EXPECT_EQ(res.value()[1].error().source, BlockSource::Caller);
    EXPECT_TRUE(res.value()[2]);
    // siblings remain retrievable - no rollback
    EXPECT_TRUE(store_->Get(cid_a_));
    EXPECT_TRUE(store_->Get(cid_b_));
}

/**
 * @given a PutMany batch containing a blake2b CID next to a valid entry
 * @when PutMany is called
 * @then std::invalid_argument is THROWN and the batch's VALID entries are
 *        NOT stored afterwards (pre-validation before any write)
 */
TEST_F(MemoryBlockStoreTest, PutManyBlake2bThrowsAndWritesNothing)
{
    const BlockInput batch[] = {
        {cid_a_, SpanOf(payload_a_)},
        {Blake2bCid(), SpanOf(payload_b_)},
    };
    EXPECT_THROW((void)store_->PutMany(batch), std::invalid_argument);
    EXPECT_FALSE(store_->Has(cid_a_).value()); // nothing written
}

/**
 * @given a store holding a block and a ReadOptions whose deadline
 *        expired an hour ago
 * @when Get runs with those options
 * @then it still succeeds - MemoryBlockStore never blocks, so deadlines
 *        are ignored by documented contract (D-15)
 */
TEST_F(MemoryBlockStoreTest, ExpiredDeadlineStillSucceedsOnMemory)
{
    ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));
    ReadOptions expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto res = store_->Get(cid_a_, expired);
    ASSERT_TRUE(res);
}

/**
 * @given a MemoryBlockStore (public dual inheritance)
 * @when the store handle is probed with
 *        std::dynamic_pointer_cast<FaultController>
 * @then the probe succeeds - the conformance kit's fault seam (D-10)
 */
TEST_F(MemoryBlockStoreTest, FaultControllerProbeSucceeds)
{
    auto probed = std::dynamic_pointer_cast<FaultController>(store_);
    ASSERT_NE(probed, nullptr);
    EXPECT_EQ(probed->GetFault(), FaultKind::None);
}

// Plan-03 kit instantiation (the marker from Plan 02): one macro line per
// suite; two distinct suite names prove coexistence in one binary.
SGNS_BLOCKSTORE_CONFORMANCE_TESTS(
    MemoryBlockStoreConformance,
    [] { return std::shared_ptr<sgns::blockstore::BlockStore>(
        std::make_shared<sgns::blockstore::MemoryBlockStore>()); })

SGNS_BLOCKSTORE_CONFORMANCE_TESTS(
    MemoryBlockStoreConformanceRepeat,
    [] { return std::shared_ptr<sgns::blockstore::BlockStore>(
        std::make_shared<sgns::blockstore::MemoryBlockStore>()); })
