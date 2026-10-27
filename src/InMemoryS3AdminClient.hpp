/**
 * @file InMemoryS3AdminClient.hpp
 * @brief Stateful hermetic fake of the S3AdminClient seam (DUR-01,
 *        Phase 10 D-05).
 *
 * A test-only implementation: scriptable stats, scriptable admin faults,
 * a scripted slow-flush delay, per-op call counters, and an optional
 * flush hook. It is NEVER read by production paths - the scripting
 * surface exists so the flush-cadence behaviors (floor, backstop,
 * evidence gate, failure backoff) can be induced deterministically.
 *
 * The fake does not transport anything: Unreachable/FlushFails/
 * GarbageStats map directly to the adapter-level taxonomy OUTCOMES the
 * real client classifies from the wire (LoopbackHttpStub is the
 * wire-verification tier against the real client, plan 10-02).
 */

#ifndef SGNS_BLOCKSTORE_INMEMORYS3ADMINCLIENT_HPP
#define SGNS_BLOCKSTORE_INMEMORYS3ADMINCLIENT_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <sgns-blockstore/S3AdminClient.hpp>

namespace sgns::blockstore
{
    /**
     * @brief The in-memory S3AdminClient fake (D-05).
     * @note No-data-race-by-construction: a small mutable mutex guards
     *       the scripted state and the counters, exactly like the
     *       InMemoryS3Client pattern (the manager calls from the drainer
     *       thread while tests script from the test thread).
     */
    class InMemoryS3AdminClient final : public S3AdminClient
    {
    public:
        InMemoryS3AdminClient() = default;
        ~InMemoryS3AdminClient() override = default;

        /**
         * @brief Script the stats every GetUploadStats() serves
         *        (defaults to an all-zero UploadStats - healthy).
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        void SetStats( UploadStats stats );

        /**
         * @brief Script an admin fault (persists until cleared).
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        void SetAdminFault( S3AdminFaultKind kind );

        /**
         * @brief Script a delay served INSIDE Flush before it succeeds
         *        (the fake sleeps; the real client would be bounded by
         *        the caller's read budget - tests keep delays tiny).
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        void SetSlowFlushDelayMs( uint32_t delay_ms );

        /**
         * @brief Number of Flush CALLS served (including faulted ones) -
         *        the cadence probe (floor/backstop/backoff assertions).
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        uint64_t flush_count() const;

        /// @brief Number of GetUploadStats calls served.
        /// @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
        uint64_t stats_count() const;

        /**
         * @brief Optional hook invoked after every (non-faulted-at-entry)
         *        Flush completes - the barrier point tests use to observe
         *        an in-flight flush from the scripting thread.
         * @note CONFORMANCE-TEST SEAM: production code MUST NOT call this.
         */
        void SetOnFlush( std::function<void()> hook );

        result<void, BlockStatus> Flush(
            std::chrono::milliseconds read_timeout_budget ) override;

        result<UploadStats, BlockStatus> GetUploadStats() override;

    private:
        /// @brief Taxonomy outcome for the scripted fault kind.
        static BlockStatus FaultFor( S3AdminFaultKind kind );

        mutable std::mutex     mutex_;
        UploadStats            stats_{};
        S3AdminFaultKind       fault_ = S3AdminFaultKind::None;
        uint32_t               slow_flush_delay_ms_ = 0;
        uint64_t               flush_count_ = 0;
        uint64_t               stats_count_ = 0;
        std::function<void()>  on_flush_;
    };
} // namespace sgns::blockstore

#endif // SGNS_BLOCKSTORE_INMEMORYS3ADMINCLIENT_HPP
