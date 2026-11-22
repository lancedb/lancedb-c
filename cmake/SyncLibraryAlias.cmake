# SPDX-License-Identifier: Apache-2.0

# Keep the build-tree linker name resolvable through the SONAME recorded in the
# library. The version changes with the tag-derived binding version, so replace
# every stale alias in this build directory before creating the current one.

if(NOT DEFINED BUILD_DIR OR NOT DEFINED LIBRARY_NAME OR NOT DEFINED ALIAS_NAME)
  message(FATAL_ERROR "BUILD_DIR, LIBRARY_NAME and ALIAS_NAME are required")
endif()

get_filename_component(_library_dir "${BUILD_DIR}" ABSOLUTE)
set(_alias_path "${_library_dir}/${ALIAS_NAME}")

file(GLOB _stale_aliases "${_library_dir}/${LIBRARY_NAME}.*")
foreach(_stale_alias IN LISTS _stale_aliases)
  file(REMOVE "${_stale_alias}")
endforeach()

file(REMOVE "${_alias_path}")
file(CREATE_LINK "${LIBRARY_NAME}" "${_alias_path}" SYMBOLIC)
