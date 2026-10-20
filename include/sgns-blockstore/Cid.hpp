/**
 * @file Cid.hpp
 * @brief Public CID type alias and the one verified CID constructor of the
 *        sgns-blockstore library.
 */

#ifndef SGNS_BLOCKSTORE_CID_HPP
#define SGNS_BLOCKSTORE_CID_HPP

#include <gsl/span>
#include <libp2p/multi/content_identifier.hpp>
#include <libp2p/multi/multicodec_type.hpp>
#include <libp2p/outcome/outcome.hpp>

namespace sgns::blockstore
{
    /**
     * @brief The outcome envelope used across the library (decision D-01).
     *
     * Alias of libp2p's vendored outcome namespace: outcome ships with
     * p2p_cid, so consumers already carry it at no extra dependency cost.
     */
    namespace outcome = libp2p::outcome;

    /**
     * @brief The one CID definition used across the library and its consumers.
     *
     * Direct alias of the libp2p CID (decision D-05): one CID type everywhere,
     * zero conversion friction for SuperGenius CRDT call sites. The library
     * links p2p_cid and re-exports it through its CMake package config.
     */
    using Cid = libp2p::multi::ContentIdentifier;

    /**
     * @brief Construct the verified CID for a block payload.
     *
     * The single verified CID constructor future consumers (the SuperGenius
     * CRDT adapter, genius-tube) use instead of hand-rolling multihash
     * encoding (decision D-07). Composed exclusively of libp2p primitives -
     * sha256 -> Multihash::create -> the 3-arg ContentIdentifier constructor -
     * with zero hand-rolled varint/multibase/multihash code.
     *
     * @param bytes the block payload to hash
     * @param codec the content codec the CID records (e.g. RAW, DAG_PB)
     * @return the CIDv1 carrying a sha-256 multihash, or error
     *
     * @note v1 hashes with sha-256 multihash ONLY (decision D-08): CidOf never
     *       hashes with anything but sha-256, and any other hash code is
     *       rejected at API entry as an invalid argument / programmer error.
     *       Boundary: the repo's CRDT CBOR path (sgns::common::getCidOf)
     *       produces blake2b-256 CIDs, which this v1 API therefore does NOT
     *       accept. Widening to more hash functions later is additive.
     */
    outcome::result<Cid> CidOf(gsl::span<const uint8_t> bytes,
                               libp2p::multi::MulticodecType::Code codec);
}

#endif  // SGNS_BLOCKSTORE_CID_HPP
