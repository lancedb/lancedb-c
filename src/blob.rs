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

use lancedb::blob::BlobFile;

use crate::connection::{get_runtime, LanceDBTable};
use crate::error::{
    clear_error_message, handle_blob_file_error, handle_error, set_invalid_argument_message,
    set_unknown_error_message, LanceDBError,
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
