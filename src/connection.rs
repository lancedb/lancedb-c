// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Connection-related FFI functions for LanceDB C bindings

use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;
use std::sync::Arc;
use std::sync::OnceLock;

use arrow_array::{RecordBatch, RecordBatchIterator, RecordBatchReader};
use arrow_schema::Schema;
use futures::TryStreamExt;
use lance_io::object_store::StorageOptionsAccessor;
use lance_namespace::models::{
    CreateNamespaceRequest, DescribeNamespaceRequest, DropNamespaceRequest,
};
use lancedb::connection::{connect, ConnectBuilder, Connection, TableNamesBuilder};
use lancedb::database::CreateTableMode;
use lancedb::database::ReadConsistency;
use lancedb::utils::validate_table_name;
use lancedb::Table;

use crate::error::{
    clear_error_message, handle_error, set_custom_error_message, set_invalid_argument_message,
    set_unknown_error_message, LanceDBError,
};
use crate::types::{
    bool_from_c_int, write_options_from_c, LanceDBCreateTableOptions, LanceDBRecordBatchReader,
};

/// Opaque handle to a ConnectBuilder
#[repr(C)]
pub struct LanceDBConnectBuilder {
    inner: Box<ConnectBuilder>,
    // The SDK dispatches every URI starting with "db" to its remote path. Keep
    // the same classification so remote-only setters never land in a native
    // storage provider's options.
    is_remote: bool,
}

/// Opaque handle to a Connection
#[repr(C)]
pub struct LanceDBConnection {
    pub(crate) inner: Connection,
    // Cache the URI as a C-compatible string for safe return from lancedb_connection_uri
    uri_cache: OnceLock<CString>,
}

/// Opaque handle to a Table
#[repr(C)]
pub struct LanceDBTable {
    pub(crate) inner: Table,
}

/// Options for `lancedb_connection_clone_table`
///
/// A NULL options pointer or a zeroed struct keeps the SDK defaults: clone the
/// source's current version into the root namespace, fail when the target
/// already exists, and perform a shallow clone. The string and array fields are
/// borrowed for the duration of the call.
#[repr(C)]
pub struct LanceDBCloneTableOptions {
    /// Optional source tag; NULL leaves it unset
    pub source_tag: *const c_char,
    /// Target namespace components, or NULL when the length is zero
    pub target_namespace: *const *const c_char,
    /// Number of target namespace components; zero selects the root namespace
    pub target_namespace_len: usize,
    /// Source version, read only when `has_source_version` is non-zero
    pub source_version: u64,
    /// Non-zero to pass `source_version` to the SDK
    pub has_source_version: c_int,
    /// Non-zero to preserve an existing target without reading the source
    pub exist_ok: c_int,
    /// Non-zero to read `is_shallow`; zero keeps the SDK default
    pub has_is_shallow: c_int,
    /// Shallow clone flag, read only when `has_is_shallow` is non-zero
    pub is_shallow: c_int,
}

/// Opaque handle to a TableNamesBuilder
#[repr(C)]
pub struct LanceDBTableNamesBuilder {
    inner: Box<TableNamesBuilder>,
}

/// Opaque handle to a Session
#[repr(C)]
pub struct LanceDBSession {
    inner: Arc<lancedb::Session>,
}

/// Opaque handle to an ObjectStoreRegistry
#[repr(C)]
pub struct LanceDBObjectStoreRegistry {
    inner: Arc<lancedb::ObjectStoreRegistry>,
}

/// Opaque handle to an ObjectStoreProvider
#[repr(C)]
pub struct LanceDBObjectStoreProvider {
    pub inner: Option<Arc<dyn lance_io::object_store::ObjectStoreProvider>>,
}

/// Session creation options
#[repr(C)]
pub struct LanceDBSessionOptions {
    pub index_cache_bytes: usize,
    pub metadata_cache_bytes: usize,
}

/// Open table option fields of `LanceDBOpenTableOptions`
///
/// These values mirror the `LanceDBOpenTableField` enumerators of the public header.
const OPEN_TABLE_INDEX_CACHE_SIZE: u32 = 1 << 0;
const OPEN_TABLE_VERSION: u32 = 1 << 1;
const OPEN_TABLE_MANAGED_VERSIONING: u32 = 1 << 2;
const OPEN_TABLE_ALL_FIELDS: u32 =
    OPEN_TABLE_INDEX_CACHE_SIZE | OPEN_TABLE_VERSION | OPEN_TABLE_MANAGED_VERSIONING;

/// Options for `lancedb_connection_open_table_with_options`
///
/// A zero-initialized struct keeps every SDK default. `set_fields` selects the
/// scalar overrides, so an omitted `index_cache_size` stays distinct from an
/// explicit zero and an omitted `managed_versioning` stays distinct from false.
///
/// `namespace_path` is a C array of `namespace_path_len` borrowed strings; NULL
/// with length zero is the root namespace. `storage_keys` and `storage_values`
/// are parallel arrays of `storage_count` borrowed strings for this table only.
/// `location` and `branch` are borrowed strings; NULL leaves them unset. The
/// struct and every string it points to are borrowed for the duration of the
/// call and are not consumed.
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct LanceDBOpenTableOptions {
    /// Bitmask of `LanceDBOpenTableField` values; 0 uses every default
    pub set_fields: u32,
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
    /// Index cache size in entries; read only when its bit is set. The SDK
    /// forwards it, but an attached connection Session keeps its own effective
    /// capacity.
    pub index_cache_size: u32,
    /// Table version; read only when its bit is set
    pub version: u64,
    /// Read-only branch name (NULL for the default branch)
    pub branch: *const c_char,
    /// Managed versioning (0 = false, 1 = true); read only when its bit is set
    pub managed_versioning: c_int,
}

impl Default for LanceDBOpenTableOptions {
    fn default() -> Self {
        Self {
            set_fields: 0,
            namespace_path: ptr::null(),
            namespace_path_len: 0,
            location: ptr::null(),
            storage_keys: ptr::null(),
            storage_values: ptr::null(),
            storage_count: 0,
            index_cache_size: 0,
            version: 0,
            branch: ptr::null(),
            managed_versioning: 0,
        }
    }
}

/// The SDK open-table settings parsed from the borrowed C options
#[derive(Default)]
struct OpenTableSettings {
    namespace_path: Vec<String>,
    location: Option<String>,
    storage_options: Vec<(String, String)>,
    index_cache_size: Option<u32>,
    version: Option<u64>,
    branch: Option<String>,
    managed_versioning: Option<bool>,
}

/// Convert one nullable borrowed string
unsafe fn open_table_optional_string(
    value: *const c_char,
    name: &str,
) -> Result<Option<String>, String> {
    if value.is_null() {
        return Ok(None);
    }
    CStr::from_ptr(value)
        .to_str()
        .map(|value| Some(value.to_string()))
        .map_err(|_| format!("'{name}' is not valid UTF-8"))
}

/// Convert the per-table storage option arrays from C
unsafe fn open_table_storage_options(
    options: &LanceDBOpenTableOptions,
) -> Result<Vec<(String, String)>, String> {
    if options.storage_count == 0 {
        return Ok(Vec::new());
    }
    if options.storage_keys.is_null() || options.storage_values.is_null() {
        return Err(
            "storage_keys and storage_values must not be NULL when storage_count is non-zero"
                .to_string(),
        );
    }

    let mut pairs = Vec::with_capacity(options.storage_count);
    for index in 0..options.storage_count {
        let key = *options.storage_keys.add(index);
        let value = *options.storage_values.add(index);
        if key.is_null() || value.is_null() {
            return Err(format!("storage option {} has a NULL key or value", index));
        }
        let key = CStr::from_ptr(key)
            .to_str()
            .map_err(|_| format!("storage option key {} is not valid UTF-8", index))?;
        let value = CStr::from_ptr(value)
            .to_str()
            .map_err(|_| format!("storage option value {} is not valid UTF-8", index))?;
        pairs.push((key.to_string(), value.to_string()));
    }
    Ok(pairs)
}

/// Parse the borrowed C options into values the SDK builder accepts
///
/// Every rejection sets the error message through `error_message` and returns
/// the matching C error code.
unsafe fn open_table_settings_from_c(
    options: *const LanceDBOpenTableOptions,
    error_message: *mut *mut c_char,
) -> Result<OpenTableSettings, LanceDBError> {
    let mut settings = OpenTableSettings::default();
    if options.is_null() {
        return Ok(settings);
    }
    let options = &*options;

    let unknown = options.set_fields & !OPEN_TABLE_ALL_FIELDS;
    if unknown != 0 {
        set_custom_error_message(
            error_message,
            &format!("unknown open table option bits: 0x{unknown:x}"),
        );
        return Err(LanceDBError::InvalidArgument);
    }

    settings.namespace_path = match namespace_path_from_c(
        options.namespace_path,
        options.namespace_path_len,
        error_message,
    ) {
        Some(path) => path,
        None => return Err(LanceDBError::InvalidArgument),
    };

    settings.location = match open_table_optional_string(options.location, "location") {
        Ok(location) => location,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return Err(LanceDBError::InvalidArgument);
        }
    };

    settings.storage_options = match open_table_storage_options(options) {
        Ok(storage_options) => storage_options,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return Err(LanceDBError::InvalidArgument);
        }
    };

    if options.set_fields & OPEN_TABLE_INDEX_CACHE_SIZE != 0 {
        settings.index_cache_size = Some(options.index_cache_size);
    }
    if options.set_fields & OPEN_TABLE_VERSION != 0 {
        settings.version = Some(options.version);
    }
    if options.set_fields & OPEN_TABLE_MANAGED_VERSIONING != 0 {
        settings.managed_versioning = Some(match options.managed_versioning {
            0 => false,
            1 => true,
            other => {
                set_custom_error_message(
                    error_message,
                    &format!("'managed_versioning' must be 0 or 1, got {other}"),
                );
                return Err(LanceDBError::InvalidArgument);
            }
        });
    }

    settings.branch = match open_table_optional_string(options.branch, "branch") {
        Ok(branch) => branch,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return Err(LanceDBError::InvalidArgument);
        }
    };

    Ok(settings)
}

/// Cache statistics for a session cache
#[repr(C)]
pub struct LanceDBSessionCacheStats {
    pub hits: u64,
    pub misses: u64,
    pub num_entries: usize,
    pub size_bytes: usize,
}

/// Read consistency mode reported to C callers
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceDBReadConsistencyType {
    Manual = 0,
    Eventual = 1,
    Strong = 2,
}

/// Read consistency of a connection returned to C callers
#[repr(C)]
pub struct LanceDBReadConsistency {
    pub kind: LanceDBReadConsistencyType,
    pub interval_ms: u64,
}

const DEFAULT_INDEX_CACHE_SIZE_BYTES: usize = 6 * 1024 * 1024 * 1024;
const DEFAULT_METADATA_CACHE_SIZE_BYTES: usize = 1024 * 1024 * 1024;

/// Runtime to handle async operations
static RUNTIME: OnceLock<tokio::runtime::Runtime> = OnceLock::new();

pub(crate) fn get_runtime() -> &'static tokio::runtime::Runtime {
    RUNTIME.get_or_init(|| tokio::runtime::Runtime::new().expect("Failed to create tokio runtime"))
}

/// Convert a borrowed C namespace path into the SDK's owned path representation.
///
/// A zero length is the root namespace and accepts a NULL array pointer. A positive
/// length requires a non-NULL array of non-NULL, NUL-terminated UTF-8 strings.
/// Component validation is left to the SDK so that the binding follows the
/// backend's namespace rules.
///
/// This is the single conversion for every namespace path entry point, including
/// the table creation and opening paths, so validation and ownership stay in one
/// place.
///
/// # Safety
/// - `namespace_path` must point to `namespace_path_len` readable pointers, or be
///   NULL when `namespace_path_len` is zero.
/// - Every element must be a valid NUL-terminated C string.
pub(crate) unsafe fn namespace_path_from_c(
    namespace_path: *const *const c_char,
    namespace_path_len: usize,
    error_message: *mut *mut c_char,
) -> Option<Vec<String>> {
    if namespace_path_len == 0 {
        return Some(Vec::new());
    }

    if namespace_path.is_null() {
        set_invalid_argument_message(error_message);
        return None;
    }

    let mut path = Vec::with_capacity(namespace_path_len);
    for index in 0..namespace_path_len {
        let component = *namespace_path.add(index);
        if component.is_null() {
            set_invalid_argument_message(error_message);
            return None;
        }

        let Ok(component) = CStr::from_ptr(component).to_str() else {
            set_invalid_argument_message(error_message);
            return None;
        };
        path.push(component.to_string());
    }

    Some(path)
}

/// Convert one optional namespace name, where NULL is the root namespace.
unsafe fn single_namespace_from_c(
    namespace_name: *const c_char,
    error_message: *mut *mut c_char,
) -> Option<Vec<String>> {
    if namespace_name.is_null() {
        return Some(Vec::new());
    }
    namespace_path_from_c(&namespace_name, 1, error_message)
}

/// Copy owned strings into a malloc'd array of owned C strings.
///
/// An empty input returns a NULL array, matching the empty list contract of the
/// public API. Returns NULL on allocation or interior-NUL failure.
unsafe fn c_string_array(values: Vec<String>) -> Option<*mut *mut c_char> {
    let count = values.len();
    if count == 0 {
        return Some(ptr::null_mut());
    }

    let array = libc::malloc(count * std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
    if array.is_null() {
        return None;
    }

    for (index, value) in values.into_iter().enumerate() {
        let Ok(value) = CString::new(value) else {
            free_c_string_array(array, index);
            return None;
        };
        *array.add(index) = value.into_raw();
    }

    Some(array)
}

/// Free an array returned by `c_string_array`.
unsafe fn free_c_string_array(values: *mut *mut c_char, count: usize) {
    if values.is_null() {
        return;
    }

    for index in 0..count {
        let value = *values.add(index);
        if !value.is_null() {
            let _ = CString::from_raw(value);
        }
    }
    libc::free(values as *mut c_void);
}

/// Convert an optional SDK string map into parallel owned C string arrays.
///
/// The boolean distinguishes a map the SDK omitted from an empty map. A present
/// empty map has no arrays and a zero count.
unsafe fn c_string_map(
    map: Option<HashMap<String, String>>,
) -> Result<(bool, *mut *mut c_char, *mut *mut c_char, usize), ()> {
    let Some(map) = map else {
        return Ok((false, ptr::null_mut(), ptr::null_mut(), 0));
    };

    let count = map.len();
    if count == 0 {
        return Ok((true, ptr::null_mut(), ptr::null_mut(), 0));
    }

    let keys = libc::malloc(count * std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
    let values = libc::malloc(count * std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
    if keys.is_null() || values.is_null() {
        if !keys.is_null() {
            libc::free(keys as *mut c_void);
        }
        if !values.is_null() {
            libc::free(values as *mut c_void);
        }
        return Err(());
    }

    for (index, (key, value)) in map.into_iter().enumerate() {
        let (Ok(key), Ok(value)) = (CString::new(key), CString::new(value)) else {
            free_c_string_array(keys, index);
            free_c_string_array(values, index);
            return Err(());
        };
        *keys.add(index) = key.into_raw();
        *values.add(index) = value.into_raw();
    }

    Ok((true, keys, values, count))
}

/// Write a string listing result into the C output parameters.
unsafe fn write_string_list(
    result: Result<Vec<String>, lancedb::error::Error>,
    values_out: *mut *mut *mut c_char,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    match result {
        Ok(values) => {
            *count_out = values.len();
            match c_string_array(values) {
                Some(array) => {
                    *values_out = array;
                    LanceDBError::Success
                }
                None => {
                    set_unknown_error_message(error_message);
                    LanceDBError::Unknown
                }
            }
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Wrap an updated SDK builder for C callers
fn connect_builder_handle(builder: ConnectBuilder, is_remote: bool) -> *mut LanceDBConnectBuilder {
    Box::into_raw(Box::new(LanceDBConnectBuilder {
        inner: Box::new(builder),
        is_remote,
    }))
}

#[cfg(feature = "remote")]
fn apply_api_key(builder: ConnectBuilder, value: &str) -> ConnectBuilder {
    builder.api_key(value)
}

#[cfg(not(feature = "remote"))]
fn apply_api_key(builder: ConnectBuilder, _value: &str) -> ConnectBuilder {
    builder
}

#[cfg(feature = "remote")]
fn apply_region(builder: ConnectBuilder, value: &str) -> ConnectBuilder {
    builder.region(value)
}

#[cfg(not(feature = "remote"))]
fn apply_region(builder: ConnectBuilder, _value: &str) -> ConnectBuilder {
    builder
}

#[cfg(feature = "remote")]
fn apply_host_override(builder: ConnectBuilder, value: &str) -> ConnectBuilder {
    builder.host_override(value)
}

#[cfg(not(feature = "remote"))]
fn apply_host_override(builder: ConnectBuilder, _value: &str) -> ConnectBuilder {
    builder
}

#[cfg(feature = "remote")]
fn apply_sql_host_override(builder: ConnectBuilder, value: &str) -> ConnectBuilder {
    builder.sql_host_override(value)
}

#[cfg(not(feature = "remote"))]
fn apply_sql_host_override(builder: ConnectBuilder, _value: &str) -> ConnectBuilder {
    builder
}

/// Consume a builder and apply a remote-only string option
///
/// A NULL builder or value leaves nothing to return. The builder is consumed
/// even when the value is invalid, matching the other consuming setters. Native
/// connections keep the builder unchanged so the value cannot reach native
/// storage options.
unsafe fn connect_builder_with_remote_string(
    builder: *mut LanceDBConnectBuilder,
    value: *const c_char,
    apply: impl FnOnce(ConnectBuilder, &str) -> ConnectBuilder,
) -> *mut LanceDBConnectBuilder {
    if builder.is_null() {
        return ptr::null_mut();
    }

    let builder_box = Box::from_raw(builder);

    if value.is_null() {
        return ptr::null_mut();
    }

    let Ok(value) = CStr::from_ptr(value).to_str() else {
        return ptr::null_mut();
    };

    let is_remote = builder_box.is_remote;
    let connect_builder = *builder_box.inner;
    let updated_builder = if is_remote {
        apply(connect_builder, value)
    } else {
        connect_builder
    };
    connect_builder_handle(updated_builder, is_remote)
}

/// Consume a builder and apply a key/value option
///
/// A NULL builder, key or value leaves nothing to return. The builder is
/// consumed even when a string is invalid, matching the other consuming setters.
unsafe fn connect_builder_with_key_value(
    builder: *mut LanceDBConnectBuilder,
    key: *const c_char,
    value: *const c_char,
    apply: impl FnOnce(ConnectBuilder, &str, &str) -> ConnectBuilder,
) -> *mut LanceDBConnectBuilder {
    if builder.is_null() {
        return ptr::null_mut();
    }

    let builder_box = Box::from_raw(builder);

    if key.is_null() || value.is_null() {
        return ptr::null_mut();
    }

    let Ok(key) = CStr::from_ptr(key).to_str() else {
        return ptr::null_mut();
    };

    let Ok(value) = CStr::from_ptr(value).to_str() else {
        return ptr::null_mut();
    };

    let is_remote = builder_box.is_remote;
    let updated_builder = apply(*builder_box.inner, key, value);
    connect_builder_handle(updated_builder, is_remote)
}

/// Create a ConnectBuilder for the given URI
///
/// # Safety
/// - `uri` must be a valid null-terminated C string
/// - The returned pointer must be freed with `lancedb_connect_builder_free`
///
/// # Returns
/// - Non-null pointer to LanceDBConnectBuilder on success
/// - Null pointer on failure (e.g., invalid UTF-8 in uri)
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect(uri: *const c_char) -> *mut LanceDBConnectBuilder {
    if uri.is_null() {
        return ptr::null_mut();
    }

    let Ok(str_uri) = CStr::from_ptr(uri).to_str() else {
        return ptr::null_mut();
    };

    let builder = connect(str_uri);
    connect_builder_handle(builder, str_uri.starts_with("db"))
}

/// Execute the connection and return a Connection handle
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` will be consumed and must not be used after calling this function
/// - `connection` must be a valid pointer to receive the connection
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_execute(
    builder: *mut LanceDBConnectBuilder,
    connection: *mut *mut LanceDBConnection,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if builder.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let builder_box = Box::from_raw(builder);
    let connect_builder = *builder_box.inner;

    if connection.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let runtime = get_runtime();
    match runtime.block_on(connect_builder.execute()) {
        Ok(conn) => {
            let boxed_connection = Box::new(LanceDBConnection {
                inner: conn,
                uri_cache: OnceLock::new(),
            });
            *connection = Box::into_raw(boxed_connection);
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Set an option for the storage layer
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` will be consumed and must not be used after calling this function
/// - `key` must be a valid null-terminated C string
/// - `value` must be a valid null-terminated C string
///
/// # Returns
/// - A new pointer to LanceDBConnectBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_storage_option(
    builder: *mut LanceDBConnectBuilder,
    key: *const c_char,
    value: *const c_char,
) -> *mut LanceDBConnectBuilder {
    connect_builder_with_key_value(builder, key, value, |builder, key, value| {
        builder.storage_option(key, value)
    })
}

/// Set the API key for a LanceDB Enterprise or LanceDB Cloud connection
///
/// The value is only applied when the connection dispatches to the SDK remote
/// path. Native connections keep their storage options unchanged.
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` will be consumed and must not be used after calling this function
/// - `api_key` must be a valid null-terminated C string
///
/// # Returns
/// - A new pointer to LanceDBConnectBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_api_key(
    builder: *mut LanceDBConnectBuilder,
    api_key: *const c_char,
) -> *mut LanceDBConnectBuilder {
    connect_builder_with_remote_string(builder, api_key, apply_api_key)
}

/// Set the region for a db:// connection
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` will be consumed and must not be used after calling this function
/// - `region` must be a valid null-terminated C string
///
/// # Returns
/// - A new pointer to LanceDBConnectBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_region(
    builder: *mut LanceDBConnectBuilder,
    region: *const c_char,
) -> *mut LanceDBConnectBuilder {
    connect_builder_with_remote_string(builder, region, apply_region)
}

/// Set the host override for a LanceDB Enterprise connection
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` will be consumed and must not be used after calling this function
/// - `host_override` must be a valid null-terminated C string
///
/// # Returns
/// - A new pointer to LanceDBConnectBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_host_override(
    builder: *mut LanceDBConnectBuilder,
    host_override: *const c_char,
) -> *mut LanceDBConnectBuilder {
    connect_builder_with_remote_string(builder, host_override, apply_host_override)
}

/// Set the SQL service host override for a LanceDB Enterprise connection
///
/// The SQL endpoint is contacted only when the connection executes SQL.
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` will be consumed and must not be used after calling this function
/// - `sql_host_override` must be a valid null-terminated C string
///
/// # Returns
/// - A new pointer to LanceDBConnectBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_sql_host_override(
    builder: *mut LanceDBConnectBuilder,
    sql_host_override: *const c_char,
) -> *mut LanceDBConnectBuilder {
    connect_builder_with_remote_string(builder, sql_host_override, apply_sql_host_override)
}

/// Set an additional property for the connection's namespace client
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` will be consumed and must not be used after calling this function
/// - `key` and `value` must be valid null-terminated C strings
///
/// # Returns
/// - A new pointer to LanceDBConnectBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_namespace_client_property(
    builder: *mut LanceDBConnectBuilder,
    key: *const c_char,
    value: *const c_char,
) -> *mut LanceDBConnectBuilder {
    connect_builder_with_key_value(builder, key, value, |builder, key, value| {
        builder.namespace_client_property(key, value)
    })
}

/// Enable or disable manifest-backed directory namespace mode
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` will be consumed and must not be used after calling this function
///
/// # Returns
/// - A new pointer to LanceDBConnectBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_manifest_enabled(
    builder: *mut LanceDBConnectBuilder,
    enabled: bool,
) -> *mut LanceDBConnectBuilder {
    if builder.is_null() {
        return ptr::null_mut();
    }

    let builder_box = Box::from_raw(builder);
    let is_remote = builder_box.is_remote;
    let updated_builder = (*builder_box.inner).manifest_enabled(enabled);
    connect_builder_handle(updated_builder, is_remote)
}

/// Set the read consistency interval in milliseconds
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` will be consumed and must not be used after calling this function
///
/// # Returns
/// - A new pointer to LanceDBConnectBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_read_consistency_interval_ms(
    builder: *mut LanceDBConnectBuilder,
    interval_ms: u64,
) -> *mut LanceDBConnectBuilder {
    if builder.is_null() {
        return ptr::null_mut();
    }

    let builder_box = Box::from_raw(builder);
    let is_remote = builder_box.is_remote;
    let updated_builder = (*builder_box.inner)
        .read_consistency_interval(std::time::Duration::from_millis(interval_ms));
    connect_builder_handle(updated_builder, is_remote)
}

/// Set a session for the connection builder
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` will be consumed and must not be used after calling this function
/// - `session` can be NULL (no-op) or a valid pointer returned from `lancedb_session_new`
///
/// # Returns
/// - A new pointer to LanceDBConnectBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_session(
    builder: *mut LanceDBConnectBuilder,
    session: *const LanceDBSession,
) -> *mut LanceDBConnectBuilder {
    if builder.is_null() {
        return ptr::null_mut();
    }

    let builder_box = Box::from_raw(builder);
    let is_remote = builder_box.is_remote;
    let connect_builder = *builder_box.inner;

    let updated_builder = if session.is_null() {
        connect_builder
    } else {
        let session_ref = &*session;
        connect_builder.session(session_ref.inner.clone())
    };

    connect_builder_handle(updated_builder, is_remote)
}

/// Free a ConnectBuilder
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connect`
/// - `builder` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_connect_builder_free(builder: *mut LanceDBConnectBuilder) {
    if !builder.is_null() {
        let _ = Box::from_raw(builder);
    }
}

/// Get the URI of the connection
///
/// # Safety
/// - `connection` must be a valid pointer returned from `lancedb_connect_builder_execute`
/// - The returned string is valid until the connection is freed
/// - The caller must not free the returned string
///
/// # Returns
/// - Non-null pointer to null-terminated C string on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_uri(
    connection: *const LanceDBConnection,
) -> *const c_char {
    if connection.is_null() {
        return ptr::null();
    }

    let lancedb_conn = &*connection;

    // Get or create the cached URI string
    let cached_uri = lancedb_conn.uri_cache.get_or_init(|| {
        let uri_str = lancedb_conn.inner.uri();
        // Convert to CString, handling potential null bytes
        CString::new(uri_str).unwrap_or_else(|_| CString::new("invalid_uri").unwrap())
    });

    cached_uri.as_ptr()
}

/// Get the read consistency of a connection
///
/// # Safety
/// - `connection` must be a valid pointer returned from `lancedb_connect_builder_execute`
/// - `consistency_out` must be a valid pointer to receive the read consistency
/// - `error_message` can be NULL to ignore detailed error messages
///
/// On success, `consistency_out` is set to the SDK's read consistency. On
/// failure, it is left unchanged, so a caller can keep its own sentinel there.
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_read_consistency(
    connection: *const LanceDBConnection,
    consistency_out: *mut LanceDBReadConsistency,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);

    if connection.is_null() || consistency_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let read_consistency = match get_runtime().block_on((*connection).inner.read_consistency()) {
        Ok(read_consistency) => read_consistency,
        Err(error) => return handle_error(&error, error_message),
    };

    let (kind, interval_ms) = match read_consistency {
        ReadConsistency::Manual => (LanceDBReadConsistencyType::Manual, 0),
        ReadConsistency::Eventual(interval) => (
            LanceDBReadConsistencyType::Eventual,
            // The C setter only creates whole-millisecond intervals. Clamping
            // keeps a duration from any other source representable in the
            // fixed-width C field instead of truncating it.
            u64::try_from(interval.as_millis()).unwrap_or(u64::MAX),
        ),
        ReadConsistency::Strong => (LanceDBReadConsistencyType::Strong, 0),
    };

    *consistency_out = LanceDBReadConsistency { kind, interval_ms };
    LanceDBError::Success
}

/// Internal helper to create a session with optional custom registry
///
/// # Safety
/// - `options` can be NULL to use default session configuration
/// - `registry` must be a valid Arc<ObjectStoreRegistry> or None for default
unsafe fn create_session_impl(
    options: *const LanceDBSessionOptions,
    registry: Option<Arc<lancedb::ObjectStoreRegistry>>,
) -> *mut LanceDBSession {
    // Determine cache sizes from options
    let (index_cache_bytes, metadata_cache_bytes) = if options.is_null() {
        (
            DEFAULT_INDEX_CACHE_SIZE_BYTES,
            DEFAULT_METADATA_CACHE_SIZE_BYTES,
        )
    } else {
        let session_options = &*options;
        let index = if session_options.index_cache_bytes == 0 {
            DEFAULT_INDEX_CACHE_SIZE_BYTES
        } else {
            session_options.index_cache_bytes
        };
        let metadata = if session_options.metadata_cache_bytes == 0 {
            DEFAULT_METADATA_CACHE_SIZE_BYTES
        } else {
            session_options.metadata_cache_bytes
        };
        (index, metadata)
    };

    // Use provided registry or create default
    let registry = registry.unwrap_or_else(|| Arc::new(lancedb::ObjectStoreRegistry::default()));

    let session = Arc::new(lancedb::Session::new(
        index_cache_bytes,
        metadata_cache_bytes,
        registry,
    ));

    Box::into_raw(Box::new(LanceDBSession { inner: session }))
}

/// Create a new session
///
/// # Safety
/// - `options` can be NULL to use default session configuration
///
/// # Returns
/// - Non-null pointer to LanceDBSession on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_session_new(
    options: *const LanceDBSessionOptions,
) -> *mut LanceDBSession {
    create_session_impl(options, None)
}

/// Create a new session with a custom ObjectStoreRegistry
///
/// This function allows external code to provide a custom ObjectStoreRegistry.
///
/// # Safety
/// - `options` can be NULL to use default session configuration
/// - `registry` must be a valid pointer to `LanceDBObjectStoreRegistry` created via `lancedb_registry_new()`,
///   or NULL to use a default registry. This function takes ownership of the registry.
///
/// # Returns
/// - Non-null pointer to LanceDBSession on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_session_new_with_registry(
    options: *const LanceDBSessionOptions,
    registry: *const LanceDBObjectStoreRegistry,
) -> *mut LanceDBSession {
    let registry_arc = if registry.is_null() {
        None
    } else {
        // Take ownership of the LanceDBObjectStoreRegistry
        let registry_box = Box::from_raw(registry as *mut LanceDBObjectStoreRegistry);
        Some(registry_box.inner)
    };
    create_session_impl(options, registry_arc)
}

/// Create a new ObjectStoreRegistry
///
/// Creates a new registry with default ObjectStore providers.
/// The registry can be passed to `lancedb_session_new_with_registry()`.
///
/// # Returns
/// - Non-null pointer to LanceDBObjectStoreRegistry on success
/// - Null pointer on failure
#[no_mangle]
pub extern "C" fn lancedb_registry_new() -> *mut LanceDBObjectStoreRegistry {
    let registry = Arc::new(lancedb::ObjectStoreRegistry::default());
    let boxed = Box::new(LanceDBObjectStoreRegistry { inner: registry });
    Box::into_raw(boxed)
}

/// Free an ObjectStoreRegistry
///
/// Only call this if the registry was NOT passed to `lancedb_session_new_with_registry()`.
/// If the registry was passed to a session, the session owns it and will free it.
///
/// # Safety
/// - `registry` must be a valid pointer returned from `lancedb_registry_new()`
/// - `registry` must not have been passed to `lancedb_session_new_with_registry()`
/// - `registry` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_registry_free(registry: *mut LanceDBObjectStoreRegistry) {
    if !registry.is_null() {
        let _ = Box::from_raw(registry);
    }
}

/// Free an ObjectStoreProvider
///
/// Only call this if the provider was NOT passed to `lancedb_registry_insert_provider()`.
/// If the provider was inserted into a registry, the registry owns it.
///
/// # Safety
/// - `provider` must not have been passed to `lancedb_registry_insert_provider()`
#[no_mangle]
pub unsafe extern "C" fn lancedb_object_store_provider_free(
    provider: *mut LanceDBObjectStoreProvider,
) {
    if !provider.is_null() {
        let _ = Box::from_raw(provider);
    }
}

/// Insert an ObjectStoreProvider into an ObjectStoreRegistry for a given URL scheme
///
/// The provider's inner implementation must have been set before calling this function.
/// Ownership of the provider is transferred to the registry.
///
/// # Safety
/// - `registry` must be a valid pointer from lancedb_registry_new()
/// - `scheme` must be a valid null-terminated C string
/// - `provider` must be a valid pointer from lancedb_object_store_provider_new()
///   with its inner implementation populated
///
/// # Returns
/// - 0 on success
/// - error code on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_registry_insert_provider(
    registry: *mut LanceDBObjectStoreRegistry,
    scheme: *const c_char,
    provider: *mut LanceDBObjectStoreProvider,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if registry.is_null() || scheme.is_null() || provider.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let scheme_str = match CStr::from_ptr(scheme).to_str() {
        Ok(s) => s,
        Err(_) => {
            set_custom_error_message(error_message, "Invalid URL scheme string");
            return LanceDBError::InvalidInput;
        }
    };

    let provider_box = Box::from_raw(provider);
    let provider_arc = match provider_box.inner {
        Some(arc) => arc,
        None => {
            set_custom_error_message(
                error_message,
                "LanceDBObjectStoreProvider is not initialized",
            );
            return LanceDBError::InvalidArgument;
        }
    };

    let registry_ref = &*registry;
    registry_ref.inner.insert(scheme_str, provider_arc);
    LanceDBError::Success
}

/// Get index cache stats for a session
///
/// # Safety
/// - `session` must be a valid pointer returned from `lancedb_session_new`
/// - `out_stats` must be a valid pointer to receive cache stats
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_session_index_cache_stats(
    session: *const LanceDBSession,
    out_stats: *mut LanceDBSessionCacheStats,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if session.is_null() || out_stats.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let session_ref = &*session;
    let stats = get_runtime().block_on(session_ref.inner.index_cache_stats());
    *out_stats = LanceDBSessionCacheStats {
        hits: stats.hits,
        misses: stats.misses,
        num_entries: stats.num_entries,
        size_bytes: stats.size_bytes,
    };
    LanceDBError::Success
}

/// Get metadata cache stats for a session
///
/// # Safety
/// - `session` must be a valid pointer returned from `lancedb_session_new`
/// - `out_stats` must be a valid pointer to receive cache stats
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_session_metadata_cache_stats(
    session: *const LanceDBSession,
    out_stats: *mut LanceDBSessionCacheStats,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if session.is_null() || out_stats.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let session_ref = &*session;
    let stats = get_runtime().block_on(session_ref.inner.metadata_cache_stats());
    *out_stats = LanceDBSessionCacheStats {
        hits: stats.hits,
        misses: stats.misses,
        num_entries: stats.num_entries,
        size_bytes: stats.size_bytes,
    };
    LanceDBError::Success
}

/// Free a Session
///
/// # Safety
/// - `session` must be a valid pointer returned from `lancedb_session_new`
/// - `session` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_session_free(session: *mut LanceDBSession) {
    if !session.is_null() {
        let _ = Box::from_raw(session);
    }
}

/// Create a new table with Arrow schema and data
///
/// # Safety
/// - `connection` must be a valid pointer returned from `lancedb_connect_builder_execute`
/// - `table_name` must be a valid null-terminated C string
/// - `schema_ptr` must be a valid pointer to Arrow C ABI schema
/// - `reader` must be a valid pointer to LanceDBRecordBatchReader or NULL for empty table
/// - `table_out` must be a valid pointer to receive the created table
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_create(
    connection: *const LanceDBConnection,
    table_name: *const c_char,
    schema_ptr: *const arrow_schema::ffi::FFI_ArrowSchema,
    reader: *mut LanceDBRecordBatchReader,
    table_out: *mut *mut LanceDBTable,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() || table_name.is_null() || schema_ptr.is_null() || table_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(table_name_str) = CStr::from_ptr(table_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let conn = &(*connection).inner;
    let runtime = get_runtime();

    match runtime.block_on(async {
        // Import schema from Arrow C ABI
        let schema = match Schema::try_from(&*schema_ptr) {
            Ok(s) => Arc::new(s),
            Err(_) => {
                return Err(lancedb::error::Error::InvalidInput {
                    message: "Failed to import Arrow schema from C ABI".to_string(),
                })
            }
        };

        let batch_reader: Box<dyn RecordBatchReader + Send> = if reader.is_null() {
            // Create empty reader with the schema
            let empty_batches = RecordBatchIterator::new(
                std::iter::empty::<Result<RecordBatch, arrow_schema::ArrowError>>(),
                schema.clone(),
            );
            Box::new(empty_batches)
        } else {
            // Take ownership of the reader
            let reader_box = Box::from_raw(reader);
            reader_box.into_inner()
        };

        // lancedb panics on an invalid table name instead of returning an error,
        // so the name is validated before the table is created
        validate_table_name(table_name_str)?;

        conn.create_table(table_name_str, batch_reader)
            .execute()
            .await
    }) {
        Ok(table) => {
            let boxed_table = Box::new(LanceDBTable { inner: table });
            *table_out = Box::into_raw(boxed_table);
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Convert a raw C create mode to the SDK mode
///
/// The SDK builds its ExistOk open request without the create location or the
/// per-table storage options. The callback puts both back so the match reopens
/// the table this create was meant to create, not a same-name default table.
fn create_table_mode_from_c(
    value: c_int,
    location: Option<String>,
    storage_options: Vec<(String, String)>,
) -> Result<CreateTableMode, String> {
    match value {
        // LANCEDB_CREATE_TABLE_MODE_CREATE
        0 => Ok(CreateTableMode::Create),
        // LANCEDB_CREATE_TABLE_MODE_OVERWRITE
        1 => Ok(CreateTableMode::Overwrite),
        // LANCEDB_CREATE_TABLE_MODE_EXIST_OK
        2 => Ok(CreateTableMode::exist_ok(move |mut request| {
            if let Some(location) = location {
                request.location = Some(location);
            }
            if !storage_options.is_empty() {
                let mut read_params = request.lance_read_params.unwrap_or_default();
                let mut store_params = read_params.store_options.unwrap_or_default();
                store_params.storage_options_accessor =
                    Some(Arc::new(StorageOptionsAccessor::with_static_options(
                        storage_options.into_iter().collect::<HashMap<_, _>>(),
                    )));
                read_params.store_options = Some(store_params);
                request.lance_read_params = Some(read_params);
            }
            request
        })),
        other => Err(format!(
            "'mode' value {} is not a LanceDBCreateTableMode enumerator",
            other
        )),
    }
}

/// Convert the per-table storage option arrays from C
unsafe fn create_storage_options(
    options: &LanceDBCreateTableOptions,
) -> Result<Vec<(String, String)>, String> {
    if options.storage_count == 0 {
        return Ok(Vec::new());
    }
    if options.storage_keys.is_null() || options.storage_values.is_null() {
        return Err(
            "storage_keys and storage_values must not be NULL when storage_count is non-zero"
                .to_string(),
        );
    }

    let mut pairs = Vec::new();
    for index in 0..options.storage_count {
        let key = *options.storage_keys.add(index);
        let value = *options.storage_values.add(index);
        if key.is_null() || value.is_null() {
            return Err(format!("storage option {} has a NULL key or value", index));
        }
        let key = CStr::from_ptr(key)
            .to_str()
            .map_err(|_| format!("storage option key {} is not valid UTF-8", index))?;
        let value = CStr::from_ptr(value)
            .to_str()
            .map_err(|_| format!("storage option value {} is not valid UTF-8", index))?;
        pairs.push((key.to_string(), value.to_string()));
    }
    Ok(pairs)
}

/// Convert the optional table location from C
unsafe fn create_location(options: &LanceDBCreateTableOptions) -> Result<Option<String>, String> {
    if options.location.is_null() {
        return Ok(None);
    }
    CStr::from_ptr(options.location)
        .to_str()
        .map(|location| Some(location.to_string()))
        .map_err(|_| "'location' is not valid UTF-8".to_string())
}

/// Create a table with the SDK create builder options
///
/// # Safety
/// - `connection` must be a valid pointer returned from `lancedb_connect_builder_execute`
/// - `table_name` must be a valid null-terminated C string
/// - `schema_ptr` must be a valid pointer to Arrow C ABI schema
/// - `reader` must be NULL or a valid reader. It is always consumed, also when
///   another argument, an option, the schema or the SDK rejects the create
/// - `options` may be NULL for the SDK defaults, or point to a readable
///   `LanceDBCreateTableOptions`. Its pointers and strings are borrowed
/// - `table_out` must be a valid pointer to receive the created table
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*table_out` set to the created or existing table
/// - `LANCEDB_INVALID_ARGUMENT` for a NULL pointer, an invalid string, an
///   unknown write option bit, an out-of-range mode or an invalid boolean
/// - The SDK error code for a rejected create
///
/// The Arrow schema stays owned by the caller. On entry the error message slot
/// is cleared; free a returned message with `lancedb_free_string()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_create_with_options(
    connection: *const LanceDBConnection,
    table_name: *const c_char,
    schema_ptr: *const arrow_schema::ffi::FFI_ArrowSchema,
    reader: *mut LanceDBRecordBatchReader,
    options: *const LanceDBCreateTableOptions,
    table_out: *mut *mut LanceDBTable,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !table_out.is_null() {
        *table_out = ptr::null_mut();
    }

    // The new entry owns the reader from the moment it is called, so every
    // rejection below drops it exactly once.
    let reader = if reader.is_null() {
        None
    } else {
        Some(Box::from_raw(reader).into_inner())
    };
    clear_error_message(error_message);

    if connection.is_null() || table_name.is_null() || schema_ptr.is_null() || table_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(table_name_str) = CStr::from_ptr(table_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let schema = match Schema::try_from(&*schema_ptr) {
        Ok(schema) => Arc::new(schema),
        Err(error) => {
            return handle_error(
                &lancedb::error::Error::Arrow { source: error },
                error_message,
            )
        }
    };

    let mut mode = CreateTableMode::Create;
    let mut namespace_path = Vec::new();
    let mut location = None;
    let mut storage_options = Vec::new();
    let mut write_options = lancedb::table::WriteOptions::default();
    let mut allow_external_blob_outside_bases = false;

    if !options.is_null() {
        let options = &*options;
        write_options = match write_options_from_c(options.write_options) {
            Ok(write_options) => write_options,
            Err(message) => {
                set_custom_error_message(error_message, &message);
                return LanceDBError::InvalidArgument;
            }
        };
        location = match create_location(options) {
            Ok(location) => location,
            Err(message) => {
                set_custom_error_message(error_message, &message);
                return LanceDBError::InvalidArgument;
            }
        };
        storage_options = match create_storage_options(options) {
            Ok(storage_options) => storage_options,
            Err(message) => {
                set_custom_error_message(error_message, &message);
                return LanceDBError::InvalidArgument;
            }
        };
        allow_external_blob_outside_bases = match bool_from_c_int(
            "allow_external_blob_outside_bases",
            options.allow_external_blob_outside_bases,
        ) {
            Ok(allow) => allow,
            Err(message) => {
                set_custom_error_message(error_message, &message);
                return LanceDBError::InvalidArgument;
            }
        };
        namespace_path = match namespace_path_from_c(
            options.namespace_path,
            options.namespace_path_len,
            error_message,
        ) {
            Some(path) => path,
            None => return LanceDBError::InvalidArgument,
        };
        mode =
            match create_table_mode_from_c(options.mode, location.clone(), storage_options.clone())
            {
                Ok(mode) => mode,
                Err(message) => {
                    set_custom_error_message(error_message, &message);
                    return LanceDBError::InvalidArgument;
                }
            };
    }

    let conn = &(*connection).inner;
    let runtime = get_runtime();
    let outcome = runtime.block_on(async {
        let mut builder = if let Some(batch_reader) = reader {
            conn.create_table(table_name_str, batch_reader)
        } else {
            conn.create_empty_table(table_name_str, schema.clone())
        };
        builder = builder.mode(mode);
        if !namespace_path.is_empty() {
            builder = builder.namespace(namespace_path);
        }
        if let Some(location) = location {
            builder = builder.location(location);
        }
        builder = builder.write_options(write_options);
        if !storage_options.is_empty() {
            builder = builder.storage_options(storage_options);
        }
        builder = builder.allow_external_blob_outside_bases(allow_external_blob_outside_bases);
        builder.execute().await
    });

    match outcome {
        Ok(table) => {
            let boxed_table = Box::new(LanceDBTable { inner: table });
            *table_out = Box::into_raw(boxed_table);
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Get table names from the connection
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `names_out` must be a valid pointer to receive the array of string pointers
/// - `count_out` must be a valid pointer to receive the count
/// - `error_message` can be NULL to ignore detailed error messages
/// - The caller is responsible for freeing the returned strings and array
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_table_names(
    connection: *const LanceDBConnection,
    names_out: *mut *mut *mut c_char,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() || names_out.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let conn = &(*connection).inner;
    let runtime = get_runtime();

    match runtime.block_on(conn.table_names().execute()) {
        Ok(names) => {
            let count = names.len();
            *count_out = count;

            match c_string_array(names) {
                Some(names_array) => {
                    *names_out = names_array;
                    LanceDBError::Success
                }
                None => {
                    set_unknown_error_message(error_message);
                    LanceDBError::Unknown
                }
            }
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Free table names array returned by lancedb_connection_table_names
///
/// # Safety
/// - `names` must be a pointer returned by `lancedb_connection_table_names`
/// - `count` must match the count returned by `lancedb_connection_table_names`
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_table_names(names: *mut *mut c_char, count: usize) {
    free_c_string_array(names, count);
}

/// Create a TableNamesBuilder for paginated table listing
///
/// # Safety
/// - `connection` must be a valid pointer
///
/// # Returns
/// - Non-null pointer to LanceDBTableNamesBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_table_names_builder(
    connection: *const LanceDBConnection,
) -> *mut LanceDBTableNamesBuilder {
    if connection.is_null() {
        return ptr::null_mut();
    }

    let conn = &(*connection).inner;
    let builder = conn.table_names();

    let boxed_builder = Box::new(LanceDBTableNamesBuilder {
        inner: Box::new(builder),
    });

    Box::into_raw(boxed_builder)
}

/// Set limit on TableNamesBuilder
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connection_table_names_builder`
/// - `builder` will be consumed and must not be used after calling this function
///
/// # Returns
/// - Non-null pointer to LanceDBTableNamesBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_names_builder_limit(
    builder: *mut LanceDBTableNamesBuilder,
    limit: u32,
) -> *mut LanceDBTableNamesBuilder {
    if builder.is_null() {
        return ptr::null_mut();
    }

    let builder_box = Box::from_raw(builder);
    let table_names_builder = *builder_box.inner;
    let updated_builder = table_names_builder.limit(limit);

    let boxed_builder = Box::new(LanceDBTableNamesBuilder {
        inner: Box::new(updated_builder),
    });

    Box::into_raw(boxed_builder)
}

/// Set start_after on TableNamesBuilder for pagination
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connection_table_names_builder`
/// - `start_after` must be a valid null-terminated C string
/// - `builder` will be consumed and must not be used after calling this function
///
/// # Returns
/// - Non-null pointer to LanceDBTableNamesBuilder on success
/// - Null pointer on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_names_builder_start_after(
    builder: *mut LanceDBTableNamesBuilder,
    start_after: *const c_char,
) -> *mut LanceDBTableNamesBuilder {
    if builder.is_null() {
        return ptr::null_mut();
    }

    let builder_box = Box::from_raw(builder);

    if start_after.is_null() {
        return ptr::null_mut();
    }

    let Ok(start_after_str) = CStr::from_ptr(start_after).to_str() else {
        return ptr::null_mut();
    };

    let table_names_builder = *builder_box.inner;
    let updated_builder = table_names_builder.start_after(start_after_str.to_string());

    let boxed_builder = Box::new(LanceDBTableNamesBuilder {
        inner: Box::new(updated_builder),
    });

    Box::into_raw(boxed_builder)
}

/// Set the namespace path on TableNamesBuilder
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connection_table_names_builder`
/// - `namespace_path` must point to `namespace_path_len` valid C strings, or be NULL
///   when `namespace_path_len` is zero
/// - `builder` will be consumed and must not be used after calling this function
///
/// # Returns
/// - Non-null pointer to LanceDBTableNamesBuilder on success
/// - Null pointer when the builder, the path, or a path component is invalid
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_names_builder_namespace(
    builder: *mut LanceDBTableNamesBuilder,
    namespace_path: *const *const c_char,
    namespace_path_len: usize,
) -> *mut LanceDBTableNamesBuilder {
    if builder.is_null() {
        return ptr::null_mut();
    }

    let builder_box = Box::from_raw(builder);
    let Some(namespace_path) =
        namespace_path_from_c(namespace_path, namespace_path_len, ptr::null_mut())
    else {
        return ptr::null_mut();
    };

    let table_names_builder = *builder_box.inner;
    let updated_builder = table_names_builder.namespace(namespace_path);

    let boxed_builder = Box::new(LanceDBTableNamesBuilder {
        inner: Box::new(updated_builder),
    });

    Box::into_raw(boxed_builder)
}

/// Execute TableNamesBuilder and return table names
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connection_table_names_builder`
/// - `builder` will be consumed and must not be used after calling this function
/// - `names_out` must be a valid pointer to receive the array of string pointers
/// - `count_out` must be a valid pointer to receive the count
/// - `error_message` can be NULL to ignore detailed error messages
/// - The caller is responsible for freeing the returned strings and array
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_names_builder_execute(
    builder: *mut LanceDBTableNamesBuilder,
    names_out: *mut *mut *mut c_char,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if builder.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let builder_box = Box::from_raw(builder);

    if names_out.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let table_names_builder = *builder_box.inner;

    let runtime = get_runtime();

    match runtime.block_on(table_names_builder.execute()) {
        Ok(names) => {
            let count = names.len();
            *count_out = count;

            match c_string_array(names) {
                Some(names_array) => {
                    *names_out = names_array;
                    LanceDBError::Success
                }
                None => {
                    set_unknown_error_message(error_message);
                    LanceDBError::Unknown
                }
            }
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Free a TableNamesBuilder
///
/// # Safety
/// - `builder` must be a valid pointer returned from `lancedb_connection_table_names_builder`
/// - `builder` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_names_builder_free(builder: *mut LanceDBTableNamesBuilder) {
    if !builder.is_null() {
        let _ = Box::from_raw(builder);
    }
}

/// Free namespace list array returned by lancedb_connection_list_namespaces
///
/// # Safety
/// - `namespaces` must be a pointer returned by `lancedb_connection_list_namespaces`
/// - `count` must match the count returned by `lancedb_connection_list_namespaces`
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_namespace_list(namespaces: *mut *mut c_char, count: usize) {
    free_c_string_array(namespaces, count);
}

/// A namespace description returned by `lancedb_connection_describe_namespace_path`
///
/// `property_keys` and `property_values` are parallel arrays of `property_count`
/// entries, and likewise for the context arrays. `has_properties` and
/// `has_context` distinguish a map the SDK omitted (false) from an empty map
/// (true, with NULL arrays and a zero count).
#[repr(C)]
pub struct LanceDBNamespaceDescription {
    /// Whether the SDK returned namespace properties
    pub has_properties: bool,
    /// Namespace property names
    pub property_keys: *mut *mut c_char,
    /// Namespace property values
    pub property_values: *mut *mut c_char,
    /// Number of namespace properties
    pub property_count: usize,
    /// Whether the SDK returned response context
    pub has_context: bool,
    /// Response context names
    pub context_keys: *mut *mut c_char,
    /// Response context values
    pub context_values: *mut *mut c_char,
    /// Number of response context entries
    pub context_count: usize,
}

/// The empty state of a `LanceDBNamespaceDescription`.
fn empty_namespace_description() -> LanceDBNamespaceDescription {
    LanceDBNamespaceDescription {
        has_properties: false,
        property_keys: ptr::null_mut(),
        property_values: ptr::null_mut(),
        property_count: 0,
        has_context: false,
        context_keys: ptr::null_mut(),
        context_values: ptr::null_mut(),
        context_count: 0,
    }
}

/// Free a namespace description returned by
/// `lancedb_connection_describe_namespace_path`
///
/// The arrays and their strings are freed and the fields are zeroed. The struct
/// itself stays owned by the caller.
///
/// # Safety
/// - `description` must be a pointer to a description returned by
///   `lancedb_connection_describe_namespace_path`, or NULL
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_namespace_description(
    description: *mut LanceDBNamespaceDescription,
) {
    if description.is_null() {
        return;
    }

    let description = &mut *description;
    free_c_string_array(description.property_keys, description.property_count);
    free_c_string_array(description.property_values, description.property_count);
    free_c_string_array(description.context_keys, description.context_count);
    free_c_string_array(description.context_values, description.context_count);

    *description = empty_namespace_description();
}

/// Open an existing table
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `table_name` must be a valid null-terminated C string
/// - `table_out` must be a valid pointer to receive the opened table
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_open_table(
    connection: *const LanceDBConnection,
    table_name: *const c_char,
    table_out: *mut *mut LanceDBTable,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() || table_name.is_null() || table_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(table_name_str) = CStr::from_ptr(table_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let conn = &(*connection).inner;
    let runtime = get_runtime();

    match runtime.block_on(async {
        // lancedb panics on an invalid table name instead of returning an error,
        // so the name is validated before the table is opened
        validate_table_name(table_name_str)?;

        conn.open_table(table_name_str).execute().await
    }) {
        Ok(table) => {
            let boxed_table = Box::new(LanceDBTable { inner: table });
            *table_out = Box::into_raw(boxed_table);
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Open an existing table with SDK open-table builder options
///
/// # Safety
/// - `connection` must be a valid pointer returned from `lancedb_connect_builder_execute`
/// - `table_name` must be a valid null-terminated C string
/// - `options` may be NULL for the SDK defaults, or point to a readable
///   `LanceDBOpenTableOptions`. Its pointers and strings are borrowed
/// - `table_out` must be a valid pointer to receive the opened table
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with `*table_out` set to the opened table
/// - `LANCEDB_INVALID_ARGUMENT` for a NULL pointer or an invalid option
/// - `LANCEDB_INVALID_TABLE_NAME` for a name the SDK rejects
/// - The SDK error code for a rejected open
///
/// On entry, `*table_out` and the error message slot are cleared. The table is
/// written only on success, so a failure leaves `*table_out` NULL. That differs
/// from the legacy `lancedb_connection_open_table`, which leaves its output
/// unchanged on failure. The caller frees the returned table with
/// `lancedb_table_free()` and a returned error message with
/// `lancedb_free_string()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_open_table_with_options(
    connection: *const LanceDBConnection,
    table_name: *const c_char,
    options: *const LanceDBOpenTableOptions,
    table_out: *mut *mut LanceDBTable,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !table_out.is_null() {
        *table_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if connection.is_null() || table_name.is_null() || table_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(table_name_str) = CStr::from_ptr(table_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    // lancedb panics on an invalid table name instead of returning an error,
    // so the name is validated before the table is opened.
    if let Err(error) = validate_table_name(table_name_str) {
        return handle_error(&error, error_message);
    }

    let settings = match open_table_settings_from_c(options, error_message) {
        Ok(settings) => settings,
        Err(error) => return error,
    };

    let conn = &(*connection).inner;
    let runtime = get_runtime();
    let outcome = runtime.block_on(async {
        let mut builder = conn
            .open_table(table_name_str)
            .namespace(settings.namespace_path);
        if let Some(location) = settings.location {
            builder = builder.location(location);
        }
        if !settings.storage_options.is_empty() {
            builder = builder.storage_options(settings.storage_options);
        }
        if let Some(index_cache_size) = settings.index_cache_size {
            builder = builder.index_cache_size(index_cache_size);
        }
        if let Some(version) = settings.version {
            builder = builder.version(version);
        }
        if let Some(branch) = settings.branch {
            builder = builder.branch(branch);
        }
        if let Some(managed_versioning) = settings.managed_versioning {
            builder = builder.managed_versioning(managed_versioning);
        }
        builder.execute().await
    });

    match outcome {
        Ok(table) => {
            let boxed_table = Box::new(LanceDBTable { inner: table });
            *table_out = Box::into_raw(boxed_table);
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Clone a table from an existing source table
///
/// A shallow clone shares the source's data files while keeping an independent
/// manifest. Embedded directory connections support shallow clones only and
/// reject a non-empty target namespace. Remote connections forward both
/// settings to the server.
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `target_table_name` and `source_uri` must be valid null-terminated C strings
/// - `options`, when not NULL, must point to a valid `LanceDBCloneTableOptions`
///   whose selected string fields are valid C strings and whose namespace array
///   obeys the same contract as the other namespace path entry points
/// - `table_out` must be a valid pointer to receive the cloned table
/// - `error_message` can be NULL to ignore detailed error messages
///
/// On entry, a non-NULL `table_out` is cleared and a non-NULL `error_message` is
/// cleared. `table_out` is written only after the clone succeeds. The source and
/// target strings and everything reached from `options` are borrowed for the
/// duration of the call.
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_clone_table(
    connection: *const LanceDBConnection,
    target_table_name: *const c_char,
    source_uri: *const c_char,
    options: *const LanceDBCloneTableOptions,
    table_out: *mut *mut LanceDBTable,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);
    if !table_out.is_null() {
        *table_out = ptr::null_mut();
    }

    if connection.is_null()
        || target_table_name.is_null()
        || source_uri.is_null()
        || table_out.is_null()
    {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(target_table_name) = CStr::from_ptr(target_table_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };
    let Ok(source_uri) = CStr::from_ptr(source_uri).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let mut builder = (*connection)
        .inner
        .clone_table(target_table_name, source_uri);

    if !options.is_null() {
        let has_source_version = ptr::addr_of!((*options).has_source_version).read();
        if has_source_version != 0 {
            builder = builder.source_version(ptr::addr_of!((*options).source_version).read());
        }

        let source_tag = ptr::addr_of!((*options).source_tag).read();
        if !source_tag.is_null() {
            let Ok(source_tag) = CStr::from_ptr(source_tag).to_str() else {
                set_invalid_argument_message(error_message);
                return LanceDBError::InvalidArgument;
            };
            builder = builder.source_tag(source_tag);
        }

        let Some(target_namespace) = namespace_path_from_c(
            ptr::addr_of!((*options).target_namespace).read(),
            ptr::addr_of!((*options).target_namespace_len).read(),
            error_message,
        ) else {
            return LanceDBError::InvalidArgument;
        };
        builder = builder.target_namespace(target_namespace);

        if ptr::addr_of!((*options).exist_ok).read() != 0 {
            builder = builder.exist_ok(true);
        }
        if ptr::addr_of!((*options).has_is_shallow).read() != 0 {
            builder = builder.is_shallow(ptr::addr_of!((*options).is_shallow).read() != 0);
        }
    }

    match get_runtime().block_on(builder.execute()) {
        Ok(table) => {
            *table_out = Box::into_raw(Box::new(LanceDBTable { inner: table }));
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Drop a table from the database
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `table_name` must be a valid null-terminated C string
/// - `namespace` can be NULL for no namespace, or a valid null-terminated C string
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_drop_table(
    connection: *const LanceDBConnection,
    table_name: *const c_char,
    namespace: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() || table_name.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(table_name_str) = CStr::from_ptr(table_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let conn = &(*connection).inner;
    let runtime = get_runtime();

    let Some(namespace_path) = single_namespace_from_c(namespace, error_message) else {
        return LanceDBError::InvalidArgument;
    };

    match runtime.block_on(conn.drop_table(table_name_str, &namespace_path)) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Drop a table from a multi-level namespace path
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `table_name` must be a valid null-terminated C string
/// - `namespace_path` must point to `namespace_path_len` valid C strings, or be NULL
///   when `namespace_path_len` is zero for the root namespace
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_drop_table_path(
    connection: *const LanceDBConnection,
    table_name: *const c_char,
    namespace_path: *const *const c_char,
    namespace_path_len: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() || table_name.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(table_name_str) = CStr::from_ptr(table_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let Some(namespace_path) =
        namespace_path_from_c(namespace_path, namespace_path_len, error_message)
    else {
        return LanceDBError::InvalidArgument;
    };

    let conn = &(*connection).inner;
    match get_runtime().block_on(conn.drop_table(table_name_str, &namespace_path)) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Rename a table in the database
///
/// Renaming is supported in LanceDB Enterprise and by namespace backends that
/// implement it. Directory namespaces support renaming tables resolved through
/// their manifest, including within or across child namespaces; directory-listing
/// root tables cannot be renamed.
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `old_name` and `new_name` must be valid null-terminated C strings
/// - `cur_namespace` and `new_namespace` can be NULL for no namespace, or valid null-terminated C strings
/// - `error_message` can be NULL to ignore detailed error messages
///
/// A NULL `new_namespace` omits the target namespace from the request. It does
/// not promise a move to the root namespace.
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_rename_table(
    connection: *const LanceDBConnection,
    old_name: *const c_char,
    new_name: *const c_char,
    cur_namespace: *const c_char,
    new_namespace: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() || old_name.is_null() || new_name.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(old_name_str) = CStr::from_ptr(old_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let Ok(new_name_str) = CStr::from_ptr(new_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let conn = &(*connection).inner;
    let runtime = get_runtime();

    let Some(cur_namespace_path) = single_namespace_from_c(cur_namespace, error_message) else {
        return LanceDBError::InvalidArgument;
    };
    let Some(new_namespace_path) = single_namespace_from_c(new_namespace, error_message) else {
        return LanceDBError::InvalidArgument;
    };

    match runtime.block_on(conn.rename_table(
        old_name_str,
        new_name_str,
        &cur_namespace_path,
        &new_namespace_path,
    )) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Rename a table between multi-level namespace paths
///
/// Renaming is supported in LanceDB Enterprise and by namespace backends that
/// implement it. Directory namespaces support renaming tables resolved through
/// their manifest, including within or across child namespaces; directory-listing
/// root tables cannot be renamed.
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `old_name` and `new_name` must be valid null-terminated C strings
/// - `cur_namespace_path` and `new_namespace_path` must each point to their
///   corresponding length of valid C strings, or be NULL when the length is zero
///   for the root namespace
/// - `error_message` can be NULL to ignore detailed error messages
///
/// A zero-length `new_namespace_path` omits the target namespace from the
/// request. It does not promise a move to the root namespace.
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_rename_table_path(
    connection: *const LanceDBConnection,
    old_name: *const c_char,
    new_name: *const c_char,
    cur_namespace_path: *const *const c_char,
    cur_namespace_path_len: usize,
    new_namespace_path: *const *const c_char,
    new_namespace_path_len: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() || old_name.is_null() || new_name.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Ok(old_name_str) = CStr::from_ptr(old_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };
    let Ok(new_name_str) = CStr::from_ptr(new_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let Some(cur_namespace_path) =
        namespace_path_from_c(cur_namespace_path, cur_namespace_path_len, error_message)
    else {
        return LanceDBError::InvalidArgument;
    };
    let Some(new_namespace_path) =
        namespace_path_from_c(new_namespace_path, new_namespace_path_len, error_message)
    else {
        return LanceDBError::InvalidArgument;
    };

    match get_runtime().block_on((*connection).inner.rename_table(
        old_name_str,
        new_name_str,
        &cur_namespace_path,
        &new_namespace_path,
    )) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Drop all tables in the database
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `namespace` can be NULL for no namespace, or a valid null-terminated C string
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_drop_all_tables(
    connection: *const LanceDBConnection,
    namespace: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let conn = &(*connection).inner;
    let runtime = get_runtime();

    let Some(namespace_path) = single_namespace_from_c(namespace, error_message) else {
        return LanceDBError::InvalidArgument;
    };

    match runtime.block_on(conn.drop_all_tables(&namespace_path)) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Drop all tables in a multi-level namespace path
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `namespace_path` must point to `namespace_path_len` valid C strings, or be NULL
///   when `namespace_path_len` is zero for the root namespace
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_drop_all_tables_path(
    connection: *const LanceDBConnection,
    namespace_path: *const *const c_char,
    namespace_path_len: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(namespace_path) =
        namespace_path_from_c(namespace_path, namespace_path_len, error_message)
    else {
        return LanceDBError::InvalidArgument;
    };

    match get_runtime().block_on((*connection).inner.drop_all_tables(&namespace_path)) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Create a new namespace
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `namespace_name` must be a valid null-terminated C string
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_create_namespace(
    connection: *const LanceDBConnection,
    namespace_name: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() || namespace_name.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(namespace_path) = single_namespace_from_c(namespace_name, error_message) else {
        return LanceDBError::InvalidArgument;
    };

    let mut request = CreateNamespaceRequest::new();
    request.id = Some(namespace_path);
    match get_runtime().block_on((*connection).inner.create_namespace(request)) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Create a namespace at a multi-level namespace path
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `namespace_path` must point to `namespace_path_len` valid C strings, or be NULL
///   when `namespace_path_len` is zero for the root namespace
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_create_namespace_path(
    connection: *const LanceDBConnection,
    namespace_path: *const *const c_char,
    namespace_path_len: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(namespace_path) =
        namespace_path_from_c(namespace_path, namespace_path_len, error_message)
    else {
        return LanceDBError::InvalidArgument;
    };

    let mut request = CreateNamespaceRequest::new();
    request.id = Some(namespace_path);
    match get_runtime().block_on((*connection).inner.create_namespace(request)) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Drop a namespace
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `namespace_name` must be a valid null-terminated C string
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_drop_namespace(
    connection: *const LanceDBConnection,
    namespace_name: *const c_char,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() || namespace_name.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(namespace_path) = single_namespace_from_c(namespace_name, error_message) else {
        return LanceDBError::InvalidArgument;
    };

    let mut request = DropNamespaceRequest::new();
    request.id = Some(namespace_path);
    match get_runtime().block_on((*connection).inner.drop_namespace(request)) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Drop a namespace at a multi-level namespace path
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `namespace_path` must point to `namespace_path_len` valid C strings, or be NULL
///   when `namespace_path_len` is zero for the root namespace
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_drop_namespace_path(
    connection: *const LanceDBConnection,
    namespace_path: *const *const c_char,
    namespace_path_len: usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(namespace_path) =
        namespace_path_from_c(namespace_path, namespace_path_len, error_message)
    else {
        return LanceDBError::InvalidArgument;
    };

    let mut request = DropNamespaceRequest::new();
    request.id = Some(namespace_path);
    match get_runtime().block_on((*connection).inner.drop_namespace(request)) {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// List all namespaces in the database
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `namespace_parent` can be NULL for root namespace, or a valid null-terminated C string
/// - `namespaces_out` must be a valid pointer to receive the array of string pointers
/// - `count_out` must be a valid pointer to receive the count
/// - `error_message` can be NULL to ignore detailed error messages
/// - The caller is responsible for freeing the returned strings and array
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_list_namespaces(
    connection: *const LanceDBConnection,
    namespace_parent: *const c_char,
    namespaces_out: *mut *mut *mut c_char,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if connection.is_null() || namespaces_out.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(parent_namespace) = single_namespace_from_c(namespace_parent, error_message) else {
        return LanceDBError::InvalidArgument;
    };

    let result = get_runtime().block_on(
        (*connection)
            .inner
            .list_namespaces(&parent_namespace, Default::default())
            .try_collect::<Vec<String>>(),
    );
    write_string_list(result, namespaces_out, count_out, error_message)
}

/// List all namespaces under a multi-level namespace path
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `namespace_path` must point to `namespace_path_len` valid C strings, or be NULL
///   when `namespace_path_len` is zero for the root namespace
/// - `namespaces_out` must be a valid pointer to receive the array of string pointers
/// - `count_out` must be a valid pointer to receive the count
/// - `error_message` can be NULL to ignore detailed error messages
/// - The caller is responsible for freeing the returned strings and array
///
/// On entry, a non-NULL `namespaces_out` is set to NULL and a non-NULL
/// `count_out` to zero. They are published only after the complete list has been
/// converted. A non-NULL `error_message` is cleared on entry.
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_list_namespaces_path(
    connection: *const LanceDBConnection,
    namespace_path: *const *const c_char,
    namespace_path_len: usize,
    namespaces_out: *mut *mut *mut c_char,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);
    if !namespaces_out.is_null() {
        *namespaces_out = ptr::null_mut();
    }
    if !count_out.is_null() {
        *count_out = 0;
    }

    if connection.is_null() || namespaces_out.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(namespace_path) =
        namespace_path_from_c(namespace_path, namespace_path_len, error_message)
    else {
        return LanceDBError::InvalidArgument;
    };

    let result = get_runtime().block_on(
        (*connection)
            .inner
            .list_namespaces(&namespace_path, Default::default())
            .try_collect::<Vec<String>>(),
    );
    match result {
        Ok(namespaces) => {
            let count = namespaces.len();
            match c_string_array(namespaces) {
                Some(namespaces_array) => {
                    *namespaces_out = namespaces_array;
                    *count_out = count;
                    LanceDBError::Success
                }
                None => {
                    set_unknown_error_message(error_message);
                    LanceDBError::Unknown
                }
            }
        }
        Err(e) => handle_error(&e, error_message),
    }
}

/// Describe a namespace at a multi-level namespace path
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `namespace_path` must point to `namespace_path_len` valid C strings, or be NULL
///   when `namespace_path_len` is zero for the root namespace
/// - `description_out` must be a valid pointer to receive the description
/// - `error_message` can be NULL to ignore detailed error messages
///
/// On entry, a non-NULL `description_out` is set to the empty description and a
/// non-NULL `error_message` is cleared. The description is published only after
/// the complete response has been converted.
///
/// On success, free the description with `lancedb_free_namespace_description`.
///
/// # Returns
/// - Error code indicating success or failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_describe_namespace_path(
    connection: *const LanceDBConnection,
    namespace_path: *const *const c_char,
    namespace_path_len: usize,
    description_out: *mut LanceDBNamespaceDescription,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);
    if !description_out.is_null() {
        *description_out = empty_namespace_description();
    }

    if connection.is_null() || description_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(namespace_path) =
        namespace_path_from_c(namespace_path, namespace_path_len, error_message)
    else {
        return LanceDBError::InvalidArgument;
    };

    let mut request = DescribeNamespaceRequest::new();
    request.id = Some(namespace_path);
    let response = match get_runtime().block_on((*connection).inner.describe_namespace(request)) {
        Ok(response) => response,
        Err(e) => return handle_error(&e, error_message),
    };

    let (has_properties, property_keys, property_values, property_count) =
        match c_string_map(response.properties) {
            Ok(properties) => properties,
            Err(()) => {
                set_unknown_error_message(error_message);
                return LanceDBError::Unknown;
            }
        };
    let (has_context, context_keys, context_values, context_count) =
        match c_string_map(response.context) {
            Ok(context) => context,
            Err(()) => {
                free_c_string_array(property_keys, property_count);
                free_c_string_array(property_values, property_count);
                set_unknown_error_message(error_message);
                return LanceDBError::Unknown;
            }
        };

    *description_out = LanceDBNamespaceDescription {
        has_properties,
        property_keys,
        property_values,
        property_count,
        has_context,
        context_keys,
        context_values,
        context_count,
    };
    LanceDBError::Success
}

/// Free a Connection
///
/// # Safety
/// - `connection` must be a valid pointer returned from `lancedb_connect_builder_execute`
/// - `connection` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_connection_free(connection: *mut LanceDBConnection) {
    if !connection.is_null() {
        let _ = Box::from_raw(connection);
    }
}

/// Free a Table
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `table` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_free(table: *mut LanceDBTable) {
    if !table.is_null() {
        let _ = Box::from_raw(table);
    }
}

/// Run a callback on an extended stack.
///
/// If the remaining stack is below `red_zone` bytes, a new stack segment
/// of `stack_size` bytes is allocated before calling the callback.
/// Use this to wrap coroutine bodies so that inner FFI calls can reuse
/// the same stack segment instead of allocating one per call.
///
/// # Safety
/// - `callback` must be a valid function pointer
/// - `user_data` is passed through to the callback unchanged
#[no_mangle]
pub unsafe extern "C" fn lancedb_run_on_stack(
    callback: Option<extern "C" fn(*mut c_void)>,
    user_data: *mut c_void,
    red_zone: usize,
    stack_size: usize,
) {
    let callback = match callback {
        Some(f) => f,
        None => return,
    };
    let stack_size = stack_size.max(red_zone);
    stacker::maybe_grow(red_zone, stack_size, || {
        callback(user_data);
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    use std::path::PathBuf;
    use std::sync::atomic::{AtomicUsize, Ordering};

    use arrow_schema::{DataType, Field};
    use lancedb::database::OpenTableRequest;

    use crate::types::LanceDBWriteOptions;

    struct TempDir(PathBuf);

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn temp_dir() -> TempDir {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "lancedb-c-create-options-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::SeqCst)
        ));
        TempDir(path)
    }

    struct CountingReader {
        schema: Arc<Schema>,
        batch: Option<RecordBatch>,
        drops: Arc<AtomicUsize>,
    }

    impl Iterator for CountingReader {
        type Item = Result<RecordBatch, arrow_schema::ArrowError>;

        fn next(&mut self) -> Option<Self::Item> {
            self.batch.take().map(Ok)
        }
    }

    impl RecordBatchReader for CountingReader {
        fn schema(&self) -> Arc<Schema> {
            self.schema.clone()
        }
    }

    impl Drop for CountingReader {
        fn drop(&mut self) {
            self.drops.fetch_add(1, Ordering::SeqCst);
        }
    }

    fn counting_reader(
        schema: Arc<Schema>,
        drops: Arc<AtomicUsize>,
    ) -> *mut LanceDBRecordBatchReader {
        let batch = RecordBatch::new_empty(schema.clone());
        let reader = CountingReader {
            schema,
            batch: Some(batch),
            drops,
        };
        Box::into_raw(Box::new(LanceDBRecordBatchReader::new(Box::new(reader))))
    }

    fn test_connection(root: &TempDir) -> *mut LanceDBConnection {
        let uri = root.0.to_str().expect("the temporary path is UTF-8");
        let connection = get_runtime()
            .block_on(connect(uri).execute())
            .expect("the temporary database connects");
        Box::into_raw(Box::new(LanceDBConnection {
            inner: connection,
            uri_cache: OnceLock::new(),
        }))
    }

    fn test_schema() -> Arc<Schema> {
        Arc::new(Schema::new(vec![Field::new(
            "value",
            DataType::Int32,
            false,
        )]))
    }

    #[test]
    fn exist_ok_reopens_with_the_create_location_and_storage_options() {
        let mode = create_table_mode_from_c(
            2,
            Some("custom-location".to_string()),
            vec![(
                "scheduler_error_mode".to_string(),
                "best_effort".to_string(),
            )],
        )
        .expect("ExistOk is a valid create mode");
        let CreateTableMode::ExistOk(callback) = mode else {
            panic!("mode 2 must build the SDK ExistOk callback");
        };

        let request = OpenTableRequest {
            name: "table".to_string(),
            namespace_path: Vec::new(),
            index_cache_size: None,
            lance_read_params: None,
            location: None,
            namespace_client: None,
            managed_versioning: None,
        };
        let request = callback(request);

        assert_eq!(request.location.as_deref(), Some("custom-location"));
        let store_options = request
            .lance_read_params
            .expect("storage options create read parameters")
            .store_options
            .expect("storage options create object store parameters");
        assert_eq!(
            store_options
                .storage_options()
                .and_then(|options| options.get("scheduler_error_mode"))
                .map(String::as_str),
            Some("best_effort")
        );
    }

    #[test]
    fn create_options_release_the_reader_exactly_once() {
        let root = temp_dir();
        let connection = test_connection(&root);
        let schema = test_schema();
        let schema_ffi = arrow_schema::ffi::FFI_ArrowSchema::try_from(schema.as_ref())
            .expect("the test schema exports");
        let options = LanceDBCreateTableOptions::default();

        let success_name = CString::new("created").unwrap();
        let drops = Arc::new(AtomicUsize::new(0));
        let mut table = ptr::null_mut();
        let status = unsafe {
            lancedb_table_create_with_options(
                connection,
                success_name.as_ptr(),
                &schema_ffi,
                counting_reader(schema.clone(), drops.clone()),
                &options,
                &mut table,
                ptr::null_mut(),
            )
        };
        assert_eq!(status, LanceDBError::Success);
        assert_eq!(drops.load(Ordering::SeqCst), 1);
        unsafe { lancedb_table_free(table) };

        let argument_name = CString::new("argument-error").unwrap();
        let drops = Arc::new(AtomicUsize::new(0));
        let status = unsafe {
            lancedb_table_create_with_options(
                connection,
                argument_name.as_ptr(),
                &schema_ffi,
                counting_reader(schema.clone(), drops.clone()),
                &options,
                ptr::null_mut(),
                ptr::null_mut(),
            )
        };
        assert_eq!(status, LanceDBError::InvalidArgument);
        assert_eq!(drops.load(Ordering::SeqCst), 1);

        let bad_write_options = LanceDBWriteOptions {
            set_fields: 1 << 31,
            ..LanceDBWriteOptions::default()
        };
        let bad_options = LanceDBCreateTableOptions {
            write_options: &bad_write_options,
            ..LanceDBCreateTableOptions::default()
        };
        let option_name = CString::new("option-error").unwrap();
        let drops = Arc::new(AtomicUsize::new(0));
        let mut table = ptr::null_mut();
        let status = unsafe {
            lancedb_table_create_with_options(
                connection,
                option_name.as_ptr(),
                &schema_ffi,
                counting_reader(schema.clone(), drops.clone()),
                &bad_options,
                &mut table,
                ptr::null_mut(),
            )
        };
        assert_eq!(status, LanceDBError::InvalidArgument);
        assert_eq!(drops.load(Ordering::SeqCst), 1);

        let invalid_name = CString::new("invalid table name").unwrap();
        let drops = Arc::new(AtomicUsize::new(0));
        let mut table = ptr::null_mut();
        let status = unsafe {
            lancedb_table_create_with_options(
                connection,
                invalid_name.as_ptr(),
                &schema_ffi,
                counting_reader(schema.clone(), drops.clone()),
                &options,
                &mut table,
                ptr::null_mut(),
            )
        };
        assert_eq!(status, LanceDBError::InvalidTableName);
        assert_eq!(drops.load(Ordering::SeqCst), 1);

        unsafe { lancedb_connection_free(connection) };
    }

    // The SDK keeps builder options private. Debug is the observable builder
    // representation at this boundary, so inspect it directly.
    fn builder_debug(builder: *mut LanceDBConnectBuilder) -> String {
        let builder = unsafe { &*builder };
        format!("{:?}", builder.inner)
    }

    #[test]
    fn dedicated_remote_strings_stay_out_of_native_options() {
        let uri = CString::new("file:///tmp/lancedb-c-native-options").unwrap();
        let secret = CString::new("dummy-native-secret").unwrap();
        let value = CString::new("dummy-native-value").unwrap();

        let builder = unsafe { lancedb_connect(uri.as_ptr()) };
        assert!(!builder.is_null());
        let builder = unsafe { lancedb_connect_builder_api_key(builder, secret.as_ptr()) };
        assert!(!builder.is_null());
        let builder = unsafe { lancedb_connect_builder_region(builder, value.as_ptr()) };
        assert!(!builder.is_null());
        let builder = unsafe { lancedb_connect_builder_host_override(builder, value.as_ptr()) };
        assert!(!builder.is_null());
        let builder = unsafe { lancedb_connect_builder_sql_host_override(builder, value.as_ptr()) };
        assert!(!builder.is_null());

        let debug = builder_debug(builder);
        assert!(!debug.contains("remote_database_api_key"));
        assert!(!debug.contains("remote_database_region"));
        assert!(!debug.contains("remote_database_host_override"));
        assert!(!debug.contains("remote_database_sql_host_override"));
        assert!(!debug.contains(secret.to_str().unwrap()));

        unsafe { lancedb_connect_builder_free(builder) };
    }

    #[cfg(feature = "remote")]
    #[test]
    fn dedicated_remote_strings_reach_the_remote_builder() {
        let uri = CString::new("db://test").unwrap();
        let secret = CString::new("dummy-remote-secret").unwrap();
        let region = CString::new("us-east-1").unwrap();

        let builder = unsafe { lancedb_connect(uri.as_ptr()) };
        assert!(!builder.is_null());
        let builder = unsafe { lancedb_connect_builder_api_key(builder, secret.as_ptr()) };
        assert!(!builder.is_null());
        let builder = unsafe { lancedb_connect_builder_region(builder, region.as_ptr()) };
        assert!(!builder.is_null());

        let debug = builder_debug(builder);
        assert!(debug.contains("remote_database_api_key"));
        assert!(debug.contains("remote_database_region"));
        assert!(debug.contains(secret.to_str().unwrap()));

        unsafe { lancedb_connect_builder_free(builder) };
    }

    #[cfg(feature = "remote")]
    #[test]
    fn remote_classification_matches_the_sdk_db_prefix() {
        let uri = CString::new("db-not-a-uri").unwrap();
        let secret = CString::new("dummy-malformed-remote-secret").unwrap();

        let builder = unsafe { lancedb_connect(uri.as_ptr()) };
        assert!(!builder.is_null());
        let builder = unsafe { lancedb_connect_builder_api_key(builder, secret.as_ptr()) };
        assert!(!builder.is_null());

        let debug = builder_debug(builder);
        assert!(debug.contains("remote_database_api_key"));
        assert!(debug.contains(secret.to_str().unwrap()));

        unsafe { lancedb_connect_builder_free(builder) };
    }

    #[cfg(not(feature = "remote"))]
    #[test]
    fn remote_setters_validate_and_return_the_builder_without_remote_feature() {
        let uri = CString::new("db://test").unwrap();
        let secret = CString::new("dummy-no-remote-secret").unwrap();

        let builder = unsafe { lancedb_connect(uri.as_ptr()) };
        assert!(!builder.is_null());
        let builder = unsafe { lancedb_connect_builder_api_key(builder, secret.as_ptr()) };
        assert!(!builder.is_null());
        let builder = unsafe { lancedb_connect_builder_region(builder, secret.as_ptr()) };
        assert!(!builder.is_null());
        let builder = unsafe { lancedb_connect_builder_host_override(builder, secret.as_ptr()) };
        assert!(!builder.is_null());
        let builder =
            unsafe { lancedb_connect_builder_sql_host_override(builder, secret.as_ptr()) };
        assert!(!builder.is_null());

        let debug = builder_debug(builder);
        assert!(!debug.contains("remote_database_api_key"));
        assert!(!debug.contains("remote_database_region"));
        assert!(!debug.contains("remote_database_host_override"));
        assert!(!debug.contains("remote_database_sql_host_override"));
        assert!(!debug.contains(secret.to_str().unwrap()));

        unsafe { lancedb_connect_builder_free(builder) };
    }
}
