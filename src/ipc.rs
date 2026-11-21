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
    clear_error_message, set_custom_error_message, set_invalid_argument_message, LanceDBError,
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

/// Smallest possible Arrow IPC file: header magic, footer length and footer magic
const MIN_IPC_FILE_LEN: usize = 18;

/// Reject bytes that cannot hold the footer their own last ten bytes announce
///
/// The Arrow reader sizes its footer buffer from the length stored in the file,
/// so checking that the footer fits the input keeps a truncated or crafted file
/// from asking for an allocation larger than the bytes the caller passed.
fn check_ipc_file_bounds(data: &[u8]) -> Result<(), ArrowError> {
    if data.len() < MIN_IPC_FILE_LEN {
        return Err(ArrowError::ParseError(format!(
            "a file of {} bytes is too short to hold an Arrow IPC footer",
            data.len()
        )));
    }

    let footer_length_and_magic: [u8; 10] = data[data.len() - 10..]
        .try_into()
        .expect("the last ten bytes exist in a checked buffer");
    let footer_len = arrow_ipc::reader::read_footer_length(footer_length_and_magic)?;
    if footer_len > data.len() - MIN_IPC_FILE_LEN {
        return Err(ArrowError::ParseError(format!(
            "a footer of {footer_len} bytes does not fit a file of {} bytes",
            data.len()
        )));
    }

    // The Arrow reader allocates a batch's metadata and body from the lengths in
    // the footer before it looks at their contents, so every block the footer
    // declares has to stay inside the bytes the caller passed
    let footer_start = data.len() - 10 - footer_len;
    let footer = arrow_ipc::root_as_footer(&data[footer_start..data.len() - 10])
        .map_err(|error| ArrowError::ParseError(format!("unreadable IPC footer: {error}")))?;
    for block in footer.recordBatches().into_iter().flatten() {
        check_block_bounds(
            block.offset(),
            block.metaDataLength(),
            block.bodyLength(),
            data.len(),
        )?;
    }
    for block in footer.dictionaries().into_iter().flatten() {
        check_block_bounds(
            block.offset(),
            block.metaDataLength(),
            block.bodyLength(),
            data.len(),
        )?;
    }

    Ok(())
}

/// Verify that one footer-declared block stays inside the input
fn check_block_bounds(
    offset: i64,
    metadata_len: i32,
    body_len: i64,
    file_len: usize,
) -> Result<(), ArrowError> {
    let reason = |reason: &str| {
        ArrowError::ParseError(format!(
            "a block at offset {offset} with {metadata_len} metadata and {body_len} body bytes {reason}"
        ))
    };
    if offset < 0 || metadata_len < 0 || body_len < 0 {
        return Err(reason("declares a negative length"));
    }

    let end = (offset as u64)
        .checked_add(metadata_len as u64)
        .and_then(|end| end.checked_add(body_len as u64))
        .ok_or_else(|| reason("declares a range that overflows"))?;
    if end > file_len as u64 {
        return Err(reason("ends outside the file"));
    }

    Ok(())
}

/// Decode every batch of a borrowed Arrow IPC file
///
/// The reader stays behind this call: it borrows `data`, which the caller may
/// release as soon as the function returns. Decoding everything up front makes
/// a failure in any batch an input error before a table can be mutated.
pub(crate) fn read_ipc_file(data: &[u8]) -> Result<(Arc<Schema>, Vec<RecordBatch>), ArrowError> {
    check_ipc_file_bounds(data)?;
    let reader = FileReader::try_new(Cursor::new(data), None)?;
    let schema = reader.schema();
    let batches = reader.collect::<Result<Vec<RecordBatch>, ArrowError>>()?;
    Ok((schema, batches))
}

/// Run a decode under a panic barrier
///
/// The Arrow IPC decoder indexes and unwraps fields it read from the file, so a
/// malformed batch can panic instead of returning an error. Containing the panic
/// here keeps it from unwinding across the C boundary: the caller sees
/// `LANCEDB_INVALID_INPUT` like for any other unreadable file.
fn contain_decode_panic<T>(decode: impl FnOnce() -> T) -> Result<T, ()> {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(decode)).map_err(|_| ())
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

/// The C outputs of a successful decode, prepared before either is handed over
struct PreparedDecode {
    c_schema: Option<arrow_schema::ffi::FFI_ArrowSchema>,
    reader: Option<LanceDBRecordBatchReader>,
}

/// Decode the input and prepare whichever C outputs the caller asked for
///
/// Both outputs are built before the caller receives either of them, so a
/// failure cannot leave one output set and the other missing.
fn prepare_decode(
    data: &[u8],
    want_schema: bool,
    want_reader: bool,
) -> Result<PreparedDecode, ArrowError> {
    let (schema, batches) = read_ipc_file(data)?;

    let c_schema = if want_schema {
        Some(arrow_schema::ffi::FFI_ArrowSchema::try_from(&*schema)?)
    } else {
        None
    };
    let reader = if want_reader {
        Some(LanceDBRecordBatchReader::new(Box::new(
            DecodedBatchesReader {
                schema,
                batches: batches.into_iter(),
            },
        )))
    } else {
        None
    };

    Ok(PreparedDecode { c_schema, reader })
}

/// Decode an Arrow IPC file into an owned reader and/or an Arrow C ABI schema
///
/// # Safety
/// - `data` must point to `len` readable bytes that stay valid for the call. It
///   may be NULL only when `len` is 0, which is reported as empty input.
/// - `reader_out` and `schema_out` must be writable when not NULL, and at least
///   one of them must not be NULL
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` with the requested outputs set
/// - `LANCEDB_INVALID_ARGUMENT` for a NULL argument
/// - `LANCEDB_INVALID_INPUT` when the bytes are not a readable Arrow IPC file,
///   including when the input is empty, a size cannot describe a buffer, or any
///   batch is truncated or corrupt
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

    if (reader_out.is_null() && schema_out.is_null()) || (data.is_null() && len > 0) {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }
    // A NULL pointer with no bytes is the empty input described below
    if len == 0 {
        set_custom_error_message(
            error_message,
            "Invalid input: an Arrow IPC file cannot be empty",
        );
        return LanceDBError::InvalidInput;
    }
    // A length that cannot describe a Rust slice is input the decoder rejects
    // before any pointer arithmetic happens
    if len > isize::MAX as usize {
        set_custom_error_message(
            error_message,
            "Invalid input: the size is too large to be an Arrow IPC file",
        );
        return LanceDBError::InvalidInput;
    }

    let bytes = std::slice::from_raw_parts(data, len);
    // Decoding and preparing both C outputs happens inside the panic barrier:
    // everything the caller asked for is ready before the first output is set
    let prepared = match contain_decode_panic(|| {
        prepare_decode(bytes, !schema_out.is_null(), !reader_out.is_null())
    }) {
        Ok(Ok(prepared)) => prepared,
        Ok(Err(error)) => {
            set_custom_error_message(
                error_message,
                &format!("Invalid input: unusable Arrow IPC file: {error}"),
            );
            return LanceDBError::InvalidInput;
        }
        Err(()) => {
            set_custom_error_message(
                error_message,
                "Invalid input: the Arrow IPC decoder could not read this file",
            );
            return LanceDBError::InvalidInput;
        }
    };

    // Hand both outputs over together
    if let Some(c_schema) = prepared.c_schema {
        *schema_out = Box::into_raw(Box::new(c_schema));
    }
    if let Some(reader) = prepared.reader {
        *reader_out = Box::into_raw(Box::new(reader));
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

#[cfg(test)]
mod tests {
    use super::*;

    use std::pin::Pin;
    use std::task::{Context, Poll};

    use arrow_array::{Int32Array, StringArray};
    use arrow_schema::{DataType, Field};

    use crate::connection::get_runtime;
    use crate::error::error_to_error_code;

    fn test_schema() -> Arc<Schema> {
        Arc::new(Schema::new(vec![
            Field::new("key", DataType::Utf8, false),
            Field::new("value", DataType::Int32, false),
        ]))
    }

    fn test_batch(schema: Arc<Schema>, keys: &[&str], values: &[i32]) -> RecordBatch {
        RecordBatch::try_new(
            schema,
            vec![
                Arc::new(StringArray::from(keys.to_vec())),
                Arc::new(Int32Array::from(values.to_vec())),
            ],
        )
        .expect("a valid test batch")
    }

    /// A stream over prepared items, so a test can decide where an SDK error or
    /// a zero-row batch appears without depending on a real query
    struct TestStream {
        schema: Arc<Schema>,
        batches: std::vec::IntoIter<Result<RecordBatch, lancedb::error::Error>>,
    }

    impl futures::Stream for TestStream {
        type Item = Result<RecordBatch, lancedb::error::Error>;

        fn poll_next(
            mut self: Pin<&mut Self>,
            _context: &mut Context<'_>,
        ) -> Poll<Option<Self::Item>> {
            Poll::Ready(self.batches.next())
        }
    }

    impl lancedb::arrow::RecordBatchStream for TestStream {
        fn schema(&self) -> Arc<Schema> {
            self.schema.clone()
        }
    }

    fn test_stream(
        schema: Arc<Schema>,
        batches: Vec<Result<RecordBatch, lancedb::error::Error>>,
    ) -> SendableRecordBatchStream {
        Box::pin(TestStream {
            schema,
            batches: batches.into_iter(),
        })
    }

    fn write_test_file(schema: Arc<Schema>, batches: Vec<RecordBatch>) -> Vec<u8> {
        get_runtime()
            .block_on(write_ipc_file_from_stream(
                schema.clone(),
                test_stream(schema, batches.into_iter().map(Ok).collect()),
            ))
            .expect("the stream is valid")
    }

    #[test]
    fn writes_batches_in_stream_order_including_zero_row_batches() {
        let schema = test_schema();
        let bytes = write_test_file(
            schema.clone(),
            vec![
                test_batch(schema.clone(), &["a", "b"], &[1, 2]),
                test_batch(schema.clone(), &[], &[]),
                test_batch(schema.clone(), &["c"], &[3]),
            ],
        );

        let mut reader = FileReader::try_new(Cursor::new(bytes), None).expect("a readable file");
        assert_eq!(reader.schema().fields().len(), 2);
        let rows: Vec<usize> = reader
            .by_ref()
            .map(|batch| batch.expect("a readable batch").num_rows())
            .collect();
        assert_eq!(rows, vec![2, 0, 1]);
    }

    #[test]
    fn stream_error_after_a_batch_keeps_the_sdk_error() {
        let schema = test_schema();
        let outcome = get_runtime().block_on(write_ipc_file_from_stream(
            schema.clone(),
            test_stream(
                schema.clone(),
                vec![
                    Ok(test_batch(schema.clone(), &["a"], &[1])),
                    Ok(test_batch(schema, &["b"], &[2])),
                    Err(lancedb::error::Error::InvalidInput {
                        message: "synthetic stream failure".to_string(),
                    }),
                ],
            ),
        ));

        let error = outcome.expect_err("a stream error must fail the whole file");
        assert_eq!(error_to_error_code(&error), LanceDBError::InvalidInput);
        assert!(error.to_string().contains("synthetic stream failure"));
    }

    #[test]
    fn rejects_a_footer_that_does_not_fit_the_file() {
        let schema = test_schema();
        let mut bytes = write_test_file(schema.clone(), vec![test_batch(schema, &["a"], &[1])]);

        // Announce a footer one byte longer than the file can hold
        let announced = bytes.len() as i32 + 1;
        let offset = bytes.len() - 10;
        bytes[offset..offset + 4].copy_from_slice(&announced.to_le_bytes());

        let error = read_ipc_file(&bytes).expect_err("the footer cannot fit the input");
        assert!(error.to_string().contains("does not fit"));
    }

    #[test]
    fn a_schema_that_cannot_be_exported_to_c_fails_without_outputs() {
        // A field name with a NUL byte is an ordinary Arrow name, but it cannot
        // be carried by the C data interface, which needs C strings
        let schema = Arc::new(Schema::new(vec![Field::new(
            "bad\0name",
            DataType::Int32,
            false,
        )]));
        let batch = RecordBatch::try_new(schema.clone(), vec![Arc::new(Int32Array::from(vec![1]))])
            .expect("a valid test batch");
        let bytes = write_test_file(schema, vec![batch]);

        // The file itself decodes, only the C schema conversion rejects it
        assert!(read_ipc_file(&bytes).is_ok());
        let error = match prepare_decode(&bytes, true, true) {
            Ok(_) => panic!("the C ABI cannot carry a field name with a NUL byte"),
            Err(error) => error,
        };
        assert!(
            error.to_string().contains("Null byte"),
            "unexpected error: {error}"
        );

        // Through the entry point, the input is a normal error and neither
        // output is handed over
        let mut reader: *mut LanceDBRecordBatchReader = ptr::null_mut();
        let mut c_schema: *mut arrow_schema::ffi::FFI_ArrowSchema = ptr::null_mut();
        let mut message: *mut c_char = ptr::null_mut();
        let status = unsafe {
            lancedb_record_batch_reader_from_ipc_file(
                bytes.as_ptr(),
                bytes.len(),
                &mut reader,
                &mut c_schema,
                &mut message,
            )
        };
        assert_eq!(status, LanceDBError::InvalidInput);
        assert!(reader.is_null());
        assert!(c_schema.is_null());
        assert!(!message.is_null());
        unsafe { crate::error::lancedb_free_string(message) };
    }

    #[test]
    fn block_ranges_must_stay_inside_the_file() {
        // A block that ends exactly at the end of the input is fine
        assert!(check_block_bounds(8, 100, 100, 208).is_ok());
        // One byte past it is not
        assert!(check_block_bounds(8, 100, 101, 208).is_err());
        // Neither is a negative or overflowing declaration
        assert!(check_block_bounds(-1, 100, 100, 208).is_err());
        assert!(check_block_bounds(8, -1, 100, 208).is_err());
        assert!(check_block_bounds(8, 100, -1, 208).is_err());
        assert!(check_block_bounds(i64::MAX, 100, 100, 208).is_err());
        assert!(check_block_bounds(0, i32::MAX, i64::MAX, 208).is_err());
    }

    #[test]
    fn a_decoder_panic_is_contained_as_invalid_input() {
        let contained = contain_decode_panic(|| -> Result<(), ArrowError> {
            panic!("synthetic decoder panic")
        });
        assert!(contained.is_err());
    }
}
