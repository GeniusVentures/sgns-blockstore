/**
 * @file S3BlockStore.cpp
 * @brief Implementation of the S3-backed BlockStore facade (BACK-07) -
 *        maps adapter results, never HTTP.
 */

#include <sgns-blockstore/S3BlockStore.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <libp2p/multi/content_identifier_codec.hpp>
#include <libp2p/multi/multihash.hpp>

namespace sgns::blockstore
{
    namespace
    {
        /**
         * @brief The single shared entry gate (D-01): rejects every
         *        multihash type outside {sha-256, blake2b-256} as
         *        programmer error BEFORE any adapter call or key
         *        derivation.
         */
        void ValidateCidHash(const Cid& cid)
        {
            const auto type = cid.content_address.getType();
            if (type != libp2p::multi::HashType::sha256
                && type != libp2p::multi::HashType::blake2b_256)
            {
                throw std::invalid_argument(
                    "sgns::blockstore: CID multihash type is not sha-256 "
                    "or blake2b-256 - accepted hashes are exactly "
                    "{sha-256, blake2b-256} (D-01); others are a "
                    "programmer error");
            }
        }

        /// @brief Display-only codec token (never parsed back). The
        ///        fixed switch covers the codecs v1 CidOf accepts; any
        ///        other codec renders as a stable catch-all token - key
        ///        uniqueness comes from the CID string, not this label.
        const char* CodecToken(libp2p::multi::MulticodecType::Code codec)
        {
            switch (codec)
            {
                case libp2p::multi::MulticodecType::Code::RAW:
                    return "raw";
                case libp2p::multi::MulticodecType::Code::DAG_PB:
                    return "dag-pb";
                case libp2p::multi::MulticodecType::Code::DAG_CBOR:
                    // The CRDT DAG block codec (Phase-6 widening): every
                    // SuperGenius CRDT block is CIDv1/DAG_CBOR/blake2b-256.
                    return "dag-cbor";
                default:
                    return "unknown-codec";
            }
        }

        /// @brief Two lowercase hex chars of the first DIGEST byte (the
        ///        fan-out component - never the hash-prefix byte, never
        ///        the CID string head: every raw+sha256 CIDv1 string
        ///        starts with the constant "bafkrei", so string-head
        ///        fan-out collapses to one bucket).
        std::string DigestFanoutByte(const Cid& cid)
        {
            static const char* kHex = "0123456789abcdef";
            const auto digest = cid.content_address.getHash();
            const uint8_t first = digest.empty() ? 0 : digest[0];
            return std::string{ kHex[first >> 4], kHex[first & 0x0F] };
        }
    } // namespace

    S3BlockStore::S3BlockStore(std::shared_ptr<S3Client> client,
                               std::string keyPrefix)
        : client_(std::move(client)), keyPrefix_(std::move(keyPrefix))
    {
    }

    result<std::unique_ptr<S3BlockStore>, BlockStatus> S3BlockStore::Open(
        std::shared_ptr<S3Client> client, const S3BlockStoreOptions& options)
    {
        if (!client)
        {
            throw std::invalid_argument(
                "sgns::blockstore: S3BlockStore::Open requires a non-null "
                "S3Client");
        }
        if (options.keyPrefix.empty())
        {
            throw std::invalid_argument(
                "sgns::blockstore: S3BlockStoreOptions.keyPrefix must be "
                "non-empty (the BACK-07 layout root)");
        }
        return std::unique_ptr<S3BlockStore>(
            new S3BlockStore(std::move(client), options.keyPrefix));
    }

    result<std::string, BlockStatus> S3BlockStore::DeriveKey(
        const Cid& cid) const
    {
        // The spike-proven codec path (cid_spike_test.cpp): toString is
        // lossless for sha-256 and blake2b-256 CIDv1 alike (the widened
        // accepted pair, D-01) and injective across CIDv0/v1. A failure
        // here is practically unreachable - mapped defensively,
        // never silently defaulted (a wrong key would misplace a block).
        auto cid_string =
            libp2p::multi::ContentIdentifierCodec::toString(cid);
        if (!cid_string)
        {
            return BlockStatus{StatusClass::Corrupt, BlockSource::Unknown,
                               "S3BlockStore: CID could not be rendered to "
                               "its string form"};
        }
        return keyPrefix_ + '/' + CodecToken(cid.content_type) + '/'
            + DigestFanoutByte(cid) + '/' + cid_string.value();
    }

    result<PutOutcome, BlockStatus> S3BlockStore::Put(
        const Cid& cid, gsl::span<const uint8_t> bytes,
        const WriteOptions& /*options*/)
    {
        ValidateCidHash(cid);
        // Verify-at-facade (S5 lesson, MemoryBlockStore discipline):
        // caller-supplied bytes must hash to the claimed CID BEFORE the
        // adapter is touched. CidOf carries the default error type
        // (std::error_code), so its failure is mapped explicitly - never
        // OUTCOME_TRY'd across the BlockStatus-typed envelope.
        // Phase-6 (D-04): recompute with the CID's OWN multihash type so
        // blake2b-256 CIDs verify under blake2b-256 (the unsupported-hash
        // throw is unreachable here - the entry gate already threw).
        auto computed =
            CidOf(bytes, cid.content_type, cid.content_address.getType());
        if (!computed)
        {
            return BlockStatus{StatusClass::Corrupt, BlockSource::Caller,
                               "Put rejected: payload could not be hashed"};
        }
        if (!(computed.value() == cid))
        {
            return BlockStatus{StatusClass::Corrupt, BlockSource::Caller,
                               "Put rejected: bytes do not hash to the "
                               "claimed CID"};
        }
        auto key = DeriveKey(cid);
        if (!key)
        {
            return key.error();
        }
        // Blocks are immutable and pre-verified: ALWAYS conditional.
        // A create-only hit returns the SUCCESS slot with
        // alreadyPresent=true (D-02 mirror) and never overwrites.
        const std::vector<uint8_t> payload(bytes.begin(), bytes.end());
        auto put = client_->Put(key.value(), payload,
                                PutObjectOptions{ /*createOnly=*/true });
        if (!put)
        {
            return put.error(); // adapter taxonomy propagates as-is
        }
        // Durability honesty: an object-store acknowledgement proves
        // receipt - exactly the Accepted level, never more.
        return PutOutcome{ DurabilityLevel::ObjectStoreAccepted,
                           put.value().alreadyPresent };
    }

    result<Block, BlockStatus> S3BlockStore::Get(
        const Cid& cid, const ReadOptions& options)
    {
        ValidateCidHash(cid);
        auto key = DeriveKey(cid);
        if (!key)
        {
            return key.error();
        }
        auto object = client_->Get(key.value());
        if (!object)
        {
            return object.error(); // authoritative miss + taxonomy as-is
        }
        const auto& bytes = object.value().bytes;
        if (options.verify)
        {
            // Re-verify the FETCHED bytes against the CID. Attribution
            // delta vs Put: mismatched bytes came from the REMOTE, so
            // the failure is Remote-sourced, never Caller.
            // Phase-6 (D-04): branch on the CID's own multihash (see Put).
            auto computed =
                CidOf(bytes, cid.content_type, cid.content_address.getType());
            if (!computed)
            {
                return BlockStatus{StatusClass::Corrupt,
                                   BlockSource::Remote,
                                   "Get rejected: fetched bytes could not "
                                   "be hashed"};
            }
            if (!(computed.value() == cid))
            {
                return BlockStatus{StatusClass::Corrupt,
                                   BlockSource::Remote,
                                   "Get rejected: fetched bytes failed CID "
                                   "re-verification"};
            }
        }
        return Block{ cid, bytes };
    }

    result<bool, BlockStatus> S3BlockStore::Has(const Cid& cid)
    {
        ValidateCidHash(cid);
        auto key = DeriveKey(cid);
        if (!key)
        {
            return key.error();
        }
        auto head = client_->Head(key.value());
        if (!head)
        {
            return head.error(); // FAILURE - never the value false (CORE-02)
        }
        return head.value().exists;
    }

    result<uint64_t, BlockStatus> S3BlockStore::GetSize(const Cid& cid)
    {
        ValidateCidHash(cid);
        auto key = DeriveKey(cid);
        if (!key)
        {
            return key.error();
        }
        auto head = client_->Head(key.value());
        if (!head)
        {
            return head.error();
        }
        if (!head.value().exists)
        {
            // Head answers authoritatively - absence is NotFound here
            // (an authoritative miss, never a guessed one).
            return BlockStatus{StatusClass::NotFound, BlockSource::Remote,
                               "S3 object not found (Head exists=false)"};
        }
        return head.value().size; // HEAD content-length; no bytes
                                  // returned, no verification applies
    }

    result<std::vector<result<PutOutcome, BlockStatus>>, BlockStatus>
    S3BlockStore::PutMany(gsl::span<const BlockInput> blocks,
                          const WriteOptions& options)
    {
        // D-01 pre-validation of the WHOLE batch before any write (a
        // mid-batch throw can never leave partial writes behind).
        for (const auto& input : blocks)
        {
            ValidateCidHash(input.cid);
        }
        std::vector<result<PutOutcome, BlockStatus>> results;
        results.reserve(blocks.size());
        for (const auto& input : blocks)
        {
            // Per-element Put preserves index correspondence; a failed
            // element never rolls back its siblings (D-14).
            results.push_back(Put(input.cid, input.bytes, options));
        }
        return results;
    }
} // namespace sgns::blockstore
