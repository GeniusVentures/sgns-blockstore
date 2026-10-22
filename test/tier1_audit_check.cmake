# tier1_audit_check.cmake - CMake script-mode (-P) static audit gating the
# Tier-1 hermetic contract (D-14, Phase 5). Fails with file:line when any
# test source contains a fixed-sleep construct outside the allowlist (sole
# allowlisted file: MinioServerHarness.cpp - the bounded WaitUntilHealthy
# deadline poll, legitimate readiness polling, Tier-2 support code) or a
# non-loopback host literal (only 127.0.0.1, localhost, and ::1 are
# allowed; play.min.io is explicitly banned), and when test/CMakeLists.txt
# registers more tests than it labels with PROPERTIES LABELS (a new
# unlabeled test trips the audit before CI can run it - D-13/Pitfall 1
# belt-and-suspenders: ctest -L never selects unlabeled tests, so an
# unlabeled test would silently vanish from the hermetic lane).
#
# Registered from test/CMakeLists.txt as:
#   add_test(NAME tier1_static_audit
#            COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -P
#                    ${CMAKE_CURRENT_SOURCE_DIR}/tier1_audit_check.cmake)
# Shell-independent: runs anywhere ctest runs, including the CI lane.

if(NOT DEFINED SRC_DIR)
  message(FATAL_ERROR "tier1_audit_check: SRC_DIR must be defined (-DSRC_DIR=<repo root>)")
endif()

# Glob scope: test sources only - never *.cmake (this script must not
# self-match its own pattern tables) and never files outside test/.
file(GLOB_RECURSE _sources
    "${SRC_DIR}/test/*.cpp"
    "${SRC_DIR}/test/*.hpp"
    "${SRC_DIR}/test/conformance/*.hpp"
    )

if(NOT _sources)
  message(FATAL_ERROR "tier1 audit violation: no test sources found under ${SRC_DIR}/test")
endif()

# Pattern set 1 (fixed sleeps). The Win32 Sleep( form is matched
# case-sensitively alongside the POSIX/std forms.
set(_sleep_patterns
    "std::this_thread::sleep_for"
    "sleep_for"
    "sleep[(]"
    "usleep[(]"
    "nanosleep[(]"
    "Sleep[(]"
    )

# Allowlist for pattern set 1 (sleeps only - the host scan below still
# applies): MinioServerHarness.cpp's WaitUntilHealthy is the ONE bounded
# deadline poll in the tree (verified by grep this phase) - legitimate
# Tier-2 support code, not a fixed sleep.
set(_sleep_allowlist
    "MinioServerHarness.cpp"
    )

# Pattern set 2 (non-loopback hosts). Host literals reach code only as
# STRING literals ("..."), so the scan below splits each line on double
# quotes and applies these patterns to the QUOTED SEGMENTS ONLY - this
# kills every C++ qualified-name shape (rocksdb::DB*, minio::s3::) that
# merely resembles a compressed IPv6 form. Patterns run on the segment
# AFTER the three allowed loopback literals (127.0.0.1, localhost, ::1)
# are neutralized and the segment is lowercased. CMake regex has no
# interval quantifiers and no lookaround, so character classes carry
# the structure.
set(_host_patterns
    "play[.]min[.]io"
    "[.](com|io|net|org)([^a-z0-9.-]|$)"
    "[0-9]+[.][0-9]+[.][0-9]+[.][0-9]+"
    "::[0-9a-f]+([^0-9a-z_]|$)"
    ":[0-9a-f]+:[0-9a-f]+:[0-9a-f]+"
    )

set(_file_count 0)
foreach(_source IN LISTS _sources)
  math(EXPR _file_count "${_file_count} + 1")
  get_filename_component(_name "${_source}" NAME)

  # Sleep allowlist: excuses pattern set 1 for the named file only.
  set(_sleep_banned TRUE)
  foreach(_allowed IN LISTS _sleep_allowlist)
    if(_name STREQUAL "${_allowed}")
      set(_sleep_banned FALSE)
    endif()
  endforeach()

  file(STRINGS "${_source}" _lines)
  foreach(_line IN LISTS _lines)
    if(_sleep_banned)
      foreach(_pattern IN LISTS _sleep_patterns)
        if(_line MATCHES ".*${_pattern}.*")
          message(FATAL_ERROR "tier1 audit violation: fixed-sleep construct in ${_source}: ${_line}")
        endif()
      endforeach()
    endif()

    # Split on double quotes: part 1 = before the first quote, part 2 =
    # the first quoted segment, part 3 = between quotes, ... Even parts
    # (1-based) are inside string literals.
    string(REPLACE "\"" ";" _parts "${_line}")
    set(_part_idx 0)
    foreach(_part IN LISTS _parts)
      math(EXPR _part_idx "${_part_idx} + 1")
      math(EXPR _in_quote "${_part_idx} % 2")
      if(NOT _in_quote EQUAL 0)
        continue()
      endif()

      # Neutralize the allowed loopback literals before the host scan.
      # The ::1 neutralizer requires non-identifier boundaries on BOTH
      # sides so fe80::1 (a different, non-loopback address) survives
      # to be caught by the compressed-IPv6 pattern below.
      string(REPLACE "127.0.0.1" "__loopback__" _scan "${_part}")
      string(REPLACE "localhost" "__loopback__" _scan "${_scan}")
      string(REGEX REPLACE "(^|[^0-9a-zA-Z_])::1([^0-9a-zA-Z_]|$)" "\\1 __lb__ \\2" _scan "${_scan}")
      string(TOLOWER "${_scan}" _scan_low)
      foreach(_pattern IN LISTS _host_patterns)
        if(_scan_low MATCHES ".*${_pattern}.*")
          message(FATAL_ERROR "tier1 audit violation: non-loopback host literal in ${_source}: ${_line}")
        endif()
      endforeach()
    endforeach()
  endforeach()
endforeach()

# Registration/label count guard (D-14): every registration in
# test/CMakeLists.txt must carry a LABELS line.
set(_registrations 0)
set(_labels 0)
if(EXISTS "${SRC_DIR}/test/CMakeLists.txt")
  file(STRINGS "${SRC_DIR}/test/CMakeLists.txt" _cm_lines)
  foreach(_cm IN LISTS _cm_lines)
    if(_cm MATCHES "addtest[(]" OR _cm MATCHES "add_test[(]")
      math(EXPR _registrations "${_registrations} + 1")
    endif()
    if(_cm MATCHES "PROPERTIES LABELS")
      math(EXPR _labels "${_labels} + 1")
    endif()
  endforeach()
  if(_labels LESS _registrations)
    message(FATAL_ERROR "tier1 audit violation: ${_registrations} test registration(s) but only ${_labels} LABELS line(s) in ${SRC_DIR}/test/CMakeLists.txt - every registered test must carry a tier label (D-13)")
  endif()
endif()

message(STATUS "tier1 audit OK: ${_file_count} source file(s) clean, ${_labels}/${_registrations} registration(s) labeled")
