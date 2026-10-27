/**
 * @file S3AdminClient.hpp
 * @brief The s3d admin API facade - flush + upload-stats evidence behind
 *        a BlockStatus seam (DUR-01, Phase 10 decisions D-05/D-06).
 *
 * INSTALLED beside the S3Client seam: SuperGenius's gated
 * ColdStorageManager constructs it via OpenS3dAdminClient (through
 * ColdStorageStack.cpp), and the implementation header
 * (InMemoryS3AdminClient.hpp) stays in src/ - only the interface header
 * is installed (S3Client.hpp promotion precedent).
 *
 * Design constraints locked by 10-CONTEXT / 10-RESEARCH:
 *  - D-06 (separate branch): the s3d admin API is a SEPARATE
 *    always-enabled listener (default 127.0.0.1:8001) authenticated by
 *    HTTP Basic with a configured ADMIN password (username ignored,
 *    empty by convention) - NOT the S3 SigV4 credentials. The admin
 *    password resolves ONLY from the environment
 *    (SGNS_BLOCKSTORE_S3_ADMIN_PASSWORD), never from config at rest,
 *    and is never logged (redaction by omission).
 *  - Flush is PROCESS-GLOBAL at the pinned s3d tag: POST /objects/flush
 *    takes NO bucket parameter and blocks until done; the
 *    pendingObjects==0 evidence from GET /stats/uploads is therefore
 *    process-wide. Shared-s3d deployments get slower advancement, never
 *    wrong advancement.
 *  - s3d has NO server-side deadline ("Blocks until done") - the caller
 *    MUST bound every call with a client-side read budget (Pitfall 4):
 *    Flush takes an EXPLICIT per-call budget (no default argument on a
 *    virtual) so the cadence tick and the shutdown final flush can pass
 *    different budgets.
 *  - Failure taxonomy (RESEARCH 1.5): EVERY failure - transport error,
 *    timeout, non-200 (including 500), non-JSON body, or a stats body
 *    missing/mistyping a required field - classifies
 *    TemporarilyUnavailable, NEVER NotFound. The admin API is not an
 *    authority on object absence; a conservative failure keeps records
 *    in their previous journal state (fail-safe advancement).
 *
 * All op results use the library-local result alias from Cid.hpp
 * (policy::terminate - guard with has_value()/if (!res) before
 * .value()/.error()) with the frozen BlockStatus taxonomy. Zero SDK
 * types appear in this header (cpp-httplib and nlohmann_json stay
 * PRIVATE in the library build).
 */

#ifndef SGNS_BLOCKSTORE_S3ADMINCLIENT_HPP
#define SGNS_BLOCKSTORE_S3ADMINCLIENT_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <sgns-blockstore/BlockStatus.hpp>
#include <sgns-blockstore/Cid.hpp>

namespace sgns::blockstore
{
    /**
     * @brief The /stats/uploads counters the durability evidence reads
     *        (verbatim s3d openapi field names; all uint64).
     *
     * pendingObjects is THE evidence field: a flush cycle may mark its
     * pre-flush UploadedAccepted snapshot Durable only after observing
     * pendingObjects == 0. activeUploads is deliberately OMITTED - it is
     * transient in-flight progress, never a per-object durability signal.
     */
    struct UploadStats
    {
        uint64_t pendingObjects   = 0; ///< objects staged but not yet flushed
        uint64_t pendingSize      = 0; ///< bytes staged but not yet flushed
        uint64_t uploadedObjects  = 0; ///< objects flushed to the backing store
        uint64_t uploadedSize     = 0; ///< bytes flushed to the backing store
        uint64_t unpinnedObjects  = 0; ///< uploaded objects no longer pinned
        uint64_t failedUploads    = 0; ///< uploads that failed permanently
        uint64_t orphanedObjects  = 0; ///< uploaded objects with no journal record
        uint64_t multipartUploads = 0; ///< in-flight multipart uploads
        uint64_t bufferUsed       = 0; ///< staging buffer bytes in use
        uint64_t bufferLimit      = 0; ///< staging buffer capacity (0 = unbounded)
        uint64_t ingressActive    = 0; ///< active S3-API ingress transfers
        uint64_t ingressBytes     = 0; ///< total S3-API ingress bytes
        uint64_t ingressRate      = 0; ///< S3-API ingress bytes/sec (windowed)
        uint64_t uploadBytes      = 0; ///< total staging-to-store upload bytes
        uint64_t uploadRate       = 0; ///< staging-to-store bytes/sec (windowed)
    };

    /**
     * @brief Construction-time configuration for every S3AdminClient
     *        implementation.
     *
     * The endpoint URL is connection topology, not a secret - it lives in
     * config (blockstore.admin_endpoint, empty = the documented s3d
     * default "http://127.0.0.1:8001"). adminPassword is a credential:
     * env-only resolution, never logged, never at rest (T-10-01).
     * tlsVerify defaults TRUE (S3ClientConfig T-3-02 posture); loopback
     * plain-HTTP lanes opt out explicitly.
     */
    struct S3AdminClientConfig
    {
        std::string endpoint;              ///< base URL, e.g. "http://127.0.0.1:8001"
        std::string adminPassword;         ///< credential - never logged
        bool        tlsVerify = true;      ///< NEVER default off (T-3-02 posture)
        uint32_t    stats_read_timeout_ms = 5000; ///< GetUploadStats read budget
    };

    /**
     * @brief Scriptable fault kinds for the admin fake (S3FaultKind
     *        precedent - installed beside the seam so SuperGenius tests
     *        can name it).
     */
    enum class S3AdminFaultKind
    {
        None,          ///< normal operation
        Unreachable,   ///< both ops fail TemporarilyUnavailable (transport)
        FlushFails,    ///< Flush fails TemporarilyUnavailable (non-200)
        GarbageStats,  ///< GetUploadStats fails TemporarilyUnavailable (parse)
        SlowFlush,     ///< Flush succeeds after a scripted delay (budget probe)
    };

    /**
     * @brief The s3d admin API facade: flush the staging pipeline and
     *        read the upload-stats evidence (D-05).
     *
     * Implementations: InMemoryS3AdminClient (hermetic fake, src/) and
     * the cpp-httplib client built by OpenS3dAdminClient (src/
     * S3AdminClient.cpp). Synchronous and blocking like the S3Client
     * seam; concurrent-call guarantees are out of scope (single drainer
     * thread + one shutdown final-flush caller, serialized by the
     * manager's lifecycle).
     */
    class S3AdminClient
    {
    public:
        virtual ~S3AdminClient() = default;

        /**
         * @brief POST /objects/flush - flush every staged object into the
         *        backing store (process-global; blocks until done).
         * @param read_timeout_budget client-side read budget for THIS
         *        call - s3d has no server-side deadline, so this is the
         *        only bound on the call (Pitfall 4)
         * @return void success; every failure (transport, timeout,
         *         non-200 including 500) is TemporarilyUnavailable
         */
        virtual result<void, BlockStatus> Flush(
            std::chrono::milliseconds read_timeout_budget ) = 0;

        /**
         * @brief GET /stats/uploads - read the staging counters,
         *        including the pendingObjects durability evidence.
         * @return UploadStats on success; transport/timeout/non-200/
         *         non-JSON/missing-or-mistyped-field failures are all
         *         TemporarilyUnavailable (strict parse - unknown extra
         *         fields are tolerated)
         */
        virtual result<UploadStats, BlockStatus> GetUploadStats() = 0;
    };

    /**
     * @brief Construct the production cpp-httplib-backed s3d admin
     *        client behind the SDK-free facade (D-05).
     *
     * The only way an out-of-tree consumer obtains the real client; the
     * implementation stays internal to the library (httplib/nlohmann are
     * PRIVATE links - zero SDK types in this header).
     *
     * @param config endpoint/adminPassword/timeouts. adminPassword is a
     *        credential this library NEVER logs or prints
     * @return the constructed client, or a BlockStatus runtime failure
     * @throws std::invalid_argument when endpoint is empty - a
     *         programmer error (S3Client factory contract)
     *
     * @note eager config validation, ZERO network I/O at construction -
     *       the client connects lazily per operation (D-06 contract
     *       shape of OpenMinioS3Client).
     */
    result<std::shared_ptr<S3AdminClient>, BlockStatus> OpenS3dAdminClient(
        const S3AdminClientConfig& config);
} // namespace sgns::blockstore

#endif // SGNS_BLOCKSTORE_S3ADMINCLIENT_HPP
