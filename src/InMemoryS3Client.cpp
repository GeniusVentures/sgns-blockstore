/**
 * @file InMemoryS3Client.cpp
 * @brief Stateful hermetic fake of the S3Client seam (D-05) - test-only.
 */

#include "InMemoryS3Client.hpp"

#include <utility>

namespace
{
    constexpr size_t OpIndex(sgns::blockstore::S3Op op)
    {
        return static_cast<size_t>(op);
    }
} // namespace

namespace sgns::blockstore
{
    void InMemoryS3Client::SetFault(S3Op op, S3FaultKind kind)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        faults_[OpIndex(op)] = kind;
    }

    void InMemoryS3Client::ClearFaults()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        faults_.fill(S3FaultKind::None);
    }

    std::vector<std::string> InMemoryS3Client::ObjectKeys() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        std::vector<std::string> keys;
        keys.reserve(objects_.size());
        for (const auto& entry : objects_) // std::map iterates in key order
        {
            keys.push_back(entry.first);
        }
        return keys; // already sorted (std::map's ordering guarantee)
    }

    std::optional<BlockStatus> InMemoryS3Client::FaultGate(S3Op op) const
    {
        switch (faults_[OpIndex(op)])
        {
            case S3FaultKind::Unavailable:
                return BlockStatus{StatusClass::TemporarilyUnavailable,
                                   BlockSource::Remote,
                                   "fault injection: S3 backend unavailable"};
            case S3FaultKind::CorruptData:
                return BlockStatus{StatusClass::Corrupt,
                                   BlockSource::Remote,
                                   "fault injection: S3 backend corrupt data"};
            case S3FaultKind::Unauthorized:
                return BlockStatus{StatusClass::Unauthorized,
                                   BlockSource::Remote,
                                   "fault injection: S3 backend authorization failure"};
            case S3FaultKind::None:
                break;
        }
        return std::nullopt;
    }

    result<S3PutResult, BlockStatus> InMemoryS3Client::Put(
        const std::string& key, const std::vector<uint8_t>& bytes,
        const PutObjectOptions& options)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (auto gate = FaultGate(S3Op::Put))
        {
            return gate.value();
        }
        const bool already_present = objects_.find(key) != objects_.end();
        if (options.createOnly && already_present)
        {
            // D-01/D-02: conditional-create hit - SUCCESS slot, original
            // bytes untouched (never a failure, never an overwrite).
            return S3PutResult{ /*achieved=*/true, /*alreadyPresent=*/true };
        }
        objects_[key] = bytes;
        return S3PutResult{ /*achieved=*/true, already_present };
    }

    result<S3Object, BlockStatus> InMemoryS3Client::Get(const std::string& key)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (auto gate = FaultGate(S3Op::Get))
        {
            return gate.value();
        }
        auto it = objects_.find(key);
        if (it == objects_.end())
        {
            return BlockStatus{StatusClass::NotFound, BlockSource::Remote,
                               "S3 object not found: " + key};
        }
        return S3Object{ key, it->second };
    }

    result<S3HeadResult, BlockStatus> InMemoryS3Client::Head(
        const std::string& key)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (auto gate = FaultGate(S3Op::Head))
        {
            return gate.value();
        }
        auto it = objects_.find(key);
        if (it == objects_.end())
        {
            // Authoritative absence: SUCCESS slot exists=false - an error
            // here would mean the answer could not be obtained.
            return S3HeadResult{ /*exists=*/false, /*size=*/0 };
        }
        return S3HeadResult{ /*exists=*/true, it->second.size() };
    }

    result<void, BlockStatus> InMemoryS3Client::Delete(const std::string& key)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (auto gate = FaultGate(S3Op::Delete))
        {
            return gate.value();
        }
        objects_.erase(key); // idempotent: missing key is success
        return outcome::success();
    }
} // namespace sgns::blockstore
