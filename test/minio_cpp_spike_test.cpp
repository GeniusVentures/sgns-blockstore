/*
 * D-11 entrance-criterion spike (minio-cpp v1.0.0) - the Phase-3 day-one
 * gate. Assertion bar, stated verbatim so the bar is not discovered
 * mid-spike:
 *   (a) a conditional If-None-Match request header with value * is
 *       observable in the raw request bytes captured by the loopback stub
 *       when sent through PutObjectArgs.headers - the extra_headers field
 *       is never used for it (03-RESEARCH.md pitfall 2: the single-part
 *       PUT path drops extra_headers);
 *   (b) BaseUrl trio: path-style addressing (virtual_style=false) is
 *       observable in the captured request line; a pinned region
 *       (us-east-1) yields exactly ONE captured request per operation (no
 *       GetBucketLocation ?location roundtrip - 03-RESEARCH.md
 *       anti-pattern "Leaving region empty"); the HTTPS field is exercised
 *       by the env-gated TLS-on smoke below;
 *   (c) TLS: plain-HTTP loopback is the PRIMARY lane everywhere (asserted
 *       by every hermetic test above); the TLS-on smoke is env-gated
 *       behind SGNS_BLOCKSTORE_TEST_MINIO_BIN with a loud labelled skip
 *       when unset (D-07/D-09, Q2).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

#include "LoopbackHttpStub.hpp"
#include "MinioServerHarness.hpp"

#include <miniocpp/client.h>
#include <miniocpp/providers.h>

namespace
{
    using sgns::blockstore::testsupport::LoopbackHttpStub;
    using sgns::blockstore::testsupport::MinioServerHarness;

    std::string ToLower(const std::string& value)
    {
        std::string out = value;
        for (char& c : out)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    }

    /**
     * @brief TU-local precondition-evidence classifier. The full failure
     *        taxonomy mapping lands in 03-03 (one place); this spike proves
     *        the 412 wire shapes classify as precondition evidence. Three
     *        string shapes observed at v1.0.0 (spike finding, 2026-10-21):
     *        - XML body WITH Content-Type: application/xml parses to the
     *          "PreconditionFailed: ..." prefix;
     *        - an unparseable/non-XML body yields
     *          "invalid response received; status code: 412; ...";
     *        - an empty body yields the status-default
     *          "server failed with HTTP status code 412".
     */
    struct PreconditionEvidence
    {
        bool operator()(const std::string& error_string) const
        {
            return error_string.find("PreconditionFailed") != std::string::npos
                || error_string.find("HTTP status code 412")
                    != std::string::npos
                || error_string.find("status code: 412") != std::string::npos;
        }
    };

    const std::string kPayload = "minio-cpp spike payload";

    minio::s3::PutObjectArgs MakePutArgs(std::istringstream& stream,
                                         bool conditional)
    {
        minio::s3::PutObjectArgs args(
            stream, std::optional<uint64_t>(kPayload.size()), /*part_size=*/0);
        args.bucket = "spike-bucket";
        args.region = "us-east-1"; // pinned: no GetBucketLocation roundtrip
        args.object = "spike-object";
        args.content_type = "application/octet-stream";
        if (conditional)
        {
            // THE field: ObjectWriteArgs::headers (flows into the wire).
            // Never extra_headers - the single-part PUT path drops it.
            args.headers.Add("If-None-Match", "*");
        }
        return args;
    }
} // namespace

/**
 * @given a loopback stub on 127.0.0.1 and a minio client with a pinned
 *        region, https=false, virtual_style=false (the default)
 * @when a conditional PutObject carries If-None-Match:* via
 *        PutObjectArgs.headers
 * @then the captured raw request bytes show the conditional header with
 *        value *, the request line uses path-style /bucket/object
 *        addressing with no ?location roundtrip, and EXACTLY one request
 *        was captured for the operation
 */
TEST(MinioCppSpikeTest, HermeticConditionalPutOnTheWire)
{
    LoopbackHttpStub stub;
    ASSERT_TRUE(stub.Start());
    stub.EnqueueOk200();

    minio::s3::BaseUrl base_url("127.0.0.1", /*https=*/false, "us-east-1");
    base_url.port = stub.Port(); // virtual_style stays false: path-style
    minio::creds::StaticProvider provider("sgnsspike", "sgnsspike-secret");
    minio::s3::Client client(base_url, &provider);

    std::istringstream stream(kPayload);
    auto result = client.PutObject(MakePutArgs(stream, /*conditional=*/true));
    // The op outcome against a stub is irrelevant to the wire evidence;
    // the assertions below are the D-11 (a)+(b) bar.

    ASSERT_EQ(stub.RequestCount(), 1u)
        << "region pin must yield exactly one request per operation";

    const std::string captured = stub.CapturedRequest(0);

    // Header names arrive LOWERCASE on the wire (cpp-httplib normalizes).
    // NOTE: search the full lowered capture, never a located line - the
    // string "if-none-match" ALSO appears inside the authorization
    // header's SignedHeaders list, which would anchor the wrong line.
    EXPECT_NE(ToLower(captured).find("if-none-match: *"), std::string::npos)
        << "conditional header not on the wire: " << captured;

    EXPECT_NE(captured.find("PUT /spike-bucket/spike-object "),
              std::string::npos)
        << "request line is not path-style: " << captured;
    EXPECT_EQ(captured.find("?location"), std::string::npos)
        << "GetBucketLocation roundtrip observed";

    (void)result;
}

/**
 * @given the stub scripting first an XML-body 412 response, then a bare
 *        empty-body 412 response
 * @when PutObject fails against each scripted shape
 * @then the resulting SDK error string classifies as precondition evidence
 *        for BOTH shapes (XML "PreconditionFailed" prefix and the
 *        no-body "HTTP status code 412" status-default)
 */
TEST(MinioCppSpikeTest, PreconditionEvidenceShapes)
{
    const std::string xml_body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Error><Code>PreconditionFailed</Code>"
        "<Message>At least one of the pre-conditions you specified did not "
        "hold</Message><Resource>/spike-bucket/spike-object</Resource>"
        "<RequestId>spike</RequestId></Error>";
    const std::string xml_412 =
        "HTTP/1.1 412 Precondition Failed\r\n"
        // Real MinIO labels the XML error body; without this the SDK never
        // parses the body and emits the "invalid response received" shape
        // (also a valid precondition signal - see the classifier above).
        "Content-Type: application/xml\r\n"
        "Content-Length: " + std::to_string(xml_body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + xml_body;
    const std::string bare_412 =
        "HTTP/1.1 412 Precondition Failed\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";

    LoopbackHttpStub stub;
    ASSERT_TRUE(stub.Start());
    stub.EnqueueResponse(xml_412);
    stub.EnqueueResponse(bare_412);

    minio::s3::BaseUrl base_url("127.0.0.1", /*https=*/false, "us-east-1");
    base_url.port = stub.Port();
    minio::creds::StaticProvider provider("sgnsspike", "sgnsspike-secret");
    minio::s3::Client client(base_url, &provider);

    PreconditionEvidence is_precondition;

    std::istringstream stream_one(kPayload);
    auto xml_result =
        client.PutObject(MakePutArgs(stream_one, /*conditional=*/true));
    ASSERT_FALSE(xml_result);
    EXPECT_TRUE(is_precondition(xml_result.error().String()))
        << "XML-body 412 not classified: " << xml_result.error().String();

    std::istringstream stream_two(kPayload);
    auto bare_result =
        client.PutObject(MakePutArgs(stream_two, /*conditional=*/true));
    ASSERT_FALSE(bare_result);
    EXPECT_TRUE(is_precondition(bare_result.error().String()))
        << "bare no-body 412 not classified: " << bare_result.error().String();
}

/**
 * @given an env-gated MinIO server binary (SGNS_BLOCKSTORE_TEST_MINIO_BIN)
 *        and a resolvable openssl for the self-signed fixture
 * @when the client connects with BaseUrl https=true plus
 *        IgnoreCertCheck(true) against the TLS-enabled local server
 * @then the TLS handshake completes and one S3 operation (MakeBucket +
 *        PutObject) succeeds over TLS
 * @note plain-HTTP loopback (TLS-off) is the primary lane, asserted by
 *       every hermetic test above; this smoke is opt-in only (D-07/D-09).
 */
TEST(MinioCppSpikeTest, EnvGatedTlsOnSmoke)
{
    if (!MinioServerHarness::Available())
    {
        GTEST_SKIP() << "[SGNS_BLOCKSTORE][MINIO-TIER] binary not provided "
                        "(set SGNS_BLOCKSTORE_TEST_MINIO_BIN)";
    }

    MinioServerHarness harness;
    std::string error;
    if (!harness.Start(/*tls=*/true, &error))
    {
        GTEST_SKIP() << "[SGNS_BLOCKSTORE][MINIO-TIER] TLS fixture "
                        "unavailable: " << error;
    }

    ASSERT_TRUE(harness.MakeBucket("spike-tls", &error)) << error;

    minio::s3::BaseUrl base_url("127.0.0.1", /*https=*/true, "us-east-1");
    base_url.port = harness.Port();
    minio::creds::StaticProvider provider(harness.AccessKey(),
                                          harness.SecretKey());
    minio::s3::Client client(base_url, &provider);
    client.IgnoreCertCheck(true); // self-signed fixture (Q2)

    std::istringstream stream(kPayload);
    auto result = client.PutObject(MakePutArgs(stream, /*conditional=*/false));
    ASSERT_TRUE(result) << result.error().String();
}
