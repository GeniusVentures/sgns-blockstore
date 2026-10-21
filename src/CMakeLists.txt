add_library(sgns-blockstore STATIC
    Cid.cpp
    ContractHeaders.cpp
    LocalBlockStore.cpp
    MemoryBlockStore.cpp
)

target_link_libraries(sgns-blockstore
    PUBLIC p2p::p2p_cid
    PUBLIC RocksDB::rocksdb
)

target_include_directories(sgns-blockstore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:include>
)
