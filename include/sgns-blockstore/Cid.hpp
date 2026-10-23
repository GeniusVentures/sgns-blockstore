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
     * @brief The result envelope for BlockStatus-typed failures (D-01).
     *
     * Deliberate policy deviation from the libp2p default alias:
     * BlockStatus is a plain struct, so boost's default policy resolves to
     * exception_ptr_rethrow observers that cannot instantiate for it (no
     * std::exception_ptr construction exists). policy::terminate keeps
     * .value()/.error() semantics identical for correctly-checked code and
     * fails fast on unchecked access instead.
     */
    template <class R, class S = std::error_code,
              class NoValuePolicy = libp2p::outcome::policy::terminate>
    using result = libp2p::outcome::basic_result<R, S, NoValuePolicy>;

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
     * @note this form delegates to the 3-arg CidOf overload pinned to
     *       sha-256 (Phase-6 widening, D-04); callers needing the blake2b-256
     *       shape every CRDT DAG block carries (the sgns::common::getCidOf
     *       output) must use the 3-arg overload below. Accepted hash types
     *       across the CidOf surface are exactly {sha-256, blake2b-256}
     *       (D-01); any other hash is a programmer error (D-03).
     */
    outcome::result<Cid> CidOf(gsl::span<const uint8_t> bytes,
                               libp2p::multi::MulticodecType::Code codec);

    /**
     * @brief Construct the verified CID for a block payload with an
     *        explicit multihash type.
     *
     * Overload for CID shapes beyond sha-256: composes the payload digest
     * with the requested hash type and codec exactly the way the ecosystem
     * producers do. The blake2b-256 branch mirrors sgns::common::getCidOf
     * (the CID every SuperGenius CRDT DAG block carries) using the same
     * vendored blake2b object code, so CID equality with the real producer
     * holds by construction (Phase-6 widening, D-04; evidenced against
     * getCidOf in docs/evidence/blake2b-crosscheck/).
     *
     * @param bytes the block payload to hash
     * @param codec the content codec the CID records (e.g. RAW, DAG_CBOR)
     * @param hash the multihash type to compose; accepted values are
     *        exactly sha256 and blake2b_256 (D-01)
     * @return the CIDv1 carrying the requested multihash, or error
     *
     * @throws std::invalid_argument when hash is any HashType other than
     *         sha256 or blake2b_256 - an out-of-set hash is a programmer
     *         error, not a runtime failure (D-03)
     */
    outcome::result<Cid> CidOf(gsl::span<const uint8_t> bytes,
                               libp2p::multi::MulticodecType::Code codec,
                               libp2p::multi::HashType hash);
}

#endif  // SGNS_BLOCKSTORE_CID_HPP
