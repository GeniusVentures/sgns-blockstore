/**
 * @file s3_block_store_test.cpp
 * @brief S3BlockStore tests (BACK-07/SC1/SC2 hermetic half): the full
 *        Phase-1 conformance kit over a FRESH InMemoryS3Client per
 *        case, plus the store-specific key-layout, idempotency, and
 *        authoritative-absence probes against the fake's key-
 *        observability seam.
 *
 * Tier: default CI lane - SDK-free (links exactly sgns-blockstore +
 * p2p::p2p_cid). The fake is reached through the src/ include
 * privilege (test-surface-only, in_memory_s3_client_test precedent);
 * s3d-facing semantics are the live tiers' job (04-02/04-03).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <sgns-blockstore/S3BlockStore.hpp>

// Internal fake - src/ include privilege (test-surface-only; the
// installed surface stays SDK-free and fake-free by construction).
#include "InMemoryS3Client.hpp"

#include <libp2p/multi/content_identifier_codec.hpp>

#include "BlockStoreConformanceTests.hpp"

namespace
{
    using sgns::blockstore::BlockStore;
    using sgns::blockstore::DurabilityLevel;
    using sgns::blockstore::InMemoryS3Client;
    using sgns::blockstore::S3BlockStore;

    /// @brief Store + fake pair fixture: the fake handle is the SC1
    ///        key-observability probe surface.
    class S3BlockStoreTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            fake_ = std::make_shared<InMemoryS3Client>();
            auto opened = S3BlockStore::Open(fake_);
            ASSERT_TRUE(opened);
            store_ = std::move(opened.value());
        }

        std::shared_ptr<InMemoryS3Client> fake_;
        std::unique_ptr<S3BlockStore> store_;
    };
} // namespace

/**
 * @given the known-vector payload "hello world" (sha256 digest starts
 *        with byte 0xb9)
 * @when it is Put through S3BlockStore over the fake
 * @then exactly one object exists and its key is the BACK-07 layout
 *        v1/blocks/<codec>/<digest-byte-hex>/<cid-string> - derived
 *        in-test via ContentIdentifierCodec::toString + the multihash
 *        digest byte, and pinned to the known vector literal (the ONLY
 *        place this literal may appear - Pitfall 8: never paste it
 *        next to other payloads)
 */
TEST_F(S3BlockStoreTest, KeyLayoutKnownVectorHelloWorld)
{
    using sgns::blockstore::conformance::MakeCid;
    using sgns::blockstore::conformance::SpanOf;
    const std::string payload = "hello world";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(store_->Put(cid, SpanOf(payload)));

    const auto keys = fake_->ObjectKeys();
    ASSERT_EQ(keys.size(), 1u);

    // Compute the expected key exactly as the store must have derived
    // it: codec token, first-digest-byte hex fan-out, CID string.
    auto cid_string =
        libp2p::multi::ContentIdentifierCodec::toString(cid);
    ASSERT_TRUE(cid_string);
    const auto digest = cid.content_address.getHash();
    ASSERT_FALSE(digest.empty());
    static const char* kHex = "0123456789abcdef";
    const auto first = digest[0];
    const std::string fanout{ kHex[first >> 4], kHex[first & 0x0F] };
    const std::string expected =
        "v1/blocks/raw/" + fanout + "/" + cid_string.value();

    EXPECT_EQ(keys[0], expected);
    // Known-vector pin (sha256("hello world") = b94d27b9...):
    EXPECT_EQ(expected,
              "v1/blocks/raw/b9/"
              "bafkreifzjut3te2nhyekklss27nh3k72ysco7y32koao5eei66wof36n5e");
}

/**
 * @given a stored block
 * @when the identical CID/bytes pair is Put again
 * @then the SUCCESS slot reports alreadyPresent=true at the
 *        ObjectStoreAccepted level (the adapter absorbs the 412 - SC2
 *        idempotency), and the fake still holds exactly one object
 *        with the original bytes
 */
TEST_F(S3BlockStoreTest, RePutSameCidIsAlreadyPresentSingleObject)
{
    using sgns::blockstore::conformance::MakeCid;
    using sgns::blockstore::conformance::SpanOf;
    const std::string payload = "idempotency payload";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(store_->Put(cid, SpanOf(payload)));

    auto re_put = store_->Put(cid, SpanOf(payload));
    ASSERT_TRUE(re_put);
    EXPECT_TRUE(re_put.value().alreadyPresent);
    EXPECT_EQ(re_put.value().achieved,
              DurabilityLevel::ObjectStoreAccepted);

    const auto keys = fake_->ObjectKeys();
    ASSERT_EQ(keys.size(), 1u); // never duplicated, never overwritten

    auto got = store_->Get(cid);
    ASSERT_TRUE(got);
    EXPECT_EQ(got.value().bytes,
              std::vector<uint8_t>(payload.begin(), payload.end()));
}

/**
 * @given a CID that was never Put anywhere
 * @when Has probes it
 * @then the SUCCESS slot carries false (Head's authoritative
 *        exists=false) - never a failure and never NotFound-as-error
 */
TEST_F(S3BlockStoreTest, HasFalseForNeverPutCid)
{
    using sgns::blockstore::conformance::MakeCid;
    const auto cid = MakeCid("never put anywhere");
    auto has = store_->Has(cid);
    ASSERT_TRUE(has);
    EXPECT_FALSE(has.value());
}

// Phase-1 conformance kit (BACK-02): the full suite against the S3
// backend over a FRESH fake per case (the kit's per-case hermeticity
// contract). Fault-conformance cases skip loudly - this backend's
// fault taxonomy is scripted at the ADAPTER fake (SetFault), exercised
// by the store-specific fault tests.
SGNS_BLOCKSTORE_CONFORMANCE_TESTS(
    S3BlockStoreConformance,
    [] {
        auto fake =
            std::make_shared<sgns::blockstore::InMemoryS3Client>();
        auto opened = sgns::blockstore::S3BlockStore::Open(fake);
        if (!opened)
        {
            return std::shared_ptr<sgns::blockstore::BlockStore>();
        }
        return std::shared_ptr<sgns::blockstore::BlockStore>(
            std::move(opened.value()));
    })
