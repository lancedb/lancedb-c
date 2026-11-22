/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

/*
 * This file is compiled as C17 and consumes the public header the same way a
 * normal C caller does.
 */
#include "lancedb.h"

/*
 * The Rust FFI layer accepts these C enums as c_int. That matches the public
 * header when the compiler uses the default int-sized enum representation.
 * Check it here on every platform that builds the tests, so a platform or flag
 * that changes the enum ABI fails at compile time instead of corrupting calls.
 */
_Static_assert(sizeof(LanceDBError) == sizeof(int), "LanceDBError must have the C int ABI");
_Static_assert(sizeof(LanceDBDistanceType) == sizeof(int), "LanceDBDistanceType must have the C int ABI");
_Static_assert(sizeof(LanceDBNormalizeMethod) == sizeof(int), "LanceDBNormalizeMethod must have the C int ABI");
_Static_assert(sizeof(LanceDBIndexType) == sizeof(int), "LanceDBIndexType must have the C int ABI");
_Static_assert(sizeof(LanceDBOptimizeType) == sizeof(int), "LanceDBOptimizeType must have the C int ABI");
_Static_assert(sizeof(LanceDBBinaryOp) == sizeof(int), "LanceDBBinaryOp must have the C int ABI");
_Static_assert(sizeof(LanceDBBranchSourceKind) == sizeof(int), "LanceDBBranchSourceKind must have the C int ABI");
_Static_assert(sizeof(float) == sizeof(int), "LanceDBVectorIndexConfig assumes int-sized fields");
_Static_assert(offsetof(LanceDBVectorIndexConfig, distance_type) == 4 * sizeof(int),
               "LanceDBVectorIndexConfig layout changed");
_Static_assert(sizeof(LanceDBVectorIndexConfig) == 5 * sizeof(int) + sizeof(float),
               "LanceDBVectorIndexConfig layout changed");

/*
 * C callers can hold enum values that are not one of the header's enumerators,
 * for example from an uninitialized struct or a header/library mismatch. These
 * helpers pass such values through the public C API. They live in a C file
 * because casting an out-of-range integer to an enum in the C++ tests would be
 * undefined behavior in the test process before the library sees the value.
 */

LanceDBError lancedb_test_create_vector_index_with_index_type(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    int index_type,
    char** error_message) {
  return lancedb_table_create_vector_index(
      table, columns, num_columns, (LanceDBIndexType)index_type, NULL, error_message);
}

LanceDBError lancedb_test_create_vector_index_with_distance_type(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    int distance_type,
    char** error_message) {
  const LanceDBVectorIndexConfig config = {
      .num_partitions = -1,
      .num_sub_vectors = -1,
      .max_iterations = -1,
      .sample_rate = 0.0f,
      .distance_type = (LanceDBDistanceType)distance_type,
      .replace = 1};
  return lancedb_table_create_vector_index(
      table, columns, num_columns, LANCEDB_INDEX_IVF_FLAT, &config, error_message);
}

LanceDBError lancedb_test_create_index_with_index_type(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    int index_type,
    char** error_message) {
  return lancedb_table_create_index(
      table, columns, num_columns, (LanceDBIndexType)index_type, NULL, error_message);
}

LanceDBError lancedb_test_create_scalar_index_with_index_type(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    int index_type,
    char** error_message) {
  const LanceDBScalarIndexConfig config = {.replace = 1};
  return lancedb_table_create_scalar_index(
      table, columns, num_columns, (LanceDBIndexType)index_type, &config, error_message);
}

LanceDBError lancedb_test_optimize_with_type(
    const LanceDBTable* table,
    int optimize_type,
    char** error_message) {
  return lancedb_table_optimize(table, (LanceDBOptimizeType)optimize_type, error_message);
}

LanceDBError lancedb_test_set_distance_type(
    LanceDBVectorQuery* query,
    int distance_type,
    char** error_message) {
  return lancedb_vector_query_distance_type(
      query, (LanceDBDistanceType)distance_type, error_message);
}

LanceDBError lancedb_test_create_branch_with_source_kind(
    const LanceDBTable* table,
    const char* name,
    int source_kind,
    const char* source_name,
    int has_version,
    uint64_t version,
    LanceDBTable** table_out,
    char** error_message) {
  const LanceDBBranchSource source = {
      .kind = (LanceDBBranchSourceKind)source_kind,
      .name = source_name,
      .has_version = has_version,
      .version = version};
  return lancedb_table_create_branch(table, name, &source, table_out, error_message);
}

LanceDBError lancedb_test_set_normalize_method(
    LanceDBVectorQuery* query,
    int normalize_method,
    char** error_message) {
  return lancedb_vector_query_norm(
      query, (LanceDBNormalizeMethod)normalize_method, error_message);
}

LanceDBExpr* lancedb_test_expr_binary(
    LanceDBExpr* left,
    int op,
    LanceDBExpr* right) {
  return lancedb_expr_binary(left, (LanceDBBinaryOp)op, right);
}
