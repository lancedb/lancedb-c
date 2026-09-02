// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Connection-related FFI functions for LanceDB C bindings

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::ptr;
use std::sync::Arc;
use std::sync::OnceLock;

use arrow_array::{RecordBatch, RecordBatchIterator, RecordBatchReader};
use arrow_schema::Schema;
use lance_namespace::models::{
    CreateNamespaceRequest, DropNamespaceRequest, ListNamespacesRequest,
};
use lancedb::connection::{connect, ConnectBuilder, Connection, TableNamesBuilder};
use lancedb::utils::validate_table_name;
use lancedb::Table;

use crate::error::{
    handle_error, set_custom_error_message, set_invalid_argument_message,
    set_unknown_error_message, LanceDBError,
};
use crate::types::LanceDBRecordBatchReader;

/// Opaque handle to a ConnectBuilder
#[repr(C)]
pub struct LanceDBConnectBuilder {
    inner: Box<ConnectBuilder>,
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

/// Cache statistics for a session cache
#[repr(C)]
pub struct LanceDBSessionCacheStats {
    pub hits: u64,
    pub misses: u64,
    pub num_entries: usize,
    pub size_bytes: usize,
}

const DEFAULT_INDEX_CACHE_SIZE_BYTES: usize = 6 * 1024 * 1024 * 1024;
const DEFAULT_METADATA_CACHE_SIZE_BYTES: usize = 1024 * 1024 * 1024;

/// Runtime to handle async operations (see `crate::runtime`).
use crate::runtime::{run_blocking, run_blocking_infallible};

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
    let boxed_builder = Box::new(LanceDBConnectBuilder {
        inner: Box::new(builder),
    });

    Box::into_raw(boxed_builder)
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

    match run_blocking(connect_builder.execute()) {
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
    if builder.is_null() {
        return ptr::null_mut();
    }

    let builder_box = Box::from_raw(builder);

    if key.is_null() || value.is_null() {
        return ptr::null_mut();
    }

    let Ok(key_str) = CStr::from_ptr(key).to_str() else {
        return ptr::null_mut();
    };

    let Ok(value_str) = CStr::from_ptr(value).to_str() else {
        return ptr::null_mut();
    };

    let connect_builder = *builder_box.inner;
    let updated_builder = connect_builder.storage_option(key_str, value_str);
    let boxed_builder = Box::new(LanceDBConnectBuilder {
        inner: Box::new(updated_builder),
    });

    Box::into_raw(boxed_builder)
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
    let connect_builder = *builder_box.inner;

    let updated_builder = if session.is_null() {
        connect_builder
    } else {
        let session_ref = &*session;
        connect_builder.session(session_ref.inner.clone())
    };

    let boxed_builder = Box::new(LanceDBConnectBuilder {
        inner: Box::new(updated_builder),
    });

    Box::into_raw(boxed_builder)
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

    let session_inner = (*session).inner.clone();
    let stats =
        match run_blocking_infallible(async move { session_inner.index_cache_stats().await }) {
            Ok(stats) => stats,
            Err(e) => return handle_error(&e, error_message),
        };
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

    let session_inner = (*session).inner.clone();
    let stats =
        match run_blocking_infallible(async move { session_inner.metadata_cache_stats().await }) {
            Ok(stats) => stats,
            Err(e) => return handle_error(&e, error_message),
        };
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

    let conn = (*connection).inner.clone();
    let table_name_owned = table_name_str.to_string();

    // Import schema from Arrow C ABI (synchronously: raw pointers must not
    // cross onto the runtime's worker threads)
    let schema = match Schema::try_from(&*schema_ptr) {
        Ok(s) => Arc::new(s),
        Err(_) => {
            let e = lancedb::error::Error::InvalidInput {
                message: "Failed to import Arrow schema from C ABI".to_string(),
            };
            return handle_error(&e, error_message);
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

    match run_blocking(async move {
        // lancedb panics on an invalid table name instead of returning an error,
        // so the name is validated before the table is created
        validate_table_name(&table_name_owned)?;

        conn.create_table(table_name_owned, batch_reader)
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

    let conn = (*connection).inner.clone();

    match run_blocking(async move { conn.table_names().execute().await }) {
        Ok(names) => {
            let count = names.len();
            *count_out = count;

            if count == 0 {
                *names_out = ptr::null_mut();
                return LanceDBError::Success;
            }

            // Allocate array of string pointers
            let names_array =
                libc::malloc(count * std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
            if names_array.is_null() {
                set_unknown_error_message(error_message);
                return LanceDBError::Unknown;
            }

            // Convert each string and store pointer
            for (i, name) in names.into_iter().enumerate() {
                match CString::new(name) {
                    Ok(c_str) => {
                        *names_array.add(i) = c_str.into_raw();
                    }
                    Err(_) => {
                        // Clean up already allocated strings
                        for j in 0..i {
                            let _ = CString::from_raw(*names_array.add(j));
                        }
                        libc::free(names_array as *mut libc::c_void);
                        set_unknown_error_message(error_message);
                        return LanceDBError::Unknown;
                    }
                }
            }

            *names_out = names_array;
            LanceDBError::Success
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
    if !names.is_null() {
        for i in 0..count {
            let name_ptr = *names.add(i);
            if !name_ptr.is_null() {
                let _ = CString::from_raw(name_ptr);
            }
        }
        libc::free(names as *mut libc::c_void);
    }
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

    match run_blocking(table_names_builder.execute()) {
        Ok(names) => {
            let count = names.len();
            *count_out = count;

            if count == 0 {
                *names_out = ptr::null_mut();
                return LanceDBError::Success;
            }

            // Allocate array of string pointers
            let names_array =
                libc::malloc(count * std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
            if names_array.is_null() {
                set_unknown_error_message(error_message);
                return LanceDBError::Unknown;
            }

            // Convert each string and store pointer
            for (i, name) in names.into_iter().enumerate() {
                match CString::new(name) {
                    Ok(c_str) => {
                        *names_array.add(i) = c_str.into_raw();
                    }
                    Err(_) => {
                        // Clean up already allocated strings
                        for j in 0..i {
                            let _ = CString::from_raw(*names_array.add(j));
                        }
                        libc::free(names_array as *mut libc::c_void);
                        set_unknown_error_message(error_message);
                        return LanceDBError::Unknown;
                    }
                }
            }

            *names_out = names_array;
            LanceDBError::Success
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
    if !namespaces.is_null() {
        for i in 0..count {
            let namespace_ptr = *namespaces.add(i);
            if !namespace_ptr.is_null() {
                let _ = CString::from_raw(namespace_ptr);
            }
        }
        libc::free(namespaces as *mut libc::c_void);
    }
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

    let conn = (*connection).inner.clone();
    let table_name_owned = table_name_str.to_string();

    match run_blocking(async move {
        // lancedb panics on an invalid table name instead of returning an error,
        // so the name is validated before the table is opened
        validate_table_name(&table_name_owned)?;

        conn.open_table(table_name_owned).execute().await
    }) {
        Ok(table) => {
            let boxed_table = Box::new(LanceDBTable { inner: table });
            *table_out = Box::into_raw(boxed_table);
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
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

    let conn = (*connection).inner.clone();
    let table_name_owned = table_name_str.to_string();

    let namespace_vec: Vec<String> = if namespace.is_null() {
        Vec::new()
    } else {
        let Ok(namespace_str) = CStr::from_ptr(namespace).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };
        vec![String::from(namespace_str)]
    };
    let result =
        run_blocking(async move { conn.drop_table(&table_name_owned, &namespace_vec).await });

    match result {
        Ok(_) => LanceDBError::Success,
        Err(e) => handle_error(&e, error_message),
    }
}

/// Rename a table in the database
///
/// # Safety
/// - `connection` must be a valid pointer
/// - `old_name` and `new_name` must be valid null-terminated C strings
/// - `cur_namespace` and `new_namespace` can be NULL for no namespace, or valid null-terminated C strings
/// - `error_message` can be NULL to ignore detailed error messages
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

    let cur_namespace_vec = if cur_namespace.is_null() {
        Vec::new()
    } else {
        let Ok(cur_namespace_str) = CStr::from_ptr(cur_namespace).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };
        vec![String::from(cur_namespace_str)]
    };

    let new_namespace_vec = if new_namespace.is_null() {
        Vec::new()
    } else {
        let Ok(new_namespace_str) = CStr::from_ptr(new_namespace).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };
        vec![String::from(new_namespace_str)]
    };

    let conn = conn.clone();
    let old_name_owned = old_name_str.to_string();
    let new_name_owned = new_name_str.to_string();
    match run_blocking(async move {
        conn.rename_table(
            &old_name_owned,
            &new_name_owned,
            &cur_namespace_vec,
            &new_namespace_vec,
        )
        .await
    }) {
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

    let conn = conn.clone();
    let namespace_vec: Vec<String> = if namespace.is_null() {
        Vec::new()
    } else {
        let Ok(namespace_str) = CStr::from_ptr(namespace).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };
        vec![String::from(namespace_str)]
    };
    let result = run_blocking(async move { conn.drop_all_tables(&namespace_vec).await });

    match result {
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

    let Ok(namespace_str) = CStr::from_ptr(namespace_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let conn = &(*connection).inner;

    let mut request = CreateNamespaceRequest::new();
    request.id = Some(vec![namespace_str.to_string()]);
    let conn = conn.clone();
    match run_blocking(async move { conn.create_namespace(request).await }) {
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

    let Ok(namespace_str) = CStr::from_ptr(namespace_name).to_str() else {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    };

    let conn = &(*connection).inner;

    let mut request = DropNamespaceRequest::new();
    request.id = Some(vec![namespace_str.to_string()]);
    let conn = conn.clone();
    match run_blocking(async move { conn.drop_namespace(request).await }) {
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

    let parent_namespace = if namespace_parent.is_null() {
        Vec::new() // Root namespace
    } else {
        let Ok(parent_str) = CStr::from_ptr(namespace_parent).to_str() else {
            set_invalid_argument_message(error_message);
            return LanceDBError::InvalidArgument;
        };
        vec![parent_str.to_string()]
    };

    let conn = &(*connection).inner;

    let mut request = ListNamespacesRequest::new();
    request.id = Some(parent_namespace);
    let conn = conn.clone();
    match run_blocking(async move { conn.list_namespaces(request).await }) {
        Ok(response) => {
            let namespaces = response.namespaces;
            let count = namespaces.len();
            *count_out = count;

            if count == 0 {
                *namespaces_out = ptr::null_mut();
                return LanceDBError::Success;
            }

            // Allocate array of string pointers
            let namespaces_array =
                libc::malloc(count * std::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
            if namespaces_array.is_null() {
                set_unknown_error_message(error_message);
                return LanceDBError::Unknown;
            }

            // Convert each string and store pointer
            for (i, namespace) in namespaces.into_iter().enumerate() {
                match CString::new(namespace) {
                    Ok(c_str) => {
                        *namespaces_array.add(i) = c_str.into_raw();
                    }
                    Err(_) => {
                        // Clean up already allocated strings
                        for j in 0..i {
                            let _ = CString::from_raw(*namespaces_array.add(j));
                        }
                        libc::free(namespaces_array as *mut libc::c_void);
                        set_unknown_error_message(error_message);
                        return LanceDBError::Unknown;
                    }
                }
            }

            *namespaces_out = namespaces_array;
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
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
