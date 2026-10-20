/**
 * @file MemoryBlockStore.cpp
 * @brief Implementation of the hermetic in-memory reference backend
 *        (BACK-01) with verify-at-facade discipline (CORE-04).
 */

#include <sgns-blockstore/MemoryBlockStore.hpp>

#include <optional>
#include <stdexcept>

#include <libp2p/multi/multihash.hpp>

namespace sgns::blockstore
{
    namespace
    {
        /**
         * @brief The single shared entry gate used by every public op:
         *        rejects non-sha256 multihash CIDs as programmer error
         *        (D-08) BEFORE any storage mutation.
         */
        void ValidateCidHash(const Cid& cid)
        {
            if (cid.content_address.getType() != libp2p::multi::HashType::sha256)
            {
                throw std::invalid_argument(
                    "sgns::blockstore: CID multihash type is not sha-256 - "
                    "v1 accepts sha-256 CIDs only (D-08; blake2b CRDT CIDs "
                    "are a documented v1 non-goal)");
            }
        }

        /**
         * @brief The fault gate consulted before storage is touched:
         *        returns an engaged BlockStatus failure under
         *        Unavailable/Unauthorized faults, disengaged otherwise.
         *        Has NEVER returns false through this path (CORE-02).
         */
        std::optional<BlockStatus> FaultGate(FaultKind fault)
        {
            switch (fault)
            {
                case FaultKind::Unavailable:
                    return BlockStatus{StatusClass::TemporarilyUnavailable,
                                       BlockSource::Memory,
                                       "fault injection: backend unavailable"};
                case FaultKind::Unauthorized:
                    return BlockStatus{StatusClass::Unauthorized,
                                       BlockSource::Memory,
                                       "fault injection: backend authorization failure"};
                default:
                    return std::nullopt;
            }
        }
    }

    result<PutOutcome, BlockStatus> MemoryBlockStore::Put(
        const Cid& cid, gsl::span<const uint8_t> bytes,
        const WriteOptions& /*options*/)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        // Verify-at-facade: caller-supplied bytes must hash to the CID.
        // NOTE: CidOf carries the default error type (std::error_code), so
        // its failure is mapped explicitly - never OUTCOME_TRY'd across the
        // BlockStatus-typed envelope (mismatched error types).
        auto computed = CidOf(bytes, cid.content_type);
        if (!computed)
        {
            return BlockStatus{StatusClass::Corrupt, BlockSource::Caller,
                               "Put rejected: payload could not be hashed"};
        }
        if (!(computed.value() == cid))
        {
            return BlockStatus{StatusClass::Corrupt, BlockSource::Caller,
                               "Put rejected: bytes do not hash to the claimed CID"};
        }
        const bool already_present = storage_.find(cid) != storage_.end();
        storage_[cid] = std::vector<uint8_t>(bytes.begin(), bytes.end());
        return PutOutcome{DurabilityLevel::LocalCommitted, already_present};
    }

    result<Block, BlockStatus> MemoryBlockStore::Get(
        const Cid& cid, const ReadOptions& options)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        auto it = storage_.find(cid);
        if (it == storage_.end())
        {
            return BlockStatus{StatusClass::NotFound, BlockSource::Memory,
                               "block not found in memory store"};
        }
        auto bytes = it->second; // deliberate copy - corruption is induced on the copy
        if (fault_ == FaultKind::CorruptData && !bytes.empty())
        {
            bytes[0] = static_cast<uint8_t>(~bytes[0]); // documented seam mechanic
        }
        if (options.verify)
        {
            auto computed = CidOf(bytes, cid.content_type);
            if (!computed)
            {
                return BlockStatus{StatusClass::Corrupt, BlockSource::Memory,
                                   "Get rejected: stored bytes could not be hashed"};
            }
            if (!(computed.value() == cid))
            {
                return BlockStatus{StatusClass::Corrupt, BlockSource::Memory,
                                   "Get rejected: stored bytes failed CID re-verification"};
            }
        }
        return Block{cid, std::move(bytes)};
    }

    result<bool, BlockStatus> MemoryBlockStore::Has(const Cid& cid)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value(); // FAILURE - never the value false (CORE-02)
        }
        return storage_.find(cid) != storage_.end();
    }

    result<uint64_t, BlockStatus> MemoryBlockStore::GetSize(const Cid& cid)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        auto it = storage_.find(cid);
        if (it == storage_.end())
        {
            return BlockStatus{StatusClass::NotFound, BlockSource::Memory,
                               "block not found in memory store"};
        }
        return static_cast<uint64_t>(it->second.size());
    }

    result<std::vector<result<PutOutcome, BlockStatus>>, BlockStatus>
    MemoryBlockStore::PutMany(gsl::span<const BlockInput> blocks,
                              const WriteOptions& options)
    {
        // D-08 pre-validation of the WHOLE batch before any write: a
        // mid-batch throw can never leave partial writes behind.
        for (const auto& input : blocks)
        {
            ValidateCidHash(input.cid);
        }
        if (auto gate = FaultGate(fault_))
        {
            return gate.value(); // outer envelope: wholesale malfunction only
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

    result<void, BlockStatus> MemoryBlockStore::EvictLocal(const Cid& cid)
    {
        storage_.erase(cid);
        return outcome::success(); // idempotent post-state: no local bytes remain
    }

    void MemoryBlockStore::SetFault(FaultKind kind)
    {
        fault_ = kind;
    }

    FaultKind MemoryBlockStore::GetFault() const
    {
        return fault_;
    }
}
