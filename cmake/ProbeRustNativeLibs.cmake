# Probe rustc for the native system libraries required when statically
# linking a Rust staticlib.
#
# Input:  RUST_TARGET_OPT  (optional, e.g. "--target x86_64-unknown-linux-gnu")
# Output: LANCEDB_NATIVE_STATIC_LIBS        (CMake list)
#         LANCEDB_NATIVE_STATIC_LIBS_STRING  (space-separated, for pkg-config)

if(NOT BUILD_SHARED_LIBS)
  set(_probe_src "${CMAKE_BINARY_DIR}/_probe_native_libs.rs")
  set(_probe_out "${CMAKE_BINARY_DIR}/_probe_native_libs${CMAKE_STATIC_LIBRARY_SUFFIX}")
  file(WRITE "${_probe_src}" "")
  execute_process(
    COMMAND rustc --print native-static-libs --crate-type staticlib
            "${_probe_src}" -o "${_probe_out}" ${RUST_TARGET_OPT}
    # Match Cargo's working directory so rustup selects the repository toolchain.
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ERROR_VARIABLE _rustc_stderr
    RESULT_VARIABLE _rustc_result
    OUTPUT_QUIET
  )
  file(REMOVE "${_probe_src}" "${_probe_out}")

  if(_rustc_result EQUAL 0 AND _rustc_stderr MATCHES "native-static-libs: ([^\n]+)")
    set(_raw_libs "${CMAKE_MATCH_1}")
    separate_arguments(_lib_list NATIVE_COMMAND "${_raw_libs}")
    set(LANCEDB_NATIVE_STATIC_LIBS "")
    set(_pending_framework FALSE)
    foreach(_lib IN LISTS _lib_list)
      if(_pending_framework)
        list(APPEND LANCEDB_NATIVE_STATIC_LIBS "-framework ${_lib}")
        set(_pending_framework FALSE)
      elseif(_lib STREQUAL "-framework")
        set(_pending_framework TRUE)
      else()
        list(APPEND LANCEDB_NATIVE_STATIC_LIBS "${_lib}")
      endif()
    endforeach()
    list(JOIN LANCEDB_NATIVE_STATIC_LIBS " " LANCEDB_NATIVE_STATIC_LIBS_STRING)
    message(STATUS "Detected native static libraries: ${LANCEDB_NATIVE_STATIC_LIBS_STRING}")
  else()
    message(FATAL_ERROR "Failed to detect native static libraries for static linking. "
      "Ensure rustc is available and supports --print native-static-libs.")
  endif()
else()
  set(LANCEDB_NATIVE_STATIC_LIBS_STRING "")
endif()
