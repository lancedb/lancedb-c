// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Full Index API implementation for LanceDB C bindings
//!
//! This module provides complete index management operations

use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_float, c_int};
use std::ptr;

use lance::index::DatasetIndexExt;
use lancedb::index::scalar::{
    BTreeIndexBuilder, BitmapIndexBuilder, FmIndexBuilder, FtsIndexBuilder, LabelListIndexBuilder,
};
use lancedb::index::vector::{
    IvfFlatIndexBuilder, IvfHnswFlatIndexBuilder, IvfHnswPqIndexBuilder, IvfHnswSqIndexBuilder,
    IvfPqIndexBuilder, IvfRqIndexBuilder, IvfSqIndexBuilder,
};
use lancedb::index::{Index, IndexType};
use lancedb::table::{OptimizeAction, OptimizeOptions};

use crate::connection::{get_runtime, LanceDBTable};
use crate::error::{
    handle_error, set_custom_error_message, set_invalid_argument_message,
    set_unknown_error_message, LanceDBError,
};
use crate::types::LanceDBDistanceType;

/// Index type enum for C API
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub enum LanceDBIndexType {
    Auto = 0,
    BTree = 1,
    Bitmap = 2,
    LabelList = 3,
    FTS = 4,
    IvfFlat = 5,
    IvfPq = 6,
    IvfHnswPq = 7,
    IvfHnswSq = 8,
    IvfSq = 9,
    IvfRq = 10,
    IvfHnswFlat = 11,
    Fm = 12,
    /// An index type this version of the bindings does not know. Only reported by the
    /// index stats, it cannot be created
    Unknown = 99,
}

impl From<IndexType> for LanceDBIndexType {
    fn from(index_type: IndexType) -> Self {
        match index_type {
            IndexType::IvfFlat => Self::IvfFlat,
            IndexType::IvfSq => Self::IvfSq,
            IndexType::IvfPq => Self::IvfPq,
            IndexType::IvfRq => Self::IvfRq,
            IndexType::IvfHnswPq => Self::IvfHnswPq,
            IndexType::IvfHnswSq => Self::IvfHnswSq,
            IndexType::IvfHnswFlat => Self::IvfHnswFlat,
            IndexType::BTree => Self::BTree,
            IndexType::Bitmap => Self::Bitmap,
            IndexType::LabelList => Self::LabelList,
            IndexType::Fm => Self::Fm,
            IndexType::FTS => Self::FTS,
            IndexType::Unknown => Self::Unknown,
        }
    }
}

/// Apply the parameters shared by all the vector index types to an index builder
macro_rules! apply_ivf_config {
    ($builder:expr, $cfg:expr) => {{
        let mut builder = $builder;
        if $cfg.num_partitions > 0 {
            builder = builder.num_partitions($cfg.num_partitions as u32);
        }
        if $cfg.max_iterations > 0 {
            builder = builder.max_iterations($cfg.max_iterations as u32);
        }
        if $cfg.sample_rate > 0.0 {
            builder = builder.sample_rate($cfg.sample_rate as u32);
        }
        builder.distance_type($cfg.distance_type.into())
    }};
}

/// Convert a C array of null-terminated strings to a vector of strings
///
/// Returns None if any of the strings is NULL or not valid UTF-8
///
/// # Safety
/// - `array` must point to `count` valid C strings
unsafe fn c_str_array_to_vec(array: *const *const c_char, count: usize) -> Option<Vec<String>> {
    let mut strings = Vec::with_capacity(count);
    for i in 0..count {
        let str_ptr = *array.add(i);
        if str_ptr.is_null() {
            return None;
        }
        strings.push(CStr::from_ptr(str_ptr).to_str().ok()?.to_string());
    }
    Some(strings)
}

/// Configuration for vector indices
#[repr(C)]
#[derive(Clone)]
pub struct LanceDBVectorIndexConfig {
    pub num_partitions: c_int, // Number of partitions for IVF indices (-1 = auto)
    pub num_sub_vectors: c_int, // Number of sub-vectors for PQ indices (-1 = auto)
    pub max_iterations: c_int, // Maximum training iterations (-1 = default)
    pub sample_rate: c_float,  // Sampling rate for training (0.0 = default)
    pub distance_type: LanceDBDistanceType, // Distance metric
    pub replace: c_int,        // Replace existing index (1 = true, 0 = false)
}

impl Default for LanceDBVectorIndexConfig {
    fn default() -> Self {
        Self {
            num_partitions: -1,
            num_sub_vectors: -1,
            max_iterations: -1,
            sample_rate: 0.0,
            distance_type: LanceDBDistanceType::L2,
            replace: 1,
        }
    }
}

/// Configuration for scalar indices
#[repr(C)]
#[derive(Clone)]
pub struct LanceDBScalarIndexConfig {
    pub replace: c_int, // Replace existing index (1 = true, 0 = false)
}

impl Default for LanceDBScalarIndexConfig {
    fn default() -> Self {
        Self { replace: 1 }
    }
}

/// Configuration for full-text search indices
#[repr(C)]
#[derive(Clone)]
pub struct LanceDBFtsIndexConfig {
    pub base_tokenizer: *const c_char, // Base tokenizer ("simple", "whitespace", etc.)
    pub language: *const c_char,       // Language for stemming ("en", "es", etc.)
    pub max_token_length: c_int,       // Maximum length of a token (-1 = no limit)
    pub lowercase: c_int,              // Convert to lowercase (1 = true, 0 = false)
    pub stem: c_int,                   // Apply stemming (1 = true, 0 = false)
    pub remove_stop_words: c_int,      // Remove stop words (1 = true, 0 = false)
    pub ascii_folding: c_int,          // Apply ASCII folding (1 = true, 0 = false)
    pub replace: c_int,                // Replace existing index (1 = true, 0 = false)
}

impl Default for LanceDBFtsIndexConfig {
    fn default() -> Self {
        Self {
            base_tokenizer: ptr::null(),
            language: ptr::null(),
            max_token_length: -1,
            lowercase: 1,
            stem: 0,
            remove_stop_words: 0,
            ascii_folding: 0,
            replace: 1,
        }
    }
}

/// Configuration for table optimize action
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub enum LanceDBOptimizeType {
    All = 0,
    Compat = 1,
    Prune = 2,
    Index = 3,
}

/// How unindexed rows are folded into existing indices
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub enum LanceDBReindexMode {
    /// Lance decides, depending on the index type:
    /// - vector index: add a new delta segment like `Append`, unless a partition has grown
    ///   too large or is too small. In that case the partition is split, or joined into
    ///   its neighbours, and all segments are merged into one
    /// - scalar or full-text index: merge the new rows into the newest segment, so the
    ///   number of segments does not grow
    Default = 0,
    /// Add a new delta segment over the new rows, never touch existing segments
    Append = 1,
    /// Merge the new rows and the newest `num_indices_to_merge` segments into one segment
    Merge = 2,
    /// Retrain a vector index on the current data and collapse it to one segment. Other
    /// indices have nothing to train, for them it behaves like `Default`
    Retrain = 3,
}

/// Options for lancedb_table_optimize_indices
#[repr(C)]
#[derive(Debug, Clone)]
pub struct LanceDBOptimizeIndexOptions {
    pub mode: LanceDBReindexMode,
    /// MERGE only: newest segments to merge with the new rows. A count above the number
    /// of segments merges them all, -1 behaves like the DEFAULT mode
    pub num_indices_to_merge: c_int,
    /// Indices to optimize (NULL = all indices)
    pub index_names: *const *const c_char,
    pub num_index_names: usize,
    /// Optional key-value pairs stored in the commit
    pub transaction_property_keys: *const *const c_char,
    /// Parallel to `transaction_property_keys`
    pub transaction_property_values: *const *const c_char,
    pub num_transaction_properties: usize,
}

impl Default for LanceDBOptimizeIndexOptions {
    fn default() -> Self {
        Self {
            mode: LanceDBReindexMode::Default,
            num_indices_to_merge: -1,
            index_names: ptr::null(),
            num_index_names: 0,
            transaction_property_keys: ptr::null(),
            transaction_property_values: ptr::null(),
            num_transaction_properties: 0,
        }
    }
}

/// Get the default options for lancedb_table_optimize_indices
///
/// Equivalent to `lancedb_table_optimize(LANCEDB_OPTIMIZE_INDEX)`
#[no_mangle]
pub extern "C" fn lancedb_optimize_index_options_default() -> LanceDBOptimizeIndexOptions {
    LanceDBOptimizeIndexOptions::default()
}

/// Build the LanceDB optimize options from the C options
///
/// On failure the error message is set and the error code is returned
///
/// # Safety
/// - the arrays in `opts` must point to as many valid C strings as their counts say
unsafe fn build_optimize_options(
    opts: &LanceDBOptimizeIndexOptions,
    error_message: *mut *mut c_char,
) -> Result<OptimizeOptions, LanceDBError> {
    let mut options = match opts.mode {
        LanceDBReindexMode::Default => OptimizeOptions::default(),
        LanceDBReindexMode::Append => OptimizeOptions::append(),
        // num_indices_to_merge is only read in the MERGE mode, the other modes ignore it
        LanceDBReindexMode::Merge => match opts.num_indices_to_merge {
            // None: Lance decides, same as the DEFAULT mode
            -1 => OptimizeOptions::default(),
            n if n >= 0 => OptimizeOptions::merge(n as usize),
            _ => {
                set_custom_error_message(
                    error_message,
                    "Invalid argument: num_indices_to_merge must be -1 or non-negative",
                );
                return Err(LanceDBError::InvalidArgument);
            }
        },
        LanceDBReindexMode::Retrain => OptimizeOptions::retrain(),
    };

    if opts.num_index_names > 0 {
        let names = if opts.index_names.is_null() {
            None
        } else {
            c_str_array_to_vec(opts.index_names, opts.num_index_names)
        };
        let Some(names) = names else {
            set_invalid_argument_message(error_message);
            return Err(LanceDBError::InvalidArgument);
        };
        options = options.index_names(names);
    }

    if opts.num_transaction_properties > 0 {
        if opts.transaction_property_keys.is_null() || opts.transaction_property_values.is_null() {
            set_invalid_argument_message(error_message);
            return Err(LanceDBError::InvalidArgument);
        }
        let keys = c_str_array_to_vec(
            opts.transaction_property_keys,
            opts.num_transaction_properties,
        );
        let values = c_str_array_to_vec(
            opts.transaction_property_values,
            opts.num_transaction_properties,
        );
        let (Some(keys), Some(values)) = (keys, values) else {
            set_invalid_argument_message(error_message);
            return Err(LanceDBError::InvalidArgument);
        };
        let properties: HashMap<String, String> = keys.into_iter().zip(values).collect();
        options = options.transaction_properties(properties);
    }

    Ok(options)
}

/// Optimize (reindex) the table's indices with explicit options
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `options` can be NULL for the defaults; its arrays must point to as many valid C
///   strings as their counts say
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` if the indices were optimized
/// - `LANCEDB_INVALID_ARGUMENT` for a bad mode, count or array combination
/// - `LANCEDB_INDEX_NOT_FOUND` if a named index does not exist
/// - `LANCEDB_NOT_SUPPORTED` on a remote table, or when the mode is not supported for the
///   index type by the underlying Lance version
/// - Other error codes on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_optimize_indices(
    table: *const LanceDBTable,
    options: *const LanceDBOptimizeIndexOptions,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let opts = if options.is_null() {
        LanceDBOptimizeIndexOptions::default()
    } else {
        (*options).clone()
    };

    let optimize_options = match build_optimize_options(&opts, error_message) {
        Ok(options) => options,
        Err(e) => return e,
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    // Lance silently skips the names of indices that do not exist, so they are
    // checked here, to fail the same way lancedb_table_index_stats does
    if let Some(names) = &optimize_options.index_names {
        let existing = match runtime.block_on(tbl.list_indices()) {
            Ok(indices) => indices,
            Err(e) => return handle_error(&e, error_message),
        };
        if let Some(missing) = names
            .iter()
            .find(|name| !existing.iter().any(|index| &index.name == *name))
        {
            set_custom_error_message(error_message, &format!("Index not found: {missing}"));
            return LanceDBError::IndexNotFound;
        }
    }

    match runtime.block_on(tbl.optimize(OptimizeAction::Index(optimize_options))) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Create a vector index on table columns
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `columns` must be an array of valid null-terminated C strings
/// - `num_columns` must match the actual number of columns in the array
/// - `config` can be NULL for defaults
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_create_vector_index(
    table: *const LanceDBTable,
    columns: *const *const c_char,
    num_columns: usize,
    index_type: LanceDBIndexType,
    config: *const LanceDBVectorIndexConfig,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || columns.is_null() || num_columns == 0 {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(column_names) = c_str_array_to_vec(columns, num_columns) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    // Use default config if none provided
    let cfg = if config.is_null() {
        LanceDBVectorIndexConfig::default()
    } else {
        (*config).clone()
    };

    let index = match index_type {
        LanceDBIndexType::Auto => Index::Auto,
        LanceDBIndexType::IvfFlat => {
            Index::IvfFlat(apply_ivf_config!(IvfFlatIndexBuilder::default(), cfg))
        }
        LanceDBIndexType::IvfSq => {
            Index::IvfSq(apply_ivf_config!(IvfSqIndexBuilder::default(), cfg))
        }
        LanceDBIndexType::IvfRq => {
            Index::IvfRq(apply_ivf_config!(IvfRqIndexBuilder::default(), cfg))
        }
        LanceDBIndexType::IvfPq => {
            let mut builder = apply_ivf_config!(IvfPqIndexBuilder::default(), cfg);
            if cfg.num_sub_vectors > 0 {
                builder = builder.num_sub_vectors(cfg.num_sub_vectors as u32);
            }
            Index::IvfPq(builder)
        }
        LanceDBIndexType::IvfHnswPq => {
            let mut builder = apply_ivf_config!(IvfHnswPqIndexBuilder::default(), cfg);
            if cfg.num_sub_vectors > 0 {
                builder = builder.num_sub_vectors(cfg.num_sub_vectors as u32);
            }
            Index::IvfHnswPq(builder)
        }
        LanceDBIndexType::IvfHnswSq => {
            Index::IvfHnswSq(apply_ivf_config!(IvfHnswSqIndexBuilder::default(), cfg))
        }
        LanceDBIndexType::IvfHnswFlat => {
            Index::IvfHnswFlat(apply_ivf_config!(IvfHnswFlatIndexBuilder::default(), cfg))
        }
        _ => {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }
    };

    match runtime.block_on(async {
        let mut index_builder = tbl.create_index(&column_names, index);
        if cfg.replace == 0 {
            index_builder = index_builder.replace(false);
        }
        index_builder.execute().await
    }) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Create a scalar index on table columns
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `columns` must be an array of valid null-terminated C strings
/// - `num_columns` must match the actual number of columns in the array
/// - `config` can be NULL for defaults
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_create_scalar_index(
    table: *const LanceDBTable,
    columns: *const *const c_char,
    num_columns: usize,
    index_type: LanceDBIndexType,
    config: *const LanceDBScalarIndexConfig,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || columns.is_null() || num_columns == 0 {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(column_names) = c_str_array_to_vec(columns, num_columns) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    // Use default config if none provided
    let cfg = if config.is_null() {
        LanceDBScalarIndexConfig::default()
    } else {
        (*config).clone()
    };

    let index = match index_type {
        LanceDBIndexType::BTree => {
            let builder = BTreeIndexBuilder::default();
            Index::BTree(builder)
        }
        LanceDBIndexType::Bitmap => Index::Bitmap(BitmapIndexBuilder::default()),
        LanceDBIndexType::LabelList => Index::LabelList(LabelListIndexBuilder::default()),
        LanceDBIndexType::Fm => Index::Fm(FmIndexBuilder::default()),
        _ => {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }
    };

    match runtime.block_on(async {
        let mut index_builder = tbl.create_index(&column_names, index);
        if cfg.replace == 0 {
            index_builder = index_builder.replace(false);
        }
        index_builder.execute().await
    }) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Create a full-text search index on table columns
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `columns` must be an array of valid null-terminated C strings
/// - `num_columns` must match the actual number of columns in the array
/// - `config` can be NULL for defaults
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_create_fts_index(
    table: *const LanceDBTable,
    columns: *const *const c_char,
    num_columns: usize,
    config: *const LanceDBFtsIndexConfig,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || columns.is_null() || num_columns == 0 {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(column_names) = c_str_array_to_vec(columns, num_columns) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    // Use default config if none provided
    let cfg = if config.is_null() {
        LanceDBFtsIndexConfig::default()
    } else {
        (*config).clone()
    };

    let mut builder = FtsIndexBuilder::default();

    if !cfg.base_tokenizer.is_null() {
        if let Ok(tokenizer_str) = CStr::from_ptr(cfg.base_tokenizer).to_str() {
            builder = builder.base_tokenizer(tokenizer_str.to_string());
        }
    }

    if !cfg.language.is_null() {
        if let Ok(lang_str) = CStr::from_ptr(cfg.language).to_str() {
            match builder.language(lang_str) {
                Ok(new_builder) => builder = new_builder,
                Err(_) => {
                    set_invalid_argument_message(error_message);
                    return LanceDBError::InvalidArgument;
                }
            }
        }
    }

    // a negative value leaves the token length unlimited
    let max_token_length = if cfg.max_token_length < 0 {
        None
    } else {
        Some(cfg.max_token_length as usize)
    };

    builder = builder
        .max_token_length(max_token_length)
        .lower_case(cfg.lowercase != 0)
        .stem(cfg.stem != 0)
        .remove_stop_words(cfg.remove_stop_words != 0)
        .ascii_folding(cfg.ascii_folding != 0);

    let index = Index::FTS(builder);

    match runtime.block_on(async {
        let mut index_builder = tbl.create_index(&column_names, index);
        if cfg.replace == 0 {
            index_builder = index_builder.replace(false);
        }
        index_builder.execute().await
    }) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// List all indices on the table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `indices_out` must be a valid pointer to receive the array of index info strings
/// - `count_out` must be a valid pointer to receive the count
/// - `error_message` can be NULL to ignore detailed error messages
/// - The caller is responsible for freeing the returned strings and array
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_list_indices(
    table: *const LanceDBTable,
    indices_out: *mut *mut *mut c_char,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || indices_out.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.list_indices()) {
        Ok(indices) => {
            let count = indices.len();
            *count_out = count;

            if count == 0 {
                *indices_out = ptr::null_mut();
                return LanceDBError::Success;
            }

            // Allocate array of string pointers
            let indices_array =
                libc::malloc(count * std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
            if indices_array.is_null() {
                set_unknown_error_message(error_message);
                return LanceDBError::Unknown;
            }

            // Convert each index info to string representation
            for (i, index_info) in indices.into_iter().enumerate() {
                let info_str = index_info.name;

                match CString::new(info_str) {
                    Ok(c_str) => {
                        *indices_array.add(i) = c_str.into_raw();
                    }
                    Err(_) => {
                        // Clean up already allocated strings
                        for j in 0..i {
                            let _ = CString::from_raw(*indices_array.add(j));
                        }
                        libc::free(indices_array as *mut libc::c_void);
                        set_unknown_error_message(error_message);
                        return LanceDBError::Unknown;
                    }
                }
            }

            *indices_out = indices_array;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Drop an index
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `index_name` must be a valid null-terminated C string
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_drop_index(
    table: *const LanceDBTable,
    index_name: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || index_name.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(index_name_str) = CStr::from_ptr(index_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.drop_index(index_name_str)) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Optimize table (rebuild indices and compact files)
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_optimize(
    table: *const LanceDBTable,
    optimize_type: LanceDBOptimizeType,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    let action = match optimize_type {
        LanceDBOptimizeType::All => OptimizeAction::All,
        LanceDBOptimizeType::Compat => OptimizeAction::Compact {
            options: Default::default(),
            remap_options: None,
        },
        LanceDBOptimizeType::Prune => OptimizeAction::Prune {
            older_than: None,
            delete_unverified: None,
            error_if_tagged_old_versions: None,
        },
        LanceDBOptimizeType::Index => {
            match build_optimize_options(&LanceDBOptimizeIndexOptions::default(), error_message) {
                Ok(options) => OptimizeAction::Index(options),
                Err(e) => return e,
            }
        }
    };

    match runtime.block_on(tbl.optimize(action)) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Free index list array returned by lancedb_table_list_indices
///
/// # Safety
/// - `indices` must be a pointer returned by `lancedb_table_list_indices`
/// - `count` must match the count returned by `lancedb_table_list_indices`
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_index_list(indices: *mut *mut c_char, count: usize) {
    if !indices.is_null() {
        for i in 0..count {
            let index_ptr = *indices.add(i);
            if !index_ptr.is_null() {
                let _ = CString::from_raw(index_ptr);
            }
        }
        libc::free(indices as *mut libc::c_void);
    }
}

/// Index statistics returned by lancedb_table_index_stats
#[repr(C)]
#[derive(Debug)]
pub struct LanceDBIndexStats {
    pub num_indexed_rows: usize,
    pub num_unindexed_rows: usize,
    pub num_indices: u32,
    pub num_indexed_fragments: usize,
    pub num_unindexed_fragments: usize,
    pub num_deleted_indexed_rows: usize,
    pub index_type: LanceDBIndexType,
    pub num_partitions: u32,
    pub dataset_version: u64,
}

/// The part of the index statistics that LanceDB does not compute, read from the
/// manifest of a native table
#[derive(Debug, Default)]
struct ManifestIndexStats {
    num_indexed_fragments: usize,
    num_unindexed_fragments: usize,
    num_deleted_indexed_rows: usize,
    num_partitions: u32,
    dataset_version: u64,
}

/// Read the manifest-level statistics of a named index
///
/// The fragment coverage, the deleted rows and the dataset version are taken from the
/// index metadata in the manifest, with no data reads. The number of partitions is only
/// known to the vector index itself, so for a vector index the index statistics of lance
/// are consulted as well
async fn manifest_index_stats(
    dataset: &lance::Dataset,
    index_name: &str,
    is_vector: bool,
) -> lancedb::Result<ManifestIndexStats> {
    let segments = dataset.load_indices_by_name(index_name).await?;
    let covered = |fragment_id: u64| {
        segments.iter().any(|segment| {
            segment
                .fragment_bitmap
                .as_ref()
                .is_some_and(|bitmap| bitmap.contains(fragment_id as u32))
        })
    };

    let mut stats = ManifestIndexStats {
        dataset_version: segments
            .iter()
            .map(|segment| segment.dataset_version)
            .max()
            .unwrap_or(0),
        ..Default::default()
    };
    for fragment in dataset.fragments().iter() {
        if covered(fragment.id) {
            stats.num_indexed_fragments += 1;
            stats.num_deleted_indexed_rows += fragment
                .deletion_file
                .as_ref()
                .and_then(|deletion_file| deletion_file.num_deleted_rows)
                .unwrap_or(0);
        } else {
            stats.num_unindexed_fragments += 1;
        }
    }

    if is_vector {
        // the statistics of a vector index list its segments, each with its own
        // partition count
        let json = dataset.index_statistics(index_name).await?;
        let value: serde_json::Value =
            serde_json::from_str(&json).map_err(|e| lancedb::Error::Runtime {
                message: format!("Failed to parse index statistics: {e}"),
            })?;
        stats.num_partitions = value["indices"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|segment| segment["num_partitions"].as_u64())
            .max()
            .unwrap_or(0) as u32;
    }

    Ok(stats)
}

/// Get statistics for a named index on the table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `index_name` must be a valid null-terminated C string
/// - `stats_out` must be a valid pointer to receive the statistics
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` if stats were retrieved
/// - `LANCEDB_INDEX_NOT_FOUND` if the index does not exist
/// - Other error codes on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_index_stats(
    table: *const LanceDBTable,
    index_name: *const c_char,
    stats_out: *mut LanceDBIndexStats,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || index_name.is_null() || stats_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(name_str) = CStr::from_ptr(index_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    let stats = match runtime.block_on(tbl.index_stats(name_str)) {
        Ok(Some(stats)) => stats,
        Ok(None) => {
            set_custom_error_message(error_message, &format!("Index not found: {name_str}"));
            return LanceDBError::IndexNotFound;
        }
        Err(e) => return handle_error(&e, error_message),
    };

    // the manifest is only reachable on a native table; a remote table reports zeros
    let manifest_stats = match tbl.dataset() {
        Some(ds) => match runtime.block_on(async {
            let dataset = ds.get().await?;
            manifest_index_stats(&dataset, name_str, stats.distance_type.is_some()).await
        }) {
            Ok(manifest_stats) => manifest_stats,
            Err(e) => return handle_error(&e, error_message),
        },
        None => ManifestIndexStats::default(),
    };

    *stats_out = LanceDBIndexStats {
        num_indexed_rows: stats.num_indexed_rows,
        num_unindexed_rows: stats.num_unindexed_rows,
        num_indices: stats.num_indices.unwrap_or(0),
        num_indexed_fragments: manifest_stats.num_indexed_fragments,
        num_unindexed_fragments: manifest_stats.num_unindexed_fragments,
        num_deleted_indexed_rows: manifest_stats.num_deleted_indexed_rows,
        index_type: stats.index_type.into(),
        num_partitions: manifest_stats.num_partitions,
        dataset_version: manifest_stats.dataset_version,
    };
    LanceDBError::Success
}
