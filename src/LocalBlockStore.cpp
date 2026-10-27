/**
 * @file LocalBlockStore.cpp
 * @brief Implementation of the durable RocksDB cold-tier backend
 *        (BACK-03/BACK-04) - ALL rocksdb::* usage lives in this TU.
 */

#include <sgns-blockstore/LocalBlockStore.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <chrono>

#include <libp2p/multi/content_identifier_codec.hpp>
#include <libp2p/multi/multihash.hpp>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/status.h>
#include <rocksdb/write_batch.h>

namespace sgns::blockstore
{
    bool EquivalentCidClaim(const Cid& computed, const Cid& claimed)
    {
        if (claimed.content_address.getType()
            != computed.content_address.getType())
        {
            return false;
        }
        if (!(claimed.content_address == computed.content_address))
        {
            return false; // digest mismatch: NOT equivalent - Corrupt stands
        }
        if (claimed.content_type != computed.content_type)
        {
            return false;
        }
        // Identical multihash + codec + hash type: the only remaining
        // difference is the version encoding (V0 <-> V1), which the IPLD
        // spec defines as the same address.
        return true;
    }

    namespace
    {
        /**
         * @brief The single shared entry gate used by every public op:
         *        rejects every multihash type outside {sha-256,
         *        blake2b-256} as programmer error (D-01) BEFORE any
         *        storage access. Verbatim widening of the
         *        MemoryBlockStore reference backend gate.
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

        /**
         * @brief The fault gate consulted BEFORE any RocksDB access:
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
                                       BlockSource::Local,
                                       "fault injection: backend unavailable"};
                case FaultKind::Unauthorized:
                    return BlockStatus{StatusClass::Unauthorized,
                                       BlockSource::Local,
                                       "fault injection: backend authorization failure"};
                default:
                    return std::nullopt;
            }
        }

        /**
         * @brief Encode a CID to its RocksDB key (D-01 anchor).
         *
         * Byte-identical to ipfs-lite-cpp RocksdbDatastore's cid.toBytes()
         * keys, so a migration between the stores needs no re-keying.
         * The codec carries the DEFAULT error type (std::error_code), so
         * its failure is mapped explicitly - never OUTCOME_TRY'd across
         * the BlockStatus-typed envelope.
         */
        result<std::string, BlockStatus> MakeKey(const Cid& cid)
        {
            auto encoded = libp2p::multi::ContentIdentifierCodec::encode(cid);
            if (!encoded)
            {
                return BlockStatus{StatusClass::Corrupt, BlockSource::Local,
                                   "CID could not be encoded to its RocksDB key"};
            }
            return std::string(encoded.value().begin(), encoded.value().end());
        }

        /**
         * @brief Journal-record codec: value schema [version-byte 0x01]
         *        [state-byte] with 0x00=PendingUpload,
         *        0x01=UploadedAccepted, 0x02=UploadedDurable.
         *
         * Conservative decoding (T-02-03): ANY unknown version or state
         * byte decodes to PendingUpload - idempotent re-upload is the
         * safe direction, and an undecodable record is never silently
         * dropped from the pending set.
         */
        struct JournalRecord
        {
            static constexpr char kVersionByte = 0x01;

            static std::string Encode(UploadState state)
            {
                char state_byte = 0x00;
                switch (state)
                {
                    case UploadState::PendingUpload:
                        state_byte = 0x00;
                        break;
                    case UploadState::UploadedAccepted:
                        state_byte = 0x01;
                        break;
                    case UploadState::UploadedDurable:
                        state_byte = 0x02;
                        break;
                }
                return std::string{kVersionByte, state_byte};
            }

            static std::optional<UploadState> Decode(const rocksdb::Slice& value)
            {
                if (value.size() == 2 && value[0] == kVersionByte)
                {
                    switch (value[1])
                    {
                        case 0x00:
                            return UploadState::PendingUpload;
                        case 0x01:
                            return UploadState::UploadedAccepted;
                        case 0x02:
                            return UploadState::UploadedDurable;
                        default:
                            break;
                    }
                }
                // Unknown version/state byte (or short value): the
                // conservative direction keeps the block in the pending set.
                return UploadState::PendingUpload;
            }
        };

        /**
         * @brief Spill-timestamp codec: value schema is EXACTLY 8 bytes
         *        of little-endian uint64 unix-ms (D-01, v1.1 Phase 11).
         *
         * The journal record itself carries NO timestamp (2 bytes only),
         * so the side column family is the sole age-evidence store. ANY
         * value that is not exactly 8 bytes decodes to nullopt - the
         * caller treats it as absent (spilled_at 0: backfill-and-wait,
         * the under-eviction direction), never as a fabricated time.
         */
        struct SpillTimeRecord
        {
            static constexpr size_t kEncodedSize = 8;

            static std::string Encode(uint64_t unix_ms)
            {
                std::string encoded(kEncodedSize, '\0');
                for (size_t i = 0; i < kEncodedSize; ++i)
                {
                    encoded[i] = static_cast<char>((unix_ms >> (8 * i)) & 0xFF);
                }
                return encoded;
            }

            static std::optional<uint64_t> Decode(const rocksdb::Slice& value)
            {
                if (value.size() != kEncodedSize)
                {
                    return std::nullopt;
                }
                uint64_t unix_ms = 0;
                for (size_t i = 0; i < kEncodedSize; ++i)
                {
                    unix_ms |= static_cast<uint64_t>(
                                   static_cast<uint8_t>(value[i]))
                               << (8 * i);
                }
                return unix_ms;
            }
        };

        /**
         * @brief RocksDB Status -> BlockStatus, switched on named Status
         *        codes ONLY (never ToString matching, never enumerating -
         *        enum values are append-only across RocksDB versions).
         *
         * Only IsNotFound maps to NotFound; corruption maps to Corrupt;
         * every IO/lock/busy/timeout class maps to TemporarilyUnavailable
         * - an IO or LOCK failure can never surface as NotFound (T-02-02).
         * Unauthorized has no RocksDB source in this backend; only the
         * FaultController seam induces it.
         */
        BlockStatus MapStatus(const rocksdb::Status& status,
                              const std::string& context)
        {
            if (status.IsNotFound())
            {
                return BlockStatus{StatusClass::NotFound, BlockSource::Local,
                                   context};
            }
            if (status.IsCorruption())
            {
                return BlockStatus{StatusClass::Corrupt, BlockSource::Local,
                                   context + ": " + status.ToString()};
            }
            // kIOError (incl. NoSpace/PathNotFound/IOFenced subcodes),
            // kBusy, kTimedOut, kTryAgain, kAborted,
            // kColumnFamilyDropped, kLockFileTimeout, ... - every
            // environment-class failure is non-authoritative.
            return BlockStatus{StatusClass::TemporarilyUnavailable,
                               BlockSource::Local,
                               context + ": " + status.ToString()};
        }

        rocksdb::Slice KeySlice(const std::string& key)
        {
            return rocksdb::Slice(key.data(), key.size());
        }

        /// Wall-clock unix-ms (the D-01 spill-timestamp clock).
        uint64_t UnixNowMs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch() )
                    .count() );
        }
    }

    //
    // Impl - every rocksdb::* member lives behind this pimpl.
    //

    class LocalBlockStore::Impl
    {
    public:
        Impl()
        {
            // The fsync half of BACK-03's "(temp + fsync + rename)"
            // mapping, set ONCE at open and never reset.
            wo_.sync = true;
        }

        rocksdb::DB* db_ = nullptr;
        rocksdb::ColumnFamilyHandle* blocks_ = nullptr;
        rocksdb::ColumnFamilyHandle* journal_ = nullptr;
        /// v1.1 Phase 11 (D-01): the spill-timestamp side column family.
        rocksdb::ColumnFamilyHandle* spill_times_ = nullptr;
        rocksdb::WriteOptions wo_;
        rocksdb::ReadOptions ro_;
    };

    LocalBlockStore::LocalBlockStore()
        : impl_(std::make_unique<Impl>())
    {
    }

    LocalBlockStore::~LocalBlockStore()
    {
        (void)Close(); // idempotent teardown; status deliberately dropped
    }

    result<std::unique_ptr<LocalBlockStore>, BlockStatus> LocalBlockStore::Open(
        const LocalBlockStoreOptions& options)
    {
        if (options.path.empty())
        {
            throw std::invalid_argument(
                "sgns::blockstore: LocalBlockStoreOptions::path is empty - "
                "the store requires the directory of its OWN RocksDB "
                "instance (D-01; never the GlobalDB handle)");
        }

        // Never rely on RocksDB to create parent directories (A4).
        std::error_code ec;
        std::filesystem::create_directories(options.path, ec);
        if (ec && !std::filesystem::is_directory(options.path))
        {
            return BlockStatus{StatusClass::TemporarilyUnavailable,
                               BlockSource::Local,
                               "Open: could not create store directory '"
                                   + options.path + "': " + ec.message()};
        }

        rocksdb::DBOptions db_options;
        db_options.create_if_missing = true;
        db_options.create_missing_column_families = true;
        // Close never flushes; reopen always replays the WAL tail -
        // the deterministic, crash-faithful recovery path.
        db_options.avoid_flush_during_shutdown = true;
        db_options.info_log_level = rocksdb::InfoLogLevel::ERROR_LEVEL;

        const std::vector<rocksdb::ColumnFamilyDescriptor> cf_descriptors = {
            // The default CF is opened because RocksDB requires ALL column
            // families be listed (the default-constructed descriptor names
            // it via GetDefaultColumnFamilyName()); it stays unused and its
            // handle is NEVER destroyed at teardown.
            rocksdb::ColumnFamilyDescriptor{},
            {kBlocksColumnFamilyName, rocksdb::ColumnFamilyOptions()},
            {kUploadJournalColumnFamilyName, rocksdb::ColumnFamilyOptions()},
            // v1.1 Phase 11 (D-01): the spill-timestamp side CF.
            // create_missing_column_families is already true, so listing
            // the descriptor here EXTENDS a pre-Phase-11 store in place
            // (A1) - no migration, no separate Open pass.
            {kSpillTimesColumnFamilyName, rocksdb::ColumnFamilyOptions()},
        };

        std::vector<rocksdb::ColumnFamilyHandle*> handles;
        rocksdb::DB* db = nullptr;
        const auto status = rocksdb::DB::Open(
            db_options, options.path, cf_descriptors, &handles, &db);
        if (!status.ok())
        {
            // Lock-held / IO / corruption surface as TemporarilyUnavailable
            // or Corrupt - never NotFound (T-02-05).
            return MapStatus(status, "Open: DB::Open failed for " + options.path);
        }

        auto store = std::unique_ptr<LocalBlockStore>(new LocalBlockStore());
        store->impl_->db_ = db;
        store->impl_->blocks_ = handles[1];
        store->impl_->journal_ = handles[2];
        store->impl_->spill_times_ = handles[3];
        return store;
    }

    result<void, BlockStatus> LocalBlockStore::Close()
    {
        if (impl_ == nullptr || impl_->db_ == nullptr)
        {
            return outcome::success(); // idempotent
        }
        // NEVER the DefaultColumnFamily() handle (Pitfall 7).
        impl_->db_->DestroyColumnFamilyHandle(impl_->blocks_);
        impl_->db_->DestroyColumnFamilyHandle(impl_->journal_);
        impl_->db_->DestroyColumnFamilyHandle(impl_->spill_times_);
        impl_->blocks_ = nullptr;
        impl_->journal_ = nullptr;
        impl_->spill_times_ = nullptr;
        // Close does not fsync the WAL files - by design: the WAL is
        // exactly what reopen replays.
        const auto status = impl_->db_->Close();
        // Regardless of the Close status, the DB must be freed - this
        // releases the LOCK so a same-process reopen succeeds.
        delete impl_->db_;
        impl_->db_ = nullptr;
        return MapStatus(status, "Close");
    }

    result<PutOutcome, BlockStatus> LocalBlockStore::Put(
        const Cid& cid, gsl::span<const uint8_t> bytes,
        const WriteOptions& /*options*/)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        // Verify-at-facade: caller-supplied bytes must hash to the CID.
        // CidOf carries the default error type (std::error_code), so its
        // failure is mapped explicitly - never OUTCOME_TRY'd across the
        // BlockStatus-typed envelope.
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
        if (!(computed.value() == cid) && !EquivalentCidClaim(computed.value(), cid))
        {
            return BlockStatus{StatusClass::Corrupt, BlockSource::Caller,
                               "Put rejected: bytes do not hash to the claimed CID"};
        }
        auto key = MakeKey(cid);
        if (!key)
        {
            return key.error();
        }
        // Read-before-write (Pitfall 6): a RocksDB Put overwrites
        // silently, so alreadyPresent must be computed before the batch -
        // and the journal record is written only for NEW blocks, so a
        // re-Put never regresses an UploadedDurable state.
        std::string existing;
        const auto presence = impl_->db_->Get(
            impl_->ro_, impl_->blocks_, KeySlice(key.value()), &existing);
        if (!presence.ok() && !presence.IsNotFound())
        {
            return MapStatus(presence, "Put: presence probe failed");
        }
        const bool already_present = presence.ok();

        // D-07 test seam: simulate process death BEFORE the atomic
        // DB::Write commit. The caller never sees success and NOTHING is
        // written - no torn block, no orphan journal record.
        if (crash_point_ == CrashPoint::BeforeCommit)
        {
            return BlockStatus{StatusClass::TemporarilyUnavailable,
                               BlockSource::Local,
                               "crash point: process simulated dead before "
                               "DB::Write"};
        }

        // D-02/D-03/D-04: ONE WriteBatch carries the block bytes AND - only
        // for a new block - the PendingUpload journal record. DB::Write is
        // the atomicity boundary: after a crash either BOTH exist or
        // neither does; there is no mid-Write observable state, and no
        // second Write call ever runs in the Put path.
        rocksdb::WriteBatch batch;
        batch.Put(impl_->blocks_, KeySlice(key.value()),
                  rocksdb::Slice(reinterpret_cast<const char*>(bytes.data()),
                                 bytes.size()));
        if (!already_present)
        {
            batch.Put(impl_->journal_, KeySlice(key.value()),
                      JournalRecord::Encode(UploadState::PendingUpload));
            // v1.1 Phase 11 (D-01): the spill timestamp rides the SAME
            // birth batch - atomic with the journal record, no second
            // Write, no timestamp churn on re-Put. The value is 8-byte
            // little-endian unix-ms.
            batch.Put(impl_->spill_times_, KeySlice(key.value()),
                      SpillTimeRecord::Encode(UnixNowMs()));
        }
        const auto committed = impl_->db_->Write(impl_->wo_, &batch);
        if (!committed.ok())
        {
            return MapStatus(committed, "Put: batch commit failed");
        }

        // D-09 test seam: corrupt the just-committed block IN PLACE -
        // after the atomic commit gate, before the caller sees success.
        // The store's OWN handles round-trip the value (no second DB
        // open, no CF-name string crossing the API) and the write-back
        // rides impl_->wo_ (sync=true) so the corruption survives the
        // no-flush close+reopen the crash suite uses as its oracle.
        // NEVER touches the journal CF, never fires on the
        // already_present re-Put path, never routed through
        // fault_/FaultGate (the kit's CorruptData corrupts only the
        // returned copy and must never evict). PRODUCTION CODE MUST
        // NEVER ARM THIS.
        if (corrupt_after_commit_ && !already_present)
        {
            std::string committed_value;
            const auto read_back = impl_->db_->Get(
                impl_->ro_, impl_->blocks_, KeySlice(key.value()),
                &committed_value);
            if (read_back.ok() && !committed_value.empty())
            {
                committed_value[0] = static_cast<char>(
                    static_cast<uint8_t>(committed_value[0]) ^ 0xFF);
                (void)impl_->db_->Put(
                    impl_->wo_, impl_->blocks_, KeySlice(key.value()),
                    rocksdb::Slice(committed_value.data(),
                                   committed_value.size()));
            }
        }
        return PutOutcome{DurabilityLevel::LocalCommitted, already_present};
    }

    result<Block, BlockStatus> LocalBlockStore::Get(
        const Cid& cid, const ReadOptions& options)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        auto key = MakeKey(cid);
        if (!key)
        {
            return key.error();
        }
        std::string value;
        const auto status = impl_->db_->Get(
            impl_->ro_, impl_->blocks_, KeySlice(key.value()), &value);
        if (status.IsNotFound())
        {
            return BlockStatus{StatusClass::NotFound, BlockSource::Local,
                               "block not found in local store"};
        }
        if (!status.ok())
        {
            return MapStatus(status, "Get: read failed");
        }
        auto bytes = std::vector<uint8_t>(value.begin(), value.end()); // deliberate copy - corruption is induced on the copy
        if (fault_ == FaultKind::CorruptData && !bytes.empty())
        {
            bytes[0] = static_cast<uint8_t>(~bytes[0]); // documented seam mechanic
        }
        if (options.verify)
        {
            // Phase-6 (D-04): branch on the CID's own multihash (see Put);
            // the V0<->V1 dag-pb equivalence applies identically here.
            auto computed =
                CidOf(bytes, cid.content_type, cid.content_address.getType());
            if (!computed)
            {
                return BlockStatus{StatusClass::Corrupt, BlockSource::Local,
                                   "Get rejected: stored bytes could not be hashed"};
            }
            if (!(computed.value() == cid) && !EquivalentCidClaim(computed.value(), cid))
            {
                if (fault_ == FaultKind::CorruptData)
                {
                    // Fault-induced corruption: the seam flipped a byte on
                    // the RETURNED COPY only - stored bytes are fine, so NO
                    // eviction (Pitfall 3; the kit's non-sticky contract).
                    return BlockStatus{StatusClass::Corrupt,
                                       BlockSource::Local,
                                       "Get rejected: stored bytes failed CID "
                                       "re-verification"};
                }
                // REAL on-disk corruption (D-09 evict-and-surface): surface
                // Corrupt for THIS call AND atomically remove block +
                // journal so subsequent Gets report NotFound and a re-Put
                // repairs. Get is write-capable ONLY on this path. The
                // eviction write is best-effort: its failure is appended to
                // the message, never swallowed, and the Corrupt result still
                // returns.
                rocksdb::WriteBatch removal;
                removal.Delete(impl_->blocks_, KeySlice(key.value()));
                removal.Delete(impl_->journal_, KeySlice(key.value()));
                const auto evicted = impl_->db_->Write(impl_->wo_, &removal);
                std::string message =
                    "Get rejected: stored bytes failed CID verification - "
                    "entry evicted, re-Put to repair";
                if (!evicted.ok())
                {
                    message += " (eviction write failed: "
                             + evicted.ToString() + ")";
                }
                return BlockStatus{StatusClass::Corrupt, BlockSource::Local,
                                   message};
            }
        }
        return Block{cid, std::move(bytes)};
    }

    result<bool, BlockStatus> LocalBlockStore::Has(const Cid& cid)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value(); // FAILURE - never the value false (CORE-02)
        }
        auto key = MakeKey(cid);
        if (!key)
        {
            return key.error();
        }
        std::string value;
        const auto status = impl_->db_->Get(
            impl_->ro_, impl_->blocks_, KeySlice(key.value()), &value);
        if (status.IsNotFound())
        {
            return false; // authoritative miss is the VALUE false
        }
        if (!status.ok())
        {
            return MapStatus(status, "Has: presence probe failed");
        }
        return true;
    }

    result<uint64_t, BlockStatus> LocalBlockStore::GetSize(const Cid& cid)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        auto key = MakeKey(cid);
        if (!key)
        {
            return key.error();
        }
        std::string value;
        const auto status = impl_->db_->Get(
            impl_->ro_, impl_->blocks_, KeySlice(key.value()), &value);
        if (status.IsNotFound())
        {
            return BlockStatus{StatusClass::NotFound, BlockSource::Local,
                               "block not found in local store"};
        }
        if (!status.ok())
        {
            return MapStatus(status, "GetSize: read failed");
        }
        return static_cast<uint64_t>(value.size());
    }

    result<std::vector<result<PutOutcome, BlockStatus>>, BlockStatus>
    LocalBlockStore::PutMany(gsl::span<const BlockInput> blocks,
                             const WriteOptions& options)
    {
        // D-01 pre-validation of the WHOLE batch before any write: a
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

    result<std::vector<Cid>, BlockStatus> LocalBlockStore::GetUploadsInState(
        UploadState state)
    {
        // A plain CF iterator over the default BytewiseComparator is
        // snapshot-consistent and yields keys in byte-lexicographic order
        // over the encoded CID bytes - the frozen ordering contract,
        // satisfied naturally. Synchronous scan-on-call (D-05): the
        // snapshot is available immediately after Open - a restart
        // re-derives the same set from the journal (the D-03 snapshot
        // semantics: journal-derived, crash-re-derivable).
        std::vector<Cid> matching;
        std::unique_ptr<rocksdb::Iterator> it(
            impl_->db_->NewIterator(impl_->ro_, impl_->journal_));
        for (it->SeekToFirst(); it->Valid(); it->Next())
        {
            const auto record = JournalRecord::Decode(it->value());
            if (!record || *record != state)
            {
                continue;
            }
            auto cid = libp2p::multi::ContentIdentifierCodec::decode(
                gsl::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(it->key().data()),
                    it->key().size()));
            if (cid)
            {
                matching.push_back(std::move(cid.value()));
            }
            // Undecodable key: skipped - audit surface, never a scan crash.
        }
        if (!it->status().ok())
        {
            return MapStatus(it->status(), "GetUploadsInState: journal scan failed");
        }
        return matching;
    }

    result<std::vector<Cid>, BlockStatus> LocalBlockStore::GetPendingUploads()
    {
        // One scan body exists: the pending set is the state-filtered
        // enumeration generalized (DUR-01, Phase 10).
        return GetUploadsInState(UploadState::PendingUpload);
    }

    result<uint64_t, BlockStatus> LocalBlockStore::GetSpilledAt(const Cid& cid)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        auto key = MakeKey(cid);
        if (!key)
        {
            return key.error();
        }
        std::string value;
        const auto status = impl_->db_->Get(
            impl_->ro_, impl_->spill_times_, KeySlice(key.value()), &value);
        // Absent vs unreadable are DISTINCT outcomes (MI-02's evidence
        // rule depends on it): only IsNotFound maps to NotFound; IO/lock
        // failures map to TemporarilyUnavailable and never masquerade as
        // absence.
        if (status.IsNotFound())
        {
            return BlockStatus{StatusClass::NotFound, BlockSource::Local,
                               "no spill timestamp for the CID"};
        }
        if (!status.ok())
        {
            return MapStatus(status, "GetSpilledAt: read failed");
        }
        auto decoded = SpillTimeRecord::Decode(rocksdb::Slice(value));
        if (!decoded)
        {
            // Undecodable side-CF value: conservative direction is absent
            // (backfill-and-wait), surfaced as Corrupt so the caller can
            // distinguish it from a clean miss.
            return BlockStatus{StatusClass::Corrupt, BlockSource::Local,
                               "spill timestamp record is not 8 bytes"};
        }
        return decoded.value();
    }

    result<void, BlockStatus> LocalBlockStore::SetSpilledAt(const Cid& cid,
                                                            uint64_t unix_ms)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        auto key = MakeKey(cid);
        if (!key)
        {
            return key.error();
        }
        // ONE atomic sync write (the TransitionJournal shape) - the
        // backfill surface for pre-Phase-11 records and the test seam for
        // age-gate fixtures; never on the Put path.
        rocksdb::WriteBatch batch;
        batch.Put(impl_->spill_times_, KeySlice(key.value()),
                  SpillTimeRecord::Encode(unix_ms));
        const auto committed = impl_->db_->Write(impl_->wo_, &batch);
        if (!committed.ok())
        {
            return MapStatus(committed, "SetSpilledAt: commit failed");
        }
        return outcome::success();
    }

    result<std::vector<SpillEntry>, BlockStatus>
    LocalBlockStore::GetDurableWithSpillTime()
    {
        // ONE scan of the journal CF filtering UploadedDurable, with point
        // reads in spill_times (the GetUploadsInState iterator discipline:
        // snapshot-consistent iterator, undecodable-record skip, iterator
        // status check after the loop). Ordering is the journal CF's
        // encoded-byte lexicographic order over the CID keys - the frozen
        // ordering contract, deterministic and restart-stable.
        std::vector<SpillEntry> durable;
        std::unique_ptr<rocksdb::Iterator> it(
            impl_->db_->NewIterator(impl_->ro_, impl_->journal_));
        for (it->SeekToFirst(); it->Valid(); it->Next())
        {
            const auto record = JournalRecord::Decode(it->value());
            if (!record || *record != UploadState::UploadedDurable)
            {
                continue;
            }
            auto cid = libp2p::multi::ContentIdentifierCodec::decode(
                gsl::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(it->key().data()),
                    it->key().size()));
            if (!cid)
            {
                continue; // undecodable key: skipped - audit surface, never a scan crash
            }
            // Point read: a missing side-CF entry yields spilled_at 0
            // (absent, never fabricated, never a scan abort).
            uint64_t spilled_at = 0;
            std::string stamp;
            const auto stamp_status = impl_->db_->Get(
                impl_->ro_, impl_->spill_times_, it->key(), &stamp);
            if (stamp_status.ok())
            {
                auto decoded = SpillTimeRecord::Decode(rocksdb::Slice(stamp));
                if (decoded)
                {
                    spilled_at = decoded.value();
                }
            }
            else if (!stamp_status.IsNotFound())
            {
                // A read failure poisons the whole enumeration's evidence:
                // surface it so the caller aborts the pass (MI-02 - an
                // unreadable read is never eligibility evidence).
                return MapStatus(stamp_status,
                                 "GetDurableWithSpillTime: spill-time read failed");
            }
            durable.push_back(SpillEntry{std::move(cid.value()), spilled_at});
        }
        if (!it->status().ok())
        {
            return MapStatus(it->status(),
                             "GetDurableWithSpillTime: journal scan failed");
        }
        return durable;
    }

    result<void, BlockStatus> LocalBlockStore::ForEach(
        const std::function<void(const Cid&, gsl::span<const uint8_t>)>& visitor)
    {
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        // A plain CF iterator over the default BytewiseComparator is
        // snapshot-consistent and yields keys in byte-lexicographic order
        // over the encoded CID bytes - the frozen ordering contract.
        std::unique_ptr<rocksdb::Iterator> it(
            impl_->db_->NewIterator(impl_->ro_, impl_->blocks_));
        for (it->SeekToFirst(); it->Valid(); it->Next())
        {
            auto cid = libp2p::multi::ContentIdentifierCodec::decode(
                gsl::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(it->key().data()),
                    it->key().size()));
            if (!cid)
            {
                continue; // undecodable key: skipped - audit surface, never a scan crash
            }
            visitor(cid.value(),
                    gsl::span<const uint8_t>(
                        reinterpret_cast<const uint8_t*>(it->value().data()),
                        it->value().size()));
        }
        if (!it->status().ok())
        {
            return MapStatus(it->status(), "ForEach: block scan failed");
        }
        return outcome::success();
    }

    void LocalBlockStore::SetFault(FaultKind kind)
    {
        fault_ = kind;
    }

    void LocalBlockStore::SetCrashPointForTesting(CrashPoint point)
    {
        crash_point_ = point;
    }

    void LocalBlockStore::SetCorruptAfterCommitForTesting(bool armed)
    {
        corrupt_after_commit_ = armed;
    }

    FaultKind LocalBlockStore::GetFault() const
    {
        return fault_;
    }

    result<void, BlockStatus> LocalBlockStore::MarkUploadedAccepted(
        const Cid& cid)
    {
        return TransitionJournal(cid, UploadState::UploadedAccepted);
    }

    result<void, BlockStatus> LocalBlockStore::MarkUploadedDurable(
        const Cid& cid)
    {
        return TransitionJournal(cid, UploadState::UploadedDurable);
    }

    result<void, BlockStatus> LocalBlockStore::MarkUploadedDurableMany(
        gsl::span<const Cid> const& cids)
    {
        // Throw-at-entry validation for the WHOLE batch (the Phase-6
        // discipline: an out-of-set hash is a programmer error).
        for (const auto& cid : cids)
        {
            ValidateCidHash(cid);
        }
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        // Per-item read-gate (RESEARCH 3.2/A6): transitions move
        // Put-born records only - a no-record CID is SKIPPED-AND-
        // CONTINUED (per-item no-fabrication isolation; the batch never
        // aborts on a missing record).
        rocksdb::WriteBatch batch;
        size_t verified = 0;
        for (const auto& cid : cids)
        {
            auto key = MakeKey(cid);
            if (!key)
            {
                continue; // defensive: practically unreachable post-validation
            }
            std::string record;
            const auto present = impl_->db_->Get(
                impl_->ro_, impl_->journal_, KeySlice(key.value()), &record);
            if (present.IsNotFound())
            {
                continue; // no journal record: never fabricated
            }
            if (!present.ok())
            {
                return MapStatus(present,
                                 "batched durable mark: record read failed");
            }
            batch.Put(impl_->journal_, KeySlice(key.value()),
                      JournalRecord::Encode(UploadState::UploadedDurable));
            ++verified;
        }
        // ONE WriteBatch + ONE sync Write over the verified set: a
        // single fsync and an all-or-nothing crash window (an empty
        // verified set is a success no-op - nothing to commit).
        if (verified > 0)
        {
            const auto committed = impl_->db_->Write(impl_->wo_, &batch);
            if (!committed.ok())
            {
                return MapStatus(committed,
                                 "batched durable mark: commit failed");
            }
        }
        return outcome::success();
    }

    result<void, BlockStatus> LocalBlockStore::TransitionJournal(
        const Cid& cid, UploadState new_state)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        auto key = MakeKey(cid);
        if (!key)
        {
            return key.error();
        }
        // Read-journal-first (T-02-08): transitions move Put-born records
        // only - marking an unknown CID would fabricate upload state, so
        // the NotFound branch writes NOTHING.
        std::string record;
        const auto present = impl_->db_->Get(
            impl_->ro_, impl_->journal_, KeySlice(key.value()), &record);
        if (present.IsNotFound())
        {
            return BlockStatus{StatusClass::NotFound, BlockSource::Local,
                               "journal transition rejected: no journal "
                               "record for the CID (records are Put-born)"};
        }
        if (!present.ok())
        {
            return MapStatus(present, "journal transition: record read failed");
        }
        // ONE atomic journal update per transition (D-06).
        rocksdb::WriteBatch batch;
        batch.Put(impl_->journal_, KeySlice(key.value()),
                  JournalRecord::Encode(new_state));
        const auto committed = impl_->db_->Write(impl_->wo_, &batch);
        if (!committed.ok())
        {
            return MapStatus(committed, "journal transition: commit failed");
        }
        return outcome::success();
    }

    result<UploadState, BlockStatus> LocalBlockStore::GetUploadState(
        const Cid& cid)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        auto key = MakeKey(cid);
        if (!key)
        {
            return key.error();
        }
        std::string record;
        const auto status = impl_->db_->Get(
            impl_->ro_, impl_->journal_, KeySlice(key.value()), &record);
        if (status.IsNotFound())
        {
            // No journal record: absence is reported, never a fabricated
            // default state value.
            return BlockStatus{StatusClass::NotFound, BlockSource::Local,
                               "no journal record for the CID"};
        }
        if (!status.ok())
        {
            return MapStatus(status, "GetUploadState: record read failed");
        }
        // The codec's conservative fallback (PendingUpload) covers any
        // undecodable record - never silently acknowledged (T-02-03).
        return JournalRecord::Decode(rocksdb::Slice(record))
            .value_or(UploadState::PendingUpload);
    }

    result<void, BlockStatus> LocalBlockStore::EvictLocal(const Cid& cid)
    {
        ValidateCidHash(cid);
        if (auto gate = FaultGate(fault_))
        {
            return gate.value();
        }
        auto key = MakeKey(cid);
        if (!key)
        {
            return key.error();
        }
        // Unconditional (D-10): journal state is NEVER consulted, and
        // deleting absent keys succeeds - the post-state (no local bytes
        // remain) holds whether or not anything existed, which is exactly
        // the idempotence contract.
        rocksdb::WriteBatch batch;
        batch.Delete(impl_->blocks_, KeySlice(key.value()));
        batch.Delete(impl_->journal_, KeySlice(key.value()));
        const auto committed = impl_->db_->Write(impl_->wo_, &batch);
        if (!committed.ok())
        {
            return MapStatus(committed, "EvictLocal: removal commit failed");
        }
        return outcome::success();
    }
}
