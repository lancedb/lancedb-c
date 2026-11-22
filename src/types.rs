// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Common types shared across LanceDB C bindings modules

use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::ptr;
use std::str::FromStr;

use arrow_array::RecordBatchReader;
use lancedb::connection::LanceFileVersion;
use lancedb::rerankers::NormalizeMethod;
use lancedb::table::WriteOptions;
use lancedb::DistanceType;

use crate::expr::LanceDBExpr;

/// Distance type enum for C API
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub enum LanceDBDistanceType {
    L2 = 0,
    Cosine = 1,
    Dot = 2,
    Hamming = 3,
}

impl LanceDBDistanceType {
    /// Convert the raw C value, rejecting anything outside the public header.
    pub(crate) fn from_c_int(value: c_int) -> Option<Self> {
        match value {
            0 => Some(Self::L2),
            1 => Some(Self::Cosine),
            2 => Some(Self::Dot),
            3 => Some(Self::Hamming),
            _ => None,
        }
    }
}

impl From<LanceDBDistanceType> for DistanceType {
    fn from(dt: LanceDBDistanceType) -> Self {
        match dt {
            LanceDBDistanceType::L2 => Self::L2,
            LanceDBDistanceType::Cosine => Self::Cosine,
            LanceDBDistanceType::Dot => Self::Dot,
            LanceDBDistanceType::Hamming => Self::Hamming,
        }
    }
}

/// Score normalization method for hybrid search reranking
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum LanceDBNormalizeMethod {
    Score = 0,
    Rank = 1,
}

impl LanceDBNormalizeMethod {
    /// Convert the raw C value, rejecting anything outside the public header.
    pub(crate) fn from_c_int(value: c_int) -> Option<Self> {
        match value {
            0 => Some(Self::Score),
            1 => Some(Self::Rank),
            _ => None,
        }
    }
}

impl From<LanceDBNormalizeMethod> for NormalizeMethod {
    fn from(method: LanceDBNormalizeMethod) -> Self {
        match method {
            LanceDBNormalizeMethod::Score => Self::Score,
            LanceDBNormalizeMethod::Rank => Self::Rank,
        }
    }
}

/// Opaque handle to Arrow RecordBatchReader for C interop
#[repr(C)]
pub struct LanceDBRecordBatchReader {
    inner: Box<dyn RecordBatchReader + Send>,
}

impl LanceDBRecordBatchReader {
    /// Create a new reader from a Box<dyn RecordBatchReader + Send>
    pub fn new(reader: Box<dyn RecordBatchReader + Send>) -> Self {
        Self { inner: reader }
    }

    /// Extract the inner reader (consumes self)
    pub fn into_inner(self) -> Box<dyn RecordBatchReader + Send> {
        self.inner
    }
}

/// What happens when `lancedb_table_create_with_options` finds an existing table
///
/// The values mirror the SDK's `CreateTableMode`. A value outside these
/// enumerators is rejected with `LANCEDB_INVALID_ARGUMENT` instead of becoming
/// an invalid Rust enum.
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum LanceDBCreateTableMode {
    /// Return `LANCEDB_TABLE_ALREADY_EXISTS`
    Create = 0,
    /// Replace the existing table with the supplied data
    Overwrite = 1,
    /// Open the existing table and ignore the supplied data
    ExistOk = 2,
}

/// Fields of `LanceDBWriteOptions` that carry an override
///
/// Every bit makes the matching field an override. A field whose bit is not set
/// keeps the SDK default, so an omitted option stays distinct from an explicit
/// zero or false. These values are part of the public header.
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum LanceDBWriteOptionsField {
    /// Use `max_rows_per_file`
    MaxRowsPerFile = 1 << 0,
    /// Use `max_rows_per_group`
    MaxRowsPerGroup = 1 << 1,
    /// Use `max_bytes_per_file`
    MaxBytesPerFile = 1 << 2,
    /// Use `data_storage_version`
    DataStorageVersion = 1 << 3,
    /// Use `enable_stable_row_ids`
    EnableStableRowIds = 1 << 4,
}

const WRITE_MAX_ROWS_PER_FILE: u32 = LanceDBWriteOptionsField::MaxRowsPerFile as u32;
const WRITE_MAX_ROWS_PER_GROUP: u32 = LanceDBWriteOptionsField::MaxRowsPerGroup as u32;
const WRITE_MAX_BYTES_PER_FILE: u32 = LanceDBWriteOptionsField::MaxBytesPerFile as u32;
const WRITE_DATA_STORAGE_VERSION: u32 = LanceDBWriteOptionsField::DataStorageVersion as u32;
const WRITE_ENABLE_STABLE_ROW_IDS: u32 = LanceDBWriteOptionsField::EnableStableRowIds as u32;
const WRITE_ALL_FIELDS: u32 = WRITE_MAX_ROWS_PER_FILE
    | WRITE_MAX_ROWS_PER_GROUP
    | WRITE_MAX_BYTES_PER_FILE
    | WRITE_DATA_STORAGE_VERSION
    | WRITE_ENABLE_STABLE_ROW_IDS;

/// Per-call Lance write parameters shared by table create and add
///
/// `set_fields` selects the fields that are read; every other field keeps the
/// SDK default. A NULL pointer or a struct whose `set_fields` is zero keeps the
/// SDK's `WriteOptions` without Lance write parameters, which is what default
/// table creation requires. The data storage version string is borrowed for the
/// duration of the call and is parsed by the SDK's own version parser.
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct LanceDBWriteOptions {
    /// Bitmask of `LanceDBWriteOptionsField` values; 0 uses every default
    pub set_fields: u32,
    /// Maximum number of rows per data file
    pub max_rows_per_file: u64,
    /// Maximum number of rows per row group
    pub max_rows_per_group: u64,
    /// Maximum number of bytes per data file
    pub max_bytes_per_file: u64,
    /// Data storage version string, for example "2.0" or "legacy" (NULL when unset)
    pub data_storage_version: *const c_char,
    /// Use stable row IDs (1 = true, 0 = false)
    pub enable_stable_row_ids: c_int,
}

impl Default for LanceDBWriteOptions {
    fn default() -> Self {
        Self {
            set_fields: 0,
            max_rows_per_file: 0,
            max_rows_per_group: 0,
            max_bytes_per_file: 0,
            data_storage_version: ptr::null(),
            enable_stable_row_ids: 0,
        }
    }
}

/// Options for `lancedb_table_create_with_options`
///
/// The struct and its strings are borrowed for the duration of the call. A NULL
/// options pointer uses every SDK default. `namespace_path` is a C array of
/// `namespace_path_len` borrowed strings; NULL with length 0 means the root
/// namespace. `storage_keys` and `storage_values` are parallel arrays of
/// `storage_count` borrowed strings for this table only. An empty storage array
/// leaves the SDK's per-table storage options unset.
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct LanceDBCreateTableOptions {
    /// One of the `LanceDBCreateTableMode` values
    pub mode: c_int,
    /// Namespace path parts (NULL with length 0 for the root namespace)
    pub namespace_path: *const *const c_char,
    /// Number of namespace path parts
    pub namespace_path_len: usize,
    /// Custom table location (NULL to derive it from the database URI and name)
    pub location: *const c_char,
    /// Storage option keys for this table (NULL when storage_count is 0)
    pub storage_keys: *const *const c_char,
    /// Storage option values matching storage_keys
    pub storage_values: *const *const c_char,
    /// Number of per-table storage options
    pub storage_count: usize,
    /// Per-call write options (NULL for the SDK defaults)
    pub write_options: *const LanceDBWriteOptions,
    /// Allow blob URIs outside registered bases (1 = true, 0 = false)
    pub allow_external_blob_outside_bases: c_int,
}

impl Default for LanceDBCreateTableOptions {
    fn default() -> Self {
        Self {
            mode: 0,
            namespace_path: ptr::null(),
            namespace_path_len: 0,
            location: ptr::null(),
            storage_keys: ptr::null(),
            storage_values: ptr::null(),
            storage_count: 0,
            write_options: ptr::null(),
            allow_external_blob_outside_bases: 0,
        }
    }
}

/// Build SDK write options from the shared C representation
///
/// A NULL pointer returns the SDK default, whose Lance write parameters stay
/// unset. The caller must only pass a non-NULL pointer to a readable struct.
pub(crate) unsafe fn write_options_from_c(
    options: *const LanceDBWriteOptions,
) -> Result<WriteOptions, String> {
    if options.is_null() {
        return Ok(WriteOptions::default());
    }
    (*options).build_write_options()
}

impl LanceDBWriteOptions {
    unsafe fn build_write_options(&self) -> Result<WriteOptions, String> {
        let unknown = self.set_fields & !WRITE_ALL_FIELDS;
        if unknown != 0 {
            return Err(format!("unknown write option bits: 0x{unknown:x}"));
        }

        let mut options = WriteOptions::default();
        if self.set_fields == 0 {
            return Ok(options);
        }

        let params = options.lance_write_params.get_or_insert_default();
        if self.set_fields & WRITE_MAX_ROWS_PER_FILE != 0 {
            params.max_rows_per_file =
                size_from_u64("write_options.max_rows_per_file", self.max_rows_per_file)?;
        }
        if self.set_fields & WRITE_MAX_ROWS_PER_GROUP != 0 {
            params.max_rows_per_group =
                size_from_u64("write_options.max_rows_per_group", self.max_rows_per_group)?;
        }
        if self.set_fields & WRITE_MAX_BYTES_PER_FILE != 0 {
            params.max_bytes_per_file =
                size_from_u64("write_options.max_bytes_per_file", self.max_bytes_per_file)?;
        }
        if self.set_fields & WRITE_DATA_STORAGE_VERSION != 0 {
            if self.data_storage_version.is_null() {
                return Err(
                    "write_options.data_storage_version must not be NULL when its bit is set"
                        .to_string(),
                );
            }
            let version = CStr::from_ptr(self.data_storage_version)
                .to_str()
                .map_err(|_| "write_options.data_storage_version is not valid UTF-8".to_string())?;
            params.data_storage_version =
                Some(LanceFileVersion::from_str(version).map_err(|error| {
                    format!("invalid write_options.data_storage_version {version:?}: {error}")
                })?);
        }
        if self.set_fields & WRITE_ENABLE_STABLE_ROW_IDS != 0 {
            params.enable_stable_row_ids = bool_from_c_int(
                "write_options.enable_stable_row_ids",
                self.enable_stable_row_ids,
            )?;
        }

        Ok(options)
    }
}

/// Convert a row or byte count from C to the SDK's `usize`
fn size_from_u64(name: &str, value: u64) -> Result<usize, String> {
    usize::try_from(value).map_err(|_| format!("'{}' value {} does not fit in size_t", name, value))
}

/// Convert a C boolean, rejecting values other than 0 and 1
pub(crate) fn bool_from_c_int(name: &str, value: c_int) -> Result<bool, String> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        other => Err(format!("'{}' must be 0 or 1, got {}", name, other)),
    }
}

/// Merge insert configuration
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct LanceDBMergeInsertConfig {
    pub when_matched_update_all: i32, // Update all columns for matched records (1 = true, 0 = false)
    pub when_not_matched_insert_all: i32, // Insert all new records (1 = true, 0 = false)
    /// SQL condition limiting which matched records are updated (NULL for no condition)
    pub when_matched_update_all_condition: *const c_char,
    /// DataFusion expression limiting which matched records are updated (NULL for no condition).
    /// Not consumed by `lancedb_table_merge_insert`, and mutually exclusive with the SQL condition
    pub when_matched_update_all_expr: *mut LanceDBExpr,
}

/// Options for `lancedb_table_merge_insert_with_result`
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct LanceDBMergeInsertOptions {
    /// Update every matched row, optionally limited by the condition below
    pub when_matched_update_all: i32,
    /// SQL condition limiting which matched records are updated (NULL for no condition)
    pub when_matched_update_all_condition: *const c_char,
    /// Insert every source row that has no match in the target
    pub when_not_matched_insert_all: i32,
    /// Delete every target row that has no match in the source, optionally filtered below
    pub when_not_matched_by_source_delete: i32,
    /// SQL filter on unmatched target rows, naming target columns directly (NULL for no filter)
    pub when_not_matched_by_source_delete_filter: *const c_char,
    /// -1 for the SDK default, 0 to disable indexed planning, 1 to allow it.
    /// The embedded and remote backends interpret this as described in the public header
    pub use_index: i32,
    /// -1 for the SDK default, otherwise milliseconds. Embedded covers Lance merge execution
    /// and retries; remote sends it as a per-request timeout header
    pub timeout_ms: i64,
}

/// Result of `lancedb_table_merge_insert_with_result`
///
/// Fields are copied unchanged from the SDK result. A missing remote field defaults to zero,
/// so zero can mean that the backend did not report it. RTE and legacy empty responses can
/// return all fields zero.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct LanceDBMergeInsertResult {
    /// Reported commit version; 0 may mean not reported
    pub version: u64,
    /// Reported number of inserted rows; 0 may mean not reported
    pub num_inserted_rows: u64,
    /// Reported number of updated rows; 0 may mean not reported
    pub num_updated_rows: u64,
    /// Reported number of deleted rows; 0 may mean not reported
    pub num_deleted_rows: u64,
    /// Reported merge attempts; 0 may mean not reported
    pub num_attempts: u32,
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    #[test]
    fn null_and_zero_write_options_keep_lance_params_unset() {
        let from_null = unsafe { write_options_from_c(ptr::null()) }.unwrap();
        assert!(from_null.lance_write_params.is_none());

        let from_zero = unsafe { write_options_from_c(&LanceDBWriteOptions::default()) }.unwrap();
        assert!(from_zero.lance_write_params.is_none());
    }

    #[test]
    fn explicit_write_options_reach_the_sdk_fields() {
        let version = CString::new("2.2").unwrap();
        let options = LanceDBWriteOptions {
            set_fields: WRITE_ALL_FIELDS,
            max_rows_per_file: 11,
            max_rows_per_group: 7,
            max_bytes_per_file: 1024,
            data_storage_version: version.as_ptr(),
            enable_stable_row_ids: 0,
        };

        let sdk = unsafe { write_options_from_c(&options) }.unwrap();
        let params = sdk
            .lance_write_params
            .expect("explicit fields create parameters");
        assert_eq!(params.max_rows_per_file, 11);
        assert_eq!(params.max_rows_per_group, 7);
        assert_eq!(params.max_bytes_per_file, 1024);
        assert_eq!(params.data_storage_version, Some(LanceFileVersion::V2_2));
        assert!(!params.enable_stable_row_ids);
    }

    #[test]
    fn invalid_write_options_are_rejected() {
        let unknown = LanceDBWriteOptions {
            set_fields: 1 << 31,
            ..LanceDBWriteOptions::default()
        };
        assert!(unsafe { write_options_from_c(&unknown) }.is_err());

        let missing_version = LanceDBWriteOptions {
            set_fields: WRITE_DATA_STORAGE_VERSION,
            ..LanceDBWriteOptions::default()
        };
        assert!(unsafe { write_options_from_c(&missing_version) }.is_err());

        let bad_bool = LanceDBWriteOptions {
            set_fields: WRITE_ENABLE_STABLE_ROW_IDS,
            enable_stable_row_ids: 2,
            ..LanceDBWriteOptions::default()
        };
        assert!(unsafe { write_options_from_c(&bad_bool) }.is_err());

        let version = CString::new("not-a-version").unwrap();
        let bad_version = LanceDBWriteOptions {
            set_fields: WRITE_DATA_STORAGE_VERSION,
            data_storage_version: version.as_ptr(),
            ..LanceDBWriteOptions::default()
        };
        assert!(unsafe { write_options_from_c(&bad_version) }.is_err());
    }
}
