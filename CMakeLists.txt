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

# Optional documentation
option(BUILD_DOCS "Build documentation" OFF)

if(BUILD_DOCS)
  # Find Doxygen
  find_package(Doxygen REQUIRED)

  # Find Sphinx
  find_program(SPHINX_EXECUTABLE
    NAMES sphinx-build
    DOC "Sphinx documentation generator"
  )

  if(NOT SPHINX_EXECUTABLE)
    message(FATAL_ERROR "Sphinx not found. Install with: pip install sphinx sphinx-rtd-theme breathe")
  endif()

  # Doxygen target - generates XML from header files
  set(DOXYGEN_INPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/lancedb.h)
  set(DOXYGEN_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/xml)
  set(DOXYFILE_IN ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile)
  set(DOXYFILE_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

  # Configure Doxyfile to use build directory for output
  configure_file(${DOXYFILE_IN} ${DOXYFILE_OUT} @ONLY)

  add_custom_target(doxygen
    COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYFILE_OUT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating Doxygen XML..."
    VERBATIM
  )

  # Sphinx target - builds HTML documentation
  set(SPHINX_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/docs)
  set(SPHINX_BUILD ${CMAKE_CURRENT_BINARY_DIR}/docs/html)

  add_custom_target(docs
    COMMAND ${CMAKE_COMMAND} -E env DOXYGEN_XML_DIR=${DOXYGEN_OUTPUT_DIR}
      ${SPHINX_EXECUTABLE} -b html
      -W --keep-going
      ${SPHINX_SOURCE} ${SPHINX_BUILD}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS doxygen
    COMMENT "Building HTML documentation with Sphinx..."
    VERBATIM
  )
endif()

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

  # Add test executable for connection tests (runs with valgrind)
  add_executable(lancedb_connection_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_connection.cpp
  )
  target_link_libraries(lancedb_connection_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_connection_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_connection_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_connection_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Add test executable for table tests (runs with valgrind)
  add_executable(lancedb_table_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_table.cpp
  )
  target_link_libraries(lancedb_table_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_table_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_table_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_table_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Add test executable for scalar index tests (runs with valgrind)
  add_executable(lancedb_index_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_index.cpp
  )
  target_link_libraries(lancedb_index_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_index_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_index_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_index_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Add test executable for vector tests (runs without valgrind)
  add_executable(lancedb_vector_index_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_vector_index.cpp
  )
  target_link_libraries(lancedb_vector_index_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_vector_index_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_vector_index_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_vector_index_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Add test executable for query tests (runs with valgrind)
  add_executable(lancedb_query_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_query.cpp
  )
  target_link_libraries(lancedb_query_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_query_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_query_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_query_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Add test executable for vector query tests (runs without valgrind)
  add_executable(lancedb_vector_query_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_vector_query.cpp
  )
  target_link_libraries(lancedb_vector_query_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_vector_query_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_vector_query_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_vector_query_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Enable CTest
  enable_testing()

  # Check if valgrind is available
  find_program(VALGRIND_EXECUTABLE valgrind)
  if(VALGRIND_EXECUTABLE)
    message(STATUS "Valgrind found: ${VALGRIND_EXECUTABLE}")
    # Run connection tests with valgrind
    # Only fail on definite leaks, not "possibly lost" from Rust runtime
    add_test(NAME lancedb_connection_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite
        --errors-for-leak-kinds=definite
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_connection.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_connection_tests>
    )
    # Run table tests with valgrind
    add_test(NAME lancedb_table_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite
        --errors-for-leak-kinds=definite
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_table.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_table_tests>
    )
    # Run scalar index tests with valgrind
    add_test(NAME lancedb_index_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite
        --errors-for-leak-kinds=definite
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_index.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_index_tests>
    )
    # Run query tests with valgrind
    add_test(NAME lancedb_query_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite
        --errors-for-leak-kinds=definite
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_query.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_query_tests>
    )
  else()
    message(WARNING "Valgrind not found, running tests without memory checking")
    add_test(NAME lancedb_connection_tests COMMAND lancedb_connection_tests)
    add_test(NAME lancedb_table_tests COMMAND lancedb_table_tests)
    add_test(NAME lancedb_index_tests COMMAND lancedb_index_tests)
    add_test(NAME lancedb_query_tests COMMAND lancedb_query_tests)
  endif()

  # Run vector index tests WITHOUT valgrind (too slow under valgrind)
  add_test(NAME lancedb_vector_index_tests COMMAND lancedb_vector_index_tests)
  # Run vector query tests WITHOUT valgrind (too slow under valgrind)
  add_test(NAME lancedb_vector_query_tests COMMAND lancedb_vector_query_tests)
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
if(BUILD_DOCS)
  message(STATUS "Documentation: Enabled")
  message(STATUS "  - HTML output: ${SPHINX_BUILD}/index.html")
else()
  message(STATUS "Documentation: Disabled (use -DBUILD_DOCS=ON to enable)")
endif()
message(STATUS "============================================")
