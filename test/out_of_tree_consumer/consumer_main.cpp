// Out-of-tree install-tree consumer proof (Phase 6, LIB-02/SC4).
//
// The ONLY library include is the installed S3Client.hpp - installed-
// header self-sufficiency is the point. Constructs the production
// MinIO-backed client via OpenMinioS3Client against a deliberately
// NON-LISTENING endpoint (127.0.0.1:1): construction performs zero
// network I/O (D-06), so success here simultaneously proves the no-I/O
// contract at the consumer boundary. Zero network operations run;
// credential values are never printed (T-06-06).
#include <sgns-blockstore/S3Client.hpp>

#include <cstdio>

int main()
{
    sgns::blockstore::S3ClientConfig config;
    config.endpoint = "127.0.0.1";
    config.port = 1; // deliberately non-listening by design
    config.bucket = "consumer-proof";
    config.accessKey = "dummy-access";
    config.secretKey = "dummy-secret";

    auto opened = sgns::blockstore::OpenMinioS3Client(config);
    if (!opened || opened.value() == nullptr)
    {
        std::puts("consumer-proof: FAILED");
        return 1;
    }
    std::puts("consumer-proof: OK");
    return 0;
}
