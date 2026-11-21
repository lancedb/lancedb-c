// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Full Index API implementation for LanceDB C bindings
//!
//! This module provides complete index management operations

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_float, c_int};
use std::ptr;
use std::time::Duration;

use lancedb::index::scalar::{
    BTreeIndexBuilder, BitmapIndexBuilder, FtsIndexBuilder, LabelListIndexBuilder,
};
use lancedb::index::vector::{
    IvfFlatIndexBuilder, IvfHnswPqIndexBuilder, IvfHnswSqIndexBuilder, IvfPqIndexBuilder,
};
use lancedb::index::Index;

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
}

impl LanceDBIndexType {
    /// Convert the raw C value, rejecting anything outside the public header.
    pub(crate) fn from_c_int(value: c_int) -> Option<Self> {
        match value {
            0 => Some(Self::Auto),
            1 => Some(Self::BTree),
            2 => Some(Self::Bitmap),
            3 => Some(Self::LabelList),
            4 => Some(Self::FTS),
            5 => Some(Self::IvfFlat),
            6 => Some(Self::IvfPq),
            7 => Some(Self::IvfHnswPq),
            8 => Some(Self::IvfHnswSq),
            _ => None,
        }
    }
}

/// Configuration for vector indices
#[repr(C)]
#[derive(Clone)]
pub struct LanceDBVectorIndexConfig {
    pub num_partitions: c_int, // Number of partitions for IVF indices (-1 = auto)
    pub num_sub_vectors: c_int, // Number of sub-vectors for PQ indices (-1 = auto)
    pub max_iterations: c_int, // Maximum training iterations (-1 = default)
    pub sample_rate: c_float,  // Sampling rate for training (0.0 = default)
    pub distance_type: c_int,  // Raw C distance type; validated before conversion
    pub replace: c_int,        // Replace existing index (1 = true, 0 = false)
}

impl Default for LanceDBVectorIndexConfig {
    fn default() -> Self {
        Self {
            num_partitions: -1,
            num_sub_vectors: -1,
            max_iterations: -1,
            sample_rate: 0.0,
            distance_type: LanceDBDistanceType::L2 as c_int,
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

/// Options for creating any supported index through `lancedb_table_create_index`
///
/// The SDK defaults are left in place for every field set to -1 (or a null
/// string). A value of 0 is an explicit zero, not an omitted parameter. Boolean
/// fields use -1 for unset, 0 for false and 1 for true.
#[repr(C)]
#[derive(Clone)]
pub struct LanceDBIndexOptions {
    pub name: *const c_char,
    pub replace: c_int,
    pub wait_timeout_ms: u64,

    // IVF tuning
    pub num_partitions: i64,
    pub sample_rate: i64,
    pub max_iterations: i64,
    pub target_partition_size: i64,

    // PQ tuning
    pub num_sub_vectors: i64,
    pub num_bits: i64,

    // HNSW tuning
    pub m: i64,
    pub ef_construction: i64,

    // Vector distance metric
    pub distance_type: c_int,

    // FTS tuning
    pub fts_base_tokenizer: *const c_char,
    pub fts_language: *const c_char,
    pub fts_max_token_length: i64,
    pub fts_lower_case: c_int,
    pub fts_stem: c_int,
    pub fts_remove_stop_words: c_int,
    pub fts_ascii_folding: c_int,
    pub fts_with_position: c_int,
    pub fts_ngram_min_length: i64,
    pub fts_ngram_max_length: i64,
    pub fts_ngram_prefix_only: c_int,
}

impl Default for LanceDBIndexOptions {
    fn default() -> Self {
        Self {
            name: ptr::null(),
            replace: -1,
            wait_timeout_ms: 0,
            num_partitions: -1,
            sample_rate: -1,
            max_iterations: -1,
            target_partition_size: -1,
            num_sub_vectors: -1,
            num_bits: -1,
            m: -1,
            ef_construction: -1,
            distance_type: -1,
            fts_base_tokenizer: ptr::null(),
            fts_language: ptr::null(),
            fts_max_token_length: -1,
            fts_lower_case: -1,
            fts_stem: -1,
            fts_remove_stop_words: -1,
            fts_ascii_folding: -1,
            fts_with_position: -1,
            fts_ngram_min_length: -1,
            fts_ngram_max_length: -1,
            fts_ngram_prefix_only: -1,
        }
    }
}

/// Return the default index creation options
///
/// Every optional field is unset and the SDK defaults apply.
#[no_mangle]
pub extern "C" fn lancedb_index_options_default() -> LanceDBIndexOptions {
    LanceDBIndexOptions::default()
}

/// Convert a raw C distance type value, preserving -1 as the unset marker
fn optional_distance_type(value: c_int) -> Result<Option<LanceDBDistanceType>, String> {
    if value == -1 {
        return Ok(None);
    }
    match LanceDBDistanceType::from_c_int(value) {
        Some(distance_type) => Ok(Some(distance_type)),
        None => Err(format!(
            "distance_type must be -1 to leave it unset or a LanceDBDistanceType value, got {value}"
        )),
    }
}

fn optional_u32(value: i64, field: &str) -> Result<Option<u32>, String> {
    if value == -1 {
        return Ok(None);
    }
    if value < 0 || value > u32::MAX as i64 {
        return Err(format!(
            "{field} must be -1 to leave it unset or between 0 and {}, got {value}",
            u32::MAX
        ));
    }
    Ok(Some(value as u32))
}

fn optional_bool(value: c_int, field: &str) -> Result<Option<bool>, String> {
    match value {
        -1 => Ok(None),
        0 => Ok(Some(false)),
        1 => Ok(Some(true)),
        _ => Err(format!(
            "{field} must be -1 to leave it unset, 0 for false or 1 for true, got {value}"
        )),
    }
}

/// Read a borrowed C string, leaving a null pointer unset
///
/// # Safety
/// `value` must be null or point to a valid null-terminated C string
unsafe fn optional_string(value: *const c_char, field: &str) -> Result<Option<String>, String> {
    if value.is_null() {
        return Ok(None);
    }
    match CStr::from_ptr(value).to_str() {
        Ok(value) => Ok(Some(value.to_string())),
        Err(_) => Err(format!("{field} must be valid UTF-8")),
    }
}

/// Build the SDK index represented by the options
///
/// Fields that do not apply to `index_type` are ignored, matching the SDK
/// builders that do not have those setters.
///
/// # Safety
/// The string fields in `options` must be null or valid C strings
unsafe fn build_index_from_options(
    index_type: LanceDBIndexType,
    options: &LanceDBIndexOptions,
) -> Result<Index, String> {
    macro_rules! apply_ivf_options {
        ($builder:expr) => {{
            let mut builder = $builder;
            if let Some(value) = optional_u32(options.num_partitions, "num_partitions")? {
                builder = builder.num_partitions(value);
            }
            if let Some(value) = optional_u32(options.sample_rate, "sample_rate")? {
                builder = builder.sample_rate(value);
            }
            if let Some(value) = optional_u32(options.max_iterations, "max_iterations")? {
                builder = builder.max_iterations(value);
            }
            if let Some(value) =
                optional_u32(options.target_partition_size, "target_partition_size")?
            {
                builder = builder.target_partition_size(value);
            }
            if let Some(distance_type) = optional_distance_type(options.distance_type)? {
                builder = builder.distance_type(distance_type.into());
            }
            builder
        }};
    }

    match index_type {
        LanceDBIndexType::Auto => Ok(Index::Auto),
        LanceDBIndexType::IvfFlat => Ok(Index::IvfFlat(apply_ivf_options!(
            IvfFlatIndexBuilder::default()
        ))),
        LanceDBIndexType::IvfPq => {
            let mut builder = apply_ivf_options!(IvfPqIndexBuilder::default());
            if let Some(value) = optional_u32(options.num_sub_vectors, "num_sub_vectors")? {
                builder = builder.num_sub_vectors(value);
            }
            if let Some(value) = optional_u32(options.num_bits, "num_bits")? {
                builder = builder.num_bits(value);
            }
            Ok(Index::IvfPq(builder))
        }
        LanceDBIndexType::IvfHnswPq => {
            let mut builder = apply_ivf_options!(IvfHnswPqIndexBuilder::default());
            if let Some(value) = optional_u32(options.m, "m")? {
                builder = builder.num_edges(value);
            }
            if let Some(value) = optional_u32(options.ef_construction, "ef_construction")? {
                builder = builder.ef_construction(value);
            }
            if let Some(value) = optional_u32(options.num_sub_vectors, "num_sub_vectors")? {
                builder = builder.num_sub_vectors(value);
            }
            if let Some(value) = optional_u32(options.num_bits, "num_bits")? {
                builder = builder.num_bits(value);
            }
            Ok(Index::IvfHnswPq(builder))
        }
        LanceDBIndexType::IvfHnswSq => {
            let mut builder = apply_ivf_options!(IvfHnswSqIndexBuilder::default());
            if let Some(value) = optional_u32(options.m, "m")? {
                builder = builder.num_edges(value);
            }
            if let Some(value) = optional_u32(options.ef_construction, "ef_construction")? {
                builder = builder.ef_construction(value);
            }
            Ok(Index::IvfHnswSq(builder))
        }
        LanceDBIndexType::BTree => Ok(Index::BTree(BTreeIndexBuilder::default())),
        LanceDBIndexType::Bitmap => Ok(Index::Bitmap(BitmapIndexBuilder::default())),
        LanceDBIndexType::LabelList => Ok(Index::LabelList(LabelListIndexBuilder::default())),
        LanceDBIndexType::FTS => {
            let mut builder = FtsIndexBuilder::default();
            if let Some(value) = optional_string(options.fts_base_tokenizer, "fts_base_tokenizer")?
            {
                builder = builder.base_tokenizer(value);
            }
            if let Some(value) = optional_string(options.fts_language, "fts_language")? {
                builder = builder
                    .language(&value)
                    .map_err(|error| format!("Invalid FTS language: {error}"))?;
            }
            if let Some(value) = optional_u32(options.fts_max_token_length, "fts_max_token_length")?
            {
                builder = builder.max_token_length(Some(value as usize));
            }
            if let Some(value) = optional_bool(options.fts_lower_case, "fts_lower_case")? {
                builder = builder.lower_case(value);
            }
            if let Some(value) = optional_bool(options.fts_stem, "fts_stem")? {
                builder = builder.stem(value);
            }
            if let Some(value) =
                optional_bool(options.fts_remove_stop_words, "fts_remove_stop_words")?
            {
                builder = builder.remove_stop_words(value);
            }
            if let Some(value) = optional_bool(options.fts_ascii_folding, "fts_ascii_folding")? {
                builder = builder.ascii_folding(value);
            }
            if let Some(value) = optional_bool(options.fts_with_position, "fts_with_position")? {
                builder = builder.with_position(value);
            }
            if let Some(value) = optional_u32(options.fts_ngram_min_length, "fts_ngram_min_length")?
            {
                builder = builder.ngram_min_length(value);
            }
            if let Some(value) = optional_u32(options.fts_ngram_max_length, "fts_ngram_max_length")?
            {
                builder = builder.ngram_max_length(value);
            }
            if let Some(value) =
                optional_bool(options.fts_ngram_prefix_only, "fts_ngram_prefix_only")?
            {
                builder = builder.ngram_prefix_only(value);
            }
            Ok(Index::FTS(builder))
        }
        _ => Err(format!(
            "Index type {index_type:?} cannot be created through lancedb_table_create_index"
        )),
    }
}

/// Create an index with a custom name, replace choice, creation wait and
/// per-type tuning parameters
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `columns` must be an array of valid null-terminated C strings
/// - `num_columns` must match the actual number of columns in the array
/// - `options` can be NULL for the SDK defaults
/// - String fields in `options` are borrowed for the duration of the call
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_create_index(
    table: *const LanceDBTable,
    columns: *const *const c_char,
    num_columns: usize,
    index_type: c_int,
    options: *const LanceDBIndexOptions,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || columns.is_null() || num_columns == 0 {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(index_type) = LanceDBIndexType::from_c_int(index_type) else {
        set_custom_error_message(error_message, "index_type is not a LanceDBIndexType value");
        return LanceDBError::InvalidArgument;
    };

    let mut column_names = Vec::with_capacity(num_columns);
    for i in 0..num_columns {
        let column_ptr = *columns.add(i);
        if column_ptr.is_null() {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        }
        match CStr::from_ptr(column_ptr).to_str() {
            Ok(column) => column_names.push(column.to_string()),
            Err(_) => {
                set_custom_error_message(error_message, "columns must be valid UTF-8");
                return LanceDBError::InvalidArgument;
            }
        }
    }

    let options = if options.is_null() {
        LanceDBIndexOptions::default()
    } else {
        (*options).clone()
    };

    let name = match optional_string(options.name, "name") {
        Ok(Some(name)) if !name.is_empty() => Some(name),
        Ok(_) => None,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return LanceDBError::InvalidArgument;
        }
    };
    let replace = match optional_bool(options.replace, "replace") {
        Ok(replace) => replace,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return LanceDBError::InvalidArgument;
        }
    };
    let index = match build_index_from_options(index_type, &options) {
        Ok(index) => index,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return LanceDBError::InvalidArgument;
        }
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    let outcome = runtime.block_on(async {
        let mut builder = tbl.create_index(&column_names, index);
        if let Some(name) = name {
            builder = builder.name(name);
        }
        if let Some(replace) = replace {
            builder = builder.replace(replace);
        }
        if options.wait_timeout_ms > 0 {
            builder = builder.wait_timeout(Duration::from_millis(options.wait_timeout_ms));
        }
        builder.execute().await
    });

    match outcome {
        Ok(()) => LanceDBError::Success,
        Err(error) => handle_error(&error, error_message),
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

impl LanceDBOptimizeType {
    /// Convert the raw C value, rejecting anything outside the public header.
    pub(crate) fn from_c_int(value: c_int) -> Option<Self> {
        match value {
            0 => Some(Self::All),
            1 => Some(Self::Compat),
            2 => Some(Self::Prune),
            3 => Some(Self::Index),
            _ => None,
        }
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
    index_type: c_int,
    config: *const LanceDBVectorIndexConfig,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || columns.is_null() || num_columns == 0 {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(index_type) = LanceDBIndexType::from_c_int(index_type) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

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
    let Some(distance_type) = LanceDBDistanceType::from_c_int(cfg.distance_type) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let index = match index_type {
        LanceDBIndexType::Auto => Index::Auto,
        LanceDBIndexType::IvfFlat => {
            let mut builder = IvfFlatIndexBuilder::default();
            if cfg.num_partitions > 0 {
                builder = builder.num_partitions(cfg.num_partitions as u32);
            }
            if cfg.max_iterations > 0 {
                builder = builder.max_iterations(cfg.max_iterations as u32);
            }
            if cfg.sample_rate > 0.0 {
                builder = builder.sample_rate(cfg.sample_rate as u32);
            }
            builder = builder.distance_type(distance_type.into());

            Index::IvfFlat(builder)
        }
        LanceDBIndexType::IvfPq => {
            let mut builder = IvfPqIndexBuilder::default();
            if cfg.num_partitions > 0 {
                builder = builder.num_partitions(cfg.num_partitions as u32);
            }
            if cfg.num_sub_vectors > 0 {
                builder = builder.num_sub_vectors(cfg.num_sub_vectors as u32);
            }
            if cfg.max_iterations > 0 {
                builder = builder.max_iterations(cfg.max_iterations as u32);
            }
            if cfg.sample_rate > 0.0 {
                builder = builder.sample_rate(cfg.sample_rate as u32);
            }
            builder = builder.distance_type(distance_type.into());

            Index::IvfPq(builder)
        }
        LanceDBIndexType::IvfHnswPq => {
            let mut builder = IvfHnswPqIndexBuilder::default();
            if cfg.num_partitions > 0 {
                builder = builder.num_partitions(cfg.num_partitions as u32);
            }
            if cfg.num_sub_vectors > 0 {
                builder = builder.num_sub_vectors(cfg.num_sub_vectors as u32);
            }
            if cfg.max_iterations > 0 {
                builder = builder.max_iterations(cfg.max_iterations as u32);
            }
            if cfg.sample_rate > 0.0 {
                builder = builder.sample_rate(cfg.sample_rate as u32);
            }
            builder = builder.distance_type(distance_type.into());

            Index::IvfHnswPq(builder)
        }
        LanceDBIndexType::IvfHnswSq => {
            let mut builder = IvfHnswSqIndexBuilder::default();
            if cfg.num_partitions > 0 {
                builder = builder.num_partitions(cfg.num_partitions as u32);
            }
            if cfg.max_iterations > 0 {
                builder = builder.max_iterations(cfg.max_iterations as u32);
            }
            if cfg.sample_rate > 0.0 {
                builder = builder.sample_rate(cfg.sample_rate as u32);
            }
            builder = builder.distance_type(distance_type.into());

            Index::IvfHnswSq(builder)
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
    index_type: c_int,
    config: *const LanceDBScalarIndexConfig,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || columns.is_null() || num_columns == 0 {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(index_type) = LanceDBIndexType::from_c_int(index_type) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

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

/// Prewarm a named index
///
/// Loads the index into memory and caches it, so the first query does not pay
/// for the load.
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `index_name` must be a valid null-terminated C string
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` if the index was prewarmed
/// - `LANCEDB_INDEX_NOT_FOUND` if the index does not exist
/// - Other error codes on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_prewarm_index(
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

    match runtime.block_on(tbl.prewarm_index(index_name_str)) {
        Ok(_) => LanceDBError::Success,
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
    optimize_type: c_int,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(optimize_type) = LanceDBOptimizeType::from_c_int(optimize_type) else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

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

/// Information about one index on a table
#[repr(C)]
#[derive(Debug)]
pub struct LanceDBIndexInfo {
    name: *mut c_char,
    columns: *mut *mut c_char,
    num_columns: usize,
    index_type: *mut c_char,
}

impl Drop for LanceDBIndexInfo {
    fn drop(&mut self) {
        unsafe {
            drop(CString::from_raw(self.name));
            for i in 0..self.num_columns {
                drop(CString::from_raw(*self.columns.add(i)));
            }
            libc::free(self.columns as *mut libc::c_void);
            drop(CString::from_raw(self.index_type));
        }
    }
}

/// Build one C index description from the SDK's index configuration
///
/// Temporary values stay owned by Rust until the complete C result can be
/// constructed, so a conversion failure releases them automatically.
unsafe fn index_info_from_sdk(index: lancedb::index::IndexConfig) -> Result<LanceDBIndexInfo, ()> {
    let name = CString::new(index.name).map_err(|_| ())?;
    let index_type = CString::new(index.index_type.to_string()).map_err(|_| ())?;
    let columns = index
        .columns
        .into_iter()
        .map(CString::new)
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| ())?;

    let num_columns = columns.len();
    // Allocate at least one slot so a returned entry always has a freeable
    // column array, even if the SDK reports no column names.
    let column_ptrs =
        libc::calloc(num_columns.max(1), std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
    if column_ptrs.is_null() {
        return Err(());
    }

    for (i, column) in columns.into_iter().enumerate() {
        *column_ptrs.add(i) = column.into_raw();
    }

    Ok(LanceDBIndexInfo {
        name: name.into_raw(),
        columns: column_ptrs,
        num_columns,
        index_type: index_type.into_raw(),
    })
}

/// List every index on the table with its name, columns and SDK index type
///
/// The names-only lancedb_table_list_indices remains available for callers that
/// only need the names.
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `indices_out` must be a valid pointer to receive the array of index information
/// - `count_out` must be a valid pointer to receive the count
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_list_indices_info(
    table: *const LanceDBTable,
    indices_out: *mut *const LanceDBIndexInfo,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || indices_out.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    *indices_out = ptr::null();
    *count_out = 0;

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.list_indices()) {
        Ok(indices) => {
            if indices.is_empty() {
                return LanceDBError::Success;
            }

            let mut infos = Vec::with_capacity(indices.len());
            for index in indices {
                match index_info_from_sdk(index) {
                    Ok(info) => infos.push(info),
                    Err(_) => {
                        set_unknown_error_message(error_message);
                        return LanceDBError::Unknown;
                    }
                }
            }

            *count_out = infos.len();
            *indices_out = Box::into_raw(infos.into_boxed_slice()) as *const LanceDBIndexInfo;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Free index information array returned by lancedb_table_list_indices_info
///
/// # Safety
/// - `indices` must be a pointer returned by `lancedb_table_list_indices_info`
/// - `count` must match the count returned by `lancedb_table_list_indices_info`
/// - Safe to call with a NULL pointer
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_index_info(indices: *const LanceDBIndexInfo, count: usize) {
    if indices.is_null() {
        return;
    }

    let slice = std::ptr::slice_from_raw_parts_mut(indices as *mut LanceDBIndexInfo, count);
    drop(Box::from_raw(slice));
}

/// Index statistics returned by lancedb_table_index_stats
#[repr(C)]
#[derive(Debug)]
pub struct LanceDBIndexStats {
    pub num_indexed_rows: usize,
    pub num_unindexed_rows: usize,
    pub num_indices: u32,
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

    match runtime.block_on(tbl.index_stats(name_str)) {
        Ok(Some(stats)) => {
            (*stats_out).num_indexed_rows = stats.num_indexed_rows;
            (*stats_out).num_unindexed_rows = stats.num_unindexed_rows;
            (*stats_out).num_indices = stats.num_indices.unwrap_or(0);
            LanceDBError::Success
        }
        Ok(None) => {
            // Index not found
            LanceDBError::IndexNotFound
        }
        Err(e) => handle_error(&e, error_message),
    }
}
