/**
 * @file BlockStatus.hpp
 * @brief The rich failure taxonomy riding the outcome envelope (D-02).
 */

#ifndef SGNS_BLOCKSTORE_BLOCKSTATUS_HPP
#define SGNS_BLOCKSTORE_BLOCKSTATUS_HPP

#include <string>

namespace sgns::blockstore
{
    /**
     * @brief The failure classification for every public BlockStore
     *        operation (CORE-02).
     *
     * Exactly four members. `Found` is NOT among them: a found block is
     * represented by the success slot of the outcome envelope, never by a
     * failure value (D-03). Non-authoritative conditions (transport,
     * outage, timeout) are TemporarilyUnavailable and must NEVER be
     * surfaced as NotFound - an authoritative miss is the only legitimate
     * NotFound.
     */
    enum class StatusClass
    {
        NotFound,               ///< authoritative absence ONLY
        TemporarilyUnavailable, ///< transport error, outage, timeout - never NotFound
        Corrupt,                ///< bytes do not hash to the requested CID
        Unauthorized,           ///< authentication / permission failure
    };

    /**
     * @brief Attribution of a status to the layer that produced it (D-02).
     *
     * The member set is planner-finalized (research discretion Q2):
     * `Caller` attributes Put-path verification failures to the
     * caller-supplied bytes; `Memory`/`Local`/`Remote`/`Peer` attribute
     * Get-path and backend failures to the layer that produced them.
     */
    enum class BlockSource
    {
        Memory,  ///< in-memory backend
        Local,   ///< local persistent tier
        Remote,  ///< remote object-store tier
        Peer,    ///< peer-sourced data
        Caller,  ///< caller-supplied bytes failed verification
        Unknown, ///< source not attributable
    };

    /**
     * @brief The failure payload riding the outcome envelope's error slot.
     *
     * A rich struct, not a bare enum, so source attribution is first-class
     * and assertable by the conformance kit (D-02).
     *
     * @note Convention (research security rule P11): `message` content must
     *       NEVER embed secrets - no credentials, keys, or tokens. Human-
     *       readable context only.
     */
    struct BlockStatus
    {
        StatusClass code;    ///< the failure classification
        BlockSource source;  ///< the layer that produced the failure
        std::string message; ///< human-readable context, never a secret
    };
}

#endif  // SGNS_BLOCKSTORE_BLOCKSTATUS_HPP
