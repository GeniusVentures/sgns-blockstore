add_library(sgns-blockstore STATIC
    Cid.cpp
    ContractHeaders.cpp
    InMemoryS3Client.cpp
    LocalBlockStore.cpp
    MemoryBlockStore.cpp
    MinioS3Client.cpp
    S3BlockStore.cpp
    UploadDrainer.cpp
)

target_link_libraries(sgns-blockstore
    PUBLIC p2p::p2p_cid
    PUBLIC RocksDB::rocksdb
    # minio-cpp stays PRIVATE: no installed header references it (the
    # 03 D-04 promotion installed the SDK-free S3Client INTERFACE header;
    # the minio-cpp-backed MinioS3Client impl and its header stay in src/).
    PRIVATE miniocpp::miniocpp
    # blake2b-256 (CRDT DAG CID composition, Phase 6) is PRIVATE exactly
    # like miniocpp: no installed header references blake2 (the hygiene
    # guard bans the include), so it reaches the export set only as a
    # LINK_ONLY entry.
    PRIVATE ipfs-lite-cpp::blake2
)

target_include_directories(sgns-blockstore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:include>
)
