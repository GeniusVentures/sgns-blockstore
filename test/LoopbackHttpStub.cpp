/**
 * @file LoopbackHttpStub.cpp
 * @brief Raw-socket scripted HTTP server implementation (D-08, D-11 evidence).
 */

#include "LoopbackHttpStub.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{
#ifdef _WIN32
    using SocketHandle = SOCKET;
    constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

    void EnsureWinsockOnce()
    {
        static std::once_flag flag;
        std::call_once(flag, [] {
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
        });
    }

    int CloseSocket(SocketHandle s)
    {
        return closesocket(s);
    }
#else
    using SocketHandle = int;
    constexpr SocketHandle kInvalidSocket = -1;

    void EnsureWinsockOnce()
    {
    }

    int CloseSocket(SocketHandle s)
    {
        return close(s);
    }
#endif

    constexpr int kReceiveTimeoutSeconds = 10;

    /**
     * @brief Read one full HTTP request: headers until CRLFCRLF, then the
     *        Content-Length body. A receive timeout ends the read early so
     *        a malformed client cannot wedge the accept loop.
     */
    bool ReadFullRequest(SocketHandle socket, std::string* out)
    {
        std::string raw;
        char buffer[4096];
        size_t header_end = std::string::npos;
        size_t content_length = 0;
        while (true)
        {
            const int received =
                recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received > 0)
            {
                raw.append(buffer, static_cast<size_t>(received));
                if (header_end == std::string::npos)
                {
                    header_end = raw.find("\r\n\r\n");
                    if (header_end != std::string::npos)
                    {
                        const std::string headers =
                            raw.substr(0, header_end);
                        size_t pos = headers.find("Content-Length:");
                        size_t value_pos = (pos == std::string::npos)
                            ? headers.find("content-length:")
                            : pos;
                        if (value_pos != std::string::npos)
                        {
                            // A garbage Content-Length value (one of the
                            // stub's own fault shapes) must not throw out
                            // of the accept loop - parse defensively.
                            try
                            {
                                content_length = static_cast<size_t>(
                                    std::stoul(headers.substr(
                                        headers.find(':', value_pos) + 1)));
                            }
                            catch (const std::exception&)
                            {
                                content_length = 0;
                            }
                        }
                    }
                }
                if (header_end != std::string::npos
                    && raw.size() >= header_end + 4 + content_length)
                {
                    break; // headers + declared body fully received
                }
                continue;
            }
            if (received == 0)
            {
                break; // client closed - request is as complete as it gets
            }
            {
                // recv() error or receive timeout: serve what arrived so a
                // truncated/garbage request still yields a scripted response.
                break;
            }
        }
        *out = raw;
        return !raw.empty();
    }

    void SendAll(SocketHandle socket, const std::string& bytes)
    {
        size_t sent_total = 0;
        while (sent_total < bytes.size())
        {
            const int sent = send(socket, bytes.data() + sent_total,
                                  static_cast<int>(bytes.size() - sent_total),
                                  0);
            if (sent <= 0)
            {
                return;
            }
            sent_total += static_cast<size_t>(sent);
        }
    }

    std::string DefaultOk200()
    {
        return "HTTP/1.1 200 OK\r\n"
               "ETag: \"loopback-stub-etag\"\r\n"
               "Content-Length: 0\r\n"
               "Connection: close\r\n"
               "\r\n";
    }
} // namespace

namespace sgns::blockstore::testsupport
{
    LoopbackHttpStub::~LoopbackHttpStub()
    {
        Stop();
    }

    bool LoopbackHttpStub::Start(std::string* error)
    {
        EnsureWinsockOnce();
        if (running_)
        {
            return true;
        }

        SocketHandle listen_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_socket == kInvalidSocket)
        {
            if (error != nullptr)
            {
                *error = "socket() failed";
            }
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 ONLY
        address.sin_port = htons(0);                      // ephemeral port
        if (bind(listen_socket,
                 reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) != 0)
        {
            if (error != nullptr)
            {
                *error = "bind(127.0.0.1:0) failed";
            }
            CloseSocket(listen_socket);
            return false;
        }
        socklen_t address_length = sizeof(address);
        if (getsockname(listen_socket,
                        reinterpret_cast<sockaddr*>(&address),
                        &address_length) != 0)
        {
            if (error != nullptr)
            {
                *error = "getsockname() failed";
            }
            CloseSocket(listen_socket);
            return false;
        }
        if (listen(listen_socket, 8) != 0)
        {
            if (error != nullptr)
            {
                *error = "listen() failed";
            }
            CloseSocket(listen_socket);
            return false;
        }

        listen_socket_ = static_cast<uintptr_t>(listen_socket);
        port_ = ntohs(address.sin_port);
        stop_requested_ = false;
        running_ = true;
        thread_ = std::thread([this] { AcceptLoop_(); });
        return true;
    }

    void LoopbackHttpStub::Stop()
    {
        if (!running_)
        {
            return;
        }
        running_ = false;
        stop_requested_ = true;
        if (listen_socket_ != 0)
        {
            // Unblocks the blocking accept() inside the accept loop.
            CloseSocket(static_cast<SocketHandle>(listen_socket_));
            listen_socket_ = 0;
        }
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    void LoopbackHttpStub::EnqueueResponse(const std::string& raw_response)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        responses_.push_back(raw_response);
    }

    void LoopbackHttpStub::EnqueueOk200()
    {
        EnqueueResponse(DefaultOk200());
    }

    size_t LoopbackHttpStub::RequestCount() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return captures_.size();
    }

    std::string LoopbackHttpStub::CapturedRequest(size_t index) const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return index < captures_.size() ? captures_[index] : std::string();
    }

    void LoopbackHttpStub::AcceptLoop_()
    {
        while (!stop_requested_)
        {
            SocketHandle client =
                accept(static_cast<SocketHandle>(listen_socket_), nullptr, nullptr);
            if (client == kInvalidSocket)
            {
                break; // listen socket closed by Stop()
            }
            ServeConnection_(static_cast<uintptr_t>(client));
        }
    }

    void LoopbackHttpStub::ServeConnection_(uintptr_t socket_handle)
    {
        SocketHandle client = static_cast<SocketHandle>(socket_handle);

        // Bound the read so a truncated/garbage request still gets served a
        // scripted response instead of wedging the stub's single thread.
#ifdef _WIN32
        DWORD timeout_ms = kReceiveTimeoutSeconds * 1000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms),
                   sizeof(timeout_ms));
#else
        timeval timeout{};
        timeout.tv_sec = kReceiveTimeoutSeconds;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

        std::string request;
        ReadFullRequest(client, &request);

        std::string response;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            captures_.push_back(request);
            response = responses_.empty() ? DefaultOk200()
                                          : responses_.front();
            if (!responses_.empty())
            {
                responses_.pop_front();
            }
        }

        SendAll(client, response);
#ifdef _WIN32
        shutdown(client, SD_BOTH);
#else
        shutdown(client, SHUT_RDWR);
#endif
        CloseSocket(client);
    }
} // namespace sgns::blockstore::testsupport
