/**
 * @file in_memory_s3_client_test.cpp
 * @brief Behavior suite for the InMemoryS3Client fake - the SC3 hermetic
 *        adapter tier (BACK-05) proving decisions D-01 (one Put with
 *        createOnly), D-02 (alreadyPresent in the success slot), and D-05
 *        (per-op fault scripting) with the frozen taxonomy (CORE-02).
 *
 * Plain GTest discipline: EXPECT_TRUE/ASSERT_FALSE + manual
 * .value()/.error() extraction, no outcome macros. Fully hermetic: no
 * network, no live endpoint, no env gate, no sleeps - runs in the default
 * CI lane. The Phase-5 fault matrix composes on this suite's surface.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "InMemoryS3Client.hpp"
#include <sgns-blockstore/S3Client.hpp>

namespace
{
    using sgns::blockstore::BlockSource;
    using sgns::blockstore::BlockStatus;
    using sgns::blockstore::InMemoryS3Client;
    using sgns::blockstore::PutObjectOptions;
    using sgns::blockstore::S3FaultKind;
    using sgns::blockstore::S3HeadResult;
    using sgns::blockstore::S3Object;
    using sgns::blockstore::S3Op;
    using sgns::blockstore::S3PutResult;
    using sgns::blockstore::StatusClass;

    std::vector<uint8_t> BytesOf(const std::string& s)
    {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    class InMemoryS3ClientTest : public ::testing::Test
    {
    public:
        InMemoryS3Client client_;
        const std::string key_a_ = "v1/blocks/aaaa";
        const std::string key_b_ = "v1/blocks/bbbb";
        const std::vector<uint8_t> bytes_a_ = BytesOf("block a payload");
        const std::vector<uint8_t> bytes_b_ =
            BytesOf("block b payload - different length");
    };

    /**
     * @given a fresh fake
     * @when a plain Put then a Get on the same key
     * @then the round trip returns identical bytes with achieved=true and
     *        alreadyPresent=false (fresh key)
     */
    TEST_F(InMemoryS3ClientTest, PutGetRoundTrip)
    {
        auto put = client_.Put(key_a_, bytes_a_);
        ASSERT_TRUE(put);
        EXPECT_TRUE(put.value().achieved);
        EXPECT_FALSE(put.value().alreadyPresent);

        auto got = client_.Get(key_a_);
        ASSERT_TRUE(got);
        EXPECT_EQ(got.value().key, key_a_);
        EXPECT_EQ(got.value().bytes, bytes_a_);
    }

    /**
     * @given a key stored by a createOnly Put
     * @when a SECOND createOnly Put arrives with different bytes
     * @then the result is the SUCCESS slot with achieved=true and
     *        alreadyPresent=true (D-02) and a follow-up Get returns the
     *        ORIGINAL first bytes - the duplicate never overwrote (D-01,
     *        the BACK-05 idempotency edge)
     */
    TEST_F(InMemoryS3ClientTest,
           CreateOnlyDuplicateReturnsSuccessSlotWithAlreadyPresent)
    {
        ASSERT_TRUE(client_.Put(key_a_, bytes_a_, PutObjectOptions{true}));

        auto duplicate =
            client_.Put(key_a_, bytes_b_, PutObjectOptions{/*createOnly=*/true});
        ASSERT_TRUE(duplicate) << "createOnly duplicate must be the success slot";
        EXPECT_TRUE(duplicate.value().achieved);
        EXPECT_TRUE(duplicate.value().alreadyPresent);

        auto got = client_.Get(key_a_);
        ASSERT_TRUE(got);
        EXPECT_EQ(got.value().bytes, bytes_a_)
            << "duplicate createOnly Put must not overwrite the original bytes";
    }

    /**
     * @given a missing key
     * @when a createOnly Put arrives
     * @then achieved=true with alreadyPresent=false (the create path)
     */
    TEST_F(InMemoryS3ClientTest, CreateOnlyFreshKeyAchieves)
    {
        auto put = client_.Put(key_a_, bytes_a_, PutObjectOptions{true});
        ASSERT_TRUE(put);
        EXPECT_TRUE(put.value().achieved);
        EXPECT_FALSE(put.value().alreadyPresent);
    }

    /**
     * @given a key stored once
     * @when a plain (createOnly=false) Put arrives with different bytes
     * @then the object is overwritten and alreadyPresent=true reports the
     *        pre-existing key (pre-write computation, MemoryBlockStore
     *        precedent)
     */
    TEST_F(InMemoryS3ClientTest, OverwriteWithoutCreateOnly)
    {
        ASSERT_TRUE(client_.Put(key_a_, bytes_a_));

        auto overwrite = client_.Put(key_a_, bytes_b_);
        ASSERT_TRUE(overwrite);
        EXPECT_TRUE(overwrite.value().achieved);
        EXPECT_TRUE(overwrite.value().alreadyPresent);

        auto got = client_.Get(key_a_);
        ASSERT_TRUE(got);
        EXPECT_EQ(got.value().bytes, bytes_b_);
    }

    /**
     * @given a missing key
     * @when Get runs
     * @then the ERROR slot carries NotFound with source Remote
     *        (authoritative absence at the adapter tier)
     */
    TEST_F(InMemoryS3ClientTest, GetMissingIsNotFound)
    {
        auto got = client_.Get(key_a_);
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::NotFound);
        EXPECT_NE(got.error().source, BlockSource::Unknown);
        EXPECT_EQ(got.error().source, BlockSource::Remote);
    }

    /**
     * @given one stored key and one missing key
     * @when Head runs on both
     * @then the hit is the success slot exists=true with the stored size,
     *        and the miss is ALSO the success slot with exists=false -
     *        never an error (an error means the answer was unobtainable)
     */
    TEST_F(InMemoryS3ClientTest, HeadHitAndMiss)
    {
        ASSERT_TRUE(client_.Put(key_a_, bytes_a_));

        auto hit = client_.Head(key_a_);
        ASSERT_TRUE(hit);
        EXPECT_TRUE(hit.value().exists);
        EXPECT_EQ(hit.value().size,
                  static_cast<uint64_t>(bytes_a_.size()));

        auto miss = client_.Head(key_b_);
        ASSERT_TRUE(miss) << "authoritative miss must be the success slot";
        EXPECT_FALSE(miss.value().exists);
        EXPECT_EQ(miss.value().size, 0u);
    }

    /**
     * @given a stored key
     * @when Delete runs twice (then a Get)
     * @then both deletes succeed (idempotent - missing key is success,
     *        S3 RemoveObject semantics) and the subsequent Get is NotFound
     */
    TEST_F(InMemoryS3ClientTest, DeleteIsIdempotent)
    {
        ASSERT_TRUE(client_.Put(key_a_, bytes_a_));

        EXPECT_TRUE(client_.Delete(key_a_));
        EXPECT_TRUE(client_.Delete(key_a_)) << "second delete must succeed";

        auto got = client_.Get(key_a_);
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::NotFound);
    }

    /**
     * @given an Unavailable fault scripted on Get
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable - explicitly NOT
     *        NotFound (SC4: non-authoritative failures must never read as
     *        authoritative absence)
     */
    TEST_F(InMemoryS3ClientTest, FaultUnavailableIsTemporarilyUnavailable)
    {
        ASSERT_TRUE(client_.Put(key_a_, bytes_a_));
        client_.SetFault(S3Op::Get, S3FaultKind::Unavailable);

        auto got = client_.Get(key_a_);
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);
        EXPECT_NE(got.error().source, BlockSource::Unknown);
    }

    /**
     * @given a CorruptData fault scripted on Get
     * @when Get runs
     * @then the error slot is Corrupt with Remote source
     */
    TEST_F(InMemoryS3ClientTest, FaultCorruptDataIsCorrupt)
    {
        client_.SetFault(S3Op::Get, S3FaultKind::CorruptData);
        auto got = client_.Get(key_a_);
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::Corrupt);
        EXPECT_EQ(got.error().source, BlockSource::Remote);
    }

    /**
     * @given an Unauthorized fault scripted on Put
     * @when Put runs
     * @then the error slot is Unauthorized (the taxonomy's fourth class)
     */
    TEST_F(InMemoryS3ClientTest, FaultUnauthorizedIsUnauthorized)
    {
        client_.SetFault(S3Op::Put, S3FaultKind::Unauthorized);
        auto put = client_.Put(key_a_, bytes_a_);
        ASSERT_FALSE(put);
        EXPECT_EQ(put.error().code, StatusClass::Unauthorized);
        EXPECT_EQ(put.error().source, BlockSource::Remote);
    }

    /**
     * @given a stored key and an Unavailable fault on Get
     * @when the fault fires once, then ClearFaults, then Get again
     * @then the faulted Get is TemporarilyUnavailable and the next Get
     *        SUCCEEDS - no sticky negative cache (mirrors the conformance
     *        kit case of the same name)
     */
    TEST_F(InMemoryS3ClientTest, FaultClearsNoStickyNegativeCache)
    {
        ASSERT_TRUE(client_.Put(key_a_, bytes_a_));
        client_.SetFault(S3Op::Get, S3FaultKind::Unavailable);

        auto faulted = client_.Get(key_a_);
        ASSERT_FALSE(faulted);
        EXPECT_EQ(faulted.error().code, StatusClass::TemporarilyUnavailable);

        client_.ClearFaults();
        auto recovered = client_.Get(key_a_);
        ASSERT_TRUE(recovered) << "cleared fault must not stick";
        EXPECT_EQ(recovered.value().bytes, bytes_a_);
    }

    /**
     * @given an Unavailable fault scripted on Put ONLY
     * @when Get runs
     * @then Get succeeds - the per-op fault table never leaks across op
     *        kinds (D-05 per-op scripting)
     */
    TEST_F(InMemoryS3ClientTest, PerOpFaultsAreIndependent)
    {
        ASSERT_TRUE(client_.Put(key_a_, bytes_a_));
        client_.SetFault(S3Op::Put, S3FaultKind::Unavailable);

        auto got = client_.Get(key_a_);
        ASSERT_TRUE(got) << "fault on Put must not affect Get";
        EXPECT_EQ(got.value().bytes, bytes_a_);

        auto put = client_.Put(key_b_, bytes_b_);
        ASSERT_FALSE(put);
        EXPECT_EQ(put.error().code, StatusClass::TemporarilyUnavailable);
    }
} // namespace
