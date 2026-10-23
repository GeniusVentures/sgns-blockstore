# header_hygiene_check.cmake - CMake script-mode (-P) test gating the
# public-header include hygiene (CORE-01): installed headers must carry
# only CID/bytes/strings + std/gsl - never SuperGenius/GeniusSDK/
# GeniusWallet/ipfs_lite/minio shapes.
#
# Registered from test/CMakeLists.txt as:
#   add_test(NAME header_hygiene
#            COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -P
#                    ${CMAKE_CURRENT_SOURCE_DIR}/header_hygiene_check.cmake)
# Shell-independent: runs anywhere ctest runs, including the CI lane.

if(NOT DEFINED SRC_DIR)
  message(FATAL_ERROR "header_hygiene_check: SRC_DIR must be defined (-DSRC_DIR=<repo root>)")
endif()

file(GLOB_RECURSE _headers "${SRC_DIR}/include/sgns-blockstore/*.hpp")

if(NOT _headers)
  message(FATAL_ERROR "header hygiene violation: no headers found under ${SRC_DIR}/include/sgns-blockstore")
endif()

set(_banned
    "ipfs_lite/ipfs/"
    "SuperGenius/"
    "GeniusSDK/"
    "GeniusWallet/"
    "minio"
    # blake2 (Phase 6): the blake2b160.hpp include lives ONLY in
    # src/Cid.cpp - it must never reach an installed header (the check
    # greps include lines only, so Doxygen prose naming blake2b-256 in
    # @note blocks cannot trip it).
    "blake2"
    )

set(_count 0)
foreach(_header IN LISTS _headers)
  math(EXPR _count "${_count} + 1")
  file(STRINGS "${_header}" _lines)
  foreach(_line IN LISTS _lines)
    foreach(_pattern IN LISTS _banned)
      if(_line MATCHES ".*#include.*${_pattern}.*")
        message(FATAL_ERROR "header hygiene violation: ${_header}: ${_line}")
      endif()
    endforeach()
  endforeach()
endforeach()

message(STATUS "header hygiene OK: ${_count} headers clean")
