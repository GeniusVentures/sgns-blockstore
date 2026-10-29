cmake_minimum_required(VERSION 3.20)

project(sgns-blockstore LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/functions.cmake)

find_package(libp2p CONFIG REQUIRED)

# NO version argument (BACK-03 dual-compat): the installed config version
# file is COMPATIBILITY SameMajorVersion, so a versioned request would
# reject a consumer-side 11.1.2 install against this 9.4 build.
find_package(RocksDB CONFIG REQUIRED)

# minio-cpp SDK lives behind the INTERNAL S3Client adapter seam (Phase 3,
# BACK-06/D-04): linked PRIVATE in src/ — no installed header references it,
# so it adds nothing to the public surface (SC2 containment).
#
# cpp-httplib is found EXPLICITLY first: miniocpp's consumer config
# (miniocpp-config.cmake -> miniocpp-deps.cmake) resolves httplib via a
# QUIET config find whose target does not register for find_package's
# imported-target-reference verification, leaving miniocpp_FOUND FALSE
# with "imported targets are referenced, but are missing: httplib::httplib"
# (Phase-3 spike finding, 2026-10-21; httplib_DIR comes from the superbuild
# entry - standalone configures pass it or CMAKE_PREFIX_PATH).
find_package(httplib CONFIG REQUIRED)
find_package(miniocpp CONFIG REQUIRED)

# nlohmann_json backs the s3d admin facade's STRICT /stats/uploads
# parsing (S3AdminClient.cpp, v1.1 Phase 10) - linked PRIVATE in src/,
# never referenced by any installed header.
find_package(nlohmann_json CONFIG REQUIRED)

# ipfs-lite-cpp provides the blake2b-256 implementation (target
# ipfs-lite-cpp::blake2) - the same object code sgns::common::getCidOf
# uses to hash every CRDT DAG block. Linking the ecosystem copy (never a
# vendored second one) guarantees CID byte-compatibility by construction
# and avoids the duplicate-symbol collision that two copies of the
# unrenamed blake2b_* globals would cause once SuperGenius links both
# archives (Phase-6 widening, v1.1 RESEARCH Q1).
find_package(ipfs-lite-cpp CONFIG REQUIRED)

if(GSL_INCLUDE_DIR)
    include_directories(${GSL_INCLUDE_DIR})
endif()

# Test gate. Deliberately named TESTING (never BUILD_TESTING): the thirdparty
# superbuild's _CMAKE_COMMON_CACHE_ARGS force -DBUILD_TESTING:BOOL=OFF into
# every ExternalProject sub-build, and later cache args win - a
# BUILD_TESTING-gated repo could never enable tests through the superbuild.
option(TESTING "Build tests" ON)
if(TESTING)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(test)
endif()

add_subdirectory(src)

# Install Headers
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.hpp")

# Install the test-fake headers (TEST-02 SC4, v1.1 Phase 12 plan 12-03,
# user ruling 2026-10-28): InMemoryS3Client / InMemoryS3AdminClient are
# compiled INTO the library (src/CMakeLists.txt add_library list) and
# their ONLY non-std includes are installed public headers
# (S3Client.hpp / S3AdminClient.hpp) - no PRIVATE dependency leaks, so
# they are installable as-is. SuperGenius's cold-storage suites consume
# them from the install tree like every other library header; a fresh
# clone no longer needs the source tree (the release tarball carried
# include/ and lib/ only, which made every cold suite un-compilable in
# CI - the 12-03 research finding 8 landmine).
install(FILES src/InMemoryS3Client.hpp src/InMemoryS3AdminClient.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sgns-blockstore")

# Install LIB
install(TARGETS sgns-blockstore
    EXPORT sgns-blockstoreTargets
    DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# Install Targets (namespaced export - deliberate improvement over the
# AsyncIOManager precedent, which omits the NAMESPACE)
set(SGNS_BLOCKSTORE_CONFIG_DESTINATION_DIR "lib/cmake/sgns-blockstore")
install(EXPORT sgns-blockstoreTargets
    FILE sgns-blockstoreTargets.cmake
    NAMESPACE sgns-blockstore::
    DESTINATION ${SGNS_BLOCKSTORE_CONFIG_DESTINATION_DIR})

include(CMakePackageConfigHelpers)

# generate the config file that includes the exports
configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/sgns-blockstoreConfig.cmake"
    INSTALL_DESTINATION ${SGNS_BLOCKSTORE_CONFIG_DESTINATION_DIR}
)

# generate the version file for the config file
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/sgns-blockstoreConfigVersion.cmake"
    VERSION 1.0.0
    COMPATIBILITY AnyNewerVersion
)

# install the configuration files
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/sgns-blockstoreConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/sgns-blockstoreConfigVersion.cmake
    DESTINATION ${SGNS_BLOCKSTORE_CONFIG_DESTINATION_DIR}
)
