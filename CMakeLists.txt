# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.22)

include(cmake/LanceDBVersion.cmake)
project(lancedb-c VERSION ${LANCEDB_PROJECT_VERSION})

set(LANCEDB_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include")
set(LANCEDB_GENERATED_HEADER "${LANCEDB_GENERATED_INCLUDE_DIR}/lancedb.h")
lancedb_render_header(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/lancedb.h"
  "${LANCEDB_GENERATED_HEADER}"
  "${LANCEDB_VERSION}"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/lancedb.h")

# macOS support is Apple Silicon only. Require exactly one arm64 architecture
# when CMAKE_OSX_ARCHITECTURES is explicit so a universal build cannot produce
# two Cargo artifacts.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _apple_architecture_count)
  if(NOT _apple_architecture_count EQUAL 1)
    message(FATAL_ERROR "macOS builds must target exactly one architecture, but CMAKE_OSX_ARCHITECTURES is '${CMAKE_OSX_ARCHITECTURES}'. Intel and universal output are not supported.")
  endif()
  if(NOT CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "Intel macOS (x86_64-apple-darwin) is not supported. Build for Apple Silicon (aarch64-apple-darwin) instead.")
  endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Keep the GNU-style warning and optimization flags for GNU-style drivers.
# MSVC and clang-cl already get warning, optimization and debug defaults from
# CMake; adding GNU flags makes cl.exe reject the compile line.
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wtype-limits")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
endif()

# An explicit arm64 architecture selects the Rust target even on an Intel host.
if(APPLE AND CMAKE_OSX_ARCHITECTURES AND NOT RUST_TARGET_TRIPLE)
  set(RUST_TARGET_TRIPLE "aarch64-apple-darwin")
endif()

# Keep configuration-time rustc probes in the source tree so rustup resolves
# the repository toolchain file when the build directory is out of tree.
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
      message(FATAL_ERROR "Intel macOS (x86_64-apple-darwin) is not supported. Build for Apple Silicon (aarch64-apple-darwin) instead.")
    elseif(CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "arm64" OR CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "aarch64")
      set(RUST_TARGET_TRIPLE "aarch64-apple-darwin")
    endif()
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE RUST_TARGET_TRIPLE
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

if(APPLE)
  if(RUST_TARGET_TRIPLE STREQUAL "x86_64-apple-darwin")
    message(FATAL_ERROR "Intel macOS (x86_64-apple-darwin) is not supported. Build for Apple Silicon (aarch64-apple-darwin) instead.")
  elseif(RUST_TARGET_TRIPLE AND NOT RUST_TARGET_TRIPLE STREQUAL "aarch64-apple-darwin")
    message(FATAL_ERROR "Unsupported macOS Rust target '${RUST_TARGET_TRIPLE}'. macOS support is Apple Silicon only (aarch64-apple-darwin).")
  endif()

  # With no explicit architecture, use the actual compiler target. This keeps
  # native Apple Silicon working while rejecting an Intel host or compiler.
  if(CMAKE_OSX_ARCHITECTURES)
    set(_apple_compiler_architecture "${CMAKE_OSX_ARCHITECTURES}")
  elseif(CMAKE_C_COMPILER_ARCHITECTURE_ID)
    set(_apple_compiler_architecture "${CMAKE_C_COMPILER_ARCHITECTURE_ID}")
  else()
    set(_apple_compiler_architecture "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  if(NOT _apple_compiler_architecture MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "macOS compiler architecture '${_apple_compiler_architecture}' does not match the supported Apple Silicon Rust target. Set CMAKE_OSX_ARCHITECTURES=arm64 or build on Apple Silicon.")
  endif()
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/${RUST_TARGET_DIR}/debug")
  set(RUST_BUILD_TYPE "")
else()
  set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/${RUST_TARGET_DIR}/release")
  set(RUST_BUILD_TYPE "--release")
endif()
option(BUILD_SHARED_LIBS "Build shared library (OFF for static)" ON)

set(STATIC_LIB_PATH "${RUST_TARGET_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}lancedb${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(SHARED_LIB_PATH "${RUST_TARGET_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}lancedb${CMAKE_SHARED_LIBRARY_SUFFIX}")
if(WIN32)
  set(SHARED_LIB_PATH "${RUST_TARGET_DIR}/lancedb${CMAKE_SHARED_LIBRARY_SUFFIX}")
  if(MSVC)
    # Rust names its MSVC import library after the DLL: lancedb.dll.lib.
    set(IMPORT_LIB_PATH "${RUST_TARGET_DIR}/lancedb${CMAKE_SHARED_LIBRARY_SUFFIX}.lib")
  else()
    set(IMPORT_LIB_PATH "${RUST_TARGET_DIR}/${CMAKE_IMPORT_LIBRARY_PREFIX}lancedb${CMAKE_IMPORT_LIBRARY_SUFFIX}")
  endif()
endif()

if(BUILD_SHARED_LIBS)
  set(LANCEDB_LIB_PATHS "${SHARED_LIB_PATH}")
  if(WIN32)
    list(APPEND LANCEDB_LIB_PATHS "${IMPORT_LIB_PATH}")
  endif()
else()
  set(LANCEDB_LIB_PATHS "${STATIC_LIB_PATH}")
endif()
option(BUILD_EXAMPLES "Build examples" OFF)
option(BUILD_DOCS "Build documentation" OFF)
option(BUILD_TESTS "Build tests" OFF)
option(BUILD_CONSUMER_TEST "Build the standalone package consumer smoke test" OFF)

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

include(cmake/ProbeRustNativeLibs.cmake)

configure_file(lancedb.pc.in "${CMAKE_BINARY_DIR}/lancedb.pc" @ONLY)

# rust-lib target
  set(CARGO_BUILD_COMMAND cargo build ${RUST_BUILD_TYPE} ${RUST_TARGET_OPT} --package lancedb-c --target-dir "${CMAKE_BINARY_DIR}/target")
# The resolved version changes the embedded runtime string. Pass it as a
# private build variable so a cached Cargo build rebuilds when it changes.
set(CARGO_BUILD_COMMAND ${CMAKE_COMMAND} -E env "LANCEDB_BUILD_VERSION=${LANCEDB_VERSION}" ${CARGO_BUILD_COMMAND})
add_custom_target(rust-lib ALL
  COMMAND ${CARGO_BUILD_COMMAND}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Building Rust library..."
  BYPRODUCTS ${LANCEDB_LIB_PATHS}
)

# lancedb target
if(BUILD_SHARED_LIBS)
  add_library(lancedb SHARED IMPORTED)
  set_target_properties(lancedb PROPERTIES
    IMPORTED_LOCATION "${SHARED_LIB_PATH}"
    IMPORTED_IMPLIB "${IMPORT_LIB_PATH}"
  )
else()
  add_library(lancedb STATIC IMPORTED)
  set_target_properties(lancedb PROPERTIES
    IMPORTED_LOCATION "${STATIC_LIB_PATH}"
  )
  set_property(TARGET lancedb PROPERTY
    INTERFACE_LINK_LIBRARIES ${LANCEDB_NATIVE_STATIC_LIBS})
endif()
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
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
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
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
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
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
  )
  target_compile_options(s3 PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(s3 PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # external_provider helper crate (provides external_object_store_provider_create)
  set(EXT_PROVIDER_CARGO_CMD cargo build ${RUST_BUILD_TYPE} ${RUST_TARGET_OPT} --package lancedb-external-provider --target-dir "${CMAKE_BINARY_DIR}/target")
  if(WIN32)
    set(EXT_PROVIDER_SHARED_PATH "${RUST_TARGET_DIR}/lancedb_external_provider${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(EXT_PROVIDER_IMPORT_PATH "${RUST_TARGET_DIR}/${CMAKE_IMPORT_LIBRARY_PREFIX}lancedb_external_provider${CMAKE_IMPORT_LIBRARY_SUFFIX}")
  else()
    set(EXT_PROVIDER_SHARED_PATH "${RUST_TARGET_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}lancedb_external_provider${CMAKE_SHARED_LIBRARY_SUFFIX}")
  endif()
  if(WIN32)
    set(EXT_PROVIDER_LIB_PATHS "${EXT_PROVIDER_SHARED_PATH}" "${EXT_PROVIDER_IMPORT_PATH}")
  else()
    set(EXT_PROVIDER_LIB_PATHS "${EXT_PROVIDER_SHARED_PATH}")
  endif()
  add_custom_target(external-provider-lib
    COMMAND ${EXT_PROVIDER_CARGO_CMD}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building external provider Rust crate..."
    BYPRODUCTS ${EXT_PROVIDER_LIB_PATHS}
  )
  add_library(lancedb_external_provider SHARED IMPORTED)
  set_target_properties(lancedb_external_provider PROPERTIES
    IMPORTED_LOCATION "${EXT_PROVIDER_SHARED_PATH}"
    IMPORTED_IMPLIB "${EXT_PROVIDER_IMPORT_PATH}"
  )
  add_dependencies(external-provider-lib rust-lib)
  add_dependencies(lancedb_external_provider external-provider-lib)

  # lancedb registry example
  add_executable(lancedb_registry examples/lancedb_registry.cpp)
  target_link_libraries(lancedb_registry
    lancedb
    lancedb_external_provider
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_registry
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
  )
  target_compile_options(lancedb_registry PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_registry PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # asio spawn coroutine example
  add_executable(asio_coroutine examples/asio_coroutine.cpp)
  find_package(Boost REQUIRED COMPONENTS coroutine context)
  target_link_libraries(asio_coroutine
    lancedb
    Threads::Threads
    ${ARROW_LIBRARIES}
    Boost::coroutine
    Boost::context
  )
  target_include_directories(asio_coroutine
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
  )
  target_compile_options(asio_coroutine PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(asio_coroutine PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  add_custom_target(examples DEPENDS simple full s3 lancedb_registry asio_coroutine)
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
  set(DOXYGEN_INPUT_DIR ${LANCEDB_GENERATED_INCLUDE_DIR}/lancedb.h)
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

  # Sphinx normally resolves the version itself. CMake passes its
  # configure-time result through a private variable so a later build-time
  # LANCEDB_VERSION cannot change the docs, while standalone Sphinx keeps the
  # strict public override path.
  add_custom_target(docs
    COMMAND ${CMAKE_COMMAND} -E env DOXYGEN_XML_DIR=${DOXYGEN_OUTPUT_DIR}
      LANCEDB_RESOLVED_VERSION=${LANCEDB_VERSION}
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
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
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
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_table_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_table_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

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
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
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
    tests/test_c_enum_inputs.c
    tests/test_index.cpp
    tests/test_index_info.cpp
    tests/test_index_stats.cpp
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
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
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
    tests/test_vector_index_stats.cpp
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
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
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
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_query_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_query_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Add test executable for DataFusion expression query tests
  add_executable(lancedb_df_query_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_c_enum_inputs.c
    tests/test_df_query.cpp
  )
  target_link_libraries(lancedb_df_query_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_df_query_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_df_query_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_df_query_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Add test executable for JSON expression tests
  add_executable(lancedb_json_tests
    tests/test_main.cpp
    tests/test_json.cpp
  )
  target_link_libraries(lancedb_json_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_json_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_json_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_json_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Add test executable for vector query tests (runs without valgrind)
  add_executable(lancedb_vector_query_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_c_enum_inputs.c
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
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_vector_query_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_vector_query_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Add test executable for full-text and hybrid query tests (runs without valgrind)
  add_executable(lancedb_fts_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_fts.cpp
  )
  target_link_libraries(lancedb_fts_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_fts_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_fts_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_fts_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Arrow C Stream import and ownership tests run under valgrind like the other
  # lifecycle-sensitive suites.
  add_executable(lancedb_arrow_stream_tests
    tests/test_main.cpp
    tests/test_common.cpp
    tests/test_arrow_stream.cpp
  )
  target_link_libraries(lancedb_arrow_stream_tests
    PRIVATE
    lancedb
    Catch2::Catch2
    Threads::Threads
    ${ARROW_LIBRARIES}
  )
  target_include_directories(lancedb_arrow_stream_tests
    PRIVATE ${ARROW_INCLUDE_DIRS}
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(lancedb_arrow_stream_tests PRIVATE ${ARROW_CFLAGS_OTHER})
  set_target_properties(lancedb_arrow_stream_tests PROPERTIES
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # C_STANDARD is a target property. Apply strict C17 only to the targets that
  # compile the C consumer helper; each has no other C sources, so the C++
  # sources and all other targets keep their existing flags.
  foreach(target lancedb_index_tests lancedb_df_query_tests lancedb_vector_query_tests)
    set_target_properties(${target} PROPERTIES
      C_STANDARD 17
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF
    )
  endforeach()

  # Enable CTest
  enable_testing()

  # Check if valgrind is available
  find_program(VALGRIND_EXECUTABLE valgrind)
  if(VALGRIND_EXECUTABLE)
    message(STATUS "Valgrind found: ${VALGRIND_EXECUTABLE}")
    # Run connection tests with valgrind
    # Fail on definite and possible leaks; known false positives are in valgrind.supp
    add_test(NAME lancedb_connection_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite,possible
        --errors-for-leak-kinds=definite,possible
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
        --show-leak-kinds=definite,possible
        --errors-for-leak-kinds=definite,possible
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_table.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_table_tests>
    )
    # Run table metadata/version tests with valgrind
    add_test(NAME lancedb_table_meta_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite,possible
        --errors-for-leak-kinds=definite,possible
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_table_meta.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_table_meta_tests>
    )
    # Run scalar index tests with valgrind
    add_test(NAME lancedb_index_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite,possible
        --errors-for-leak-kinds=definite,possible
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
        --show-leak-kinds=definite,possible
        --errors-for-leak-kinds=definite,possible
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_query.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_query_tests>
    )
    # Run DataFusion expression query tests with valgrind
    add_test(NAME lancedb_df_query_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite,possible
        --errors-for-leak-kinds=definite,possible
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_df_query.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_df_query_tests>
    )
    # Run JSON expression tests with valgrind
    add_test(NAME lancedb_json_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite,possible
        --errors-for-leak-kinds=definite,possible
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_json.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_json_tests>
    )
    # Run Arrow C Stream import and ownership tests with valgrind
    add_test(NAME lancedb_arrow_stream_tests
      COMMAND ${VALGRIND_EXECUTABLE}
        --tool=memcheck
        --leak-check=full
        --show-leak-kinds=definite,possible
        --errors-for-leak-kinds=definite,possible
        --track-origins=yes
        --error-exitcode=1
        --log-file=${CMAKE_BINARY_DIR}/valgrind_arrow_stream.txt
        --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/valgrind.supp
        $<TARGET_FILE:lancedb_arrow_stream_tests>
    )
  else()
    message(WARNING "Valgrind not found, running tests without memory checking")
    add_test(NAME lancedb_connection_tests COMMAND $<TARGET_FILE:lancedb_connection_tests>)
    add_test(NAME lancedb_table_tests COMMAND $<TARGET_FILE:lancedb_table_tests>)
    add_test(NAME lancedb_table_meta_tests COMMAND $<TARGET_FILE:lancedb_table_meta_tests>)
    add_test(NAME lancedb_index_tests COMMAND $<TARGET_FILE:lancedb_index_tests>)
    add_test(NAME lancedb_query_tests COMMAND $<TARGET_FILE:lancedb_query_tests>)
    add_test(NAME lancedb_df_query_tests COMMAND $<TARGET_FILE:lancedb_df_query_tests>)
    add_test(NAME lancedb_json_tests COMMAND $<TARGET_FILE:lancedb_json_tests>)
    add_test(NAME lancedb_arrow_stream_tests COMMAND $<TARGET_FILE:lancedb_arrow_stream_tests>)
  endif()

  # Run vector index tests WITHOUT valgrind (too slow under valgrind)
  add_test(NAME lancedb_vector_index_tests COMMAND $<TARGET_FILE:lancedb_vector_index_tests>)
  # Run vector query tests WITHOUT valgrind (too slow under valgrind)
  add_test(NAME lancedb_vector_query_tests COMMAND $<TARGET_FILE:lancedb_vector_query_tests>)
  # Run full-text and hybrid query tests WITHOUT valgrind (index and query work is slow under valgrind)
  add_test(NAME lancedb_fts_tests COMMAND $<TARGET_FILE:lancedb_fts_tests>)
endif()

# Compile and run the public header with a strict C17 consumer. The source has
# no C++ dependencies, so this catches C-incompatible header changes and C ABI
# failures that the C++ tests cannot see. BUILD_CONSUMER_TEST lets platform CI
# exercise this check without enabling the Catch2 C++ test suite.
if(BUILD_TESTS OR BUILD_CONSUMER_TEST)
  enable_testing()

  add_executable(lancedb_c17_consumer tests/c17_consumer.c)
  target_link_libraries(lancedb_c17_consumer
    PRIVATE
    lancedb
    Threads::Threads
  )
  target_include_directories(lancedb_c17_consumer
    PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR}
  )
  target_compile_options(lancedb_c17_consumer PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Werror;-pedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4;/WX>
  )
  set_target_properties(lancedb_c17_consumer PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    BUILD_RPATH ${RUST_TARGET_DIR}
  )

  # Compile the same strict C17 consumer against the checked-in header. The
  # source header must stay usable directly, without a CMake or packaging step.
  add_library(lancedb_source_header_c17 OBJECT tests/c17_consumer.c)
  target_include_directories(lancedb_source_header_c17
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_compile_options(lancedb_source_header_c17 PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Werror;-pedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4;/WX>
  )
  set_target_properties(lancedb_source_header_c17 PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
  )

  set(_c17_ctest_config_args)
  if(CMAKE_CONFIGURATION_TYPES)
    set(_c17_ctest_config_args -C $<CONFIG>)
  endif()

  add_custom_target(check-c17
    COMMAND ${CMAKE_CTEST_COMMAND}
      --test-dir ${CMAKE_CURRENT_BINARY_DIR}
      ${_c17_ctest_config_args}
      -R "^lancedb_c17_consumer$"
      --output-on-failure
      --no-tests=error
    DEPENDS lancedb_c17_consumer lancedb_source_header_c17
    COMMENT "Checking the public header with a strict C17 consumer..."
    VERBATIM
  )

  add_test(NAME lancedb_c17_consumer
    COMMAND $<TARGET_FILE:lancedb_c17_consumer>
      ${CMAKE_CURRENT_BINARY_DIR}/c17-consumer-db
  )

  if(BUILD_CONSUMER_TEST)
    add_executable(package_consumer tests/package_consumer.cpp)
    target_link_libraries(package_consumer PRIVATE lancedb Threads::Threads)
    target_include_directories(package_consumer PRIVATE ${LANCEDB_GENERATED_INCLUDE_DIR})
    set_target_properties(package_consumer PROPERTIES BUILD_RPATH ${RUST_TARGET_DIR})
    add_test(NAME package_consumer
      COMMAND package_consumer "${CMAKE_BINARY_DIR}/package-consumer-db")
  endif()

  if(WIN32 AND BUILD_SHARED_LIBS)
    # The DLL stays in the Rust target directory; put it on the test PATH
    # instead of adding a second DLL staging step.
    set(_consumer_tests lancedb_c17_consumer)
    if(BUILD_CONSUMER_TEST)
      list(APPEND _consumer_tests package_consumer)
    endif()
    set_tests_properties(${_consumer_tests} PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${RUST_TARGET_DIR}"
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
  COMMAND cargo clippy --all-targets -- -D warnings
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Running Rust formatting and linting checks..."
)

# The public header is hand-written. Generate declarations from the Rust FFI
# with the pinned cbindgen and compare them with clang's view of both headers.
find_program(PYTHON3_EXECUTABLE NAMES python3 python)
add_custom_target(check-abi
  COMMAND ${PYTHON3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/.github/scripts/check_abi.py
    --repo ${CMAKE_CURRENT_SOURCE_DIR}
    --self-test
  COMMAND ${PYTHON3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/.github/scripts/check_abi.py
    --repo ${CMAKE_CURRENT_SOURCE_DIR}
    --target x86_64-pc-windows-msvc
    --self-test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Checking public C declarations against the Rust FFI..."
  VERBATIM
)

# Install
if(BUILD_SHARED_LIBS)
  if(WIN32)
    install(FILES "${SHARED_LIB_PATH}" DESTINATION bin)
    install(FILES "${IMPORT_LIB_PATH}" DESTINATION lib)
  else()
    install(FILES "${SHARED_LIB_PATH}" DESTINATION lib)
  endif()
else()
  install(FILES "${STATIC_LIB_PATH}" DESTINATION lib)
endif()
install(FILES "${LANCEDB_GENERATED_HEADER}" DESTINATION include)
install(FILES "${CMAKE_BINARY_DIR}/lancedb.pc" DESTINATION lib/pkgconfig)

message(STATUS "=== LanceDB C/C++ Bindings Configuration ===")
message(STATUS "Version: ${LANCEDB_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
if(BUILD_SHARED_LIBS)
  message(STATUS "Library type: Shared")
else()
  message(STATUS "Library type: Static")
endif()
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
if(BUILD_CONSUMER_TEST)
  message(STATUS "Consumer test: Enabled")
else()
  message(STATUS "Consumer test: Disabled (use -DBUILD_CONSUMER_TEST=ON to enable)")
endif()
if(BUILD_DOCS)
  message(STATUS "Documentation: Enabled")
  message(STATUS "  - HTML output: ${SPHINX_BUILD}/index.html")
else()
  message(STATUS "Documentation: Disabled (use -DBUILD_DOCS=ON to enable)")
endif()
message(STATUS "============================================")
