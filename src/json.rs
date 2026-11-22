// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! JSON expression matching API
//!
//! Provides C-callable functions to build DataFusion JSON expressions and
//! evaluate them against Arrow RecordBatches containing JSON string columns.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::sync::Arc;

use arrow_array::cast::AsArray;
use arrow_array::{Array, RecordBatch, StructArray};
use arrow_schema::ffi::FFI_ArrowSchema;
use lancedb::datafusion::common::DFSchema;
use lancedb::datafusion::logical_expr::execution_props::ExecutionProps;
use lancedb::datafusion::logical_expr::expr::ScalarFunction;
use lancedb::datafusion::logical_expr::physical_planning_context::PhysicalPlanningContext;
use lancedb::datafusion::logical_expr::{lit, ColumnarValue, Expr};
use lancedb::datafusion::physical_expr::create_physical_expr;

use crate::error::{set_custom_error_message, set_invalid_argument_message, LanceDBError};
use crate::expr::LanceDBExpr;

#[repr(C)]
struct ArrowArrayRef {
    length: i64,
    null_count: i64,
    offset: i64,
    n_buffers: i64,
    n_children: i64,
    buffers: *mut *const std::ffi::c_void,
    children: *mut *mut ArrowArrayRef,
    dictionary: *mut ArrowArrayRef,
    release: Option<unsafe extern "C" fn(*mut ArrowArrayRef)>,
    private_data: *mut std::ffi::c_void,
}

// validate that ArrowArrayRef has the same layout as FFI_ArrowArray
const _: () = assert!(
    std::mem::size_of::<ArrowArrayRef>() == std::mem::size_of::<arrow_array::ffi::FFI_ArrowArray>()
);
const _: () = assert!(
    std::mem::align_of::<ArrowArrayRef>()
        == std::mem::align_of::<arrow_array::ffi::FFI_ArrowArray>()
);

unsafe fn non_owning_import(
    array_ptr: *const arrow_array::ffi::FFI_ArrowArray,
    schema_ptr: *const FFI_ArrowSchema,
) -> Result<RecordBatch, arrow_schema::ArrowError> {
    let mut raw: ArrowArrayRef = std::ptr::read(array_ptr as *const ArrowArrayRef);
    raw.release = None;
    let ffi_copy: arrow_array::ffi::FFI_ArrowArray = std::mem::transmute(raw);
    let array_data = arrow_array::ffi::from_ffi(ffi_copy, &*schema_ptr)?;
    let struct_array = StructArray::from(array_data);
    Ok(RecordBatch::from(&struct_array))
}

unsafe fn build_json_udf_expr(
    udf: Arc<lancedb::datafusion::logical_expr::ScalarUDF>,
    json_expr: *mut LanceDBExpr,
    path: *const *const c_char,
    path_len: usize,
) -> *mut LanceDBExpr {
    if json_expr.is_null() || path.is_null() || path_len == 0 {
        if !json_expr.is_null() {
            let _ = Box::from_raw(json_expr);
        }
        return std::ptr::null_mut();
    }

    // json_expr is the first argument, followed by path segments as string literals
    let mut args = Vec::with_capacity(1 + path_len);
    args.push(Box::from_raw(json_expr).inner);

    for i in 0..path_len {
        let seg = *path.add(i);
        if seg.is_null() {
            return std::ptr::null_mut();
        }
        match CStr::from_ptr(seg).to_str() {
            Ok(s) => args.push(lit(s.to_string())),
            Err(_) => return std::ptr::null_mut(),
        }
    }

    let func_expr = Expr::ScalarFunction(ScalarFunction::new_udf(udf, args));
    Box::into_raw(Box::new(LanceDBExpr { inner: func_expr }))
}

/// Create a json_get_str expression: extract a string value from JSON by path
///
/// # Safety
/// - `json_expr` must be a valid pointer returned from lancedb_expr_* functions (consumed)
/// - `path` must be a valid pointer to `path_len` null-terminated C strings
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_json_get_str(
    json_expr: *mut LanceDBExpr,
    path: *const *const c_char,
    path_len: usize,
) -> *mut LanceDBExpr {
    build_json_udf_expr(
        datafusion_functions_json::udfs::json_get_str_udf(),
        json_expr,
        path,
        path_len,
    )
}

/// Create a json_get_int expression: extract an integer value from JSON by path
///
/// # Safety
/// - `json_expr` must be a valid pointer returned from lancedb_expr_* functions (consumed)
/// - `path` must be a valid pointer to `path_len` null-terminated C strings
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_json_get_int(
    json_expr: *mut LanceDBExpr,
    path: *const *const c_char,
    path_len: usize,
) -> *mut LanceDBExpr {
    build_json_udf_expr(
        datafusion_functions_json::udfs::json_get_int_udf(),
        json_expr,
        path,
        path_len,
    )
}

/// Create a json_get_float expression: extract a float value from JSON by path
///
/// # Safety
/// - `json_expr` must be a valid pointer returned from lancedb_expr_* functions (consumed)
/// - `path` must be a valid pointer to `path_len` null-terminated C strings
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_json_get_float(
    json_expr: *mut LanceDBExpr,
    path: *const *const c_char,
    path_len: usize,
) -> *mut LanceDBExpr {
    build_json_udf_expr(
        datafusion_functions_json::udfs::json_get_float_udf(),
        json_expr,
        path,
        path_len,
    )
}

/// Create a json_get_bool expression: extract a boolean value from JSON by path
///
/// # Safety
/// - `json_expr` must be a valid pointer returned from lancedb_expr_* functions (consumed)
/// - `path` must be a valid pointer to `path_len` null-terminated C strings
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_json_get_bool(
    json_expr: *mut LanceDBExpr,
    path: *const *const c_char,
    path_len: usize,
) -> *mut LanceDBExpr {
    build_json_udf_expr(
        datafusion_functions_json::udfs::json_get_bool_udf(),
        json_expr,
        path,
        path_len,
    )
}

/// Create a json_contains expression: check if a key exists in JSON
///
/// # Safety
/// - `json_expr` must be a valid pointer returned from lancedb_expr_* functions (consumed)
/// - `path` must be a valid pointer to `path_len` null-terminated C strings
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_json_contains(
    json_expr: *mut LanceDBExpr,
    path: *const *const c_char,
    path_len: usize,
) -> *mut LanceDBExpr {
    build_json_udf_expr(
        datafusion_functions_json::udfs::json_contains_udf(),
        json_expr,
        path,
        path_len,
    )
}

/// Create a json_array_has expression: check if a JSON array contains a value
///
/// Composes `json_get_array` (extracts a JSON array as `List<Utf8>`) with
/// `array_has` to test membership. The `quote_value` flag controls how the
/// value expression is adapted to match `json_get_array` output:
/// - `true`: for JSON string arrays — value is wrapped in JSON quotes
///   (`concat('"', cast(val, Utf8), '"')`) since elements include surrounding quotes.
/// - `false`: for JSON number/boolean arrays — value is cast to Utf8
///   since elements are stored as their unquoted text (e.g. `42`, `true`).
///
/// For arrays of non-scalar values (objects, nested arrays), it uses exact string
/// matching against the raw JSON text, which is not JSON-aware.
///
/// # Safety
/// - `json_expr` must be a valid pointer returned from lancedb_expr_* functions (consumed)
/// - `path` must be a valid pointer to `path_len` null-terminated C strings
/// - `value_expr` must be a valid pointer returned from lancedb_expr_* functions (consumed)
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_json_array_has(
    json_expr: *mut LanceDBExpr,
    path: *const *const c_char,
    path_len: usize,
    value_expr: *mut LanceDBExpr,
    quote_value: bool,
) -> *mut LanceDBExpr {
    let val = if !value_expr.is_null() {
        Some(Box::from_raw(value_expr).inner)
    } else {
        None
    };

    if val.is_none() {
        if !json_expr.is_null() {
            let _ = Box::from_raw(json_expr);
        }
        return std::ptr::null_mut();
    }
    let val = val.unwrap();

    let array_expr_ptr = build_json_udf_expr(
        datafusion_functions_json::udfs::json_get_array_udf(),
        json_expr,
        path,
        path_len,
    );
    if array_expr_ptr.is_null() {
        return std::ptr::null_mut();
    }

    let array_inner = Box::from_raw(array_expr_ptr).inner;
    let udf = lancedb::datafusion::functions_nested::array_has::array_has_udf();

    let val_as_utf8 = Expr::Cast(lancedb::datafusion::logical_expr::Cast::new(
        Box::new(val),
        arrow_schema::DataType::Utf8,
    ));

    let search_val = if quote_value {
        let concat_udf = lancedb::datafusion::functions::string::concat();
        Expr::ScalarFunction(ScalarFunction::new_udf(
            concat_udf,
            vec![lit("\""), val_as_utf8, lit("\"")],
        ))
    } else {
        val_as_utf8
    };

    let func_expr =
        Expr::ScalarFunction(ScalarFunction::new_udf(udf, vec![array_inner, search_val]));
    Box::into_raw(Box::new(LanceDBExpr { inner: func_expr }))
}

/// Evaluate a JSON filter expression against Arrow RecordBatches.
///
/// Takes the Arrow FFI arrays and schema from `lancedb_query_result_to_arrow`
/// and a filter expression. Returns a boolean array indicating which rows
/// matched. The arrays are NOT
/// consumed — the caller can still use them afterward.
///
/// The caller must call this function while the arrays are valid (before
/// consuming them with `arrow::ImportRecordBatch` or freeing with
/// `lancedb_free_arrow_arrays`).
///
/// # Safety
/// - `arrays` must be a valid pointer to `batch_count` FFI_ArrowArray pointers
/// - `schema` must be a valid pointer to an FFI_ArrowSchema
/// - `expr` must be a valid pointer returned from lancedb_expr_* functions (consumed)
/// - `results_out` must be a valid pointer to receive the allocated bool array
/// - `count_out` must be a valid pointer to receive the total row count
/// - `expr` is consumed by this function; do not use or free it after calling
/// - The caller must free `*results_out` with `lancedb_free_json_matches()`
#[no_mangle]
pub unsafe extern "C" fn lancedb_json_matches(
    arrays: *const *mut arrow_array::ffi::FFI_ArrowArray,
    schema: *const FFI_ArrowSchema,
    batch_count: usize,
    expr: *mut LanceDBExpr,
    results_out: *mut *mut bool,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    let filter_expr = if !expr.is_null() {
        Some(Box::from_raw(expr).inner)
    } else {
        None
    };

    if arrays.is_null()
        || schema.is_null()
        || filter_expr.is_none()
        || results_out.is_null()
        || count_out.is_null()
        || batch_count == 0
    {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }
    let filter_expr = filter_expr.unwrap();

    let arrow_schema = match arrow_schema::Schema::try_from(&*schema) {
        Ok(s) => Arc::new(s),
        Err(e) => {
            set_custom_error_message(error_message, &format!("{e}"));
            return LanceDBError::Arrow;
        }
    };
    let df_schema = match DFSchema::try_from(arrow_schema) {
        Ok(s) => s,
        Err(e) => {
            set_custom_error_message(error_message, &format!("{e}"));
            return LanceDBError::Other;
        }
    };
    let execution_props = ExecutionProps::new();
    let physical_expr = match create_physical_expr(
        &filter_expr,
        &df_schema,
        &execution_props,
        &PhysicalPlanningContext::default(),
    ) {
        Ok(pe) => pe,
        Err(e) => {
            set_custom_error_message(error_message, &format!("{e}"));
            return LanceDBError::Other;
        }
    };

    // Count total rows from FFI array length fields
    let mut total: usize = 0;
    for i in 0..batch_count {
        let arr_ptr = *arrays.add(i);
        if arr_ptr.is_null() {
            set_custom_error_message(error_message, "Null array pointer in batch");
            return LanceDBError::InvalidArgument;
        }
        let raw = &*(arr_ptr as *const ArrowArrayRef);
        let batch_len = match usize::try_from(raw.length) {
            Ok(l) => l,
            Err(_) => {
                set_custom_error_message(error_message, "Invalid array length in batch");
                return LanceDBError::InvalidArgument;
            }
        };
        total = match total.checked_add(batch_len) {
            Some(t) => t,
            None => {
                set_custom_error_message(error_message, "Total row count overflow");
                return LanceDBError::InvalidArgument;
            }
        };
    }

    if total == 0 {
        *results_out = std::ptr::null_mut();
        *count_out = 0;
        return LanceDBError::Success;
    }

    let out = libc::malloc(total * std::mem::size_of::<bool>()) as *mut bool;
    if out.is_null() {
        set_custom_error_message(error_message, "Failed to allocate results array");
        return LanceDBError::Other;
    }

    let mut offset: usize = 0;
    for i in 0..batch_count {
        let batch = match non_owning_import(*arrays.add(i), schema) {
            Ok(b) => b,
            Err(e) => {
                libc::free(out as *mut libc::c_void);
                set_custom_error_message(error_message, &format!("{e}"));
                return LanceDBError::Arrow;
            }
        };
        let result = match physical_expr.evaluate(&batch) {
            Ok(r) => r,
            Err(e) => {
                libc::free(out as *mut libc::c_void);
                set_custom_error_message(error_message, &format!("{e}"));
                return LanceDBError::Other;
            }
        };
        let result_array = match result {
            ColumnarValue::Array(arr) => arr,
            ColumnarValue::Scalar(sv) => match sv.to_array_of_size(batch.num_rows()) {
                Ok(arr) => arr,
                Err(e) => {
                    libc::free(out as *mut libc::c_void);
                    set_custom_error_message(error_message, &format!("{e}"));
                    return LanceDBError::Other;
                }
            },
        };
        if *result_array.data_type() != arrow_schema::DataType::Boolean {
            libc::free(out as *mut libc::c_void);
            set_custom_error_message(
                error_message,
                &format!(
                    "Expression must produce Boolean, got {:?}",
                    result_array.data_type()
                ),
            );
            return LanceDBError::InvalidArgument;
        }
        let bool_array = result_array.as_boolean();
        for j in 0..bool_array.len() {
            *out.add(offset + j) = bool_array.is_valid(j) && bool_array.value(j);
        }
        offset += bool_array.len();
    }

    *results_out = out;
    *count_out = total;
    LanceDBError::Success
}

/// Free the results array returned by `lancedb_json_matches`.
///
/// # Safety
/// - `results` must be a pointer returned by `lancedb_json_matches`, or NULL.
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_json_matches(results: *mut bool) {
    if !results.is_null() {
        libc::free(results as *mut libc::c_void);
    }
}
