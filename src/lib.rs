// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! C FFI bindings for LanceDB
//!
//! This crate provides a C-compatible API for LanceDB, allowing integration
//! with C and C++ applications. The API follows standard C conventions with
//! opaque handles, explicit error codes, and manual memory management.

pub mod arrow_stream;
pub mod branch_diff;
pub mod connection;
pub mod delta;
pub mod error;
pub mod expr;
pub mod index;
pub mod ipc;
pub mod json;
pub mod optimize;
pub mod query;
pub mod table;
pub mod take;
pub mod types;
pub mod version;

// Re-export all public FFI functions
pub use arrow_stream::*;
pub use branch_diff::*;
pub use connection::*;
pub use delta::*;
pub use error::*;
pub use expr::*;
pub use index::*;
pub use ipc::*;
pub use json::*;
pub use optimize::*;
pub use query::*;
pub use table::*;
pub use take::*;
pub use types::*;
pub use version::*;
