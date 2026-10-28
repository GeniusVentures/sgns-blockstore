# sgns-blockstore - project-specific build parameters for the cmaketemplate
# platform entries (build/<Platform>/CMakeLists.txt).
#
# Mirrors the GeniusSDK / SuperGenius consumption pattern: every dependency
# resolves against a prebuilt thirdparty artifact tree (THIRDPARTY_BUILD_DIR),
# exactly how CI consumes thirdparty release tarballs. The repo's root
# CMakeLists.txt remains the entry for the thirdparty superbuild path
# (ExternalProject drives it with its own cache args) and for in-tree dev
# builds; this file is the template-path counterpart.

set(THIRDPARTY_BUILD_DIR "" CACHE PATH "Prebuilt thirdparty artifact tree (thirdparty build/<Platform>/<Type>[/abi])")
if(NOT THIRDPARTY_BUILD_DIR)
    message(FATAL_ERROR
        "THIRDPARTY_BUILD_DIR must point at a prebuilt thirdparty tree "
        "(e.g. the extracted release tarball's build/<Platform>/<Type>). "
        "Configure via build/<Platform>/CMakeLists.txt with -DTHIRDPARTY_BUILD_DIR=...")
endif()

# ---------------------------------------------------------------------------
# GSL (header-only; the installed-header hygiene guard bans it from public
# headers, but internal src/ + test/ translation units include it).
# ---------------------------------------------------------------------------
set(GSL_INCLUDE_DIR "${THIRDPARTY_BUILD_DIR}/Microsoft.GSL/include" CACHE PATH "GSL include dir")
include_directories(SYSTEM ${GSL_INCLUDE_DIR})

# ---------------------------------------------------------------------------
# Boost - the artifact installs under boost/build with a VERSIONED cmake
# tree (Boost-<ver>, boost_<comp>-<ver>); glob the version instead of
# pinning it so the file tracks whatever thirdparty built.
# libp2p's installed config does find_dependency(Boost CONFIG REQUIRED
# random filesystem program_options), so BoostConfig + the component
# configs must resolve. The single boost/build prefix covers every
# component config via cmake/<name>* search.
# ---------------------------------------------------------------------------
file(GLOB _sgnsb_boost_cfg "${THIRDPARTY_BUILD_DIR}/boost/build/lib/cmake/Boost-[0-9]*")
if(NOT _sgnsb_boost_cfg)
    message(FATAL_ERROR "Boost config not found under ${THIRDPARTY_BUILD_DIR}/boost/build/lib/cmake")
endif()
list(SORT _sgnsb_boost_cfg)
list(GET _sgnsb_boost_cfg 0 Boost_DIR)
list(APPEND CMAKE_PREFIX_PATH "${THIRDPARTY_BUILD_DIR}/boost/build")
# Module-mode stragglers (older CMake toolchains) honor these too.
set(BOOST_ROOT "${THIRDPARTY_BUILD_DIR}/boost/build")
set(Boost_NO_SYSTEM_PATHS ON)
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME ON)

# ---------------------------------------------------------------------------
# libp2p transitive closure - libp2p's installed config find_depends:
# Boost, OpenSSL, Protobuf(+absl/utf8_range), c-ares, fmt, yaml-cpp,
# soralog, tsl_hat_trie, Boost.DI, SQLiteModernCpp(+sqlite3).
# Every hint points inside the artifact; nothing resolves from the system.
# ---------------------------------------------------------------------------
set(OPENSSL_ROOT_DIR "${THIRDPARTY_BUILD_DIR}/openssl/build")
set(OPENSSL_INCLUDE_DIR "${THIRDPARTY_BUILD_DIR}/openssl/build/include")
set(Protobuf_DIR "${THIRDPARTY_BUILD_DIR}/protobuf/lib/cmake/protobuf")
set(absl_DIR "${THIRDPARTY_BUILD_DIR}/protobuf/lib/cmake/absl")
set(utf8_range_DIR "${THIRDPARTY_BUILD_DIR}/protobuf/lib/cmake/utf8_range")
set(c-ares_DIR "${THIRDPARTY_BUILD_DIR}/cares/lib/cmake/c-ares")
set(fmt_DIR "${THIRDPARTY_BUILD_DIR}/fmt/lib/cmake/fmt")
set(yaml-cpp_DIR "${THIRDPARTY_BUILD_DIR}/yaml-cpp/lib/cmake/yaml-cpp")
set(soralog_DIR "${THIRDPARTY_BUILD_DIR}/soralog/lib/cmake/soralog")
set(tsl_hat_trie_DIR "${THIRDPARTY_BUILD_DIR}/tsl_hat_trie/lib/cmake/tsl_hat_trie")
set(Boost.DI_DIR "${THIRDPARTY_BUILD_DIR}/Boost.DI/lib/cmake/Boost.DI")
set(SQLiteModernCpp_DIR "${THIRDPARTY_BUILD_DIR}/SQLiteModernCpp/lib/cmake/SQLiteModernCpp")
set(sqlite3_DIR "${THIRDPARTY_BUILD_DIR}/sqlite3/cmake")

# RocksDB needs Snappy forwarded through its config's find_dependency.
set(Snappy_DIR "${THIRDPARTY_BUILD_DIR}/snappy/lib/cmake/Snappy")
set(RocksDB_DIR "${THIRDPARTY_BUILD_DIR}/rocksdb/lib/cmake/rocksdb")

# ZLIB: the artifact's protobuf and miniocpp link interfaces reference the
# superbuild's ZLIB::ZLIBSTATIC imported target, which plain find_package
# never defines - import the artifact's targets file directly (the same
# file the thirdparty superbuild injects via its FindZLIB shim +
# ZLIB_SUPERBUILD_TARGETS_FILE). Must precede every find_package whose
# config carries a ZLIB link interface.
set(ZLIB_DIR "${THIRDPARTY_BUILD_DIR}/zlib/lib/cmake/zlib")
set(ZLIB_ROOT "${THIRDPARTY_BUILD_DIR}/zlib")
if(EXISTS "${ZLIB_DIR}/ZLIB-static.cmake")
    include("${ZLIB_DIR}/ZLIB-static.cmake")
endif()
# CACHE so dependent tooling (out_of_tree_consumer_proof forwards this to its
# inner consumer, mirroring the superbuild's FindZLIB shim contract) sees it.
set(ZLIB_SUPERBUILD_TARGETS_FILE "${ZLIB_DIR}/ZLIB-static.cmake" CACHE FILEPATH
    "Artifact file defining the ZLIB::ZLIBSTATIC imported target")
# The vendored FindZLIB shim (cmake/FindZLIB.cmake, same module the thirdparty
# superbuild injects) makes every find_package(ZLIB) in this tree - including
# ones re-run inside installed config chains - resolve ZLIB::ZLIBSTATIC from
# the artifact without ever executing ZLIBConfig.cmake (the httplib
# set_and_check corruption trap). Prepend so it wins over any builtin.
list(PREPEND CMAKE_MODULE_PATH "${PROJECT_ROOT}/cmake")

# ---------------------------------------------------------------------------
# Direct dependencies - mirrors the root CMakeLists.txt ordering exactly.
# ---------------------------------------------------------------------------
# NO version argument (BACK-03 dual-RocksDB compat posture).
set(RocksDB_DIR "${THIRDPARTY_BUILD_DIR}/rocksdb/lib/cmake/rocksdb")

set(libp2p_DIR "${THIRDPARTY_BUILD_DIR}/libp2p/lib/cmake/libp2p")
find_package(libp2p CONFIG REQUIRED)

# NO version argument (BACK-03 dual-RocksDB compat posture) - same position
# as the root CMakeLists: resolved before the httplib chain.
find_package(RocksDB CONFIG REQUIRED)

# httplib chain - ORDER MATTERS (Phase-3 spike finding, preserved from the
# root CMakeLists and the GeniusSDK consumption block): httplib MUST be
# found EXPLICITLY first; miniocpp's consumer config re-runs miniocpp-deps
# in THIS context (pugixml/nlohmann_json/httplib must resolve here too),
# and its QUIET sub-find does not register httplib::httplib for
# imported-target-reference verification.
set(pugixml_DIR "${THIRDPARTY_BUILD_DIR}/pugixml/lib/cmake/pugixml")
set(nlohmann_json_DIR "${THIRDPARTY_BUILD_DIR}/json/share/cmake/nlohmann_json")
set(httplib_DIR "${THIRDPARTY_BUILD_DIR}/minio-cpp/lib/cmake/httplib")
set(miniocpp_DIR "${THIRDPARTY_BUILD_DIR}/minio-cpp/lib/cmake/miniocpp")
find_package(httplib CONFIG REQUIRED)
find_package(miniocpp CONFIG REQUIRED)

find_package(nlohmann_json CONFIG REQUIRED)

set(ipfs-lite-cpp_DIR "${THIRDPARTY_BUILD_DIR}/ipfs-lite-cpp/lib/cmake/ipfs-lite-cpp")
find_package(ipfs-lite-cpp CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Sources (SDK pattern: the platform entry adds src/ and test/ directly;
# the root CMakeLists stays untouched for the superbuild path).
# ---------------------------------------------------------------------------
include(${PROJECT_ROOT}/cmake/functions.cmake)
add_subdirectory(${PROJECT_ROOT}/src ${CMAKE_BINARY_DIR}/src)

# Install rules - the same contract the root CMakeLists.txt installs under
# the superbuild (out_of_tree_consumer_proof consumes the install tree via
# ${CMAKE_INSTALL_PREFIX}/lib/cmake/sgns-blockstore). CI runs
# `cmake --install` between build and ctest.
include(GNUInstallDirs)
install(DIRECTORY "${PROJECT_ROOT}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.hpp")

install(TARGETS sgns-blockstore
    EXPORT sgns-blockstoreTargets
    DESTINATION "${CMAKE_INSTALL_LIBDIR}")

set(SGNS_BLOCKSTORE_CONFIG_DESTINATION_DIR "lib/cmake/sgns-blockstore")
install(EXPORT sgns-blockstoreTargets
    FILE sgns-blockstoreTargets.cmake
    NAMESPACE sgns-blockstore::
    DESTINATION ${SGNS_BLOCKSTORE_CONFIG_DESTINATION_DIR})

include(CMakePackageConfigHelpers)
configure_package_config_file(${PROJECT_ROOT}/cmake/config.cmake.in
    "${CMAKE_BINARY_DIR}/sgns-blockstoreConfig.cmake"
    INSTALL_DESTINATION ${SGNS_BLOCKSTORE_CONFIG_DESTINATION_DIR}
)
write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/sgns-blockstoreConfigVersion.cmake"
    VERSION 1.0.0
    COMPATIBILITY AnyNewerVersion
)
install(FILES
    ${CMAKE_BINARY_DIR}/sgns-blockstoreConfig.cmake
    ${CMAKE_BINARY_DIR}/sgns-blockstoreConfigVersion.cmake
    DESTINATION ${SGNS_BLOCKSTORE_CONFIG_DESTINATION_DIR}
)

option(TESTING "Build tests" ON)
if(TESTING)
    enable_testing()
    set(GTest_DIR "${THIRDPARTY_BUILD_DIR}/GTest/lib/cmake/GTest")
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(${PROJECT_ROOT}/test ${CMAKE_BINARY_DIR}/test)
endif()
