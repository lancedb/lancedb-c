// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Full Query API implementation for LanceDB C bindings
//!
//! This module provides complete query operations with proper Arrow integration

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_float, c_int, c_void};
use std::ptr;
use std::sync::Arc;

use arrow_array::ffi::FFI_ArrowArray;
use arrow_array::{Array, RecordBatch, StructArray};
use arrow_schema::ffi::FFI_ArrowSchema;
use futures::TryStreamExt;
use half::f16;

use lancedb::arrow::arrow_data::ArrayData;
use lancedb::arrow::SendableRecordBatchStream;
use lancedb::datafusion::logical_expr::Expr;
use lancedb::index::scalar::{
    BooleanQuery, BoostQuery, FtsQuery, FullTextSearchQuery, MatchQuery, Occur, Operator,
    PhraseQuery,
};
use lancedb::query::{
    ColumnOrdering, ExecutableQuery, HasQuery, Query, QueryBase, QueryFilter, Select, VectorQuery,
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
use crate::types::{LanceDBDistanceType, LanceDBNormalizeMethod, LanceDBQueryVectorType};

/// Opaque handle to a LanceDB Query
#[repr(C)]
pub struct LanceDBQuery {
    table: Arc<Table>,
    limit: Option<usize>,
    offset: Option<usize>,
    select: Option<Select>,
    order_by: Option<Vec<ColumnOrdering>>,
    with_row_id: bool,
    fast_search: bool,
    filter: Option<String>,
    df_filter: Option<Expr>,
    full_text_search: Option<FullTextSearchQuery>,
}

/// Opaque handle to a structured full-text query
///
/// The constructor copies its inputs. Builder setters clone the stored SDK
/// query, so one handle can be attached to several builders and freed as soon
/// as the setters return. Create with lancedb_fts_query_new_match(),
/// lancedb_fts_query_new_phrase(), lancedb_fts_query_new_boolean() or
/// lancedb_fts_query_new_boost().
#[repr(C)]
pub struct LanceDBFtsQuery {
    inner: FullTextSearchQuery,
}

/// Term combination operator for a structured Match query
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub enum LanceDBFtsOperator {
    Or = 0,
    And = 1,
}

impl LanceDBFtsOperator {
    /// Convert the raw C value, rejecting anything outside the public header.
    fn from_c_int(value: c_int) -> Option<Self> {
        match value {
            0 => Some(Self::Or),
            1 => Some(Self::And),
            _ => None,
        }
    }
}

impl From<LanceDBFtsOperator> for Operator {
    fn from(operator: LanceDBFtsOperator) -> Self {
        match operator {
            LanceDBFtsOperator::Or => Self::Or,
            LanceDBFtsOperator::And => Self::And,
        }
    }
}

/// Occurrence of one clause in a structured Boolean query
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub enum LanceDBFtsOccur {
    Should = 0,
    Must = 1,
    MustNot = 2,
}

impl LanceDBFtsOccur {
    /// Convert the raw C value, rejecting anything outside the public header.
    fn from_c_int(value: c_int) -> Option<Self> {
        match value {
            0 => Some(Self::Should),
            1 => Some(Self::Must),
            2 => Some(Self::MustNot),
            _ => None,
        }
    }
}

impl From<LanceDBFtsOccur> for Occur {
    fn from(occur: LanceDBFtsOccur) -> Self {
        match occur {
            LanceDBFtsOccur::Should => Self::Should,
            LanceDBFtsOccur::Must => Self::Must,
            LanceDBFtsOccur::MustNot => Self::MustNot,
        }
    }
}

/// One borrowed clause of a structured Boolean query
///
/// `query` is borrowed and its structured query node is copied into the Boolean
/// query. `occur` holds a raw `LanceDBFtsOccur` value so an out-of-range C value
/// can be rejected before it becomes a Rust enum.
#[repr(C)]
pub struct LanceDBFtsClause {
    pub query: *const LanceDBFtsQuery,
    pub occur: c_int,
}

/// Options for a structured Match query
///
/// `fuzziness` uses -1 for the SDK's automatic choice and 0 through
/// `UINT32_MAX` for an explicit edit distance. `operator` holds a raw
/// `LanceDBFtsOperator` value so an out-of-range C value can be rejected before
/// it becomes a Rust enum. The ordinary embedded planner shares one
/// max_expansions budget across a Match leaf's query positions and selected
/// physical segments; the pinned MemWAL path applies it separately to the base
/// table, each SSTable and each in-memory source, using each source's own
/// vocabulary.
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct LanceDBFtsMatchOptions {
    pub fuzziness: i64,
    pub boost: f32,
    pub max_expansions: usize,
    pub term_operator: c_int,
    pub prefix_length: u32,
}

impl Default for LanceDBFtsMatchOptions {
    fn default() -> Self {
        // Read the defaults from the SDK constructor so this struct stays in
        // sync with the SDK instead of repeating its values in C.
        let defaults = MatchQuery::new(String::new());
        Self {
            fuzziness: defaults.fuzziness.map(i64::from).unwrap_or(-1),
            boost: defaults.boost,
            max_expansions: defaults.max_expansions,
            term_operator: match defaults.operator {
                Operator::Or => LanceDBFtsOperator::Or as c_int,
                Operator::And => LanceDBFtsOperator::And as c_int,
            },
            prefix_length: defaults.prefix_length,
        }
    }
}

/// Return the default structured Match query options
///
/// The values come from the SDK's `MatchQuery::new`: OR term combination,
/// boost 1.0, exact matching (`fuzziness = 0`), 50 maximum expansions, and
/// prefix length 0.
#[no_mangle]
pub extern "C" fn lancedb_fts_match_options_default() -> LanceDBFtsMatchOptions {
    LanceDBFtsMatchOptions::default()
}

/// One borrowed typed query vector
///
/// `kind` selects the element type, `data` points to `dimension` elements of
/// that type, and both constructors and the add setter copy the elements before
/// returning.
#[repr(C)]
pub struct LanceDBQueryVector {
    pub kind: c_int,
    pub data: *const c_void,
    pub dimension: usize,
}

/// A query vector copied from C and kept in its input form until the SDK query
/// is built.
#[derive(Clone, Debug)]
enum StoredQueryVector {
    F16(Vec<f16>),
    F32(Vec<f32>),
    F64(Vec<f64>),
    Binary(Vec<u8>),
}

impl StoredQueryVector {
    /// Copy one C vector, rejecting only a broken description. Values are
    /// validated by the SDK when the query is configured, matching the legacy
    /// f32 constructor.
    unsafe fn copy_from_c(vector: &LanceDBQueryVector) -> Result<Self, ()> {
        if vector.data.is_null() || vector.dimension == 0 {
            return Err(());
        }

        let kind = LanceDBQueryVectorType::from_c_int(vector.kind).ok_or(())?;
        match kind {
            LanceDBQueryVectorType::F16 => {
                let bits = std::slice::from_raw_parts(vector.data as *const u16, vector.dimension);
                Ok(Self::F16(
                    bits.iter().map(|bits| f16::from_bits(*bits)).collect(),
                ))
            }
            LanceDBQueryVectorType::F32 => {
                let values =
                    std::slice::from_raw_parts(vector.data as *const f32, vector.dimension);
                Ok(Self::F32(values.to_vec()))
            }
            LanceDBQueryVectorType::F64 => {
                let values =
                    std::slice::from_raw_parts(vector.data as *const f64, vector.dimension);
                Ok(Self::F64(values.to_vec()))
            }
            LanceDBQueryVectorType::Binary => {
                let values = std::slice::from_raw_parts(vector.data as *const u8, vector.dimension);
                Ok(Self::Binary(values.to_vec()))
            }
        }
    }
}

/// Start a vector query from one stored vector.
fn nearest_to_stored(
    query: Query,
    vector: &StoredQueryVector,
) -> Result<VectorQuery, lancedb::error::Error> {
    match vector {
        StoredQueryVector::F16(values) => query.nearest_to(values.as_slice()),
        StoredQueryVector::F32(values) => query.nearest_to(values.as_slice()),
        StoredQueryVector::F64(values) => query.nearest_to(values.as_slice()),
        StoredQueryVector::Binary(values) => {
            // The SDK exposes no byte-slice query input. Wide each byte to its
            // numeric value so a UInt8 column is cast back at planning time.
            let widened: Vec<f32> = values.iter().map(|value| f32::from(*value)).collect();
            query.nearest_to(widened.as_slice())
        }
    }
}

/// Add one stored vector to an existing vector query.
fn add_stored_query_vector(
    query: VectorQuery,
    vector: &StoredQueryVector,
) -> Result<VectorQuery, lancedb::error::Error> {
    match vector {
        StoredQueryVector::F16(values) => query.add_query_vector(values.as_slice()),
        StoredQueryVector::F32(values) => query.add_query_vector(values.as_slice()),
        StoredQueryVector::F64(values) => query.add_query_vector(values.as_slice()),
        StoredQueryVector::Binary(values) => {
            let widened: Vec<f32> = values.iter().map(|value| f32::from(*value)).collect();
            query.add_query_vector(widened.as_slice())
        }
    }
}

/// Opaque handle to a LanceDB VectorQuery
#[repr(C)]
pub struct LanceDBVectorQuery {
    table: Arc<Table>,
    query_vectors: Vec<StoredQueryVector>,
    column: Option<String>,
    limit: Option<usize>,
    offset: Option<usize>,
    select: Option<Select>,
    order_by: Option<Vec<ColumnOrdering>>,
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

/// One column ordering for `lancedb_query_order_by` and
/// `lancedb_vector_query_order_by`.
#[repr(C)]
#[derive(Debug)]
pub struct LanceDBColumnOrdering {
    /// NUL-terminated UTF-8 column name.
    pub column: *const c_char,
    /// Non-zero for ascending order, zero for descending.
    pub ascending: i32,
    /// Non-zero puts NULL values first, regardless of direction.
    pub nulls_first: i32,
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
        order_by: None,
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
        query_vectors: vec![StoredQueryVector::F32(vec_data)],
        column: None,
        limit: None,
        offset: None,
        select: None,
        order_by: None,
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

/// Create a vector query from one or more typed query vectors
///
/// `vectors` is borrowed for this call and every input is copied before the
/// function returns. The first vector becomes the nearest-neighbour query and
/// each later vector is added as an SDK batch query vector. `num_vectors` must
/// be at least one.
///
/// F16, F32 and F64 inputs are passed to the SDK in their declared element
/// type; the SDK converts F16 and F64 components to Float32 before planning.
/// The binary form widens each byte to its numeric Float32 value, so a
/// `FixedSizeList<UInt8>` column is cast back to UInt8 at planning time. Lance
/// supports only `LANCEDB_DISTANCE_HAMMING` for such a column. No additional
/// precision is preserved for F64 input.
///
/// Against a fixed-size-list float vector column, multiple vectors produce one
/// top-K result set per vector and the result adds an Int32 `query_index`
/// column. Against a `List<FixedSizeList<...>>` multivector column, multiple
/// vectors form one multivector search and no `query_index` column is added. A
/// UInt8 fixed-size vector column keeps one plan per vector and also adds
/// `query_index`; Hamming is the only supported distance.
///
/// These batch contracts apply to an ordinary vector search, without a
/// full-text query. `limit` and `offset` apply independently to each vector in
/// a batch, and `query_index` is the zero-based position of that vector, not a
/// rank. Rows for different indexes are not required to be grouped or ordered.
/// On a multivector column, `limit` selects table rows in total and each row's
/// `_distance` includes the contribution of every input vector.
///
/// The pinned SDK beta.26 cannot merge a hybrid query whose vector leg returns
/// batch results with `query_index` and whose full-text leg returns matches:
/// the SDK's RRF merge combines the batch `query_index` column with the
/// full-text schema and returns an Arrow schema error. This includes multiple
/// vectors on a fixed-size-list float column and the per-vector UInt8/Hamming
/// path. This binding forwards the configured SDK query; it does not split the
/// batch, drop `query_index`, or run a C-side fusion. Use one query vector for
/// that hybrid shape until the SDK supports it. A `List<FixedSizeList<...>>`
/// multivector column adds no `query_index`, so it does not hit that merge
/// shape.
///
/// A vector-only query is planned locally by the native table, including on a
/// native namespace connection. On a namespace connection with QueryTable
/// pushdown, only an eligible hybrid full-text leg can use the namespace
/// QueryTable path; the vector batch remains a locally planned query. This
/// binding does not choose a different route.
///
/// For a remote LanceDB, the SDK sends Float32 vectors. Enterprise server 0.2.0
/// and later receive all input vectors in one request; older servers receive
/// one request per vector and the SDK combines the responses with
/// `query_index`. The server decides how it interprets the vector shape and
/// distance and which schema it returns; this binding does not reshape or
/// re-issue the result.
///
/// A table routed through the pinned MemWAL LSM scanner rejects a single plan
/// containing multiple query vectors, UInt8 vector search, or both vector and
/// full-text search with `LANCEDB_NOT_SUPPORTED`. Hybrid execution does not
/// build such a plan: it runs separate vector and full-text searches, and can
/// fall back to primary-key fusion when the table rejects `_rowid`. An eligible
/// single-vector hybrid query can therefore execute. This binding exposes no
/// route override.
///
/// This constructor only copies and checks the description. Like
/// `lancedb_vector_query_new`, the SDK reports non-finite values, F64 values
/// that become non-finite after conversion, and mismatched dimensions when the
/// query is executed or its plan or output schema is requested.
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `vectors` must point to `num_vectors` valid descriptions
/// - each `data` pointer must point to `dimension` elements of its declared type
///
/// # Returns
/// - Non-null pointer to LanceDBVectorQuery on success
/// - Null pointer when an argument is missing, a kind is outside
///   `LanceDBQueryVectorType`, or a vector has NULL data or zero dimension
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_new_typed(
    table: *const LanceDBTable,
    vectors: *const LanceDBQueryVector,
    num_vectors: usize,
) -> *mut LanceDBVectorQuery {
    if table.is_null() || vectors.is_null() || num_vectors == 0 {
        return ptr::null_mut();
    }

    let mut query_vectors = Vec::with_capacity(num_vectors);
    for index in 0..num_vectors {
        let vector = &*vectors.add(index);
        match StoredQueryVector::copy_from_c(vector) {
            Ok(vector) => query_vectors.push(vector),
            Err(()) => return ptr::null_mut(),
        }
    }

    let tbl = &(*table).inner;
    Box::into_raw(Box::new(LanceDBVectorQuery {
        table: Arc::new(tbl.clone()),
        query_vectors,
        column: None,
        limit: None,
        offset: None,
        select: None,
        order_by: None,
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
    }))
}

/// Add a typed query vector to a vector query
///
/// The vector is borrowed for this call and copied before it returns. On
/// success it is appended to the SDK query as a batch query vector, with the
/// same result schema and validation timing as `lancedb_vector_query_new_typed`.
/// The builder may come from either constructor. Appending a second or later
/// vector makes the query use the batch shape described there, including its
/// ordinary-vector top-K/query_index contract and its hybrid and MemWAL
/// limitations.
///
/// # Safety
/// - `query` must be a valid pointer returned from a vector query constructor
/// - `vector` must point to a valid description
/// - its `data` pointer must point to `dimension` elements of its declared type
/// - `error_message` can be NULL to ignore detailed error messages
///
/// A missing query or vector, NULL data, zero dimension, or a kind outside
/// `LanceDBQueryVectorType` returns `LANCEDB_INVALID_ARGUMENT` without changing
/// the builder, so it still executes with the vectors it already had.
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_add_query_vector(
    query: *mut LanceDBVectorQuery,
    vector: *const LanceDBQueryVector,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);

    if query.is_null() || vector.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match StoredQueryVector::copy_from_c(&*vector) {
        Ok(vector) => {
            (*query).query_vectors.push(vector);
            LanceDBError::Success
        }
        Err(()) => {
            set_invalid_argument_message(error_message);
            LanceDBError::InvalidArgument
        }
    }
}

/// Copy the query text and optional text column from C strings
///
/// # Safety
/// - `text` must be a valid null-terminated UTF-8 string
/// - `column` may be NULL or a valid null-terminated UTF-8 string. An empty
///   column leaves the choice to LanceDB, which uses the indexed text column.
unsafe fn full_text_search_parts_from_c(
    text: *const c_char,
    column: *const c_char,
) -> Result<(String, Option<String>), String> {
    if text.is_null() {
        return Err("full-text search text is required".to_string());
    }

    let text = CStr::from_ptr(text)
        .to_str()
        .map_err(|_| "full-text search text is not valid UTF-8".to_string())?
        .to_string();

    let column = if column.is_null() {
        None
    } else {
        let column = CStr::from_ptr(column)
            .to_str()
            .map_err(|_| "full-text search column is not valid UTF-8".to_string())?;
        if column.is_empty() {
            None
        } else {
            Some(column.to_string())
        }
    };

    Ok((text, column))
}

/// Build the legacy text Match query from the C text and optional column
///
/// # Safety
/// See `full_text_search_parts_from_c`.
unsafe fn full_text_search_from_c(
    text: *const c_char,
    column: *const c_char,
) -> Result<FullTextSearchQuery, String> {
    let (text, column) = full_text_search_parts_from_c(text, column)?;
    let mut search = FullTextSearchQuery::new(text);

    if let Some(column) = column {
        search = search
            .with_column(column)
            .map_err(|error| format!("Invalid FTS column: {error}"))?;
    }

    Ok(search)
}

/// Build a structured Match query from C text, column, and optional options
///
/// A NULL options pointer uses the SDK's `MatchQuery::new` defaults. The
/// fuzziness and operator values are representational choices, so they are
/// validated here; score and planner validation stay in the SDK.
///
/// # Safety
/// See `full_text_search_parts_from_c`. `options` may be NULL or point to a
/// readable `LanceDBFtsMatchOptions`.
unsafe fn match_query_from_c(
    text: *const c_char,
    column: *const c_char,
    options: *const LanceDBFtsMatchOptions,
) -> Result<MatchQuery, String> {
    let (text, column) = full_text_search_parts_from_c(text, column)?;
    let mut query = MatchQuery::new(text);
    if let Some(column) = column {
        query = query.with_column(Some(column));
    }

    if options.is_null() {
        return Ok(query);
    }

    let options = &*options;
    let fuzziness = match options.fuzziness {
        -1 => None,
        value if (0..=i64::from(u32::MAX)).contains(&value) => Some(value as u32),
        value => {
            return Err(format!(
                "fuzziness must be -1 for automatic matching or between 0 and {}, got {value}",
                u32::MAX
            ));
        }
    };
    let operator = LanceDBFtsOperator::from_c_int(options.term_operator).ok_or_else(|| {
        format!(
            "operator must be a LanceDBFtsOperator value, got {}",
            options.term_operator
        )
    })?;

    Ok(query
        .with_fuzziness(fuzziness)
        .with_boost(options.boost)
        .with_max_expansions(options.max_expansions)
        .with_operator(operator.into())
        .with_prefix_length(options.prefix_length))
}

/// Clone one borrowed handle's structured query node
///
/// Only the node is copied; the handle itself stays owned by the caller.
///
/// # Safety
/// `query` must be a valid pointer returned from an `lancedb_fts_query_new_*`
/// constructor.
unsafe fn clone_fts_node(query: *const LanceDBFtsQuery, role: &str) -> Result<FtsQuery, String> {
    if query.is_null() {
        return Err(format!("the Boost {role} query is required"));
    }
    Ok((*query).inner.query.clone())
}

/// Build a structured Boolean query from a flat borrowed clause array
///
/// A NULL array with zero clauses builds an empty Boolean query, which the SDK
/// rejects when the query is planned: it has no leaf for granularity and no
/// SHOULD or MUST clause for scoring. Occurrence values are representational,
/// so they are validated here; clause semantics, scoring and the non-empty
/// requirements stay in the SDK.
///
/// # Safety
/// `clauses` may be NULL only when `num_clauses` is zero. Otherwise it must
/// point to `num_clauses` readable clauses whose `query` handles are valid.
unsafe fn boolean_query_from_c(
    clauses: *const LanceDBFtsClause,
    num_clauses: usize,
) -> Result<BooleanQuery, String> {
    let clauses = if num_clauses == 0 {
        &[]
    } else {
        if clauses.is_null() {
            return Err("boolean clauses are required when num_clauses is not zero".to_string());
        }
        std::slice::from_raw_parts(clauses, num_clauses)
    };

    let mut entries = Vec::with_capacity(clauses.len());
    for (index, clause) in clauses.iter().enumerate() {
        let occur = LanceDBFtsOccur::from_c_int(clause.occur).ok_or_else(|| {
            format!(
                "clause {index} occurrence must be a LanceDBFtsOccur value, got {}",
                clause.occur
            )
        })?;
        if clause.query.is_null() {
            return Err(format!("clause {index} query handle is required"));
        }
        entries.push((occur.into(), (*clause.query).inner.query.clone()));
    }

    Ok(BooleanQuery::new(entries))
}

/// Build a structured Boost query from borrowed positive and negative handles
///
/// A NULL multiplier pointer passes `None` to the SDK constructor, which
/// supplies its default 0.5. The multiplier value, including its finite and
/// non-negative requirement, is validated by the SDK when the query is planned.
///
/// # Safety
/// `positive` and `negative` must be valid pointers returned from an
/// `lancedb_fts_query_new_*` constructor; `negative_boost` may be NULL or point
/// to a readable `f32`.
unsafe fn boost_query_from_c(
    positive: *const LanceDBFtsQuery,
    negative: *const LanceDBFtsQuery,
    negative_boost: *const f32,
) -> Result<BoostQuery, String> {
    let positive = clone_fts_node(positive, "positive")?;
    let negative = clone_fts_node(negative, "negative")?;
    let negative_boost = if negative_boost.is_null() {
        None
    } else {
        Some(*negative_boost)
    };

    Ok(BoostQuery::new(positive, negative, negative_boost))
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

/// Read a borrowed C column-ordering array into the SDK's ordering values.
///
/// A zero count clears the ordering, so `orderings` may be NULL. A positive
/// count requires an array whose entries and column names are non-NULL and
/// valid UTF-8. Entries are copied in array order.
unsafe fn column_orderings_from_c(
    orderings: *const LanceDBColumnOrdering,
    num_orderings: usize,
) -> Result<Option<Vec<ColumnOrdering>>, ()> {
    if num_orderings == 0 {
        return Ok(None);
    }
    if orderings.is_null() {
        return Err(());
    }

    let mut parsed = Vec::with_capacity(num_orderings);
    for index in 0..num_orderings {
        let ordering = &*orderings.add(index);
        if ordering.column.is_null() {
            return Err(());
        }
        let column = CStr::from_ptr(ordering.column)
            .to_str()
            .map_err(|_| ())?
            .to_string();
        parsed.push(ColumnOrdering {
            column_name: column,
            ascending: ordering.ascending != 0,
            nulls_first: ordering.nulls_first != 0,
        });
    }

    Ok(Some(parsed))
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

/// Set the column ordering for a plain query
///
/// The ordering array is borrowed for the call and copied. A zero count clears
/// the ordering and accepts a NULL array; a positive count requires a non-NULL
/// array whose entries and column names are non-NULL and valid UTF-8. Invalid
/// input returns `LANCEDB_INVALID_ARGUMENT` without changing the previous
/// ordering.
///
/// A nonempty ordering sorts an ordinary scan before limit and offset are
/// applied. On a standalone full-text search it sorts all matches before the
/// search page is selected. On a hybrid vector plus full-text query it applies
/// to both search legs, and the reranker still determines the final fused
/// order.
///
/// A native namespace connection with QueryTable pushdown executes a nonempty
/// ordering locally, so it needs client access to table storage. A table routed
/// through the MemWAL LSM scanner rejects a nonempty ordering with
/// `LANCEDB_NOT_SUPPORTED`; clear the ordering to return to the default route.
/// A remote LanceDB forwards the ordering to the server.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `orderings` must be NULL or point to `num_orderings` entries
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_order_by(
    query: *mut LanceDBQuery,
    orderings: *const LanceDBColumnOrdering,
    num_orderings: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match column_orderings_from_c(orderings, num_orderings) {
        Ok(ordering) => {
            (*query).order_by = ordering;
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

/// Create a structured Match full-text query
///
/// The text is tokenized by the target index at execution time. A NULL options
/// pointer uses the SDK defaults; `lancedb_fts_match_options_default()` returns
/// the same values for callers that want to change individual fields. The
/// text and column are copied. A NULL or empty column leaves the choice to the
/// index.
///
/// `fuzziness = -1` forwards the SDK's automatic mode, while 0 through
/// `UINT32_MAX` selects an explicit edit distance. The pinned embedded and
/// MemWAL paths currently resolve automatic fuzziness to exact matching.
/// Options that are constructed successfully still depend on the selected
/// backend: a namespace QueryTable pushdown sends a root OR Match as text and
/// column only, while a root AND Match keeps the structured node. The namespace
/// model stores fuzziness, max_expansions and prefix_length as signed 32-bit
/// values, so values above INT32_MAX fail an AND pushdown during execution with
/// `LANCEDB_INVALID_INPUT`. A MemWAL LSM query requires an explicit single text
/// column whose FTS index is maintained by the write spec and carried by every
/// resident MemTable; otherwise it returns `LANCEDB_NOT_SUPPORTED`. Enterprise
/// servers 0.3.0 and later receive the structured query; older servers receive
/// only text and column, so those options are not transmitted, and a root AND
/// Match returns `LANCEDB_NOT_SUPPORTED`.
///
/// # Safety
/// - `text` must be a valid null-terminated UTF-8 string
/// - `column` may be NULL or a valid null-terminated UTF-8 string naming the
///   text column; an empty string means automatic discovery
/// - `options` may be NULL or point to initialized `LanceDBFtsMatchOptions`
/// - `query_out` must be a valid pointer to receive the created handle
/// - `error_message` can be NULL to ignore detailed error messages
///
/// On entry `*query_out` and the error message are cleared. On success the
/// caller owns the handle and frees it with `lancedb_fts_query_free()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_fts_query_new_match(
    text: *const c_char,
    column: *const c_char,
    options: *const LanceDBFtsMatchOptions,
    query_out: *mut *mut LanceDBFtsQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !query_out.is_null() {
        *query_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if query_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let query = match match_query_from_c(text, column, options) {
        Ok(query) => query,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return LanceDBError::InvalidArgument;
        }
    };
    let search = FullTextSearchQuery::new_query(query.into());

    *query_out = Box::into_raw(Box::new(LanceDBFtsQuery { inner: search }));
    LanceDBError::Success
}

/// Create a structured phrase full-text query
///
/// The phrase text and column are copied, and `slop` is forwarded unchanged to
/// the SDK's `PhraseQuery`. A NULL or empty column leaves the choice to the
/// index; an explicit column can be scanned without an index by embedded
/// LanceDB. Indexed phrase queries require the index to store positions.
///
/// The SDK selects embedded, namespace, or Enterprise execution from the
/// connection. A namespace connection forwards the structured node only with
/// QueryTable pushdown enabled and selected. Enterprise servers 0.3.0 and later
/// accept the node; older servers receive only the quoted phrase text, so a
/// nonzero slop is not transmitted.
///
/// The pinned Lance indexed path converts `slop` to signed 32-bit. Values up to
/// `i32::MAX` keep their indexed meaning; larger values are forwarded unchanged
/// but can return no matches. This binding does not clamp or reject them.
///
/// # Safety
/// - `text` must be a valid null-terminated UTF-8 string
/// - `column` may be NULL or a valid null-terminated UTF-8 string naming the
///   text column; an empty string means automatic discovery
/// - `query_out` must be a valid pointer to receive the created handle
/// - `error_message` can be NULL to ignore detailed error messages
///
/// On entry `*query_out` and the error message are cleared. On success the
/// caller owns the handle and frees it with `lancedb_fts_query_free()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_fts_query_new_phrase(
    text: *const c_char,
    column: *const c_char,
    slop: u32,
    query_out: *mut *mut LanceDBFtsQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !query_out.is_null() {
        *query_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if query_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let (text, column) = match full_text_search_parts_from_c(text, column) {
        Ok(parts) => parts,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return LanceDBError::InvalidArgument;
        }
    };

    let mut phrase = PhraseQuery::new(text).with_slop(slop);
    if let Some(column) = column {
        phrase = phrase.with_column(Some(column));
    }
    let search = FullTextSearchQuery::new_query(phrase.into());

    *query_out = Box::into_raw(Box::new(LanceDBFtsQuery { inner: search }));
    LanceDBError::Success
}

/// Create a structured Boolean full-text query
///
/// Clauses are a flat borrowed array; nesting is expressed by composing handles
/// rather than by a recursive C representation. Each clause's structured query
/// node is copied, so child handles can be reused or freed after this call
/// returns.
///
/// SDK semantics apply unchanged. MUST clauses must all match and their scores
/// are summed. SHOULD clauses are optional when a MUST clause exists and add
/// their score when they match; without MUST clauses, at least one SHOULD must
/// match and the matching SHOULD scores are summed. MUST_NOT clauses exclude
/// matching documents and never contribute score. An empty Boolean query, and
/// a query whose only clauses are MUST_NOT, construct successfully; the
/// embedded dataset scanner rejects them at execution with
/// `LANCEDB_INVALID_INPUT` because it requires a leaf for document-granularity
/// resolution and a SHOULD or MUST clause for scoring. This constructor does
/// not impose either requirement early. A MemWAL LSM query runs its column and
/// maintained-index checks first, so a tree without a named column reports that
/// column error before clause validation, and a negative-only query that passes
/// the column checks is validated by the LSM path. A namespace QueryTable
/// pushdown and Enterprise forward the structured tree instead of planning it
/// locally, so the server determines the error after forwarding or projection.
///
/// Backend limitations are documented on the C declaration; they cover
/// namespace int32 narrowing, the older Enterprise transport's empty query
/// string and the MemWAL single explicit column requirement.
///
/// # Safety
/// - `clauses` may be NULL only when `num_clauses` is zero; otherwise it must
///   point to `num_clauses` readable `LanceDBFtsClause` entries
/// - every clause's `query` must be a valid borrowed handle
/// - `query_out` must be a valid pointer to receive the created handle
/// - `error_message` can be NULL to ignore detailed error messages
///
/// On entry `*query_out` and the error message are cleared. On success the
/// caller owns the handle and frees it with `lancedb_fts_query_free()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_fts_query_new_boolean(
    clauses: *const LanceDBFtsClause,
    num_clauses: usize,
    query_out: *mut *mut LanceDBFtsQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !query_out.is_null() {
        *query_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if query_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let query = match boolean_query_from_c(clauses, num_clauses) {
        Ok(query) => query,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return LanceDBError::InvalidArgument;
        }
    };
    let search = FullTextSearchQuery::new_query(query.into());

    *query_out = Box::into_raw(Box::new(LanceDBFtsQuery { inner: search }));
    LanceDBError::Success
}

/// Create a structured Boost full-text query
///
/// Both legs are required borrowed handles, and each leg's structured query
/// node is copied so the handles can be reused or freed after this call
/// returns. A NULL `negative_boost` passes `None` to the SDK constructor, which
/// supplies its default 0.5; a pointed-to value is forwarded unchanged. The
/// embedded dataset scanner requires it to be finite and non-negative and
/// otherwise fails while planning with `LANCEDB_INVALID_INPUT`. A MemWAL LSM
/// query runs its column checks first, and namespace or Enterprise execution
/// determines the outcome after forwarding the structured tree.
///
/// The positive query drives iteration, so only its matches appear. When the
/// negative query also matches a document, its score times the multiplier is
/// subtracted from the positive score; otherwise the positive score is
/// returned. Boost is a positive gate plus a demotion, not a disjunction.
///
/// Backend limitations are documented on the C declaration; they cover
/// namespace int32 narrowing, the older Enterprise transport's positive-only
/// flat query and the MemWAL single explicit column requirement.
///
/// # Safety
/// - `positive` and `negative` must be valid borrowed handles
/// - `negative_boost` may be NULL or point to a readable `f32`
/// - `query_out` must be a valid pointer to receive the created handle
/// - `error_message` can be NULL to ignore detailed error messages
///
/// On entry `*query_out` and the error message are cleared. On success the
/// caller owns the handle and frees it with `lancedb_fts_query_free()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_fts_query_new_boost(
    positive: *const LanceDBFtsQuery,
    negative: *const LanceDBFtsQuery,
    negative_boost: *const f32,
    query_out: *mut *mut LanceDBFtsQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !query_out.is_null() {
        *query_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if query_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let query = match boost_query_from_c(positive, negative, negative_boost) {
        Ok(query) => query,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return LanceDBError::InvalidArgument;
        }
    };
    let search = FullTextSearchQuery::new_query(query.into());

    *query_out = Box::into_raw(Box::new(LanceDBFtsQuery { inner: search }));
    LanceDBError::Success
}

/// Free a structured full-text query
///
/// # Safety
/// - `query` must be NULL or a valid pointer returned from
///   `lancedb_fts_query_new_match`, `lancedb_fts_query_new_phrase`,
///   `lancedb_fts_query_new_boolean` or `lancedb_fts_query_new_boost`
/// - `query` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_fts_query_free(query: *mut LanceDBFtsQuery) {
    if !query.is_null() {
        let _ = Box::from_raw(query);
    }
}

/// Set a structured full-text query for query
///
/// This replaces any full-text query set by
/// `lancedb_query_full_text_search` or an earlier call to this function. The
/// builder clones the handle's query, so the caller keeps ownership and may
/// reuse or free the handle immediately.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_query_new`
/// - `full_text_query` must be a valid pointer returned from
///   `lancedb_fts_query_new_match`, `lancedb_fts_query_new_phrase`,
///   `lancedb_fts_query_new_boolean` or `lancedb_fts_query_new_boost`; it is
///   borrowed, not consumed
/// - `error_message` can be NULL to ignore detailed error messages
///
/// On failure, including a NULL handle, the builder keeps its previous
/// full-text setting.
#[no_mangle]
pub unsafe extern "C" fn lancedb_query_full_text_search_query(
    query: *mut LanceDBQuery,
    full_text_query: *const LanceDBFtsQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);

    if query.is_null() || full_text_query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    (*query).full_text_search = Some((*full_text_query).inner.clone());
    LanceDBError::Success
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

/// Set the column ordering for a vector query
///
/// The ordering array is borrowed for the call and copied. A zero count clears
/// the ordering and accepts a NULL array; a positive count requires a non-NULL
/// array whose entries and column names are non-NULL and valid UTF-8. Invalid
/// input returns `LANCEDB_INVALID_ARGUMENT` without changing the previous
/// ordering.
///
/// A vector search first selects its nearest-neighbour candidates using the
/// configured limit and offset, or the SDK's default top-K when no limit is
/// set, and then orders those candidates. It does not sort the whole table. On
/// a hybrid vector plus full-text query it applies to both search legs, and the
/// reranker still determines the final fused order.
///
/// A native namespace connection with QueryTable pushdown executes a nonempty
/// ordering locally, so it needs client access to table storage. A table routed
/// through the MemWAL LSM scanner rejects a nonempty ordering with
/// `LANCEDB_NOT_SUPPORTED`; clear the ordering to return to the default route.
/// A remote LanceDB forwards the ordering to the server.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `orderings` must be NULL or point to `num_orderings` entries
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_order_by(
    query: *mut LanceDBVectorQuery,
    orderings: *const LanceDBColumnOrdering,
    num_orderings: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match column_orderings_from_c(orderings, num_orderings) {
        Ok(ordering) => {
            (*query).order_by = ordering;
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
/// If the vector leg returns batch results with `query_index` and the
/// full-text leg returns matches, the pinned SDK beta.26 RRF merge fails with
/// an Arrow schema error. See `lancedb_vector_query_new_typed` for the vector
/// types and routing that produce `query_index`.
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

/// Set a structured full-text query for a vector query
///
/// This replaces any full-text query set by
/// `lancedb_vector_query_full_text_search` or an earlier call to this function,
/// making the builder a hybrid nearest-neighbour plus full-text search. The
/// builder clones the handle's query, so the caller keeps ownership and may
/// reuse or free the handle immediately.
///
/// If the vector leg returns batch results with `query_index` and the
/// full-text leg returns matches, the pinned SDK beta.26 RRF merge fails with
/// an Arrow schema error. See `lancedb_vector_query_new_typed` for the vector
/// types and routing that produce `query_index`.
///
/// # Safety
/// - `query` must be a valid pointer returned from `lancedb_vector_query_new`
/// - `full_text_query` must be a valid pointer returned from
///   `lancedb_fts_query_new_match`, `lancedb_fts_query_new_phrase`,
///   `lancedb_fts_query_new_boolean` or `lancedb_fts_query_new_boost`; it is
///   borrowed, not consumed
/// - `error_message` can be NULL to ignore detailed error messages
///
/// On failure, including a NULL handle, the builder keeps its previous
/// full-text setting.
#[no_mangle]
pub unsafe extern "C" fn lancedb_vector_query_full_text_search_query(
    query: *mut LanceDBVectorQuery,
    full_text_query: *const LanceDBFtsQuery,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);

    if query.is_null() || full_text_query.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    (*query).full_text_search = Some((*full_text_query).inner.clone());
    LanceDBError::Success
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
    if let Some(ref order_by) = query.order_by {
        rust_query = rust_query.order_by(Some(order_by.clone()));
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
    let mut vectors = query.query_vectors.iter();
    let first = vectors
        .next()
        .ok_or_else(|| lancedb::error::Error::InvalidInput {
            message: "query vector is required for vector search".to_string(),
        })?;
    let mut rust_query = nearest_to_stored(query.table.query(), first)?;
    for vector in vectors {
        rust_query = add_stored_query_vector(rust_query, vector)?;
    }

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
    if let Some(ref order_by) = query.order_by {
        rust_query = rust_query.order_by(Some(order_by.clone()));
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
