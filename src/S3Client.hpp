/**
 * @file S3Client.hpp
 * @brief INTERNAL S3 adapter seam - object-level ops behind the BlockStore
 *        facade (BACK-05, decisions D-01/D-03/D-04).
 *
 * NOT INSTALLED (D-04): this header lives in src/ and is deliberately
 * absent from every install rule. It is promoted to an installed header
 * only with Phase 4's S3BlockStore - the first public consumer. Until
 * then the installed surface of sgns-blockstore is unchanged.
 *
 * Design constraints locked by CONTEXT (Phase 3):
 *  - D-01: ONE Put op; conditional create-only is the PutObjectOptions
 *    createOnly flag. No HTTP-isms cross this seam - no header names, no
 *    status codes, no wire types. The adapter implementation translates.
 *  - D-02: "object already exists" (a conditional-create hit) reports
 *    through the SUCCESS slot with alreadyPresent=true - mirroring the
 *    frozen Phase-1 PutOutcome payload shape so Phase-4 maps 1:1.
 *  - D-03: Delete IS an op here (future tooling/audit consumers), but it
 *    NEVER climbs to the public BlockStore surface.
 *  - Threading (Phase-3 contract): the seam is SYNCHRONOUS and BLOCKING.
 *    No S3Client implementation starts background threads in Phase 3;
 *    concurrent-call guarantees are explicitly out of scope and are
 *    revisited with Phase 4's real call sites.
 *
 * All op results use the library-local result alias from Cid.hpp
 * (policy::terminate - guard with has_value()/if (!res) before
 * .value()/.error(); Phase-1 landmine) with the frozen BlockStatus
 * taxonomy. Zero SDK types appear in this header (SC2/BACK-06).
 */

#ifndef SGNS_BLOCKSTORE_S3CLIENT_HPP
#define SGNS_BLOCKSTORE_S3CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <sgns-blockstore/BlockStatus.hpp>
#include <sgns-blockstore/Cid.hpp>

namespace sgns::blockstore
{
    /**
     * @brief Construction-time configuration for every S3Client
     *        implementation (the fifth "op group" of the seam).
     *
     * The only path by which transport posture is set (threat T-3-02):
     * `tlsVerify` defaults to TRUE - a default-off verification would
     * silently downgrade every future consumer; tests may turn it off
     * explicitly for loopback fixtures, the default never does.
     */
    struct S3ClientConfig
    {
        std::string endpoint;         ///< host, e.g. "127.0.0.1"
        uint16_t port = 0;            ///< 0 = scheme default
        /// @brief Pinned region. Empty regions trigger a bucket-location
        ///        roundtrip before the first op on some SDKs - pin it.
        std::string region = "us-east-1";
        bool useTls = false;          ///< plain HTTP is the test-primary lane
        bool tlsVerify = true;        ///< NEVER default off (T-3-02)
        bool pathStyle = true;        ///< path-style addressing (no vhost)
        std::string accessKey;        ///< credential - never logged
        std::string secretKey;        ///< credential - never logged
        std::string bucket;           ///< target bucket (pre-provisioned)
    };

    /**
     * @brief Options for the Put op (D-01: one Put, the flag carries the
     *        conditional-create precondition; no HTTP field names).
     */
    struct PutObjectOptions
    {
        bool createOnly = false; ///< fail-with-alreadyPresent when the key exists
    };

    /**
     * @brief Success payload of Put - the D-02 mirror of the frozen
     *        PutOutcome two-field shape.
     */
    struct S3PutResult
    {
        bool achieved;      ///< the object is stored (or was already)
        bool alreadyPresent; ///< true when the key pre-existed this Put
    };

    /// @brief A fetched object: its key and full bytes.
    struct S3Object
    {
        std::string key;              ///< the requested key
        std::vector<uint8_t> bytes;   ///< the stored bytes
    };

    /**
     * @brief Success payload of Head - authoritative absence reports
     *        through the SUCCESS slot as exists=false, never as NotFound
     *        (an error there means the answer could not be obtained).
     */
    struct S3HeadResult
    {
        bool exists = false;  ///< authoritative existence answer
        uint64_t size = 0;    ///< stored size when exists
    };

    /// @brief The op kinds addressable by the fake's fault table (D-05).
    enum class S3Op
    {
        Put,    ///< the Put op
        Get,    ///< the Get op
        Head,   ///< the Head op
        Delete, ///< the Delete op
    };

    /// @brief Scriptable fault kinds (mirrors FaultController's enum).
    enum class S3FaultKind
    {
        None,         ///< normal operation
        Unavailable,  ///< op fails TemporarilyUnavailable (never NotFound)
        CorruptData,  ///< op fails Corrupt
        Unauthorized, ///< op fails Unauthorized
    };

    /**
     * @brief The internal S3 adapter interface (D-01/D-03/D-04).
     *
     * Implementations: InMemoryS3Client (hermetic fake, this phase) and
     * MinioS3Client (minio-cpp backing, 03-03). Delete is internal-only -
     * guard tests keep the public BlockStore delete-free.
     */
    class S3Client
    {
    public:
        virtual ~S3Client() = default;

        /**
         * @brief Store bytes under a key, optionally create-only.
         * @param key the object key
         * @param bytes the payload
         * @param options createOnly carries the conditional precondition
         * @return S3PutResult; a createOnly hit on an existing key is the
         *         SUCCESS slot with alreadyPresent=true (D-02)
         */
        virtual result<S3PutResult, BlockStatus> Put(
            const std::string& key,
            const std::vector<uint8_t>& bytes,
            const PutObjectOptions& options = {}) = 0;

        /**
         * @brief Fetch an object's bytes.
         * @return S3Object on hit; authoritative miss is the ERROR slot
         *         NotFound (BlockSource::Remote)
         */
        virtual result<S3Object, BlockStatus> Get(const std::string& key) = 0;

        /**
         * @brief Probe existence and size without transferring bytes.
         * @return S3HeadResult in the SUCCESS slot for both hit and
         *         authoritative miss (exists=false) - errors mean the
         *         answer could not be obtained
         */
        virtual result<S3HeadResult, BlockStatus> Head(const std::string& key) = 0;

        /**
         * @brief Delete an object (internal op, D-03 - never on BlockStore).
         * @return void success; idempotent - deleting a missing key
         *         succeeds, matching S3 RemoveObject semantics
         */
        virtual result<void, BlockStatus> Delete(const std::string& key) = 0;
    };
} // namespace sgns::blockstore

#endif // SGNS_BLOCKSTORE_S3CLIENT_HPP
