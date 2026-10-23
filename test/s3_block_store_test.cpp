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
#include <set>
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
 * @given the pinned CRDT-shape block {} (RFC 8949 a0) with its real
 *        blake2b-256 CID (cross-check-log vector, D-02)
 * @when it is Put through S3BlockStore over the fake
 * @then exactly one object exists and its key is
 *        v1/blocks/dag-cbor/d3/<pinned CID string> - the DAG_CBOR token
 *        lands in the layout, and the pinned CID string doubles as the
 *        DAG_CBOR toString proof (a codec that could not render would
 *        fail DeriveKey loudly - RESEARCH landmine 3)
 */
TEST_F(S3BlockStoreTest, DagCborKeyLayoutKnownVector)
{
    using sgns::blockstore::conformance::SpanOf;
    const std::string payload("\xa0", 1);
    const auto cid = sgns::blockstore::CidOf(
        SpanOf(payload),
        libp2p::multi::MulticodecType::Code::DAG_CBOR,
        libp2p::multi::HashType::blake2b_256).value();
    ASSERT_TRUE(store_->Put(cid, SpanOf(payload)));

    const auto keys = fake_->ObjectKeys();
    ASSERT_EQ(keys.size(), 1u);

    // Fan-out byte = first blake2b digest byte (0xd3 for the a0 vector:
    // the cross-check digest d36a2619...). Full key pinned to the
    // cross-check-log CID literal (D-02).
    EXPECT_EQ(keys[0],
              "v1/blocks/dag-cbor/d3/"
              "bafy2bzacedjwujqzuzzesrqe4en3ir6lz5jdd2psxis4efuro7w4sqn5kcwwy");
}

/**
 * @given a stored pinned blake2b block whose FETCHED bytes the fake
 *        flips (SetFlipBytesForKey)
 * @when Get runs with verification
 * @then Corrupt attributed to Remote - the verify-on-get site branches
 *        on the CID's own blake2b-256 multihash (SC2), and clearing the
 *        flip is non-sticky
 */
TEST_F(S3BlockStoreTest, Blake2bGetCorruptedFetchIsCorruptWithRemoteSource)
{
    using sgns::blockstore::conformance::SpanOf;
    const std::string payload("\xa0", 1);
    const auto cid = sgns::blockstore::CidOf(
        SpanOf(payload),
        libp2p::multi::MulticodecType::Code::DAG_CBOR,
        libp2p::multi::HashType::blake2b_256).value();
    ASSERT_TRUE(store_->Put(cid, SpanOf(payload)));

    const std::string key =
        "v1/blocks/dag-cbor/d3/"
        "bafy2bzacedjwujqzuzzesrqe4en3ir6lz5jdd2psxis4efuro7w4sqn5kcwwy";
    fake_->SetFlipBytesForKey(key, true);
    auto got = store_->Get(cid);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, sgns::blockstore::StatusClass::Corrupt);
    EXPECT_EQ(got.error().source, sgns::blockstore::BlockSource::Remote);
    fake_->SetFlipBytesForKey(key, false);
    ASSERT_TRUE(store_->Get(cid));
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

/**
 * @given a fixture set of payloads (>= 4)
 * @when each is Put through the store
 * @then every derived key's fan-out component equals the two-lowercase-
 *        hex rendering of that CID's FIRST MULTIHASH DIGEST BYTE computed
 *        in-test from the CID, and the set demonstrates >= 2 distinct
 *        components (the operational fan-out). Invariant: the fan-out
 *        source is NEVER the CID string head - every raw+sha256 CIDv1
 *        string starts with the constant prefix "bafkrei", so
 *        string-head fan-out would collapse to exactly one bucket.
 */
TEST_F(S3BlockStoreTest, FanoutComponentIsFirstDigestByteHex)
{
    using sgns::blockstore::conformance::MakeCid;
    using sgns::blockstore::conformance::SpanOf;
    const std::vector<std::string> payloads = {
        "fanout fixture one", "fanout fixture two - longer",
        "fanout fixture three", "fanout fixture four !!",
    };
    static const char* kHex = "0123456789abcdef";
    std::set<std::string> distinct_components;
    for (const auto& payload : payloads)
    {
        const auto cid = MakeCid(payload);
        ASSERT_TRUE(store_->Put(cid, SpanOf(payload)));
        const auto digest = cid.content_address.getHash();
        ASSERT_FALSE(digest.empty());
        const auto first = digest[0];
        const std::string expected_fanout{
            kHex[first >> 4], kHex[first & 0x0F]};

        const auto keys = fake_->ObjectKeys();
        ASSERT_EQ(keys.size(), 1u); // fresh fake check is per-iteration;
        // rebuild below uses cumulative assertion via per-case store
        for (const auto& key : keys)
        {
            // key = <prefix>/<codec>/<xx>/<cid> - the fan-out component
            // is the second-to-last path segment.
            const auto last_slash = key.rfind('/');
            ASSERT_NE(last_slash, std::string::npos);
            const auto rest = key.substr(0, last_slash);
            const auto prev_slash = rest.rfind('/');
            ASSERT_NE(prev_slash, std::string::npos);
            EXPECT_EQ(key.substr(prev_slash + 1,
                                 last_slash - prev_slash - 1),
                      expected_fanout);
            distinct_components.insert(
                key.substr(prev_slash + 1, last_slash - prev_slash - 1));
        }
        // Reset per-iteration state by draining the fake through the
        // adapter directly (keys accumulate otherwise).
        for (const auto& key : fake_->ObjectKeys())
        {
            ASSERT_TRUE(fake_->Delete(key));
        }
    }
    EXPECT_GE(distinct_components.size(), 2u);
}

/**
 * @given wrong bytes planted at a CID's derived key directly through
 *        the fake (the fake stores by key - a mismatched payload at the
 *        right key simulates remote corruption)
 * @when the store Gets that CID
 * @then the failure is Corrupt with BlockSource::Remote and an
 *        op-context message - never NotFound, never Caller attribution
 *        (the bytes came from the remote).
 */
TEST_F(S3BlockStoreTest, GetWrongBytesAtDerivedKeyIsCorruptRemote)
{
    using sgns::blockstore::conformance::MakeCid;
    using sgns::blockstore::conformance::SpanOf;
    const std::string payload = "remote corruption probe payload";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(store_->Put(cid, SpanOf(payload)));

    // Derive the key in-test exactly as the store does, then plant
    // mismatched bytes at it through the fake.
    auto cid_string =
        libp2p::multi::ContentIdentifierCodec::toString(cid);
    ASSERT_TRUE(cid_string);
    const auto digest = cid.content_address.getHash();
    static const char* kHex = "0123456789abcdef";
    const std::string fanout{
        kHex[digest[0] >> 4], kHex[digest[0] & 0x0F]};
    const std::string key = "v1/blocks/raw/" + fanout + "/"
        + cid_string.value();
    const std::string wrong = "NOT the block that hashes to this CID";
    const std::vector<uint8_t> wrong_bytes(wrong.begin(), wrong.end());
    auto planted = fake_->Put(key, wrong_bytes);
    ASSERT_TRUE(planted);

    auto got = store_->Get(cid);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, sgns::blockstore::StatusClass::Corrupt);
    EXPECT_EQ(got.error().source, sgns::blockstore::BlockSource::Remote);
    EXPECT_NE(got.error().message.find("Get"),
              std::string::npos); // op context present
}

/**
 * @given a stored block and the fake's success-slot flip armed at the
 *        derived key (the adapter SUCCEEDS with a first-byte-flipped
 *        copy - the store's CID verification is the detector, D-08/D-10)
 * @when the store Gets the CID twice, then the flip is disarmed and it
 *        Gets again
 * @then BOTH Gets fail Corrupt+Remote with op context (every Get
 *        re-fetches - no bad-byte cache, D-11), the post-repair Get
 *        succeeds with the original bytes (no sticky negative cache,
 *        D-06 inline recovery), and the fake still holds the key - the
 *        remote block was NEVER deleted (CORE-05: repair is external)
 */
TEST_F(S3BlockStoreTest,
       GetScriptedFlipIsCorruptRemoteReFetchStaysCorruptAndRecoversInline)
{
    using sgns::blockstore::conformance::MakeCid;
    using sgns::blockstore::conformance::SpanOf;
    const std::string payload = "scripted flip corruption payload";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(store_->Put(cid, SpanOf(payload)));

    // Derive the key in-test exactly as the store does (the
    // GetWrongBytesAtDerivedKeyIsCorruptRemote computation).
    auto cid_string =
        libp2p::multi::ContentIdentifierCodec::toString(cid);
    ASSERT_TRUE(cid_string);
    const auto digest = cid.content_address.getHash();
    static const char* kHex = "0123456789abcdef";
    const std::string fanout{
        kHex[digest[0] >> 4], kHex[digest[0] & 0x0F]};
    const std::string key = "v1/blocks/raw/" + fanout + "/"
        + cid_string.value();

    fake_->SetFlipBytesForKey(key, true);
    auto got = store_->Get(cid);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, sgns::blockstore::StatusClass::Corrupt);
    EXPECT_EQ(got.error().source, sgns::blockstore::BlockSource::Remote);
    EXPECT_NE(got.error().message.find("Get"),
              std::string::npos); // op context present

    // D-11: every Get re-fetches - the second Get is STILL Corrupt
    // (no bad-byte cache at the store layer).
    auto again = store_->Get(cid);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, sgns::blockstore::StatusClass::Corrupt);
    EXPECT_EQ(again.error().source, sgns::blockstore::BlockSource::Remote);

    // D-06 inline recovery: repair (disarm the flip) -> good fetch in
    // the SAME test - no sticky negative cache.
    fake_->SetFlipBytesForKey(key, false);
    auto recovered = store_->Get(cid);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered.value().bytes,
              std::vector<uint8_t>(payload.begin(), payload.end()));

    // CORE-05/D-11: the remote block was NEVER deleted - the corrupt
    // object stayed in place through every failing Get.
    const auto keys = fake_->ObjectKeys();
    const std::set<std::string> held(keys.begin(), keys.end());
    EXPECT_EQ(held.count(key), 1u);
}

/**
 * @given a complete-but-wrong-length body: a one-byte-short copy of the
 *        payload planted at the derived key through the fake
 * @when the store Gets that CID
 * @then the failure is Corrupt+Remote with op context - exact-byte CID
 *        verification has no length tolerance, and this store-layer
 *        assertion is the D-08 split's store half (the transport half
 *        - a mid-read framing break - lives in minio_s3_client_test)
 */
TEST_F(S3BlockStoreTest, GetTruncatedRemoteBodyIsCorruptRemote)
{
    using sgns::blockstore::conformance::MakeCid;
    using sgns::blockstore::conformance::SpanOf;
    const std::string payload = "truncated remote body probe payload";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(store_->Put(cid, SpanOf(payload)));

    auto cid_string =
        libp2p::multi::ContentIdentifierCodec::toString(cid);
    ASSERT_TRUE(cid_string);
    const auto digest = cid.content_address.getHash();
    static const char* kHex = "0123456789abcdef";
    const std::string fanout{
        kHex[digest[0] >> 4], kHex[digest[0] & 0x0F]};
    const std::string key = "v1/blocks/raw/" + fanout + "/"
        + cid_string.value();

    // One byte short of the payload (string iterators - never
    // gsl::span iterator temporaries, the MSVC debug landmine).
    const std::vector<uint8_t> short_bytes(payload.begin(),
                                           payload.end() - 1);
    auto planted = fake_->Put(key, short_bytes);
    ASSERT_TRUE(planted);

    auto got = store_->Get(cid);
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, sgns::blockstore::StatusClass::Corrupt);
    EXPECT_EQ(got.error().source, sgns::blockstore::BlockSource::Remote);
    EXPECT_NE(got.error().message.find("Get"),
              std::string::npos); // op context present
}

/**
 * @given Get with verify disabled (ReadOptions::verify == false)
 * @when wrong bytes sit at the derived key
 * @then the store returns the planted bytes unrejected - the seam is
 *        documented honestly; the prohibition against compiling
 *        verification out is Options.hpp's contract, not this store's.
 */
TEST_F(S3BlockStoreTest, GetVerifyDisabledReturnsPlantedBytes)
{
    using sgns::blockstore::conformance::MakeCid;
    using sgns::blockstore::conformance::SpanOf;
    const std::string payload = "verify-off probe payload";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(store_->Put(cid, SpanOf(payload)));

    auto cid_string =
        libp2p::multi::ContentIdentifierCodec::toString(cid);
    ASSERT_TRUE(cid_string);
    const auto digest = cid.content_address.getHash();
    static const char* kHex = "0123456789abcdef";
    const std::string fanout{
        kHex[digest[0] >> 4], kHex[digest[0] & 0x0F]};
    const std::string key = "v1/blocks/raw/" + fanout + "/"
        + cid_string.value();
    const std::string wrong = "unverified bytes pass through";
    const std::vector<uint8_t> wrong_bytes(wrong.begin(), wrong.end());
    ASSERT_TRUE(fake_->Put(key, wrong_bytes));

    sgns::blockstore::ReadOptions verify_off;
    verify_off.verify = false;
    auto got = store_->Get(cid, verify_off);
    ASSERT_TRUE(got);
    EXPECT_EQ(got.value().bytes, wrong_bytes);
}

/**
 * @given a stored block and a never-put CID
 * @when GetSize measures both, and Has probes the never-put CID
 * @then GetSize hit == payload size (HEAD content-length - no bytes
 *        returned, no verification applies); GetSize miss == NotFound
 *        FAILURE; Has miss == success false (Head authoritative
 *        absence).
 */
TEST_F(S3BlockStoreTest, GetSizeAndHasHeadMapping)
{
    using sgns::blockstore::conformance::MakeCid;
    using sgns::blockstore::conformance::SpanOf;
    const std::string payload = "head mapping probe";
    const auto cid = MakeCid(payload);
    ASSERT_TRUE(store_->Put(cid, SpanOf(payload)));

    auto size = store_->GetSize(cid);
    ASSERT_TRUE(size);
    EXPECT_EQ(size.value(),
              static_cast<uint64_t>(payload.size()));

    const auto missing = MakeCid("head mapping never-put cid");
    auto size_missing = store_->GetSize(missing);
    ASSERT_FALSE(size_missing);
    EXPECT_EQ(size_missing.error().code,
              sgns::blockstore::StatusClass::NotFound);

    auto has_missing = store_->Has(missing);
    ASSERT_TRUE(has_missing);
    EXPECT_FALSE(has_missing.value());
}

/**
 * @given SetFault(S3Op::Head, Unavailable) scripted at the fake
 * @when Has and GetSize run
 * @then both return TemporarilyUnavailable FAILURES - taxonomy honesty:
 *        never the value false, never NotFound (an unobtainable answer
 *        is not an absence).
 */
TEST_F(S3BlockStoreTest, HeadFaultIsTemporarilyUnavailableNeverAbsent)
{
    using sgns::blockstore::conformance::MakeCid;
    const auto cid = MakeCid("head fault probe");
    fake_->SetFault(sgns::blockstore::S3Op::Head,
                    sgns::blockstore::S3FaultKind::Unavailable);

    auto has = store_->Has(cid);
    ASSERT_FALSE(has.has_value());
    EXPECT_EQ(has.error().code,
              sgns::blockstore::StatusClass::TemporarilyUnavailable);

    auto size = store_->GetSize(cid);
    ASSERT_FALSE(size.has_value());
    EXPECT_EQ(size.error().code,
              sgns::blockstore::StatusClass::TemporarilyUnavailable);
}

/**
 * @given Put faults scripted at the fake, then cleared
 * @when Put runs under each fault, then clean
 * @then Unauthorized and TemporarilyUnavailable propagate with their
 *        classes intact, and the clean re-Put succeeds - no sticky
 *        negative caching at the store layer.
 */
TEST_F(S3BlockStoreTest, PutFaultPropagationAndNoStickyFailure)
{
    using sgns::blockstore::conformance::MakeCid;
    using sgns::blockstore::conformance::SpanOf;
    const std::string payload = "put fault propagation probe";
    const auto cid = MakeCid(payload);

    fake_->SetFault(sgns::blockstore::S3Op::Put,
                    sgns::blockstore::S3FaultKind::Unauthorized);
    auto unauthorized = store_->Put(cid, SpanOf(payload));
    ASSERT_FALSE(unauthorized);
    EXPECT_EQ(unauthorized.error().code,
              sgns::blockstore::StatusClass::Unauthorized);

    fake_->SetFault(sgns::blockstore::S3Op::Put,
                    sgns::blockstore::S3FaultKind::Unavailable);
    auto unavailable = store_->Put(cid, SpanOf(payload));
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().code,
              sgns::blockstore::StatusClass::TemporarilyUnavailable);

    fake_->ClearFaults();
    auto clean = store_->Put(cid, SpanOf(payload));
    ASSERT_TRUE(clean);
    EXPECT_FALSE(clean.value().alreadyPresent);
}

/**
 * @given the store's static capability surface
 * @when Capabilities() is read
 * @then maxDurability == ObjectStoreAccepted - the maximum proven
 *        level; no durable-level claim exists anywhere on the surface
 *        (a store never reports an achieved level it has not verified).
 */
TEST_F(S3BlockStoreTest, CapabilitiesAdvertiseAcceptedMaximum)
{
    const auto caps = store_->Capabilities();
    EXPECT_EQ(caps.maxDurability,
              sgns::blockstore::DurabilityLevel::ObjectStoreAccepted);
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
