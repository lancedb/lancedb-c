// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Borrowed imports of Arrow C Data struct arrays.
//!
//! The C Data Interface transfers ownership of an `ArrowArray` when it is
//! imported. Some C entry points only read an array for the duration of the
//! call and leave the producer's release callback in the caller's hands. The
//! helper here makes that borrowing explicit without parsing Arrow buffers by
//! hand.

use std::ptr;
use std::sync::Arc;

use arrow_array::ffi::FFI_ArrowArray;
use arrow_array::{make_array, ArrayRef, StructArray};
use arrow_schema::ffi::FFI_ArrowSchema;
use arrow_schema::{ArrowError, DataType};
use lancedb::arrow::arrow_buffer::NullBuffer;
use lancedb::arrow::arrow_data::ArrayData;

/// Import a caller-owned Arrow C Data struct array without consuming it.
///
/// [`arrow_array::ffi::from_ffi`] normally consumes the `ArrowArray` and calls
/// its release callback when the imported data is dropped. This helper copies
/// the C struct, clears the release callback in the copy, and imports that copy.
/// The returned array therefore reads the producer's buffers without taking
/// ownership of them, and the caller's `ArrowArray` is left unchanged.
///
/// The result is only valid while the caller keeps the original array alive and
/// unchanged. It must not be stored beyond the FFI call. The caller still owns
/// the original array and releases it through the Arrow C Data Interface.
///
/// # Safety
/// - `array` and `schema` must point to valid, initialized Arrow C Data structs
/// - `array` must stay alive and unchanged for the lifetime of the returned array
/// - `schema` must describe a struct array
pub(crate) unsafe fn import_borrowed_struct_array(
    array: *const FFI_ArrowArray,
    schema: *const FFI_ArrowSchema,
) -> Result<StructArray, ArrowError> {
    let mut borrowed = ptr::read(array);
    borrowed.release = None;
    let data = arrow_array::ffi::from_ffi(borrowed, &*schema)?;
    struct_array_from_data(data)
}

/// Turn imported struct data into a zero-offset `StructArray`.
///
/// `StructArray::from(ArrayData)` applies a parent offset to its children and
/// then converts a nested struct child with `StructArray::from` again, which
/// double-applies the offset for nested structs. C Data producers commonly
/// export a sliced struct with the parent offset set and children left at full
/// length, so normalize the offset recursively before building the arrays.
fn struct_array_from_data(data: ArrayData) -> Result<StructArray, ArrowError> {
    let (data_type, len, nulls, offset, _buffers, child_data) = data.into_parts();
    // `nulls` already has `offset` applied to its logical start.
    normalize_struct(data_type, len, nulls, offset, child_data)
}

fn normalize_struct(
    data_type: DataType,
    len: usize,
    nulls: Option<NullBuffer>,
    child_offset: usize,
    child_data: Vec<ArrayData>,
) -> Result<StructArray, ArrowError> {
    let DataType::Struct(fields) = data_type else {
        return Err(ArrowError::CDataInterface(
            "Expected a struct Arrow array".to_string(),
        ));
    };

    let mut arrays = Vec::with_capacity(child_data.len());
    for child in child_data {
        arrays.push(normalize_child(child, child_offset, len)?);
    }
    StructArray::try_new_with_length(fields, arrays, nulls, len)
}

fn normalize_child(
    data: ArrayData,
    parent_offset: usize,
    len: usize,
) -> Result<ArrayRef, ArrowError> {
    let end = parent_offset
        .checked_add(len)
        .ok_or_else(|| ArrowError::CDataInterface("Arrow child offset overflow".to_string()))?;
    if end > data.len() {
        return Err(ArrowError::CDataInterface(format!(
            "Arrow child offset {parent_offset} plus length {len} exceeds its {} values",
            data.len()
        )));
    }

    if matches!(data.data_type(), DataType::Struct(_)) {
        let (data_type, _len, nulls, child_offset, _buffers, child_data) = data.into_parts();
        let child_offset = child_offset
            .checked_add(parent_offset)
            .ok_or_else(|| ArrowError::CDataInterface("Arrow child offset overflow".to_string()))?;
        let nulls = nulls.map(|nulls| nulls.slice(parent_offset, len));
        Ok(Arc::new(normalize_struct(
            data_type,
            len,
            nulls,
            child_offset,
            child_data,
        )?))
    } else {
        Ok(make_array(data.slice(parent_offset, len)))
    }
}
