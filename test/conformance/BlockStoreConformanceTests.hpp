/**
 * @file BlockStoreConformanceTests.hpp
 * @brief The reusable BlockStore conformance kit (BACK-02, D-09..D-12).
 *
 * One macro - SGNS_BLOCKSTORE_CONFORMANCE_TESTS(SuiteName, factory) -
 * instantiates the full suite against any backend. The factory is invoked
 * PER CASE so every TEST_F sees a fresh store (hermetic fault cases).
 *
 * Discipline (D-11): plain GTest EXPECT/ASSERT + manual .value()/.error()
 * extraction, zero outcome test macros; the kit depends on GTest + outcome
 * + std only.
 *
 * Build-time only (D-12): this header is NEVER installed; it lives under
 * test/conformance/ and compiles into backend test binaries via
 * target_include_directories(... ${CMAKE_SOURCE_DIR}/test/conformance).
 */

#ifndef SGNS_BLOCKSTORE_CONFORMANCETESTS_HPP
#define SGNS_BLOCKSTORE_CONFORMANCETESTS_HPP

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <sgns-blockstore/Block.hpp>
#include <sgns-blockstore/BlockStatus.hpp>
#include <sgns-blockstore/BlockStore.hpp>
#include <sgns-blockstore/Cid.hpp>
#include <sgns-blockstore/Durability.hpp>
#include <sgns-blockstore/FaultController.hpp>
#include <sgns-blockstore/Options.hpp>

#include <libp2p/multi/multihash.hpp>

namespace sgns::blockstore::conformance
{
    inline gsl::span<const uint8_t> SpanOf(const std::string& s)
    {
        return gsl::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    inline Cid MakeCid(const std::string& payload)
    {
        // SHA-256 CIDv1 RAW - the only CID shape v1 accepts (D-08).
        return CidOf(SpanOf(payload), libp2p::multi::MulticodecType::Code::RAW)
            .value();
    }

    inline Cid MakeBlake2bCid()
    {
        // The CRDT CBOR boundary shape that must throw at API entry (D-08).
        static const uint8_t digest[32] = {
            0xb1, 0xb2, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
            0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
            0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
        };
        return Cid(Cid::Version::V1, libp2p::multi::MulticodecType::Code::RAW,
                   libp2p::multi::Multihash::create(
                       libp2p::multi::HashType::blake2b_256,
                       gsl::span<const uint8_t>(digest, 32))
                       .value());
    }
}

/**
 * @brief Instantiate the full BlockStore conformance suite (D-09).
 *
 * @param SuiteName unique GTest suite name; every generated symbol is
 *                  parameterized on it, so multiple instantiations coexist
 *                  in one binary (BACK-02 adjacency edge self-check)
 * @param FACTORY   expression returning std::shared_ptr<BlockStore>;
 *                  invoked per case for hermetic fresh-store semantics
 */
#define SGNS_BLOCKSTORE_CONFORMANCE_TESTS(SuiteName, FACTORY)                  \
    class SuiteName : public ::testing::Test                                   \
    {                                                                          \
    public:                                                                    \
        void SetUp() override { store_ = FACTORY(); }                          \
        std::shared_ptr<sgns::blockstore::BlockStore> store_;                  \
        std::string payload_a_ = "conformance payload a";                      \
        std::string payload_b_ = "conformance payload b - different length";   \
        sgns::blockstore::Cid cid_a_ =                                         \
            sgns::blockstore::conformance::MakeCid(payload_a_);                \
        sgns::blockstore::Cid cid_b_ =                                         \
            sgns::blockstore::conformance::MakeCid(payload_b_);                \
    };                                                                         \
    /* @given a store and a payload with its CidOf-computed CID                \
     * @when Put then Get run                                                  \
     * @then the round trip is byte-identical                                  \
     */                                                                        \
    TEST_F(SuiteName, PutGetRoundTrip)                                         \
    {                                                                          \
        using sgns::blockstore::conformance::SpanOf;                           \
        ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));                  \
        auto got = store_->Get(cid_a_);                                        \
        ASSERT_TRUE(got);                                                      \
        EXPECT_EQ(got.value().bytes,                                           \
                  std::vector<uint8_t>(payload_a_.begin(), payload_a_.end())); \
    }                                                                          \
    /* @given an empty store @when Get fetches a missing CID                   \
     * @then a FAILURE with code NotFound and a set source (D-03)              \
     */                                                                        \
    TEST_F(SuiteName, GetMissingReportsNotFoundFailure)                        \
    {                                                                          \
        using sgns::blockstore::conformance::SpanOf;                           \
        auto res = store_->Get(cid_a_);                                        \
        ASSERT_FALSE(res);                                                     \
        EXPECT_EQ(res.error().code,                                            \
                  sgns::blockstore::StatusClass::NotFound);                    \
        EXPECT_NE(res.error().source, sgns::blockstore::BlockSource::Unknown); \
    }                                                                          \
    /* @given an empty store @when Has checks a missing CID                    \
     * @then SUCCESS with the value false - authoritative absence is a         \
     * success bool for Has only (never a failure)                             \
     */                                                                        \
    TEST_F(SuiteName, HasFalseWhenMissingIsSuccessFalse)                       \
    {                                                                          \
        auto res = store_->Has(cid_a_);                                        \
        ASSERT_TRUE(res);                                                      \
        EXPECT_FALSE(res.value());                                             \
    }                                                                          \
    TEST_F(SuiteName, HasTrueAfterPut)                                         \
    {                                                                          \
        using sgns::blockstore::conformance::SpanOf;                           \
        ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));                  \
        auto res = store_->Has(cid_a_);                                        \
        ASSERT_TRUE(res);                                                      \
        EXPECT_TRUE(res.value());                                              \
    }                                                                          \
    /* @given a stored block @when GetSize measures it                          \
     * @then the exact byte count (CORE-04 precision)                          \
     */                                                                        \
    TEST_F(SuiteName, GetSizeMatchesPayloadBytes)                              \
    {                                                                          \
        using sgns::blockstore::conformance::SpanOf;                           \
        ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));                  \
        auto res = store_->GetSize(cid_a_);                                    \
        ASSERT_TRUE(res);                                                      \
        EXPECT_EQ(res.value(), static_cast<uint64_t>(payload_a_.size()));      \
    }                                                                          \
    /* @given the empty payload (sha-256 of empty input - CORE-04 empty edge)  \
     * @when Put then Get run                                                  \
     * @then the round trip succeeds with zero bytes                           \
     */                                                                        \
    TEST_F(SuiteName, EmptyPayloadRoundTrip)                                   \
    {                                                                          \
        const auto empty_cid = sgns::blockstore::conformance::MakeCid("");     \
        ASSERT_TRUE(store_->Put(empty_cid, gsl::span<const uint8_t>()));       \
        auto got = store_->Get(empty_cid);                                     \
        ASSERT_TRUE(got);                                                      \
        EXPECT_TRUE(got.value().bytes.empty());                                \
    }                                                                          \
    TEST_F(SuiteName, EmptyStoreGetSizeIsNotFoundFailure)                      \
    {                                                                          \
        auto res = store_->GetSize(cid_a_);                                    \
        ASSERT_FALSE(res);                                                     \
        EXPECT_EQ(res.error().code,                                            \
                  sgns::blockstore::StatusClass::NotFound);                    \
    }                                                                          \
    /* @given a stored block @when the identical pair is Put again             \
     * @then alreadyPresent == true and the original bytes survive             \
     * (identical re-put merges, never duplicates)                             \
     */                                                                        \
    TEST_F(SuiteName, RePutIdenticalBytesIsAlreadyPresent)                     \
    {                                                                          \
        using sgns::blockstore::conformance::SpanOf;                           \
        ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));                  \
        auto res = store_->Put(cid_a_, SpanOf(payload_a_));                    \
        ASSERT_TRUE(res);                                                      \
        EXPECT_TRUE(res.value().alreadyPresent);                               \
        auto got = store_->Get(cid_a_);                                        \
        ASSERT_TRUE(got);                                                      \
        EXPECT_EQ(got.value().bytes,                                           \
                  std::vector<uint8_t>(payload_a_.begin(), payload_a_.end())); \
    }                                                                          \
    /* @given a payload with one flipped byte vs its claimed CID               \
     * @when Put runs (CORE-04 boundary: exact match succeeds, one byte        \
     * either side is Corrupt)                                                 \
     * @then failure Corrupt with source == Caller                             \
     */                                                                        \
    TEST_F(SuiteName, PutMismatchedBytesIsCorruptWithCallerSource)             \
    {                                                                          \
        using sgns::blockstore::conformance::SpanOf;                           \
        auto corrupted = payload_a_;                                           \
        corrupted[0] = static_cast<char>(~corrupted[0]);                       \
        auto res = store_->Put(cid_a_, SpanOf(corrupted));                     \
        ASSERT_FALSE(res);                                                     \
        EXPECT_EQ(res.error().code, sgns::blockstore::StatusClass::Corrupt);   \
        EXPECT_EQ(res.error().source, sgns::blockstore::BlockSource::Caller);  \
    }                                                                          \
    TEST_F(SuiteName, PutNonSha256HashThrowsInvalidArgument)                   \
    {                                                                          \
        using sgns::blockstore::conformance::SpanOf;                           \
        EXPECT_THROW(                                                          \
            (void)store_->Put(sgns::blockstore::conformance::MakeBlake2bCid(), \
                              SpanOf(payload_a_)),                             \
            std::invalid_argument);                                            \
    }                                                                          \
    TEST_F(SuiteName, GetNonSha256HashThrowsInvalidArgument)                   \
    {                                                                          \
        EXPECT_THROW(                                                          \
            (void)store_->Get(sgns::blockstore::conformance::MakeBlake2bCid()),\
            std::invalid_argument);                                            \
    }                                                                          \
    /* @given [good, mismatched, good] @when PutMany runs                      \
     * @then size 3 with index correspondence, middle Corrupt, siblings        \
     * stored and retrievable (D-14 ordering + no rollback)                    \
     */                                                                        \
    TEST_F(SuiteName, PutManyPreservesOrderAndNeverRollsBack)                  \
    {                                                                          \
        using sgns::blockstore::conformance::SpanOf;                           \
        auto corrupted = payload_a_;                                           \
        corrupted[1] = static_cast<char>(~corrupted[1]);                       \
        const sgns::blockstore::BlockInput batch[] = {                         \
            {cid_a_, SpanOf(payload_a_)},                                      \
            {cid_a_, SpanOf(corrupted)},                                       \
            {cid_b_, SpanOf(payload_b_)},                                      \
        };                                                                     \
        auto res = store_->PutMany(batch);                                     \
        ASSERT_TRUE(res);                                                      \
        ASSERT_EQ(res.value().size(), 3u);                                     \
        EXPECT_TRUE(res.value()[0]);                                           \
        ASSERT_FALSE(res.value()[1]);                                          \
        EXPECT_EQ(res.value()[1].error().code,                                 \
                  sgns::blockstore::StatusClass::Corrupt);                     \
        EXPECT_TRUE(res.value()[2]);                                           \
        EXPECT_TRUE(store_->Get(cid_a_));                                      \
        EXPECT_TRUE(store_->Get(cid_b_));                                      \
    }                                                                          \
    /* @given a batch containing a blake2b CID @when PutMany runs              \
     * @then std::invalid_argument BEFORE any write; valid entries absent      \
     */                                                                        \
    TEST_F(SuiteName, PutManyInvalidHashTypeThrowsBeforeAnyWrite)              \
    {                                                                          \
        using sgns::blockstore::conformance::SpanOf;                           \
        const sgns::blockstore::BlockInput batch[] = {                         \
            {cid_a_, SpanOf(payload_a_)},                                      \
            {sgns::blockstore::conformance::MakeBlake2bCid(),                  \
             SpanOf(payload_b_)},                                              \
        };                                                                     \
        EXPECT_THROW((void)store_->PutMany(batch), std::invalid_argument);     \
        auto has_valid = store_->Has(cid_a_);                                  \
        ASSERT_TRUE(has_valid);                                                \
        EXPECT_FALSE(has_valid.value());                                       \
    }                                                                          \
    /* Fault-seam cases (D-10): probed via dynamic_pointer_cast; backends      \
     * without the seam get loud labelled skips naming [fault-conformance].    \
     */                                                                        \
    TEST_F(SuiteName, FaultUnavailableSurfacesAsTemporarilyUnavailableNeverNotFound)      \
    {                                                                                      \
        using sgns::blockstore::conformance::SpanOf;                                       \
        ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));                              \
        auto fc = std::dynamic_pointer_cast<sgns::blockstore::FaultController>(store_);    \
        if (!fc)                                                                           \
        {                                                                                  \
            GTEST_SKIP() << "[fault-conformance] backend does not implement "              \
                            "FaultController";                                             \
        }                                                                                  \
        fc->SetFault(sgns::blockstore::FaultKind::Unavailable);                            \
        auto got = store_->Get(cid_a_);                                                    \
        ASSERT_FALSE(got);                                                                 \
        EXPECT_EQ(got.error().code,                                                        \
                  sgns::blockstore::StatusClass::TemporarilyUnavailable);                  \
        auto has = store_->Has(cid_a_);                                                    \
        /* Must be a FAILURE - success-false and NotFound failure both fail. */             \
        ASSERT_FALSE(has.has_value());                                                     \
        EXPECT_EQ(has.error().code,                                                        \
                  sgns::blockstore::StatusClass::TemporarilyUnavailable);                  \
        EXPECT_NE(has.error().code, sgns::blockstore::StatusClass::NotFound);              \
    }                                                                                      \
    TEST_F(SuiteName, FaultClearsNoStickyNegativeCache)                                    \
    {                                                                                      \
        using sgns::blockstore::conformance::SpanOf;                                       \
        ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));                              \
        auto fc = std::dynamic_pointer_cast<sgns::blockstore::FaultController>(store_);    \
        if (!fc)                                                                           \
        {                                                                                  \
            GTEST_SKIP() << "[fault-conformance] backend does not implement "              \
                            "FaultController";                                             \
        }                                                                                  \
        fc->SetFault(sgns::blockstore::FaultKind::Unavailable);                            \
        ASSERT_FALSE(store_->Get(cid_a_));                                                 \
        fc->SetFault(sgns::blockstore::FaultKind::None);                                   \
        /* Same-CID Get succeeds - a failed fetch never poisons the next. */               \
        auto got = store_->Get(cid_a_);                                                    \
        ASSERT_TRUE(got);                                                                  \
    }                                                                                      \
    TEST_F(SuiteName, FaultCorruptDataRejectedCorruptWithBackendSource)                    \
    {                                                                                      \
        using sgns::blockstore::conformance::SpanOf;                                       \
        ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));                              \
        auto fc = std::dynamic_pointer_cast<sgns::blockstore::FaultController>(store_);    \
        if (!fc)                                                                           \
        {                                                                                  \
            GTEST_SKIP() << "[fault-conformance] backend does not implement "              \
                            "FaultController";                                             \
        }                                                                                  \
        fc->SetFault(sgns::blockstore::FaultKind::CorruptData);                            \
        auto got = store_->Get(cid_a_);                                                    \
        ASSERT_FALSE(got);                                                                 \
        EXPECT_EQ(got.error().code, sgns::blockstore::StatusClass::Corrupt);               \
        /* Backend-side corruption is never attributed to the caller. */                   \
        EXPECT_NE(got.error().source, sgns::blockstore::BlockSource::Caller);              \
        EXPECT_NE(got.error().source, sgns::blockstore::BlockSource::Unknown);             \
        fc->SetFault(sgns::blockstore::FaultKind::None);                                   \
        ASSERT_TRUE(store_->Get(cid_a_));                                                  \
    }                                                                                      \
    /* Unauthorized is its own class: never NotFound, never                \
     * TemporarilyUnavailable, never success-false - and non-sticky after   \
     * the fault clears (success criterion 2's Unauthorized clause).        \
     */                                                                    \
    TEST_F(SuiteName, FaultUnauthorizedSurfacesAsUnauthorizedNeverNotFound)  \
    {                                                                      \
        using sgns::blockstore::conformance::SpanOf;                       \
        ASSERT_TRUE(store_->Put(cid_a_, SpanOf(payload_a_)));              \
        auto fc =                                                           \
            std::dynamic_pointer_cast<sgns::blockstore::FaultController>(  \
                store_);                                                    \
        if (!fc)                                                            \
        {                                                                  \
            GTEST_SKIP() << "[fault-conformance] backend does not implement " \
                            "FaultController";                               \
        }                                                                  \
        fc->SetFault(sgns::blockstore::FaultKind::Unauthorized);           \
        auto got = store_->Get(cid_a_);                                    \
        ASSERT_FALSE(got);                                                 \
        EXPECT_EQ(got.error().code,                                        \
                  sgns::blockstore::StatusClass::Unauthorized);            \
        EXPECT_NE(got.error().code,                                        \
                  sgns::blockstore::StatusClass::NotFound);                \
        EXPECT_NE(got.error().code,                                        \
                  sgns::blockstore::StatusClass::TemporarilyUnavailable);  \
        auto has = store_->Has(cid_a_);                                    \
        ASSERT_FALSE(has.has_value()); /* failure, not success-false */    \
        EXPECT_EQ(has.error().code,                                        \
                  sgns::blockstore::StatusClass::Unauthorized);            \
        fc->SetFault(sgns::blockstore::FaultKind::None);                   \
        ASSERT_TRUE(store_->Get(cid_a_)); /* non-sticky */                 \
    }

#endif  // SGNS_BLOCKSTORE_CONFORMANCETESTS_HPP
