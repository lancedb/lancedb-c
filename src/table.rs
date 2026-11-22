// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Complete Table API implementation for LanceDB C bindings
//!
//! This module provides all table operations using Arrow-only APIs,
//! combining both simple and full table functionality.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;
use std::time::Duration;

use arrow_array::{Array, RecordBatch, RecordBatchReader, StructArray};
use arrow_schema::{ArrowError, Schema};
use futures::TryStreamExt;
use lance::dataset::transaction::UpdateMapEntry;
use lancedb::query::{ExecutableQuery, QueryBase};
use lancedb::table::{ColumnAlteration, NewColumnTransform};

use crate::connection::{get_runtime, LanceDBTable};
use crate::error::{
    clear_error_message, handle_error, set_custom_error_message, set_invalid_argument_message,
    set_not_supported_message, set_unknown_error_message, LanceDBError,
};
use crate::expr::LanceDBExpr;
use crate::ipc::{self, LanceDBIpcBuffer};
use crate::types::{
    LanceDBMergeInsertConfig, LanceDBMergeInsertOptions, LanceDBMergeInsertResult,
    LanceDBRecordBatchReader,
};

/// Get table schema as Arrow C ABI
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `schema_out` must be a valid pointer to receive the Arrow schema
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
/// - The caller is responsible for releasing the schema using Arrow C ABI
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_arrow_schema(
    table: *const LanceDBTable,
    schema_out: *mut *mut arrow_schema::ffi::FFI_ArrowSchema,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || schema_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.schema()) {
        Ok(schema) => {
            // Convert to Arrow C ABI
            let ffi_schema =
                Box::new(arrow_schema::ffi::FFI_ArrowSchema::try_from(&*schema).unwrap());
            *schema_out = Box::into_raw(ffi_schema);
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Write the table schema as an Arrow IPC file that holds no batch
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
///   or `lancedb_table_create`; it is only borrowed
/// - `buffer_out` must be a valid pointer to receive the file bytes
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*buffer_out` holding a readable IPC file that carries
///   the full table schema and no record batch
/// - The SDK error code and text when the schema cannot be read, or
///   `LANCEDB_ARROW` when the schema cannot be encoded
///
/// On entry, `*buffer_out` and `*error_message` are cleared. Free the bytes with
/// `lancedb_ipc_buffer_free()` and a returned error message with
/// `lancedb_free_string()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_schema_ipc_file(
    table: *const LanceDBTable,
    buffer_out: *mut LanceDBIpcBuffer,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !buffer_out.is_null() {
        *buffer_out = LanceDBIpcBuffer::EMPTY;
    }
    clear_error_message(error_message);

    if table.is_null() || buffer_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    match get_runtime().block_on(tbl.schema()) {
        Ok(schema) => match ipc::write_ipc_file_from_schema(&schema) {
            Ok(bytes) => {
                *buffer_out = LanceDBIpcBuffer::from_file_bytes(bytes);
                LanceDBError::Success
            }
            Err(error) => handle_error(
                &lancedb::error::Error::Arrow { source: error },
                error_message,
            ),
        },
        Err(error) => handle_error(&error, error_message),
    }
}

/// Add data to table using Arrow RecordBatchReader
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `reader` must be a valid pointer to LanceDBRecordBatchReader
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_add(
    table: *const LanceDBTable,
    reader: *mut LanceDBRecordBatchReader,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || reader.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    // Take ownership of the reader
    let reader_box = Box::from_raw(reader);

    match runtime.block_on(tbl.add(reader_box.into_inner()).execute()) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// One way the SDK can restrict which matched rows a merge updates
enum MergeUpdateCondition<'a> {
    Sql(&'a str),
    Expr(&'a LanceDBExpr),
}

/// The parts of a merge insert request that do not depend on C argument layout
struct MergeInsertSettings<'a> {
    when_matched_update_all: bool,
    update_condition: Option<MergeUpdateCondition<'a>>,
    when_not_matched_insert_all: bool,
    when_not_matched_by_source_delete: bool,
    when_not_matched_by_source_delete_filter: Option<&'a str>,
    timeout: Option<Duration>,
    use_index: Option<bool>,
}

/// Read the join columns, rejecting a missing array or an empty or non-UTF-8 name
unsafe fn parse_on_columns(
    on_columns: *const *const c_char,
    num_columns: usize,
) -> Option<Vec<String>> {
    if on_columns.is_null() || num_columns == 0 {
        return None;
    }

    let mut column_names = Vec::with_capacity(num_columns);
    for index in 0..num_columns {
        let column_pointer = *on_columns.add(index);
        if column_pointer.is_null() {
            return None;
        }

        let column = CStr::from_ptr(column_pointer).to_str().ok()?;
        column_names.push(column.to_owned());
    }

    Some(column_names)
}

/// Run exactly one SDK merge insert over a schema-bearing reader
fn execute_merge_insert(
    table: &lancedb::Table,
    reader: Box<dyn RecordBatchReader + Send>,
    on_columns: &[&str],
    settings: MergeInsertSettings<'_>,
) -> lancedb::error::Result<lancedb::table::MergeResult> {
    get_runtime().block_on(async {
        let mut builder = table.merge_insert(on_columns);

        if settings.when_matched_update_all {
            match settings.update_condition {
                // The expression is borrowed, ownership stays with the caller
                Some(MergeUpdateCondition::Expr(expr)) => {
                    builder.when_matched_update_all_expr(expr.inner.clone());
                }
                Some(MergeUpdateCondition::Sql(condition)) => {
                    builder.when_matched_update_all(Some(condition.to_owned()));
                }
                None => {
                    builder.when_matched_update_all(None);
                }
            }
        }
        if settings.when_not_matched_insert_all {
            builder.when_not_matched_insert_all();
        }
        if settings.when_not_matched_by_source_delete {
            builder.when_not_matched_by_source_delete(
                settings
                    .when_not_matched_by_source_delete_filter
                    .map(|filter| filter.to_owned()),
            );
        }
        if let Some(timeout) = settings.timeout {
            builder.timeout(timeout);
        }
        if let Some(use_index) = settings.use_index {
            builder.use_index(use_index);
        }

        builder.execute(reader).await
    })
}

/// Merge data into table (upsert operation) using Arrow data
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `data` must be a valid pointer to LanceDBRecordBatchReader containing data to merge
/// - `on_columns` must be an array of valid null-terminated C strings containing column names
/// - `num_on_columns` must match the actual number of columns in the array
/// - `config.when_matched_update_all_condition`, when not NULL, must be a valid null-terminated
///   C string containing an SQL predicate
/// - `config.when_matched_update_all_expr`, when not NULL, must be a valid pointer returned from
///   `lancedb_expr_*` functions. It is not consumed, and remains owned by the caller
/// - the two conditions above are only used when `config.when_matched_update_all` is set,
///   and are mutually exclusive, setting both is an error
/// - `data` is always consumed, also when the other arguments are rejected
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_merge_insert(
    table: *const LanceDBTable,
    data: *mut LanceDBRecordBatchReader,
    on_columns: *const *const c_char,
    num_columns: usize,
    config: *const LanceDBMergeInsertConfig,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if data.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    // Take ownership of the data reader before anything else, so that it is freed
    // on every path, including the ones rejecting the rest of the arguments
    let data_box = Box::from_raw(data);

    if table.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(column_names) = parse_on_columns(on_columns, num_columns) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };
    let on_columns: Vec<&str> = column_names.iter().map(String::as_str).collect();

    // A NULL config keeps the historical default upsert: update matching rows and
    // insert the rest
    let mut settings = MergeInsertSettings {
        when_matched_update_all: true,
        update_condition: None,
        when_not_matched_insert_all: true,
        when_not_matched_by_source_delete: false,
        when_not_matched_by_source_delete_filter: None,
        timeout: None,
        use_index: None,
    };

    if !config.is_null() {
        let config = &*config;
        settings.when_matched_update_all = config.when_matched_update_all != 0;
        settings.when_not_matched_insert_all = config.when_not_matched_insert_all != 0;

        // The update condition is only used when matched records are updated
        if settings.when_matched_update_all {
            let has_expr = !config.when_matched_update_all_expr.is_null();
            let has_sql = !config.when_matched_update_all_condition.is_null();

            // the two flavors of the condition are mutually exclusive
            if has_expr && has_sql {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            }

            if has_expr {
                settings.update_condition = Some(MergeUpdateCondition::Expr(
                    &*config.when_matched_update_all_expr,
                ));
            } else if has_sql {
                let Ok(condition) =
                    CStr::from_ptr(config.when_matched_update_all_condition).to_str()
                else {
                    set_invalid_argument_message(error_message);
                    return LanceDBError::InvalidArgument;
                };
                settings.update_condition = Some(MergeUpdateCondition::Sql(condition));
            }
        }
    }

    match execute_merge_insert(
        &(*table).inner,
        data_box.into_inner(),
        &on_columns,
        settings,
    ) {
        Ok(_) => LanceDBError::Success,
        Err(error) => handle_error(&error, error_message),
    }
}

/// Merge data into table with explicit actions and a result
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `data` must be a valid pointer to a LanceDBRecordBatchReader. It carries both
///   the source schema and every source batch, so one call performs one SDK merge.
///   A reader over a schema-only Arrow IPC file is an empty source
/// - `on_columns` must be an array of `num_columns` valid null-terminated C strings
/// - `options`, when not NULL, must point to a readable `LanceDBMergeInsertOptions`.
///   Its conditions and filters are borrowed for the call
/// - `result_out` must be a valid pointer to receive the SDK result
/// - `data` is always consumed, also when the other arguments are rejected
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*result_out` filled from the SDK result
/// - `LANCEDB_INVALID_ARGUMENT` for a NULL pointer, an empty join-key list, a
///   non-UTF-8 string, or an out-of-range `use_index`/`timeout_ms` sentinel
/// - The SDK error code for a rejected merge
///
/// On entry, `*result_out` and the error message are cleared. The result is only
/// meaningful when the call succeeds. Free a returned error message with
/// `lancedb_free_string()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_merge_insert_with_result(
    table: *const LanceDBTable,
    data: *mut LanceDBRecordBatchReader,
    on_columns: *const *const c_char,
    num_columns: usize,
    options: *const LanceDBMergeInsertOptions,
    result_out: *mut LanceDBMergeInsertResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !result_out.is_null() {
        *result_out = LanceDBMergeInsertResult::default();
    }
    clear_error_message(error_message);

    if data.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    // Take ownership of the data reader before anything else, so that it is freed
    // on every path, including the ones rejecting the rest of the arguments
    let data_box = Box::from_raw(data);

    if table.is_null() || result_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(column_names) = parse_on_columns(on_columns, num_columns) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };
    let on_columns: Vec<&str> = column_names.iter().map(String::as_str).collect();

    // A NULL options pointer keeps the historical default upsert
    let mut settings = MergeInsertSettings {
        when_matched_update_all: true,
        update_condition: None,
        when_not_matched_insert_all: true,
        when_not_matched_by_source_delete: false,
        when_not_matched_by_source_delete_filter: None,
        timeout: None,
        use_index: None,
    };

    if !options.is_null() {
        let options = &*options;
        settings.when_matched_update_all = options.when_matched_update_all != 0;
        settings.when_not_matched_insert_all = options.when_not_matched_insert_all != 0;
        settings.when_not_matched_by_source_delete = options.when_not_matched_by_source_delete != 0;

        if settings.when_matched_update_all && !options.when_matched_update_all_condition.is_null()
        {
            let Ok(condition) = CStr::from_ptr(options.when_matched_update_all_condition).to_str()
            else {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            };
            settings.update_condition = Some(MergeUpdateCondition::Sql(condition));
        }

        if settings.when_not_matched_by_source_delete
            && !options.when_not_matched_by_source_delete_filter.is_null()
        {
            let Ok(filter) =
                CStr::from_ptr(options.when_not_matched_by_source_delete_filter).to_str()
            else {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            };
            settings.when_not_matched_by_source_delete_filter = Some(filter);
        }

        settings.use_index = match options.use_index {
            -1 => None,
            0 => Some(false),
            1 => Some(true),
            _ => {
                set_custom_error_message(
                    error_message,
                    "Invalid argument: use_index must be -1, 0, or 1",
                );
                return LanceDBError::InvalidArgument;
            }
        };

        settings.timeout = match options.timeout_ms {
            -1 => None,
            timeout_ms if timeout_ms >= 0 => Some(Duration::from_millis(timeout_ms as u64)),
            _ => {
                set_custom_error_message(
                    error_message,
                    "Invalid argument: timeout_ms must be -1 or a non-negative number",
                );
                return LanceDBError::InvalidArgument;
            }
        };
    }

    match execute_merge_insert(
        &(*table).inner,
        data_box.into_inner(),
        &on_columns,
        settings,
    ) {
        Ok(merge_result) => {
            *result_out = LanceDBMergeInsertResult {
                version: merge_result.version,
                num_inserted_rows: merge_result.num_inserted_rows,
                num_updated_rows: merge_result.num_updated_rows,
                num_deleted_rows: merge_result.num_deleted_rows,
                num_attempts: merge_result.num_attempts,
            };
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Update option fields of `LanceDBUpdateOptions`
///
/// These values mirror the `LanceDBUpdateField` enumerators of the public header.
const UPDATE_RETRIES: u32 = 1 << 0;
const UPDATE_ALL_FIELDS: u32 = UPDATE_RETRIES;

/// Options for `lancedb_table_update_with_options`
///
/// `set_fields` selects the overrides; a field whose bit is not set keeps the
/// SDK default, so an omitted retry stays distinct from an explicit 0.
#[repr(C)]
#[derive(Debug, Copy, Clone, Default)]
pub struct LanceDBUpdateOptions {
    /// Bitmask of `LanceDBUpdateField` values; 0 uses every default
    pub set_fields: u32,
    /// Per-call remote retry budget; 0 disables retries. Embedded tables ignore it.
    pub retries: u8,
}

/// Result of an update operation.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct LanceDBUpdateResult {
    /// Number of rows changed by the update. A legacy remote empty response
    /// can leave this at zero, which then means unknown rather than no rows.
    pub rows_updated: u64,
    /// Table version committed by the update. A legacy remote empty response
    /// can leave this at zero, which then means unknown.
    pub version: u64,
}

/// Run one SDK update from the C argument layout
///
/// The caller decides whether a result pointer is required. `result_out` is
/// written only after a successful commit, so a failure leaves it unchanged.
unsafe fn execute_update(
    table: *const LanceDBTable,
    columns: *const *const c_char,
    expressions: *const *const c_char,
    num_assignments: usize,
    predicate: *const c_char,
    options: *const LanceDBUpdateOptions,
    result_out: *mut LanceDBUpdateResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || columns.is_null() || expressions.is_null() || num_assignments == 0 {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let retries = if options.is_null() {
        None
    } else {
        let options = &*options;
        let unknown = options.set_fields & !UPDATE_ALL_FIELDS;
        if unknown != 0 {
            set_custom_error_message(
                error_message,
                &format!("unknown update option bits: 0x{unknown:x}"),
            );
            return LanceDBError::InvalidArgument;
        }
        if options.set_fields & UPDATE_RETRIES != 0 {
            Some(options.retries)
        } else {
            None
        }
    };

    let mut assignments = Vec::with_capacity(num_assignments);
    for i in 0..num_assignments {
        let column_ptr = *columns.add(i);
        let expression_ptr = *expressions.add(i);
        if column_ptr.is_null() || expression_ptr.is_null() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }

        let (Ok(column), Ok(expression)) = (
            CStr::from_ptr(column_ptr).to_str(),
            CStr::from_ptr(expression_ptr).to_str(),
        ) else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };

        if column.is_empty() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }

        assignments.push((column, expression));
    }

    let predicate = if predicate.is_null() {
        None
    } else {
        let Ok(predicate) = CStr::from_ptr(predicate).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };
        // An empty predicate is the documented "update all rows" form.
        if predicate.is_empty() {
            None
        } else {
            Some(predicate)
        }
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(async {
        let mut builder = tbl.update();
        if let Some(predicate) = predicate {
            builder = builder.only_if(predicate);
        }
        if let Some(retries) = retries {
            // An explicit 0 reaches the SDK as a disabled retry budget; an
            // omitted retry leaves the SDK default in place.
            builder = builder.retries(retries);
        }
        for (column, expression) in assignments {
            // The input order only preserves the caller's column/expression pairing.
            // The SDK evaluates every expression against the original row.
            builder = builder.column(column, expression);
        }
        builder.execute().await
    }) {
        Ok(update_result) => {
            if !result_out.is_null() {
                *result_out = LanceDBUpdateResult {
                    rows_updated: update_result.rows_updated,
                    version: update_result.version,
                };
            }
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Update rows in a table using SQL expressions
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `columns` and `expressions` must point to arrays of `num_assignments` valid
///   null-terminated C strings, with `columns[i]` assigned the value of `expressions[i]`.
///   The inputs are paired by index; the SDK evaluates every expression against the
///   original row, so assignments do not observe each other
/// - `num_assignments` must be greater than zero
/// - `predicate`, when not NULL, must be a valid null-terminated C string containing an SQL
///   predicate. An empty predicate updates all rows
/// - `result_out` must be a valid pointer to receive the operation result
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_update(
    table: *const LanceDBTable,
    columns: *const *const c_char,
    expressions: *const *const c_char,
    num_assignments: usize,
    predicate: *const c_char,
    result_out: *mut LanceDBUpdateResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if result_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    execute_update(
        table,
        columns,
        expressions,
        num_assignments,
        predicate,
        ptr::null(),
        result_out,
        error_message,
    )
}

/// Update rows in a table using SQL expressions, with options and an optional result
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `columns` and `expressions` must point to arrays of `num_assignments` valid
///   null-terminated C strings, with `columns[i]` assigned the value of `expressions[i]`.
///   The inputs are paired by index; the SDK evaluates every expression against the
///   original row, so assignments do not observe each other
/// - `num_assignments` must be greater than zero
/// - `predicate`, when not NULL, must be a valid null-terminated C string containing an SQL
///   predicate. An empty predicate updates all rows
/// - `options`, when not NULL, must point to a valid `LanceDBUpdateOptions`
/// - `result_out` can be NULL when the caller does not need the result
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_update_with_options(
    table: *const LanceDBTable,
    columns: *const *const c_char,
    expressions: *const *const c_char,
    num_assignments: usize,
    predicate: *const c_char,
    options: *const LanceDBUpdateOptions,
    result_out: *mut LanceDBUpdateResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    execute_update(
        table,
        columns,
        expressions,
        num_assignments,
        predicate,
        options,
        result_out,
        error_message,
    )
}

/// Create a RecordBatchReader from Arrow C ABI structures
///
/// # Safety
/// - `array` must be a valid pointer to FFI_ArrowArray containing the record batch data
/// - `schema` must be a valid pointer to FFI_ArrowSchema containing the schema
/// - `reader_out` must be a valid pointer to receive the created reader
/// - `error_message` can be NULL to ignore detailed error messages
/// - The caller is responsible for ensuring the array and schema are properly formatted
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_record_batch_reader_from_arrow(
    array: *const arrow_array::ffi::FFI_ArrowArray,
    schema: *const arrow_schema::ffi::FFI_ArrowSchema,
    reader_out: *mut *mut LanceDBRecordBatchReader,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if array.is_null() || schema.is_null() || reader_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    // Import the schema from C ABI
    let imported_schema = match Schema::try_from(&*schema) {
        Ok(schema) => std::sync::Arc::new(schema),
        Err(e) => {
            return handle_error(&lancedb::error::Error::Arrow { source: e }, error_message);
        }
    };

    // Import the array from C ABI and convert to RecordBatch
    // We need to create owned copies of the FFI structures for the conversion
    let array_ffi = unsafe { ptr::read(array) };
    let record_batch = match arrow_array::ffi::from_ffi(array_ffi, &*schema) {
        Ok(array_data) => {
            // Convert the imported array data to a StructArray, then to RecordBatch
            let struct_array = StructArray::from(array_data);
            RecordBatch::from(&struct_array)
        }
        Err(e) => {
            return handle_error(&lancedb::error::Error::Arrow { source: e }, error_message);
        }
    };

    // Note: According to Arrow C ABI specification, this function consumes the array.
    // The caller should not call the release function after passing the array here.

    // Create a RecordBatchReader that yields this single batch
    struct SingleBatchReader {
        schema: std::sync::Arc<Schema>,
        batch: Option<RecordBatch>,
        exhausted: bool,
    }

    impl Iterator for SingleBatchReader {
        type Item = Result<RecordBatch, ArrowError>;

        fn next(&mut self) -> Option<Self::Item> {
            if self.exhausted {
                None
            } else {
                self.exhausted = true;
                self.batch.take().map(Ok)
            }
        }
    }

    impl RecordBatchReader for SingleBatchReader {
        fn schema(&self) -> std::sync::Arc<Schema> {
            self.schema.clone()
        }
    }

    let reader = SingleBatchReader {
        schema: imported_schema.clone(),
        batch: Some(record_batch),
        exhausted: false,
    };

    let wrapper = Box::new(LanceDBRecordBatchReader::new(Box::new(reader)));

    *reader_out = Box::into_raw(wrapper);
    LanceDBError::Success
}

/// Free a RecordBatchReader wrapper
///
/// # Safety
/// - `reader` must be a valid pointer returned from `lancedb_record_batch_reader_new`
/// - `reader` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_record_batch_reader_free(reader: *mut LanceDBRecordBatchReader) {
    if !reader.is_null() {
        let _ = Box::from_raw(reader);
    }
}

/// Free Arrow C ABI schema
///
/// # Safety
/// - `schema` must be a valid pointer returned by LanceDB functions
/// - `schema` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_arrow_schema(
    schema: *mut arrow_schema::ffi::FFI_ArrowSchema,
) {
    if !schema.is_null() {
        let _ = Box::from_raw(schema);
    }
}

/* ========== TABLE UTILITY OPERATIONS ========== */

/// Get table version
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
///
/// # Returns
/// - Table version number on success, 0 on failure
///
/// A version is never 0 for a valid table, so 0 is a safe failure sentinel. Use
/// `lancedb_table_version_with_status` when the failure details are needed.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_version(table: *const LanceDBTable) -> u64 {
    if table.is_null() {
        return 0;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    runtime.block_on(tbl.version()).unwrap_or(0)
}

/// Get table version, preserving failures
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `version_out` must be a valid pointer to receive the table version
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
///
/// On success, `version_out` is set to the table version. On failure,
/// `version_out` is left unchanged, so a caller can keep its own sentinel there.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_version_with_status(
    table: *const LanceDBTable,
    version_out: *mut u64,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || version_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.version()) {
        Ok(version) => {
            *version_out = version;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Count rows in table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
///
/// # Returns
/// - Number of rows in table on success, 0 on failure (or empty table)
///
/// Use `lancedb_table_count_rows_with_status` when an empty table must be told
/// apart from a failed call.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_count_rows(table: *const LanceDBTable) -> u64 {
    if table.is_null() {
        return 0;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.count_rows(None)) {
        Ok(count) => count as u64,
        Err(_) => 0,
    }
}

/// Count rows in table, preserving failures
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `count_out` must be a valid pointer to receive the row count
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
///
/// On success, `count_out` is set to the number of rows, which is 0 for an empty
/// table. On failure, `count_out` is left unchanged.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_count_rows_with_status(
    table: *const LanceDBTable,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.count_rows(None)) {
        Ok(count) => {
            *count_out = count;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Count rows in table matching an optional SQL filter, preserving failures
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `filter`, when not NULL, must be a valid null-terminated C string containing an
///   SQL predicate. A NULL filter counts every row; a non-NULL string, including an
///   empty one, is passed to the SDK unchanged
/// - `count_out` must be a valid pointer to receive the row count
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
///
/// On success, `count_out` is set to the number of matching rows, which is 0 when
/// none match. On failure, `count_out` is left unchanged.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_count_rows_filtered(
    table: *const LanceDBTable,
    filter: *const c_char,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let filter = if filter.is_null() {
        None
    } else {
        let Ok(filter) = CStr::from_ptr(filter).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };
        Some(filter.to_owned())
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.count_rows(filter)) {
        Ok(count) => {
            *count_out = count;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Statistics on the number of rows in table fragments, part of `LanceDBTableStats`
#[repr(C)]
#[derive(Debug)]
pub struct LanceDBFragmentSummaryStats {
    pub min: usize,
    pub max: usize,
    pub mean: usize,
    pub p25: usize,
    pub p50: usize,
    pub p75: usize,
    pub p99: usize,
}

/// Statistics on table fragments, part of `LanceDBTableStats`
#[repr(C)]
#[derive(Debug)]
pub struct LanceDBFragmentStats {
    pub num_fragments: usize,
    pub num_small_fragments: usize,
    pub lengths: LanceDBFragmentSummaryStats,
}

/// Table statistics returned by `lancedb_table_stats`
#[repr(C)]
#[derive(Debug)]
pub struct LanceDBTableStats {
    pub total_bytes: usize,
    pub num_rows: usize,
    /// Rows marked as deleted but not yet compacted away, or -1 if unknown
    pub num_deleted_rows: i64,
    pub num_indices: usize,
    pub fragment_stats: LanceDBFragmentStats,
}

/// Get table statistics
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `stats_out` must be a valid pointer to receive the statistics
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_stats(
    table: *const LanceDBTable,
    stats_out: *mut LanceDBTableStats,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || stats_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    let stats = match runtime.block_on(tbl.stats()) {
        Ok(stats) => stats,
        Err(e) => return handle_error(&e, error_message),
    };

    // Deleted rows are only marked in the deletion files of the underlying
    // lance dataset, so they are counted from there. Remote tables do not
    // expose their dataset, in which case the count is reported as unknown.
    let num_deleted_rows = match tbl.dataset() {
        Some(ds) => {
            let count = runtime.block_on(async {
                let dataset = ds.get().await?;
                dataset
                    .count_deleted_rows()
                    .await
                    .map_err(lancedb::Error::from)
            });
            match count {
                Ok(count) => count as i64,
                Err(e) => return handle_error(&e, error_message),
            }
        }
        None => -1,
    };

    let lengths = &stats.fragment_stats.lengths;
    *stats_out = LanceDBTableStats {
        total_bytes: stats.total_bytes,
        num_rows: stats.num_rows,
        num_deleted_rows,
        num_indices: stats.num_indices,
        fragment_stats: LanceDBFragmentStats {
            num_fragments: stats.fragment_stats.num_fragments,
            num_small_fragments: stats.fragment_stats.num_small_fragments,
            lengths: LanceDBFragmentSummaryStats {
                min: lengths.min,
                max: lengths.max,
                mean: lengths.mean,
                p25: lengths.p25,
                p50: lengths.p50,
                p75: lengths.p75,
                p99: lengths.p99,
            },
        },
    };

    LanceDBError::Success
}

/// Result of a delete operation.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct LanceDBDeleteResult {
    /// Number of rows deleted. A legacy remote empty response can leave this at
    /// zero, which then means unknown rather than no rows.
    pub num_deleted_rows: u64,
    /// Table version committed by the delete. A legacy remote empty response can
    /// leave this at zero, which then means unknown.
    pub version: u64,
}

/// Run one SDK delete with an SQL predicate
///
/// `result_out` is written only after a successful commit, so a failure leaves
/// it unchanged. The caller decides whether the result is required.
unsafe fn execute_sql_delete(
    table: *const LanceDBTable,
    predicate: *const c_char,
    result_out: *mut LanceDBDeleteResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || predicate.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(predicate_str) = CStr::from_ptr(predicate).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.delete(predicate_str)) {
        Ok(delete_result) => {
            if !result_out.is_null() {
                *result_out = LanceDBDeleteResult {
                    num_deleted_rows: delete_result.num_deleted_rows,
                    version: delete_result.version,
                };
            }
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Run one SDK delete with a DataFusion expression
///
/// The expression is consumed on every path. `result_out` is written only after
/// a successful commit, so a failure leaves it unchanged.
unsafe fn execute_df_delete(
    table: *const LanceDBTable,
    expr: *mut LanceDBExpr,
    result_out: *mut LanceDBDeleteResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if expr.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let expr_box = Box::from_raw(expr);

    if table.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }
    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.delete(&expr_box.inner)) {
        Ok(delete_result) => {
            if !result_out.is_null() {
                *result_out = LanceDBDeleteResult {
                    num_deleted_rows: delete_result.num_deleted_rows,
                    version: delete_result.version,
                };
            }
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Delete rows from table based on predicate
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `predicate` must be a valid null-terminated C string containing SQL WHERE clause
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delete(
    table: *const LanceDBTable,
    predicate: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    execute_sql_delete(table, predicate, ptr::null_mut(), error_message)
}

/// Delete rows from table based on predicate, with an optional SDK result
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `predicate` must be a valid null-terminated C string containing SQL WHERE clause
/// - `result_out` can be NULL when the caller does not need the result
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delete_with_result(
    table: *const LanceDBTable,
    predicate: *const c_char,
    result_out: *mut LanceDBDeleteResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    execute_sql_delete(table, predicate, result_out, error_message)
}

/// Delete rows from table using a DataFusion expression
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `expr` must be a valid pointer returned from `lancedb_expr_*` functions
/// - `expr` is consumed by this function; do not use or free it after calling
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_df_delete(
    table: *const LanceDBTable,
    expr: *mut LanceDBExpr,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    execute_df_delete(table, expr, ptr::null_mut(), error_message)
}

/// Delete rows from table using a DataFusion expression, with an optional SDK result
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `expr` must be a valid pointer returned from `lancedb_expr_*` functions
/// - `expr` is consumed by this function on every path; do not use or free it after calling
/// - `result_out` can be NULL when the caller does not need the result
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_df_delete_with_result(
    table: *const LanceDBTable,
    expr: *mut LanceDBExpr,
    result_out: *mut LanceDBDeleteResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    execute_df_delete(table, expr, result_out, error_message)
}

/// Vector search using nearest_to with full result conversion
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `vector` must be a valid pointer to array of floats
/// - `dimension` must match the actual dimension of the vector column
/// - `limit` must be > 0
/// - `result_arrays` must be a valid pointer to receive Arrow C ABI array results
/// - `result_schema` must be a valid pointer to receive single Arrow C ABI schema
/// - `count_out` must be a valid pointer to receive the number of result batches
///
/// # Returns
/// - Error code indicating success or failure
/// - Caller must free arrays with `lancedb_free_arrow_arrays` and schema with `lancedb_free_arrow_schema`
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_nearest_to(
    table: *const LanceDBTable,
    vector: *const f32,
    dimension: usize,
    limit: usize,
    column: *const c_char,
    result_arrays: *mut *mut *mut arrow_array::ffi::FFI_ArrowArray,
    result_schema: *mut *mut arrow_schema::ffi::FFI_ArrowSchema,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null()
        || vector.is_null()
        || dimension == 0
        || limit == 0
        || result_arrays.is_null()
        || result_schema.is_null()
        || count_out.is_null()
    {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();
    let vec_slice = std::slice::from_raw_parts(vector, dimension);
    let vec_data: Vec<f32> = vec_slice.to_vec();

    let column_name = if column.is_null() {
        None
    } else {
        match CStr::from_ptr(column).to_str() {
            Ok(s) => Some(s),
            Err(_) => {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            }
        }
    };

    match runtime.block_on(async {
        let mut query = tbl.query().limit(limit).nearest_to(vec_data)?;

        if let Some(col) = column_name {
            query = query.column(col);
        }

        let batches: Vec<RecordBatch> = query.execute().await?.try_collect().await?;
        Ok::<Vec<RecordBatch>, lancedb::error::Error>(batches)
    }) {
        Ok(batches) => {
            let count = batches.len();
            *count_out = count;

            if count == 0 {
                *result_arrays = ptr::null_mut();
                *result_schema = ptr::null_mut();
                return LanceDBError::Success;
            }

            // Get schema from first batch (all batches have same schema)
            let schema = batches[0].schema();
            let ffi_schema = match arrow_schema::ffi::FFI_ArrowSchema::try_from(&*schema) {
                Ok(schema) => Box::new(schema),
                Err(_) => {
                    set_unknown_error_message(error_message);
                    return LanceDBError::Unknown;
                }
            };
            *result_schema = Box::into_raw(ffi_schema);

            // Allocate array for Arrow C ABI array structures
            let arrays_ptr =
                libc::malloc(count * std::mem::size_of::<*mut arrow_array::ffi::FFI_ArrowArray>())
                    as *mut *mut arrow_array::ffi::FFI_ArrowArray;
            if arrays_ptr.is_null() {
                // Clean up schema on allocation failure
                let _ = Box::from_raw(*result_schema);
                *result_schema = ptr::null_mut();
                set_unknown_error_message(error_message);
                return LanceDBError::Unknown;
            }

            for (i, batch) in batches.into_iter().enumerate() {
                // Convert RecordBatch to StructArray first, then to FFI_ArrowArray
                let struct_array: StructArray = batch.clone().into();
                let array_data: lancedb::arrow::arrow_data::ArrayData = struct_array.into_data();
                let ffi_array = Box::new(arrow_array::ffi::FFI_ArrowArray::new(&array_data));
                *arrays_ptr.add(i) = Box::into_raw(ffi_array);
            }

            *result_arrays = arrays_ptr;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// C-compatible version information struct
#[repr(C)]
pub struct LanceDBVersion {
    pub version: u64,
    pub timestamp_seconds: i64,
    pub timestamp_nanos: u32,
}

/// C-compatible per-version metadata struct
#[repr(C)]
pub struct LanceDBVersionMetadata {
    pub keys: *mut *mut c_char,
    pub values: *mut *mut c_char,
    pub count: usize,
}

/// List all versions of the table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `versions_out` must be a valid pointer to receive the array of LanceDBVersion structs
/// - `metadata_out` can be NULL to skip per-version metadata
/// - `count_out` must be a valid pointer to receive the count
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_list_versions(
    table: *const LanceDBTable,
    versions_out: *mut *mut LanceDBVersion,
    metadata_out: *mut *mut LanceDBVersionMetadata,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || versions_out.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();
    let include_metadata = !metadata_out.is_null();

    match runtime.block_on(tbl.list_versions()) {
        Ok(versions) => {
            let count = versions.len();
            *count_out = count;

            if count == 0 {
                *versions_out = ptr::null_mut();
                if include_metadata {
                    *metadata_out = ptr::null_mut();
                }
                return LanceDBError::Success;
            }

            // Allocate array of LanceDBVersion structs
            let versions_array =
                libc::malloc(count * std::mem::size_of::<LanceDBVersion>()) as *mut LanceDBVersion;
            if versions_array.is_null() {
                set_unknown_error_message(error_message);
                return LanceDBError::Unknown;
            }

            // Allocate metadata array if requested
            let metadata_array = if include_metadata {
                let arr = libc::malloc(count * std::mem::size_of::<LanceDBVersionMetadata>())
                    as *mut LanceDBVersionMetadata;
                if arr.is_null() {
                    libc::free(versions_array as *mut libc::c_void);
                    set_unknown_error_message(error_message);
                    return LanceDBError::Unknown;
                }
                arr
            } else {
                ptr::null_mut()
            };

            for (i, ver) in versions.into_iter().enumerate() {
                let dest = &mut *versions_array.add(i);
                dest.version = ver.version;
                dest.timestamp_seconds = ver.timestamp.timestamp();
                dest.timestamp_nanos = ver.timestamp.timestamp_subsec_nanos();

                if include_metadata {
                    let md = &mut *metadata_array.add(i);
                    let metadata_count = ver.metadata.len();
                    md.count = metadata_count;

                    if metadata_count == 0 {
                        md.keys = ptr::null_mut();
                        md.values = ptr::null_mut();
                    } else {
                        let keys_array =
                            libc::malloc(metadata_count * std::mem::size_of::<*mut c_char>())
                                as *mut *mut c_char;
                        let values_array =
                            libc::malloc(metadata_count * std::mem::size_of::<*mut c_char>())
                                as *mut *mut c_char;

                        if keys_array.is_null() || values_array.is_null() {
                            if !keys_array.is_null() {
                                libc::free(keys_array as *mut libc::c_void);
                            }
                            if !values_array.is_null() {
                                libc::free(values_array as *mut libc::c_void);
                            }
                            // Clean up previously allocated metadata
                            for j in 0..i {
                                free_version_metadata(&*metadata_array.add(j));
                            }
                            libc::free(metadata_array as *mut libc::c_void);
                            libc::free(versions_array as *mut libc::c_void);
                            set_unknown_error_message(error_message);
                            return LanceDBError::Unknown;
                        }

                        for (j, (key, value)) in ver.metadata.into_iter().enumerate() {
                            let c_key = CString::new(key).unwrap_or_default();
                            let c_value = CString::new(value).unwrap_or_default();
                            *keys_array.add(j) = c_key.into_raw();
                            *values_array.add(j) = c_value.into_raw();
                        }

                        md.keys = keys_array;
                        md.values = values_array;
                    }
                }
            }

            *versions_out = versions_array;
            if include_metadata {
                *metadata_out = metadata_array;
            }
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Helper to free metadata arrays of a single LanceDBVersionMetadata
unsafe fn free_version_metadata(md: &LanceDBVersionMetadata) {
    if !md.keys.is_null() {
        for j in 0..md.count {
            let key_ptr = *md.keys.add(j);
            if !key_ptr.is_null() {
                let _ = CString::from_raw(key_ptr);
            }
        }
        libc::free(md.keys as *mut libc::c_void);
    }
    if !md.values.is_null() {
        for j in 0..md.count {
            let val_ptr = *md.values.add(j);
            if !val_ptr.is_null() {
                let _ = CString::from_raw(val_ptr);
            }
        }
        libc::free(md.values as *mut libc::c_void);
    }
}

/// Free versions array returned by lancedb_table_list_versions
///
/// # Safety
/// - `versions` must be a pointer returned by `lancedb_table_list_versions`
/// - `count` must match the count returned by `lancedb_table_list_versions`
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_versions(versions: *mut LanceDBVersion, count: usize) {
    let _ = count;
    if !versions.is_null() {
        libc::free(versions as *mut libc::c_void);
    }
}

/// Free version metadata array returned by lancedb_table_list_versions
///
/// # Safety
/// - `metadata` must be a pointer returned by `lancedb_table_list_versions`
/// - `count` must match the count returned by `lancedb_table_list_versions`
/// - Safe to call with NULL pointer
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_version_metadata(
    metadata: *mut LanceDBVersionMetadata,
    count: usize,
) {
    if !metadata.is_null() {
        for i in 0..count {
            free_version_metadata(&*metadata.add(i));
        }
        libc::free(metadata as *mut libc::c_void);
    }
}

/* ========== TABLE HISTORY OPERATIONS ========== */

/// C-compatible tag information struct
///
/// The array and its strings are read-only to C callers and owned by the
/// library. Free them with `lancedb_free_tags`.
#[repr(C)]
pub struct LanceDBTag {
    pub name: *const c_char,
    pub version: u64,
    pub manifest_size: u64,
    pub branch: *const c_char,
}

/// Check out a specific version of the table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_checkout(
    table: *const LanceDBTable,
    version: u64,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = (*table).inner.clone();
    let runtime = get_runtime();

    match runtime.block_on(async move { tbl.checkout(version).await }) {
        Ok(()) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Check out the version referenced by a tag
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `tag` must be a valid null-terminated C string
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_checkout_tag(
    table: *const LanceDBTable,
    tag: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || tag.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(tag_str) = CStr::from_ptr(tag).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };
    let tag = tag_str.to_string();

    let tbl = (*table).inner.clone();
    let runtime = get_runtime();

    match runtime.block_on(async move { tbl.checkout_tag(&tag).await }) {
        Ok(()) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Return the table to the latest version
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_checkout_latest(
    table: *const LanceDBTable,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = (*table).inner.clone();
    let runtime = get_runtime();

    match runtime.block_on(async move { tbl.checkout_latest().await }) {
        Ok(()) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Restore the table to the currently checked out version
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_restore(
    table: *const LanceDBTable,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = (*table).inner.clone();
    let runtime = get_runtime();

    match runtime.block_on(async move { tbl.restore().await }) {
        Ok(()) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// List all tags of the table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `tags_out` must be a valid pointer to receive the read-only array of LanceDBTag structs
/// - `count_out` must be a valid pointer to receive the count
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_list_tags(
    table: *const LanceDBTable,
    tags_out: *mut *const LanceDBTag,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || tags_out.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = (*table).inner.clone();
    let runtime = get_runtime();

    let tags = match runtime.block_on(async move {
        let tags = tbl.tags().await?;
        tags.list().await
    }) {
        Ok(tags) => tags,
        Err(e) => return handle_error(&e, error_message),
    };

    // Convert everything before allocating so that a string conversion failure
    // cannot leave a partially initialized array behind.
    let mut entries: Vec<(CString, u64, u64, Option<CString>)> = Vec::with_capacity(tags.len());
    for (name, contents) in tags {
        let Ok(name) = CString::new(name) else {
            set_unknown_error_message(error_message);
            return LanceDBError::Unknown;
        };
        let branch = match contents.branch {
            Some(branch) => match CString::new(branch) {
                Ok(branch) => Some(branch),
                Err(_) => {
                    set_unknown_error_message(error_message);
                    return LanceDBError::Unknown;
                }
            },
            None => None,
        };
        entries.push((
            name,
            contents.version,
            contents.manifest_size as u64,
            branch,
        ));
    }

    let count = entries.len();
    if count == 0 {
        *tags_out = ptr::null();
        *count_out = 0;
        return LanceDBError::Success;
    }

    let tags_array = libc::malloc(count * std::mem::size_of::<LanceDBTag>()) as *mut LanceDBTag;
    if tags_array.is_null() {
        set_unknown_error_message(error_message);
        return LanceDBError::Unknown;
    }

    for (i, (name, version, manifest_size, branch)) in entries.into_iter().enumerate() {
        let dest = &mut *tags_array.add(i);
        dest.name = name.into_raw() as *const c_char;
        dest.version = version;
        dest.manifest_size = manifest_size;
        dest.branch = match branch {
            Some(branch) => branch.into_raw() as *const c_char,
            None => ptr::null(),
        };
    }

    *tags_out = tags_array as *const LanceDBTag;
    *count_out = count;
    LanceDBError::Success
}

/// Get the version referenced by a tag
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `tag` must be a valid null-terminated C string
/// - `version_out` must be a valid pointer to receive the tag's version
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_get_tag_version(
    table: *const LanceDBTable,
    tag: *const c_char,
    version_out: *mut u64,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || tag.is_null() || version_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(tag_str) = CStr::from_ptr(tag).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };
    let tag = tag_str.to_string();

    let tbl = (*table).inner.clone();
    let runtime = get_runtime();

    match runtime.block_on(async move {
        let tags = tbl.tags().await?;
        tags.get_version(&tag).await
    }) {
        Ok(version) => {
            *version_out = version;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Create a tag for a table version
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `tag` must be a valid null-terminated C string
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_create_tag(
    table: *const LanceDBTable,
    tag: *const c_char,
    version: u64,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || tag.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(tag_str) = CStr::from_ptr(tag).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };
    let tag = tag_str.to_string();

    let tbl = (*table).inner.clone();
    let runtime = get_runtime();

    match runtime.block_on(async move {
        let mut tags = tbl.tags().await?;
        tags.create(&tag, version).await
    }) {
        Ok(()) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Delete a tag from the table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `tag` must be a valid null-terminated C string
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delete_tag(
    table: *const LanceDBTable,
    tag: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || tag.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(tag_str) = CStr::from_ptr(tag).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };
    let tag = tag_str.to_string();

    let tbl = (*table).inner.clone();
    let runtime = get_runtime();

    match runtime.block_on(async move {
        let mut tags = tbl.tags().await?;
        tags.delete(&tag).await
    }) {
        Ok(()) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Update an existing tag to reference another table version
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `tag` must be a valid null-terminated C string
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_update_tag(
    table: *const LanceDBTable,
    tag: *const c_char,
    version: u64,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || tag.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(tag_str) = CStr::from_ptr(tag).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };
    let tag = tag_str.to_string();

    let tbl = (*table).inner.clone();
    let runtime = get_runtime();

    match runtime.block_on(async move {
        let mut tags = tbl.tags().await?;
        tags.update(&tag, version).await
    }) {
        Ok(()) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Reclaim a tag string allocated by lancedb_table_list_tags
unsafe fn free_tag_string(value: *const c_char) {
    if !value.is_null() {
        let _ = CString::from_raw(value as *mut c_char);
    }
}

/// Free tag array returned by lancedb_table_list_tags
///
/// # Safety
/// - `tags` must be a read-only pointer returned by `lancedb_table_list_tags`
/// - `count` must match the count returned by `lancedb_table_list_tags`
/// - Safe to call with NULL pointer
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_tags(tags: *const LanceDBTag, count: usize) {
    if tags.is_null() {
        return;
    }

    for i in 0..count {
        let tag = &*tags.add(i);
        free_tag_string(tag.name);
        free_tag_string(tag.branch);
    }
    libc::free(tags as *mut libc::c_void);
}

/* ========== TABLE METADATA OPERATIONS ========== */

/// Get table metadata as key-value pairs
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `filter_keys` can be NULL to get all metadata; if non-NULL, `filter_count` must be > 0
/// - `keys_out`, `values_out`, and `count_out` must be valid pointers
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
/// - The caller is responsible for freeing the returned arrays using `lancedb_free_metadata`
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_get_metadata(
    table: *const LanceDBTable,
    filter_keys: *const *const c_char,
    filter_count: usize,
    keys_out: *mut *mut *mut c_char,
    values_out: *mut *mut *mut c_char,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null()
        || keys_out.is_null()
        || values_out.is_null()
        || count_out.is_null()
        || (filter_keys.is_null() && filter_count > 0)
        || (!filter_keys.is_null() && filter_count == 0)
    {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    let ds = match tbl.dataset() {
        Some(ds) => ds,
        None => {
            set_not_supported_message(error_message);
            return LanceDBError::NotSupported;
        }
    };

    let guard = match runtime.block_on(ds.get()) {
        Ok(g) => g,
        Err(e) => return handle_error(&e, error_message),
    };

    let metadata = &guard.manifest().table_metadata;

    // Collect matching entries: either filtered or all
    let entries: Vec<(&str, &str)> = if !filter_keys.is_null() && filter_count > 0 {
        let mut result = Vec::with_capacity(filter_count);
        for i in 0..filter_count {
            let key_ptr = *filter_keys.add(i);
            if key_ptr.is_null() {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            }
            let Ok(key_str) = CStr::from_ptr(key_ptr).to_str() else {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            };
            if let Some(value) = metadata.get(key_str) {
                result.push((key_str, value.as_str()));
            }
        }
        result
    } else {
        metadata
            .iter()
            .map(|(k, v)| (k.as_str(), v.as_str()))
            .collect()
    };

    let count = entries.len();
    *count_out = count;

    if count == 0 {
        *keys_out = ptr::null_mut();
        *values_out = ptr::null_mut();
        return LanceDBError::Success;
    }

    // Allocate arrays
    let keys_array = libc::malloc(count * std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
    let values_array = libc::malloc(count * std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;

    if keys_array.is_null() || values_array.is_null() {
        if !keys_array.is_null() {
            libc::free(keys_array as *mut libc::c_void);
        }
        if !values_array.is_null() {
            libc::free(values_array as *mut libc::c_void);
        }
        set_unknown_error_message(error_message);
        return LanceDBError::Unknown;
    }

    for (i, (key, value)) in entries.iter().enumerate() {
        let c_key = CString::new(*key).unwrap_or_default();
        let c_value = CString::new(*value).unwrap_or_default();
        *keys_array.add(i) = c_key.into_raw();
        *values_array.add(i) = c_value.into_raw();
    }

    *keys_out = keys_array;
    *values_out = values_array;
    LanceDBError::Success
}

/// Set (upsert) table metadata key-value pairs
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `keys` and `values` must be arrays of valid null-terminated C strings
/// - `count` must match the actual number of entries in the arrays
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_set_metadata(
    table: *const LanceDBTable,
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || keys.is_null() || values.is_null() || count == 0 {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    // Extract key-value pairs
    let mut entries = Vec::with_capacity(count);
    for i in 0..count {
        let key_ptr = *keys.add(i);
        let val_ptr = *values.add(i);
        if key_ptr.is_null() || val_ptr.is_null() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }

        let Ok(key_str) = CStr::from_ptr(key_ptr).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };
        let Ok(val_str) = CStr::from_ptr(val_ptr).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };

        entries.push(UpdateMapEntry {
            key: key_str.to_string(),
            value: Some(val_str.to_string()),
        });
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    let ds = match tbl.dataset() {
        Some(ds) => ds,
        None => {
            set_not_supported_message(error_message);
            return LanceDBError::NotSupported;
        }
    };

    match runtime.block_on(async {
        ds.ensure_mutable()?;
        let mut dataset = (*ds.get().await?).clone();
        dataset.update_metadata(entries).await?;
        ds.update(dataset);
        Ok::<_, lancedb::error::Error>(())
    }) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Delete table metadata keys
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `keys` must be an array of valid null-terminated C strings
/// - `count` must match the actual number of keys in the array
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_delete_metadata(
    table: *const LanceDBTable,
    keys: *const *const c_char,
    count: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || keys.is_null() || count == 0 {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    // Extract keys as delete entries (value = None)
    let mut entries = Vec::with_capacity(count);
    for i in 0..count {
        let key_ptr = *keys.add(i);
        if key_ptr.is_null() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }

        let Ok(key_str) = CStr::from_ptr(key_ptr).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };

        entries.push(UpdateMapEntry {
            key: key_str.to_string(),
            value: None,
        });
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    let ds = match tbl.dataset() {
        Some(ds) => ds,
        None => {
            set_not_supported_message(error_message);
            return LanceDBError::NotSupported;
        }
    };

    match runtime.block_on(async {
        ds.ensure_mutable()?;
        let mut dataset = (*ds.get().await?).clone();
        dataset.update_metadata(entries).await?;
        ds.update(dataset);
        Ok::<_, lancedb::error::Error>(())
    }) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Free metadata arrays returned by lancedb_table_get_metadata
///
/// # Safety
/// - `keys` and `values` must be pointers returned by `lancedb_table_get_metadata`
/// - `count` must match the count returned by `lancedb_table_get_metadata`
/// - Safe to call with NULL pointers
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_metadata(
    keys: *mut *mut c_char,
    values: *mut *mut c_char,
    count: usize,
) {
    if !keys.is_null() {
        for i in 0..count {
            let key_ptr = *keys.add(i);
            if !key_ptr.is_null() {
                let _ = CString::from_raw(key_ptr);
            }
        }
        libc::free(keys as *mut libc::c_void);
    }
    if !values.is_null() {
        for i in 0..count {
            let val_ptr = *values.add(i);
            if !val_ptr.is_null() {
                let _ = CString::from_raw(val_ptr);
            }
        }
        libc::free(values as *mut libc::c_void);
    }
}

/* ========== SCHEMA EVOLUTION ========== */

/// One column alteration for `lancedb_table_alter_columns`.
#[repr(C)]
#[derive(Debug)]
pub struct LanceDBColumnAlteration {
    /// Existing column to alter.
    pub path: *const c_char,
    /// New column name, or NULL to leave the name unchanged.
    pub rename: *const c_char,
    /// 0 leaves nullability unchanged, 1 applies `nullable`.
    pub set_nullable: i32,
    /// New nullability when `set_nullable` is 1: 0 for NOT NULL, non-zero for NULL.
    pub nullable: i32,
}

/// Read a required C string as UTF-8, reporting an invalid argument on failure.
unsafe fn required_c_string<'a>(
    value: *const c_char,
    error_message: *mut *mut c_char,
) -> Option<&'a str> {
    if value.is_null() {
        set_invalid_argument_message(error_message);
        return None;
    }

    match CStr::from_ptr(value).to_str() {
        Ok(value) => Some(value),
        Err(_) => {
            set_invalid_argument_message(error_message);
            None
        }
    }
}

/// Add new columns using SQL expressions
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `names` and `expressions` must point to arrays of `num_columns` valid
///   null-terminated C strings, with `names[i]` assigned the value of `expressions[i]`
/// - `num_columns` must be greater than zero
/// - `version_out` must be a valid pointer to receive the commit version
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_add_columns(
    table: *const LanceDBTable,
    names: *const *const c_char,
    expressions: *const *const c_char,
    num_columns: usize,
    version_out: *mut u64,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null()
        || names.is_null()
        || expressions.is_null()
        || num_columns == 0
        || version_out.is_null()
    {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let mut transforms = Vec::with_capacity(num_columns);
    for i in 0..num_columns {
        let Some(name) = required_c_string(*names.add(i), error_message) else {
            return LanceDBError::InvalidArgument;
        };
        let Some(expression) = required_c_string(*expressions.add(i), error_message) else {
            return LanceDBError::InvalidArgument;
        };
        if name.trim().is_empty() || expression.trim().is_empty() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }
        transforms.push((name.to_string(), expression.to_string()));
    }

    let tbl = &(*table).inner;
    let result = get_runtime().block_on(
        tbl.base_table()
            .add_columns(NewColumnTransform::SqlExpressions(transforms), None),
    );

    match result {
        Ok(result) => {
            *version_out = result.version;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Alter column names and nullability
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `alterations` must point to an array of `num_alterations` entries
/// - Each `path` must be a valid null-terminated C string naming an existing column
/// - Each `rename`, when not NULL, must be a valid null-terminated C string
/// - Each entry must change the name or nullability
/// - `version_out` must be a valid pointer to receive the commit version
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_alter_columns(
    table: *const LanceDBTable,
    alterations: *const LanceDBColumnAlteration,
    num_alterations: usize,
    version_out: *mut u64,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || alterations.is_null() || num_alterations == 0 || version_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let mut parsed_alterations = Vec::with_capacity(num_alterations);
    for i in 0..num_alterations {
        let alteration = &*alterations.add(i);
        let Some(path) = required_c_string(alteration.path, error_message) else {
            return LanceDBError::InvalidArgument;
        };
        if path.trim().is_empty() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }

        let rename = if alteration.rename.is_null() {
            None
        } else {
            let Some(rename) = required_c_string(alteration.rename, error_message) else {
                return LanceDBError::InvalidArgument;
            };
            if rename.trim().is_empty() {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            }
            Some(rename.to_string())
        };

        let nullable = match alteration.set_nullable {
            0 => None,
            1 => Some(alteration.nullable != 0),
            _ => {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            }
        };

        if rename.is_none() && nullable.is_none() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }

        let mut column = ColumnAlteration::new(path.to_string());
        if let Some(rename) = rename {
            column = column.rename(rename);
        }
        if let Some(nullable) = nullable {
            column = column.set_nullable(nullable);
        }
        parsed_alterations.push(column);
    }

    let tbl = &(*table).inner;
    let result = get_runtime().block_on(tbl.base_table().alter_columns(&parsed_alterations));

    match result {
        Ok(result) => {
            *version_out = result.version;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Drop columns from a table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `columns` must point to an array of `num_columns` valid null-terminated C strings
/// - `num_columns` must be greater than zero and every name must be non-empty
/// - `version_out` must be a valid pointer to receive the commit version
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_drop_columns(
    table: *const LanceDBTable,
    columns: *const *const c_char,
    num_columns: usize,
    version_out: *mut u64,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || columns.is_null() || num_columns == 0 || version_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let mut column_names = Vec::with_capacity(num_columns);
    for i in 0..num_columns {
        let Some(name) = required_c_string(*columns.add(i), error_message) else {
            return LanceDBError::InvalidArgument;
        };
        if name.trim().is_empty() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }
        column_names.push(name);
    }

    let tbl = &(*table).inner;
    let result = get_runtime().block_on(tbl.base_table().drop_columns(&column_names));

    match result {
        Ok(result) => {
            *version_out = result.version;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}
