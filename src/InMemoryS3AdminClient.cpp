/**
 * @file InMemoryS3AdminClient.cpp
 * @brief Fake-body for the hermetic S3AdminClient seam (DUR-01,
 *        Phase 10 D-05). Test-surface only - never linked by any
 *        production path (the header is deliberately NOT installed).
 */

#include "InMemoryS3AdminClient.hpp"

namespace sgns::blockstore
{
    void InMemoryS3AdminClient::SetStats( UploadStats stats )
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        stats_ = stats;
    }

    void InMemoryS3AdminClient::SetAdminFault( S3AdminFaultKind kind )
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        fault_ = kind;
    }

    void InMemoryS3AdminClient::SetSlowFlushDelayMs( uint32_t delay_ms )
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        slow_flush_delay_ms_ = delay_ms;
    }

    uint64_t InMemoryS3AdminClient::flush_count() const
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        return flush_count_;
    }

    uint64_t InMemoryS3AdminClient::stats_count() const
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        return stats_count_;
    }

    void InMemoryS3AdminClient::SetOnFlush( std::function<void()> hook )
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        on_flush_ = std::move( hook );
    }

    BlockStatus InMemoryS3AdminClient::FaultFor( S3AdminFaultKind kind )
    {
        // Every scripted admin fault is an adapter-level taxonomy
        // OUTCOME, never a wire shape - and never a NotFound (the
        // frozen taxonomy: absence is not what an admin fault means).
        return BlockStatus{ StatusClass::TemporarilyUnavailable,
                            BlockSource::Remote,
                            "scripted admin fault (kind="
                                + std::to_string( static_cast<int>( kind ) ) + ")" };
    }

    result<void, BlockStatus> InMemoryS3AdminClient::Flush(
        std::chrono::milliseconds read_timeout_budget )
    {
        S3AdminFaultKind fault;
        uint32_t         delay_ms;
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock( mutex_ );
            ++flush_count_; // call probe: counts ATTEMPTS, faulted or not
            fault    = fault_;
            delay_ms = slow_flush_delay_ms_;
            hook     = on_flush_;
        }
        // The scripted delay sleeps OUTSIDE the mutex so scripting from
        // the test thread is never blocked by an in-flight slow flush.
        // Budget-faithful slow flush (v1.1 Phase 10 plan 10-02): a delay
        // beyond the caller's read budget surfaces as the client-side
        // TIMEOUT the real client would produce after the budget elapses
        // - the fake honors the contract, so bounded-stop proofs work
        // through it.
        if ( delay_ms > 0 )
        {
            const auto budget_ms = static_cast<uint32_t>( read_timeout_budget.count() );
            if ( delay_ms > budget_ms && budget_ms > 0 )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( budget_ms ) );
                return BlockStatus{ StatusClass::TemporarilyUnavailable,
                                    BlockSource::Remote,
                                    "scripted slow flush exceeded the read budget (timeout)" };
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( delay_ms ) );
        }
        if ( fault == S3AdminFaultKind::Unreachable || fault == S3AdminFaultKind::FlushFails )
        {
            return FaultFor( fault );
        }
        if ( hook )
        {
            hook();
        }
        return outcome::success();
    }

    result<UploadStats, BlockStatus> InMemoryS3AdminClient::GetUploadStats()
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        ++stats_count_;
        if ( fault_ == S3AdminFaultKind::Unreachable
             || fault_ == S3AdminFaultKind::GarbageStats )
        {
            return FaultFor( fault_ );
        }
        return stats_;
    }
} // namespace sgns::blockstore
