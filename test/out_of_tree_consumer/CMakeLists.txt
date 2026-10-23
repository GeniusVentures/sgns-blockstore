# 3.20 (the library root's own floor), NOT lower: CMAKE_MSVC_RUNTIME_
# LIBRARY only takes effect under CMP0091 (NEW from 3.15+) - with an
# older minimum the consumer TU silently builds MDd against the
# superbuild's MTd static-runtime libraries (LNK2038).
cmake_minimum_required(VERSION 3.20)

# sgns_blockstore_consumer_proof - the Phase-6 out-of-tree install-tree
# consumer (LIB-02/SC4, D-08). This is a STANDALONE project: it is never
# part of the sgns-blockstore build; the script-mode ctest driver
# (run_consumer_proof.cmake) configures/builds/runs it out-of-tree.
#
# The point: this TU and this link line are ALL an out-of-tree consumer
# gets - find_package the installed package, link the single exported
# target, include the single installed header. NO other find_package:
# every transitive dependency must resolve through the package-config
# chain plus the -D hints the driver forwards (D-07 prove-first: the
# consumer's forced configure failures are exactly the config.cmake.in
# hardening evidence).
project(sgns_blockstore_consumer_proof LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# cpp-httplib is found EXPLICITLY first, exactly like the library's own
# root CMakeLists (the Phase-3 spike finding, 2026-10-21): miniocpp's
# consumer config resolves httplib via a QUIET config find whose target
# does not register for find_package's imported-target-reference
# verification, leaving miniocpp_FOUND FALSE with "imported targets are
# referenced, but are missing: httplib::httplib". RESEARCH Risk 5: this
# fix belongs HERE (consumer side, httplib_DIR forwarded by the driver),
# never in config.cmake.in.
find_package(httplib CONFIG REQUIRED)

find_package(sgns-blockstore CONFIG REQUIRED)

add_executable(consumer_proof consumer_main.cpp)
target_link_libraries(consumer_proof PRIVATE sgns-blockstore::sgns-blockstore)

# The installed headers use gsl/span (the library's own build resolves it
# via GSL_INCLUDE_DIR); the driver forwards the same include dir.
if(GSL_INCLUDE_DIR)
    include_directories(${GSL_INCLUDE_DIR})
endif()
