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

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

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

#ifdef _WIN32
    using SocketHandle = SOCKET;
#else
    using SocketHandle = int;
#endif

    /// @brief One TLS-wrapped health exchange over an ALREADY-CONNECTED
    ///        socket: SSL handshake, then the same health request /
    ///        " 200 " status-line check as the plaintext probe. Community
    ///        MinIO serves TLS-only on the API port once --certs-dir is
    ///        configured, so the TLS-on readiness poll must speak TLS
    ///        (user-approved option (a), G-3-1 closure). Certificate
    ///        verification is disabled (SSL_VERIFY_NONE): the fixture
    ///        serves the in-process self-signed cert, mirroring the
    ///        client-side IgnoreCertCheck(true) posture (Q2). The caller
    ///        owns the socket and closes it; both OpenSSL handles are
    ///        freed on every path (the poll re-invokes this every
    ///        250 ms up to the deadline - leaks would compound).
    /// @param socket_handle connected TCP socket to the MinIO API port
    /// @param request HTTP/1.0 health-request bytes to send
    /// @return true when the response status line contains " 200 "
    bool TlsHealthExchange(SocketHandle socket_handle,
                           const std::string& request)
    {
        SSL_CTX* context = SSL_CTX_new(TLS_client_method());
        if (context == nullptr)
        {
            return false;
        }
        SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
        SSL* ssl = SSL_new(context);
        if (ssl == nullptr)
        {
            SSL_CTX_free(context);
            return false;
        }
        bool healthy = false;
        if (SSL_set_fd(ssl, static_cast<int>(socket_handle)) == 1
            && SSL_connect(ssl) == 1)
        {
            if (SSL_write(ssl, request.data(),
                          static_cast<int>(request.size()))
                > 0)
            {
                char response[256] = {0};
                const int received =
                    SSL_read(ssl, response, sizeof(response) - 1);
                healthy = received > 0
                    && std::string(response).find(" 200 ")
                           != std::string::npos;
            }
        }
        SSL_shutdown(ssl); // best-effort close_notify
        SSL_free(ssl);
        SSL_CTX_free(context);
        return healthy;
    }

    /// @brief Deadline-bounded liveness poll of the MinIO health endpoint:
    ///        TCP connect + "GET /minio/health/live" until a 200 status
    ///        line - plaintext when @p tls is false (byte-identical to
    ///        the historical probe), TLS-wrapped via TlsHealthExchange
    ///        when @p tls is true (the API port is TLS-only once
    ///        --certs-dir is configured). Never a fixed sleep (Phase-1
    ///        discipline).
    bool WaitUntilHealthy(const std::string& host, uint16_t port,
                          bool tls, int deadline_seconds)
    {
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
                    bool healthy = false;
                    if (tls)
                    {
                        healthy = TlsHealthExchange(socket_handle,
                                                    request);
                    }
                    else
                    {
                        send(socket_handle, request.data(),
                             static_cast<int>(request.size()), 0);
                        char response[256] = {0};
                        const int received =
                            recv(socket_handle, response,
                                 sizeof(response) - 1, 0);
                        healthy = received > 0
                            && std::string(response).find(" 200 ")
                                   != std::string::npos;
                    }
#ifdef _WIN32
                    closesocket(socket_handle);
#else
                    close(socket_handle);
#endif
                    if (healthy)
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

    bool MinioServerHarness::GenerateSelfSignedCert(
        const std::string& certs_dir, std::string* error)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(certs_dir, ec);
        const fs::path key_path = fs::path(certs_dir) / "private.key";
        const fs::path crt_path = fs::path(certs_dir) / "public.crt";

        // One OpenSSL error pop per failure path; the fallback covers
        // branches where the error queue is empty (the failure is ours,
        // not OpenSSL's).
        const auto Fail = [error](const char* fallback) {
            if (error != nullptr)
            {
                const char* reason =
                    ERR_reason_error_string(ERR_get_error());
                *error = std::string("cert generation failed: ")
                    + (reason != nullptr ? reason : fallback);
            }
            return false;
        };

        // Ephemeral RSA-2048 pair from the CSPRNG; per-run, protecting
        // only the loopback server (threat T-3-07).
        EVP_PKEY* pkey = EVP_RSA_gen(2048);
        if (pkey == nullptr)
        {
            return Fail("RSA key generation failed");
        }
        X509* certificate = X509_new();
        if (certificate == nullptr)
        {
            EVP_PKEY_free(pkey);
            return Fail("X509 allocation failed");
        }

        bool ok = X509_set_version(certificate, 2) == 1; // X509 v3
        if (ok)
        {
            // Random positive serial (CSPRNG bytes; std::random_device
            // already drives the per-run credentials above).
            uint64_t serial = 0;
            for (int i = 0; i < 8; ++i)
            {
                serial = (serial << 8)
                    | (std::random_device{}() & 0xFF);
            }
            serial &= 0x7FFFFFFFFFFFFFFFULL; // positive (sign bit clear)
            serial |= 1ULL;                  // non-zero
            ok = ASN1_INTEGER_set_uint64(
                     X509_get_serialNumber(certificate), serial)
                == 1;
        }
        // 5-minute clock-skew margin back-dating; ~1-day validity
        // (matches the old "-days 1" fixture).
        ok = ok
            && X509_gmtime_adj(X509_getm_notBefore(certificate), -300)
                != nullptr;
        ok = ok
            && X509_gmtime_adj(X509_getm_notAfter(certificate), 24 * 3600)
                != nullptr;
        if (ok)
        {
            // Self-signed: subject and issuer are the same name.
            X509_NAME* name = X509_get_subject_name(certificate);
            ok = X509_NAME_add_entry_by_txt(
                     name, "CN", MBSTRING_ASC,
                     reinterpret_cast<const unsigned char*>("127.0.0.1"),
                     -1, -1, 0)
                    == 1
                && X509_set_issuer_name(certificate, name) == 1
                && X509_set_pubkey(certificate, pkey) == 1;
        }
        if (ok)
        {
            // SAN IP:127.0.0.1 (the harness endpoint address).
            X509V3_CTX context{};
            X509V3_set_ctx(&context, nullptr, certificate, nullptr,
                           nullptr, 0);
            X509_EXTENSION* extension = X509V3_EXT_conf_nid(
                nullptr, &context, NID_subject_alt_name, "IP:127.0.0.1");
            if (extension != nullptr)
            {
                ok = X509_add_ext(certificate, extension, -1) == 1;
                X509_EXTENSION_free(extension);
            }
            else
            {
                ok = false;
            }
        }
        if (ok)
        {
            ok = X509_sign(certificate, pkey, EVP_sha256()) > 0;
        }

        if (ok)
        {
            BIO* key_bio = BIO_new_file(key_path.string().c_str(), "wb");
            if (key_bio != nullptr)
            {
                ok = PEM_write_bio_PrivateKey(key_bio, pkey, nullptr,
                                              nullptr, 0, nullptr,
                                              nullptr)
                    == 1;
                BIO_free(key_bio);
            }
            else
            {
                ok = false;
            }
        }
        if (ok)
        {
            BIO* crt_bio = BIO_new_file(crt_path.string().c_str(), "wb");
            if (crt_bio != nullptr)
            {
                ok = PEM_write_bio_X509(crt_bio, certificate) == 1;
                BIO_free(crt_bio);
            }
            else
            {
                ok = false;
            }
        }

        EVP_PKEY_free(pkey);
        X509_free(certificate);
        if (!ok)
        {
            return Fail("certificate write failed");
        }

        // Postcondition (same check the shell-out implementation used):
        // both files exist and are non-empty.
        std::error_code size_ec;
        const bool files_ok = fs::exists(key_path, size_ec)
            && fs::exists(crt_path, size_ec)
            && fs::file_size(key_path, size_ec) > 0
            && fs::file_size(crt_path, size_ec) > 0;
        if (!files_ok)
        {
            if (error != nullptr)
            {
                *error = "cert generation failed: written files missing "
                         "or empty";
            }
            return false;
        }
        if (error != nullptr)
        {
            *error = "";
        }
        return true;
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
            certs_dir_ = (std::filesystem::path(data_dir_) / "certs").string();
            if (!GenerateSelfSignedCert(certs_dir_, error))
            {
                return false; // *error already labelled by the generator
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
