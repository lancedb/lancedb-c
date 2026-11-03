// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! DataFusion Expr C API for building filter expressions
//!
//! This module provides C-callable functions to construct DataFusion `Expr` values
//! that can be used as query filters via `lancedb_query_df_filter()`.

use std::ffi::CStr;
use std::os::raw::c_char;

use crate::error::set_invalid_argument_message;
use datafusion_expr::expr::{BinaryExpr, ScalarFunction};
use datafusion_expr::{col, lit, Expr, Operator};

/// Opaque handle to a DataFusion Expr
pub struct LanceDBExpr {
    pub(crate) inner: Expr,
}

/// Binary operator enum for C API
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub enum LanceDBBinaryOp {
    Eq = 0,
    NotEq = 1,
    Lt = 2,
    LtEq = 3,
    Gt = 4,
    GtEq = 5,
    And = 6,
    Or = 7,
    Plus = 8,
    Minus = 9,
    Multiply = 10,
    Divide = 11,
    Modulo = 12,
}

impl From<LanceDBBinaryOp> for Operator {
    fn from(op: LanceDBBinaryOp) -> Self {
        match op {
            LanceDBBinaryOp::Eq => Operator::Eq,
            LanceDBBinaryOp::NotEq => Operator::NotEq,
            LanceDBBinaryOp::Lt => Operator::Lt,
            LanceDBBinaryOp::LtEq => Operator::LtEq,
            LanceDBBinaryOp::Gt => Operator::Gt,
            LanceDBBinaryOp::GtEq => Operator::GtEq,
            LanceDBBinaryOp::And => Operator::And,
            LanceDBBinaryOp::Or => Operator::Or,
            LanceDBBinaryOp::Plus => Operator::Plus,
            LanceDBBinaryOp::Minus => Operator::Minus,
            LanceDBBinaryOp::Multiply => Operator::Multiply,
            LanceDBBinaryOp::Divide => Operator::Divide,
            LanceDBBinaryOp::Modulo => Operator::Modulo,
        }
    }
}

/// Create a column reference expression
///
/// # Safety
/// - `name` must be a valid null-terminated C string
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_column(name: *const c_char) -> *mut LanceDBExpr {
    if name.is_null() {
        return std::ptr::null_mut();
    }

    let name_str = match CStr::from_ptr(name).to_str() {
        Ok(s) => s,
        Err(_) => return std::ptr::null_mut(),
    };

    Box::into_raw(Box::new(LanceDBExpr {
        inner: col(name_str),
    }))
}

/// Create a string literal expression
///
/// # Safety
/// - `value` must be a valid null-terminated C string
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_literal_string(value: *const c_char) -> *mut LanceDBExpr {
    if value.is_null() {
        return std::ptr::null_mut();
    }

    let value_str = match CStr::from_ptr(value).to_str() {
        Ok(s) => s.to_string(),
        Err(_) => return std::ptr::null_mut(),
    };

    Box::into_raw(Box::new(LanceDBExpr {
        inner: lit(value_str),
    }))
}

/// Create an integer literal expression (i64)
///
/// # Returns
/// - Non-null pointer to LanceDBExpr
#[no_mangle]
pub extern "C" fn lancedb_expr_literal_i64(value: i64) -> *mut LanceDBExpr {
    Box::into_raw(Box::new(LanceDBExpr { inner: lit(value) }))
}

/// Create a float literal expression (f64)
///
/// # Returns
/// - Non-null pointer to LanceDBExpr
#[no_mangle]
pub extern "C" fn lancedb_expr_literal_f64(value: f64) -> *mut LanceDBExpr {
    Box::into_raw(Box::new(LanceDBExpr { inner: lit(value) }))
}

/// Create a boolean literal expression
///
/// # Returns
/// - Non-null pointer to LanceDBExpr
#[no_mangle]
pub extern "C" fn lancedb_expr_literal_bool(value: bool) -> *mut LanceDBExpr {
    Box::into_raw(Box::new(LanceDBExpr { inner: lit(value) }))
}

/// Create a binary expression (left op right)
///
/// # Safety
/// - `left` and `right` must be valid pointers returned from lancedb_expr_* functions
/// - Both `left` and `right` are consumed by this function; do not use or free them after calling
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_binary(
    left: *mut LanceDBExpr,
    op: LanceDBBinaryOp,
    right: *mut LanceDBExpr,
) -> *mut LanceDBExpr {
    let left_expr = if !left.is_null() {
        Some(Box::from_raw(left).inner)
    } else {
        None
    };
    let right_expr = if !right.is_null() {
        Some(Box::from_raw(right).inner)
    } else {
        None
    };

    let (Some(left_expr), Some(right_expr)) = (left_expr, right_expr) else {
        return std::ptr::null_mut();
    };

    Box::into_raw(Box::new(LanceDBExpr {
        inner: Expr::BinaryExpr(BinaryExpr::new(
            Box::new(left_expr),
            op.into(),
            Box::new(right_expr),
        )),
    }))
}

/// Create a NOT expression
///
/// # Safety
/// - `expr` must be a valid pointer returned from lancedb_expr_* functions
/// - `expr` is consumed by this function; do not use or free it after calling
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_not(expr: *mut LanceDBExpr) -> *mut LanceDBExpr {
    if expr.is_null() {
        return std::ptr::null_mut();
    }

    let inner = Box::from_raw(expr).inner;

    Box::into_raw(Box::new(LanceDBExpr {
        inner: Expr::Not(Box::new(inner)),
    }))
}

/// Create an IS NULL expression
///
/// # Safety
/// - `expr` must be a valid pointer returned from lancedb_expr_* functions
/// - `expr` is consumed by this function; do not use or free it after calling
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_is_null(expr: *mut LanceDBExpr) -> *mut LanceDBExpr {
    if expr.is_null() {
        return std::ptr::null_mut();
    }

    let inner = Box::from_raw(expr).inner;

    Box::into_raw(Box::new(LanceDBExpr {
        inner: Expr::IsNull(Box::new(inner)),
    }))
}

/// Create a STARTS WITH expression
///
/// # Safety
/// - `str_expr` must be a valid pointer returned from lancedb_expr_* functions
/// - `prefix_expr` must be a valid pointer returned from lancedb_expr_* functions
/// - Both inputs are consumed; do not use or free them after calling
/// - `error_message` may be NULL; otherwise it receives an error message on failure
///   that the caller must free with lancedb_free_string()
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_starts_with(
    str_expr: *mut LanceDBExpr,
    prefix_expr: *mut LanceDBExpr,
    error_message: *mut *mut c_char,
) -> *mut LanceDBExpr {
    let str_e = if !str_expr.is_null() {
        Some(Box::from_raw(str_expr).inner)
    } else {
        None
    };

    let pre_e = if !prefix_expr.is_null() {
        Some(Box::from_raw(prefix_expr).inner)
    } else {
        None
    };

    let (Some(str_e), Some(pre_e)) = (str_e, pre_e) else {
        set_invalid_argument_message(error_message);
        return std::ptr::null_mut();
    };

    let udf = datafusion_functions::string::starts_with();

    let func_expr = Expr::ScalarFunction(ScalarFunction::new_udf(udf, vec![str_e, pre_e]));

    Box::into_raw(Box::new(LanceDBExpr { inner: func_expr }))
}

/// Create an IS NOT NULL expression
///
/// # Safety
/// - `expr` must be a valid pointer returned from lancedb_expr_* functions
/// - `expr` is consumed by this function; do not use or free it after calling
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_is_not_null(expr: *mut LanceDBExpr) -> *mut LanceDBExpr {
    if expr.is_null() {
        return std::ptr::null_mut();
    }

    let inner = Box::from_raw(expr).inner;

    Box::into_raw(Box::new(LanceDBExpr {
        inner: Expr::IsNotNull(Box::new(inner)),
    }))
}

/// Create an AND expression (convenience for binary AND)
///
/// # Safety
/// - `left` and `right` must be valid pointers returned from lancedb_expr_* functions
/// - Both are consumed by this function; do not use or free them after calling
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_and(
    left: *mut LanceDBExpr,
    right: *mut LanceDBExpr,
) -> *mut LanceDBExpr {
    let left_expr = if !left.is_null() {
        Some(Box::from_raw(left).inner)
    } else {
        None
    };
    let right_expr = if !right.is_null() {
        Some(Box::from_raw(right).inner)
    } else {
        None
    };

    let (Some(left_expr), Some(right_expr)) = (left_expr, right_expr) else {
        return std::ptr::null_mut();
    };

    Box::into_raw(Box::new(LanceDBExpr {
        inner: left_expr.and(right_expr),
    }))
}

/// Create an OR expression (convenience for binary OR)
///
/// # Safety
/// - `left` and `right` must be valid pointers returned from lancedb_expr_* functions
/// - Both are consumed by this function; do not use or free them after calling
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_or(
    left: *mut LanceDBExpr,
    right: *mut LanceDBExpr,
) -> *mut LanceDBExpr {
    let left_expr = if !left.is_null() {
        Some(Box::from_raw(left).inner)
    } else {
        None
    };
    let right_expr = if !right.is_null() {
        Some(Box::from_raw(right).inner)
    } else {
        None
    };

    let (Some(left_expr), Some(right_expr)) = (left_expr, right_expr) else {
        return std::ptr::null_mut();
    };

    Box::into_raw(Box::new(LanceDBExpr {
        inner: left_expr.or(right_expr),
    }))
}

/// Create an IN list expression (expr IN (value1, value2, ...))
///
/// # Safety
/// - `expr` must be a valid pointer returned from lancedb_expr_* functions
/// - `list` must be an array of valid pointers to LanceDBExpr
/// - `list_len` must match the actual number of elements in the array
/// - `expr` and all elements of `list` are consumed; do not use or free them after calling
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_in_list(
    expr: *mut LanceDBExpr,
    list: *const *mut LanceDBExpr,
    list_len: usize,
    negated: bool,
    error_message: *mut *mut c_char,
) -> *mut LanceDBExpr {
    if expr.is_null() {
        set_invalid_argument_message(error_message);
        return std::ptr::null_mut();
    }

    let inner = Box::from_raw(expr).inner;

    if list.is_null() {
        set_invalid_argument_message(error_message);
        return std::ptr::null_mut();
    }

    let mut list_exprs = Vec::with_capacity(list_len);
    let mut has_null = false;
    for i in 0..list_len {
        let item = *list.add(i);
        if !item.is_null() {
            list_exprs.push(Box::from_raw(item).inner);
        } else {
            has_null = true;
        }
    }
    if has_null {
        set_invalid_argument_message(error_message);
        return std::ptr::null_mut();
    }

    Box::into_raw(Box::new(LanceDBExpr {
        inner: inner.in_list(list_exprs, negated),
    }))
}

/// Create an array_has expression: check if array column contains a value
///
/// # Safety
/// - `array_expr` must be a valid pointer returned from lancedb_expr_* functions (consumed)
/// - `value_expr` must be a valid pointer returned from lancedb_expr_* functions (consumed)
/// - Both inputs are consumed; do not use or free them after calling
///
/// # Returns
/// - Non-null pointer to LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_array_has(
    array_expr: *mut LanceDBExpr,
    value_expr: *mut LanceDBExpr,
    error_message: *mut *mut c_char,
) -> *mut LanceDBExpr {
    let arr = if !array_expr.is_null() {
        Some(Box::from_raw(array_expr).inner)
    } else {
        None
    };
    let val = if !value_expr.is_null() {
        Some(Box::from_raw(value_expr).inner)
    } else {
        None
    };

    let (Some(arr), Some(val)) = (arr, val) else {
        set_invalid_argument_message(error_message);
        return std::ptr::null_mut();
    };

    let udf = datafusion_functions_nested::array_has::array_has_udf();
    let func_expr = Expr::ScalarFunction(ScalarFunction::new_udf(udf, vec![arr, val]));
    Box::into_raw(Box::new(LanceDBExpr { inner: func_expr }))
}

/// Clone an expression (creates an independent copy)
///
/// # Safety
/// - `expr` must be a valid pointer returned from lancedb_expr_* functions
/// - The original `expr` remains valid and must still be freed separately
///
/// # Returns
/// - Non-null pointer to a new LanceDBExpr on success, NULL on failure
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_clone(expr: *const LanceDBExpr) -> *mut LanceDBExpr {
    if expr.is_null() {
        return std::ptr::null_mut();
    }

    Box::into_raw(Box::new(LanceDBExpr {
        inner: (*expr).inner.clone(),
    }))
}

/// Free an expression
///
/// # Safety
/// - `expr` must be a valid pointer returned from lancedb_expr_* functions
/// - `expr` must not be used after calling this function
#[no_mangle]
pub unsafe extern "C" fn lancedb_expr_free(expr: *mut LanceDBExpr) {
    if !expr.is_null() {
        let _ = Box::from_raw(expr);
    }
}
