/**
 * @file Block.hpp
 * @brief The block payload types crossing the BlockStore API.
 */

#ifndef SGNS_BLOCKSTORE_BLOCK_HPP
#define SGNS_BLOCKSTORE_BLOCK_HPP

#include <gsl/span>
#include <vector>

#include <sgns-blockstore/Cid.hpp>

namespace sgns::blockstore
{
    /**
     * @brief A retrieved block: its CID and verified byte payload.
     */
    struct Block
    {
        Cid cid;                 ///< the content identifier the bytes verified against
        std::vector<uint8_t> bytes; ///< the block payload
    };

    /**
     * @brief A Put/PutMany input element: a CID and a caller-owned byte view.
     *
     * The span is non-owning; callers retain the bytes until the Put call
     * returns. PutMany batches are arrays of these.
     */
    struct BlockInput
    {
        Cid cid;                         ///< the claimed content identifier
        gsl::span<const uint8_t> bytes;  ///< caller-owned payload view
    };
}

#endif  // SGNS_BLOCKSTORE_BLOCK_HPP
