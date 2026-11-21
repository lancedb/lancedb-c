# SPDX-License-Identifier: Apache-2.0

# Validate a strict SemVer string without the v prefix and return the
# normalized value. The policy matches .github/scripts/version.py: ASCII
# digits, no leading zeroes in core or numeric prerelease identifiers, no empty
# identifiers, and optional build metadata.
function(lancedb_parse_version input out_var)
  set(${out_var} "" PARENT_SCOPE)
  if(NOT input MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)(-([0-9A-Za-z.-]+))?([+]([0-9A-Za-z.-]+))?$")
    return()
  endif()

  set(major "${CMAKE_MATCH_1}")
  set(minor "${CMAKE_MATCH_2}")
  set(patch "${CMAKE_MATCH_3}")
  set(prerelease "${CMAKE_MATCH_5}")
  set(build "${CMAKE_MATCH_7}")

  foreach(part IN ITEMS "${major}" "${minor}" "${patch}")
    if(part MATCHES "^0[0-9]+$")
      return()
    endif()
  endforeach()

  if(NOT prerelease STREQUAL "")
    if(prerelease MATCHES "(^[.]|[.]$|[.][.])")
      return()
    endif()
    string(REPLACE "." ";" identifiers "${prerelease}")
    foreach(identifier IN LISTS identifiers)
      if(identifier STREQUAL "" OR NOT identifier MATCHES "^[0-9A-Za-z-]+$")
        return()
      endif()
      if(identifier MATCHES "^[0-9]+$" AND identifier MATCHES "^0[0-9]+$")
        return()
      endif()
    endforeach()
  endif()

  if(NOT build STREQUAL "")
    if(build MATCHES "(^[.]|[.]$|[.][.])")
      return()
    endif()
    string(REPLACE "." ";" identifiers "${build}")
    foreach(identifier IN LISTS identifiers)
      if(identifier STREQUAL "" OR NOT identifier MATCHES "^[0-9A-Za-z-]+$")
        return()
      endif()
    endforeach()
  endif()

  set(version "${major}.${minor}.${patch}")
  if(NOT prerelease STREQUAL "")
    string(APPEND version "-${prerelease}")
  endif()
  if(NOT build STREQUAL "")
    string(APPEND version "+${build}")
  endif()
  set(${out_var} "${version}" PARENT_SCOPE)
endfunction()

# Determine the version from a verified override or the exact Git tag refs at
# HEAD. If several valid tags point at HEAD, prefer the last stable tag in
# Git's version sort, otherwise the last prerelease. Untagged trees and source
# archives without their own Git repository report 0.0.0-dev.
set(LANCEDB_DEV_VERSION "0.0.0-dev")

if(DEFINED ENV{LANCEDB_VERSION} AND NOT "$ENV{LANCEDB_VERSION}" STREQUAL "")
  set(LANCEDB_VERSION "$ENV{LANCEDB_VERSION}")
  if(LANCEDB_VERSION STREQUAL LANCEDB_DEV_VERSION)
    message(FATAL_ERROR "LANCEDB_VERSION ${LANCEDB_DEV_VERSION} is reserved for untagged builds")
  endif()
  lancedb_parse_version("${LANCEDB_VERSION}" LANCEDB_PARSED_VERSION)
  if(NOT LANCEDB_PARSED_VERSION STREQUAL LANCEDB_VERSION)
    message(FATAL_ERROR "Invalid lancedb-c version '${LANCEDB_VERSION}'; expected strict SemVer without the v prefix")
  endif()
else()
  set(LANCEDB_VERSION "${LANCEDB_DEV_VERSION}")
  find_program(LANCEDB_GIT_EXECUTABLE NAMES git)
  if(LANCEDB_GIT_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
      COMMAND "${LANCEDB_GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
              rev-parse HEAD
      RESULT_VARIABLE LANCEDB_HEAD_RESULT
      OUTPUT_VARIABLE LANCEDB_HEAD_COMMIT
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
      COMMAND "${LANCEDB_GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
              tag --points-at HEAD --sort=version:refname
      RESULT_VARIABLE LANCEDB_TAGS_RESULT
      OUTPUT_VARIABLE LANCEDB_TAGS_OUTPUT
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(LANCEDB_HEAD_RESULT EQUAL 0 AND LANCEDB_TAGS_RESULT EQUAL 0)
      unset(LANCEDB_LAST_STABLE)
      unset(LANCEDB_LAST_PRERELEASE)
      string(REPLACE "\n" ";" LANCEDB_TAGS "${LANCEDB_TAGS_OUTPUT}")
      foreach(LANCEDB_TAG IN LISTS LANCEDB_TAGS)
        if(NOT LANCEDB_TAG MATCHES "^v(.*)$")
          continue()
        endif()
        lancedb_parse_version("${CMAKE_MATCH_1}" LANCEDB_TAG_VERSION)
        if(LANCEDB_TAG_VERSION STREQUAL "" OR LANCEDB_TAG_VERSION STREQUAL LANCEDB_DEV_VERSION)
          continue()
        endif()
        execute_process(
          COMMAND "${LANCEDB_GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                  rev-parse "refs/tags/${LANCEDB_TAG}^{}"
          RESULT_VARIABLE LANCEDB_PEEL_RESULT
          OUTPUT_VARIABLE LANCEDB_PEELED_COMMIT
          ERROR_QUIET
          OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT LANCEDB_PEEL_RESULT EQUAL 0 OR NOT LANCEDB_PEELED_COMMIT STREQUAL LANCEDB_HEAD_COMMIT)
          continue()
        endif()
        string(REGEX REPLACE "[+].*$" "" LANCEDB_TAG_WITHOUT_BUILD "${LANCEDB_TAG_VERSION}")
        if(LANCEDB_TAG_WITHOUT_BUILD MATCHES "-")
          set(LANCEDB_LAST_PRERELEASE "${LANCEDB_TAG_VERSION}")
        else()
          set(LANCEDB_LAST_STABLE "${LANCEDB_TAG_VERSION}")
        endif()
      endforeach()
      if(LANCEDB_LAST_STABLE)
        set(LANCEDB_VERSION "${LANCEDB_LAST_STABLE}")
      elseif(LANCEDB_LAST_PRERELEASE)
        set(LANCEDB_VERSION "${LANCEDB_LAST_PRERELEASE}")
      endif()
    endif()
  endif()
endif()

if(NOT LANCEDB_VERSION STREQUAL LANCEDB_DEV_VERSION)
  lancedb_parse_version("${LANCEDB_VERSION}" LANCEDB_PARSED_VERSION)
  if(NOT LANCEDB_PARSED_VERSION STREQUAL LANCEDB_VERSION)
    message(FATAL_ERROR "Invalid lancedb-c version '${LANCEDB_VERSION}'")
  endif()
endif()
string(REGEX MATCH "^[0-9]+[.][0-9]+[.][0-9]+" LANCEDB_PROJECT_VERSION "${LANCEDB_VERSION}")
