//! C entry points that exist only to exercise runtime failure paths from the
//! C++ test suite. Compiled only with the `test-hooks` cargo feature, which
//! the CMake test build enables; never part of the shipped library.

use std::os::raw::c_char;

use crate::error::{handle_error, LanceDBError};
use crate::runtime::run_blocking;

/// Run a task that panics; the panic must surface as `LANCEDB_RUNTIME`
/// with a message, never as a process abort.
///
/// # Safety
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_test_panic_in_task(
    error_message: *mut *mut c_char,
) -> LanceDBError {
    match run_blocking(async {
        panic!("test hook: deliberate panic inside a runtime task");
        #[allow(unreachable_code)]
        Ok(())
    }) {
        Ok(()) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Call the blocking bridge from inside a runtime task, i.e. from a tokio
/// worker thread — the misuse the API forbids. It must be reported as
/// `LANCEDB_RUNTIME` rather than aborting the process.
///
/// # Safety
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_test_call_from_runtime(
    error_message: *mut *mut c_char,
) -> LanceDBError {
    match run_blocking(async {
        // nested use of the bridge on a worker thread
        run_blocking(async { Ok(()) })
    }) {
        Ok(()) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}
