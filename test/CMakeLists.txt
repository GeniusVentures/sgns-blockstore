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
)
if(WIN32)
    # Raw-socket stub + harness health probe (WinSock).
    target_link_libraries(minio_cpp_spike_test ws2_32)
endif()

add_test(NAME header_hygiene
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/header_hygiene_check.cmake)
