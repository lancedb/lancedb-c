// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Full Index API implementation for LanceDB C bindings
//!
//! This module provides complete index management operations

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

    // Extract column names
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

    // Extract column names
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

    // Extract column names
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

    use lancedb::table::{OptimizeAction, OptimizeOptions};

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
        LanceDBOptimizeType::Index => OptimizeAction::Index(OptimizeOptions::default()),
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
