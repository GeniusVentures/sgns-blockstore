add_library(sgns-blockstore STATIC
    Cid.cpp
    ContractHeaders.cpp
    InMemoryS3AdminClient.cpp
    InMemoryS3Client.cpp
    LocalBlockStore.cpp
    MemoryBlockStore.cpp
    MinioS3Client.cpp
    S3AdminClient.cpp
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
    # cpp-httplib + nlohmann_json back the s3d admin facade
    # (S3AdminClient.cpp, v1.1 Phase 10) and stay PRIVATE exactly like
    # miniocpp/blake2: no installed header references either (the
    # hygiene guard bans the includes), so they reach the export set
    # only as LINK_ONLY entries.
    PRIVATE httplib::httplib
    PRIVATE nlohmann_json::nlohmann_json
    # Static libcrypto (via miniocpp) references dlopen/dlsym for provider
    # loading; on glibc < 2.34 (AlmaLinux 8 CI) libdl is a separate library
    # that no artifact config pulls onto the link line. CMAKE_DL_LIBS is
    # CMake's portable idiom: "dl" exactly where needed, empty on
    # Windows/macOS, harmless on glibc >= 2.34 (stub). Without it every
    # test executable linking this library fails at link on EL8.
    PRIVATE ${CMAKE_DL_LIBS}
)

target_include_directories(sgns-blockstore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:include>
)
