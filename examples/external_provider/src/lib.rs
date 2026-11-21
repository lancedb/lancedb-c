// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! External ObjectStoreProvider example for lancedb-c registry.
//!
//! Demonstrates how to create a custom `LanceDBObjectStoreProvider` that can
//! be inserted into an `LanceDBObjectStoreRegistry` via
//! `lancedb_registry_insert_provider()`.
//!
//! For this example, the provider is initialized with `MemoryStoreProvider`
//! (an in-memory object store). In practice, this could be replaced with
//! any `ObjectStoreProvider` implementation (e.g., S3, GCS, Azure, or a
//! fully custom backend).

use std::sync::Arc;

use lance_io::object_store::providers::memory::MemoryStoreProvider;
use lance_io::object_store::ObjectStoreProvider;
use lancedb::LanceDBObjectStoreProvider;

/// Create an external ObjectStoreProvider for use with `lancedb_registry_insert_provider()`
///
/// For this example, the provider uses `MemoryStoreProvider` (in-memory storage).
/// Replace with any `ObjectStoreProvider` implementation for other backends.
///
/// # Returns
/// - Non-null pointer to LanceDBObjectStoreProvider on success
#[no_mangle]
pub extern "C" fn external_object_store_provider_create() -> *mut LanceDBObjectStoreProvider {
    let provider: Arc<dyn ObjectStoreProvider> = Arc::new(MemoryStoreProvider);
    Box::into_raw(Box::new(LanceDBObjectStoreProvider {
        inner: Some(provider),
    }))
}
