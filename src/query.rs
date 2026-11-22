// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Full Query API implementation for LanceDB C bindings
//!
//! This module provides complete query operations with proper Arrow integration

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_float, c_int};
use std::ptr;
use std::sync::Arc;

use arrow_array::ffi::FFI_ArrowArray;
use arrow_array::{Array, RecordBatch, StructArray};
use arrow_schema::ffi::FFI_ArrowSchema;
use futures::TryStreamExt;

use lancedb::arrow::arrow_data::ArrayData;
use lancedb::arrow::SendableRecordBatchStream;
use lancedb::datafusion::logical_expr::Expr;
use lancedb::index::scalar::FullTextSearchQuery;
use lancedb::query::{
    ExecutableQuery, HasQuery, Query, QueryBase, QueryFilter, Select, VectorQuery,
};
use lancedb::rerankers::rrf::RRFReranker;
use lancedb::rerankers::NormalizeMethod;
use lancedb::{DistanceType, Table};

use crate::connection::{get_runtime, LanceDBTable};
use crate::error::{
    clear_error_message, handle_error, set_custom_error_message, set_invalid_argument_message,
    set_unknown_error_message, LanceDBError,
};
use crate::expr::LanceDBExpr;
use crate::ipc::{self, LanceDBIpcBuffer};
use crate::types::{LanceDBDistanceType, LanceDBNormalizeMethod};

/// Opaque handle to a LanceDB Query
#[repr(C)]
pub struct LanceDBQuery {
    table: Arc<Table>,
    limit: Option<usize>,
    offset: Option<usize>,
    select: Option<Select>,
    with_row_id: bool,
    fast_search: bool,
    filter: Option<String>,
    df_filter: Option<Expr>,
    full_text_search: Option<FullTextSearchQuery>,
}

/// Opaque handle to a LanceDB VectorQuery
#[repr(C)]
pub struct LanceDBVectorQuery {
    table: Arc<Table>,
    query_vector: Vec<f32>,
    column: Option<String>,
    limit: Option<usize>,
    offset: Option<usize>,
    select: Option<Select>,
    with_row_id: bool,
    fast_search: bool,
    bypass_vector_index: bool,
    rrf_k: Option<f32>,
    norm: Option<NormalizeMethod>,
    filter: Option<String>,
    df_filter: Option<Expr>,
    full_text_search: Option<FullTextSearchQuery>,
    postfilter: bool,
    distance_type: Option<DistanceType>,
    nprobes: Option<usize>,
    refine_factor: Option<u32>,
    ef: Option<usize>,
}

/// Query result handle for streaming results
#[repr(C)]
pub struct LanceDBQueryResult {
    /// The SDK stream, kept whole so that the schema of a projected result and
    /// the error text of a stream that fails after its first batch both survive
    /// until the result is consumed
    inner: SendableRecordBatchStream,
}

/// Create a new query for the given table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
///
/// # Returns
/// - Non-null pointer to LanceDBQuery on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_new(table: *const LanceDBTable) -> *mut LanceDBQuery {
    if table.is_null() {
        return ptr::null_mut();
    }

    let tbl = &(*table).inner;
    let query = Box::new(LanceDBQuery {
        table: Arc::new(tbl.clone()),
        limit: None,
        offset: None,
        select: None,
        with_row_id: false,
        fast_search: false,
        filter: None,
        df_filter: None,
        full_text_search: None,
    });

    Box::into_raw(query)
}

/// Create a vector query from table with query vector
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `vector` must be a valid pointer to array of floats
/// - `dimension` must match the actual dimension of the vector column
///
/// # Returns
/// - Non-null pointer to LanceDBVectorQuery on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_new(
    table: *const LanceDBTable,
    vector: *const c_float,
    dimension: usize,
) -> *mut LanceDBVectorQuery {
    if table.is_null() || vector.is_null() || dimension == 0 {
        return ptr::null_mut();
    }

    let tbl = &(*table).inner;
    let vec_slice = std::slice::from_raw_parts(vector, dimension);
    let vec_data: Vec<f32> = vec_slice.to_vec();

    let vector_query = Box::new(LanceDBVectorQuery {
        table: Arc::new(tbl.clone()),
        query_vector: vec_data,
        column: None,
        limit: None,
        offset: None,
        select: None,
        with_row_id: false,
        fast_search: false,
        bypass_vector_index: false,
        rrf_k: None,
        norm: None,
        filter: None,
        df_filter: None,
        full_text_search: None,
        postfilter: false,
        distance_type: None,
        nprobes: None,
        refine_factor: None,
        ef: None,
    });

    Box::into_raw(vector_query)
}

/// Build a full-text search from the query text and optional text column
///
/// # Safety
/// - `text` must be a valid null-terminated UTF-8 string
/// - `column` may be NULL or a valid null-terminated UTF-8 string. An empty
///   column leaves the choice to LanceDB, which uses the indexed text column.
unsafe fn full_text_search_from_c(
    text: *const c_char,
    column: *const c_char,
) -> Result<FullTextSearchQuery, String> {
    if text.is_null() {
        return Err("full-text search text is required".to_string());
    }

    let text = CStr::from_ptr(text)
        .to_str()
        .map_err(|_| "full-text search text is not valid UTF-8".to_string())?;
    let mut search = FullTextSearchQuery::new(text.to_string());

    if !column.is_null() {
        let column = CStr::from_ptr(column)
            .to_str()
            .map_err(|_| "full-text search column is not valid UTF-8".to_string())?;
        if !column.is_empty() {
            search = search
                .with_column(column.to_string())
                .map_err(|error| format!("Invalid FTS column: {error}"))?;
        }
    }

    Ok(search)
}

/// Read parallel C arrays into the SDK's ordered dynamic projection pairs.
///
/// The first array names each output column and the second holds its SQL
/// expression. Both arrays and every entry must be non-NULL; a zero count
/// describes an empty projection.
unsafe fn dynamic_projection_from_c(
    names: *const *const c_char,
    expressions: *const *const c_char,
    num_columns: usize,
) -> Result<Select, ()> {
    if names.is_null() || expressions.is_null() {
        return Err(());
    }

    let mut projection = Vec::with_capacity(num_columns);
    for index in 0..num_columns {
        let name = *names.add(index);
        let expression = *expressions.add(index);
        if name.is_null() || expression.is_null() {
            return Err(());
        }

        let name = CStr::from_ptr(name).to_str().map_err(|_| ())?.to_string();
        let expression = CStr::from_ptr(expression)
            .to_str()
            .map_err(|_| ())?
            .to_string();
        projection.push((name, expression));
    }

    Ok(Select::Dynamic(projection))
}

/// Set limit for query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_limit(
    query: *mut LanceDBQuery,
    limit: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).limit = Some(limit);
    LanceDBError::Success
}

/// Set offset for query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_offset(
    query: *mut LanceDBQuery,
    offset: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).offset = Some(offset);
    LanceDBError::Success
}

/// Set columns to select
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `columns` must be an array of valid null-terminated C strings
/// - `num_columns` must match the actual number of strings in the array
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_select(
    query: *mut LanceDBQuery,
    columns: *const *const c_char,
    num_columns: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() || columns.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let mut column_names = Vec::with_capacity(num_columns);
    for i in 0..num_columns {
        let col_ptr = *columns.add(i);
        if col_ptr.is_null() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }

        let col_str = match CStr::from_ptr(col_ptr).to_str() {
            Ok(s) => s.to_string(),
            Err(_) => {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            }
        };
        column_names.push(col_str);
    }

    (*query).select = Some(Select::columns(
        &column_names.iter().map(|s| s.as_str()).collect::<Vec<_>>(),
    ));
    LanceDBError::Success
}

/// Reset a plain query to every non-system column
///
/// This clears a projection set by `lancedb_query_select` or
/// `lancedb_query_select_dynamic`, including when the query is executed
/// later. `lancedb_query_with_row_id` still appends the metadata column.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_select_all(
    query: *mut LanceDBQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function

    // `None` is the same SDK default as Select::All and keeps the builder from
    // sending a redundant selection.
    (*query).select = None;
    LanceDBError::Success
}

/// Set an expression-based column projection for query
///
/// Each pair adds one output column: `names[i]` is the output name and
/// `expressions[i]` is the SQL expression that computes it. Columns are
/// returned in the given order, which may differ from the table order. The
/// names and expressions are copied. A zero count selects no columns; both
/// arrays must still be non-NULL.
///
/// This replaces a projection set by `lancedb_query_select` or
/// `lancedb_query_select_all`. A later call to either function replaces this
/// projection. The SDK decides whether a backend supports dynamic projection
/// and reports its own error unchanged.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `names` and `expressions` must each point to `num_columns` valid
///   NUL-terminated UTF-8 strings
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_select_dynamic(
    query: *mut LanceDBQuery,
    names: *const *const c_char,
    expressions: *const *const c_char,
    num_columns: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match dynamic_projection_from_c(names, expressions, num_columns) {
        Ok(select) => {
            (*query).select = Some(select);
            LanceDBError::Success
        }
        Err(()) => {
            set_invalid_argument_message(error_message);
            LanceDBError::InvalidArgument
        }
    }
}

/// Ask a plain query to return the `_rowid` metadata column
///
/// This applies to ordinary scans and standalone full-text searches built with
/// `lancedb_query_full_text_search`. The metadata column is appended to the
/// projection.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_with_row_id(
    query: *mut LanceDBQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).with_row_id = true;
    LanceDBError::Success
}

/// Restrict a plain query to data covered by an index
///
/// For an ordinary scan this applies only when the filter plan uses a scalar
/// index; an unfiltered scan, or a filter without an index, still reads every
/// selected fragment. A standalone full-text search reads only indexed rows.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_fast_search(
    query: *mut LanceDBQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).fast_search = true;
    LanceDBError::Success
}

/// Set WHERE filter for query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `filter` must be a valid null-terminated C string containing SQL WHERE clause
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_where_filter(
    query: *mut LanceDBQuery,
    filter: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() || filter.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let filter_str = match CStr::from_ptr(filter).to_str() {
        Ok(s) => s.to_string(),
        Err(_) => {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }
    };

    (*query).filter = Some(filter_str);
    LanceDBError::Success
}

/// Set full-text search for query
///
/// LanceDB applies the filter and projection in the same query and adds its
/// score column. If no limit is set, LanceDB's default top-K applies. A NULL
/// or empty column uses automatic discovery; an explicit text column can also
/// be scanned without an index by embedded LanceDB.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `text` must be a valid null-terminated UTF-8 string
/// - `column` may be NULL or an empty string for automatic discovery, or a
///   valid null-terminated UTF-8 string naming the text column
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_full_text_search(
    query: *mut LanceDBQuery,
    text: *const c_char,
    column: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match full_text_search_from_c(text, column) {
        Ok(search) => {
            (*query).full_text_search = Some(search);
            LanceDBError::Success
        }
        Err(message) => {
            set_custom_error_message(error_message, &message);
            LanceDBError::InvalidArgument
        }
    }
}

/// Set DataFusion Expr filter for query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `expr` must be a valid pointer returned from `lancedb_expr_*` functions
/// - `expr` is consumed by this function; do not use or free it after calling
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_df_filter(
    query: *mut LanceDBQuery,
    expr: *mut LanceDBExpr,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() || expr.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let expr_box = Box::from_raw(expr);
    (*query).df_filter = Some(expr_box.inner);
    LanceDBError::Success
}

/// Set limit for vector query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_limit(
    query: *mut LanceDBVectorQuery,
    limit: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).limit = Some(limit);
    LanceDBError::Success
}

/// Set offset for vector query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_offset(
    query: *mut LanceDBVectorQuery,
    offset: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).offset = Some(offset);
    LanceDBError::Success
}

/// Set vector column for vector query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `column` must be a valid null-terminated C string containing the column name
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_column(
    query: *mut LanceDBVectorQuery,
    column: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() || column.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let column_str = match CStr::from_ptr(column).to_str() {
        Ok(s) => s.to_string(),
        Err(_) => {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }
    };

    (*query).column = Some(column_str);
    LanceDBError::Success
}

/// Set columns to select for vector query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `columns` must be an array of valid null-terminated C strings
/// - `num_columns` must match the actual number of strings in the array
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_select(
    query: *mut LanceDBVectorQuery,
    columns: *const *const c_char,
    num_columns: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() || columns.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let mut column_names = Vec::with_capacity(num_columns);
    for i in 0..num_columns {
        let col_ptr = *columns.add(i);
        if col_ptr.is_null() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }

        let col_str = match CStr::from_ptr(col_ptr).to_str() {
            Ok(s) => s.to_string(),
            Err(_) => {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            }
        };
        column_names.push(col_str);
    }

    (*query).select = Some(Select::columns(
        &column_names.iter().map(|s| s.as_str()).collect::<Vec<_>>(),
    ));
    LanceDBError::Success
}

/// Reset a vector query to every non-system column
///
/// This clears a projection set by `lancedb_vector_query_select` or
/// `lancedb_vector_query_select_dynamic`, including when the query is
/// executed later. `lancedb_vector_query_with_row_id` still appends the
/// metadata column.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_select_all(
    query: *mut LanceDBVectorQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function

    // `None` is the same SDK default as Select::All and keeps the builder from
    // sending a redundant selection.
    (*query).select = None;
    LanceDBError::Success
}

/// Set an expression-based column projection for vector query
///
/// Each pair adds one output column: `names[i]` is the output name and
/// `expressions[i]` is the SQL expression that computes it. Columns are
/// returned in the given order, which may differ from the table order. The
/// names and expressions are copied. A zero count selects no columns; both
/// arrays must still be non-NULL.
///
/// This replaces a projection set by `lancedb_vector_query_select` or
/// `lancedb_vector_query_select_all`. A later call to either function replaces
/// this projection. The SDK decides whether a backend supports dynamic
/// projection and reports its own error unchanged.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `names` and `expressions` must each point to `num_columns` valid
///   NUL-terminated UTF-8 strings
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_select_dynamic(
    query: *mut LanceDBVectorQuery,
    names: *const *const c_char,
    expressions: *const *const c_char,
    num_columns: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match dynamic_projection_from_c(names, expressions, num_columns) {
        Ok(select) => {
            (*query).select = Some(select);
            LanceDBError::Success
        }
        Err(()) => {
            set_invalid_argument_message(error_message);
            LanceDBError::InvalidArgument
        }
    }
}

/// Ask a vector query to return the `_rowid` metadata column
///
/// This also applies to a hybrid vector plus full-text query. The metadata
/// column is appended to the projection.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_with_row_id(
    query: *mut LanceDBVectorQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).with_row_id = true;
    LanceDBError::Success
}

/// Restrict a vector query to rows covered by the vector index
///
/// The SDK turns this into an index-only search: rows appended or updated
/// after the index was built may be absent, and a query with no usable vector
/// index returns no rows.
///
/// On a hybrid query this flag is part of the shared request, so it applies to
/// both the full-text and vector legs. `lancedb_vector_query_bypass_vector_index`
/// only affects the vector leg. Embedded LanceDB applies fast search after the
/// bypass flag, so fast search wins when both are set. A remote LanceDB forwards
/// both flags to the server, which decides their interaction.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_fast_search(
    query: *mut LanceDBVectorQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).fast_search = true;
    LanceDBError::Success
}

/// Skip the vector index and compare the query vector with every row
///
/// This performs an exhaustive flat search even when a vector index exists.
/// It is useful as ground truth for recall measurements. A filter still keeps
/// its configured pre-filter or post-filter placement.
///
/// On a hybrid query this only affects the vector leg. Embedded LanceDB applies
/// fast search after this flag, so a query with both set behaves as a fast
/// search. A remote LanceDB forwards both flags to the server.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_bypass_vector_index(
    query: *mut LanceDBVectorQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).bypass_vector_index = true;
    LanceDBError::Success
}

/// Set WHERE filter for vector query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `filter` must be a valid null-terminated C string containing SQL WHERE clause
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_where_filter(
    query: *mut LanceDBVectorQuery,
    filter: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() || filter.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let filter_str = match CStr::from_ptr(filter).to_str() {
        Ok(s) => s.to_string(),
        Err(_) => {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }
    };

    (*query).filter = Some(filter_str);
    LanceDBError::Success
}

/// Set full-text search for vector query
///
/// This turns the vector query into a hybrid nearest-neighbour plus full-text
/// search. Embedded LanceDB executes it as two searches and fuses the result
/// sets. A NULL or empty column uses automatic discovery; an explicit text
/// column can also be scanned without an index by embedded LanceDB. EXPLAIN,
/// output schema inspection, and ANALYZE need a single SDK plan and may return
/// the SDK's unsupported-shape error for this hybrid; execute the query instead.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `text` must be a valid null-terminated UTF-8 string
/// - `column` may be NULL or an empty string for automatic discovery, or a
///   valid null-terminated UTF-8 string naming the text column
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_full_text_search(
    query: *mut LanceDBVectorQuery,
    text: *const c_char,
    column: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match full_text_search_from_c(text, column) {
        Ok(search) => {
            (*query).full_text_search = Some(search);
            LanceDBError::Success
        }
        Err(message) => {
            set_custom_error_message(error_message, &message);
            LanceDBError::InvalidArgument
        }
    }
}

/// Set DataFusion Expr filter for vector query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `expr` must be a valid pointer returned from `lancedb_expr_*` functions
/// - `expr` is consumed by this function; do not use or free it after calling
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_df_filter(
    query: *mut LanceDBVectorQuery,
    expr: *mut LanceDBExpr,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() || expr.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let expr_box = Box::from_raw(expr);
    (*query).df_filter = Some(expr_box.inner);
    LanceDBError::Success
}

/// Make the vector query filter run after the vector search
///
/// This only affects queries that also set a filter. Post-filtering can
/// return fewer than `limit` rows when the nearest vectors do not match the
/// filter; a higher refine factor can recover some results.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_postfilter(
    query: *mut LanceDBVectorQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).postfilter = true;
    LanceDBError::Success
}

/// Use reciprocal rank fusion with an explicit `k` for hybrid search
///
/// This installs the SDK's `RRFReranker` on the vector query. It only changes
/// the result of a hybrid query built with
/// `lancedb_vector_query_full_text_search`; on a vector-only query the
/// reranker is stored but unused. Fusion scores each result as
/// `1 / (k + rank)` with ranks starting at 1, summed across the vector and
/// full-text lists. When this setter is not called, the SDK uses its own
/// default `k = 60`. A caller that treats zero as "use the default" must pass
/// 60 here instead, because the value is forwarded unchanged.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_rerank_rrf(
    query: *mut LanceDBVectorQuery,
    k: c_float,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).rrf_k = Some(k);
    LanceDBError::Success
}

/// Set how hybrid-search scores are normalized before reranking
///
/// This only affects a hybrid query built with
/// `lancedb_vector_query_full_text_search`. `LANCEDB_NORMALIZE_SCORE`
/// normalizes the raw scores and `LANCEDB_NORMALIZE_RANK` converts them to
/// ranks first. The SDK always normalizes the vector distance and full-text
/// score columns before invoking the reranker; this setter selects whether
/// that normalization starts from the scores or from their ranks.
///
/// The built-in RRF reranker uses only row identities and list positions, so
/// this setting does not change its ranking or `_relevance_score`. It does
/// change the normalized score columns preserved in the result. When this
/// setter is not called, the SDK's default normalization applies. An invalid
/// value is rejected before assignment, leaving any previous selection intact.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_norm(
    query: *mut LanceDBVectorQuery,
    normalize_method: c_int,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(normalize_method) = LanceDBNormalizeMethod::from_c_int(normalize_method) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    (*query).norm = Some(normalize_method.into());
    LanceDBError::Success
}

/// Set distance type for vector query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_distance_type(
    query: *mut LanceDBVectorQuery,
    distance_type: c_int,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(distance_type) = LanceDBDistanceType::from_c_int(distance_type) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    (*query).distance_type = Some(distance_type.into());
    LanceDBError::Success
}

/// Set number of probes for vector query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_nprobes(
    query: *mut LanceDBVectorQuery,
    nprobes: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).nprobes = Some(nprobes);
    LanceDBError::Success
}

/// Set refine factor for vector query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_refine_factor(
    query: *mut LanceDBVectorQuery,
    refine_factor: u32,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).refine_factor = Some(refine_factor);
    LanceDBError::Success
}

/// Set ef parameter for HNSW vector query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_ef(
    query: *mut LanceDBVectorQuery,
    ef: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let _ = error_message; // No errors possible in this function
    (*query).ef = Some(ef);
    LanceDBError::Success
}

/// Build the SDK query from every setting held by a plain C query handle.
///
/// Execution, plan, schema and analysis all use this path so they observe the
/// same projection, predicate and query options.
fn configured_plain_query(query: &LanceDBQuery) -> Query {
    let mut rust_query = query.table.query();

    if let Some(limit) = query.limit {
        rust_query = rust_query.limit(limit);
    }
    if let Some(offset) = query.offset {
        rust_query = rust_query.offset(offset);
    }
    if let Some(ref select) = query.select {
        rust_query = rust_query.select(select.clone());
    }
    if query.with_row_id {
        rust_query = rust_query.with_row_id();
    }
    if query.fast_search {
        rust_query = rust_query.fast_search();
    }
    if let Some(ref df_filter) = query.df_filter {
        rust_query.mut_query().filter = Some(QueryFilter::Datafusion(df_filter.clone()));
    } else if let Some(ref filter) = query.filter {
        rust_query = rust_query.only_if(filter);
    }
    if let Some(ref full_text_search) = query.full_text_search {
        rust_query = rust_query.full_text_search(full_text_search.clone());
    }

    rust_query
}

/// Build the SDK vector query from every setting held by its C handle.
///
/// Execution, plan, schema and analysis all use this path so they observe the
/// same vector, projection, predicate and tuning options.
fn configured_vector_query(
    query: &LanceDBVectorQuery,
) -> Result<VectorQuery, lancedb::error::Error> {
    let mut rust_query = query.table.query().nearest_to(query.query_vector.clone())?;

    if let Some(ref column) = query.column {
        rust_query = rust_query.column(column);
    }
    if let Some(limit) = query.limit {
        rust_query = rust_query.limit(limit);
    }
    if let Some(offset) = query.offset {
        rust_query = rust_query.offset(offset);
    }
    if let Some(ref select) = query.select {
        rust_query = rust_query.select(select.clone());
    }
    if query.with_row_id {
        rust_query = rust_query.with_row_id();
    }
    if query.fast_search {
        rust_query = rust_query.fast_search();
    }
    if query.bypass_vector_index {
        rust_query = rust_query.bypass_vector_index();
    }
    if let Some(ref df_filter) = query.df_filter {
        rust_query.mut_query().filter = Some(QueryFilter::Datafusion(df_filter.clone()));
    } else if let Some(ref filter) = query.filter {
        rust_query = rust_query.only_if(filter);
    }
    if let Some(ref full_text_search) = query.full_text_search {
        rust_query = rust_query.full_text_search(full_text_search.clone());
    }
    if let Some(distance_type) = query.distance_type {
        rust_query = rust_query.distance_type(distance_type);
    }
    if let Some(nprobes) = query.nprobes {
        rust_query = rust_query.nprobes(nprobes)?;
    }
    if let Some(refine_factor) = query.refine_factor {
        rust_query = rust_query.refine_factor(refine_factor);
    }
    if let Some(ef) = query.ef {
        rust_query = rust_query.ef(ef);
    }
    if query.postfilter {
        rust_query = rust_query.postfilter();
    }
    if let Some(k) = query.rrf_k {
        rust_query = rust_query.rerank(Arc::new(RRFReranker::new(k)));
    }
    if let Some(norm) = query.norm.clone() {
        rust_query = rust_query.norm(norm);
    }

    Ok(rust_query)
}

/// Hand an SDK plan string to C after validating that it has no NUL byte.
unsafe fn publish_plan(
    plan: String,
    plan_out: *mut *mut c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    match CString::new(plan) {
        Ok(plan) => {
            *plan_out = plan.into_raw();
            LanceDBError::Success
        }
        Err(_) => {
            set_unknown_error_message(error_message);
            LanceDBError::Unknown
        }
    }
}

/// Hand an owned Arrow schema to C, reporting a C Data conversion failure.
unsafe fn publish_output_schema(
    schema: &arrow_schema::Schema,
    schema_out: *mut *mut FFI_ArrowSchema,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    match FFI_ArrowSchema::try_from(schema) {
        Ok(ffi_schema) => {
            *schema_out = Box::into_raw(Box::new(ffi_schema));
            LanceDBError::Success
        }
        Err(error) => {
            set_custom_error_message(
                error_message,
                &format!("Failed to convert Arrow schema to C ABI: {error}"),
            );
            LanceDBError::Arrow
        }
    }
}

/// Get the query execution plan without executing the query
///
/// Returns a human-readable string describing what steps will be taken
/// to execute the query. Does NOT consume the query — it can still be
/// executed afterwards.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `plan_out` must be a valid pointer to receive the plan string
/// - `error_message` can be NULL to ignore detailed error messages
/// - Caller must free the returned plan string with `lancedb_free_string()`
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_explain_plan(
    query: *const LanceDBQuery,
    verbose: bool,
    plan_out: *mut *mut c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() || plan_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let query_ref = &*query;
    let runtime = get_runtime();

    match runtime.block_on(configured_plain_query(query_ref).explain_plan(verbose)) {
        Ok(plan) => publish_plan(plan, plan_out, error_message),
        Err(error) => handle_error(&error, error_message),
    }
}

/// Get the vector query execution plan without executing the query
///
/// Returns a human-readable string describing what steps will be taken
/// to execute the query. Does NOT consume the query — it can still be
/// executed afterwards.
///
/// Embedded LanceDB executes a hybrid query that combines nearest-neighbour and
/// full-text search as two searches and fuses their results. EXPLAIN requires a
/// single SDK plan, so embedded LanceDB rejects that hybrid at plan time with
/// the SDK's unsupported-shape error. That is an SDK limitation, not a C-side
/// argument error; execute the query instead.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `plan_out` must be a valid pointer to receive the plan string
/// - `error_message` can be NULL to ignore detailed error messages
/// - Caller must free the returned plan string with `lancedb_free_string()`
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_explain_plan(
    query: *const LanceDBVectorQuery,
    verbose: bool,
    plan_out: *mut *mut c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() || plan_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let query_ref = &*query;
    let runtime = get_runtime();
    let rust_query = match configured_vector_query(query_ref) {
        Ok(rust_query) => rust_query,
        Err(error) => return handle_error(&error, error_message),
    };

    match runtime.block_on(rust_query.explain_plan(verbose)) {
        Ok(plan) => publish_plan(plan, plan_out, error_message),
        Err(error) => handle_error(&error, error_message),
    }
}

/// Return the schema the query would produce without returning rows
///
/// The SDK derives the schema from the same configured query used by
/// `lancedb_query_execute`, so a dynamic projection's names, types and order
/// are visible before execution. The query is not consumed. The SDK decides
/// how a backend resolves the plan; an Enterprise table may send the SDK's
/// `/query/` request and read the schema from the returned stream.
///
/// On entry, `*schema_out` and the error message are cleared. On success,
/// `*schema_out` holds an owned Arrow C Data schema that the caller releases
/// with `lancedb_free_arrow_schema()`. On failure it stays NULL.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `schema_out` must be a valid pointer to receive the schema
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_output_schema(
    query: *const LanceDBQuery,
    schema_out: *mut *mut FFI_ArrowSchema,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !schema_out.is_null() {
        *schema_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if query.is_null() || schema_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match get_runtime().block_on(configured_plain_query(&*query).output_schema()) {
        Ok(schema) => publish_output_schema(schema.as_ref(), schema_out, error_message),
        Err(error) => handle_error(&error, error_message),
    }
}

/// Return the schema a vector query would produce without returning rows
///
/// The SDK derives the schema from the same configured query used by
/// `lancedb_vector_query_execute`, including a dynamic projection's names,
/// types and order. The query is not consumed. The SDK decides how a backend
/// resolves the plan; an Enterprise table may send the SDK's `/query/` request
/// and read the schema from the returned stream.
///
/// Embedded LanceDB executes a hybrid nearest-neighbour plus full-text query as
/// two searches and fuses their results. Output schema inspection requires a
/// single SDK plan, so it may return the same unsupported hybrid error as
/// `lancedb_vector_query_explain_plan`.
///
/// On entry, `*schema_out` and the error message are cleared. On success,
/// `*schema_out` holds an owned Arrow C Data schema that the caller releases
/// with `lancedb_free_arrow_schema()`. On failure it stays NULL.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `schema_out` must be a valid pointer to receive the schema
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_output_schema(
    query: *const LanceDBVectorQuery,
    schema_out: *mut *mut FFI_ArrowSchema,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !schema_out.is_null() {
        *schema_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if query.is_null() || schema_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let rust_query = match configured_vector_query(&*query) {
        Ok(rust_query) => rust_query,
        Err(error) => return handle_error(&error, error_message),
    };

    match get_runtime().block_on(rust_query.output_schema()) {
        Ok(schema) => publish_output_schema(schema.as_ref(), schema_out, error_message),
        Err(error) => handle_error(&error, error_message),
    }
}

/// Execute the query and return an analyzed plan with runtime metrics
///
/// ANALYZE executes an analyzable query plan to collect runtime metrics. It
/// reads the same data and has the same side effects as execution while
/// returning no rows. The query handle is not consumed and can still be
/// executed.
///
/// On entry, `*plan_out` and the error message are cleared. On success,
/// `*plan_out` holds a NUL-terminated string that the caller releases with
/// `lancedb_free_string()`. On failure it stays NULL.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `plan_out` must be a valid pointer to receive the plan string
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_analyze_plan(
    query: *const LanceDBQuery,
    plan_out: *mut *mut c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !plan_out.is_null() {
        *plan_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if query.is_null() || plan_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match get_runtime().block_on(configured_plain_query(&*query).analyze_plan()) {
        Ok(plan) => publish_plan(plan, plan_out, error_message),
        Err(error) => handle_error(&error, error_message),
    }
}

/// Execute the vector query and return an analyzed plan with runtime metrics
///
/// ANALYZE executes an analyzable query plan to collect runtime metrics. It
/// reads the same data and has the same side effects as execution while
/// returning no rows. The query handle is not consumed and can still be
/// executed.
///
/// Embedded LanceDB executes a hybrid nearest-neighbour plus full-text query as
/// two searches and fuses their results. ANALYZE requires one analyzable SDK
/// plan, so it may return the same unsupported hybrid error as
/// `lancedb_vector_query_explain_plan`.
///
/// On entry, `*plan_out` and the error message are cleared. On success,
/// `*plan_out` holds a NUL-terminated string that the caller releases with
/// `lancedb_free_string()`. On failure it stays NULL.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `plan_out` must be a valid pointer to receive the plan string
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_analyze_plan(
    query: *const LanceDBVectorQuery,
    plan_out: *mut *mut c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !plan_out.is_null() {
        *plan_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if query.is_null() || plan_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let rust_query = match configured_vector_query(&*query) {
        Ok(rust_query) => rust_query,
        Err(error) => return handle_error(&error, error_message),
    };

    match get_runtime().block_on(rust_query.analyze_plan()) {
        Ok(plan) => publish_plan(plan, plan_out, error_message),
        Err(error) => handle_error(&error, error_message),
    }
}

/// Run the plain query held by `query` and return the stream the SDK started
fn run_plain_query(
    query: Box<LanceDBQuery>,
) -> Result<SendableRecordBatchStream, lancedb::error::Error> {
    get_runtime().block_on(async move { configured_plain_query(query.as_ref()).execute().await })
}

/// Run the vector query held by `query` and return the stream the SDK started
fn run_vector_query(
    query: Box<LanceDBVectorQuery>,
) -> Result<SendableRecordBatchStream, lancedb::error::Error> {
    get_runtime().block_on(async move {
        let rust_query = configured_vector_query(query.as_ref())?;
        rust_query.execute().await
    })
}

/// Wrap a stream the SDK started in the result handle exposed to C
pub(crate) fn query_result_from_stream(
    stream: SendableRecordBatchStream,
) -> *mut LanceDBQueryResult {
    Box::into_raw(Box::new(LanceDBQueryResult { inner: stream }))
}

/// Execute query and return streaming result
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - This function consumes the query pointer; do not use it after calling
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_execute(
    query: *mut LanceDBQuery,
) -> *mut LanceDBQueryResult {
    if query.is_null() {
        return ptr::null_mut();
    }

    match run_plain_query(Box::from_raw(query)) {
        Ok(stream) => query_result_from_stream(stream),
        Err(_) => ptr::null_mut(),
    }
}

/// Execute query and report why the SDK could not start it
///
/// # Safety
/// - `query` must be NULL or a valid pointer returned from `lancedb_query_new`.
///   A non-NULL pointer is consumed, also when the call fails.
/// - `result_out` must be a valid pointer to receive the result handle
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*result_out` holding the result, or the SDK error
///   code with the SDK error text in `*error_message`
///
/// On entry, `*result_out` and `*error_message` are cleared. Free a returned
/// result with `lancedb_query_result_free()` and a returned error message with
/// `lancedb_free_string()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_execute_with_status(
    query: *mut LanceDBQuery,
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
    // The builder is consumed on every path, like in lancedb_table_merge_insert
    let query = Box::from_raw(query);
    if result_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match run_plain_query(query) {
        Ok(stream) => {
            *result_out = query_result_from_stream(stream);
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Execute vector query and return streaming result
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - This function consumes the query pointer; do not use it after calling
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_execute(
    query: *mut LanceDBVectorQuery,
) -> *mut LanceDBQueryResult {
    if query.is_null() {
        return ptr::null_mut();
    }

    match run_vector_query(Box::from_raw(query)) {
        Ok(stream) => query_result_from_stream(stream),
        Err(_) => ptr::null_mut(),
    }
}

/// Execute vector query and report why the SDK could not start it
///
/// # Safety
/// - `query` must be NULL or a valid pointer returned from `lancedb_vector_query_new`.
///   A non-NULL pointer is consumed, also when the call fails.
/// - `result_out` must be a valid pointer to receive the result handle
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*result_out` holding the result, or the SDK error
///   code with the SDK error text in `*error_message`
///
/// On entry, `*result_out` and `*error_message` are cleared. Free a returned
/// result with `lancedb_query_result_free()` and a returned error message with
/// `lancedb_free_string()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_execute_with_status(
    query: *mut LanceDBVectorQuery,
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
    // The builder is consumed on every path, like in lancedb_table_merge_insert
    let query = Box::from_raw(query);
    if result_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match run_vector_query(query) {
        Ok(stream) => {
            *result_out = query_result_from_stream(stream);
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Convert query result to Arrow RecordBatch array
///
/// # Safety
/// - `result` must be NULL or a valid pointer returned from query execution
///   functions. When `result`, `batches_out`, `schema_out` and `count_out` are
///   all non-NULL, the result is consumed on every path, including a stream or
///   schema conversion failure. When any of the four is NULL, the call returns
///   `LANCEDB_INVALID_ARGUMENT` without consuming a non-NULL result, so the
///   caller may retry it or free it with `lancedb_query_result_free()`.
/// - `batches_out`, `schema_out`, `count_out` must be writable when non-NULL
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*count_out` set to the number of batches, and
///   `*batches_out` and `*schema_out` holding the batches and their schema.
///   An empty result succeeds with both pointers NULL and `*count_out` zero.
/// - The SDK error code with the SDK error text in `*error_message` when the
///   stream fails, or `LANCEDB_ARROW` with the conversion detail when the Arrow
///   schema cannot be represented through the C data interface.
///
/// On entry, non-NULL output slots and the error message are cleared. A failure
/// leaves all three outputs clear. Free returned arrays with
/// `lancedb_free_arrow_arrays()` and a returned schema with
/// `lancedb_free_arrow_schema()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_result_to_arrow(
    result: *mut LanceDBQueryResult,
    batches_out: *mut *mut *mut arrow_array::ffi::FFI_ArrowArray,
    schema_out: *mut *mut arrow_schema::ffi::FFI_ArrowSchema,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !batches_out.is_null() {
        *batches_out = ptr::null_mut();
    }
    if !schema_out.is_null() {
        *schema_out = ptr::null_mut();
    }
    if !count_out.is_null() {
        *count_out = 0;
    }
    clear_error_message(error_message);

    // A missing required pointer rejects the call before the result is consumed,
    // so the caller can retry it or free it afterwards. This is the one documented
    // exception to consuming a non-NULL result.
    if result.is_null() || batches_out.is_null() || schema_out.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let result_box = Box::from_raw(result);
    let runtime = get_runtime();

    match runtime.block_on(async {
        let batches: Vec<RecordBatch> = result_box.inner.try_collect().await?;
        Ok::<Vec<RecordBatch>, lancedb::error::Error>(batches)
    }) {
        Ok(batches) => {
            // An empty result keeps its legacy representation: no batch, no
            // schema and a zero count.
            if batches.is_empty() {
                return LanceDBError::Success;
            }

            // Get schema from first batch (all batches have same schema)
            let schema = batches[0].schema();
            let ffi_schema = match arrow_schema::ffi::FFI_ArrowSchema::try_from(&*schema) {
                Ok(schema) => Box::new(schema),
                Err(error) => {
                    set_custom_error_message(
                        error_message,
                        &format!("Failed to convert Arrow schema to C ABI: {error}"),
                    );
                    return LanceDBError::Arrow;
                }
            };

            let count = batches.len();
            // Allocate array for Arrow C ABI array structures
            let arrays_ptr =
                libc::malloc(count * std::mem::size_of::<*mut arrow_array::ffi::FFI_ArrowArray>())
                    as *mut *mut arrow_array::ffi::FFI_ArrowArray;
            if arrays_ptr.is_null() {
                // The prepared schema is dropped with this frame, so a failed
                // allocation cannot leave a partial result behind.
                set_unknown_error_message(error_message);
                return LanceDBError::Unknown;
            }

            for (i, batch) in batches.into_iter().enumerate() {
                // Convert RecordBatch to StructArray first, then to FFI_ArrowArray
                let struct_array: StructArray = batch.clone().into();
                let array_data: ArrayData = struct_array.into_data();
                let ffi_array = Box::new(FFI_ArrowArray::new(&array_data));
                *arrays_ptr.add(i) = Box::into_raw(ffi_array);
            }

            // Publish all three outputs only after every piece is ready
            *count_out = count;
            *schema_out = Box::into_raw(ffi_schema);
            *batches_out = arrays_ptr;
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Convert a query result into an Arrow IPC file
///
/// # Safety
/// - `result` must be NULL or a valid pointer returned by query execution
///   (`lancedb_query_execute()`, `lancedb_query_execute_with_status()` or a
///   vector variant). A non-NULL pointer is consumed, also when the call fails.
/// - `buffer_out` must be a valid pointer to receive the file bytes
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*buffer_out` holding one complete IPC file. The file
///   carries the projected schema and one batch per batch the stream yielded, in
///   order, including zero-row batches; an empty result keeps the schema and
///   holds no batch.
/// - The error code of the SDK error that ended the stream, or `LANCEDB_ARROW`
///   when the file could not be encoded
///
/// The bytes are handed over only after the file footer is written, so any
/// failure leaves `*buffer_out` empty. On entry, `*buffer_out` and
/// `*error_message` are cleared. Free the bytes with `lancedb_ipc_buffer_free()`
/// and a returned error message with `lancedb_free_string()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_result_to_ipc_file(
    result: *mut LanceDBQueryResult,
    buffer_out: *mut LanceDBIpcBuffer,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !buffer_out.is_null() {
        *buffer_out = LanceDBIpcBuffer::EMPTY;
    }
    clear_error_message(error_message);

    if result.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }
    // A non-NULL result is consumed on every path, so a caller never keeps it
    // alive for a call that cannot write its output
    let stream = Box::from_raw(result).inner;
    if buffer_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let schema = stream.schema();

    match get_runtime().block_on(ipc::write_ipc_file_from_stream(schema, stream)) {
        Ok(bytes) => {
            *buffer_out = LanceDBIpcBuffer::from_file_bytes(bytes);
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Free a Query
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `query` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_free(query: *mut LanceDBQuery) {
    if !query.is_null() {
        let _ = Box::from_raw(query);
    }
}

/// Free a VectorQuery
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `query` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_free(query: *mut LanceDBVectorQuery) {
    if !query.is_null() {
        let _ = Box::from_raw(query);
    }
}

/// Free a QueryResult
///
/// # Safety
/// - `result` must be a valid pointer returned from query execution functions
/// - `result` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_result_free(result: *mut LanceDBQueryResult) {
    if !result.is_null() {
        let _ = Box::from_raw(result);
    }
}

/// Free Arrow arrays returned by query result functions
///
/// # Safety
/// - `batches` must be a pointer returned by `lancedb_query_result_to_arrow`
/// - `count` must match the count returned by `lancedb_query_result_to_arrow`
/// - `batches` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_arrow_arrays(
    batches: *mut *mut arrow_array::ffi::FFI_ArrowArray,
    count: usize,
) {
    if !batches.is_null() {
        for i in 0..count {
            let batch_ptr = *batches.add(i);
            if !batch_ptr.is_null() {
                let _ = Box::from_raw(batch_ptr);
            }
        }
        libc::free(batches as *mut libc::c_void);
    }
}
