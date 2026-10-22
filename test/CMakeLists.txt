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

# In-process self-signed cert generation (OpenSSL::Crypto) and the
# TLS-wrapped health probe (OpenSSL::SSL) in MinioServerHarness (G-3-1):
# explicit links on every target compiling the harness - never
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
    OpenSSL::SSL
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

# BACK-07 hermetic store tier (Phase 4, 04-01): S3BlockStore - the full
# conformance kit + key-layout/idempotency probes over the fake. SDK-free
# like every default-lane suite; src/ privilege reaches the fake.
addtest(s3_block_store_test s3_block_store_test.cpp)
target_include_directories(s3_block_store_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(s3_block_store_test PRIVATE ${CMAKE_SOURCE_DIR}/test/conformance)
target_link_libraries(s3_block_store_test
    sgns-blockstore
    p2p::p2p_cid
)

# D-01..D-04 drainer tier (Phase 4, 04-01): DrainPendingUploads against
# a REAL LocalBlockStore draining into the fake-backed S3BlockStore.
# Conformance-free link line (local_block_store_test shape; RocksDB
# arrives transitively through the library); src/ privilege reaches the
# fake.
addtest(upload_drainer_test upload_drainer_test.cpp)
target_include_directories(upload_drainer_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(upload_drainer_test
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
    OpenSSL::SSL
)
if(WIN32)
    # Stub + harness sockets (WinSock).
    target_link_libraries(minio_s3_client_test ws2_32)
endif()

# S3D-TIER dev-gated live spike (Phase 4, 04-02, D-05/D-06): connects
# to a dev-managed s3d v0.2.1 endpoint ONLY - never spawns or manages
# the server (D-08), never a CMake gate: with the SGNS_BLOCKSTORE_TEST_S3D_*
# env vars unset both cases loud-skip [SGNS_BLOCKSTORE][S3D-TIER]
# (runtime skip, TEST-01). Links the SDK for the raw wire-path probe.
addtest(s3d_spike_test s3d_spike_test.cpp)
target_include_directories(s3d_spike_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(s3d_spike_test
    sgns-blockstore
    p2p::p2p_cid
    miniocpp::miniocpp
    OpenSSL::Crypto
    OpenSSL::SSL
)
if(WIN32)
    target_link_libraries(s3d_spike_test ws2_32)
endif()

# S3D-01/SC3 opt-in MinIO-binary store tier (Phase 4, 04-03): the real
# MinioS3Client -> S3BlockStore composition against the env-gated local
# MinIO community binary (SGNS_BLOCKSTORE_TEST_MINIO_BIN). Runtime skip
# never a CMake gate, no Docker, no auto-download; harness instantiated
# UNCHANGED from Phase 3.
addtest(s3_block_store_minio_test
    s3_block_store_minio_test.cpp
    MinioServerHarness.cpp
)
target_include_directories(s3_block_store_minio_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(s3_block_store_minio_test
    sgns-blockstore
    p2p::p2p_cid
    miniocpp::miniocpp
    OpenSSL::Crypto
    OpenSSL::SSL
)
if(WIN32)
    target_link_libraries(s3_block_store_minio_test ws2_32)
endif()

add_test(NAME header_hygiene
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/header_hygiene_check.cmake)
