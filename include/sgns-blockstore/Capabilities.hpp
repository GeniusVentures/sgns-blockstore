/**
 * @file Capabilities.hpp
 * @brief Static capability reporting for backend stores.
 */

#ifndef SGNS_BLOCKSTORE_CAPABILITIES_HPP
#define SGNS_BLOCKSTORE_CAPABILITIES_HPP

#include <sgns-blockstore/Durability.hpp>

namespace sgns::blockstore
{
    /**
     * @brief What a backend store can promise.
     *
     * Minimal v1 field set (planner discretion: nothing speculative beyond
     * maximum verified-durability reporting).
     */
    struct BlockStoreCapabilities
    {
        DurabilityLevel maxDurability; ///< highest durability level this backend can verify
    };
}

#endif  // SGNS_BLOCKSTORE_CAPABILITIES_HPP
