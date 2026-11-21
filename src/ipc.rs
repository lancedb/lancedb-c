// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Arrow IPC file transport across the C boundary
//!
//! Consumers that cannot use the Arrow C Data interface still need every batch
//! and the exact projected schema. Both cross this boundary as Arrow IPC files:
//! an input file is only borrowed for the duration of one call and is decoded
//! into Rust-owned batches, an output file is Rust-owned bytes released with
//! `lancedb_ipc_buffer_free()`. The calling entry point decides when a request is
//! valid; this module only converts representation and ownership.

use std::io::Cursor;
use std::os::raw::c_char;
use std::ptr;
use std::sync::Arc;

use arrow_array::{RecordBatch, RecordBatchReader};
use arrow_ipc::reader::FileReader;
use arrow_ipc::writer::FileWriter;
use arrow_schema::{ArrowError, Schema};
use futures::TryStreamExt;
use lancedb::arrow::SendableRecordBatchStream;

use crate::error::{
    clear_error_message, handle_error, set_custom_error_message, set_invalid_argument_message,
    LanceDBError,
};
use crate::types::LanceDBRecordBatchReader;

/// Bytes of an Arrow IPC file owned by LanceDB
#[repr(C)]
pub struct LanceDBIpcBuffer {
    /// Start of the file bytes, NULL when no file is held
    pub data: *const u8,
    /// Length of the file bytes
    pub len: usize,
}

impl LanceDBIpcBuffer {
    /// A buffer that holds no file
    pub(crate) const EMPTY: LanceDBIpcBuffer = LanceDBIpcBuffer {
        data: ptr::null(),
        len: 0,
    };

    /// Take ownership of a complete IPC file
    pub(crate) fn from_file_bytes(bytes: Vec<u8>) -> Self {
        let boxed = bytes.into_boxed_slice();
        let len = boxed.len();
        Self {
            data: Box::into_raw(boxed) as *const u8,
            len,
        }
    }
}

/// Free the bytes of an Arrow IPC file and reset the buffer
///
/// # Safety
/// - `buffer` must be NULL or point to a writable `LanceDBIpcBuffer`
/// - `buffer` must either hold bytes from an Arrow IPC entry point or be empty
///
/// The two fields are read as they are, so the caller may keep them unchanged
/// after the call. Freeing an already freed or empty buffer is a no-op.
#[no_mangle]
pub unsafe extern "C" fn lancedb_ipc_buffer_free(buffer: *mut LanceDBIpcBuffer) {
    if buffer.is_null() {
        return;
    }

    let data = (*buffer).data;
    let len = (*buffer).len;
    if !data.is_null() {
        drop(Box::from_raw(ptr::slice_from_raw_parts_mut(
            data as *mut u8,
            len,
        )));
    }

    (*buffer).data = ptr::null();
    (*buffer).len = 0;
}

/// Decode every batch of a borrowed Arrow IPC file
///
/// The reader stays behind this call: it borrows `data`, which the caller may
/// release as soon as the function returns. Decoding everything up front makes
/// a failure in any batch an input error before a table can be mutated.
pub(crate) fn read_ipc_file(data: &[u8]) -> Result<(Arc<Schema>, Vec<RecordBatch>), ArrowError> {
    let reader = FileReader::try_new(Cursor::new(data), None)?;
    let schema = reader.schema();
    let batches = reader.collect::<Result<Vec<RecordBatch>, ArrowError>>()?;
    Ok((schema, batches))
}

/// A reader over the batches decoded from an Arrow IPC file
struct DecodedBatchesReader {
    schema: Arc<Schema>,
    batches: std::vec::IntoIter<RecordBatch>,
}

impl Iterator for DecodedBatchesReader {
    type Item = Result<RecordBatch, ArrowError>;

    fn next(&mut self) -> Option<Self::Item> {
        self.batches.next().map(Ok)
    }
}

impl RecordBatchReader for DecodedBatchesReader {
    fn schema(&self) -> Arc<Schema> {
        self.schema.clone()
    }
}

/// Decode an Arrow IPC file into an owned reader and/or an Arrow C ABI schema
///
/// # Safety
/// - `data` must point to `len` readable bytes that stay valid for the call
/// - `reader_out` and `schema_out` must be writable when not NULL, and at least
///   one of them must not be NULL
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with the requested outputs set
/// - `LANCEDB_INVALID_ARGUMENT` for a NULL argument
/// - `LANCEDB_INVALID_INPUT` when the bytes are not a readable Arrow IPC file,
///   including when any batch is truncated or corrupt
///
/// Every batch is decoded before this function returns, so the caller may
/// release `data` immediately. A returned reader owns all batches and is freed
/// with `lancedb_record_batch_reader_free()`. A returned schema is freed with
/// `lancedb_free_arrow_schema()`. On entry, both outputs and the error message
/// are cleared; the caller must free a returned error message with
/// `lancedb_free_string()`.
#[no_mangle]
pub unsafe extern "C" fn lancedb_record_batch_reader_from_ipc_file(
    data: *const u8,
    len: usize,
    reader_out: *mut *mut LanceDBRecordBatchReader,
    schema_out: *mut *mut arrow_schema::ffi::FFI_ArrowSchema,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !reader_out.is_null() {
        *reader_out = ptr::null_mut();
    }
    if !schema_out.is_null() {
        *schema_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if data.is_null() || (reader_out.is_null() && schema_out.is_null()) {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }
    if len == 0 {
        set_custom_error_message(
            error_message,
            "Invalid input: an Arrow IPC file cannot be empty",
        );
        return LanceDBError::InvalidInput;
    }

    let bytes = std::slice::from_raw_parts(data, len);
    let (schema, batches) = match read_ipc_file(bytes) {
        Ok(decoded) => decoded,
        Err(error) => {
            set_custom_error_message(
                error_message,
                &format!("Invalid input: failed to read the Arrow IPC file: {error}"),
            );
            return LanceDBError::InvalidInput;
        }
    };

    // The schema is exported first, so a schema the C ABI cannot represent fails
    // the call before any reader takes ownership of the batches
    if !schema_out.is_null() {
        match arrow_schema::ffi::FFI_ArrowSchema::try_from(&*schema) {
            Ok(ffi_schema) => {
                *schema_out = Box::into_raw(Box::new(ffi_schema));
            }
            Err(error) => {
                return handle_error(
                    &lancedb::error::Error::Arrow { source: error },
                    error_message,
                )
            }
        }
    }

    if !reader_out.is_null() {
        let reader = DecodedBatchesReader {
            schema,
            batches: batches.into_iter(),
        };
        *reader_out = Box::into_raw(Box::new(LanceDBRecordBatchReader::new(Box::new(reader))));
    }

    LanceDBError::Success
}

/// Write an Arrow IPC file with `schema` and every batch of `stream`
///
/// The bytes are returned only after the file footer is written, so a stream
/// error after any batch or an encoding error yields no file at all. Batches are
/// written in the order the stream yields them, zero-row batches included.
pub(crate) async fn write_ipc_file_from_stream(
    schema: Arc<Schema>,
    mut stream: SendableRecordBatchStream,
) -> Result<Vec<u8>, lancedb::error::Error> {
    let mut writer = FileWriter::try_new(Vec::<u8>::new(), &schema)?;
    while let Some(batch) = stream.try_next().await? {
        writer.write(&batch)?;
    }
    writer.finish()?;
    let file = writer.into_inner()?;
    Ok(file)
}

/// Write an Arrow IPC file that carries a schema and no batches
pub(crate) fn write_ipc_file_from_schema(schema: &Schema) -> Result<Vec<u8>, ArrowError> {
    let mut writer = FileWriter::try_new(Vec::<u8>::new(), schema)?;
    writer.finish()?;
    writer.into_inner()
}
