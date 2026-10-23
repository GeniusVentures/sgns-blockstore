/**
 * @file Cid.cpp
 * @brief Implementation of the verified CID constructor (D-07/D-08;
 *        Phase-6 blake2b-256 widening D-04).
 */

#include <sgns-blockstore/Cid.hpp>

#include <stdexcept>

#include <crypto/blake2/blake2b160.hpp>
#include <libp2p/crypto/sha/sha256.hpp>
#include <libp2p/multi/multihash.hpp>
#include <libp2p/outcome/outcome.hpp>

namespace sgns::blockstore
{
    outcome::result<Cid> CidOf(gsl::span<const uint8_t> bytes,
                               libp2p::multi::MulticodecType::Code codec,
                               libp2p::multi::HashType hash)
    {
        switch (hash)
        {
            case libp2p::multi::HashType::sha256:
            {
                // libp2p::crypto::sha256 returns outcome::result<libp2p::common::Hash256>,
                // where Hash256 = std::array<uint8_t, 32>. Note: the vendored
                // OUTCOME_TRY wrapper injects `auto` itself - pass the bare name.
                OUTCOME_TRY(digest, libp2p::crypto::sha256(bytes));
                OUTCOME_TRY(multihash,
                            libp2p::multi::Multihash::create(
                                libp2p::multi::HashType::sha256,
                                gsl::span<const uint8_t>(digest.data(), digest.size())));
                return Cid(Cid::Version::V1, codec, std::move(multihash));
            }
            case libp2p::multi::HashType::blake2b_256:
            {
                // Mirrors sgns::common::getCidOf (primitives/cid/cid.cpp),
                // the composition behind every CRDT DAG block CID:
                // blake2b_256 -> Multihash::create -> the 3-arg
                // ContentIdentifier constructor. Same vendored blake2b
                // object code (ipfs-lite-cpp::blake2), so CID equality
                // with the real producer holds by construction (D-02
                // evidence: docs/evidence/blake2b-crosscheck/).
                const auto digest = sgns::crypto::blake2b::blake2b_256(bytes);
                OUTCOME_TRY(multihash,
                            libp2p::multi::Multihash::create(
                                libp2p::multi::HashType::blake2b_256,
                                gsl::span<const uint8_t>(digest.data(), digest.size())));
                return Cid(Cid::Version::V1, codec, std::move(multihash));
            }
            default:
                // Out-of-set hash is a programmer error, not a runtime
                // failure (D-01/D-03): the accepted set is exactly
                // {sha-256, blake2b-256}.
                throw std::invalid_argument(
                    "sgns::blockstore: CidOf: unsupported HashType - accepted: "
                    "{sha256, blake2b-256} (D-03)");
        }
    }

    outcome::result<Cid> CidOf(gsl::span<const uint8_t> bytes,
                               libp2p::multi::MulticodecType::Code codec)
    {
        // 2-arg form pinned to sha-256: unchanged v1.0 behavior (D-04).
        return CidOf(bytes, codec, libp2p::multi::HashType::sha256);
    }
}
