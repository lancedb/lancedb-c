// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Complete Table API implementation for LanceDB C bindings
//!
//! This module provides all table operations using Arrow-only APIs,
//! combining both simple and full table functionality.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;

use arrow_array::{Array, RecordBatch, RecordBatchReader, StructArray};
use arrow_schema::{ArrowError, Schema};
use futures::TryStreamExt;
use lance::dataset::transaction::UpdateMapEntry;
use lancedb::query::{ExecutableQuery, QueryBase};

use crate::connection::LanceDBTable;
use crate::error::{
    handle_error, set_invalid_argument_message, set_not_supported_message,
    set_unknown_error_message, LanceDBError,
};
use crate::expr::LanceDBExpr;
use crate::runtime::run_blocking;
use crate::types::{LanceDBMergeInsertConfig, LanceDBRecordBatchReader};

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

    let tbl = (*table).inner.clone();

    match run_blocking(async move { tbl.schema().await }) {
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

    let tbl = (*table).inner.clone();

    // Take ownership of the reader
    let reader = Box::from_raw(reader).into_inner();

    match run_blocking(async move { tbl.add(reader).execute().await }) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
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

    if table.is_null() || on_columns.is_null() || num_columns == 0 {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    // Extract column names
    let mut column_names = Vec::with_capacity(num_columns);
    for i in 0..num_columns {
        let col_ptr = *on_columns.add(i);
        if col_ptr.is_null() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }

        let Ok(col_str) = CStr::from_ptr(col_ptr).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };
        column_names.push(col_str);
    }

    // Extract the optional "when matched" condition, which is only used when matched
    // records are updated
    let mut update_condition_sql = None;
    let mut update_condition_expr = None;
    if !config.is_null() && (*config).when_matched_update_all != 0 {
        let cfg = &*config;
        let has_expr = !cfg.when_matched_update_all_expr.is_null();
        let has_sql = !cfg.when_matched_update_all_condition.is_null();

        // the two flavors of the condition are mutually exclusive
        if has_expr && has_sql {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }

        if has_expr {
            // the expression is borrowed, ownership stays with the caller
            update_condition_expr = Some((*cfg.when_matched_update_all_expr).inner.clone());
        } else if has_sql {
            let Ok(condition) = CStr::from_ptr(cfg.when_matched_update_all_condition).to_str()
            else {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            };
            update_condition_sql = Some(condition.to_string());
        }
    }

    let tbl = (*table).inner.clone();
    let column_names: Vec<String> = column_names.iter().map(|s| s.to_string()).collect();

    // Read the remaining configuration synchronously (raw pointers must not
    // cross onto the runtime's worker threads); default is upsert behaviour.
    let (update_all, insert_all) = if config.is_null() {
        (true, true)
    } else {
        let cfg = &*config;
        (
            cfg.when_matched_update_all != 0,
            cfg.when_not_matched_insert_all != 0,
        )
    };

    // The reader was taken over above (data_box); hand it to the task
    let data = data_box.into_inner();

    match run_blocking(async move {
        let on: Vec<&str> = column_names.iter().map(String::as_str).collect();
        let mut merge_builder = tbl.merge_insert(&on);
        if update_all {
            if let Some(expr) = update_condition_expr {
                merge_builder.when_matched_update_all_expr(expr);
            } else {
                merge_builder.when_matched_update_all(update_condition_sql);
            }
        }
        if insert_all {
            merge_builder.when_not_matched_insert_all();
        }
        merge_builder.execute(data).await
    }) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
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
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_version(table: *const LanceDBTable) -> u64 {
    if table.is_null() {
        return 0;
    }

    let tbl = (*table).inner.clone();

    run_blocking(async move { tbl.version().await }).unwrap_or(0)
}

/// Count rows in table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
///
/// # Returns
/// - Number of rows in table on success, 0 on failure (or empty table)
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_count_rows(table: *const LanceDBTable) -> u64 {
    if table.is_null() {
        return 0;
    }

    let tbl = (*table).inner.clone();

    match run_blocking(async move { tbl.count_rows(None).await }) {
        Ok(count) => count as u64,
        Err(_) => 0,
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
    if table.is_null() || predicate.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(predicate_str) = CStr::from_ptr(predicate).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let tbl = (*table).inner.clone();
    let predicate = predicate_str.to_string();

    match run_blocking(async move { tbl.delete(&predicate).await }) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
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
    if expr.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let expr_box = Box::from_raw(expr);

    if table.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }
    let tbl = (*table).inner.clone();

    match run_blocking(async move { tbl.delete(&expr_box.inner).await }) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
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

    let tbl = (*table).inner.clone();
    let vec_slice = std::slice::from_raw_parts(vector, dimension);
    let vec_data: Vec<f32> = vec_slice.to_vec();

    let column_name = if column.is_null() {
        None
    } else {
        match CStr::from_ptr(column).to_str() {
            Ok(s) => Some(s.to_string()),
            Err(_) => {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            }
        }
    };

    match run_blocking(async move {
        let mut query = tbl.query().limit(limit).nearest_to(vec_data)?;

        if let Some(col) = column_name {
            query = query.column(&col);
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
                let array_data: arrow_data::ArrayData = struct_array.into_data();
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

    let tbl = (*table).inner.clone();
    let include_metadata = !metadata_out.is_null();

    match run_blocking(async move { tbl.list_versions().await }) {
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

    let ds = match tbl.dataset() {
        Some(ds) => ds,
        None => {
            set_not_supported_message(error_message);
            return LanceDBError::NotSupported;
        }
    };

    // Parse the filter keys synchronously (raw pointers stay on this thread)
    let filter: Option<Vec<String>> = if !filter_keys.is_null() && filter_count > 0 {
        let mut keys = Vec::with_capacity(filter_count);
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
            keys.push(key_str.to_string());
        }
        Some(keys)
    } else {
        None
    };

    // The dataset read guard cannot leave the worker thread: copy the
    // matching entries out while holding it.
    let ds = ds.clone();
    let entries: Vec<(String, String)> = match run_blocking(async move {
        let guard = ds.get().await?;
        let metadata = &guard.manifest().table_metadata;
        let entries = match &filter {
            Some(keys) => keys
                .iter()
                .filter_map(|k| metadata.get(k).map(|v| (k.clone(), v.clone())))
                .collect(),
            None => metadata
                .iter()
                .map(|(k, v)| (k.clone(), v.clone()))
                .collect(),
        };
        Ok(entries)
    }) {
        Ok(entries) => entries,
        Err(e) => return handle_error(&e, error_message),
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
        let c_key = CString::new(key.as_str()).unwrap_or_default();
        let c_value = CString::new(value.as_str()).unwrap_or_default();
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

    let ds = match tbl.dataset() {
        Some(ds) => ds,
        None => {
            set_not_supported_message(error_message);
            return LanceDBError::NotSupported;
        }
    };

    let ds = ds.clone();
    match run_blocking(async move {
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

    let ds = match tbl.dataset() {
        Some(ds) => ds,
        None => {
            set_not_supported_message(error_message);
            return LanceDBError::NotSupported;
        }
    };

    let ds = ds.clone();
    match run_blocking(async move {
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
