// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! TakeQuery API for the LanceDB C bindings
//!
//! A take query selects rows by dataset offset or row ID. The SDK restores the
//! requested occurrences before the result is handed to the caller, so the FFI
//! layer only copies the input and reuses the ordinary query result path.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::ptr;

use lancedb::arrow::SendableRecordBatchStream;
use lancedb::query::{ExecutableQuery, Select, TakeQuery};

use crate::connection::{get_runtime, LanceDBTable};
use crate::error::{
    clear_error_message, handle_error, set_custom_error_message, set_invalid_argument_message,
    LanceDBError,
};
use crate::query::{query_result_from_stream, LanceDBQueryResult};

/// Opaque handle to a LanceDB TakeQuery
#[repr(C)]
pub struct LanceDBTakeQuery {
    /// The SDK builder, taken out while a setter consumes and returns it. A
    /// non-NULL handle always holds `Some` until execution consumes the handle.
    inner: Option<TakeQuery>,
    /// Whether the rows are selected by dataset offset rather than by row ID.
    /// This distinguishes the two SDK ordering rules without inspecting the
    /// SDK request.
    by_offsets: bool,
    /// Preserve the exact requested offset order. Only offset takes use this
    /// flag; row-ID takes already follow the requested order in the SDK.
    preserve_order: bool,
}

/// Read a C array of borrowed column names.
///
/// A zero count selects no columns and may pass a NULL array. Any other count
/// requires an array whose entries are valid UTF-8, NUL-terminated strings.
unsafe fn columns_from_c(
    columns: *const *const c_char,
    num_columns: usize,
) -> Result<Vec<String>, ()> {
    if num_columns == 0 {
        return Ok(Vec::new());
    }
    if columns.is_null() {
        return Err(());
    }

    let mut names = Vec::with_capacity(num_columns);
    for index in 0..num_columns {
        let name = *columns.add(index);
        if name.is_null() {
            return Err(());
        }
        names.push(CStr::from_ptr(name).to_str().map_err(|_| ())?.to_string());
    }
    Ok(names)
}

/// Consume and rebuild the SDK builder held by a C handle.
///
/// Returns `None` when the handle no longer holds a builder, which can only
/// happen if a caller reuses a handle that execution already consumed.
unsafe fn update_take_query(
    query: *mut LanceDBTakeQuery,
    update: impl FnOnce(TakeQuery) -> TakeQuery,
) -> Option<()> {
    let inner = (*query).inner.take()?;
    (*query).inner = Some(update(inner));
    Some(())
}

unsafe fn take_offsets_or_row_ids(
    table: *const LanceDBTable,
    values: *const u64,
    count: usize,
    query_out: *mut *mut LanceDBTakeQuery,
    by_offsets: bool,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !query_out.is_null() {
        *query_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if table.is_null() || query_out.is_null() || (count > 0 && values.is_null()) {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    // The caller keeps ownership of `values`. Copy it before returning so that
    // a later change to the caller's array cannot affect the builder.
    let values = if count == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(values, count).to_vec()
    };
    let inner = if by_offsets {
        (*table).inner.take_offsets(values)
    } else {
        (*table).inner.take_row_ids(values)
    };

    *query_out = Box::into_raw(Box::new(LanceDBTakeQuery {
        inner: Some(inner),
        by_offsets,
        preserve_order: false,
    }));
    LanceDBError::Success
}

/// Create a take query that selects rows by dataset offset
///
/// Offsets are 0-indexed and relative to the current table version; they can
/// change when rows are added or deleted. The SDK copies the requested offsets,
/// including duplicates. Without `lancedb_take_query_preserve_order()` the
/// result order is unspecified; missing offsets are omitted. The caller keeps
/// ownership of `offsets` and may release or change it once this call returns.
///
/// # Safety
/// - `table` must be a valid pointer returned from a table open or create call
/// - `offsets` must point to `count` readable `uint64_t` values, or may be NULL
///   when `count` is zero
/// - `query_out` must be a valid pointer to receive the builder
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_take_offsets(
    table: *const LanceDBTable,
    offsets: *const u64,
    count: usize,
    query_out: *mut *mut LanceDBTakeQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    take_offsets_or_row_ids(table, offsets, count, query_out, true, error_message)
}

/// Create a take query that selects rows by row ID
///
/// Row IDs are relative to the current table version and can change after
/// compaction or updates. The SDK follows the requested order, including
/// repeated IDs, and omits missing or deleted IDs. The caller keeps ownership
/// of `row_ids` and may release or change it once this call returns.
///
/// # Safety
/// - `table` must be a valid pointer returned from a table open or create call
/// - `row_ids` must point to `count` readable `uint64_t` values, or may be NULL
///   when `count` is zero
/// - `query_out` must be a valid pointer to receive the builder
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_take_row_ids(
    table: *const LanceDBTable,
    row_ids: *const u64,
    count: usize,
    query_out: *mut *mut LanceDBTakeQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    take_offsets_or_row_ids(table, row_ids, count, query_out, false, error_message)
}

/// Set the column projection of a take query
///
/// Columns are returned in the requested order. A zero `num_columns` selects
/// no columns and keeps an empty result schema; `columns` may then be NULL.
/// The names are copied and are not borrowed after this call.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_table_take_offsets`
///   or `lancedb_table_take_row_ids`
/// - `columns` must point to `num_columns` valid NUL-terminated UTF-8 strings,
///   or may be NULL when `num_columns` is zero
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_take_query_select(
    query: *mut LanceDBTakeQuery,
    columns: *const *const c_char,
    num_columns: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let names = match columns_from_c(columns, num_columns) {
        Ok(names) => names,
        Err(()) => {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }
    };

    if update_take_query(query, |inner| inner.select(Select::columns(&names))).is_none() {
        set_custom_error_message(
            error_message,
            "take query builder has already been consumed",
        );
        return LanceDBError::InvalidArgument;
    }
    LanceDBError::Success
}

/// Ask a take query to return the `_rowid` metadata column
///
/// The metadata column is appended to the projection, including a projection
/// selected by `lancedb_take_query_select()`. For an offset take it carries the
/// row ID, which is not the same as the requested dataset offset.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_table_take_offsets`
///   or `lancedb_table_take_row_ids`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_take_query_with_row_id(
    query: *mut LanceDBTakeQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    if update_take_query(query, TakeQuery::with_row_id).is_none() {
        set_custom_error_message(
            error_message,
            "take query builder has already been consumed",
        );
        return LanceDBError::InvalidArgument;
    }
    LanceDBError::Success
}

/// Restore offset-take occurrences in the exact requested order
///
/// Row-ID takes already follow the requested order and ignore this flag. For an
/// offset take, the result follows the input order, including repeated offsets,
/// and missing offsets are omitted.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_table_take_offsets`
///   or `lancedb_table_take_row_ids`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_take_query_preserve_order(
    query: *mut LanceDBTakeQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    (*query).preserve_order = true;
    LanceDBError::Success
}

/// Run the take query held by `query` and return the stream the SDK started
fn run_take_query(query: TakeQuery) -> Result<SendableRecordBatchStream, lancedb::error::Error> {
    get_runtime().block_on(async move { query.execute().await })
}

/// Execute a take query and return the streaming result
///
/// The builder is consumed on every path with a non-NULL pointer, including a
/// rejected result pointer. A returned result uses the same Arrow C Data and
/// Arrow IPC conversions as every other query result.
///
/// # Safety
/// - `query` must be NULL or a valid pointer returned from
///   `lancedb_table_take_offsets` or `lancedb_table_take_row_ids`. A non-NULL
///   pointer is consumed, also when the call fails.
/// - `result_out` must be a valid pointer to receive the result handle
/// - `error_message` can be NULL to ignore detailed error messages
///
/// On entry, `*result_out` and the error message are cleared. On success,
/// `*result_out` holds the streaming result. On failure, the SDK error code and
/// message are returned, including a remote backend's unsupported error.
#[no_mangle]
pub unsafe extern "C" fn lancedb_take_query_execute(
    query: *mut LanceDBTakeQuery,
    result_out: *mut *mut LanceDBQueryResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !result_out.is_null() {
        *result_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let LanceDBTakeQuery {
        inner,
        by_offsets,
        preserve_order,
    } = *Box::from_raw(query);
    if result_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(inner) = inner else {
        set_custom_error_message(
            error_message,
            "take query builder has already been consumed",
        );
        return LanceDBError::InvalidArgument;
    };
    let inner = if by_offsets && preserve_order {
        inner.preserve_order()
    } else {
        inner
    };

    match run_take_query(inner) {
        Ok(stream) => {
            *result_out = query_result_from_stream(stream);
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Free a TakeQuery
///
/// # Safety
/// - `query` must be NULL or a valid pointer returned from
///   `lancedb_table_take_offsets` or `lancedb_table_take_row_ids`
/// - `query` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_take_query_free(query: *mut LanceDBTakeQuery) {
    if !query.is_null() {
        let _ = Box::from_raw(query);
    }
}
