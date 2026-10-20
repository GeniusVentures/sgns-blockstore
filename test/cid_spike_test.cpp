/*
 * C1 CID-dependency spike - the Phase-1 entrance criterion (decision D-06).
 *
 * Part 3 assertion bar, pre-refined per research Pitfall 1 and stated here
 * verbatim so the bar is not discovered mid-spike:
 *   (a) this TU compiles and links against p2p::p2p_cid alone;
 *   (b) no ipfs-lite-cpp target appears on its link line and no ipfs_lite
 *       headers are included anywhere in this repo (enforced repo-wide by
 *       the dependency-shaped grep in the plan's acceptance gate);
 *   (c) no DIRECT fmt or c-ares edge on p2p_cid itself;
 *   (d) the transitive p2p_cid -> p2p_uvarint -> p2p_logger ->
 *       soralog/yaml-cpp/Boost.DI edge is ACCEPTED libp2p-family overhead -
 *       identical to what every libp2p consumer already carries, and
 *       strictly lighter than the ipfs-lite-cpp granular cid fallback,
 *       which links c-ares/fmt/blake2 directly. Do not attempt to fix
 *       libp2p.
 */

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <vector>

#include <sgns-blockstore/Cid.hpp>

#include <libp2p/crypto/sha/sha256.hpp>
#include <libp2p/multi/content_identifier_codec.hpp>
#include <libp2p/multi/multihash.hpp>

namespace
{
    using sgns::blockstore::Cid;
    using libp2p::multi::ContentIdentifier;
    using libp2p::multi::ContentIdentifierCodec;
    using libp2p::multi::HashType;
    using libp2p::multi::MulticodecType;
    using libp2p::multi::Multihash;

    const std::string kPayload = "hello world";

    gsl::span<const uint8_t> PayloadBytes()
    {
        return gsl::span<const uint8_t>(
            reinterpret_cast<const uint8_t *>(kPayload.data()), kPayload.size());
    }
}

/**
 * @given the byte payload "hello world"
 * @when a CIDv1 is built via CidOf and pushed through string and wire
 *        round-trips, and a CIDv0 is built via encodeCIDV0
 * @then every round-trip is lossless and the known sha-256 CIDv1 RAW vector
 *        is reproduced exactly
 */
TEST(CidSpikeTest, Part1RoundTripCidV0AndV1)
{
    // --- CIDv1 path (base32 string form) ---
    auto cid_result = sgns::blockstore::CidOf(PayloadBytes(),
                                              MulticodecType::Code::RAW);
    ASSERT_TRUE(cid_result);
    const auto cid = cid_result.value();

    auto str_result = ContentIdentifierCodec::toString(cid);
    ASSERT_TRUE(str_result);
    // Known vector: sha256("hello world") CIDv1 raw.
    // Regenerated and cross-checked during the spike against an independent
    // from-scratch base32 computation of 0x01 0x55 0x12 0x20 || sha256
    // digest: the research-doc constant had a transcription error at
    // position 33 ('w' instead of 'y') - the runtime codec and the
    // independent computation agree; details recorded in the plan SUMMARY.
    EXPECT_EQ(str_result.value(),
              "bafkreifzjut3te2nhyekklss27nh3k72ysco7y32koao5eei66wof36n5e");

    auto from_str = ContentIdentifierCodec::fromString(str_result.value());
    ASSERT_TRUE(from_str);
    EXPECT_TRUE(from_str.value() == cid);

    auto wire = ContentIdentifierCodec::encode(cid);
    ASSERT_TRUE(wire);
    auto from_wire = ContentIdentifierCodec::decode(wire.value());
    ASSERT_TRUE(from_wire);
    EXPECT_TRUE(from_wire.value() == cid);

    // --- CIDv0 path (encodeCIDV0 hashes sha-256 internally) ---
    const auto v0 = ContentIdentifierCodec::encodeCIDV0(kPayload.data(),
                                                        kPayload.size());
    ASSERT_EQ(v0.size(), 34u);
    EXPECT_EQ(v0[0], 0x12);
    EXPECT_EQ(v0[1], 0x20);

    auto v0_cid = ContentIdentifierCodec::decode(v0);
    ASSERT_TRUE(v0_cid);
    auto v0_reencoded = ContentIdentifierCodec::encode(v0_cid.value());
    ASSERT_TRUE(v0_reencoded);
    EXPECT_EQ(v0_reencoded.value(), v0);

    auto v0_str = ContentIdentifierCodec::toString(v0_cid.value());
    ASSERT_TRUE(v0_str);
    EXPECT_EQ(v0_str.value().substr(0, 2), "Qm");
}

/**
 * @given the byte payload "hello world" and its sha-256 digest
 * @when CidOf output is compared against a CID manually assembled from
 *        Multihash::create + the 3-arg ContentIdentifier constructor
 * @then CidOf matches the manual construction for RAW and DAG_PB, the
 *        digest is 32 bytes, the version is V1, and the multihash type is
 *        sha256 in every output (D-08)
 */
TEST(CidSpikeTest, Part2CidOfKnownVectors)
{
    auto digest_result = libp2p::crypto::sha256(PayloadBytes());
    ASSERT_TRUE(digest_result);
    const auto digest = digest_result.value();
    ASSERT_EQ(digest.size(), 32u);

    auto hash_result = Multihash::create(
        HashType::sha256,
        gsl::span<const uint8_t>(digest.data(), digest.size()));
    ASSERT_TRUE(hash_result);

    // RAW codec
    const ContentIdentifier manual_raw(ContentIdentifier::Version::V1,
                                       MulticodecType::Code::RAW,
                                       hash_result.value());
    auto raw_result = sgns::blockstore::CidOf(PayloadBytes(),
                                              MulticodecType::Code::RAW);
    ASSERT_TRUE(raw_result);
    EXPECT_TRUE(raw_result.value() == manual_raw);
    EXPECT_EQ(raw_result.value().version, ContentIdentifier::Version::V1);
    EXPECT_EQ(raw_result.value().content_address.getType(), HashType::sha256);

    // DAG_PB codec - CidOf is total over the accepted codecs
    const ContentIdentifier manual_pb(ContentIdentifier::Version::V1,
                                      MulticodecType::Code::DAG_PB,
                                      hash_result.value());
    auto pb_result = sgns::blockstore::CidOf(PayloadBytes(),
                                             MulticodecType::Code::DAG_PB);
    ASSERT_TRUE(pb_result);
    EXPECT_TRUE(pb_result.value() == manual_pb);
    EXPECT_EQ(pb_result.value().version, ContentIdentifier::Version::V1);
    EXPECT_EQ(pb_result.value().content_address.getType(), HashType::sha256);
}

/**
 * @given this TU as the standalone consumer: its only link line is
 *        sgns-blockstore + p2p::p2p_cid (see test/CMakeLists.txt) and its
 *        library include set is the public Cid.hpp plus the libp2p headers
 *        that family drags
 * @when the alias and the CidOf surface are exercised
 * @then the public CID type IS the libp2p alias (D-05), CidOf is reachable
 *        through the public header alone, and every output is sha-256
 *        multihash CIDv1 - the Part-3 link bar holds by construction
 */
TEST(CidSpikeTest, Part3StandaloneConsumerLinkBar)
{
    static_assert(
        std::is_same<Cid, libp2p::multi::ContentIdentifier>::value,
        "D-05: the public CID type must be the direct libp2p alias");
    SUCCEED() << "D-05 alias holds: sgns::blockstore::Cid IS "
                 "libp2p::multi::ContentIdentifier";

    auto cid_result = sgns::blockstore::CidOf(PayloadBytes(),
                                              MulticodecType::Code::RAW);
    ASSERT_TRUE(cid_result);
    EXPECT_EQ(cid_result.value().version, ContentIdentifier::Version::V1);
    EXPECT_EQ(cid_result.value().content_address.getType(), HashType::sha256);
}
