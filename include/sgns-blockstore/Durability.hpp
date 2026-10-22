/**
 * @file Durability.hpp
 * @brief Durability acknowledgment levels and the Put result payload
 *        (D-04/D-13, DurabilityLevel verbatim from the architecture doc).
 */

#ifndef SGNS_BLOCKSTORE_DURABILITY_HPP
#define SGNS_BLOCKSTORE_DURABILITY_HPP

namespace sgns::blockstore
{
    /**
     * @brief The durability acknowledgment levels a store can report.
     *
     * Verbatim from supergenius-sharded-ipld-cache-architecture.md. The
     * distinction between ObjectStoreAccepted and ObjectStoreDurable
     * matters for Sia s3d: an acknowledged upload may initially reside in
     * durable local staging before background transfer. `PeerReplicated`
     * is the placeholder anchoring future eviction work. For the pinned
     * Sia s3d backend (v0.2.1), the staged-vs-durable ceiling, the flush
     * primitive, and the absence of any per-object durability signal
     * are characterized with committed evidence in docs/s3d-durability.md.
     */
    enum class DurabilityLevel
    {
        LocalCommitted,      ///< bytes are in locally-committed storage
        PeerReplicated,     ///< bytes are replicated across peers (v1 placeholder)
        /**
         * @brief The object store acknowledged receipt - durability NOT
         *        yet proven.
         *
         * For Sia s3d this means the object was accepted into local
         * staging (it may already be readable); it does NOT mean the
         * object is durable on the Sia network. The v0.2.1 ceiling, the
         * blocking flush primitive, and the no-per-object-signal
         * finding are characterized in docs/s3d-durability.md
         * (evidence-cited; see docs/evidence/s3d-spike/).
         */
        ObjectStoreAccepted, ///< object store acknowledged receipt (not yet proven durable)
        ObjectStoreDurable,  ///< object store has proven durable storage
    };

    /**
     * @brief The success payload of Put/PutMany elements (D-04).
     *
     * @note A store NEVER reports an `achieved` level it has not verified.
     */
    struct PutOutcome
    {
        DurabilityLevel achieved; ///< the verified durability level of the write
        bool alreadyPresent;      ///< true when the block was already stored (identical re-put)
    };
}

#endif  // SGNS_BLOCKSTORE_DURABILITY_HPP
