// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Lazy blob-file handles for the C API.
//!
//! `Table::fetch_blob_files` opens one seekable handle per row without
//! materializing a blob. External blobs without a stored size may be sized
//! during fetch. Each handle owns its SDK file and cursor independently of the
//! table handle and of other handles for the same value. No retention lease is
//! acquired for a handle, so later cleanup or external changes can invalidate
//! it.

use std::os::raw::c_char;
use std::ptr;

use arrow_array::ffi::FFI_ArrowArray;
use arrow_array::{
    Array, ArrowPrimitiveType, BooleanArray, PrimitiveArray, StringArray, StructArray, UInt32Array,
    UInt64Array, UInt8Array,
};
use arrow_schema::ffi::FFI_ArrowSchema;
use arrow_schema::{DataType, Field, Fields, Schema};
use lancedb::blob::{BlobDescriptor, BlobDescriptorSource, BlobFile};

use crate::arrow_ffi::import_descriptor_struct_array;
use crate::connection::{get_runtime, LanceDBTable};
use crate::error::{
    clear_error_message, handle_blob_file_error, handle_error, set_custom_error_message,
    set_invalid_argument_message, set_unknown_error_message, LanceDBError,
};
use crate::table::{borrowed_slice, required_c_string};

/// Opaque handle to one lazy blob file
pub struct LanceDBBlobFile {
    inner: BlobFile,
}

/// Bytes of one blob read owned by LanceDB
#[repr(C)]
pub struct LanceDBBlobBuffer {
    /// Start of the blob bytes, NULL when the read returned no bytes
    pub data: *const u8,
    /// Number of bytes in the read
    pub len: usize,
}

impl LanceDBBlobBuffer {
    /// A buffer that holds no bytes
    const EMPTY: LanceDBBlobBuffer = LanceDBBlobBuffer {
        data: ptr::null(),
        len: 0,
    };
}

/// Physical location of one blob inside its data file
#[repr(C)]
pub struct LanceDBBlobFilePosition {
    /// Non-zero when the handle exposes a physical byte offset
    pub has_position: bool,
    /// Physical byte offset inside the data file when `has_position` is non-zero
    pub position: u64,
}

/// Copy one completed read into caller-owned storage.
///
/// The SDK may return a slice of a prefetched buffer, so the bytes are copied
/// once to give the C caller storage it can free independently of the handle.
unsafe fn publish_blob_buffer(bytes: &[u8], buffer_out: *mut LanceDBBlobBuffer) {
    if bytes.is_empty() {
        *buffer_out = LanceDBBlobBuffer::EMPTY;
        return;
    }

    let bytes = bytes.to_vec().into_boxed_slice();
    let len = bytes.len();
    *buffer_out = LanceDBBlobBuffer {
        data: Box::into_raw(bytes) as *const u8,
        len,
    };
}

/// Copy fetched handles into a malloc'd array of optional handles.
///
/// A NULL slot represents a NULL blob. The array and every non-NULL handle are
/// owned by the caller and released together with `lancedb_free_blob_files`.
unsafe fn publish_blob_files(
    files: Vec<Option<BlobFile>>,
    files_out: *mut *mut *mut LanceDBBlobFile,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    let count = files.len();
    if count == 0 {
        return LanceDBError::Success;
    }

    let array = libc::malloc(count * std::mem::size_of::<*mut LanceDBBlobFile>())
        as *mut *mut LanceDBBlobFile;
    if array.is_null() {
        set_unknown_error_message(error_message);
        return LanceDBError::Unknown;
    }

    for (index, file) in files.into_iter().enumerate() {
        let handle = file.map(|file| Box::into_raw(Box::new(LanceDBBlobFile { inner: file })));
        *array.add(index) = handle.unwrap_or(ptr::null_mut());
    }

    *files_out = array;
    *count_out = count;
    LanceDBError::Success
}

/// Open lazy blob handles for borrowed row ids.
///
/// The result follows `row_ids` in length, order and duplicates. A NULL blob
/// yields a NULL handle; a valid empty blob yields a non-NULL handle whose size
/// is zero. An empty input succeeds without opening handles. One missing or
/// deleted row id rejects the whole request without partial handles.
///
/// Fetching opens handles without materializing a blob, but it is not always
/// free of I/O. An External blob stored without a size is sized during fetch:
/// embedded tables query the object size, while remote tables may issue a
/// byte-range size probe. Access and response errors can therefore fail the
/// fetch before any handle is returned.
///
/// Each handle keeps its own cursor and buffers and does not depend on the table
/// handle; it stays usable after the table is freed. An embedded handle captures
/// the SDK's selected storage and offset, and a remote handle captures the
/// selected version and branch. No `BlobFile` retention lease is acquired for a
/// handle, so dataset cleanup can remove old data and external storage controls
/// its content. A remote row-ID handle also carries no descriptor source-commit
/// identity, so it does not detect same-name recreation; reads can fail after
/// any of these changes.
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `column` must be a valid NUL-terminated UTF-8 string
/// - `row_ids` is borrowed for this call; it may be NULL only when `num_row_ids` is 0
/// - `files_out` must be a valid pointer to receive the optional handle array
/// - `count_out` must be a valid pointer to receive the handle count
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
/// - The caller frees the handles and array with `lancedb_free_blob_files`
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_fetch_blob_files(
    table: *const LanceDBTable,
    column: *const c_char,
    row_ids: *const u64,
    num_row_ids: usize,
    files_out: *mut *mut *mut LanceDBBlobFile,
    count_out: *mut usize,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !files_out.is_null() {
        *files_out = ptr::null_mut();
    }
    if !count_out.is_null() {
        *count_out = 0;
    }
    clear_error_message(error_message);

    if table.is_null() || files_out.is_null() || count_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }
    let Some(column) = required_c_string(column, error_message) else {
        return LanceDBError::InvalidArgument;
    };
    let Some(row_ids) = borrowed_slice(row_ids, num_row_ids, error_message) else {
        return LanceDBError::InvalidArgument;
    };

    let tbl = &(*table).inner;
    match get_runtime().block_on(tbl.fetch_blob_files(column, row_ids)) {
        Ok(files) => publish_blob_files(files, files_out, count_out, error_message),
        Err(error) => handle_error(&error, error_message),
    }
}

/// Free every handle in an array returned by `lancedb_table_fetch_blob_files`.
///
/// # Safety
/// - `files` must be NULL or the array returned by `lancedb_table_fetch_blob_files`
/// - `count` must match the count returned by that call
/// - Every handle in the array must still be owned by the caller, i.e. a handle
///   freed earlier with `lancedb_blob_file_free` must have been set to NULL
#[no_mangle]
pub unsafe extern "C" fn lancedb_free_blob_files(files: *mut *mut LanceDBBlobFile, count: usize) {
    if files.is_null() {
        return;
    }

    for index in 0..count {
        let file = *files.add(index);
        if !file.is_null() {
            let _ = Box::from_raw(file);
        }
    }
    libc::free(files as *mut libc::c_void);
}

/// Descriptor parsing failure with the C error code it maps to.
struct DescriptorError {
    code: LanceDBError,
    message: String,
}

impl DescriptorError {
    fn invalid(message: impl Into<String>) -> Self {
        Self {
            code: LanceDBError::InvalidArgument,
            message: message.into(),
        }
    }

    fn arrow(message: impl Into<String>) -> Self {
        Self {
            code: LanceDBError::Arrow,
            message: message.into(),
        }
    }
}

/// Require one field by name and exact Arrow type.
fn require_field<'a>(
    fields: &'a Fields,
    scope: &str,
    name: &str,
    expected: &DataType,
) -> Result<&'a Field, DescriptorError> {
    let field = fields
        .iter()
        .find(|field| field.name() == name)
        .ok_or_else(|| {
            DescriptorError::invalid(format!(
                "Descriptor schema is missing required field '{scope}{name}'"
            ))
        })?;

    if field.data_type() != expected {
        return Err(DescriptorError::invalid(format!(
            "Descriptor field '{scope}{name}' must have type {expected}, got {}",
            field.data_type()
        )));
    }

    Ok(field.as_ref())
}

/// Validate the descriptor struct shape before reading any row value.
///
/// Fields are identified by name, so a consumer may reorder them or carry extra
/// fields. The required fields and their exact Arrow types must still match the
/// SQL `blob_descriptor` output. Value validation, such as a non-zero version or
/// a known storage kind, belongs to the SDK and is deliberately left there.
fn validate_descriptor_schema(schema: &Schema) -> Result<(), DescriptorError> {
    require_field(schema.fields(), "", "kind", &DataType::UInt8)?;
    require_field(schema.fields(), "", "position", &DataType::UInt64)?;
    require_field(schema.fields(), "", "size", &DataType::UInt64)?;
    require_field(schema.fields(), "", "blob_id", &DataType::UInt32)?;
    require_field(schema.fields(), "", "blob_uri", &DataType::Utf8)?;

    let source_field = schema
        .fields()
        .iter()
        .find(|field| field.name() == "source")
        .ok_or_else(|| {
            DescriptorError::invalid("Descriptor schema is missing required field 'source'")
        })?;
    let DataType::Struct(source_fields) = source_field.data_type() else {
        return Err(DescriptorError::invalid(format!(
            "Descriptor field 'source' must have type Struct, got {}",
            source_field.data_type()
        )));
    };

    require_field(source_fields, "source.", "uri", &DataType::Utf8)?;
    require_field(source_fields, "source.", "version", &DataType::UInt64)?;
    require_field(
        source_fields,
        "source.",
        "committed_at_micros",
        &DataType::UInt64,
    )?;
    require_field(source_fields, "source.", "branch", &DataType::Utf8)?;
    require_field(source_fields, "source.", "column", &DataType::Utf8)?;
    require_field(source_fields, "source.", "row_reference", &DataType::UInt64)?;
    require_field(
        source_fields,
        "source.",
        "row_is_address",
        &DataType::Boolean,
    )?;

    Ok(())
}

/// Downcast one already validated descriptor struct field.
fn descriptor_column<'a, T: 'static>(
    array: &'a StructArray,
    name: &str,
) -> Result<&'a T, DescriptorError> {
    let column = array.column_by_name(name).ok_or_else(|| {
        DescriptorError::arrow(format!("Descriptor array is missing field '{name}'"))
    })?;
    column.as_any().downcast_ref::<T>().ok_or_else(|| {
        DescriptorError::arrow(format!(
            "Descriptor array field '{name}' does not have the validated Arrow type"
        ))
    })
}

/// Read one required primitive value, rejecting an actual NULL.
fn required_primitive<T: ArrowPrimitiveType>(
    array: &PrimitiveArray<T>,
    row: usize,
    name: &str,
) -> Result<T::Native, DescriptorError> {
    if array.is_null(row) {
        Err(DescriptorError::invalid(format!(
            "Descriptor field '{name}' is NULL"
        )))
    } else {
        Ok(array.value(row))
    }
}

/// Read one required UTF-8 value, rejecting an actual NULL.
fn required_string(array: &StringArray, row: usize, name: &str) -> Result<String, DescriptorError> {
    if array.is_null(row) {
        Err(DescriptorError::invalid(format!(
            "Descriptor field '{name}' is NULL"
        )))
    } else {
        Ok(array.value(row).to_string())
    }
}

/// Read one required Boolean value, rejecting an actual NULL.
fn required_bool(array: &BooleanArray, row: usize, name: &str) -> Result<bool, DescriptorError> {
    if array.is_null(row) {
        Err(DescriptorError::invalid(format!(
            "Descriptor field '{name}' is NULL"
        )))
    } else {
        Ok(array.value(row))
    }
}

/// Read one descriptor row without taking ownership of the Arrow input.
///
/// A top-level NULL descriptor is a SQL NULL and returns `Ok(None)`. Every
/// required field must be present and non-NULL for a non-NULL row. Strings are
/// copied so the returned descriptor no longer refers to the caller's buffers.
unsafe fn read_descriptor_row(
    array_ptr: *const FFI_ArrowArray,
    schema_ptr: *const FFI_ArrowSchema,
    row_index: i64,
) -> Result<Option<BlobDescriptor>, DescriptorError> {
    if row_index < 0 {
        return Err(DescriptorError::invalid(format!(
            "Descriptor row index {row_index} must not be negative"
        )));
    }

    let schema = Schema::try_from(&*schema_ptr).map_err(|error| {
        DescriptorError::arrow(format!("Failed to import descriptor Arrow schema: {error}"))
    })?;
    validate_descriptor_schema(&schema)?;

    let array = import_descriptor_struct_array(array_ptr, schema_ptr).map_err(|error| {
        DescriptorError::arrow(format!("Failed to import descriptor Arrow array: {error}"))
    })?;

    let row = match usize::try_from(row_index) {
        Ok(row) => row,
        Err(_) => {
            return Err(DescriptorError::invalid(format!(
                "Descriptor row index {row_index} does not fit this platform"
            )))
        }
    };
    if row >= array.len() {
        return Err(DescriptorError::invalid(format!(
            "Descriptor row index {row_index} is out of bounds for {} rows",
            array.len()
        )));
    }

    if array.is_null(row) {
        return Ok(None);
    }

    let kind = descriptor_column::<UInt8Array>(&array, "kind")?;
    let position = descriptor_column::<UInt64Array>(&array, "position")?;
    let size = descriptor_column::<UInt64Array>(&array, "size")?;
    let blob_id = descriptor_column::<UInt32Array>(&array, "blob_id")?;
    let blob_uri = descriptor_column::<StringArray>(&array, "blob_uri")?;
    let source = descriptor_column::<StructArray>(&array, "source")?;

    if source.is_null(row) {
        return Err(DescriptorError::invalid(
            "Descriptor field 'source' is NULL",
        ));
    }

    let source_uri = descriptor_column::<StringArray>(source, "uri")?;
    let source_version = descriptor_column::<UInt64Array>(source, "version")?;
    let committed_at_micros = descriptor_column::<UInt64Array>(source, "committed_at_micros")?;
    let source_branch = descriptor_column::<StringArray>(source, "branch")?;
    let source_column = descriptor_column::<StringArray>(source, "column")?;
    let row_reference = descriptor_column::<UInt64Array>(source, "row_reference")?;
    let row_is_address = descriptor_column::<BooleanArray>(source, "row_is_address")?;

    let branch = if source_branch.is_null(row) {
        None
    } else {
        Some(source_branch.value(row).to_string())
    };

    Ok(Some(BlobDescriptor {
        kind: required_primitive(kind, row, "kind")?,
        position: required_primitive(position, row, "position")?,
        size: required_primitive(size, row, "size")?,
        blob_id: required_primitive(blob_id, row, "blob_id")?,
        blob_uri: required_string(blob_uri, row, "blob_uri")?,
        source: BlobDescriptorSource {
            uri: required_string(source_uri, row, "source.uri")?,
            version: required_primitive(source_version, row, "source.version")?,
            committed_at_micros: required_primitive(
                committed_at_micros,
                row,
                "source.committed_at_micros",
            )?,
            branch,
            column: required_string(source_column, row, "source.column")?,
            row_reference: required_primitive(row_reference, row, "source.row_reference")?,
            row_is_address: required_bool(row_is_address, row, "source.row_is_address")?,
        },
    }))
}

/// Open one lazy blob file from a borrowed Arrow descriptor row.
///
/// The array and schema are one `blob_descriptor` struct column. Both are
/// borrowed for this call and remain owned by the caller; this function does
/// not call their release callbacks. A top-level NULL descriptor succeeds with
/// a NULL file, while a valid empty blob returns a non-NULL handle whose size is
/// zero. Required descriptor fields and the nested source must be present and
/// non-NULL. Field order does not matter and extra fields are ignored.
///
/// The returned handle owns its SDK `BlobFile` and does not depend on the table
/// or on the Arrow input. The SDK verifies the pinned source and opens the exact
/// source version and row; this function does not reinterpret or repair the
/// source. Native tables check the branch URI, source version, commit
/// microseconds, column and row reference during open and do not change the
/// caller's table checkout. Remote tables require a server with blob-file
/// support (0.5.0 or newer) and send the source URI, branch, version, commit
/// microseconds and row reference to the authenticated byte-range endpoint; the
/// server performs the source check, so a mismatch can surface at open or on a
/// later read. An External descriptor with a zero stored size can issue an
/// object metadata/HEAD request during open, so access or response errors can
/// fail before a handle is returned. As with `fetch_blob_files`, no `BlobFile`
/// retention lease is acquired: dataset cleanup or external object changes can
/// make later reads fail. SDK errors keep their existing mapping and timing.
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `descriptors` and `descriptor_schema` must point to valid Arrow C Data structures
///   that stay alive for the duration of the call
/// - `file_out` must be a valid pointer to receive the optional handle
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
/// - The caller frees a returned handle with `lancedb_blob_file_free`
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_open_blob_file(
    table: *const LanceDBTable,
    descriptors: *const FFI_ArrowArray,
    descriptor_schema: *const FFI_ArrowSchema,
    row_index: i64,
    file_out: *mut *mut LanceDBBlobFile,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !file_out.is_null() {
        *file_out = ptr::null_mut();
    }
    clear_error_message(error_message);

    if table.is_null() || descriptors.is_null() || descriptor_schema.is_null() || file_out.is_null()
    {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let descriptor = match read_descriptor_row(descriptors, descriptor_schema, row_index) {
        Ok(Some(descriptor)) => descriptor,
        Ok(None) => return LanceDBError::Success,
        Err(error) => {
            set_custom_error_message(error_message, &error.message);
            return error.code;
        }
    };

    let tbl = &(*table).inner;
    match get_runtime().block_on(tbl.open_blob_file(&descriptor)) {
        Ok(file) => {
            *file_out = Box::into_raw(Box::new(LanceDBBlobFile { inner: file }));
            LanceDBError::Success
        }
        Err(error) => handle_error(&error, error_message),
    }
}

/// Free one blob file handle.
///
/// # Safety
/// - `file` must be NULL or a handle returned by `lancedb_table_fetch_blob_files`
/// - The handle must not be used after this call
#[no_mangle]
pub unsafe extern "C" fn lancedb_blob_file_free(file: *mut LanceDBBlobFile) {
    if !file.is_null() {
        let _ = Box::from_raw(file);
    }
}

/// Free the bytes of one blob read and reset the buffer.
///
/// # Safety
/// - `buffer` must be NULL or point to a writable `LanceDBBlobBuffer`
/// - `buffer` must either hold bytes from a successful read or be empty
///
/// Freeing an already freed or empty buffer is a no-op.
#[no_mangle]
pub unsafe extern "C" fn lancedb_blob_buffer_free(buffer: *mut LanceDBBlobBuffer) {
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

    *buffer = LanceDBBlobBuffer::EMPTY;
}

/// Return the blob length in bytes.
///
/// The size stays available after the handle is closed, and zero is a valid
/// size for an empty blob.
///
/// # Safety
/// - `file` must be a valid pointer returned by `lancedb_table_fetch_blob_files`
/// - `size_out` must be a valid pointer to receive the size
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_blob_file_size(
    file: *const LanceDBBlobFile,
    size_out: *mut u64,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);

    if file.is_null() || size_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    *size_out = (*file).inner.size();
    LanceDBError::Success
}

/// Return the physical byte offset of the blob inside its data file.
///
/// Embedded handles report `has_position`. Remote handles do not expose their
/// storage layout and report `has_position` as zero with `position` zeroed.
/// This is not the read cursor: seek and read move the cursor, while
/// `read_range` leaves it unchanged.
///
/// # Safety
/// - `file` must be a valid pointer returned by `lancedb_table_fetch_blob_files`
/// - `position_out` must be a valid pointer to receive the position
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_blob_file_position(
    file: *const LanceDBBlobFile,
    position_out: *mut LanceDBBlobFilePosition,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);

    if file.is_null() || position_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    *position_out = match (*file).inner.position() {
        Some(position) => LanceDBBlobFilePosition {
            has_position: true,
            position,
        },
        None => LanceDBBlobFilePosition {
            has_position: false,
            position: 0,
        },
    };
    LanceDBError::Success
}

/// Move the read cursor to a byte offset inside the blob.
///
/// The SDK accepts an offset past the end of the blob; a later read then returns
/// no bytes. Seeking a closed handle fails.
///
/// # Safety
/// - `file` must be a valid pointer returned by `lancedb_table_fetch_blob_files`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_blob_file_seek(
    file: *const LanceDBBlobFile,
    position: u64,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);

    if file.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match get_runtime().block_on((*file).inner.seek(position)) {
        Ok(()) => LanceDBError::Success,
        Err(error) => handle_blob_file_error(error, error_message),
    }
}

/// Read from the current cursor to the end of the blob.
///
/// A successful read advances the cursor to the blob size. Reading at or past
/// the end succeeds with an empty buffer. Reading a closed handle fails.
///
/// # Safety
/// - `file` must be a valid pointer returned by `lancedb_table_fetch_blob_files`
/// - `buffer_out` must be a valid pointer to receive the bytes
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
/// - The caller frees the bytes with `lancedb_blob_buffer_free`
#[no_mangle]
pub unsafe extern "C" fn lancedb_blob_file_read(
    file: *const LanceDBBlobFile,
    buffer_out: *mut LanceDBBlobBuffer,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !buffer_out.is_null() {
        *buffer_out = LanceDBBlobBuffer::EMPTY;
    }
    clear_error_message(error_message);

    if file.is_null() || buffer_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match get_runtime().block_on((*file).inner.read()) {
        Ok(bytes) => {
            publish_blob_buffer(&bytes, buffer_out);
            LanceDBError::Success
        }
        Err(error) => handle_blob_file_error(error, error_message),
    }
}

/// Read a blob-local byte range without moving the cursor.
///
/// `offset` and `length` are relative to the beginning of the blob. Empty ranges
/// are allowed at or before the blob size. An end past the blob size, or an
/// offset and length whose sum overflows `uint64_t`, is rejected. Reading a
/// closed handle fails.
///
/// # Safety
/// - `file` must be a valid pointer returned by `lancedb_table_fetch_blob_files`
/// - `buffer_out` must be a valid pointer to receive the bytes
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - Error code indicating success or failure
/// - The caller frees the bytes with `lancedb_blob_buffer_free`
#[no_mangle]
pub unsafe extern "C" fn lancedb_blob_file_read_range(
    file: *const LanceDBBlobFile,
    offset: u64,
    length: u64,
    buffer_out: *mut LanceDBBlobBuffer,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if !buffer_out.is_null() {
        *buffer_out = LanceDBBlobBuffer::EMPTY;
    }
    clear_error_message(error_message);

    if file.is_null() || buffer_out.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    let Some(end) = offset.checked_add(length) else {
        return handle_blob_file_error(
            lance::Error::invalid_input(format!(
                "Blob range offset {offset} plus length {length} overflowed"
            )),
            error_message,
        );
    };

    match get_runtime().block_on((*file).inner.read_range(offset..end)) {
        Ok(bytes) => {
            publish_blob_buffer(&bytes, buffer_out);
            LanceDBError::Success
        }
        Err(error) => handle_blob_file_error(error, error_message),
    }
}

/// Close the blob file and release its resources.
///
/// Closing is idempotent. After a close, reads, range reads and seeks fail,
/// while size and position remain available. Reads already in flight may still
/// complete.
///
/// # Safety
/// - `file` must be a valid pointer returned by `lancedb_table_fetch_blob_files`
/// - `error_message` can be NULL to ignore detailed error messages
#[no_mangle]
pub unsafe extern "C" fn lancedb_blob_file_close(
    file: *const LanceDBBlobFile,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    clear_error_message(error_message);

    if file.is_null() {
        set_invalid_argument_message(error_message);
        return LanceDBError::InvalidArgument;
    }

    match get_runtime().block_on((*file).inner.close()) {
        Ok(()) => LanceDBError::Success,
        Err(error) => handle_blob_file_error(error, error_message),
    }
}
