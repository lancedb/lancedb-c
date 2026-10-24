/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <string>
#include <memory>
#include "test_common.h"

// Read the first value of the "data" column of the row with the given key
static float first_data_value(LanceDBTable* table, const std::string& key) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);

  char* error_message = nullptr;
  const auto filter = "key = '" + key + "'";
  REQUIRE(lancedb_query_where_filter(query, filter.c_str(), &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  LanceDBQueryResult* query_result = lancedb_query_execute(query);
  REQUIRE(query_result != nullptr);

  FFI_ArrowArray** result_arrays = nullptr;
  FFI_ArrowSchema* result_schema = nullptr;
  size_t count = 0;
  REQUIRE(lancedb_query_result_to_arrow(
      query_result, &result_arrays, &result_schema, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count == 1);

  auto imported_schema = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(result_schema));
  REQUIRE(imported_schema.ok());
  auto imported_batch = arrow::ImportRecordBatch(
      reinterpret_cast<ArrowArray*>(result_arrays[0]), imported_schema.ValueUnsafe());
  REQUIRE(imported_batch.ok());

  auto batch = imported_batch.ValueUnsafe();
  REQUIRE(batch->num_rows() == 1);
  auto data_array = std::static_pointer_cast<arrow::FixedSizeListArray>(batch->column(1));
  auto values = std::static_pointer_cast<arrow::FloatArray>(data_array->values());
  const auto value = values->Value(data_array->value_offset(0));

  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);

  return value;
}

// Create a reader with rows for keys "key_<start_index>" .. "key_<start_index + num_rows - 1>",
// where all values of the "data" column are set to "value"
static LanceDBRecordBatchReader* create_reader_with_value(int num_rows, int start_index, float value) {
  auto schema = create_test_schema();

  arrow::StringBuilder key_builder;
  arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
      std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

  for (int i = 0; i < num_rows; i++) {
    REQUIRE(key_builder.Append("key_" + std::to_string(start_index + i)).ok());
    auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
      REQUIRE(list_builder->Append(value).ok());
    }
    REQUIRE(data_builder.Append().ok());
  }

  std::shared_ptr<arrow::Array> key_array, data_array;
  REQUIRE(key_builder.Finish(&key_array).ok());
  REQUIRE(data_builder.Finish(&data_array).ok());

  auto batch = arrow::RecordBatch::Make(schema, num_rows, {key_array, data_array});
  return create_reader_from_batch(batch);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Creation", "[table]") {
  SECTION("Create empty table") {
    create_empty_table("empty_table");
  }

  SECTION("Create table with data") {
    constexpr auto row_num = 10;
    LanceDBTable* table = create_table_with_data("table_with_data", row_num, 0);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
    lancedb_table_free(table);
  }

  SECTION("Create table with data then reopen and verify") {
    const std::string table_name = "table_reopen_test";
    constexpr auto row_num = 15;
    LanceDBTable* table = create_table_with_data(table_name, row_num, 0);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
    lancedb_table_free(table);

    // Reopen the table
    LanceDBTable* reopened_table = open_table(table_name);
    REQUIRE(lancedb_table_count_rows(reopened_table) == row_num);
    lancedb_table_free(reopened_table);
  }

  SECTION("Create table with invalid name should fail") {
    auto schema = create_test_schema();
    auto batch = create_test_record_batch(5, 0);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    struct ArrowSchema c_schema;
    REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

    LanceDBTable* table = nullptr;
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_create(
        db,
        "invalid table name",
        reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
        reader,
        &table,
        &error_message
    );

    REQUIRE(result == LANCEDB_INVALID_TABLE_NAME);
    REQUIRE(table == nullptr);

    // Note: Reader was consumed by lancedb_table_create even on failure

    if (error_message) {
      lancedb_free_string(error_message);
    }

    // Opening a table with an invalid name fails as well
    LanceDBTable* invalid_table = nullptr;
    REQUIRE(lancedb_connection_open_table(db, "invalid table name", &invalid_table, &error_message)
        == LANCEDB_INVALID_TABLE_NAME);
    REQUIRE(invalid_table == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    // Clean up schema
    if (c_schema.release) {
      c_schema.release(&c_schema);
    }
  }

  SECTION("Create table that already exists should fail") {
    const std::string table_name = "duplicate_table";

    // First create the table
    LanceDBTable* table = create_table_with_data(table_name, 5, 0);
    lancedb_table_free(table);

    // Try to create the same table again
    auto schema = create_test_schema();
    auto batch = create_test_record_batch(10, 0);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    struct ArrowSchema c_schema;
    REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

    LanceDBTable* table2 = nullptr;
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_create(
        db,
        table_name.c_str(),
        reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
        reader,
        &table2,
        &error_message
    );

    REQUIRE(result == LANCEDB_TABLE_ALREADY_EXISTS);

    if (error_message) {
      lancedb_free_string(error_message);
    }

    // Note: Reader was consumed by lancedb_table_create even on failure

    // Clean up schema
    if (c_schema.release) {
      c_schema.release(&c_schema);
    }
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Add", "[table]") {
  // Create a test table
  const std::string table_name = "test_add_table";
  create_empty_table(table_name);

  // Open the table
  LanceDBTable* table = open_table(table_name);

  SECTION("Add data to empty table") {
    // Verify table is initially empty
    REQUIRE(lancedb_table_count_rows(table) == 0);

    // Initial version should be 1 (empty table)
    auto version = lancedb_table_version(table);
    REQUIRE(version == 1);

    constexpr auto row_num = 10;

    // Create and add a batch of data
    auto batch = create_test_record_batch(row_num, 0);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_add(table, reader, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Verify row count
    REQUIRE(lancedb_table_count_rows(table) == row_num);

    // Version should increment to 2
    version = lancedb_table_version(table);
    REQUIRE(version == 2);
  }

  SECTION("Add multiple batches of data") {
    // Initial version should be 1 (empty table)
    auto version = lancedb_table_version(table);
    REQUIRE(version == 1);

    // Add first batch
    constexpr auto row_num1 = 5;
    auto batch1 = create_test_record_batch(row_num1, 0);
    auto reader1 = create_reader_from_batch(batch1);
    REQUIRE(reader1 != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_add(table, reader1, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num1);

    // Version should increment to 2
    version = lancedb_table_version(table);
    REQUIRE(version == 2);

    // Add second batch
    constexpr auto row_num2 = 7;
    auto batch2 = create_test_record_batch(row_num2, row_num1);
    auto reader2 = create_reader_from_batch(batch2);
    REQUIRE(reader2 != nullptr);

    result = lancedb_table_add(table, reader2, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num1+row_num2);

    // Version should increment to 3
    version = lancedb_table_version(table);
    REQUIRE(version == 3);
  }

  SECTION("Add data with duplicate keys creates duplicate rows") {
    // Add initial data with keys 0-9
    constexpr auto row_num = 10;
    auto batch1 = create_test_record_batch(row_num, 0);
    auto reader1 = create_reader_from_batch(batch1);
    REQUIRE(reader1 != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_add(table, reader1, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num);

    // Add data with overlapping keys (5-14)
    // Keys 5-9 already exist in the table
    constexpr auto overlap_start = 5;
    constexpr auto overlap_count = 10;
    auto batch2 = create_test_record_batch(overlap_count, overlap_start);
    auto reader2 = create_reader_from_batch(batch2);
    REQUIRE(reader2 != nullptr);

    result = lancedb_table_add(table, reader2, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // table_add adds all rows
    // So we should have 10 (original) + 10 (new batch) = 20 rows
    // Even though keys 5-9 exist in both batches
    REQUIRE(lancedb_table_count_rows(table) == 20);

    // Version should increment
    auto version = lancedb_table_version(table);
    REQUIRE(version == 3);
  }

  SECTION("Add data with null reader should fail") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_add(table, nullptr, &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);

    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  SECTION("Add data to null table should fail") {
    auto batch = create_test_record_batch(5, 0);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_add(nullptr, reader, &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);

    // Reader was not consumed due to error, must free it
    lancedb_record_batch_reader_free(reader);

    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Merge Insert", "[table]") {
  // Create a test table with initial data
  const std::string table_name = "test_merge_table";
  create_empty_table(table_name);

  // Open the table
  LanceDBTable* table = open_table(table_name);

  // Add initial data
  constexpr auto row_num = 10;
  auto initial_batch = create_test_record_batch(row_num, 0);
  auto initial_reader = create_reader_from_batch(initial_batch);
  REQUIRE(initial_reader != nullptr);

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_add(table, initial_reader, &error_message);

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == row_num);

  // Initial version after add should be 2 (1 for empty table creation, 2 after add)
  auto version = lancedb_table_version(table);
  REQUIRE(version == 2);

  SECTION("Merge insert with update and insert") {
    // Create data with some overlapping keys (0-4) and some new keys (10-14)
    auto schema = create_test_schema();

    arrow::StringBuilder key_builder;
    arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
        std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

    // Add overlapping keys (should update)
    for (int i = 0; i < 5; i++) {
      REQUIRE(key_builder.Append("key_" + std::to_string(i)).ok());
      auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        REQUIRE(list_builder->Append(static_cast<float>(999 + i)).ok());  // Different values
      }
      REQUIRE(data_builder.Append().ok());
    }

    // Add new keys (should insert)
    for (int i = 10; i < 15; i++) {
      REQUIRE(key_builder.Append("key_" + std::to_string(i)).ok());
      auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        REQUIRE(list_builder->Append(static_cast<float>(i * 10 + j)).ok());
      }
      REQUIRE(data_builder.Append().ok());
    }

    std::shared_ptr<arrow::Array> key_array, data_array;
    REQUIRE(key_builder.Finish(&key_array).ok());
    REQUIRE(data_builder.Finish(&data_array).ok());

    auto merge_batch = arrow::RecordBatch::Make(schema, 10, {key_array, data_array});
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Should have 10 (original) - 5 (overlapping) + 10 (total in merge) = 15 rows
    REQUIRE(lancedb_table_count_rows(table) == 15);

    // Version should increment to 3 (was 2 before merge insert)
    auto version = lancedb_table_version(table);
    REQUIRE(version == 3);
  }

  SECTION("Merge insert with update only") {
    // Create data with only overlapping keys
    auto schema = create_test_schema();

    arrow::StringBuilder key_builder;
    arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
        std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

    for (int i = 0; i < 5; i++) {
      REQUIRE(key_builder.Append("key_" + std::to_string(i)).ok());
      auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        REQUIRE(list_builder->Append(static_cast<float>(888 + i)).ok());
      }
      REQUIRE(data_builder.Append().ok());
    }

    std::shared_ptr<arrow::Array> key_array, data_array;
    REQUIRE(key_builder.Finish(&key_array).ok());
    REQUIRE(data_builder.Finish(&data_array).ok());

    auto merge_batch = arrow::RecordBatch::Make(schema, 5, {key_array, data_array});
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0,  // Don't insert new rows
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Should still have 10 rows (only updates, no inserts)
    REQUIRE(lancedb_table_count_rows(table) == 10);

    // Version should increment to 3 (was 2 before merge insert)
    auto version = lancedb_table_version(table);
    REQUIRE(version == 3);
  }

  SECTION("Merge insert with SQL condition") {
    // Merge keys 0-4 with a new value, but update only key_0
    auto merge_reader = create_reader_with_value(5, 0, 888.0F);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0,
      .when_matched_update_all_condition = "target.key = 'key_0'",
      .when_matched_update_all_expr = nullptr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(lancedb_table_count_rows(table) == 10);
    // Only the row satisfying the condition was updated
    REQUIRE(first_data_value(table, "key_0") == 888.0F);
    REQUIRE(first_data_value(table, "key_1") == 10.0F);
  }

  SECTION("Merge insert with DataFusion expression condition") {
    // Merge keys 0-4 with a new value, but update only key_1
    auto merge_reader = create_reader_with_value(5, 0, 888.0F);
    REQUIRE(merge_reader != nullptr);

    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("target.key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_1"));
    REQUIRE(expr != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = expr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // The expression is not consumed by the merge insert
    lancedb_expr_free(expr);

    REQUIRE(lancedb_table_count_rows(table) == 10);
    // Only the row satisfying the condition was updated
    REQUIRE(first_data_value(table, "key_1") == 888.0F);
    REQUIRE(first_data_value(table, "key_0") == 0.0F);
  }

  SECTION("Merge insert with both SQL and DataFusion conditions should fail") {
    auto merge_reader = create_reader_with_value(5, 0, 888.0F);
    REQUIRE(merge_reader != nullptr);

    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("target.key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_1"));
    REQUIRE(expr != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0,
      .when_matched_update_all_condition = "target.key = 'key_0'",
      .when_matched_update_all_expr = expr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);

    // Nothing was merged, and no row was updated
    REQUIRE(lancedb_table_count_rows(table) == 10);
    REQUIRE(first_data_value(table, "key_0") == 0.0F);
    REQUIRE(first_data_value(table, "key_1") == 10.0F);

    // Note: Reader was consumed by lancedb_table_merge_insert even on failure
    lancedb_expr_free(expr);

    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  SECTION("Merge insert conditions are ignored when matched records are not updated") {
    // Merge keys 0-4 (existing) and 10-14 (new)
    auto merge_reader = create_reader_with_value(15, 0, 888.0F);
    REQUIRE(merge_reader != nullptr);

    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("target.key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_1"));
    REQUIRE(expr != nullptr);

    // Both conditions are set, which is not rejected since they are not used
    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 0,
      .when_not_matched_insert_all = 1,
      .when_matched_update_all_condition = "target.key = 'key_0'",
      .when_matched_update_all_expr = expr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    lancedb_expr_free(expr);

    // New rows were inserted, and no row was updated
    REQUIRE(lancedb_table_count_rows(table) == 15);
    REQUIRE(first_data_value(table, "key_0") == 0.0F);
  }

  SECTION("Merge insert with invalid SQL condition should fail") {
    auto merge_reader = create_reader_with_value(5, 0, 888.0F);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0,
      .when_matched_update_all_condition = "target.no_such_column = 42",
      .when_matched_update_all_expr = nullptr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(lancedb_table_count_rows(table) == 10);

    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  SECTION("Merge insert with insert only") {
    // Create data with only new keys
    auto schema = create_test_schema();

    arrow::StringBuilder key_builder;
    arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
        std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

    for (int i = 20; i < 25; i++) {
      REQUIRE(key_builder.Append("key_" + std::to_string(i)).ok());
      auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        REQUIRE(list_builder->Append(static_cast<float>(i * 10 + j)).ok());
      }
      REQUIRE(data_builder.Append().ok());
    }

    std::shared_ptr<arrow::Array> key_array, data_array;
    REQUIRE(key_builder.Finish(&key_array).ok());
    REQUIRE(data_builder.Finish(&data_array).ok());

    auto merge_batch = arrow::RecordBatch::Make(schema, 5, {key_array, data_array});
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 0,  // Don't update existing rows
      .when_not_matched_insert_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Should have 10 + 5 = 15 rows (only inserts, no updates)
    REQUIRE(lancedb_table_count_rows(table) == 15);

    // Version should increment to 3 (was 2 before merge insert)
    auto version = lancedb_table_version(table);
    REQUIRE(version == 3);
  }

  SECTION("Merge insert with matching rows but no update") {
    // All merged keys (0-4) match existing rows, but updating them is disabled
    auto merge_reader = create_reader_with_value(5, 0, 888.0F);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 0,  // Don't update existing rows
      .when_not_matched_insert_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Nothing to insert, and matched rows keep their original values
    REQUIRE(lancedb_table_count_rows(table) == 10);
    REQUIRE(first_data_value(table, "key_0") == 0.0F);
    REQUIRE(first_data_value(table, "key_4") == 40.0F);

    // Version increments to 3 (was 2 before merge insert) even though nothing changed
    auto version = lancedb_table_version(table);
    REQUIRE(version == 3);
  }

  SECTION("Merge insert with null config uses defaults") {
    auto merge_batch = create_test_record_batch(3, 0);
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, nullptr, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Default behavior should handle the merge
    REQUIRE(lancedb_table_count_rows(table) >= 10);

    // Version should increment to 3 (was 2 before merge insert)
    auto version = lancedb_table_version(table);
    REQUIRE(version == 3);
  }

  SECTION("Merge insert with no actual changes") {
    // Get current version
    auto version = lancedb_table_version(table);
    REQUIRE(version == 2);

    // Create data with same keys and same values as existing data
    auto schema = create_test_schema();

    arrow::StringBuilder key_builder;
    arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
        std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

    // Use exact same data as initial batch (keys 0-4)
    for (int i = 0; i < 5; i++) {
      REQUIRE(key_builder.Append("key_" + std::to_string(i)).ok());
      auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        REQUIRE(list_builder->Append(static_cast<float>(i * 10 + j)).ok());
      }
      REQUIRE(data_builder.Append().ok());
    }

    std::shared_ptr<arrow::Array> key_array, data_array;
    REQUIRE(key_builder.Finish(&key_array).ok());
    REQUIRE(data_builder.Finish(&data_array).ok());

    auto merge_batch = arrow::RecordBatch::Make(schema, 5, {key_array, data_array});
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Row count should remain 10 (no new rows)
    REQUIRE(lancedb_table_count_rows(table) == 10);

    // Check if version changed even though data is identical
    version = lancedb_table_version(table);
    // Version increments even if data doesn't actually change
    REQUIRE(version == 3);
  }

  SECTION("Merge insert with null reader should fail") {
    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, nullptr, on_columns, 1, &config, &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);

    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  SECTION("Merge insert with null table should fail") {
    auto merge_batch = create_test_record_batch(3, 0);
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        nullptr, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);

    // Note: Reader was consumed by lancedb_table_merge_insert even on failure

    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  SECTION("Merge insert with null on_columns should fail") {
    auto merge_batch = create_test_record_batch(3, 0);
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, nullptr, 1, &config, &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);

    // Note: Reader was consumed by lancedb_table_merge_insert even on failure

    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  lancedb_table_free(table);
}

// Schema of a table holding a version per key, to be used with conditions
// comparing the merged data with the data already in the table
static std::shared_ptr<arrow::Schema> create_versioned_schema() {
  return arrow::schema({arrow::field("key", arrow::utf8()),
                        arrow::field("version", arrow::int32())});
}

// Create a reader with rows for keys "key_0" .. "key_<num_rows - 1>",
// where the version of each row is taken from "versions"
static LanceDBRecordBatchReader* create_versioned_reader(const std::vector<int32_t>& versions) {
  arrow::StringBuilder key_builder;
  arrow::Int32Builder version_builder;

  for (size_t i = 0; i < versions.size(); i++) {
    REQUIRE(key_builder.Append("key_" + std::to_string(i)).ok());
    REQUIRE(version_builder.Append(versions[i]).ok());
  }

  std::shared_ptr<arrow::Array> key_array, version_array;
  REQUIRE(key_builder.Finish(&key_array).ok());
  REQUIRE(version_builder.Finish(&version_array).ok());

  auto batch = arrow::RecordBatch::Make(create_versioned_schema(),
      static_cast<int64_t>(versions.size()), {key_array, version_array});
  return create_reader_from_batch(batch);
}

// Read the "version" column of the row with the given key
static int32_t version_value(LanceDBTable* table, const std::string& key) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);

  char* error_message = nullptr;
  const auto filter = "key = '" + key + "'";
  REQUIRE(lancedb_query_where_filter(query, filter.c_str(), &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  LanceDBQueryResult* query_result = lancedb_query_execute(query);
  REQUIRE(query_result != nullptr);

  FFI_ArrowArray** result_arrays = nullptr;
  FFI_ArrowSchema* result_schema = nullptr;
  size_t count = 0;
  REQUIRE(lancedb_query_result_to_arrow(
      query_result, &result_arrays, &result_schema, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count == 1);

  auto imported_schema = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(result_schema));
  REQUIRE(imported_schema.ok());
  auto imported_batch = arrow::ImportRecordBatch(
      reinterpret_cast<ArrowArray*>(result_arrays[0]), imported_schema.ValueUnsafe());
  REQUIRE(imported_batch.ok());

  auto batch = imported_batch.ValueUnsafe();
  REQUIRE(batch->num_rows() == 1);
  auto version_array = std::static_pointer_cast<arrow::Int32Array>(batch->column(1));
  const auto value = version_array->Value(0);

  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);

  return value;
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Merge Insert with source condition", "[table]") {
  const std::string table_name = "test_merge_insert_source_table";

  // Create a table where all 5 keys are at version 10
  auto schema = create_versioned_schema();
  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_create(
      db, table_name.c_str(), reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      create_versioned_reader({10, 10, 10, 10, 10}), &table, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  // Merge newer versions for key_0 and key_1, and older ones for the rest
  const std::vector<int32_t> merged_versions = {11, 12, 9, 8, 10};
  const char* on_columns[] = {"key"};

  SECTION("Update only rows where the merged version is newer, using an SQL condition") {
    auto merge_reader = create_versioned_reader(merged_versions);
    REQUIRE(merge_reader != nullptr);

    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0,
      .when_matched_update_all_condition = "target.version < source.version",
      .when_matched_update_all_expr = nullptr
    };

    error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Only the rows with a newer version in the merged data were updated
    REQUIRE(version_value(table, "key_0") == 11);
    REQUIRE(version_value(table, "key_1") == 12);
    REQUIRE(version_value(table, "key_2") == 10);
    REQUIRE(version_value(table, "key_3") == 10);
    REQUIRE(version_value(table, "key_4") == 10);
  }

  SECTION("Update only rows where the merged version is newer, using a DataFusion expression") {
    auto merge_reader = create_versioned_reader(merged_versions);
    REQUIRE(merge_reader != nullptr);

    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("target.version"),
        LANCEDB_BINARY_OP_LT,
        lancedb_expr_column("source.version"));
    REQUIRE(expr != nullptr);

    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = expr
    };

    error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    lancedb_expr_free(expr);

    // Only the rows with a newer version in the merged data were updated
    REQUIRE(version_value(table, "key_0") == 11);
    REQUIRE(version_value(table, "key_1") == 12);
    REQUIRE(version_value(table, "key_2") == 10);
    REQUIRE(version_value(table, "key_3") == 10);
    REQUIRE(version_value(table, "key_4") == 10);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Delete", "[table]") {
  const std::string table_name = "test_delete_table";
  create_empty_table(table_name);

  LanceDBTable* table = open_table(table_name);

  // Add initial data (keys key_0 through key_9)
  constexpr auto row_num = 20;
  auto initial_batch = create_test_record_batch(row_num, 0);
  auto initial_reader = create_reader_from_batch(initial_batch);
  REQUIRE(initial_reader != nullptr);

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_add(table, initial_reader, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == row_num);

  SECTION("Delete row matching predicate") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_delete(table, "key = 'key_0'", &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 1);
  }

  SECTION("Delete multiple rows matching predicate") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_delete(
        table, "key IN ('key_0', 'key_1', 'key_2')", &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 3);

    result = lancedb_table_delete(
        table, "key = 'key_10' OR key = 'key_11' OR key = 'key_12')", &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 6);
  }

  SECTION("Delete with predicate matching no rows") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_delete(
        table, "key = 'nonexistent'", &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
  }

  SECTION("Delete all rows") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_delete(
        table, "key IS NOT NULL", &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == 0);
  }

  SECTION("Delete with unknown column should fail") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_delete(
        table, "unknown = 'key_0'", &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Delete with empty predicate should fail") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_delete(
        table, "", &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Delete with null table should fail") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_delete(
        nullptr, "key = 'key_0'", &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Delete with null predicate should fail") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_delete(
        table, nullptr, &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table DF Delete", "[table]") {
  const std::string table_name = "test_df_delete_table";
  create_empty_table(table_name);

  LanceDBTable* table = open_table(table_name);

  constexpr auto row_num = 20;
  auto initial_batch = create_test_record_batch(row_num, 0);
  auto initial_reader = create_reader_from_batch(initial_batch);
  REQUIRE(initial_reader != nullptr);

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_add(table, initial_reader, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == row_num);

  SECTION("Delete row matching expression") {
    char* error_message = nullptr;
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_0"));
    REQUIRE(expr != nullptr);

    LanceDBError result = lancedb_table_df_delete(table, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 1);
  }

  SECTION("Delete multiple rows with IN list") {
    char* error_message = nullptr;
    LanceDBExpr* col_expr = lancedb_expr_column("key");
    LanceDBExpr* list_items[3];
    list_items[0] = lancedb_expr_literal_string("key_0");
    list_items[1] = lancedb_expr_literal_string("key_1");
    list_items[2] = lancedb_expr_literal_string("key_2");

    LanceDBExpr* in_expr = lancedb_expr_in_list(col_expr, list_items, 3, false, &error_message);
    REQUIRE(in_expr != nullptr);
    REQUIRE(error_message == nullptr);

    LanceDBError result = lancedb_table_df_delete(table, in_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 3);
  }

  SECTION("Delete multiple rows with OR expression") {
    char* error_message = nullptr;
    LanceDBExpr* or_expr = lancedb_expr_or(
        lancedb_expr_binary(
            lancedb_expr_column("key"),
            LANCEDB_BINARY_OP_EQ,
            lancedb_expr_literal_string("key_10")),
        lancedb_expr_binary(
            lancedb_expr_column("key"),
            LANCEDB_BINARY_OP_EQ,
            lancedb_expr_literal_string("key_11")));
    REQUIRE(or_expr != nullptr);

    LanceDBError result = lancedb_table_df_delete(table, or_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 2);
  }

  SECTION("Delete with expression matching no rows") {
    char* error_message = nullptr;
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("nonexistent"));
    REQUIRE(expr != nullptr);

    LanceDBError result = lancedb_table_df_delete(table, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
  }

  SECTION("Delete all rows with IS NOT NULL") {
    char* error_message = nullptr;
    LanceDBExpr* expr = lancedb_expr_is_not_null(lancedb_expr_column("key"));
    REQUIRE(expr != nullptr);

    LanceDBError result = lancedb_table_df_delete(table, expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == 0);
  }

  SECTION("Delete with null table should fail") {
    char* error_message = nullptr;
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_0"));
    REQUIRE(expr != nullptr);

    LanceDBError result = lancedb_table_df_delete(nullptr, expr, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Delete with null expr should fail") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_df_delete(table, nullptr, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Create Reader", "[table]") {
  constexpr auto row_num = 10;
  auto batch = create_test_record_batch(row_num, 0);

  SECTION("Successfully create reader") {
    struct ArrowArray c_array;
    struct ArrowSchema c_schema;

    REQUIRE(arrow::ExportRecordBatch(*batch, &c_array, &c_schema).ok());

    LanceDBRecordBatchReader* reader = nullptr;
    char* error_message = nullptr;
    // We expect success here
    LanceDBError result = lancedb_record_batch_reader_from_arrow(
      reinterpret_cast<FFI_ArrowArray*>(&c_array),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      &reader,
      &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(reader);
    REQUIRE(error_message == nullptr);

    // According to docs/code: Schema is only read by the function, so we must release it.
    // The array IS CONSUMED by definition of the function (though technically checked early before consumption, if success occurs, array is consumed).
    if (c_schema.release) {
      c_schema.release(&c_schema);
    }

    // We must free the reader
    lancedb_record_batch_reader_free(reader);
  }

  SECTION("Create reader with null array") {
    struct ArrowArray c_array;
    struct ArrowSchema c_schema;

    REQUIRE(arrow::ExportRecordBatch(*batch, &c_array, &c_schema).ok());

    LanceDBRecordBatchReader* reader = nullptr;
    char* error_message = nullptr;
    LanceDBError result = lancedb_record_batch_reader_from_arrow(
      nullptr,
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      &reader,
      &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(reader == nullptr);
    REQUIRE(error_message);

    lancedb_free_string(error_message);

    // Since it failed early (null check), array was not consumed.
    if (c_array.release) {
      c_array.release(&c_array);
    }
    if (c_schema.release) {
      c_schema.release(&c_schema);
    }
  }

  SECTION("Create reader with null schema") {
    struct ArrowArray c_array;
    struct ArrowSchema c_schema;

    REQUIRE(arrow::ExportRecordBatch(*batch, &c_array, &c_schema).ok());

    LanceDBRecordBatchReader* reader = nullptr;
    char* error_message = nullptr;
    LanceDBError result = lancedb_record_batch_reader_from_arrow(
      reinterpret_cast<FFI_ArrowArray*>(&c_array),
      nullptr,
      &reader,
      &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(reader == nullptr);
    REQUIRE(error_message);

    lancedb_free_string(error_message);

    // Since it failed early, array was not consumed.
    if (c_array.release) {
      c_array.release(&c_array);
    }
    if (c_schema.release) {
      c_schema.release(&c_schema);
    }
  }

  SECTION("Create reader with null output pointer") {
    struct ArrowArray c_array;
    struct ArrowSchema c_schema;

    REQUIRE(arrow::ExportRecordBatch(*batch, &c_array, &c_schema).ok());

    char* error_message = nullptr;
    LanceDBError result = lancedb_record_batch_reader_from_arrow(
      reinterpret_cast<FFI_ArrowArray*>(&c_array),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      nullptr,
      &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message);

    lancedb_free_string(error_message);

    // Since it failed early, array was not consumed.
    if (c_array.release) {
      c_array.release(&c_array);
    }
    if (c_schema.release) {
      c_schema.release(&c_schema);
    }
  }
}
TEST_CASE_METHOD(LanceDBSessionFixture, "LanceDB Table CRUD with same session across multiple tables", "[table][session]") {
  const char* _namespace = nullptr;
  const std::string table_a_name = "session_table_a";
  const std::string table_b_name = "session_table_b";
  constexpr auto table_a_rows = 10;
  constexpr auto table_b_rows = 15;

  // Create
  LanceDBTable* table_a = create_table_with_data(table_a_name, table_a_rows, 0);
  LanceDBTable* table_b = create_table_with_data(table_b_name, table_b_rows, 0);
  REQUIRE(table_a != nullptr);
  REQUIRE(table_b != nullptr);

  // Read after create
  REQUIRE(lancedb_table_count_rows(table_a) == table_a_rows);
  REQUIRE(lancedb_table_count_rows(table_b) == table_b_rows);

  // Reopen and read again
  lancedb_table_free(table_a);
  lancedb_table_free(table_b);
  table_a = open_table(table_a_name);
  table_b = open_table(table_b_name);
  REQUIRE(lancedb_table_count_rows(table_a) == table_a_rows);
  REQUIRE(lancedb_table_count_rows(table_b) == table_b_rows);

  // Delete
  lancedb_table_free(table_a);
  lancedb_table_free(table_b);
  table_a = nullptr;
  table_b = nullptr;
  char* error_message = nullptr;
  LanceDBError result = lancedb_connection_drop_table(db, table_a_name.c_str(), _namespace, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  result = lancedb_connection_drop_table(db, table_b_name.c_str(), _namespace, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  // Verify delete
  REQUIRE(lancedb_connection_open_table(db, table_a_name.c_str(), &table_a, &error_message)
      == LANCEDB_TABLE_NOT_FOUND);
  REQUIRE(error_message != nullptr);
  lancedb_free_string(error_message);
  error_message = nullptr;
  REQUIRE(table_a == nullptr);

  REQUIRE(lancedb_connection_open_table(db, table_b_name.c_str(), &table_b, &error_message)
      == LANCEDB_TABLE_NOT_FOUND);
  REQUIRE(error_message != nullptr);
  lancedb_free_string(error_message);
  REQUIRE(table_b == nullptr);
}

// A string that is not valid UTF-8, to exercise the string conversion failures
static const char* const INVALID_UTF8_STR = "\xff\xfe";

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table - invalid arguments", "[table]") {
  const std::string table_name = "table_invalid_args_test";
  LanceDBTable* table = create_table_with_data(table_name, 10, 0);
  REQUIRE(table != nullptr);

  SECTION("Table accessors with null table") {
    REQUIRE(lancedb_table_version(nullptr) == 0);
    REQUIRE(lancedb_table_count_rows(nullptr) == 0);
    REQUIRE(lancedb_table_delete(nullptr, "key = 'key_0'", nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_delete(table, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_delete(table, INVALID_UTF8_STR, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Merge insert with invalid column names should fail") {
    const char* null_columns[] = {nullptr};
    const char* invalid_columns[] = {INVALID_UTF8_STR};

    auto reader = create_reader_from_batch(create_test_record_batch(3, 0));
    REQUIRE(reader != nullptr);
    REQUIRE(lancedb_table_merge_insert(
        table, reader, null_columns, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);

    reader = create_reader_from_batch(create_test_record_batch(3, 0));
    REQUIRE(reader != nullptr);
    REQUIRE(lancedb_table_merge_insert(
        table, reader, invalid_columns, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Merge insert with an invalid condition string should fail") {
    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0,
      .when_matched_update_all_condition = INVALID_UTF8_STR,
      .when_matched_update_all_expr = nullptr
    };

    auto reader = create_reader_from_batch(create_test_record_batch(3, 0));
    REQUIRE(reader != nullptr);
    REQUIRE(lancedb_table_merge_insert(
        table, reader, on_columns, 1, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Vector search with invalid arguments should fail") {
    const std::vector<float> vector(TEST_SCHEMA_DIMENSIONS, 1.0F);
    FFI_ArrowArray** arrays = nullptr;
    FFI_ArrowSchema* schema = nullptr;
    size_t count = 0;

    REQUIRE(lancedb_table_nearest_to(
        nullptr, vector.data(), vector.size(), 5, "data", &arrays, &schema, &count, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_nearest_to(
        table, nullptr, vector.size(), 5, "data", &arrays, &schema, &count, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_nearest_to(
        table, vector.data(), 0, 5, "data", &arrays, &schema, &count, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_nearest_to(
        table, vector.data(), vector.size(), 0, "data", &arrays, &schema, &count, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_nearest_to(
        table, vector.data(), vector.size(), 5, "data", nullptr, &schema, &count, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_nearest_to(
        table, vector.data(), vector.size(), 5, "data", &arrays, nullptr, &count, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_nearest_to(
        table, vector.data(), vector.size(), 5, "data", &arrays, &schema, nullptr, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Vector search on an empty table returns no results") {
    const std::string empty_name = "empty_nearest_to_test";
    create_empty_table(empty_name);
    LanceDBTable* empty_table = open_table(empty_name);

    const std::vector<float> vector(TEST_SCHEMA_DIMENSIONS, 1.0F);
    FFI_ArrowArray** arrays = nullptr;
    FFI_ArrowSchema* schema = nullptr;
    size_t count = 0;
    char* error_message = nullptr;

    REQUIRE(lancedb_table_nearest_to(
        empty_table, vector.data(), vector.size(), 5, "data",
        &arrays, &schema, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 0);
    REQUIRE(arrays == nullptr);
    REQUIRE(schema == nullptr);

    lancedb_table_free(empty_table);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table - error reporting", "[table]") {
  const std::string table_name = "table_error_reporting_test";
  LanceDBTable* table = create_table_with_data(table_name, 10, 0);
  REQUIRE(table != nullptr);

  SECTION("Failures without an error message pointer are reported by the error code") {
    // The failure is reported even when the caller is not interested in the message
    REQUIRE(lancedb_table_delete(table, "this is not a predicate", nullptr) != LANCEDB_SUCCESS);
  }

  SECTION("Adding data with a mismatched schema should fail") {
    auto schema = arrow::schema({arrow::field("other", arrow::int32())});
    arrow::Int32Builder builder;
    REQUIRE(builder.Append(1).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());
    auto batch = arrow::RecordBatch::Make(schema, 1, {array});

    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_table_add(table, reader, &error_message) != LANCEDB_SUCCESS);
    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  lancedb_table_free(table);
}
