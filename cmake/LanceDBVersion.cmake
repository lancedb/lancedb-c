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
      set(LANCEDB_TAGS_REMAINING "${LANCEDB_TAGS_OUTPUT}")
      while(NOT "${LANCEDB_TAGS_REMAINING}" STREQUAL "")
        string(FIND "${LANCEDB_TAGS_REMAINING}" "\n" LANCEDB_TAG_END)
        if(LANCEDB_TAG_END EQUAL -1)
          set(LANCEDB_TAG "${LANCEDB_TAGS_REMAINING}")
          set(LANCEDB_TAGS_REMAINING "")
        else()
          string(SUBSTRING "${LANCEDB_TAGS_REMAINING}" 0 ${LANCEDB_TAG_END} LANCEDB_TAG)
          math(EXPR LANCEDB_TAG_NEXT "${LANCEDB_TAG_END} + 1")
          string(SUBSTRING "${LANCEDB_TAGS_REMAINING}" ${LANCEDB_TAG_NEXT} -1 LANCEDB_TAGS_REMAINING)
        endif()
        if(NOT "${LANCEDB_TAG}" MATCHES "^v(.*)$")
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
      endwhile()
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

# The source header keeps these defaults so it compiles directly without a
# generated file. CMake renders the block with the resolved build version for
# the installed header. The package builder performs the same rendering.
set(LANCEDB_HEADER_VERSION_DEFAULT "#define LANCEDB_VERSION_MAJOR 0
#define LANCEDB_VERSION_MINOR 0
#define LANCEDB_VERSION_PATCH 0
#define LANCEDB_VERSION_STRING \"0.0.0-dev\"")

function(lancedb_render_header source destination version)
  if(NOT version STREQUAL LANCEDB_DEV_VERSION)
    lancedb_parse_version("${version}" parsed_version)
    if(NOT parsed_version STREQUAL version)
      message(FATAL_ERROR "Cannot render invalid lancedb-c version '${version}'")
    endif()
  endif()

  string(REGEX MATCH "^([0-9]+)[.]([0-9]+)[.]([0-9]+)" version_core "${version}")
  set(major "${CMAKE_MATCH_1}")
  set(minor "${CMAKE_MATCH_2}")
  set(patch "${CMAKE_MATCH_3}")
  if(version_core STREQUAL "" OR major STREQUAL "" OR minor STREQUAL "" OR patch STREQUAL "")
    message(FATAL_ERROR "Cannot render lancedb-c version '${version}' without a numeric core")
  endif()

  set(rendered "#define LANCEDB_VERSION_MAJOR ${major}
#define LANCEDB_VERSION_MINOR ${minor}
#define LANCEDB_VERSION_PATCH ${patch}
#define LANCEDB_VERSION_STRING \"${version}\"")

  file(READ "${source}" source_text)
  string(FIND "${source_text}" "\r" carriage_return)
  if(NOT carriage_return EQUAL -1)
    message(FATAL_ERROR "${source} must use LF line endings")
  endif()
  string(FIND "${source_text}" "${LANCEDB_HEADER_VERSION_DEFAULT}" default_position)
  if(default_position EQUAL -1)
    message(FATAL_ERROR "${source} does not contain the default LANCEDB_VERSION macros")
  endif()
  string(REPLACE "${LANCEDB_HEADER_VERSION_DEFAULT}" "${rendered}" rendered_text "${source_text}")

  get_filename_component(destination_directory "${destination}" DIRECTORY)
  file(MAKE_DIRECTORY "${destination_directory}")
  file(WRITE "${destination}" "${rendered_text}")
endfunction()

# Tag refs are inputs to the configure-time result. Track them so an existing
# build directory reconfigures when HEAD or its tags change, instead of silently
# reusing the previous version. An environment override still needs a manual
# configuration step because CMake cannot observe arbitrary environment changes.
if(LANCEDB_GIT_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  foreach(git_directory_arg IN ITEMS --git-dir --git-common-dir)
    execute_process(
      COMMAND "${LANCEDB_GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
              rev-parse "${git_directory_arg}"
      RESULT_VARIABLE git_directory_result
      OUTPUT_VARIABLE git_directory
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT git_directory_result EQUAL 0 OR git_directory STREQUAL "")
      continue()
    endif()
    if(NOT IS_ABSOLUTE "${git_directory}")
      set(git_directory "${CMAKE_CURRENT_SOURCE_DIR}/${git_directory}")
    endif()
    foreach(relative IN ITEMS HEAD packed-refs refs refs/tags)
      if(EXISTS "${git_directory}/${relative}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
          "${git_directory}/${relative}")
      endif()
    endforeach()
  endforeach()
endif()
