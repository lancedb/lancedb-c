/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

static void verify_query_result(LanceDBQueryResult* query_result, size_t expected_rows) {
  // Convert to Arrow
  FFI_ArrowArray** result_arrays = nullptr;
  FFI_ArrowSchema* result_schema = nullptr;
  size_t count = 0;
  char* error_message = nullptr;
  const auto result = lancedb_query_result_to_arrow(
      query_result, &result_arrays, &result_schema, &count, &error_message);

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count > 0);
  REQUIRE(result_arrays != nullptr);
  REQUIRE(result_schema != nullptr);

  // Count total rows across all batches
  size_t sum_rows = 0;
  for (size_t i = 0; i < count; i++) {
    sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
  }
  REQUIRE(sum_rows == expected_rows);

  // Verify schema has 2 columns
  REQUIRE(reinterpret_cast<ArrowSchema*>(result_schema)->n_children == 2);

  // Clean up
  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);
}

static void create_key_index(LanceDBTable* table) {
  // Create BTREE index on "key" column
  const char* index_columns[] = {"key"};
  LanceDBScalarIndexConfig config = {
    .replace = 0,
    .force_update_statistics = 0
  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_scalar_index(
      table, index_columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - DataFusion Expr Filter", "[query][expr]") {
  const std::string table_name = "query_df_filter_test";

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, 100, 0);
  REQUIRE(table != nullptr);

  create_key_index(table);

  SECTION("Filter by single key using Expr") {
    // Build expr: key = "key_42"
    LanceDBExpr* col_expr = lancedb_expr_column("key");
    REQUIRE(col_expr != nullptr);
    LanceDBExpr* val_expr = lancedb_expr_literal_string("key_42");
    REQUIRE(val_expr != nullptr);
    LanceDBExpr* eq_expr = lancedb_expr_binary(col_expr, LANCEDB_BINARY_OP_EQ, val_expr);
    REQUIRE(eq_expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    error_message = nullptr;
    result = lancedb_query_df_filter(query, eq_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    verify_query_result(query_result, 1);
  }

  SECTION("Filter by IN list using Expr") {
    // Build expr: key IN ("key_10", "key_20", "key_30", "key_40", "key_50")
    LanceDBExpr* col_expr = lancedb_expr_column("key");
    REQUIRE(col_expr != nullptr);

    LanceDBExpr* list_items[5];
    list_items[0] = lancedb_expr_literal_string("key_10");
    list_items[1] = lancedb_expr_literal_string("key_20");
    list_items[2] = lancedb_expr_literal_string("key_30");
    list_items[3] = lancedb_expr_literal_string("key_40");
    list_items[4] = lancedb_expr_literal_string("key_50");
    for (auto& item : list_items) {
      REQUIRE(item != nullptr);
    }

    char* error_message = nullptr;
    LanceDBExpr* in_expr = lancedb_expr_in_list(col_expr, list_items, 5, false, &error_message);
    REQUIRE(in_expr != nullptr);
    REQUIRE(error_message == nullptr);
    // col_expr and all list_items are consumed

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    error_message = nullptr;
    result = lancedb_query_df_filter(query, in_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    verify_query_result(query_result, 5);
  }

  SECTION("Expr NOT") {
    // Build expr: NOT (key = "key_42")
    LanceDBExpr* col_expr = lancedb_expr_column("key");
    LanceDBExpr* val_expr = lancedb_expr_literal_string("key_42");
    LanceDBExpr* eq_expr = lancedb_expr_binary(col_expr, LANCEDB_BINARY_OP_EQ, val_expr);
    LanceDBExpr* not_expr = lancedb_expr_not(eq_expr);
    REQUIRE(not_expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, not_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    verify_query_result(query_result, 99);
  }

  SECTION("Expr AND combining two conditions") {
    // Build expr: key = "key_42" AND key > "key_41"
    // (tautological AND to test the AND combinator)
    LanceDBExpr* eq1 = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_42"));
    LanceDBExpr* eq2 = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_GT,
        lancedb_expr_literal_string("key_41"));
    LanceDBExpr* and_expr = lancedb_expr_and(eq1, eq2);
    REQUIRE(and_expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    result = lancedb_query_df_filter(query, and_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);
    verify_query_result(query_result, 1);
  }

  SECTION("Expr OR combining two conditions") {
    // Build expr: key = "key_10" OR key = "key_20"
    LanceDBExpr* eq1 = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_10"));
    LanceDBExpr* eq2 = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_20"));
    LanceDBExpr* or_expr = lancedb_expr_or(eq1, eq2);
    REQUIRE(or_expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    result = lancedb_query_df_filter(query, or_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);
    verify_query_result(query_result, 2);
  }

  SECTION("Expr clone") {
    LanceDBExpr* original = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_42"));
    REQUIRE(original != nullptr);

    LanceDBExpr* another_eq = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_24"));
    REQUIRE(another_eq != nullptr);
    // cloned include the original with an OR condition
    LanceDBExpr* cloned = lancedb_expr_or(
        lancedb_expr_clone(original),
        another_eq);
    REQUIRE(cloned != nullptr);

    // Use original in first query
    LanceDBQuery* query1 = lancedb_query_new(table);
    REQUIRE(query1 != nullptr);
    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query1, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    result = lancedb_query_df_filter(query1, original, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    LanceDBQueryResult* result1 = lancedb_query_execute(query1, nullptr);
    REQUIRE(result1 != nullptr);
    verify_query_result(result1, 1);

    // Use clone in second query
    LanceDBQuery* query2 = lancedb_query_new(table);
    REQUIRE(query2 != nullptr);
    result = lancedb_query_select(query2, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    result = lancedb_query_df_filter(query2, cloned, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    LanceDBQueryResult* result2 = lancedb_query_execute(query2, nullptr);
    REQUIRE(result2 != nullptr);
    verify_query_result(result2, 2);
  }

  SECTION("Null arguments") {
    char* error_message = nullptr;
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    // Null expr
    LanceDBError result = lancedb_query_df_filter(query, nullptr, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Null query
    LanceDBExpr* expr = lancedb_expr_column("key");
    result = lancedb_query_df_filter(nullptr, expr, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_expr_free(expr);

    lancedb_query_free(query);
  }

  SECTION("Filter by unknown key using Expr (should return empty result)") {
    // Build expr: key = "key_999" (doesn't exist in table with key_0 to key_99)
    LanceDBExpr* eq_expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_999"));
    REQUIRE(eq_expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, eq_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow - should return empty result
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 0);
    REQUIRE(result_arrays == nullptr);
    REQUIRE(result_schema == nullptr);
  }

  SECTION("Filter on non-existent column using Expr (should fail at execution)") {
    // Build expr: nonexistent_column = "value"
    LanceDBExpr* eq_expr = lancedb_expr_binary(
        lancedb_expr_column("nonexistent_column"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("value"));
    REQUIRE(eq_expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, eq_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Error should be caught at execution time
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result == nullptr);
  }

  SECTION("DataFusion filter takes precedence over SQL filter") {
    // Set SQL filter that matches key_10 only
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // SQL filter: key = "key_10" (would return 1 row)
    error_message = nullptr;
    result = lancedb_query_where_filter(query, "key = \"key_10\"", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // DataFusion filter: key = "key_42" (should override SQL filter)
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_42"));
    REQUIRE(expr != nullptr);

    error_message = nullptr;
    result = lancedb_query_df_filter(query, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute - should return key_42, not key_10
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify the returned key is "key_42"
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);

    // Import into Arrow RecordBatch to inspect the key value
    auto imported_schema = arrow::ImportSchema(
        reinterpret_cast<ArrowSchema*>(result_schema));
    REQUIRE(imported_schema.ok());

    auto imported_batch = arrow::ImportRecordBatch(
        reinterpret_cast<ArrowArray*>(result_arrays[0]),
        imported_schema.ValueUnsafe());
    REQUIRE(imported_batch.ok());

    auto batch = imported_batch.ValueUnsafe();
    REQUIRE(batch->num_rows() == 1);

    auto key_array = std::static_pointer_cast<arrow::StringArray>(
        batch->column(0));
    REQUIRE(key_array->GetString(0) == "key_42");

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Expr free on NULL is safe") {
    lancedb_expr_free(nullptr);
  }

  SECTION("Expr builder NULL arguments") {
    REQUIRE(lancedb_expr_column(nullptr) == nullptr);
    REQUIRE(lancedb_expr_literal_string(nullptr) == nullptr);
    REQUIRE(lancedb_expr_binary(nullptr, LANCEDB_BINARY_OP_EQ, nullptr) == nullptr);
    REQUIRE(lancedb_expr_not(nullptr) == nullptr);
    REQUIRE(lancedb_expr_is_null(nullptr) == nullptr);
    REQUIRE(lancedb_expr_is_not_null(nullptr) == nullptr);
    REQUIRE(lancedb_expr_and(nullptr, nullptr) == nullptr);
    REQUIRE(lancedb_expr_or(nullptr, nullptr) == nullptr);
    REQUIRE(lancedb_expr_clone(nullptr) == nullptr);
  }

  SECTION("Partial NULL arguments consume non-null expr (binary)") {
    LanceDBExpr* left = lancedb_expr_column("key");
    REQUIRE(left != nullptr);
    REQUIRE(lancedb_expr_binary(left, LANCEDB_BINARY_OP_EQ, nullptr) == nullptr);

    LanceDBExpr* right = lancedb_expr_literal_string("value");
    REQUIRE(right != nullptr);
    REQUIRE(lancedb_expr_binary(nullptr, LANCEDB_BINARY_OP_EQ, right) == nullptr);
  }

  SECTION("Partial NULL arguments consume non-null expr (and)") {
    LanceDBExpr* left = lancedb_expr_literal_bool(true);
    REQUIRE(left != nullptr);
    REQUIRE(lancedb_expr_and(left, nullptr) == nullptr);

    LanceDBExpr* right = lancedb_expr_literal_bool(false);
    REQUIRE(right != nullptr);
    REQUIRE(lancedb_expr_and(nullptr, right) == nullptr);
  }

  SECTION("Partial NULL arguments consume non-null expr (or)") {
    LanceDBExpr* left = lancedb_expr_literal_bool(true);
    REQUIRE(left != nullptr);
    REQUIRE(lancedb_expr_or(left, nullptr) == nullptr);

    LanceDBExpr* right = lancedb_expr_literal_bool(false);
    REQUIRE(right != nullptr);
    REQUIRE(lancedb_expr_or(nullptr, right) == nullptr);
  }

  SECTION("Partial NULL arguments consume non-null expr (in_list)") {
    // expr is valid, list is NULL — expr must still be consumed
    char* error_message = nullptr;
    LanceDBExpr* expr = lancedb_expr_column("key");
    REQUIRE(expr != nullptr);
    REQUIRE(lancedb_expr_in_list(expr, nullptr, 0, false, &error_message) == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // expr is valid, list has a NULL element — expr and non-null items must be consumed
    LanceDBExpr* list_items[2];
    list_items[0] = lancedb_expr_literal_string("value1");
    list_items[1] = nullptr;
    expr = lancedb_expr_column("key");
    REQUIRE(expr != nullptr);
    REQUIRE(lancedb_expr_in_list(expr, list_items, 2, false, &error_message) == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - Expr Type Mismatches", "[query][expr]") {
  // Create a table schema with numeric-only string keys ("0", "1", ..., "9")
  auto schema = create_test_schema();

  arrow::StringBuilder key_builder;
  arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
      std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

  for (int i = 0; i < 10; i++) {
    REQUIRE(key_builder.Append(std::to_string(i)).ok());
    REQUIRE(data_builder.Append().ok());
    auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
      REQUIRE(list_builder->Append(static_cast<float>(i * 10 + j)).ok());
    }
  }

  std::shared_ptr<arrow::Array> key_array, data_array;
  REQUIRE(key_builder.Finish(&key_array).ok());
  REQUIRE(data_builder.Finish(&data_array).ok());

  auto batch = arrow::RecordBatch::Make(schema, 10, {key_array, data_array});
  auto reader = create_reader_from_batch(batch);
  REQUIRE(reader != nullptr);

  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create(
      db, "numeric_keys_test",
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      reader, &table, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  SECTION("Arithmetic on non-numeric column (string + int)") {
    // Build expr: key + 1 (key is utf8, not numeric)
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_PLUS,
        lancedb_expr_literal_i64(1));
    REQUIRE(expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Should fail at execution due to type mismatch
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result == nullptr);
  }

  SECTION("Equality with type coercion (string column vs. int") {
    // Build expr: key = 5 (key is utf8 but values are numeric strings)
    // DataFusion coerces int to string, matching row with key "5"
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_i64(5));
    REQUIRE(expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Coercion works: should match exactly one row (key "5")
    verify_query_result(query_result, 1);
  }

  SECTION("Boolean AND on non-boolean expressions (int AND int)") {
    // Build expr: 42 AND 7 (both are i64, not boolean)
    LanceDBExpr* expr = lancedb_expr_and(
        lancedb_expr_literal_i64(42),
        lancedb_expr_literal_i64(7));
    REQUIRE(expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Should fail at execution due to type mismatch
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result == nullptr);
  }

  SECTION("Boolean OR on string expressions") {
    // Build expr: "hello" OR "world" (strings, not boolean)
    LanceDBExpr* expr = lancedb_expr_or(
        lancedb_expr_literal_string("hello"),
        lancedb_expr_literal_string("world"));
    REQUIRE(expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Should fail at execution due to type mismatch
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result == nullptr);
  }

  SECTION("NOT on non-boolean expression (NOT int)") {
    // Build expr: NOT 42 (i64, not boolean)
    LanceDBExpr* expr = lancedb_expr_not(lancedb_expr_literal_i64(42));
    REQUIRE(expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Should fail at execution due to type mismatch
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result == nullptr);
  }

  SECTION("Comparison between string literal and float literal") {
    // Build expr: "hello" > 3.14
    // DataFusion coerces types, so the query succeeds
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_literal_string("hello"),
        LANCEDB_BINARY_OP_GT,
        lancedb_expr_literal_f64(3.14));
    REQUIRE(expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    result = lancedb_query_df_filter(query, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // DataFusion coerces and evaluates this - query succeeds
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);
    lancedb_query_result_free(query_result);
  }

  SECTION("IN list with float literal against vector column") {
    // Build expr: 3.14 IN (data)
    LanceDBExpr* col_expr = lancedb_expr_column("data");
    REQUIRE(col_expr != nullptr);

    char* error_message = nullptr;
    LanceDBExpr* in_expr = lancedb_expr_in_list(
        lancedb_expr_literal_f64(3.14), &col_expr, 1, false, &error_message);
    REQUIRE(in_expr != nullptr);
    REQUIRE(error_message == nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, in_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Should fail at execution — cannot use IN with a vector column
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result == nullptr);
  }

  SECTION("IN list with string column against int literals") {
    // Build expr: key IN (1, 2, 3)
    LanceDBExpr* col_expr = lancedb_expr_column("key");
    LanceDBExpr* list_items[3];
    list_items[0] = lancedb_expr_literal_i64(1);
    list_items[1] = lancedb_expr_literal_i64(2);
    list_items[2] = lancedb_expr_literal_i64(3);

    char* error_message = nullptr;
    LanceDBExpr* in_expr = lancedb_expr_in_list(
        col_expr,
        list_items, 3, false, &error_message);
    REQUIRE(in_expr != nullptr);
    REQUIRE(error_message == nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, in_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // DataFusion coerces ints to strings so query suceeds
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);

    // Import into Arrow RecordBatch to inspect the key value
    auto imported_schema = arrow::ImportSchema(
        reinterpret_cast<ArrowSchema*>(result_schema));
    REQUIRE(imported_schema.ok());

    auto imported_batch = arrow::ImportRecordBatch(
        reinterpret_cast<ArrowArray*>(result_arrays[0]),
        imported_schema.ValueUnsafe());
    REQUIRE(imported_batch.ok());

    auto batch = imported_batch.ValueUnsafe();
    REQUIRE(batch->num_rows() == 3);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("IN list with string literal against string literal (not a list)") {
    // Build expr: "hello" IN ("hello world")
    LanceDBExpr* list_item = lancedb_expr_literal_string("hello world");
    REQUIRE(list_item != nullptr);

    char* error_message = nullptr;
    LanceDBExpr* in_expr = lancedb_expr_in_list(
        lancedb_expr_literal_string("hello"), &list_item, 1, false, &error_message);
    REQUIRE(in_expr != nullptr);
    REQUIRE(error_message == nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, in_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // "hello" != "hello world" no substring matching, just equality
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    // Constant false filter: no rows returned
    REQUIRE(count == 0);
  }

  SECTION("Comparison with boolean sub-expression (int > bool)") {
    // Build expr: 42 > (42 > 7)
    // Inner: 42 > 7 produces a boolean
    // Outer: 42 > <boolean> is a type mismatch (int vs bool)
    LanceDBExpr* inner = lancedb_expr_binary(
        lancedb_expr_literal_i64(42),
        LANCEDB_BINARY_OP_GT,
        lancedb_expr_literal_i64(7));
    REQUIRE(inner != nullptr);

    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_literal_i64(42),
        LANCEDB_BINARY_OP_GT,
        inner);
    REQUIRE(expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_df_filter(query, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Should fail at execution due to type mismatch
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result == nullptr);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - Expr array_has filter", "[query][expr]") {
  // Create a schema with key (utf8) and tags (list<utf8>)
  auto key_field = arrow::field("key", arrow::utf8());
  auto tags_field = arrow::field("tags", arrow::list(arrow::utf8()));
  auto schema = arrow::schema({key_field, tags_field});

  arrow::StringBuilder key_builder;
  auto tags_builder = std::make_shared<arrow::ListBuilder>(
      arrow::default_memory_pool(),
      std::make_shared<arrow::StringBuilder>());

  // Row 0: key="k0", tags=["red", "blue"]
  REQUIRE(key_builder.Append("k0").ok());
  REQUIRE(tags_builder->Append().ok());
  auto* tag_val_builder = static_cast<arrow::StringBuilder*>(tags_builder->value_builder());
  REQUIRE(tag_val_builder->Append("red").ok());
  REQUIRE(tag_val_builder->Append("blue").ok());

  // Row 1: key="k1", tags=["green"]
  REQUIRE(key_builder.Append("k1").ok());
  REQUIRE(tags_builder->Append().ok());
  REQUIRE(tag_val_builder->Append("green").ok());

  // Row 2: key="k2", tags=["red", "green", "blue"]
  REQUIRE(key_builder.Append("k2").ok());
  REQUIRE(tags_builder->Append().ok());
  REQUIRE(tag_val_builder->Append("red").ok());
  REQUIRE(tag_val_builder->Append("green").ok());
  REQUIRE(tag_val_builder->Append("blue").ok());

  // Row 3: key="k3", tags=["yellow"]
  REQUIRE(key_builder.Append("k3").ok());
  REQUIRE(tags_builder->Append().ok());
  REQUIRE(tag_val_builder->Append("yellow").ok());

  std::shared_ptr<arrow::Array> key_array, tags_array;
  REQUIRE(key_builder.Finish(&key_array).ok());
  REQUIRE(tags_builder->Finish(&tags_array).ok());

  auto batch = arrow::RecordBatch::Make(schema, 4, {key_array, tags_array});
  auto reader = create_reader_from_batch(batch);
  REQUIRE(reader != nullptr);

  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create(
      db, "array_has_test",
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      reader, &table, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  SECTION("array_has finds matching rows") {
    // Build expr: array_has(tags, "red") — should match k0, k2
    char* err = nullptr;
    LanceDBExpr* has_expr = lancedb_expr_array_has(
        lancedb_expr_column("tags"),
        lancedb_expr_literal_string("red"),
        &err);
    REQUIRE(has_expr != nullptr);
    REQUIRE(err == nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "tags"};
    result = lancedb_query_select(query, columns, 2, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);
    result = lancedb_query_df_filter(query, has_expr, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);
    REQUIRE(count > 0);

    size_t total_rows = 0;
    for (size_t i = 0; i < count; i++) {
      total_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(total_rows == 2);

    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("array_has with no matches") {
    // Build expr: array_has(tags, "purple") — no rows have "purple"
    char* err = nullptr;
    LanceDBExpr* has_expr = lancedb_expr_array_has(
        lancedb_expr_column("tags"),
        lancedb_expr_literal_string("purple"),
        &err);
    REQUIRE(has_expr != nullptr);
    REQUIRE(err == nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "tags"};
    result = lancedb_query_select(query, columns, 2, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);
    result = lancedb_query_df_filter(query, has_expr, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);
    REQUIRE(count == 0);
  }

  SECTION("array_has combined with AND") {
    // Build expr: array_has(tags, "red") AND array_has(tags, "green")
    // Should match only k2
    char* err = nullptr;
    LanceDBExpr* has_red = lancedb_expr_array_has(
        lancedb_expr_column("tags"),
        lancedb_expr_literal_string("red"),
        &err);
    REQUIRE(has_red != nullptr);

    LanceDBExpr* has_green = lancedb_expr_array_has(
        lancedb_expr_column("tags"),
        lancedb_expr_literal_string("green"),
        &err);
    REQUIRE(has_green != nullptr);

    LanceDBExpr* and_expr = lancedb_expr_and(has_red, has_green);
    REQUIRE(and_expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "tags"};
    result = lancedb_query_select(query, columns, 2, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);
    result = lancedb_query_df_filter(query, and_expr, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);
    REQUIRE(count > 0);

    size_t total_rows = 0;
    for (size_t i = 0; i < count; i++) {
      total_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(total_rows == 1);

    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("array_has on non-array column fails at execution") {
    // key is utf8, not a list — array_has should fail at query execution
    char* err = nullptr;
    LanceDBExpr* has_expr = lancedb_expr_array_has(
        lancedb_expr_column("key"),
        lancedb_expr_literal_string("k2"),
        &err);
    REQUIRE(has_expr != nullptr);
    REQUIRE(err == nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "tags"};
    result = lancedb_query_select(query, columns, 2, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);
    result = lancedb_query_df_filter(query, has_expr, &err);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(err == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result == nullptr);
  }

  SECTION("Null arguments") {
    char* err = nullptr;
    REQUIRE(lancedb_expr_array_has(nullptr, nullptr, &err) == nullptr);
    REQUIRE(err != nullptr);
    lancedb_free_string(err);

    err = nullptr;
    LanceDBExpr* col = lancedb_expr_column("tags");
    REQUIRE(lancedb_expr_array_has(col, nullptr, &err) == nullptr);
    REQUIRE(err != nullptr);
    lancedb_free_string(err);

    err = nullptr;
    LanceDBExpr* val = lancedb_expr_literal_string("red");
    REQUIRE(lancedb_expr_array_has(nullptr, val, &err) == nullptr);
    REQUIRE(err != nullptr);
    lancedb_free_string(err);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - DF filter + JSON post-filter on metadata", "[query][expr][json]") {
  // Create a schema with key (utf8) and metadata (utf8 holding JSON)
  auto key_field = arrow::field("key", arrow::utf8());
  auto meta_field = arrow::field("metadata", arrow::utf8());
  auto schema = arrow::schema({key_field, meta_field});

  // Build rows: k0..k4 with JSON metadata
  arrow::StringBuilder key_builder;
  arrow::StringBuilder meta_builder;

  REQUIRE(key_builder.Append("k0").ok());
  REQUIRE(meta_builder.Append(R"({"color":"red","priority":1})").ok());
  REQUIRE(key_builder.Append("k1").ok());
  REQUIRE(meta_builder.Append(R"({"color":"blue","priority":5})").ok());
  REQUIRE(key_builder.Append("k2").ok());
  REQUIRE(meta_builder.Append(R"({"color":"red","priority":3})").ok());
  REQUIRE(key_builder.Append("k3").ok());
  REQUIRE(meta_builder.Append(R"({"color":"green","priority":2})").ok());
  REQUIRE(key_builder.Append("k4").ok());
  REQUIRE(meta_builder.Append(R"({"color":"blue","priority":4})").ok());

  std::shared_ptr<arrow::Array> key_array, meta_array;
  REQUIRE(key_builder.Finish(&key_array).ok());
  REQUIRE(meta_builder.Finish(&meta_array).ok());

  auto batch = arrow::RecordBatch::Make(schema, 5, {key_array, meta_array});
  auto reader = create_reader_from_batch(batch);
  REQUIRE(reader != nullptr);

  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create(
      db, "df_json_meta_test",
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      reader, &table, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  // Use a DataFusion Expr to query: key >= "k1" AND key <= "k3"
  // This should return k1, k2, k3 (3 rows)
  LanceDBExpr* df_filter = lancedb_expr_binary(
      lancedb_expr_binary(
          lancedb_expr_column("key"),
          LANCEDB_BINARY_OP_GT_EQ,
          lancedb_expr_literal_string("k1")),
      LANCEDB_BINARY_OP_AND,
      lancedb_expr_binary(
          lancedb_expr_column("key"),
          LANCEDB_BINARY_OP_LT_EQ,
          lancedb_expr_literal_string("k3")));
  REQUIRE(df_filter != nullptr);

  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);
  const char* columns[] = {"key", "metadata"};
  result = lancedb_query_select(query, columns, 2, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  result = lancedb_query_df_filter(query, df_filter, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
  REQUIRE(query_result != nullptr);

  // Convert to Arrow
  FFI_ArrowArray** result_arrays = nullptr;
  FFI_ArrowSchema* result_schema = nullptr;
  size_t count = 0;
  result = lancedb_query_result_to_arrow(
      query_result, &result_arrays, &result_schema, &count, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count > 0);

  // JSON post-filter: color = "red" AND priority > 1
  const char* color_path[] = {"color"};
  const char* priority_path[] = {"priority"};
  LanceDBExpr* json_filter = lancedb_expr_binary(
      lancedb_expr_binary(
          lancedb_expr_json_get_str(lancedb_expr_column("metadata"), color_path, 1),
          LANCEDB_BINARY_OP_EQ,
          lancedb_expr_literal_string("red")),
      LANCEDB_BINARY_OP_AND,
      lancedb_expr_binary(
          lancedb_expr_json_get_int(lancedb_expr_column("metadata"), priority_path, 1),
          LANCEDB_BINARY_OP_GT,
          lancedb_expr_literal_i64(1)));
  REQUIRE(json_filter != nullptr);

  bool* json_results = nullptr;
  size_t json_count = 0;
  char* json_err = nullptr;
  result = lancedb_json_matches(
      result_arrays, result_schema, count,
      json_filter, &json_results, &json_count, &json_err);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(json_err == nullptr);
  REQUIRE(json_count == 3);
  // Among k1(blue,5), k2(red,3), k3(green,2) only k2 matches
  size_t matched = 0;
  for (size_t i = 0; i < json_count; i++) {
    if (json_results[i]) matched++;
  }
  REQUIRE(matched == 1);
  lancedb_free_json_matches(json_results);

  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);
  lancedb_table_free(table);
}

