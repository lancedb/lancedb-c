// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Error handling utility functions for LanceDB C bindings

use std::ffi::CString;
use std::os::raw::c_char;
use std::ptr;

/// Result codes for C API
#[repr(C)]
#[derive(Debug, PartialEq)]
pub enum LanceDBError {
    Success = 0,
    InvalidArgument = 1,
    InvalidTableName = 2,
    InvalidInput = 3,
    TableNotFound = 4,
    DatabaseNotFound = 5,
    DatabaseAlreadyExists = 6,
    IndexNotFound = 7,
    EmbeddingFunctionNotFound = 8,
    TableAlreadyExists = 9,
    CreateDir = 10,
    Schema = 11,
    Runtime = 12,
    Timeout = 13,
    ObjectStore = 14,
    Lance = 15,
    Http = 16,
    Retry = 17,
    Arrow = 18,
    NotSupported = 19,
    Other = 20,
    Namespace = 21,
    PermissionDenied = 22,
    NotFound = 23,
    TableCorrupted = 24,
    ColumnNotFound = 25,
    FieldNotFound = 26,
    ColumnAlreadyExists = 27,
    NotAComputedColumn = 28,
    NotAMaterializedView = 29,
    InvalidExpression = 30,
    Sql = 31,
    JobFailed = 32,
    JobCancelled = 33,
    JobNotFound = 34,
    ResponseInterrupted = 35,
    External = 36,
    // keep last, so that new error codes can be added without renumbering
    Unknown = 99,
}

/// The message a namespace backend produces when a table is missing, as rendered by
/// `lance_namespace::NamespaceError::TableNotFound`
const NAMESPACE_TABLE_NOT_FOUND: &str = "Table not found:";

/// The message a namespace backend produces when a namespace is missing, as rendered by
/// `lance_namespace::NamespaceError::NamespaceNotFound`
const NAMESPACE_NOT_FOUND: &str = "Namespace not found:";

/// Map an error message of a namespace backend to a "not found" error code
///
/// The namespace errors lose their original variant on the way out of lancedb, so the
/// message is the only thing left to tell a missing table or namespace apart from any
/// other namespace failure. Returns None when the message describes anything else.
fn not_found_from_message(message: &str) -> Option<LanceDBError> {
    if message.contains(NAMESPACE_TABLE_NOT_FOUND) {
        Some(LanceDBError::TableNotFound)
    } else if message.contains(NAMESPACE_NOT_FOUND) {
        Some(LanceDBError::DatabaseNotFound)
    } else {
        None
    }
}

/// Markers of a storage failure caused by missing privileges
///
/// Both the display and the debug format of the errors are covered, since lancedb
/// reports some of them with one format and some with the other. An S3 store does not
/// use the typed variants of the object store: it reports a generic error that carries
/// the HTTP status and the error code of the S3 response, so the S3 error code is
/// matched instead. Only markers that were seen coming out of a real backend are listed
const PERMISSION_DENIED_MARKERS: [&str; 3] = [
    "PermissionDenied",  // OS error kind, on a file backend (debug)
    "Permission denied", // OS error, on a file backend (display)
    "AccessDenied",      // S3 error code of a 403 response
];

/// Markers of a storage failure caused by a missing object
const NOT_FOUND_MARKERS: [&str; 1] = [
    "NoSuchBucket", // S3 error code of a 404 response
];

/// Map an error message of the storage layer to a "permission denied" or "not found"
/// error code
fn storage_failure_from_message(message: &str) -> Option<LanceDBError> {
    if PERMISSION_DENIED_MARKERS
        .iter()
        .any(|m| message.contains(m))
    {
        Some(LanceDBError::PermissionDenied)
    } else if NOT_FOUND_MARKERS.iter().any(|m| message.contains(m)) {
        Some(LanceDBError::NotFound)
    } else {
        None
    }
}

/// The message lancedb produces when the internal namespace client cannot be connected
const NAMESPACE_CONNECT_FAILED: &str = "Failed to connect to namespace:";

/// Convert Rust Error to C error code
///
/// Keep the match exhaustive. lancedb does not mark `Error` as non-exhaustive, so a
/// new SDK variant must be assigned a C code here and fails the build until it is.
pub(crate) fn error_to_error_code(error: &lancedb::error::Error) -> LanceDBError {
    match error {
        lancedb::error::Error::InvalidTableName { .. } => LanceDBError::InvalidTableName,
        // a failure to connect to the internal namespace client is not an invalid input,
        // it is a storage failure that lost its original error on the way out of lancedb
        lancedb::error::Error::InvalidInput { message }
            if message.contains(NAMESPACE_CONNECT_FAILED) =>
        {
            storage_failure_from_message(message).unwrap_or(LanceDBError::Lance)
        }
        lancedb::error::Error::InvalidInput { .. } => LanceDBError::InvalidInput,
        lancedb::error::Error::TableNotFound { .. } => LanceDBError::TableNotFound,
        lancedb::error::Error::TableCorrupted { .. } => LanceDBError::TableCorrupted,
        lancedb::error::Error::DatabaseNotFound { .. } => LanceDBError::DatabaseNotFound,
        lancedb::error::Error::DatabaseAlreadyExists { .. } => LanceDBError::DatabaseAlreadyExists,
        lancedb::error::Error::IndexNotFound { .. } => LanceDBError::IndexNotFound,
        lancedb::error::Error::EmbeddingFunctionNotFound { .. } => {
            LanceDBError::EmbeddingFunctionNotFound
        }
        lancedb::error::Error::ColumnNotFound { .. } => LanceDBError::ColumnNotFound,
        lancedb::error::Error::FieldNotFound { .. } => LanceDBError::FieldNotFound,
        lancedb::error::Error::ColumnAlreadyExists { .. } => LanceDBError::ColumnAlreadyExists,
        lancedb::error::Error::NotAComputedColumn { .. } => LanceDBError::NotAComputedColumn,
        lancedb::error::Error::NotAMaterializedView { .. } => LanceDBError::NotAMaterializedView,
        lancedb::error::Error::InvalidExpression { .. } => LanceDBError::InvalidExpression,
        lancedb::error::Error::TableAlreadyExists { .. } => LanceDBError::TableAlreadyExists,
        lancedb::error::Error::CreateDir { .. } => LanceDBError::CreateDir,
        lancedb::error::Error::Schema { .. } => LanceDBError::Schema,
        lancedb::error::Error::Sql { .. } => LanceDBError::Sql,
        // the namespace backend wraps all of its failures into a generic runtime error
        // (lancedb: database/namespace.rs), so a missing table or namespace is recovered
        // from the message
        lancedb::error::Error::Runtime { message } => {
            not_found_from_message(message).unwrap_or(LanceDBError::Runtime)
        }
        lancedb::error::Error::Timeout { .. } => LanceDBError::Timeout,
        lancedb::error::Error::JobFailed { .. } => LanceDBError::JobFailed,
        lancedb::error::Error::JobCancelled { .. } => LanceDBError::JobCancelled,
        lancedb::error::Error::JobNotFound { .. } => LanceDBError::JobNotFound,
        lancedb::error::Error::ObjectStore { source } => {
            storage_failure_from_message(&source.to_string()).unwrap_or(LanceDBError::ObjectStore)
        }
        // the namespace errors and the object store errors are wrapped by lance without
        // their original variant, so the actionable ones are recovered from the message
        lancedb::error::Error::Lance { source } => {
            let message = source.to_string();
            let fallback = match source {
                lance::Error::NotFound { .. } => LanceDBError::NotFound,
                lance::Error::IndexNotFound { .. } => LanceDBError::IndexNotFound,
                lance::Error::Namespace { .. } => LanceDBError::Namespace,
                _ => LanceDBError::Lance,
            };
            not_found_from_message(&message)
                .or_else(|| storage_failure_from_message(&message))
                .unwrap_or(fallback)
        }
        lancedb::error::Error::Http { .. } => LanceDBError::Http,
        lancedb::error::Error::Retry { .. } => LanceDBError::Retry,
        lancedb::error::Error::ResponseInterrupted { .. } => LanceDBError::ResponseInterrupted,
        lancedb::error::Error::Arrow { .. } => LanceDBError::Arrow,
        lancedb::error::Error::NotSupported { .. } => LanceDBError::NotSupported,
        // An External error is a transparent pass-through of an error created outside
        // lancedb. It stays distinct from Other, whose message and optional source are
        // owned by lancedb, so a C caller can tell the two apart.
        lancedb::error::Error::External { .. } => LanceDBError::External,
        lancedb::error::Error::Other { .. } => LanceDBError::Other,
    }
}

/// Set error message for detailed error reporting
pub(crate) unsafe fn set_error_message(
    error_message_out: *mut *mut c_char,
    error: &lancedb::error::Error,
) {
    if error_message_out.is_null() {
        return;
    }

    let error_string = format!("{error}");
    match CString::new(error_string) {
        Ok(c_str) => {
            *error_message_out = c_str.into_raw();
        }
        Err(_) => {
            *error_message_out = ptr::null_mut();
        }
    }
}

/// Handle error with optional message
pub(crate) unsafe fn handle_error(
    error: &lancedb::error::Error,
    error_message_out: *mut *mut c_char,
) -> LanceDBError {
    if !error_message_out.is_null() {
        set_error_message(error_message_out, error);
    }
    error_to_error_code(error)
}

/// Clear an optional error message slot before an entry point reports a status
///
/// Entry points that return a status instead of a handle clear the slot on
/// entry, so a caller never frees a stale error string left from an earlier call.
pub(crate) unsafe fn clear_error_message(error_message_out: *mut *mut c_char) {
    if !error_message_out.is_null() {
        *error_message_out = ptr::null_mut();
    }
}

/// Set simple error message for invalid argument cases
pub(crate) unsafe fn set_invalid_argument_message(error_message_out: *mut *mut c_char) {
    if error_message_out.is_null() {
        return;
    }

    match CString::new("Invalid argument") {
        Ok(c_str) => {
            *error_message_out = c_str.into_raw();
        }
        Err(_) => {
            *error_message_out = ptr::null_mut();
        }
    }
}

/// Set simple error message for the unknown error cases
pub(crate) unsafe fn set_unknown_error_message(error_message_out: *mut *mut c_char) {
    if error_message_out.is_null() {
        return;
    }

    match CString::new("Unknown error") {
        Ok(c_str) => {
            *error_message_out = c_str.into_raw();
        }
        Err(_) => {
            *error_message_out = ptr::null_mut();
        }
    }
}

/// Set a custom error message string
pub(crate) unsafe fn set_custom_error_message(error_message_out: *mut *mut c_char, msg: &str) {
    if error_message_out.is_null() {
        return;
    }

    match CString::new(msg) {
        Ok(c_str) => {
            *error_message_out = c_str.into_raw();
        }
        Err(_) => {
            *error_message_out = ptr::null_mut();
        }
    }
}

/// Set simple error message for the not supported error cases
pub(crate) unsafe fn set_not_supported_message(error_message_out: *mut *mut c_char) {
    if error_message_out.is_null() {
        return;
    }

    match CString::new("Not supported") {
        Ok(c_str) => {
            *error_message_out = c_str.into_raw();
        }
        Err(_) => {
            *error_message_out = ptr::null_mut();
        }
    }
}

/// Free string returned by LanceDB functions
///
/// # Safety
/// - `str` must be a pointer returned by LanceDB functions (e.g., error messages)
/// - `str` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_string(str: *mut c_char) {
    if !str.is_null() {
        let _ = CString::from_raw(str);
    }
}
