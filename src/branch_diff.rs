// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Remote branch diff and cherry-pick FFI for LanceDB C bindings
//!
//! The SDK owns the protocol and result model: this module only forwards the
//! call, converts the returned `BranchDiff` or `CherryPickResult` into C-owned
//! data, and releases it as one result. A cherry-pick that the server rejects
//! with HTTP 409 is an SDK success carrying `CherryPickStatus::Failed`; only a
//! transport or SDK failure becomes a C error status.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::ptr;

use lancedb::table::{
    BranchDiff, CherryPickError, CherryPickErrorCode, CherryPickResult, CherryPickStatus,
    ColumnChange, ColumnSummary, IndexSummary, RowCountSummary,
};

use crate::connection::{get_runtime, LanceDBTable};
use crate::error::{
    clear_error_message, handle_error, set_invalid_argument_message, set_unknown_error_message,
    LanceDBError,
};

/// One column description in a branch diff
///
/// The strings are read-only and owned by the result that contains them.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceDBColumnSummary {
    pub name: *const c_char,
    pub data_type: *const c_char,
    pub nullable: c_int,
}

/// A column that exists in both branches with different definitions
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceDBColumnChange {
    pub name: *const c_char,
    pub main: LanceDBColumnSummary,
    pub branch: LanceDBColumnSummary,
}

/// One index description in a branch diff
///
/// `index_type` is NULL when the SDK response omitted it. `columns` is NULL
/// when `column_count` is zero.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceDBIndexSummary {
    pub index_name: *const c_char,
    pub columns: *const *const c_char,
    pub column_count: usize,
    pub index_type: *const c_char,
    pub status: *const c_char,
}

/// Row-count classification from the SDK
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceDBRowCountSummary {
    pub unchanged: u64,
    pub new_on_base: u64,
    pub new_on_branch: u64,
    pub stale_recompute: u64,
    pub inputs_changed: u64,
    pub delta_available: c_int,
}

/// Reason the server gave for a blocked cherry-pick
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceDBCherryPickErrorCode {
    BaseMoved = 0,
    RowCountMismatch = 1,
    RowsChanged = 2,
    ColumnRemoved = 3,
    ColumnChanged = 4,
    NothingToApply = 5,
    NoColumnChanges = 6,
    InputColumnDependency = 7,
    ParentNotMain = 8,
    /// A code the pinned SDK does not know; the SDK maps it to `Unknown`.
    Unknown = 9,
}

/// One error from a branch diff or cherry-pick result
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceDBCherryPickError {
    pub code: LanceDBCherryPickErrorCode,
    pub message: *const c_char,
}

/// Full branch diff returned by `lancedb_table_diff_branch`
///
/// Every array is in the SDK response order and NULL when its count is zero.
/// The result owns all strings and arrays. Free the whole result with
/// `lancedb_free_branch_diff`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceDBBranchDiff {
    pub from_branch: *const c_char,
    pub parent_version: u64,
    pub main_version: u64,
    pub branch_version: u64,
    pub base_moved: c_int,
    pub row_count_main: u64,
    pub row_count_branch: u64,
    pub row_summary: LanceDBRowCountSummary,
    pub added_columns: *const LanceDBColumnSummary,
    pub added_column_count: usize,
    pub removed_columns: *const LanceDBColumnSummary,
    pub removed_column_count: usize,
    pub changed_columns: *const LanceDBColumnChange,
    pub changed_column_count: usize,
    pub added_indexes: *const LanceDBIndexSummary,
    pub added_index_count: usize,
    pub removed_indexes: *const LanceDBIndexSummary,
    pub removed_index_count: usize,
    pub errors: *const LanceDBCherryPickError,
    pub error_count: usize,
}

/// Outcome reported by `lancedb_table_cherry_pick`
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceDBCherryPickStatus {
    Ready = 0,
    Failed = 1,
    NotImplemented = 2,
    CherryPicked = 3,
    /// A status the pinned SDK does not know; the SDK maps it to `Unknown`.
    Unknown = 4,
}

/// Result of `lancedb_table_cherry_pick`
///
/// `diff` is an embedded copy of the branch diff; do not pass its address to
/// `lancedb_free_branch_diff`. `promoted_columns` is NULL when
/// `promoted_column_count` is zero. `main_version_after` is read only when
/// `has_main_version_after` is non-zero, because the SDK field is optional.
/// Free the whole result with `lancedb_free_cherry_pick_result`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceDBCherryPickResult {
    pub status: LanceDBCherryPickStatus,
    pub diff: LanceDBBranchDiff,
    pub promoted_columns: *const *const c_char,
    pub promoted_column_count: usize,
    pub has_main_version_after: c_int,
    pub main_version_after: u64,
}

/// Owns every allocation pointed to by a `LanceDBBranchDiff`
#[repr(C)]
struct BranchDiffOwner {
    // Must stay first: the free function casts the public pointer back to this.
    c: LanceDBBranchDiff,
    strings: Vec<CString>,
    added_columns: Vec<LanceDBColumnSummary>,
    removed_columns: Vec<LanceDBColumnSummary>,
    changed_columns: Vec<LanceDBColumnChange>,
    added_indexes: Vec<LanceDBIndexSummary>,
    removed_indexes: Vec<LanceDBIndexSummary>,
    index_columns: Vec<Vec<*const c_char>>,
    errors: Vec<LanceDBCherryPickError>,
}

/// Owns every allocation pointed to by a `LanceDBCherryPickResult`
#[repr(C)]
struct CherryPickResultOwner {
    // Must stay first: the free function casts the public pointer back to this.
    c: LanceDBCherryPickResult,
    diff: Box<BranchDiffOwner>,
    strings: Vec<CString>,
    promoted_columns: Vec<*const c_char>,
}

fn slice_pointer<T>(values: &[T]) -> *const T {
    if values.is_empty() {
        ptr::null()
    } else {
        values.as_ptr()
    }
}

/// Move one SDK string into the result's string arena and return its C pointer
fn push_string(strings: &mut Vec<CString>, value: String) -> Result<*const c_char, ()> {
    let value = CString::new(value).map_err(|_| ())?;
    let pointer = value.as_ptr();
    strings.push(value);
    Ok(pointer)
}

fn convert_column_summary(
    summary: ColumnSummary,
    strings: &mut Vec<CString>,
) -> Result<LanceDBColumnSummary, ()> {
    Ok(LanceDBColumnSummary {
        name: push_string(strings, summary.name)?,
        data_type: push_string(strings, summary.data_type)?,
        nullable: if summary.nullable { 1 } else { 0 },
    })
}

fn convert_column_change(
    change: ColumnChange,
    strings: &mut Vec<CString>,
) -> Result<LanceDBColumnChange, ()> {
    Ok(LanceDBColumnChange {
        name: push_string(strings, change.name)?,
        main: convert_column_summary(change.main, strings)?,
        branch: convert_column_summary(change.branch, strings)?,
    })
}

fn convert_index_summary(
    summary: IndexSummary,
    strings: &mut Vec<CString>,
    index_columns: &mut Vec<Vec<*const c_char>>,
) -> Result<LanceDBIndexSummary, ()> {
    let index_name = push_string(strings, summary.index_name)?;

    let mut columns = Vec::with_capacity(summary.columns.len());
    for column in summary.columns {
        columns.push(push_string(strings, column)?);
    }
    let column_count = columns.len();
    let columns_pointer = slice_pointer(&columns);
    index_columns.push(columns);

    let index_type = match summary.index_type {
        Some(index_type) => push_string(strings, index_type)?,
        None => ptr::null(),
    };

    Ok(LanceDBIndexSummary {
        index_name,
        columns: columns_pointer,
        column_count,
        index_type,
        status: push_string(strings, summary.status)?,
    })
}

fn convert_error_code(code: CherryPickErrorCode) -> LanceDBCherryPickErrorCode {
    match code {
        CherryPickErrorCode::BaseMoved => LanceDBCherryPickErrorCode::BaseMoved,
        CherryPickErrorCode::RowCountMismatch => LanceDBCherryPickErrorCode::RowCountMismatch,
        CherryPickErrorCode::RowsChanged => LanceDBCherryPickErrorCode::RowsChanged,
        CherryPickErrorCode::ColumnRemoved => LanceDBCherryPickErrorCode::ColumnRemoved,
        CherryPickErrorCode::ColumnChanged => LanceDBCherryPickErrorCode::ColumnChanged,
        CherryPickErrorCode::NothingToApply => LanceDBCherryPickErrorCode::NothingToApply,
        CherryPickErrorCode::NoColumnChanges => LanceDBCherryPickErrorCode::NoColumnChanges,
        CherryPickErrorCode::InputColumnDependency => {
            LanceDBCherryPickErrorCode::InputColumnDependency
        }
        CherryPickErrorCode::ParentNotMain => LanceDBCherryPickErrorCode::ParentNotMain,
        CherryPickErrorCode::Unknown => LanceDBCherryPickErrorCode::Unknown,
    }
}

fn convert_error(
    error: CherryPickError,
    strings: &mut Vec<CString>,
) -> Result<LanceDBCherryPickError, ()> {
    Ok(LanceDBCherryPickError {
        code: convert_error_code(error.code),
        message: push_string(strings, error.message)?,
    })
}

fn convert_status(status: CherryPickStatus) -> LanceDBCherryPickStatus {
    match status {
        CherryPickStatus::Ready => LanceDBCherryPickStatus::Ready,
        CherryPickStatus::Failed => LanceDBCherryPickStatus::Failed,
        CherryPickStatus::NotImplemented => LanceDBCherryPickStatus::NotImplemented,
        CherryPickStatus::CherryPicked => LanceDBCherryPickStatus::CherryPicked,
        CherryPickStatus::Unknown => LanceDBCherryPickStatus::Unknown,
    }
}

/// Convert the SDK result before any output is published
fn convert_branch_diff(diff: BranchDiff) -> Result<Box<BranchDiffOwner>, ()> {
    let BranchDiff {
        from_branch,
        parent_version,
        main_version,
        branch_version,
        base_moved,
        row_count_main,
        row_count_branch,
        row_summary,
        added_columns: sdk_added_columns,
        removed_columns: sdk_removed_columns,
        changed_columns: sdk_changed_columns,
        added_indexes: sdk_added_indexes,
        removed_indexes: sdk_removed_indexes,
        errors: sdk_errors,
    } = diff;

    let mut strings = Vec::new();
    let from_branch = push_string(&mut strings, from_branch)?;

    let mut added_columns = Vec::with_capacity(sdk_added_columns.len());
    for column in sdk_added_columns {
        added_columns.push(convert_column_summary(column, &mut strings)?);
    }

    let mut removed_columns = Vec::with_capacity(sdk_removed_columns.len());
    for column in sdk_removed_columns {
        removed_columns.push(convert_column_summary(column, &mut strings)?);
    }

    let mut changed_columns = Vec::with_capacity(sdk_changed_columns.len());
    for change in sdk_changed_columns {
        changed_columns.push(convert_column_change(change, &mut strings)?);
    }

    // Each index keeps its own pointer array so column order and per-index
    // ownership stay explicit in the public layout.
    let mut index_columns = Vec::new();
    let mut added_indexes = Vec::with_capacity(sdk_added_indexes.len());
    for index in sdk_added_indexes {
        added_indexes.push(convert_index_summary(
            index,
            &mut strings,
            &mut index_columns,
        )?);
    }
    let mut removed_indexes = Vec::with_capacity(sdk_removed_indexes.len());
    for index in sdk_removed_indexes {
        removed_indexes.push(convert_index_summary(
            index,
            &mut strings,
            &mut index_columns,
        )?);
    }

    let mut errors = Vec::with_capacity(sdk_errors.len());
    for error in sdk_errors {
        errors.push(convert_error(error, &mut strings)?);
    }

    let RowCountSummary {
        unchanged,
        new_on_base,
        new_on_branch,
        stale_recompute,
        inputs_changed,
        delta_available,
    } = row_summary;

    let c = LanceDBBranchDiff {
        from_branch,
        parent_version,
        main_version,
        branch_version,
        base_moved: if base_moved { 1 } else { 0 },
        row_count_main,
        row_count_branch,
        row_summary: LanceDBRowCountSummary {
            unchanged,
            new_on_base,
            new_on_branch,
            stale_recompute,
            inputs_changed,
            delta_available: if delta_available { 1 } else { 0 },
        },
        added_columns: slice_pointer(&added_columns),
        added_column_count: added_columns.len(),
        removed_columns: slice_pointer(&removed_columns),
        removed_column_count: removed_columns.len(),
        changed_columns: slice_pointer(&changed_columns),
        changed_column_count: changed_columns.len(),
        added_indexes: slice_pointer(&added_indexes),
        added_index_count: added_indexes.len(),
        removed_indexes: slice_pointer(&removed_indexes),
        removed_index_count: removed_indexes.len(),
        errors: slice_pointer(&errors),
        error_count: errors.len(),
    };

    Ok(Box::new(BranchDiffOwner {
        c,
        strings,
        added_columns,
        removed_columns,
        changed_columns,
        added_indexes,
        removed_indexes,
        index_columns,
        errors,
    }))
}

/// Convert the SDK result before any output is published
fn convert_cherry_pick_result(result: CherryPickResult) -> Result<Box<CherryPickResultOwner>, ()> {
    let CherryPickResult {
        status,
        diff,
        preview,
        main_version_after,
    } = result;

    let diff = convert_branch_diff(diff)?;
    let mut strings = Vec::new();
    let mut promoted_columns = Vec::with_capacity(preview.promoted_columns.len());
    for column in preview.promoted_columns {
        promoted_columns.push(push_string(&mut strings, column)?);
    }

    let c = LanceDBCherryPickResult {
        status: convert_status(status),
        diff: diff.c,
        promoted_columns: slice_pointer(&promoted_columns),
        promoted_column_count: promoted_columns.len(),
        has_main_version_after: if main_version_after.is_some() { 1 } else { 0 },
        main_version_after: main_version_after.unwrap_or(0),
    };

    Ok(Box::new(CherryPickResultOwner {
        c,
        diff,
        strings,
        promoted_columns,
    }))
}

/// Diff a branch against main on a remote table
///
/// # Safety
/// - `table` must be a valid pointer returned by the open-table entry points
/// - `from_branch` must be a valid NUL-terminated C string
/// - `diff_out` must be a valid pointer to receive the result
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*diff_out` holding the result, or the SDK error
///
/// On entry, `*diff_out` and the error message are cleared. The result is
/// published only on success and owns every nested string and array. Free it
/// with `lancedb_free_branch_diff()`. Native tables return the SDK
/// `LANCEDB_NOT_SUPPORTED` status and message unchanged.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_diff_branch(
    table: *const LanceDBTable,
    from_branch: *const c_char,
    diff_out: *mut *mut LanceDBBranchDiff,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !diff_out.is_null() {
        *diff_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if table.is_null() || from_branch.is_null() || diff_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(from_branch) = CStr::from_ptr(from_branch).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let from_branch = from_branch.to_string();
    let table = (*table).inner.clone();
    let result = get_runtime().block_on(async move { table.diff_branch(&from_branch).await });

    match result {
        Ok(diff) => match convert_branch_diff(diff) {
            Ok(diff) => {
                *diff_out = Box::into_raw(diff) as *mut LanceDBBranchDiff;
                LanceDBError::Success
            }
            Err(()) => {
                set_unknown_error_message(error_message);
                LanceDBError::Unknown
            }
        },
        Err(error) => handle_error(&error, error_message),
    }
}

/// Cherry-pick a branch onto main, or preview the operation with a dry run
///
/// # Safety
/// - `table` must be a valid pointer returned by the open-table entry points
/// - `from_branch` must be a valid NUL-terminated C string
/// - `result_out` must be a valid pointer to receive the result
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*result_out` holding the result, or the SDK error
///
/// `dry_run` selects the preview request. An HTTP 409 from the server is not a
/// transport error: the SDK returns its `CherryPickResult` with
/// `LanceDBCherryPickStatus::Failed`, and this entry point returns
/// `LANCEDB_SUCCESS` with that result. Only a transport or SDK failure becomes
/// an error status. On entry, `*result_out` and the error message are cleared.
/// The result is published only on success and owns every nested string and
/// array. Free it with `lancedb_free_cherry_pick_result()`. Native tables
/// return the SDK `LANCEDB_NOT_SUPPORTED` status and message unchanged.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_cherry_pick(
    table: *const LanceDBTable,
    from_branch: *const c_char,
    dry_run: c_int,
    result_out: *mut *mut LanceDBCherryPickResult,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !result_out.is_null() {
        *result_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if table.is_null() || from_branch.is_null() || result_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(from_branch) = CStr::from_ptr(from_branch).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let from_branch = from_branch.to_string();
    let dry_run = dry_run != 0;
    let table = (*table).inner.clone();
    let result =
        get_runtime().block_on(async move { table.cherry_pick(&from_branch, dry_run).await });

    match result {
        Ok(result) => match convert_cherry_pick_result(result) {
            Ok(result) => {
                *result_out = Box::into_raw(result) as *mut LanceDBCherryPickResult;
                LanceDBError::Success
            }
            Err(()) => {
                set_unknown_error_message(error_message);
                LanceDBError::Unknown
            }
        },
        Err(error) => handle_error(&error, error_message),
    }
}

/// Free a branch diff returned by `lancedb_table_diff_branch`
///
/// # Safety
/// - `diff` must be NULL or a pointer returned by `lancedb_table_diff_branch`
/// - The pointer must not be used after this call
///
/// The call frees every string and nested array owned by the result. Do not
/// free nested pointers individually or pass an embedded
/// `LanceDBCherryPickResult.diff` address to this function.
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_branch_diff(diff: *mut LanceDBBranchDiff) {
    if diff.is_null() {
        return;
    }
    drop(Box::from_raw(diff as *mut BranchDiffOwner));
}

/// Free a cherry-pick result returned by `lancedb_table_cherry_pick`
///
/// # Safety
/// - `result` must be NULL or a pointer returned by `lancedb_table_cherry_pick`
/// - The pointer must not be used after this call, including its embedded diff
///
/// The call frees every string and nested array owned by the result. Do not
/// free nested pointers individually or pass the embedded diff to
/// `lancedb_free_branch_diff()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_cherry_pick_result(result: *mut LanceDBCherryPickResult) {
    if result.is_null() {
        return;
    }
    drop(Box::from_raw(result as *mut CherryPickResultOwner));
}
