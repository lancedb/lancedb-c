# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.22)
project(lancedb_find_package_consumer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lancedb CONFIG REQUIRED)

add_executable(lancedb_c_find_package_consumer
  "${CMAKE_CURRENT_LIST_DIR}/../c17_consumer.c")
target_link_libraries(lancedb_c_find_package_consumer PRIVATE lancedb::lancedb)

add_executable(lancedb_cxx_find_package_consumer
  "${CMAKE_CURRENT_LIST_DIR}/../package_consumer.cpp")
target_link_libraries(lancedb_cxx_find_package_consumer PRIVATE lancedb::lancedb)
