# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.22)
project(lancedb_find_package_version LANGUAGES C)

if(LANCEDB_FIND_VERSION STREQUAL "")
  find_package(lancedb CONFIG REQUIRED)
elseif(LANCEDB_FIND_EXACT)
  find_package(lancedb ${LANCEDB_FIND_VERSION} CONFIG REQUIRED EXACT)
else()
  find_package(lancedb ${LANCEDB_FIND_VERSION} CONFIG REQUIRED)
endif()

message(STATUS "selected lancedb_VERSION=${lancedb_VERSION}")
