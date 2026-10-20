/**
 * @file Cid.cpp
 * @brief Implementation of the verified CID constructor (D-07/D-08).
 */

#include <sgns-blockstore/Cid.hpp>

#include <libp2p/crypto/sha/sha256.hpp>
#include <libp2p/multi/multihash.hpp>
#include <libp2p/outcome/outcome.hpp>

namespace sgns::blockstore
{
    outcome::result<Cid> CidOf(gsl::span<const uint8_t> bytes,
                               libp2p::multi::MulticodecType::Code codec)
    {
        // libp2p::crypto::sha256 returns outcome::result<libp2p::common::Hash256>,
        // where Hash256 = std::array<uint8_t, 32>. Note: the vendored
        // OUTCOME_TRY wrapper injects `auto` itself - pass the bare name.
        OUTCOME_TRY(digest, libp2p::crypto::sha256(bytes));
        OUTCOME_TRY(hash,
                    libp2p::multi::Multihash::create(
                        libp2p::multi::HashType::sha256,
                        gsl::span<const uint8_t>(digest.data(), digest.size())));
        return Cid(Cid::Version::V1, codec, std::move(hash));
    }
}
