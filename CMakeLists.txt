# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.25)
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

if(CMAKE_CROSSCOMPILING AND NOT RUST_TARGET_TRIPLE)
  # Map CMake compiler architecture and platform to the corresponding Rust target triple.
  # Note: CMAKE_C_COMPILER_ARCHITECTURE_ID requires CMake 3.10+, with expanded support in 4.1+.
  if(WIN32)
    if(CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64" OR CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x86_64")
      if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        set(RUST_TARGET_TRIPLE "x86_64-pc-windows-gnu")
      elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        set(RUST_TARGET_TRIPLE "x86_64-pc-windows-gnullvm")
      elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        set(RUST_TARGET_TRIPLE "x86_64-pc-windows-msvc")
      endif()
    endif()
  elseif(APPLE)
    if(CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x86_64")
      set(RUST_TARGET_TRIPLE "x86_64-apple-darwin")
    elseif(CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "arm64")
      set(RUST_TARGET_TRIPLE "aarch64-apple-darwin")
    endif()
  elseif(LINUX)
    if(CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x86_64")
      set(RUST_TARGET_TRIPLE "x86_64-unknown-linux-gnu")
    elseif(CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "aarch64")
      set(RUST_TARGET_TRIPLE "aarch64-unknown-linux-gnu")
    endif()
  endif()

  if(RUST_TARGET_TRIPLE)
    message(STATUS "Cross-compiling: Automatically detected Rust target triple.")
  else()
    message(FATAL_ERROR "Cross-compiling: Failed to detect Rust target triple. Please set RUST_TARGET_TRIPLE manually.")
  endif()
endif()

set(RUST_TARGET_DIR target)

if(RUST_TARGET_TRIPLE)
  set(RUST_TARGET_DIR "${RUST_TARGET_DIR}/${RUST_TARGET_TRIPLE}")
  set(RUST_TARGET_OPT --target ${RUST_TARGET_TRIPLE})
else()
  execute_process(
    COMMAND rustc --print host-tuple
    OUTPUT_VARIABLE RUST_TARGET_TRIPLE
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/${RUST_TARGET_DIR}/debug")
  set(RUST_BUILD_TYPE "")
else()
  set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/${RUST_TARGET_DIR}/release")
  set(RUST_BUILD_TYPE "--release")
endif()
set(STATIC_LIB_PATH "${RUST_TARGET_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}lancedb${CMAKE_STATIC_LIBRARY_SUFFIX}")
if(WIN32)
  set(SHARED_LIB_PATH "${RUST_TARGET_DIR}/lancedb${CMAKE_SHARED_LIBRARY_SUFFIX}")
  set(IMPORT_LIB_PATH "${RUST_TARGET_DIR}/${CMAKE_IMPORT_LIBRARY_PREFIX}lancedb${CMAKE_IMPORT_LIBRARY_SUFFIX}")
  set(LANCEDB_LIB_PATHS "${STATIC_LIB_PATH}" "${SHARED_LIB_PATH}" "${IMPORT_LIB_PATH}")
else()
  set(SHARED_LIB_PATH "${RUST_TARGET_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}lancedb${CMAKE_SHARED_LIBRARY_SUFFIX}")
  set(LANCEDB_LIB_PATHS "${STATIC_LIB_PATH}" "${SHARED_LIB_PATH}")
endif()

option(BUILD_EXAMPLES "Build examples" OFF)
option(BUILD_DOCS "Build documentation" OFF)
option(BUILD_TESTS "Build tests" OFF)
option(BUILD_COVERAGE "Enable Rust code coverage reporting (requires llvm-tools-preview)" OFF)

if(BUILD_COVERAGE)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(WARNING "Code coverage results with an optimized build may be misleading. Consider using -DCMAKE_BUILD_TYPE=Debug")
  endif()

  # Find llvm-profdata and llvm-cov via rustc sysroot or system PATH
  execute_process(COMMAND rustc --print sysroot OUTPUT_VARIABLE RUST_SYSROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND rustc -vV OUTPUT_VARIABLE RUSTC_VERSION_INFO OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REGEX MATCH "host: ([a-zA-Z0-9_-]+)" _match "${RUSTC_VERSION_INFO}")
  set(RUST_HOST_TRIPLE "${CMAKE_MATCH_1}")
  find_program(_profdata llvm-profdata HINTS "${RUST_SYSROOT}/lib/rustlib/${RUST_HOST_TRIPLE}/bin" NO_DEFAULT_PATH)
  find_program(_cov llvm-cov HINTS "${RUST_SYSROOT}/lib/rustlib/${RUST_HOST_TRIPLE}/bin" NO_DEFAULT_PATH)
  if(_profdata AND _cov)
    list(GET _profdata 0 LLVM_PROFDATA)
    list(GET _cov 0 LLVM_COV)
  else()
    find_program(LLVM_PROFDATA llvm-profdata REQUIRED)
    find_program(LLVM_COV llvm-cov REQUIRED)
  endif()
endif()

find_package(Threads REQUIRED)
if (BUILD_EXAMPLES OR BUILD_TESTS)
  find_package(Arrow REQUIRED)
  foreach(_TARGET Arrow::arrow_shared arrow_shared Arrow::arrow_static arrow_static)
    if(TARGET ${_TARGET})
      set(ARROW_LIBRARIES ${_TARGET})
      break()
    endif()
  endforeach()
  if(NOT ARROW_LIBRARIES)
    message(FATAL_ERROR "Arrow package found but no arrow_shared or arrow_static targets exist")
  endif()
endif()

# rust-lib target
set(CARGO_BUILD_COMMAND cargo build ${RUST_BUILD_TYPE} ${RUST_TARGET_OPT} --target-dir "${CMAKE_BINARY_DIR}/target")
if(BUILD_COVERAGE)
  set(PROFRAW_DIR "${CMAKE_BINARY_DIR}/profraw")
  set(CARGO_BUILD_COMMAND ${CMAKE_COMMAND} -E env "RUSTFLAGS=-C instrument-coverage" "LLVM_PROFILE_FILE=${PROFRAW_DIR}/%p_%m.profraw" ${CARGO_BUILD_COMMAND})
endif()
add_custom_target(rust-lib ALL
  COMMAND ${CARGO_BUILD_COMMAND}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Building Rust library..."
  BYPRODUCTS ${LANCEDB_LIB_PATHS}
)

# lancedb target
add_library(lancedb SHARED IMPORTED)
set_target_properties(lancedb PROPERTIES
  IMPORTED_LOCATION "${SHARED_LIB_PATH}"
  IMPORTED_IMPLIB "${IMPORT_LIB_PATH}"
)
add_dependencies(lancedb rust-lib)


if(BUILD_EXAMPLES)
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
endif()

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

  set(TEST_TARGETS "")

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
  list(APPEND TEST_TARGETS lancedb_connection_tests)

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
  list(APPEND TEST_TARGETS lancedb_table_tests)

  # Add test executable for table metadata/version tests (runs with valgrind)
  add_executable(lancedb_table_meta_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_table_meta.cpp
  )
  target_link_libraries(lancedb_table_meta_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_table_meta_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_table_meta_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_table_meta_tests PROPERTIES
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
  list(APPEND TEST_TARGETS lancedb_index_tests)

  # Add test executable for async table tests (runs without valgrind)
  add_executable(lancedb_table_async_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_table_async.cpp
  )
  target_link_libraries(lancedb_table_async_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_table_async_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_table_async_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_table_async_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )
  list(APPEND TEST_TARGETS lancedb_table_async_tests)

  # Add test executable for coroutine table tests (runs with valgrind)
  find_package(Boost REQUIRED COMPONENTS coroutine context)
  add_executable(lancedb_table_coro_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_table_coro.cpp
  )
  target_link_libraries(lancedb_table_coro_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
    Boost::coroutine
    Boost::context
  )
  target_include_directories(lancedb_table_coro_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_table_coro_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_table_coro_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )
  list(APPEND TEST_TARGETS lancedb_table_coro_tests)

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
  list(APPEND TEST_TARGETS lancedb_vector_index_tests)

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
  list(APPEND TEST_TARGETS lancedb_query_tests)

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
  list(APPEND TEST_TARGETS lancedb_vector_query_tests)

  # Enable CTest
  enable_testing()

  # Set up coverage profraw output directory
  if(BUILD_COVERAGE)
    set(TEST_ENV_PREFIX ${CMAKE_COMMAND} -E env "LLVM_PROFILE_FILE=${PROFRAW_DIR}/%p_%m.profraw")
  else()
    set(TEST_ENV_PREFIX)
  endif()

  # Check if valgrind is available
  find_program(VALGRIND_EXECUTABLE valgrind)
  if(VALGRIND_EXECUTABLE)
    message(STATUS "Valgrind found: ${VALGRIND_EXECUTABLE}")
    # Run connection tests with valgrind
    # Only fail on definite leaks, not "possibly lost" from Rust runtime
    add_test(NAME lancedb_connection_tests
      COMMAND ${TEST_ENV_PREFIX} ${VALGRIND_EXECUTABLE}
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
      COMMAND ${TEST_ENV_PREFIX} ${VALGRIND_EXECUTABLE}
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
    # Run table metadata/version tests with valgrind
    add_test(NAME lancedb_table_meta_tests
      COMMAND ${TEST_ENV_PREFIX} ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite
        --errors-for-leak-kinds=definite
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_table_meta.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_table_meta_tests>
    )
    # Run scalar index tests with valgrind
    add_test(NAME lancedb_index_tests
      COMMAND ${TEST_ENV_PREFIX} ${VALGRIND_EXECUTABLE}
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
      COMMAND ${TEST_ENV_PREFIX} ${VALGRIND_EXECUTABLE}
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
    # Run async table tests with valgrind
    add_test(NAME lancedb_table_async_tests
      COMMAND ${TEST_ENV_PREFIX} ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite
        --errors-for-leak-kinds=definite
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_table_async.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_table_async_tests>
    )
    # Run coroutine table tests with valgrind
    add_test(NAME lancedb_table_coro_tests
      COMMAND ${TEST_ENV_PREFIX} ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite
        --errors-for-leak-kinds=definite
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_table_coro.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_table_coro_tests>
    )
  else()
    message(WARNING "Valgrind not found, running tests without memory checking")
    add_test(NAME lancedb_connection_tests COMMAND ${TEST_ENV_PREFIX} $<TARGET_FILE:lancedb_connection_tests>)
    add_test(NAME lancedb_table_tests COMMAND ${TEST_ENV_PREFIX} $<TARGET_FILE:lancedb_table_tests>)
    add_test(NAME lancedb_table_meta_tests COMMAND ${TEST_ENV_PREFIX} $<TARGET_FILE:lancedb_table_meta_tests>)
    add_test(NAME lancedb_index_tests COMMAND ${TEST_ENV_PREFIX} $<TARGET_FILE:lancedb_index_tests>)
    add_test(NAME lancedb_query_tests COMMAND ${TEST_ENV_PREFIX} $<TARGET_FILE:lancedb_query_tests>)
    add_test(NAME lancedb_table_async_tests COMMAND ${TEST_ENV_PREFIX} $<TARGET_FILE:lancedb_table_async_tests>)
    add_test(NAME lancedb_table_coro_tests COMMAND ${TEST_ENV_PREFIX} $<TARGET_FILE:lancedb_table_coro_tests>)
  endif()

  # Run vector index tests WITHOUT valgrind (too slow under valgrind)
  add_test(NAME lancedb_vector_index_tests COMMAND ${TEST_ENV_PREFIX} $<TARGET_FILE:lancedb_vector_index_tests>)
  # Run vector query tests WITHOUT valgrind (too slow under valgrind)
  add_test(NAME lancedb_vector_query_tests COMMAND ${TEST_ENV_PREFIX} $<TARGET_FILE:lancedb_vector_query_tests>)

  if(BUILD_COVERAGE)
    set(PROFDATA_FILE "${CMAKE_BINARY_DIR}/coverage.profdata")
    set(COVERAGE_REPORT_DIR "${CMAKE_BINARY_DIR}/coverage-report")

    add_custom_target(coverage
      # Run tests via ctest with profiling output directed to profraw dir
      COMMAND ${TEST_ENV_PREFIX}
        ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} -j
      # Merge profiling data
      COMMAND sh -c "${LLVM_PROFDATA} merge -sparse ${PROFRAW_DIR}/*.profraw -o ${PROFDATA_FILE}"
      # Print coverage summary
      COMMAND ${LLVM_COV} report ${SHARED_LIB_PATH}
        --instr-profile=${PROFDATA_FILE}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/
      # Generate HTML report
      COMMAND ${LLVM_COV} show ${SHARED_LIB_PATH}
        --instr-profile=${PROFDATA_FILE}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/
        --format=html
        --output-dir=${COVERAGE_REPORT_DIR}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      DEPENDS ${TEST_TARGETS}
      COMMENT "Running tests and generating Rust code coverage report..."
    )
  endif()
endif()

add_custom_target(clean-all
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target clean
  COMMAND cargo clean --target-dir "${CMAKE_BINARY_DIR}/target"
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

# Install
if(WIN32)
  install(FILES "${SHARED_LIB_PATH}" DESTINATION bin)
  install(FILES "${STATIC_LIB_PATH}" "${IMPORT_LIB_PATH}" DESTINATION lib)
else()
  install(FILES ${LANCEDB_LIB_PATHS} DESTINATION lib)
endif()
install(DIRECTORY include/ DESTINATION include)

message(STATUS "=== LanceDB C/C++ Bindings Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Rust target triple: ${RUST_TARGET_TRIPLE}")
message(STATUS "lancedb-c library paths: ${LANCEDB_LIB_PATHS}")
if (BUILD_EXAMPLES OR BUILD_TESTS)
  message(STATUS "Arrow library: ${ARROW_LIBRARIES}")
endif()
if(BUILD_EXAMPLES)
  message(STATUS "Examples: Enabled")
else()
  message(STATUS "Examples: Disabled (use -DBUILD_EXAMPLES=ON to enable)")
endif()
if(BUILD_TESTS)
  message(STATUS "Tests: Enabled")
else()
  message(STATUS "Tests: Disabled (use -DBUILD_TESTS=ON to enable)")
endif()
if(BUILD_COVERAGE)
  message(STATUS "Coverage: Enabled")
else()
  message(STATUS "Coverage: Disabled (use -DBUILD_COVERAGE=ON to enable)")
endif()
if(BUILD_DOCS)
  message(STATUS "Documentation: Enabled")
  message(STATUS "  - HTML output: ${SPHINX_BUILD}/index.html")
else()
  message(STATUS "Documentation: Disabled (use -DBUILD_DOCS=ON to enable)")
endif()
message(STATUS "============================================")
