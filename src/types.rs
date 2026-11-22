// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Common types shared across LanceDB C bindings modules

use std::os::raw::{c_char, c_int};

use arrow_array::RecordBatchReader;
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
    /// -1 for the SDK default, 0 to skip the join-key index, 1 to use it
    pub use_index: i32,
    /// -1 for the SDK default, otherwise the operation timeout in milliseconds
    pub timeout_ms: i64,
}

/// Result of `lancedb_table_merge_insert_with_result`
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct LanceDBMergeInsertResult {
    /// Table version committed by the merge; 0 may mean unknown on legacy remote responses
    pub version: u64,
    /// Number of inserted rows
    pub num_inserted_rows: u64,
    /// Number of updated rows
    pub num_updated_rows: u64,
    /// Number of deleted rows
    pub num_deleted_rows: u64,
    /// Number of attempts, including retries for transaction conflicts
    pub num_attempts: u32,
}
