// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Import an Arrow C Stream as a LanceDB record batch reader
//!
//! The C stream interface moves a producer's stream into the binding. The
//! binding validates the callbacks before the move, acquires the schema once,
//! and leaves the batches to be pulled lazily when a table write consumes the
//! reader. The standard Arrow structs are declared here as well as in the
//! public header so this module does not depend on Arrow's private layout.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;
use std::sync::Arc;

use arrow_array::ffi::{from_ffi_and_data_type, FFI_ArrowArray};
use arrow_array::{RecordBatch, RecordBatchOptions, RecordBatchReader, StructArray};
use arrow_schema::ffi::FFI_ArrowSchema;
use arrow_schema::{ArrowError, DataType, Schema, SchemaRef};

use crate::error::{
    clear_error_message, set_custom_error_message, set_invalid_argument_message, LanceDBError,
};
use crate::types::LanceDBRecordBatchReader;

/// ABI-compatible struct for ArrowSchema from the Arrow C Data Interface
#[repr(C)]
pub struct ArrowSchema {
    /// Arrow type description
    pub format: *const c_char,
    /// Field or array name
    pub name: *const c_char,
    /// Binary metadata
    pub metadata: *const c_char,
    /// Field flags
    pub flags: i64,
    /// Number of child schemas
    pub n_children: i64,
    /// Child schemas
    pub children: *mut *mut ArrowSchema,
    /// Dictionary value schema
    pub dictionary: *mut ArrowSchema,
    /// Producer release callback
    pub release: Option<unsafe extern "C" fn(*mut ArrowSchema)>,
    /// Producer private data
    pub private_data: *mut c_void,
}

/// ABI-compatible struct for ArrowArray from the Arrow C Data Interface
#[repr(C)]
pub struct ArrowArray {
    /// Number of elements
    pub length: i64,
    /// Number of null elements
    pub null_count: i64,
    /// Element offset
    pub offset: i64,
    /// Number of buffers
    pub n_buffers: i64,
    /// Number of child arrays
    pub n_children: i64,
    /// Data buffers
    pub buffers: *const *const c_void,
    /// Child arrays
    pub children: *mut *mut ArrowArray,
    /// Dictionary values
    pub dictionary: *mut ArrowArray,
    /// Producer release callback
    pub release: Option<unsafe extern "C" fn(*mut ArrowArray)>,
    /// Producer private data
    pub private_data: *mut c_void,
}

/// ABI-compatible struct for ArrowArrayStream from the Arrow C Stream Interface
#[repr(C)]
pub struct ArrowArrayStream {
    /// Callback to get the stream schema
    pub get_schema: Option<unsafe extern "C" fn(*mut ArrowArrayStream, *mut ArrowSchema) -> c_int>,
    /// Callback to get the next array, or an array with no release callback at EOF
    pub get_next: Option<unsafe extern "C" fn(*mut ArrowArrayStream, *mut ArrowArray) -> c_int>,
    /// Callback to get the last operation's error text, if one is available
    pub get_last_error: Option<unsafe extern "C" fn(*mut ArrowArrayStream) -> *const c_char>,
    /// Callback to release the stream's own resources
    pub release: Option<unsafe extern "C" fn(*mut ArrowArrayStream)>,
    /// Producer private data
    pub private_data: *mut c_void,
}

impl ArrowSchema {
    fn empty() -> Self {
        Self {
            format: ptr::null(),
            name: ptr::null(),
            metadata: ptr::null(),
            flags: 0,
            n_children: 0,
            children: ptr::null_mut(),
            dictionary: ptr::null_mut(),
            release: None,
            private_data: ptr::null_mut(),
        }
    }
}

impl Drop for ArrowSchema {
    fn drop(&mut self) {
        if let Some(release) = self.release {
            unsafe { release(self) };
        }
    }
}

impl ArrowArray {
    fn empty() -> Self {
        Self {
            length: 0,
            null_count: 0,
            offset: 0,
            n_buffers: 0,
            n_children: 0,
            buffers: ptr::null(),
            children: ptr::null_mut(),
            dictionary: ptr::null_mut(),
            release: None,
            private_data: ptr::null_mut(),
        }
    }

    fn is_released(&self) -> bool {
        self.release.is_none()
    }
}

impl Drop for ArrowArray {
    fn drop(&mut self) {
        if let Some(release) = self.release {
            unsafe { release(self) };
        }
    }
}

impl ArrowArrayStream {
    fn empty() -> Self {
        Self {
            get_schema: None,
            get_next: None,
            get_last_error: None,
            release: None,
            private_data: ptr::null_mut(),
        }
    }

    /// Move a validated stream out of the caller's struct storage
    ///
    /// The caller keeps ownership of the outer struct; this leaves it empty so
    /// the caller cannot release the producer a second time.
    unsafe fn take(stream: *mut Self) -> Self {
        unsafe { ptr::replace(stream, Self::empty()) }
    }

    fn last_error(&mut self) -> Option<String> {
        let get_last_error = self.get_last_error?;
        let message = unsafe { get_last_error(self) };
        if message.is_null() {
            return None;
        }
        Some(
            unsafe { CStr::from_ptr(message) }
                .to_string_lossy()
                .into_owned(),
        )
    }

    fn callback_error(&mut self, operation: &str, code: c_int) -> String {
        match self.last_error() {
            Some(message) if !message.is_empty() => {
                format!("ArrowArrayStream {operation} failed with code {code}: {message}")
            }
            _ => format!(
                "ArrowArrayStream {operation} failed with code {code} without an error message"
            ),
        }
    }
}

impl Drop for ArrowArrayStream {
    fn drop(&mut self) {
        if let Some(release) = self.release {
            unsafe { release(self) };
        }
    }
}

// The producer may pull batches and release the stream from SDK worker threads.
unsafe impl Send for ArrowArrayStream {}

const _: () = {
    assert!(std::mem::size_of::<ArrowSchema>() == std::mem::size_of::<FFI_ArrowSchema>());
    assert!(std::mem::align_of::<ArrowSchema>() == std::mem::align_of::<FFI_ArrowSchema>());
    assert!(std::mem::size_of::<ArrowArray>() == std::mem::size_of::<FFI_ArrowArray>());
    assert!(std::mem::align_of::<ArrowArray>() == std::mem::align_of::<FFI_ArrowArray>());
};

/// Transfer schema ownership to arrow-rs
///
/// # Safety
/// The two structs have identical `repr(C)` layouts, checked above.
fn into_ffi_schema(schema: ArrowSchema) -> FFI_ArrowSchema {
    unsafe { std::mem::transmute::<ArrowSchema, FFI_ArrowSchema>(schema) }
}

/// Transfer array ownership to arrow-rs
///
/// # Safety
/// The two structs have identical `repr(C)` layouts, checked above.
fn into_ffi_array(array: ArrowArray) -> FFI_ArrowArray {
    unsafe { std::mem::transmute::<ArrowArray, FFI_ArrowArray>(array) }
}

/// A failure while acquiring and importing the stream schema
struct StreamImportError {
    code: LanceDBError,
    message: String,
}

/// Read the producer schema once, before the reader is returned
fn read_imported_schema(stream: &mut ArrowArrayStream) -> Result<SchemaRef, StreamImportError> {
    let get_schema = stream.get_schema.ok_or_else(|| StreamImportError {
        code: LanceDBError::InvalidArgument,
        message: "ArrowArrayStream is missing its get_schema callback".to_string(),
    })?;

    let mut c_schema = ArrowSchema::empty();
    let code = unsafe { get_schema(stream, &mut c_schema) };
    if code != 0 {
        return Err(StreamImportError {
            code: LanceDBError::InvalidInput,
            message: stream.callback_error("get_schema", code),
        });
    }

    if c_schema.release.is_none() {
        return Err(StreamImportError {
            code: LanceDBError::Arrow,
            message:
                "ArrowArrayStream get_schema succeeded without an ArrowSchema release callback"
                    .to_string(),
        });
    }

    let ffi_schema = into_ffi_schema(c_schema);
    Schema::try_from(&ffi_schema)
        .map(Arc::new)
        .map_err(|error| StreamImportError {
            code: LanceDBError::Arrow,
            message: format!("could not import the Arrow schema from the stream: {error}"),
        })
}

/// A record batch reader that pulls Arrow arrays from a C stream on demand
struct ImportedArrowStreamReader {
    stream: ArrowArrayStream,
    schema: SchemaRef,
}

impl Iterator for ImportedArrowStreamReader {
    type Item = Result<RecordBatch, ArrowError>;

    fn next(&mut self) -> Option<Self::Item> {
        let get_next = self.stream.get_next?;
        let mut c_array = ArrowArray::empty();
        let code = unsafe { get_next(&mut self.stream, &mut c_array) };

        if code != 0 {
            return Some(Err(ArrowError::CDataInterface(
                self.stream.callback_error("get_next", code),
            )));
        }

        // A successful get_next with no release callback marks the end of the
        // stream. A zero-row array still carries a release callback.
        if c_array.is_released() {
            return None;
        }

        let data_type = DataType::Struct(self.schema.fields().clone());
        let array = into_ffi_array(c_array);
        let data = match unsafe { from_ffi_and_data_type(array, data_type) } {
            Ok(data) => data,
            Err(error) => {
                return Some(Err(ArrowError::CDataInterface(format!(
                    "could not import an Arrow array from the stream: {error}"
                ))))
            }
        };

        // A RecordBatch represents rows only through its child columns, so a
        // top-level struct with actual null rows cannot be represented. Check
        // the parsed root before extracting the children; dropping `data`
        // releases the imported ArrowArray on this error path.
        let root_null_count = data.null_count();
        if root_null_count != 0 {
            return Some(Err(ArrowError::InvalidArgumentError(format!(
                "the imported Arrow struct has {root_null_count} null rows, which a RecordBatch cannot represent"
            ))));
        }

        let row_count = data.len();
        let batch = RecordBatch::try_new_with_options(
            self.schema.clone(),
            StructArray::from(data).into_parts().1,
            &RecordBatchOptions::new().with_row_count(Some(row_count)),
        );
        match batch {
            Ok(batch) => Some(Ok(batch)),
            Err(error) => Some(Err(ArrowError::CDataInterface(format!(
                "could not build a record batch from the imported Arrow array: {error}"
            )))),
        }
    }
}

impl RecordBatchReader for ImportedArrowStreamReader {
    fn schema(&self) -> SchemaRef {
        self.schema.clone()
    }
}

/// Create a RecordBatchReader from an Arrow C Stream
///
/// # Safety
/// - `stream` must point to a readable and writable `ArrowArrayStream`
/// - `reader_out` must be a writable pointer that receives the reader
/// - `error_message` can be NULL to ignore detailed error messages
///
/// The required callbacks and output pointers are checked before the stream is
/// moved. If those checks fail, the stream still belongs to the caller. Once
/// the stream is moved, this function releases it exactly once, including when
/// the schema cannot be acquired. The caller keeps ownership of the outer
/// struct's storage.
#[no_mangle]
pub unsafe extern "C" fn lancedb_record_batch_reader_from_arrow_stream(
    stream: *mut ArrowArrayStream,
    reader_out: *mut *mut LanceDBRecordBatchReader,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !reader_out.is_null() {
        *reader_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if stream.is_null() || reader_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    if (*stream).get_schema.is_none() || (*stream).get_next.is_none() || (*stream).release.is_none()
    {
        set_custom_error_message(
            error_message,
            "ArrowArrayStream is missing a required get_schema, get_next or release callback",
        );
        return LanceDBError::InvalidArgument;
    }

    // Only move after every required callback is known to be present. The
    // source struct is left empty so its release callback cannot run twice.
    let mut owned_stream = unsafe { ArrowArrayStream::take(stream) };
    let schema = match read_imported_schema(&mut owned_stream) {
        Ok(schema) => schema,
        Err(error) => {
            // owned_stream is released here, after the failed schema callback.
            set_custom_error_message(error_message, &error.message);
            return error.code;
        }
    };

    let reader = ImportedArrowStreamReader {
        stream: owned_stream,
        schema,
    };
    let wrapper = Box::new(LanceDBRecordBatchReader::new(Box::new(reader)));
    *reader_out = Box::into_raw(wrapper);
    LanceDBError::Success
}
