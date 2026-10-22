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
)

target_include_directories(sgns-blockstore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:include>
)
