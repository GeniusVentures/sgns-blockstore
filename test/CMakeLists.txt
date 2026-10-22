addtest(memory_block_store_test memory_block_store_test.cpp)
target_link_libraries(memory_block_store_test
    sgns-blockstore
    p2p::p2p_cid
)
# D-13 (Phase 5): every registration carries its tier LABEL - the tier
# assignment of each test is readable from this file without running
# anything; CI selects the hermetic lane with a fail-closed -L Tier1.
set_tests_properties(memory_block_store_test PROPERTIES LABELS "Tier1")

# conformance + spike test targets land with the spike task
addtest(cid_spike_test cid_spike_test.cpp)
target_link_libraries(cid_spike_test
    sgns-blockstore
    p2p::p2p_cid
)
set_tests_properties(cid_spike_test PROPERTIES LABELS "Tier1")
target_include_directories(memory_block_store_test PRIVATE ${CMAKE_SOURCE_DIR}/test/conformance)

addtest(local_block_store_test local_block_store_test.cpp)
target_link_libraries(local_block_store_test
    sgns-blockstore
    p2p::p2p_cid
)
set_tests_properties(local_block_store_test PROPERTIES LABELS "Tier1")
target_include_directories(local_block_store_test PRIVATE ${CMAKE_SOURCE_DIR}/test/conformance)

# Crash suite (states 3/4 of the Phase-5 four-state matrix reach the
# InMemoryS3Client fake - test-surface-only src/ privilege,
# in_memory_s3_client_test precedent; the raw second-handle RocksDB
# link was retired with the D-09 corrupt-after-commit hook).
addtest(local_block_store_crash_test local_block_store_crash_test.cpp)
target_include_directories(local_block_store_crash_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(local_block_store_crash_test
    sgns-blockstore
    p2p::p2p_cid
)
set_tests_properties(local_block_store_crash_test PROPERTIES LABELS "Tier1")

addtest(api_hygiene_test api_hygiene_test.cpp)
target_link_libraries(api_hygiene_test
    sgns-blockstore
    p2p::p2p_cid
)
set_tests_properties(api_hygiene_test PROPERTIES LABELS "Tier1")

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
# D-13: MIXED binary - hermetic stub cases run in the Tier1 CI lane while
# the env-gated live TLS smoke loud-skips without SGNS_BLOCKSTORE_TEST_MINIO_BIN;
# a dev -L Tier2 run with the gate set is the complete opt-in lane.
set_tests_properties(minio_cpp_spike_test PROPERTIES LABELS "Tier1;Tier2")

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
set_tests_properties(in_memory_s3_client_test PROPERTIES LABELS "Tier1")

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
set_tests_properties(s3_block_store_test PROPERTIES LABELS "Tier1")

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
set_tests_properties(upload_drainer_test PROPERTIES LABELS "Tier1")

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
# D-13: MIXED binary - hermetic stub matrix runs in the Tier1 CI lane while
# the env-gated live roundtrips loud-skip without SGNS_BLOCKSTORE_TEST_MINIO_BIN;
# a dev -L Tier2 run with the gate set is the complete opt-in lane.
set_tests_properties(minio_s3_client_test PROPERTIES LABELS "Tier1;Tier2")

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
# D-13: dev-gated live spike against a dev-managed s3d endpoint - Tier3.
set_tests_properties(s3d_spike_test PROPERTIES LABELS "Tier3")

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
# D-13: every case env-gated on SGNS_BLOCKSTORE_TEST_MINIO_BIN - pure Tier2.
set_tests_properties(s3_block_store_minio_test PROPERTIES LABELS "Tier2")

# S3D-02/SC4 dev-gated two-run E2E (Phase 4, 04-03): ONE binary, TWO
# GTest cases, TWO SEPARATE INVOCATIONS via --gtest_filter - two
# invocations ARE the two fresh processes; running it once as one
# ctest pass is NOT the proof. Run A (S3dE2E.UploadRun): local Put ->
# DrainPendingUploads -> s3d, journal asserted Accepted-level. Run B
# (S3dE2E.VerifyRun): honest cold retrieval - no local store anywhere.
# Dev-gated S3D-TIER (runtime skip, never CI); the binary only
# connects - it never spawns or manages s3d (D-08).
addtest(s3d_roundtrip_e2e s3d_roundtrip_e2e.cpp)
target_include_directories(s3d_roundtrip_e2e PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(s3d_roundtrip_e2e
    sgns-blockstore
    p2p::p2p_cid
    miniocpp::miniocpp
    OpenSSL::Crypto
    OpenSSL::SSL
)
if(WIN32)
    target_link_libraries(s3d_roundtrip_e2e ws2_32)
endif()
# D-13: dev-gated two-run fresh-process E2E - Tier3 (never CI).
set_tests_properties(s3d_roundtrip_e2e PROPERTIES LABELS "Tier3")

add_test(NAME header_hygiene
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/header_hygiene_check.cmake)
set_tests_properties(header_hygiene PROPERTIES LABELS "Tier1")

# D-14 (Phase 5): self-inspecting Tier-1 audit - fixed-sleep + non-loopback
# host scan over the test sources, plus the registration/label count guard
# (a new unlabeled test trips the audit before CI can run it). Runs in
# every ctest context, CI lane included - header_hygiene precedent.
add_test(NAME tier1_static_audit
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tier1_audit_check.cmake)
set_tests_properties(tier1_static_audit PROPERTIES LABELS "Tier1")
