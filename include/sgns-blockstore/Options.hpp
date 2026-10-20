/**
 * @file Options.hpp
 * @brief Per-operation options (D-15, verbatim field sets).
 */

#ifndef SGNS_BLOCKSTORE_OPTIONS_HPP
#define SGNS_BLOCKSTORE_OPTIONS_HPP

#include <chrono>

namespace sgns::blockstore
{
    /**
     * @brief Options for read-path operations (Get).
     *
     * D-15 verbatim: deadline plus the CORE-04 verify toggle, nothing
     * speculative.
     *
     * @note `verify` defaults to true and CID verification can never be
     *       compiled out - no build configuration (e.g. NDEBUG) may
     *       disable the facade verification path (CORE-04 prohibition).
     * @note Backends that never block (MemoryBlockStore) ignore
     *       `deadline`; backends that can honor it do.
     */
    struct ReadOptions
    {
        std::chrono::steady_clock::time_point deadline; ///< abort waiting after this point
        bool verify = true; ///< re-verify returned bytes against the CID (default on)
    };

    /**
     * @brief Options for write-path operations (Put/PutMany).
     *
     * D-15 verbatim: deadline only.
     *
     * @note Backends that never block (MemoryBlockStore) ignore
     *       `deadline`.
     */
    struct WriteOptions
    {
        std::chrono::steady_clock::time_point deadline; ///< abort waiting after this point
    };
}

#endif  // SGNS_BLOCKSTORE_OPTIONS_HPP
