/**
 * @file MinioServerHarness.cpp
 * @brief Env-gated MinIO server child-process harness implementation
 *        (D-06/D-07/D-09; `#ifdef _WIN32` CreateProcess / POSIX fork-exec).
 */

#include "MinioServerHarness.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include <miniocpp/client.h>
#include <miniocpp/providers.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    using sgns::blockstore::testsupport::MinioServerHarness;

    constexpr int kReadyDeadlineSeconds = 30;
    constexpr int kPollIntervalMs = 250;

    // SHORT relative path under the build tree (Windows MAX_PATH); a
    // monotonic counter gives every harness instance its own directory.
    std::string NextUniqueDir()
    {
        static std::atomic<uint64_t> next{0};
        return "test-data/minio-tier/case-"
             + std::to_string(next.fetch_add(1));
    }

    std::string RandomAlnum(size_t length)
    {
        static const char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        std::random_device device;
        std::string out;
        out.reserve(length);
        for (size_t i = 0; i < length; ++i)
        {
            out.push_back(
                kAlphabet[device() % (sizeof(kAlphabet) - 1)]);
        }
        return out;
    }

    /// @brief Grab a free ephemeral port on 127.0.0.1 (bind :0, read back,
    ///        close). Small TOCTOU race is accepted for the opt-in tier.
    bool ProbeEphemeralPort(uint16_t* port)
    {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        bool ok = false;
        const auto socket_handle =
            ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_handle != static_cast<decltype(socket_handle)>(-1))
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(0);
            if (bind(socket_handle,
                     reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)) == 0)
            {
                socklen_t length = sizeof(address);
                if (getsockname(socket_handle,
                                reinterpret_cast<sockaddr*>(&address),
                                &length) == 0)
                {
                    *port = ntohs(address.sin_port);
                    ok = true;
                }
            }
#ifdef _WIN32
            closesocket(socket_handle);
#else
            close(socket_handle);
#endif
        }
        return ok;
    }

    /// @brief Deadline-bounded liveness poll of the MinIO health endpoint:
    ///        TCP connect + "GET /minio/health/live" until a 200 status line.
    ///        Never a fixed sleep (Phase-1 discipline).
    bool WaitUntilHealthy(const std::string& host, uint16_t port,
                          bool tls, int deadline_seconds)
    {
        (void)tls; // plain socket probe: TLS-off connect check is sufficient
        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(deadline_seconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
#ifdef _WIN32
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
#endif
            const auto socket_handle = ::socket(AF_INET, SOCK_STREAM, 0);
            if (socket_handle != static_cast<decltype(socket_handle)>(-1))
            {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = inet_addr(host.c_str());
                address.sin_port = htons(port);
                if (connect(socket_handle,
                            reinterpret_cast<sockaddr*>(&address),
                            sizeof(address)) == 0)
                {
                    const std::string request =
                        "GET /minio/health/live HTTP/1.0\r\n"
                        "Host: " + host + "\r\n\r\n";
                    send(socket_handle, request.data(),
                         static_cast<int>(request.size()), 0);
                    char response[256] = {0};
                    const int received =
                        recv(socket_handle, response, sizeof(response) - 1, 0);
#ifdef _WIN32
                    closesocket(socket_handle);
#else
                    close(socket_handle);
#endif
                    if (received > 0
                        && std::string(response).find(" 200 ") != std::string::npos)
                    {
                        return true;
                    }
                }
                else
                {
#ifdef _WIN32
                    closesocket(socket_handle);
#else
                    close(socket_handle);
#endif
                }
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kPollIntervalMs));
        }
        return false;
    }

    /// @brief Resolve an openssl binary: SGNS_BLOCKSTORE_TEST_OPENSSL_BIN,
    ///        else the superbuild's openssl install prefix (relative to this
    ///        test binary, .../<superbuild>/openssl), else PATH lookup.
    std::string ResolveOpenssl()
    {
        if (const char* env = std::getenv("SGNS_BLOCKSTORE_TEST_OPENSSL_BIN"))
        {
            if (std::filesystem::exists(env))
            {
                return env;
            }
        }
        namespace fs = std::filesystem;
        // Superbuild openssl install prefix relative to the ctest working
        // directory (.../<superbuild>/sgns-blockstore/src/sgns-blockstore-build/test):
        // ../../../../../openssl/bin/openssl[.exe]
        const fs::path superbuild_candidate = fs::path(
            "../../../../../openssl/bin/openssl"
#ifdef _WIN32
            ".exe"
#endif
        );
        std::error_code ec;
        if (fs::exists(superbuild_candidate, ec))
        {
            return fs::absolute(superbuild_candidate, ec).string();
        }
        return std::string();
    }

    /// @brief Generate a self-signed cert/key pair for the TLS-on smoke (Q2).
    bool GenerateSelfSignedCert(const std::string& openssl_bin,
                                const std::string& certs_dir,
                                std::string* error)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(certs_dir, ec);
        const fs::path key = fs::path(certs_dir) / "private.key";
        const fs::path crt = fs::path(certs_dir) / "public.crt";
        const std::string command = "\"" + openssl_bin + "\" req -x509 -newkey "
            "rsa:2048 -nodes -days 1 "
            "-subj \"/CN=127.0.0.1\" "
            "-addext \"subjectAltName=IP:127.0.0.1\" "
            "-keyout \"" + key.string() + "\" "
            "-out \"" + crt.string() + "\" 2>nul";
        const int exit_code = std::system(command.c_str());
        if (exit_code != 0 || !fs::exists(key, ec) || !fs::exists(crt, ec))
        {
            if (error != nullptr)
            {
                *error = "openssl cert generation failed (exit "
                       + std::to_string(exit_code) + ")";
            }
            return false;
        }
        return true;
    }

#ifdef _WIN32
    /// @brief Spawn a detached child with the current environment (the
    ///        per-run random MINIO_ROOT_* values were set before the call).
    bool SpawnChild(const std::string& command_line, void** child)
    {
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::string mutable_command = command_line;
        if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                            FALSE, 0, nullptr, nullptr, &startup, &process))
        {
            return false;
        }
        CloseHandle(process.hThread);
        *child = process.hProcess;
        return true;
    }

    void TerminateChild(void* child)
    {
        TerminateProcess(static_cast<HANDLE>(child), 1);
        WaitForSingleObject(static_cast<HANDLE>(child), 10000);
        CloseHandle(static_cast<HANDLE>(child));
    }
#else
    bool SpawnChild(const std::string& command_line, void** child)
    {
        const pid_t pid = fork();
        if (pid == 0)
        {
            execl("/bin/sh", "sh", "-c", command_line.c_str(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }
        if (pid < 0)
        {
            return false;
        }
        *child = new pid_t(pid);
        return true;
    }

    void TerminateChild(void* child)
    {
        auto* pid = static_cast<pid_t*>(child);
        kill(*pid, SIGTERM);
        int status = 0;
        waitpid(*pid, &status, 0);
        delete pid;
    }
#endif
} // namespace

namespace sgns::blockstore::testsupport
{
    bool MinioServerHarness::Available()
    {
        const std::string path = BinaryPath();
        if (path.empty())
        {
            return false;
        }
        std::error_code ec;
        return std::filesystem::exists(path, ec);
    }

    std::string MinioServerHarness::BinaryPath()
    {
        const char* env = std::getenv("SGNS_BLOCKSTORE_TEST_MINIO_BIN");
        return env != nullptr ? std::string(env) : std::string();
    }

    MinioServerHarness::~MinioServerHarness()
    {
        Stop();
    }

    bool MinioServerHarness::Start(bool tls, std::string* error)
    {
        const std::string binary = BinaryPath();
        if (binary.empty() || !Available())
        {
            if (error != nullptr)
            {
                *error = "SGNS_BLOCKSTORE_TEST_MINIO_BIN not set or missing";
            }
            return false;
        }

        if (!ProbeEphemeralPort(&port_))
        {
            if (error != nullptr)
            {
                *error = "ephemeral port probe failed";
            }
            return false;
        }

        data_dir_ = NextUniqueDir();
        std::error_code ec;
        std::filesystem::create_directories(data_dir_, ec);

        // Per-run random credentials (>= 8 chars, threat T-3-01); never
        // logged, never committed constants.
        access_key_ = "sgns-" + RandomAlnum(8);
        secret_key_ = RandomAlnum(16);

        std::string command = "\"" + binary + "\" server \"" + data_dir_
            + "\" --address 127.0.0.1:" + std::to_string(port_);
        if (tls)
        {
            const std::string openssl_bin = ResolveOpenssl();
            if (openssl_bin.empty())
            {
                if (error != nullptr)
                {
                    *error = "no openssl resolvable (set "
                             "SGNS_BLOCKSTORE_TEST_OPENSSL_BIN or build the "
                             "superbuild openssl)";
                }
                return false;
            }
            certs_dir_ = (std::filesystem::path(data_dir_) / "certs").string();
            if (!GenerateSelfSignedCert(openssl_bin, certs_dir_, error))
            {
                return false;
            }
            command += " --certs-dir \"" + certs_dir_ + "\"";
        }

        // Credentials travel to the child via the process environment only.
#ifdef _WIN32
        SetEnvironmentVariableA("MINIO_ROOT_USER", access_key_.c_str());
        SetEnvironmentVariableA("MINIO_ROOT_PASSWORD", secret_key_.c_str());
#else
        setenv("MINIO_ROOT_USER", access_key_.c_str(), 1);
        setenv("MINIO_ROOT_PASSWORD", secret_key_.c_str(), 1);
#endif
        if (!SpawnChild(command, &child_))
        {
            if (error != nullptr)
            {
                *error = "spawn failed: " + command;
            }
            return false;
        }
        running_ = true;
        tls_ = tls;

        if (!WaitUntilHealthy("127.0.0.1", port_, tls, kReadyDeadlineSeconds))
        {
            // Health poll failure: tear the child down before reporting.
            Stop();
            if (error != nullptr)
            {
                *error = "MinIO did not become healthy within "
                       + std::to_string(kReadyDeadlineSeconds) + "s";
            }
            return false;
        }
        return true;
    }

    void MinioServerHarness::Stop()
    {
        if (!running_)
        {
            return;
        }
        running_ = false;
        if (child_ != nullptr)
        {
            TerminateChild(child_); // terminate -> wait -> close (T-3-06)
            child_ = nullptr;
        }
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec); // idempotent cleanup
    }

    bool MinioServerHarness::MakeBucket(const std::string& name,
                                        std::string* error)
    {
        minio::s3::BaseUrl base_url("127.0.0.1", /*https=*/tls_, "us-east-1");
        base_url.port = port_;
        minio::creds::StaticProvider provider(access_key_, secret_key_);
        minio::s3::Client client(base_url, &provider);
        if (tls_)
        {
            client.IgnoreCertCheck(true); // self-signed fixture (Q2)
        }

        minio::s3::MakeBucketArgs args;
        args.bucket = name;
        args.region = "us-east-1";
        const auto result = client.MakeBucket(args);
        if (!result)
        {
            if (error != nullptr)
            {
                *error = result.error().String();
            }
            return false;
        }
        return true;
    }
} // namespace sgns::blockstore::testsupport
