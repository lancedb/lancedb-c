# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.15)
project(lancedb-c VERSION 0.21.2)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(RUST_TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/target/debug")
  set(RUST_BUILD_TYPE "")
else()
  set(RUST_TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/target/release")
  set(RUST_BUILD_TYPE "--release")
endif()
set(RUST_LIB_PATH "${RUST_TARGET_DIR}/liblancedb.so")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ARROW REQUIRED arrow)

# rust-lib target
add_custom_target(rust-lib
  COMMAND cargo build ${RUST_BUILD_TYPE}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Building Rust library..."
  BYPRODUCTS ${RUST_LIB_PATH}
)

# lancedb target
add_library(lancedb SHARED IMPORTED)
set_target_properties(lancedb PROPERTIES
  IMPORTED_LOCATION ${RUST_LIB_PATH}
)
add_dependencies(lancedb rust-lib)

# simple example
add_executable(simple examples/simple.cpp)
target_link_libraries(simple
  lancedb
  Threads::Threads
  ${ARROW_LIBRARIES}
)
target_include_directories(simple
  PRIVATE ${ARROW_INCLUDE_DIRS}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(simple PRIVATE ${ARROW_CFLAGS_OTHER})
set_target_properties(simple PROPERTIES
  BUILD_RPATH ${RUST_TARGET_DIR}
)

# full example
add_executable(full examples/full.cpp)
target_link_libraries(full
  lancedb
  Threads::Threads
  ${ARROW_LIBRARIES}
)
target_include_directories(full
  PRIVATE ${ARROW_INCLUDE_DIRS}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(full PRIVATE ${ARROW_CFLAGS_OTHER})
set_target_properties(full PROPERTIES
  BUILD_RPATH ${RUST_TARGET_DIR}
)

# s3 storage example
add_executable(s3 examples/s3.cpp)
target_link_libraries(s3
  lancedb
  Threads::Threads
  ${ARROW_LIBRARIES}
)
target_include_directories(s3
  PRIVATE ${ARROW_INCLUDE_DIRS}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(s3 PRIVATE ${ARROW_CFLAGS_OTHER})
set_target_properties(s3 PROPERTIES
  BUILD_RPATH ${RUST_TARGET_DIR}
)

add_custom_target(examples DEPENDS simple full s3)

# Optional test framework
option(BUILD_TESTS "Build tests" OFF)

if(BUILD_TESTS)
  # Find or fetch Catch2 v2
  find_package(Catch2 2 QUIET)

  if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found, fetching from GitHub...")
    include(FetchContent)
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG        v2.13.10
    )
    FetchContent_MakeAvailable(Catch2)
  endif()

  # Add test executable
  add_executable(lancedb_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_connection.cpp
    tests/test_table.cpp
  )
  target_link_libraries(lancedb_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Enable CTest
  enable_testing()

  # Check if valgrind is available
  find_program(VALGRIND_EXECUTABLE valgrind)
  if(VALGRIND_EXECUTABLE)
    message(STATUS "Valgrind found: ${VALGRIND_EXECUTABLE}")
    # Run tests with valgrind
    add_test(NAME lancedb_unit_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite,possible
        --track-origins=yes
        --error-exitcode=0
        --log-file=${CMAKE_BINARY_DIR}/valgrind_output.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_tests>
    )
  else()
    message(WARNING "Valgrind not found, running tests without memory checking")
    add_test(NAME lancedb_unit_tests COMMAND lancedb_tests)
  endif()
endif()

add_custom_target(clean-all
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target clean
  COMMAND cargo clean
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Cleaning all build artifacts..."
)

# Check target for code quality
add_custom_target(check
  COMMAND cargo fmt --check
  COMMAND cargo clippy
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Running Rust formatting and linting checks..."
)

message(STATUS "=== LanceDB C/C++ Bindings Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Rust library path: ${RUST_LIB_PATH}")
message(STATUS "Arrow library path: ${ARROW_LIBRARIES}")
if(BUILD_TESTS)
  message(STATUS "Tests: Enabled")
else()
  message(STATUS "Tests: Disabled (use -DBUILD_TESTS=ON to enable)")
endif()
message(STATUS "============================================")
