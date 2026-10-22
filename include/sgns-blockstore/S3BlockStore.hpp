/**
 * @file S3BlockStore.hpp
 * @brief The S3-backed terminal cold-tier BlockStore (BACK-07) - a thin
 *        facade over the installed S3Client adapter seam (03 D-04).
 *
 * Composition (Phase 4, D-04 promotion's first public consumer): every
 * op derives the object key from the CID, calls exactly one adapter op,
 * and maps the result into the frozen outcome envelope. Raw HTTP never
 * crosses this boundary - the adapter's one-place mapper already owns
 * SDK-failure classification, and this store NEVER re-maps an adapter
 * failure downward to NotFound (CORE-02 CRDT-corruption trap; the
 * default bucket stays TemporarilyUnavailable).
 *
 * Key layout (BACK-07 letter, codec-preserving and fan-out-safe):
 *   <keyPrefix>/<codec>/<xx>/<cid-string>
 *   v1/blocks/raw/b9/bafkreifzjut3te2nhyekklss27nh3k72ysco7y32koao5eei66wof36n5e
 * - <cid-string> is ContentIdentifierCodec::toString(cid) - the CID is
 *   authoritative, the exact key is not (architecture doc verbatim);
 * - <codec> is a display-only token (raw / dag-pb), never parsed back;
 * - <xx> is the FIRST MULTIHASH DIGEST BYTE as two lowercase hex chars
 *   (256-way fan-out). NEVER fan out on the CID string head: every
 *   raw+sha256 CIDv1 string starts with the constant prefix "bafkrei",
 *   so string-head fan-out collapses to exactly one bucket;
 * - CIDv0 and CIDv1 are deliberately NOT normalized - identical content
 *   under both versions yields two objects (dedup is by exact CID; a
 *   documented v1 non-goal mirroring the D-08 blake2b boundary note).
 *
 * Durability honesty: every successful Put reports
 * DurabilityLevel::ObjectStoreAccepted - the maximum level this store
 * has PROVEN (the object store acknowledged receipt; durability is not
 * yet verified). A store never reports an achieved level it has not
 * verified (Durability.hpp contract).
 *
 * @note Thread-safety contract (v1, CORE-03): single-threaded use - no
 *       internal synchronization is promised (same contract as the
 *       other v1 backends). The adapter seam underneath is synchronous
 *       and blocking (Phase-3 contract).
 */

#ifndef SGNS_BLOCKSTORE_S3BLOCKSTORE_HPP
#define SGNS_BLOCKSTORE_S3BLOCKSTORE_HPP

#include <memory>
#include <string>

#include <gsl/span>

#include <sgns-blockstore/Block.hpp>
#include <sgns-blockstore/BlockStore.hpp>
#include <sgns-blockstore/BlockStatus.hpp>
#include <sgns-blockstore/Cid.hpp>
#include <sgns-blockstore/Capabilities.hpp>
#include <sgns-blockstore/Durability.hpp>
#include <sgns-blockstore/Options.hpp>
#include <sgns-blockstore/S3Client.hpp>

namespace sgns::blockstore
{
    /**
     * @brief Options for opening an S3BlockStore.
     */
    struct S3BlockStoreOptions
    {
        /**
         * @brief Root of the derived object-key layout.
         *
         * The BACK-07 letter's defaulted root: keys derive as
         * <keyPrefix>/<codec>/<xx>/<cid-string>. MUST be non-empty
         * (Open throws std::invalid_argument otherwise - programmer
         * error, MinioS3Client::Open precedent). Keep it
         * charset-simple ([a-z0-9/-]): it becomes literal key bytes.
         */
        std::string keyPrefix = "v1/blocks";
    };

    /**
     * @brief BlockStore backend over an injected S3Client adapter
     *        (BACK-07; InMemoryS3Client fake in tests, MinioS3Client in
     *        production - the store never knows which).
     *
     * Blocks are immutable and pre-verified at entry, so every Put is
     * CONDITIONAL (PutObjectOptions{createOnly=true}) by construction:
     * a re-put of a stored CID returns the SUCCESS slot with
     * alreadyPresent=true (the adapter absorbs the object store's 412
     * precondition evidence), and the original bytes are never
     * overwritten. Create-only conditionality is evaluated atomically
     * with the write server/adapter-side, so an interrupted or
     * concurrent Put never leaves a torn or partial object - this store
     * adds no client-side locking and none is needed.
     */
    class S3BlockStore final : public BlockStore
    {
    public:
        ~S3BlockStore() override = default;

        /**
         * @brief Open a store over an adapter handle.
         * @param client the S3Client to compose (shared_ptr injection so
         *        tests retain a handle to the fake's key-observability
         *        seam); must be non-null
         * @param options the key-layout root
         * @return the opened store
         * @throws std::invalid_argument when client is null or
         *         options.keyPrefix is empty (programmer error,
         *         MinioS3Client::Open precedent)
         */
        static result<std::unique_ptr<S3BlockStore>, BlockStatus> Open(
            std::shared_ptr<S3Client> client,
            const S3BlockStoreOptions& options = {});

        /** @copydoc BlockStore::Put() */
        result<PutOutcome, BlockStatus> Put(
            const Cid& cid, gsl::span<const uint8_t> bytes,
            const WriteOptions& options = {}) override;

        /** @copydoc BlockStore::Get() */
        result<Block, BlockStatus> Get(
            const Cid& cid, const ReadOptions& options = {}) override;

        /**
         * @copydoc BlockStore::Has()
         *
         * D-12 (Phase 5) honesty note: Has is NOT content-verifying -
         * it reads HEAD metadata only, so an object whose bytes fail
         * CID verification still reports exists. Corruption detection
         * rides the Get path (verification-at-fetch).
         */
        result<bool, BlockStatus> Has(const Cid& cid) override;

        /**
         * @copydoc BlockStore::GetSize()
         *
         * D-12 (Phase 5) honesty note: GetSize is NOT content-verifying
         * - it reads HEAD metadata only, so a corrupted or wrong-length
         * object still reports its stored size. Corruption detection
         * rides the Get path (verification-at-fetch).
         */
        result<uint64_t, BlockStatus> GetSize(const Cid& cid) override;

        /** @copydoc BlockStore::PutMany() */
        result<std::vector<result<PutOutcome, BlockStatus>>, BlockStatus>
        PutMany(gsl::span<const BlockInput> blocks,
                const WriteOptions& options = {}) override;

        /**
         * @brief The static capability report for this backend.
         * @return maxDurability == ObjectStoreAccepted - the highest
         *         level an object-store acknowledgement proves. No
         *         durable-level claim is made or implied.
         */
        BlockStoreCapabilities Capabilities() const
        {
            return BlockStoreCapabilities{
                /*maxDurability=*/DurabilityLevel::ObjectStoreAccepted};
        }

    private:
        S3BlockStore(std::shared_ptr<S3Client> client,
                     std::string keyPrefix);

        /**
         * @brief The ONE key-derivation place (BACK-07 layout).
         * @param cid a sha256-gated CID (ValidateCidHash ran first)
         * @return <keyPrefix>/<codec>/<xx>/<cid-string>, or a Corrupt
         *         failure when the CID cannot be rendered (practically
         *         unreachable for sha256 CIDv1/v0 - defensive mapping)
         */
        result<std::string, BlockStatus> DeriveKey(const Cid& cid) const;

        std::shared_ptr<S3Client> client_; ///< the composed adapter
        std::string keyPrefix_;            ///< layout root (non-empty)
    };
} // namespace sgns::blockstore

#endif // SGNS_BLOCKSTORE_S3BLOCKSTORE_HPP
