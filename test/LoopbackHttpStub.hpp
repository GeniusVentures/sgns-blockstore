/**
 * @file LoopbackHttpStub.hpp
 * @brief Raw-socket scripted HTTP server for wire-level test evidence (D-08).
 *
 * The transport-flavored equivalent of the crash suite's fault forge
 * (local_block_store_crash_test.cpp CorruptStoredBlock): it produces
 * GENUINE wire-level bytes, not simulated outcomes. Accepts one connection
 * at a time on 127.0.0.1 ONLY (threat T-3-05 - never a wildcard bind),
 * reads a full HTTP request, appends the raw request bytes to a capture
 * buffer (the D-11 header-on-the-wire evidence), then sends the next
 * scripted response and closes the connection.
 *
 * Scripted responses are a FIFO queue (FaultGate scripting philosophy):
 * when the queue is empty a bare 200 OK is served. Responses are raw wire
 * bytes, so a script can serve any fault shape - XML-body 412, empty-body
 * 412, bare 5xx, deliberately truncated bodies, or garbage bytes.
 */

#ifndef SGNS_BLOCKSTORE_TEST_LOOPBACKHTTPSTUB_HPP
#define SGNS_BLOCKSTORE_TEST_LOOPBACKHTTPSTUB_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sgns::blockstore::testsupport
{
    /**
     * @brief Self-contained raw-socket HTTP stub bound to 127.0.0.1.
     * @note No new library dependencies beyond ws2_32 on Windows; socket
     *       handles are stored as uintptr_t so this header stays portable.
     */
    class LoopbackHttpStub
    {
    public:
        LoopbackHttpStub() = default;
        ~LoopbackHttpStub(); ///< Stop() if still running (idempotent).

        LoopbackHttpStub(const LoopbackHttpStub&) = delete;
        LoopbackHttpStub& operator=(const LoopbackHttpStub&) = delete;

        /**
         * @brief Bind 127.0.0.1 on an ephemeral port and start serving.
         * @param error optional error text on failure
         * @return true when the accept loop is running
         */
        bool Start(std::string* error = nullptr);

        /// @brief Stop the accept loop and close sockets (idempotent).
        void Stop();

        /// @return the bound ephemeral port (valid after Start())
        uint16_t Port() const
        {
            return port_;
        }

        /**
         * @brief Script the raw wire response served to the NEXT accepted
         *        connection (FIFO; a bare 200 OK is served when empty).
         * @param raw_response full HTTP response bytes to send
         */
        void EnqueueResponse(const std::string& raw_response);

        /// @brief Enqueue "HTTP/1.1 200 OK" with an empty body.
        void EnqueueOk200();

        /// @return number of requests captured so far
        size_t RequestCount() const;

        /**
         * @brief Raw request bytes captured for connection @p index,
         *        exactly as they arrived on the wire.
         */
        std::string CapturedRequest(size_t index) const;

    private:
        void AcceptLoop_();
        void ServeConnection_(uintptr_t socket);

        mutable std::mutex mutex_;
        std::deque<std::string> responses_;
        std::vector<std::string> captures_;
        uintptr_t listen_socket_ = 0;
        uint16_t port_ = 0;
        bool running_ = false;
        volatile bool stop_requested_ = false;
        std::thread thread_;
    };
} // namespace sgns::blockstore::testsupport

#endif // SGNS_BLOCKSTORE_TEST_LOOPBACKHTTPSTUB_HPP
