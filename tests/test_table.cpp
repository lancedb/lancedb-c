/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <string>
#include <memory>
#include <utility>
#include "test_common.h"

struct RunOnStackCtx {
  LanceDBTable* table;
  LanceDBError result;
  char* error_message;
};

extern "C" void merge_insert_on_stack(void* p) {
  auto* c = static_cast<RunOnStackCtx*>(p);
  try {
    auto schema = create_test_schema();
    arrow::StringBuilder key_builder;
    arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
        std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);
    for (int i = 0; i < 5; i++) {
      key_builder.Append("key_" + std::to_string(i + 10)).ok();
      auto* fb = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        fb->Append(static_cast<float>(i)).ok();
      }
      data_builder.Append().ok();
    }
    std::shared_ptr<arrow::Array> key_array, data_array;
    key_builder.Finish(&key_array).ok();
    data_builder.Finish(&data_array).ok();

    auto batch = arrow::RecordBatch::Make(schema, 5, {key_array, data_array});
    auto reader = create_reader_from_batch(batch);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr
    };
    c->result = lancedb_table_merge_insert(
        c->table, reader, on_columns, 1, &config, &c->error_message);
  } catch (...) {
    c->result = LANCEDB_UNKNOWN;
  }
}

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

extern "C" void sql_delete_on_stack(void* p) {
  auto* c = static_cast<RunOnStackCtx*>(p);
  try {
    c->result = lancedb_table_delete(c->table, "key = 'key_0'", &c->error_message);
  } catch (...) {
    c->result = LANCEDB_UNKNOWN;
  }
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table - status-returning count and version", "[table]") {
  const std::string table_name = "test_table_status_getters";
  create_empty_table(table_name);
  LanceDBTable* table = open_table(table_name);

  SECTION("Empty table count succeeds with zero rows") {
    size_t count = 42;
    char* error_message = nullptr;

    REQUIRE(lancedb_table_count_rows_with_status(table, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 0);

    // The old getter keeps its zero sentinel for an empty table.
    REQUIRE(lancedb_table_count_rows(table) == 0);
  }

  SECTION("Version succeeds and matches the old getter") {
    uint64_t version = 0;
    char* error_message = nullptr;

    REQUIRE(lancedb_table_version_with_status(table, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == lancedb_table_version(table));
    REQUIRE(version == 1);
  }

  SECTION("Count and version reflect a write") {
    constexpr auto row_num = 4;
    auto reader = create_reader_from_batch(create_test_record_batch(row_num, 0));
    REQUIRE(reader != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    size_t count = 0;
    REQUIRE(lancedb_table_count_rows_with_status(table, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == row_num);

    uint64_t version = 0;
    REQUIRE(lancedb_table_version_with_status(table, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == 2);
    REQUIRE(version == lancedb_table_version(table));
  }

  SECTION("Failures are reported and leave the output unchanged") {
    size_t count = 42;
    uint64_t version = 42;
    char* error_message = nullptr;

    REQUIRE(lancedb_table_count_rows_with_status(nullptr, &count, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    REQUIRE(count == 42);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_version_with_status(nullptr, &version, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    REQUIRE(version == 42);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // A missing output pointer is rejected instead of writing through it.
    REQUIRE(lancedb_table_count_rows_with_status(table, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_version_with_status(table, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    // The old getters keep their zero sentinel for a rejected call.
    REQUIRE(lancedb_table_count_rows(nullptr) == 0);
    REQUIRE(lancedb_table_version(nullptr) == 0);
  }

  lancedb_table_free(table);
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Update", "[table]") {
  const std::string table_name = "test_update_table";
  auto schema = create_versioned_schema();
  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_create(
      db, table_name.c_str(), reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      create_versioned_reader({10, 10, 10}), &table, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  const auto initial_version = lancedb_table_version(table);

  SECTION("Update only rows matching the predicate") {
    const char* columns[] = {"version"};
    const char* expressions[] = {"version + 5"};
    LanceDBUpdateResult update_result = {};

    REQUIRE(lancedb_table_update(
        table, columns, expressions, 1, "key = 'key_1'", &update_result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(update_result.rows_updated == 1);
    REQUIRE(update_result.version == initial_version + 1);
    REQUIRE(update_result.version == lancedb_table_version(table));

    REQUIRE(version_value(table, "key_0") == 10);
    REQUIRE(version_value(table, "key_1") == 15);
    REQUIRE(version_value(table, "key_2") == 10);
  }

  SECTION("Update all rows with multiple assignments") {
    const char* columns[] = {"key", "version"};
    const char* expressions[] = {"upper(key)", "version + 1"};
    LanceDBUpdateResult update_result = {};

    REQUIRE(lancedb_table_update(
        table, columns, expressions, 2, nullptr, &update_result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(update_result.rows_updated == 3);
    REQUIRE(update_result.version == initial_version + 1);
    REQUIRE(update_result.version == lancedb_table_version(table));

    REQUIRE(version_value(table, "KEY_0") == 11);
    REQUIRE(version_value(table, "KEY_1") == 11);
    REQUIRE(version_value(table, "KEY_2") == 11);
  }

  SECTION("Empty predicate updates every row") {
    const char* columns[] = {"version"};
    const char* expressions[] = {"version + 2"};
    LanceDBUpdateResult update_result = {};

    REQUIRE(lancedb_table_update(
        table, columns, expressions, 1, "", &update_result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(update_result.rows_updated == 3);
    REQUIRE(update_result.version == initial_version + 1);

    REQUIRE(version_value(table, "key_0") == 12);
    REQUIRE(version_value(table, "key_1") == 12);
    REQUIRE(version_value(table, "key_2") == 12);
  }

  SECTION("Invalid expression preserves the SDK error") {
    const char* columns[] = {"version"};
    const char* expressions[] = {"no_such_column + 1"};
    LanceDBUpdateResult update_result = {};

    REQUIRE(lancedb_table_update(
        table, columns, expressions, 1, "key = 'key_1'", &update_result, &error_message)
        != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("no_such_column") != std::string::npos);
    lancedb_free_string(error_message);

    REQUIRE(lancedb_table_version(table) == initial_version);
    REQUIRE(version_value(table, "key_0") == 10);
    REQUIRE(version_value(table, "key_1") == 10);
    REQUIRE(version_value(table, "key_2") == 10);
  }

  SECTION("Invalid and empty input is rejected") {
    const char* columns[] = {"version"};
    const char* expressions[] = {"version + 1"};
    const char* null_columns[] = {nullptr};
    const char* null_expressions[] = {nullptr};
    const char* empty_columns[] = {""};
    const char* invalid_columns[] = {"\xff\xfe"};
    const char* invalid_expressions[] = {"\xff\xfe"};
    LanceDBUpdateResult update_result = {};

    REQUIRE(lancedb_table_update(
        nullptr, columns, expressions, 1, nullptr, &update_result, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update(
        table, nullptr, expressions, 1, nullptr, &update_result, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update(
        table, columns, nullptr, 1, nullptr, &update_result, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update(
        table, columns, expressions, 0, nullptr, &update_result, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update(
        table, columns, expressions, 1, nullptr, nullptr, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update(
        table, null_columns, expressions, 1, nullptr, &update_result, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update(
        table, columns, null_expressions, 1, nullptr, &update_result, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update(
        table, empty_columns, expressions, 1, nullptr, &update_result, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update(
        table, invalid_columns, expressions, 1, nullptr, &update_result, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update(
        table, columns, invalid_expressions, 1, nullptr, &update_result, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update(
        table, columns, expressions, 1, "\xff\xfe", &update_result, nullptr)
        == LANCEDB_INVALID_ARGUMENT);

    REQUIRE(lancedb_table_version(table) == initial_version);
  }

  lancedb_table_free(table);
}

// Schema and reader for the update-evaluation regression: one row (a, b)
static std::shared_ptr<arrow::Schema> create_pair_schema() {
  return arrow::schema({arrow::field("a", arrow::int32()),
                        arrow::field("b", arrow::int32())});
}

static LanceDBRecordBatchReader* create_pair_reader(int32_t a, int32_t b) {
  arrow::Int32Builder a_builder;
  arrow::Int32Builder b_builder;
  REQUIRE(a_builder.Append(a).ok());
  REQUIRE(b_builder.Append(b).ok());

  std::shared_ptr<arrow::Array> a_array, b_array;
  REQUIRE(a_builder.Finish(&a_array).ok());
  REQUIRE(b_builder.Finish(&b_array).ok());

  auto batch = arrow::RecordBatch::Make(create_pair_schema(), 1, {a_array, b_array});
  return create_reader_from_batch(batch);
}

// Read the (a, b) values of the single row in a pair table
static std::pair<int32_t, int32_t> pair_values(LanceDBTable* table) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);

  LanceDBQueryResult* query_result = lancedb_query_execute(query);
  REQUIRE(query_result != nullptr);

  char* error_message = nullptr;
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
  auto a_array = std::static_pointer_cast<arrow::Int32Array>(batch->column(0));
  auto b_array = std::static_pointer_cast<arrow::Int32Array>(batch->column(1));
  const auto values = std::make_pair(a_array->Value(0), b_array->Value(0));

  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);

  return values;
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Update evaluates against the original row", "[table]") {
  const std::string table_name = "test_update_original_row";
  auto schema = create_pair_schema();
  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_create(
      db, table_name.c_str(), reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      create_pair_reader(1, 2), &table, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  // Swapping the two columns only produces (2, 1) when both expressions read
  // the original row rather than the batch changed by the other assignment.
  const char* columns[] = {"a", "b"};
  const char* expressions[] = {"b", "a"};
  LanceDBUpdateResult update_result = {};
  REQUIRE(lancedb_table_update(
      table, columns, expressions, 2, nullptr, &update_result, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(update_result.rows_updated == 1);
  REQUIRE(update_result.version != 0);

  const auto values = pair_values(table);
  REQUIRE(values.first == 2);
  REQUIRE(values.second == 1);

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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB run_on_stack wrapping", "[table]") {
  const std::string table_name = "test_run_on_stack";
  create_empty_table(table_name);

  LanceDBTable* table = open_table(table_name);

  constexpr auto row_num = 10;
  auto initial_batch = create_test_record_batch(row_num, 0);
  auto initial_reader = create_reader_from_batch(initial_batch);
  REQUIRE(initial_reader != nullptr);

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_add(table, initial_reader, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == row_num);

  SECTION("No-op run_on_stack") {
    lancedb_run_on_stack(nullptr, nullptr, 0, 0);
  }

  SECTION("Merge insert via run_on_stack") {
    RunOnStackCtx ctx{table, LANCEDB_UNKNOWN, nullptr};
    lancedb_run_on_stack(merge_insert_on_stack, &ctx, 512 * 1024, 1024 * 1024);

    REQUIRE(ctx.result == LANCEDB_SUCCESS);
    REQUIRE(ctx.error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num + 5);
  }

  SECTION("SQL delete via run_on_stack") {
    RunOnStackCtx ctx{table, LANCEDB_UNKNOWN, nullptr};
    lancedb_run_on_stack(sql_delete_on_stack, &ctx, 512 * 1024, 1024 * 1024);

    REQUIRE(ctx.result == LANCEDB_SUCCESS);
    REQUIRE(ctx.error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 1);
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table - Arrow IPC input", "[table][ipc]") {
  SECTION("A schema-only file creates an empty table of the same shape") {
    const std::string source_name = "ipc_schema_only_source";
    create_empty_table(source_name);
    LanceDBTable* table = open_table(source_name);

    LanceDBIpcBuffer buffer = {};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_schema_ipc_file(table, &buffer, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(buffer.data != nullptr);
    REQUIRE(buffer.len > 0);

    auto contents = read_ipc_file(buffer.data, buffer.len);
    REQUIRE(contents.schema->num_fields() == 2);
    REQUIRE(contents.schema->field(0)->name() == "key");
    REQUIRE(contents.schema->field(1)->name() == "data");
    REQUIRE(contents.batches.empty());

    // The schema alone is enough to create a table of the same shape
    FFI_ArrowSchema* c_schema = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        buffer.data, buffer.len, nullptr, &c_schema, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(c_schema != nullptr);

    // The bytes are only borrowed, so they can be released before the schema is used
    lancedb_ipc_buffer_free(&buffer);
    REQUIRE(buffer.data == nullptr);
    REQUIRE(buffer.len == 0);
    // Freeing an already freed buffer is a no-op
    lancedb_ipc_buffer_free(&buffer);
    // Freeing no buffer at all is a no-op too
    lancedb_ipc_buffer_free(nullptr);

    LanceDBTable* created = nullptr;
    REQUIRE(lancedb_table_create(
        db, "ipc_schema_only_created", c_schema, nullptr, &created, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(created != nullptr);
    lancedb_free_arrow_schema(c_schema);

    REQUIRE(lancedb_table_count_rows(created) == 0);
    REQUIRE(lancedb_table_version(created) == 1);

    FFI_ArrowSchema* exported = nullptr;
    REQUIRE(lancedb_table_arrow_schema(created, &exported, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(exported != nullptr);
    lancedb_free_arrow_schema(exported);

    lancedb_table_free(created);
    lancedb_table_free(table);
  }

  SECTION("Two uneven batches in one file are written by a single add") {
    const std::string table_name = "ipc_multi_batch";
    create_empty_table(table_name);
    LanceDBTable* table = open_table(table_name);
    REQUIRE(lancedb_table_version(table) == 1);

    auto schema = create_test_schema();
    auto bytes = write_ipc_file(
        schema, {create_test_record_batch(2, 0), create_test_record_batch(3, 2)});

    LanceDBRecordBatchReader* reader = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        bytes.data(), bytes.size(), &reader, nullptr, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(reader != nullptr);

    // The reader owns every batch before the call returns, so the input bytes
    // may go away while the table is still being written
    std::fill(bytes.begin(), bytes.end(), 0);

    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == 5);
    // One commit for the whole file, not one per batch
    REQUIRE(lancedb_table_version(table) == 2);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key"};
    REQUIRE(lancedb_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(result != nullptr);

    LanceDBIpcBuffer output = {};
    REQUIRE(lancedb_query_result_to_ipc_file(result, &output, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // The file does not depend on the table handle that produced it
    lancedb_table_free(table);
    table = nullptr;

    auto contents = read_ipc_file(output.data, output.len);
    REQUIRE(contents.schema->num_fields() == 1);
    REQUIRE(contents.schema->field(0)->name() == "key");
    REQUIRE(contents.batches.size() >= 1);
    REQUIRE(join_strings(read_ipc_file_strings(contents, 0)) ==
            "key_0,key_1,key_2,key_3,key_4");

    lancedb_ipc_buffer_free(&output);
    REQUIRE(output.data == nullptr);
    REQUIRE(output.len == 0);
  }

  SECTION("A zero-row batch among data batches keeps every row") {
    const std::string table_name = "ipc_zero_row_batch";
    create_empty_table(table_name);
    LanceDBTable* table = open_table(table_name);

    auto schema = create_test_schema();
    auto bytes = write_ipc_file(schema,
        {create_test_record_batch(2, 0), create_test_record_batch(0, 2),
         create_test_record_batch(3, 2)});

    LanceDBRecordBatchReader* reader = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        bytes.data(), bytes.size(), &reader, nullptr, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(reader != nullptr);

    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == 5);
    REQUIRE(lancedb_table_version(table) == 2);

    lancedb_table_free(table);
  }

  SECTION("Bad input fails before the table changes") {
    const std::string table_name = "ipc_invalid_input";
    create_empty_table(table_name);
    LanceDBTable* table = open_table(table_name);
    const auto version = lancedb_table_version(table);
    const auto rows = lancedb_table_count_rows(table);

    auto schema = create_test_schema();
    auto first = create_test_record_batch(2, 0);
    auto second = create_test_record_batch(3, 2);
    const auto valid = write_ipc_file(schema, {first, second});
    const auto stream = write_ipc_stream(schema, {first, second});
    const auto corrupt = write_ipc_file_with_corrupt_second_batch(schema, first, second);

    // The crafted file is what the test claims: the footer and the first batch
    // are readable, the second batch is not
    {
      auto buffer = arrow::Buffer::FromString(
          std::string(reinterpret_cast<const char*>(corrupt.data()), corrupt.size()));
      auto input = std::make_shared<arrow::io::BufferReader>(buffer);
      auto reader = arrow::ipc::RecordBatchFileReader::Open(input).ValueOrDie();
      REQUIRE(reader->num_record_batches() == 2);
      REQUIRE(reader->ReadRecordBatch(0).ok());
      REQUIRE(!reader->ReadRecordBatch(1).ok());
    }

    char* error_message = nullptr;
    LanceDBRecordBatchReader* reader = nullptr;
    FFI_ArrowSchema* c_schema = nullptr;

    // A missing pointer is a programming error, not bad input
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        nullptr, valid.size(), &reader, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        valid.data(), valid.size(), nullptr, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    // Bytes that cannot hold the footer their own last ten bytes announce are
    // rejected before the decoder reads or allocates from them
    auto oversized_footer = valid;
    const uint32_t announced_footer = static_cast<uint32_t>(oversized_footer.size()) + 1;
    std::memcpy(oversized_footer.data() + oversized_footer.size() - 10, &announced_footer,
                sizeof(announced_footer));

    // Bytes without an IPC file are input errors, reported with a decoding message
    const std::vector<std::vector<uint8_t>> rejected = {
        std::vector<uint8_t>{'A', 'R', 'R', 'O', 'W', '1', 0, 0},  // too short for a footer
        std::vector<uint8_t>(valid.begin(), valid.end() - 40),      // truncated
        stream,                                                    // stream, not a file
        corrupt,                                                   // second batch corrupt
        oversized_footer,                                          // footer does not fit
    };
    for (const auto& bytes : rejected) {
      reader = nullptr;
      c_schema = nullptr;
      error_message = nullptr;
      REQUIRE(lancedb_record_batch_reader_from_ipc_file(
          bytes.data(), bytes.size(), &reader, &c_schema, &error_message)
          == LANCEDB_INVALID_INPUT);
      REQUIRE(reader == nullptr);
      REQUIRE(c_schema == nullptr);
      REQUIRE(error_message != nullptr);
      // The decoding failure keeps its diagnostic instead of a generic message
      REQUIRE(std::string(error_message).find("Invalid input") != std::string::npos);
      lancedb_free_string(error_message);
    }

    // The announced footer is checked against the input before decoding
    reader = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        oversized_footer.data(), oversized_footer.size(), &reader, nullptr, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(reader == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("does not fit") != std::string::npos);
    lancedb_free_string(error_message);

    // An empty input is rejected without reading any byte
    reader = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        valid.data(), 0, &reader, nullptr, &error_message) == LANCEDB_INVALID_INPUT);
    REQUIRE(reader == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    // A size that cannot describe a buffer is rejected before any pointer is read
    reader = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        valid.data(), SIZE_MAX, &reader, nullptr, &error_message) == LANCEDB_INVALID_INPUT);
    REQUIRE(reader == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("too large") != std::string::npos);
    lancedb_free_string(error_message);

    // A footer that fits the input but cannot be parsed is an input error
    const auto unreadable_footer = write_ipc_file_with_unreadable_footer();
    reader = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        unreadable_footer.data(), unreadable_footer.size(), &reader, nullptr, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(reader == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("Invalid input") != std::string::npos);
    lancedb_free_string(error_message);

    // A footer that parses but declares blocks past the end of the input is
    // rejected before the decoder reads any of them
    const auto blocks_outside = write_ipc_file_with_blocks_outside_the_file();
    reader = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        blocks_outside.data(), blocks_outside.size(), &reader, nullptr, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(reader == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("ends outside the file") != std::string::npos);
    lancedb_free_string(error_message);

    // A file with dictionary blocks decodes with its dictionary encoding intact
    const auto dictionary_file = write_ipc_file_with_dictionary_column();
    reader = nullptr;
    c_schema = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        dictionary_file.data(), dictionary_file.size(), &reader, &c_schema, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(reader != nullptr);
    REQUIRE(c_schema != nullptr);
    {
      auto* schema = reinterpret_cast<ArrowSchema*>(c_schema);
      REQUIRE(schema->n_children == 1);
      REQUIRE(schema->children[0]->dictionary != nullptr);
      REQUIRE(std::string(schema->children[0]->dictionary->format) == "u");
    }
    lancedb_free_arrow_schema(c_schema);
    lancedb_record_batch_reader_free(reader);

    // The same file works when only one of the two outputs is requested
    reader = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        dictionary_file.data(), dictionary_file.size(), &reader, nullptr, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(reader != nullptr);
    lancedb_record_batch_reader_free(reader);

    c_schema = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        dictionary_file.data(), dictionary_file.size(), nullptr, &c_schema, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(c_schema != nullptr);
    lancedb_free_arrow_schema(c_schema);

    // A schema the Arrow C ABI cannot carry is a normal invalid-input error, and
    // neither requested output is handed over
    const auto unrepresentable = write_ipc_file_with_unrepresentable_field_name();
    reader = nullptr;
    c_schema = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        unrepresentable.data(), unrepresentable.size(), &reader, &c_schema, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(reader == nullptr);
    REQUIRE(c_schema == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("Invalid input") != std::string::npos);
    lancedb_free_string(error_message);

    // A valid file still decodes
    reader = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_record_batch_reader_from_ipc_file(
        valid.data(), valid.size(), &reader, nullptr, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(reader != nullptr);
    lancedb_record_batch_reader_free(reader);

    // Nothing reached the table
    REQUIRE(lancedb_table_version(table) == version);
    REQUIRE(lancedb_table_count_rows(table) == rows);

    // The buffer entry points reject a missing handle the same way
    LanceDBIpcBuffer buffer = {};
    error_message = nullptr;
    REQUIRE(lancedb_table_schema_ipc_file(nullptr, &buffer, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(buffer.data == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_table_free(table);
  }
}
