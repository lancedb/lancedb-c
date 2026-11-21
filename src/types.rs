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
