/**
 * @file FaultController.hpp
 * @brief The fault-injection seam probed by the conformance kit (D-10).
 */

#ifndef SGNS_BLOCKSTORE_FAULTCONTROLLER_HPP
#define SGNS_BLOCKSTORE_FAULTCONTROLLER_HPP

namespace sgns::blockstore
{
    /**
     * @brief The fault kinds a backend can inject for conformance testing.
     */
    enum class FaultKind
    {
        None,         ///< normal operation
        Unavailable,  ///< all ops fail TemporarilyUnavailable (never NotFound, never false)
        CorruptData,  ///< Get returns bytes that fail CID verification
        Unauthorized, ///< all ops fail Unauthorized (auth/permission failure)
    };

    /**
     * @brief The fault-injection seam of the conformance kit (D-10).
     *
     * Backends that publicly implement this interface get fault conformance
     * tests (induced failure -> correct taxonomy class, no sticky negative
     * cache) for free from the BlockStoreConformanceTests kit; backends
     * without it get loud labelled skips.
     *
     * @note This is a CONFORMANCE-TEST SEAM. Production code MUST NOT call
     *       SetFault/GetFault - they exist so the kit can induce the
     *       failure taxonomy classes deterministically.
     * @note The header is installed public deliberately (planner
     *       resolution): backends publicly inherit FaultController so
     *       std::dynamic_pointer_cast<FaultController>(store) probing
     *       works from the kit's separately-compiled test TUs. The
     *       research's test-dir placement cannot support that cast.
     * @note `Unauthorized` simulates a backing-store auth/permission
     *       failure so the taxonomy's fourth failure class is
     *       conformance-provable in v1; Phase 5 later induces the same
     *       class over real transports.
     */
    class FaultController
    {
    public:
        virtual ~FaultController() = default;

        /**
         * @brief Set the injected fault kind.
         * @param kind the fault to induce (None clears)
         */
        virtual void SetFault(FaultKind kind) = 0;

        /**
         * @brief Read the currently injected fault kind.
         * @return the active FaultKind
         */
        virtual FaultKind GetFault() const = 0;
    };
}

#endif  // SGNS_BLOCKSTORE_FAULTCONTROLLER_HPP
