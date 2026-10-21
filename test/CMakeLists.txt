addtest(memory_block_store_test memory_block_store_test.cpp)
target_link_libraries(memory_block_store_test
    sgns-blockstore
    p2p::p2p_cid
)

# conformance + spike test targets land with the spike task
addtest(cid_spike_test cid_spike_test.cpp)
target_link_libraries(cid_spike_test
    sgns-blockstore
    p2p::p2p_cid
)
target_include_directories(memory_block_store_test PRIVATE ${CMAKE_SOURCE_DIR}/test/conformance)

addtest(local_block_store_test local_block_store_test.cpp)
target_link_libraries(local_block_store_test
    sgns-blockstore
    p2p::p2p_cid
)
target_include_directories(local_block_store_test PRIVATE ${CMAKE_SOURCE_DIR}/test/conformance)

# Crash suite links RocksDB DIRECTLY for the raw second-handle corruption
# forge - test-surface-only privilege; the installed library's public link
# surface is unchanged.
addtest(local_block_store_crash_test local_block_store_crash_test.cpp)
target_link_libraries(local_block_store_crash_test
    sgns-blockstore
    p2p::p2p_cid
    RocksDB::rocksdb
)

addtest(api_hygiene_test api_hygiene_test.cpp)
target_link_libraries(api_hygiene_test
    sgns-blockstore
    p2p::p2p_cid
)

# In-process self-signed cert generation in MinioServerHarness (G-3-1):
# explicit OpenSSL::Crypto on every target compiling the harness - never
# miniocpp's transitive surface. Resolution is deterministic: the
# superbuild injects OPENSSL_ROOT_DIR / OPENSSL_USE_STATIC_LIBS /
# OPENSSL_MSVC_STATIC_RT into this sub-build via its openssl cache args.
find_package(OpenSSL REQUIRED)

# D-11 day-one spike (Phase 3): hermetic wire-evidence tests against the
# raw-socket loopback stub, plus the env-gated MinIO-binary TLS smoke
# (D-06/D-07/D-09 - runtime-skipped, never a CMake gate).
addtest(minio_cpp_spike_test
    minio_cpp_spike_test.cpp
    LoopbackHttpStub.cpp
    MinioServerHarness.cpp
)
target_link_libraries(minio_cpp_spike_test
    sgns-blockstore
    p2p::p2p_cid
    miniocpp::miniocpp
    OpenSSL::Crypto
)
if(WIN32)
    # Raw-socket stub + harness health probe (WinSock).
    target_link_libraries(minio_cpp_spike_test ws2_32)
endif()

# SC3 hermetic adapter tier (Phase 3, 03-02): the InMemoryS3Client fake.
# SDK-free - links exactly the library + p2p_cid like every backend suite.
# src/ include path: the internal adapter headers are deliberately NOT on
# the library's public include surface (D-04) - the test reaches them
# directly, test-surface-only privilege (crash-suite precedent).
addtest(in_memory_s3_client_test in_memory_s3_client_test.cpp)
target_include_directories(in_memory_s3_client_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(in_memory_s3_client_test
    sgns-blockstore
    p2p::p2p_cid
)

# D-08 stub fault matrix against the REAL MinioS3Client + the env-gated
# MinIO-binary live tier (03-03). Hermetic matrix runs in the default CI
# lane; live cases runtime-skip loudly when the env gate is unset.
addtest(minio_s3_client_test
    minio_s3_client_test.cpp
    LoopbackHttpStub.cpp
    MinioServerHarness.cpp
)
target_include_directories(minio_s3_client_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(minio_s3_client_test
    sgns-blockstore
    p2p::p2p_cid
    miniocpp::miniocpp
    OpenSSL::Crypto
)
if(WIN32)
    # Stub + harness sockets (WinSock).
    target_link_libraries(minio_s3_client_test ws2_32)
endif()

add_test(NAME header_hygiene
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/header_hygiene_check.cmake)
