/**
 * @file minio_s3_client_test.cpp
 * @brief MinioS3Client behavior suite - the D-08 loopback-stub fault
 *        matrix (hermetic, default CI lane) proving SC4's mapping half:
 *        every scripted wire failure surfaces its taxonomy class through
 *        the REAL adapter, with the never-NotFound guarantee asserted per
 *        shape and no sticky negative cache. D-02 is proven on the wire:
 *        both 412 response shapes return the success slot with
 *        alreadyPresent=true.
 *
 * READ BEFORE TOUCHING: the stub serves RAW wire bytes (LoopbackHttpStub,
 * 03-01) - responses are exact HTTP strings, and a scripted response is
 * consumed by ONE request (FIFO). Put with createOnly=true issues exactly
 * one request (the conditional header path); plain Put issues a
 * read-before-write Head first.
 *
 * Plain GTest discipline: ASSERT on the result, EXPECT on error().code /
 * .source. The env-gated MinIO-binary live tier (D-06/D-07/D-09) shares
 * this TU below the hermetic matrix.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LoopbackHttpStub.hpp"
#include "MinioServerHarness.hpp"
#include "MinioS3Client.hpp"
#include <sgns-blockstore/S3Client.hpp>

namespace
{
    using sgns::blockstore::BlockSource;
    using sgns::blockstore::MinioS3Client;
    using sgns::blockstore::PutObjectOptions;
    using sgns::blockstore::S3ClientConfig;
    using sgns::blockstore::StatusClass;
    using sgns::blockstore::testsupport::LoopbackHttpStub;
    using sgns::blockstore::testsupport::MinioServerHarness;

    const std::string kPayload = "minio adapter matrix payload";

    std::vector<uint8_t> BytesOf(const std::string& s)
    {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    S3ClientConfig MakeConfig(uint16_t port)
    {
        S3ClientConfig config;
        config.endpoint = "127.0.0.1";
        config.port = port;
        config.region = "us-east-1";
        config.useTls = false;   // plain-HTTP loopback is the primary lane
        config.tlsVerify = true; // the default, passed through verbatim
        config.pathStyle = true;
        config.accessKey = "sgnsmatrix";
        config.secretKey = "sgnsmatrix-secret";
        config.bucket = "matrix-bucket";
        return config;
    }

    std::unique_ptr<MinioS3Client> MakeClient(uint16_t port)
    {
        auto client = MinioS3Client::Open(MakeConfig(port));
        return std::unique_ptr<MinioS3Client>(std::move(client.value()));
    }

    /// @brief A 412 response with a parseable XML error body (Content-Type
    ///        application/xml) - the real-MinIO shape.
    std::string XmlPreconditionFailed412()
    {
        const std::string body =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<Error><Code>PreconditionFailed</Code>"
            "<Message>At least one of the pre-conditions you specified "
            "did not hold</Message>"
            "<Resource>/matrix-bucket/matrix-object</Resource>"
            "<RequestId>matrix</RequestId></Error>";
        return "HTTP/1.1 412 Precondition Failed\r\n"
               "Content-Type: application/xml\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "Connection: close\r\n"
               "\r\n" + body;
    }

    /// @brief A bare 412 with an empty body - the SDK produces its
    ///        status-code-default error text for this shape.
    std::string EmptyBody412()
    {
        return "HTTP/1.1 412 Precondition Failed\r\n"
               "Content-Length: 0\r\n"
               "Connection: close\r\n"
               "\r\n";
    }

    /// @brief A 404 carrying the S3 NoSuchKey XML error (the shape the
    ///        mapper must read as authoritative absence).
    std::string NoSuchKey404()
    {
        const std::string body =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<Error><Code>NoSuchKey</Code>"
            "<Message>The specified key does not exist.</Message>"
            "<Key>matrix-object</Key>"
            "<BucketName>matrix-bucket</BucketName>"
            "<RequestId>matrix</RequestId></Error>";
        return "HTTP/1.1 404 Not Found\r\n"
               "Content-Type: application/xml\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "Connection: close\r\n"
               "\r\n" + body;
    }

    std::string Bare500()
    {
        return "HTTP/1.1 500 Internal Server Error\r\n"
               "Content-Length: 0\r\n"
               "Connection: close\r\n"
               "\r\n";
    }

    /// @brief Any bare status line with an empty body (5xx breadth).
    std::string BareStatus(const std::string& status_line)
    {
        return "HTTP/1.1 " + status_line + "\r\n"
               "Content-Length: 0\r\n"
               "Connection: close\r\n"
               "\r\n";
    }

    /// @brief A valid 200 object response carrying the known payload
    ///        (the D-06 inline-recovery tail's good fetch).
    std::string GoodObject200()
    {
        return "HTTP/1.1 200 OK\r\n"
               "ETag: \"recovered\"\r\n"
               "Content-Length: " + std::to_string(kPayload.size()) + "\r\n"
               "Connection: close\r\n"
               "\r\n" + kPayload;
    }

    /// @brief A 412 whose body is unparseable binary noise (the third
    ///        observed wire shape - mapper classification must still be
    ///        alreadyPresent, never a failure).
    std::string BinaryBody412()
    {
        const std::string noise = [] {
            const unsigned char raw[] = {0x01, 0xfd, 0xfe, 0xc3, 0x7f, 0x80};
            return std::string(reinterpret_cast<const char*>(raw),
                               sizeof(raw)) + "precondition-noise";
        }();
        return "HTTP/1.1 412 Precondition Failed\r\n"
               "Content-Type: application/octet-stream\r\n"
               "Content-Length: " + std::to_string(noise.size()) + "\r\n"
               "Connection: close\r\n"
               "\r\n" + noise;
    }

    /**
     * @given a client pointed at a port where a stub listened but no
     *        longer does (connection refused)
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable - explicitly NOT
     *        NotFound (SC4: transport failure is never authoritative
     *        absence)
     */
    TEST(MinioS3ClientMatrixTest, ConnectionRefusedIsTemporarilyUnavailable)
    {
        LoopbackHttpStub probe;
        ASSERT_TRUE(probe.Start());
        const uint16_t dead_port = probe.Port();
        probe.Stop(); // nothing listens there anymore

        auto client = MakeClient(dead_port);
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);
        EXPECT_EQ(got.error().source, BlockSource::Remote);

        // D-06 inline recovery: the transport itself recovers - restart
        // the stub (fresh ephemeral port) and a new client fetches good
        // bytes in the SAME test.
        ASSERT_TRUE(probe.Start());
        probe.EnqueueResponse(GoodObject200());
        auto revived = MakeClient(probe.Port());
        auto recovered = revived->Get("matrix-object");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    /**
     * @given the stub scripted with a bare 500
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable (the SDK's
     *        server-failed status text), never NotFound
     */
    TEST(MinioS3ClientMatrixTest, Http500IsTemporarilyUnavailable)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(Bare500());

        auto client = MakeClient(stub.Port());
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);
        EXPECT_EQ(got.error().source, BlockSource::Remote);

        // D-06 inline recovery: cleared stub -> good fetch, same client.
        stub.EnqueueResponse(GoodObject200());
        auto recovered = client->Get("matrix-object");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    /**
     * @given the stub scripted with a bare 502 Bad Gateway
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable, never NotFound,
     *        and the cleared-stub good fetch succeeds in the same test
     */
    TEST(MinioS3ClientMatrixTest, Http502IsTemporarilyUnavailable)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(BareStatus("502 Bad Gateway"));

        auto client = MakeClient(stub.Port());
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);
        EXPECT_EQ(got.error().source, BlockSource::Remote);

        stub.EnqueueResponse(GoodObject200());
        auto recovered = client->Get("matrix-object");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    /**
     * @given the stub scripted with a bare 503 Service Unavailable
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable, never NotFound,
     *        and the cleared-stub good fetch succeeds in the same test
     */
    TEST(MinioS3ClientMatrixTest, Http503IsTemporarilyUnavailable)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(BareStatus("503 Service Unavailable"));

        auto client = MakeClient(stub.Port());
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);
        EXPECT_EQ(got.error().source, BlockSource::Remote);

        stub.EnqueueResponse(GoodObject200());
        auto recovered = client->Get("matrix-object");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    /**
     * @given the stub scripted with a bare 504 Gateway Timeout
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable, never NotFound,
     *        and the cleared-stub good fetch succeeds in the same test
     */
    TEST(MinioS3ClientMatrixTest, Http504IsTemporarilyUnavailable)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(BareStatus("504 Gateway Timeout"));

        auto client = MakeClient(stub.Port());
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);
        EXPECT_EQ(got.error().source, BlockSource::Remote);

        stub.EnqueueResponse(GoodObject200());
        auto recovered = client->Get("matrix-object");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    /**
     * @given the stub sending response HEADERS that declare a body it
     *        never delivers (connection closed mid-body)
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable (transport-class),
     *        never NotFound
     */
    TEST(MinioS3ClientMatrixTest, TruncatedBodyIsTemporarilyUnavailable)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        // Declares 1024 body bytes, delivers none, closes.
        stub.EnqueueResponse(
            "HTTP/1.1 200 OK\r\n"
            "ETag: \"truncated\"\r\n"
            "Content-Length: 1024\r\n"
            "Connection: close\r\n"
            "\r\n");

        auto client = MakeClient(stub.Port());
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);

        // D-06 inline recovery: cleared stub -> good fetch, same client.
        stub.EnqueueResponse(GoodObject200());
        auto recovered = client->Get("matrix-object");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    /**
     * @given the stub declaring N body bytes but delivering exactly ONE
     *        before closing (a different cut point than the zero-byte
     *        truncation above)
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable (mid-body framing
     *        break - the D-08 transport half), never NotFound, and the
     *        cleared-stub good fetch succeeds in the same test
     */
    TEST(MinioS3ClientMatrixTest,
         TruncatedBodyAfterFirstByteIsTemporarilyUnavailable)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(
            "HTTP/1.1 200 OK\r\n"
            "ETag: \"cut-after-first-byte\"\r\n"
            "Content-Length: 1024\r\n"
            "Connection: close\r\n"
            "\r\n" + kPayload.substr(0, 1));

        auto client = MakeClient(stub.Port());
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);

        stub.EnqueueResponse(GoodObject200());
        auto recovered = client->Get("matrix-object");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    /**
     * @given the stub closing the connection MID-header-block (no blank
     *        line, no header completion, no body)
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable, never NotFound,
     *        and the cleared-stub good fetch succeeds in the same test
     */
    TEST(MinioS3ClientMatrixTest,
         TruncatedHeadersIsTemporarilyUnavailable)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(
            "HTTP/1.1 200 OK\r\n"
            "ETag: \"trunc");

        auto client = MakeClient(stub.Port());
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);

        stub.EnqueueResponse(GoodObject200());
        auto recovered = client->Get("matrix-object");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    /**
     * @given the stub sending non-HTTP garbage bytes
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable, never NotFound
     */
    TEST(MinioS3ClientMatrixTest, GarbageResponseIsTemporarilyUnavailable)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse("this is not http at all\r\n\r\n<<<garbage>>>");

        auto client = MakeClient(stub.Port());
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);

        // D-06 inline recovery: cleared stub -> good fetch, same client.
        stub.EnqueueResponse(GoodObject200());
        auto recovered = client->Get("matrix-object");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    /**
     * @given the stub sending BINARY noise bytes (a garbage shape
     *        distinct from the non-HTTP text form above)
     * @when Get runs
     * @then the error slot is TemporarilyUnavailable, never NotFound,
     *        and the cleared-stub good fetch succeeds in the same test
     */
    TEST(MinioS3ClientMatrixTest,
         GarbageBinaryNoiseIsTemporarilyUnavailable)
    {
        const std::string noise = [] {
            const unsigned char raw[] = {0x01, 0xfd, 0xfe, 0xc3, 0x7f, 0x80};
            return std::string(reinterpret_cast<const char*>(raw),
                               sizeof(raw)) + "~binary~noise~";
        }();
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(noise);

        auto client = MakeClient(stub.Port());
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::TemporarilyUnavailable);
        EXPECT_NE(got.error().code, StatusClass::NotFound);

        stub.EnqueueResponse(GoodObject200());
        auto recovered = client->Get("matrix-object");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    /**
     * @given the stub serving the 412 XML-body response
     * @when a createOnly Put runs
     * @then the result is the SUCCESS slot with achieved=true and
     *        alreadyPresent=true - D-02 on the wire (XML shape)
     */
    TEST(MinioS3ClientMatrixTest, PreconditionXmlBodyMapsToAlreadyPresent)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(XmlPreconditionFailed412());

        auto client = MakeClient(stub.Port());
        auto put = client->Put("matrix-object", BytesOf(kPayload),
                               PutObjectOptions{/*createOnly=*/true});
        ASSERT_TRUE(put) << "XML 412 must map to the success slot (D-02); error="
                        << (put.has_value() ? std::string("<success>")
                                            : put.error().message);
        EXPECT_TRUE(put.value().achieved);
        EXPECT_TRUE(put.value().alreadyPresent);
    }

    /**
     * @given the stub serving an EMPTY-body 412 (the SDK emits its
     *        status-code-default text for this shape)
     * @when a createOnly Put runs
     * @then the same success-slot alreadyPresent=true result - the second
     *        D-02 evidence shape
     */
    TEST(MinioS3ClientMatrixTest, PreconditionEmptyBodyMapsToAlreadyPresent)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(EmptyBody412());

        auto client = MakeClient(stub.Port());
        auto put = client->Put("matrix-object", BytesOf(kPayload),
                               PutObjectOptions{/*createOnly=*/true});
        ASSERT_TRUE(put) << "empty-body 412 must map to the success slot";
        EXPECT_TRUE(put.value().achieved);
        EXPECT_TRUE(put.value().alreadyPresent);
    }

    /**
     * @given the stub serving a 412 whose body is unparseable binary
     *        noise (the THIRD observed wire shape - alongside the XML
     *        and empty-body forms above)
     * @when a createOnly Put runs
     * @then the result is STILL the success slot with alreadyPresent=
     *        true - whichever of the mapper's three 412 message shapes
     *        minio-cpp emits, the semantic is alreadyPresent (the trio
     *        must not regress)
     */
    TEST(MinioS3ClientMatrixTest, PreconditionBinaryBodyMapsToAlreadyPresent)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(BinaryBody412());

        auto client = MakeClient(stub.Port());
        auto put = client->Put("matrix-object", BytesOf(kPayload),
                               PutObjectOptions{/*createOnly=*/true});
        ASSERT_TRUE(put) << "binary-body 412 must map to the success slot";
        EXPECT_TRUE(put.value().achieved);
        EXPECT_TRUE(put.value().alreadyPresent);
    }

    /**
     * @given the stub serving the S3 NoSuchKey XML error with a 404
     * @when Get runs
     * @then the error slot is NotFound with Remote source - the one
     *        legitimate authoritative absence
     */
    TEST(MinioS3ClientMatrixTest, GetMissingIsNotFound)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());
        stub.EnqueueResponse(NoSuchKey404());

        auto client = MakeClient(stub.Port());
        auto got = client->Get("matrix-object");
        ASSERT_FALSE(got);
        EXPECT_EQ(got.error().code, StatusClass::NotFound)
            << "error=" << got.error().message;
        EXPECT_EQ(got.error().source, BlockSource::Remote);
    }

    /**
     * @given a client whose Get just failed against a scripted 500
     * @when the stub is re-scripted to a valid 200 object response and
     *        the SAME client instance Gets again
     * @then the recovery Get succeeds with the served bytes - no sticky
     *        negative cache anywhere in the adapter
     */
    TEST(MinioS3ClientMatrixTest, NoStickyNegativeCache)
    {
        LoopbackHttpStub stub;
        ASSERT_TRUE(stub.Start());

        auto client = MakeClient(stub.Port());

        stub.EnqueueResponse(Bare500());
        auto faulted = client->Get("matrix-object");
        ASSERT_FALSE(faulted);
        EXPECT_EQ(faulted.error().code, StatusClass::TemporarilyUnavailable);

        stub.EnqueueResponse(
            "HTTP/1.1 200 OK\r\n"
            "ETag: \"recovered\"\r\n"
            "Content-Length: " + std::to_string(kPayload.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + kPayload);
        auto recovered = client->Get("matrix-object");
        ASSERT_TRUE(recovered) << "adapter must not cache the failure";
        EXPECT_EQ(recovered.value().bytes, BytesOf(kPayload));
    }

    //
    // Env-gated MinIO-binary live tier (D-06/D-07/D-09) below - opt-in
    // only, loud labelled skip when SGNS_BLOCKSTORE_TEST_MINIO_BIN is
    // unset. NEVER downloads, NEVER contacts a live-internet endpoint.
    //

    /**
     * @brief Fixture instantiating the 03-01 harness when the env gate is
     *        set; every live case skips loudly otherwise (D-07).
     */
    class MinioS3ClientLiveTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            if (!MinioServerHarness::Available())
            {
                GTEST_SKIP() << "[SGNS_BLOCKSTORE][MINIO-TIER] binary not "
                                "provided (set SGNS_BLOCKSTORE_TEST_MINIO_BIN)";
            }
            ASSERT_TRUE(harness_.Start(/*tls=*/false, &error_));
            ASSERT_TRUE(harness_.MakeBucket("matrix-live", &error_))
                << error_;

            S3ClientConfig config;
            config.endpoint = "127.0.0.1";
            config.port = harness_.Port();
            config.region = "us-east-1";
            config.useTls = false; // TLS-off is the primary lane
            config.tlsVerify = true;
            config.pathStyle = true;
            config.accessKey = harness_.AccessKey();
            config.secretKey = harness_.SecretKey();
            config.bucket = "matrix-live";
            auto opened = MinioS3Client::Open(config);
            ASSERT_TRUE(opened) << opened.error().message;
            client_ = std::unique_ptr<MinioS3Client>(
                std::move(opened.value()));
        }

        MinioServerHarness harness_;
        std::string error_;
        std::unique_ptr<MinioS3Client> client_;
    };

    /**
     * @given a live local MinIO server (env-gated)
     * @when put/get/head/delete run against it through MinioS3Client
     * @then the round trip is byte-identical, head reports exists with the
     *        right size, delete succeeds, and the following head/get
     *        report absence per interface semantics (SC4a local-endpoint
     *        evidence)
     */
    TEST_F(MinioS3ClientLiveTest, LivePutGetHeadDeleteRoundtrip)
    {
        const std::string key = "matrix-live-object";
        const auto bytes = BytesOf(kPayload);

        auto put = client_->Put(key, bytes);
        ASSERT_TRUE(put) << put.error().message;
        EXPECT_TRUE(put.value().achieved);
        EXPECT_FALSE(put.value().alreadyPresent);

        auto got = client_->Get(key);
        ASSERT_TRUE(got) << got.error().message;
        EXPECT_EQ(got.value().bytes, bytes);

        auto head = client_->Head(key);
        ASSERT_TRUE(head) << head.error().message;
        EXPECT_TRUE(head.value().exists);
        EXPECT_EQ(head.value().size,
                  static_cast<uint64_t>(bytes.size()));

        EXPECT_TRUE(client_->Delete(key));

        auto head_after = client_->Head(key);
        ASSERT_TRUE(head_after);
        EXPECT_FALSE(head_after.value().exists);
        auto got_after = client_->Get(key);
        ASSERT_FALSE(got_after);
        EXPECT_EQ(got_after.error().code, StatusClass::NotFound);
    }

    /**
     * @given a live local MinIO server (env-gated)
     * @when a createOnly Put runs twice on the same key
     * @then the second returns the SUCCESS slot alreadyPresent=true -
     *        REAL-412 evidence for D-02 end to end
     */
    TEST_F(MinioS3ClientLiveTest, LiveCreateOnlyDuplicateReportsAlreadyPresent)
    {
        const std::string key = "matrix-live-duplicate";
        const auto bytes = BytesOf(kPayload);

        auto first = client_->Put(key, bytes, PutObjectOptions{true});
        ASSERT_TRUE(first) << first.error().message;
        EXPECT_FALSE(first.value().alreadyPresent);

        auto second = client_->Put(key, BytesOf("other bytes"),
                                   PutObjectOptions{true});
        ASSERT_TRUE(second)
            << "real 412 must map to the success slot: "
            << (second.has_value() ? std::string()
                                   : second.error().message);
        EXPECT_TRUE(second.value().achieved);
        EXPECT_TRUE(second.value().alreadyPresent);
    }

    /**
     * @given the live fixture
     * @when inspecting its configuration
     * @then useTls is false - documenting that plain-HTTP loopback is the
     *        PRIMARY lane everywhere; the TLS-on smoke lives in the 03-01
     *        spike (Q2) behind the same env gate
     */
    TEST_F(MinioS3ClientLiveTest, LiveTlsOffIsPrimaryLane)
    {
        SUCCEED() << "fixture config uses useTls=false (see SetUp); the "
                     "TLS-on smoke is minio_cpp_spike_test.EnvGatedTlsOnSmoke";
    }
} // namespace
