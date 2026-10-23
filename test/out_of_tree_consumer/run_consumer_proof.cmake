# run_consumer_proof.cmake - script-mode ctest driver for the Phase-6
# out-of-tree install-tree consumer proof (LIB-02, D-08).
#
# Hermeticity (T-06-07): every *_DIR hint comes from the sub-build's
# already-resolved cache and points at superbuild install trees, so
# miniocpp-deps.cmake resolves httplib/nlohmann_json/pugixml via config
# dirs and its git-clone-source fallback can never run. Zero network:
# the consumer only constructs a client against a non-listening endpoint.
#
# Registered from test/CMakeLists.txt as:
#   add_test(NAME out_of_tree_consumer_proof
#            COMMAND ${CMAKE_COMMAND} -DSRC_DIR=... -DBUILD_DIR=...
#                    [hint set] -P run_consumer_proof.cmake)

if(NOT DEFINED SRC_DIR)
  message(FATAL_ERROR "run_consumer_proof: SRC_DIR must be defined")
endif()
if(NOT DEFINED BUILD_DIR)
  message(FATAL_ERROR "run_consumer_proof: BUILD_DIR must be defined")
endif()

set(CONFIG_ARG "Debug")
if(DEFINED CONFIG)
  set(CONFIG_ARG "${CONFIG}")
endif()

# Script mode does not define CMAKE_EXECUTABLE_SUFFIX - default it per
# host or the RUN step looks for an extensionless binary.
if(NOT DEFINED CMAKE_EXECUTABLE_SUFFIX)
  if(WIN32)
    set(CMAKE_EXECUTABLE_SUFFIX ".exe")
  else()
    set(CMAKE_EXECUTABLE_SUFFIX "")
  endif()
endif()

# Clean slate per run - the consumer build dir is disposable.
file(REMOVE_RECURSE "${BUILD_DIR}")

# Windows: no cl on PATH and the script-mode cmake default generator is
# NOT the VS generator - pin it explicitly (the superbuild's own choice).
set(GEN_ARGS)
if(WIN32)
  list(APPEND GEN_ARGS -G "Visual Studio 17 2022" -A x64)
endif()

# The hint closure forwarded verbatim from the sub-build's resolved cache
# (plus the composed CMAKE_PREFIX_PATH carrying Boost.DI, whose dotted
# cache name cannot ride a command line -D). CMAKE_MODULE_PATH +
# ZLIB_SUPERBUILD_TARGETS_FILE keep protobuf::libprotobuf's ZLIB::ZLIB
# reference resolving through the superbuild FindZLIB shim.
set(HINTS)
foreach(_v
    CMAKE_PREFIX_PATH
    CMAKE_MODULE_PATH
    ZLIB_SUPERBUILD_TARGETS_FILE
    CMAKE_MSVC_RUNTIME_LIBRARY
    Boost_USE_STATIC_RUNTIME
    GSL_INCLUDE_DIR
    sgns-blockstore_DIR
    libp2p_DIR
    RocksDB_DIR
    Snappy_DIR
    miniocpp_DIR
    httplib_DIR
    nlohmann_json_DIR
    pugixml_DIR
    ipfs-lite-cpp_DIR
    Boost_DIR
    Protobuf_DIR
    c-ares_DIR
    fmt_DIR
    yaml-cpp_DIR
    soralog_DIR
    spdlog_DIR
    tsl_hat_trie_DIR
    SQLiteModernCpp_DIR
    sqlite3_DIR
    absl_DIR
    utf8_range_DIR
    OPENSSL_ROOT_DIR
    OPENSSL_USE_STATIC_LIBS
    OPENSSL_MSVC_STATIC_RT
    )
  if(DEFINED ${_v})
    # Escape semicolons inside list-valued hints (CMAKE_PREFIX_PATH) so
    # the unquoted ${HINTS} expansion keeps each -D as ONE argument.
    set(_val "${${_v}}")
    string(REPLACE ";" "\\;" _val "${_val}")
    list(APPEND HINTS "-D${_v}=${_val}")
  endif()
endforeach()

execute_process(
    COMMAND ${CMAKE_COMMAND} -S "${SRC_DIR}" -B "${BUILD_DIR}"
            ${GEN_ARGS} ${HINTS}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_out
    ERROR_VARIABLE _configure_out)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR
    "out_of_tree_consumer_proof: CONFIGURE step failed "
    "(exit ${_configure_result}). This is the D-07 forced-failure shape "
    "when config.cmake.in is missing a find_dependency the export "
    "requires:\n${_configure_out}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${BUILD_DIR}" --config "${CONFIG_ARG}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_out
    ERROR_VARIABLE _build_out)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR
    "out_of_tree_consumer_proof: BUILD step failed "
    "(exit ${_build_result}):\n${_build_out}")
endif()

# Multi-config generators put the exe under <config>/; single-config at
# the build root. Cover both.
set(_exe "${BUILD_DIR}/${CONFIG_ARG}/consumer_proof${CMAKE_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${_exe}")
  set(_exe "${BUILD_DIR}/consumer_proof${CMAKE_EXECUTABLE_SUFFIX}")
endif()
if(NOT EXISTS "${_exe}")
  message(FATAL_ERROR
    "out_of_tree_consumer_proof: RUN step failed - executable not found "
    "at ${_exe}")
endif()

execute_process(
    COMMAND "${_exe}"
    RESULT_VARIABLE _run_result
    OUTPUT_VARIABLE _run_out
    ERROR_VARIABLE _run_out)
if(NOT _run_result EQUAL 0)
  message(FATAL_ERROR
    "out_of_tree_consumer_proof: RUN step failed (exit ${_run_result}). "
    "Construction against a non-listening endpoint must SUCCEED (D-06 "
    "zero-network-I/O contract) - output: ${_run_out}")
endif()
message(STATUS "out_of_tree_consumer_proof: ${_run_out}")
