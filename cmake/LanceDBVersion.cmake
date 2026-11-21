# SPDX-License-Identifier: Apache-2.0

# Determine the C bindings version from the exact Git tag at HEAD.
#
# A release workflow sets LANCEDB_VERSION only after verifying that the tag
# peels to the checked-out commit. Local tagged builds derive the version from
# Git; untagged trees and source archives without Git report 0.0.0-dev.
set(LANCEDB_DEV_VERSION "0.0.0-dev")

if(DEFINED ENV{LANCEDB_VERSION} AND NOT "$ENV{LANCEDB_VERSION}" STREQUAL "")
  set(LANCEDB_VERSION "$ENV{LANCEDB_VERSION}")
else()
  set(LANCEDB_VERSION "${LANCEDB_DEV_VERSION}")
  find_program(LANCEDB_GIT_EXECUTABLE NAMES git)
  if(LANCEDB_GIT_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
      COMMAND "${LANCEDB_GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
              describe --tags --exact-match --match "v[0-9]*" HEAD
      RESULT_VARIABLE LANCEDB_GIT_RESULT
      OUTPUT_VARIABLE LANCEDB_GIT_TAG
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(LANCEDB_GIT_RESULT EQUAL 0 AND
       LANCEDB_GIT_TAG MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?([+][0-9A-Za-z.-]+)?)$")
      set(LANCEDB_VERSION "${CMAKE_MATCH_1}")
    endif()
  endif()
endif()

if(NOT LANCEDB_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?([+][0-9A-Za-z.-]+)?$")
  message(FATAL_ERROR "Invalid lancedb-c version '${LANCEDB_VERSION}'")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" LANCEDB_PROJECT_VERSION "${LANCEDB_VERSION}")
