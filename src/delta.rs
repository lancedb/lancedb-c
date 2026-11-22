// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Table delta FFI for LanceDB C bindings
//!
//! A delta pins two persisted versions on the table's current branch and
//! streams the rows that changed between them. The endpoint snapshot and the
//! stable-row-id checks live in the SDK; this module only converts handles,
//! endpoint references, streams and errors.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::ptr;

use lancedb::table::{DeltaRef, TableDelta};

use crate::connection::{get_runtime, LanceDBTable};
use crate::error::{clear_error_message, handle_error, set_invalid_argument_message, LanceDBError};
use crate::query::{query_result_from_stream, LanceDBQueryResult};

/// Opaque handle to a TableDelta
#[repr(C)]
pub struct LanceDBTableDelta {
    inner: TableDelta,
}

/// Kind of endpoint a table delta compares
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceDBDeltaRefType {
    Version = 0,
    Tag = 1,
}

/// One endpoint of a table delta
///
/// `kind` is a raw C `LanceDBDeltaRefType` value and is validated before use.
/// A version endpoint reads `version`; a tag endpoint reads `tag` as a
/// NUL-terminated string and resolves it on the table's current branch.
#[repr(C)]
pub struct LanceDBDeltaRef {
    pub kind: c_int,
    pub version: u64,
    pub tag: *const c_char,
}

/// Convert a C endpoint reference into an SDK endpoint
unsafe fn delta_ref_from_c(reference: &LanceDBDeltaRef) -> Option<DeltaRef> {
    match reference.kind {
        value if value == LanceDBDeltaRefType::Version as c_int => {
            Some(DeltaRef::Version(reference.version))
        }
        value if value == LanceDBDeltaRefType::Tag as c_int => {
            if reference.tag.is_null() {
                return None;
            }
            let tag = CStr::from_ptr(reference.tag).to_str().ok()?;
            Some(DeltaRef::Tag(tag.to_string()))
        }
        _ => None,
    }
}

/// The row-level reader selected on a delta
#[derive(Clone, Copy)]
enum DeltaRows {
    Inserted,
    Updated,
    Upserted,
    Deleted,
}

/// Run one SDK reader and wrap the returned stream in the existing result handle
unsafe fn read_delta_rows(
    delta: *const LanceDBTableDelta,
    rows: DeltaRows,
    result_out: *mut *mut LanceDBQueryResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !result_out.is_null() {
        *result_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if delta.is_null() || result_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let delta = &(*delta).inner;
    let result = get_runtime().block_on(async {
        match rows {
            DeltaRows::Inserted => delta.inserted_rows().await,
            DeltaRows::Updated => delta.updated_rows().await,
            DeltaRows::Upserted => delta.upserted_rows().await,
            DeltaRows::Deleted => delta.deleted_row_ids().await,
        }
    });

    match result {
        Ok(stream) => {
            *result_out = query_result_from_stream(stream.into());
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Create a delta between two endpoints on the table's current branch
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `begin` must be a valid pointer to a `LanceDBDeltaRef`
/// - `end` can be NULL for the table's visible snapshot, or a valid pointer to
///   a `LanceDBDeltaRef`
/// - `delta_out` must be a valid pointer to receive the delta handle
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*delta_out` holding the delta, or the SDK error
///   code when an endpoint cannot be resolved or the table is not embedded
///
/// Both endpoints are resolved when the delta is created, so later tag moves,
/// checkouts or table commits do not change it. `begin` is exclusive and
/// required; `end` is inclusive and defaults to the visible snapshot. Tags are
/// resolved on the current branch. Free the handle with
/// `lancedb_table_delta_free()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delta(
    table: *const LanceDBTable,
    begin: *const LanceDBDeltaRef,
    end: *const LanceDBDeltaRef,
    delta_out: *mut *mut LanceDBTableDelta,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !delta_out.is_null() {
        *delta_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if table.is_null() || begin.is_null() || delta_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(begin) = delta_ref_from_c(&*begin) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };
    let end = if end.is_null() {
        None
    } else {
        let Some(end) = delta_ref_from_c(&*end) else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };
        Some(end)
    };

    let table = &(*table).inner;
    match get_runtime().block_on(table.delta(begin, end)) {
        Ok(delta) => {
            *delta_out = Box::into_raw(Box::new(LanceDBTableDelta { inner: delta }));
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Get the resolved begin version of a delta
///
/// # Safety
/// - `delta` must be NULL or a valid pointer returned by `lancedb_table_delta`
///
/// # Returns
/// - The first version in the delta's exclusive begin, or 0 for a NULL handle
///
/// A persisted version is never 0, so 0 is a safe failure sentinel.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delta_begin_version(delta: *const LanceDBTableDelta) -> u64 {
    if delta.is_null() {
        return 0;
    }
    (*delta).inner.begin_version()
}

/// Get the resolved end version of a delta
///
/// # Safety
/// - `delta` must be NULL or a valid pointer returned by `lancedb_table_delta`
///
/// # Returns
/// - The delta's inclusive end version, or 0 for a NULL handle
///
/// A persisted version is never 0, so 0 is a safe failure sentinel.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delta_end_version(delta: *const LanceDBTableDelta) -> u64 {
    if delta.is_null() {
        return 0;
    }
    (*delta).inner.end_version()
}

/// Stream rows inserted between the delta endpoints
///
/// # Safety
/// - `delta` must be a valid pointer returned by `lancedb_table_delta`
/// - `result_out` must be a valid pointer to receive the streaming result
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*result_out` holding the result, or the SDK error
///   code that rejected the reader
///
/// Each batch carries the table columns plus `_rowid`,
/// `_row_created_at_version` and `_row_last_updated_at_version`, read as of the
/// end version. On entry, `*result_out` and the error message are cleared. The
/// returned result is one owning handle, independent of the delta and the
/// source table. Dispose of it exactly once: call
/// `lancedb_query_result_free()` without consuming it, or pass it to exactly
/// one conversion. `lancedb_query_result_to_ipc_file()` consumes a non-NULL
/// result on success or failure. `lancedb_query_result_to_arrow()` consumes the
/// result when all required out-pointers are non-NULL, on success or failure;
/// with a NULL required out-pointer it returns InvalidArgument and leaves the
/// result with the caller. Do not free a result after a conversion consumed it;
/// the conversion docs name the frees for returned arrays/schema and IPC bytes.
///
/// Equal endpoints return a successful empty stream that keeps this reader's
/// schema without requiring stable row ids. Use
/// `lancedb_query_result_to_ipc_file()` to read that schema; the Arrow C Data
/// conversion reports an empty result as NULL arrays, NULL schema and a zero
/// count.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delta_inserted_rows(
    delta: *const LanceDBTableDelta,
    result_out: *mut *mut LanceDBQueryResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    read_delta_rows(delta, DeltaRows::Inserted, result_out, error_message)
}

/// Stream rows updated between the delta endpoints
///
/// # Safety
/// - `delta` must be a valid pointer returned by `lancedb_table_delta`
/// - `result_out` must be a valid pointer to receive the streaming result
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*result_out` holding the result, or the SDK error
///   code that rejected the reader
///
/// Newly inserted rows are excluded; see `lancedb_table_delta_upserted_rows()`
/// for both. Each batch carries the table columns plus `_rowid`,
/// `_row_created_at_version` and `_row_last_updated_at_version`, read as of the
/// end version. On entry, `*result_out` and the error message are cleared. The
/// returned result is one owning handle, independent of the delta and the
/// source table. Dispose of it exactly once: call
/// `lancedb_query_result_free()` without consuming it, or pass it to exactly
/// one conversion. `lancedb_query_result_to_ipc_file()` consumes a non-NULL
/// result on success or failure. `lancedb_query_result_to_arrow()` consumes the
/// result when all required out-pointers are non-NULL, on success or failure;
/// with a NULL required out-pointer it returns InvalidArgument and leaves the
/// result with the caller. Do not free a result after a conversion consumed it;
/// the conversion docs name the frees for returned arrays/schema and IPC bytes.
///
/// Equal endpoints return a successful empty stream that keeps this reader's
/// schema without requiring stable row ids. Use
/// `lancedb_query_result_to_ipc_file()` to read that schema; the Arrow C Data
/// conversion reports an empty result as NULL arrays, NULL schema and a zero
/// count.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delta_updated_rows(
    delta: *const LanceDBTableDelta,
    result_out: *mut *mut LanceDBQueryResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    read_delta_rows(delta, DeltaRows::Updated, result_out, error_message)
}

/// Stream inserted and updated rows between the delta endpoints
///
/// # Safety
/// - `delta` must be a valid pointer returned by `lancedb_table_delta`
/// - `result_out` must be a valid pointer to receive the streaming result
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*result_out` holding the result, or the SDK error
///   code that rejected the reader
///
/// Each batch carries the table columns plus `_rowid`,
/// `_row_created_at_version` and `_row_last_updated_at_version`, read as of the
/// end version. On entry, `*result_out` and the error message are cleared. The
/// returned result is one owning handle, independent of the delta and the
/// source table. Dispose of it exactly once: call
/// `lancedb_query_result_free()` without consuming it, or pass it to exactly
/// one conversion. `lancedb_query_result_to_ipc_file()` consumes a non-NULL
/// result on success or failure. `lancedb_query_result_to_arrow()` consumes the
/// result when all required out-pointers are non-NULL, on success or failure;
/// with a NULL required out-pointer it returns InvalidArgument and leaves the
/// result with the caller. Do not free a result after a conversion consumed it;
/// the conversion docs name the frees for returned arrays/schema and IPC bytes.
///
/// Equal endpoints return a successful empty stream that keeps this reader's
/// schema without requiring stable row ids. Use
/// `lancedb_query_result_to_ipc_file()` to read that schema; the Arrow C Data
/// conversion reports an empty result as NULL arrays, NULL schema and a zero
/// count.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delta_upserted_rows(
    delta: *const LanceDBTableDelta,
    result_out: *mut *mut LanceDBQueryResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    read_delta_rows(delta, DeltaRows::Upserted, result_out, error_message)
}

/// Stream the ids of rows deleted between the delta endpoints
///
/// # Safety
/// - `delta` must be a valid pointer returned by `lancedb_table_delta`
/// - `result_out` must be a valid pointer to receive the streaming result
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*result_out` holding the result, or the SDK error
///   code that rejected the reader
///
/// Each batch carries one `_rowid` column of type `UInt64`. Deleted rows are
/// not reconstructed from earlier versions. On entry, `*result_out` and the
/// error message are cleared. The returned result is one owning handle,
/// independent of the delta and the source table. Dispose of it exactly once:
/// call `lancedb_query_result_free()` without consuming it, or pass it to
/// exactly one conversion. `lancedb_query_result_to_ipc_file()` consumes a
/// non-NULL result on success or failure. `lancedb_query_result_to_arrow()`
/// consumes the result when all required out-pointers are non-NULL, on success
/// or failure; with a NULL required out-pointer it returns InvalidArgument and
/// leaves the result with the caller. Do not free a result after a conversion
/// consumed it; the conversion docs name the frees for returned arrays/schema
/// and IPC bytes.
///
/// Equal endpoints return a successful empty stream that keeps this reader's
/// schema without requiring stable row ids. Use
/// `lancedb_query_result_to_ipc_file()` to read that schema; the Arrow C Data
/// conversion reports an empty result as NULL arrays, NULL schema and a zero
/// count.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delta_deleted_row_ids(
    delta: *const LanceDBTableDelta,
    result_out: *mut *mut LanceDBQueryResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    read_delta_rows(delta, DeltaRows::Deleted, result_out, error_message)
}

/// Free a TableDelta
///
/// # Safety
/// - `delta` must be NULL or a valid pointer returned by `lancedb_table_delta`
/// - `delta` must not be used after calling this function
///
/// Results already returned by a reader stay valid after the delta is freed.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delta_free(delta: *mut LanceDBTableDelta) {
    if !delta.is_null() {
        drop(Box::from_raw(delta));
    }
}
