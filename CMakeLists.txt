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
