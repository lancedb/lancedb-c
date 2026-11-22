// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Binding version exposed at runtime.

use std::os::raw::c_char;

// The NUL keeps the static string valid for C callers without allocating.
static VERSION: &str = concat!(env!("LANCEDB_BUILD_VERSION"), "\0");

/// Return the binding version embedded in this library.
///
/// The pointer refers to static storage owned by the library and must not be
/// freed by the caller.
#[no_mangle]
pub extern "C" fn lancedb_version() -> *const c_char {
    VERSION.as_ptr().cast()
}
