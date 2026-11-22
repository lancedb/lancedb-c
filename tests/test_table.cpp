/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <iterator>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
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

// Create a batch with rows for keys "key_<start_index>" ..
// "key_<start_index + num_rows - 1>", where all values of the "data" column
// are set to "value"
static std::shared_ptr<arrow::RecordBatch> create_batch_with_value(
    int num_rows, int start_index, float value) {
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

  return arrow::RecordBatch::Make(schema, num_rows, {key_array, data_array});
}

// Create a reader with rows for keys "key_<start_index>" .. "key_<start_index + num_rows - 1>",
// where all values of the "data" column are set to "value"
static LanceDBRecordBatchReader* create_reader_with_value(int num_rows, int start_index, float value) {
  return create_reader_from_batch(create_batch_with_value(num_rows, start_index, value));
}

// Create a reader over every batch of a valid Arrow IPC file
static LanceDBRecordBatchReader* create_reader_from_ipc_file(const std::vector<uint8_t>& bytes) {
  LanceDBRecordBatchReader* reader = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_record_batch_reader_from_ipc_file(
      bytes.data(), bytes.size(), &reader, nullptr, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(reader != nullptr);
  return reader;
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table - filtered count", "[table]") {
  const std::string table_name = "test_table_filtered_count";
  constexpr auto row_num = 6;
  LanceDBTable* table = create_table_with_data(table_name, row_num, 0);

  SECTION("A NULL filter counts every row") {
    size_t count = 42;
    char* error_message = nullptr;

    REQUIRE(lancedb_table_count_rows_filtered(table, nullptr, &count, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == row_num);
    REQUIRE(count == lancedb_table_count_rows(table));
  }

  SECTION("An SQL filter counts only matching rows") {
    size_t count = 42;
    char* error_message = nullptr;

    REQUIRE(lancedb_table_count_rows_filtered(
        table, "key IN ('key_1', 'key_4')", &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 2);

    // The old getters keep counting every row.
    REQUIRE(lancedb_table_count_rows(table) == row_num);
  }

  SECTION("An invalid filter keeps the previous count") {
    const auto initial_version = lancedb_table_version(table);
    size_t count = 42;
    char* error_message = nullptr;

    REQUIRE(lancedb_table_count_rows_filtered(
        table, "no_such_column = 1", &count, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("no_such_column") != std::string::npos);
    lancedb_free_string(error_message);
    REQUIRE(count == 42);
    REQUIRE(lancedb_table_version(table) == initial_version);
  }

  SECTION("Invalid input is rejected and keeps the previous count") {
    size_t count = 42;
    char* error_message = nullptr;

    REQUIRE(lancedb_table_count_rows_filtered(nullptr, nullptr, &count, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    REQUIRE(count == 42);

    REQUIRE(lancedb_table_count_rows_filtered(table, nullptr, nullptr, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_count_rows_filtered(table, "\xff\xfe", &count, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    REQUIRE(count == 42);
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Merge Insert with result", "[table][ipc]") {
  const std::string table_name = "test_merge_insert_result_table";
  create_empty_table(table_name);
  LanceDBTable* table = open_table(table_name);

  constexpr int row_num = 10;
  auto initial_reader = create_reader_from_batch(create_test_record_batch(row_num, 0));
  REQUIRE(initial_reader != nullptr);

  char* error_message = nullptr;
  REQUIRE(lancedb_table_add(table, initial_reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == row_num);
  REQUIRE(lancedb_table_version(table) == 2);

  const char* on_columns[] = {"key"};
  auto schema = create_test_schema();

  SECTION("Updates matched rows and reports the commit") {
    auto bytes = write_ipc_file(schema, {create_batch_with_value(5, 0, 888.0F)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 0,
      .when_not_matched_by_source_delete = 0,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = -1,
      .timeout_ms = -1,
    };
    LanceDBMergeInsertResult result = {};

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_result(
        table, reader, on_columns, 1, &options, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(result.version == 3);
    REQUIRE(result.num_inserted_rows == 0);
    REQUIRE(result.num_updated_rows == 5);
    REQUIRE(result.num_deleted_rows == 0);
    REQUIRE(result.num_attempts == 1);
    REQUIRE(lancedb_table_version(table) == result.version);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
    REQUIRE(first_data_value(table, "key_0") == 888.0F);
    REQUIRE(first_data_value(table, "key_4") == 888.0F);
    REQUIRE(first_data_value(table, "key_5") == 50.0F);
  }

  SECTION("Inserts unmatched rows and reports the commit") {
    auto bytes = write_ipc_file(schema, {create_test_record_batch(5, row_num)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 0,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 1,
      .when_not_matched_by_source_delete = 0,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = -1,
      .timeout_ms = -1,
    };
    LanceDBMergeInsertResult result = {};

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_result(
        table, reader, on_columns, 1, &options, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(result.version == 3);
    REQUIRE(result.num_inserted_rows == 5);
    REQUIRE(result.num_updated_rows == 0);
    REQUIRE(result.num_deleted_rows == 0);
    REQUIRE(result.num_attempts == 1);
    REQUIRE(lancedb_table_version(table) == result.version);
    REQUIRE(lancedb_table_count_rows(table) == row_num + 5);
    REQUIRE(first_data_value(table, "key_10") == 100.0F);
  }

  SECTION("Applies the matched condition with an explicit index choice and timeout") {
    auto bytes = write_ipc_file(schema, {create_batch_with_value(5, 0, 777.0F)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 1,
      .when_matched_update_all_condition = "target.key = 'key_1'",
      .when_not_matched_insert_all = 0,
      .when_not_matched_by_source_delete = 0,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = 0,
      .timeout_ms = 60000,
    };
    LanceDBMergeInsertResult result = {};

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_result(
        table, reader, on_columns, 1, &options, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(result.version == 3);
    REQUIRE(result.num_updated_rows == 1);
    REQUIRE(result.num_inserted_rows == 0);
    REQUIRE(result.num_deleted_rows == 0);
    REQUIRE(result.num_attempts == 1);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
    REQUIRE(first_data_value(table, "key_0") == 0.0F);
    REQUIRE(first_data_value(table, "key_1") == 777.0F);
    REQUIRE(first_data_value(table, "key_2") == 20.0F);
  }

  SECTION("Merges every batch of one IPC file in a single commit") {
    auto bytes = write_ipc_file(
        schema,
        {create_batch_with_value(2, 0, 555.0F), create_test_record_batch(2, row_num)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 1,
      .when_not_matched_by_source_delete = 0,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = 1,
      .timeout_ms = -1,
    };
    LanceDBMergeInsertResult result = {};

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_result(
        table, reader, on_columns, 1, &options, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(result.version == 3);
    REQUIRE(result.num_inserted_rows == 2);
    REQUIRE(result.num_updated_rows == 2);
    REQUIRE(result.num_deleted_rows == 0);
    REQUIRE(result.num_attempts == 1);
    REQUIRE(lancedb_table_count_rows(table) == row_num + 2);
    REQUIRE(first_data_value(table, "key_0") == 555.0F);
    REQUIRE(first_data_value(table, "key_2") == 20.0F);
    REQUIRE(first_data_value(table, "key_10") == 100.0F);
  }

  SECTION("Deletes unmatched target rows only where the source filter matches") {
    auto bytes = write_ipc_file(schema, {create_test_record_batch(5, 0)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 0,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 0,
      .when_not_matched_by_source_delete = 1,
      .when_not_matched_by_source_delete_filter = "key = 'key_5' OR key = 'key_6'",
      .use_index = -1,
      .timeout_ms = -1,
    };
    LanceDBMergeInsertResult result = {};

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_result(
        table, reader, on_columns, 1, &options, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(result.version == 3);
    REQUIRE(result.num_inserted_rows == 0);
    REQUIRE(result.num_updated_rows == 0);
    REQUIRE(result.num_deleted_rows == 2);
    REQUIRE(result.num_attempts == 1);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 2);
    REQUIRE(first_data_value(table, "key_4") == 40.0F);
    REQUIRE(first_data_value(table, "key_7") == 70.0F);
  }

  SECTION("Merges an empty source carrying the table schema") {
    LanceDBIpcBuffer buffer = {};
    REQUIRE(lancedb_table_schema_ipc_file(table, &buffer, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(buffer.data != nullptr);
    std::vector<uint8_t> bytes(buffer.data, buffer.data + buffer.len);
    lancedb_ipc_buffer_free(&buffer);
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 0,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 0,
      .when_not_matched_by_source_delete = 1,
      .when_not_matched_by_source_delete_filter = "key = 'key_9'",
      .use_index = -1,
      .timeout_ms = -1,
    };
    LanceDBMergeInsertResult result = {};

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_result(
        table, reader, on_columns, 1, &options, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(result.version == 3);
    REQUIRE(result.num_inserted_rows == 0);
    REQUIRE(result.num_updated_rows == 0);
    REQUIRE(result.num_deleted_rows == 1);
    REQUIRE(result.num_attempts == 1);
    REQUIRE(lancedb_table_version(table) == result.version);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 1);
    REQUIRE(first_data_value(table, "key_0") == 0.0F);
  }

  SECTION("An empty source without delete actions changes nothing") {
    LanceDBIpcBuffer buffer = {};
    REQUIRE(lancedb_table_schema_ipc_file(table, &buffer, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    std::vector<uint8_t> bytes(buffer.data, buffer.data + buffer.len);
    lancedb_ipc_buffer_free(&buffer);
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 1,
      .when_not_matched_by_source_delete = 0,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = -1,
      .timeout_ms = -1,
    };
    LanceDBMergeInsertResult result = {};

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_result(
        table, reader, on_columns, 1, &options, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(result.num_inserted_rows == 0);
    REQUIRE(result.num_updated_rows == 0);
    REQUIRE(result.num_deleted_rows == 0);
    REQUIRE(result.num_attempts == 1);
    REQUIRE(lancedb_table_version(table) == result.version);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
  }

  SECTION("Rejects an out-of-range use_index sentinel") {
    auto bytes = write_ipc_file(schema, {create_test_record_batch(1, 0)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 0,
      .when_not_matched_by_source_delete = 0,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = 2,
      .timeout_ms = -1,
    };
    LanceDBMergeInsertResult result = {};
    result.version = 123;

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_result(
        table, reader, on_columns, 1, &options, &result, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("use_index") != std::string::npos);
    lancedb_free_string(error_message);
    REQUIRE(result.version == 0);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
    REQUIRE(lancedb_table_version(table) == 2);
  }

  SECTION("Rejects an out-of-range timeout sentinel") {
    auto bytes = write_ipc_file(schema, {create_test_record_batch(1, 0)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 0,
      .when_not_matched_by_source_delete = 0,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = -1,
      .timeout_ms = -2,
    };
    LanceDBMergeInsertResult result = {};

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_result(
        table, reader, on_columns, 1, &options, &result, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("timeout_ms") != std::string::npos);
    lancedb_free_string(error_message);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
    REQUIRE(lancedb_table_version(table) == 2);
  }

  SECTION("Rejects a missing result pointer and still consumes the reader") {
    auto bytes = write_ipc_file(schema, {create_test_record_batch(1, 0)});
    auto reader = create_reader_from_ipc_file(bytes);

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_result(
        table, reader, on_columns, 1, nullptr, nullptr, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
    REQUIRE(lancedb_table_version(table) == 2);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Merge Insert with complete options", "[table][ipc]") {
  const std::string table_name = "test_merge_insert_options_table";
  create_empty_table(table_name);
  LanceDBTable* table = open_table(table_name);

  constexpr int row_num = 10;
  auto initial_reader = create_reader_from_batch(create_test_record_batch(row_num, 0));
  REQUIRE(initial_reader != nullptr);

  char* error_message = nullptr;
  REQUIRE(lancedb_table_add(table, initial_reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == row_num);
  REQUIRE(lancedb_table_version(table) == 2);

  const char* on_columns[] = {"key"};
  auto schema = create_test_schema();

  SECTION("Reports every SDK field for one update-and-insert merge") {
    auto bytes = write_ipc_file(
        schema,
        {create_batch_with_value(2, 0, 555.0F), create_test_record_batch(2, row_num)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 1,
      .when_not_matched_by_source_delete = 0,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = -1,
      .timeout_ms = -1,
    };
    uint8_t retries = 0;
    LanceDBMergeInsertResultV2 result = {};
    result.version = 123;

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_options(
        table, reader, on_columns, 1, &options, &retries, nullptr, &result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(result.version == 3);
    REQUIRE(result.num_inserted_rows == 2);
    REQUIRE(result.num_updated_rows == 2);
    REQUIRE(result.num_deleted_rows == 0);
    REQUIRE(result.num_attempts == 1);
    REQUIRE(result.num_rows == 4);
    REQUIRE(lancedb_table_version(table) == result.version);
    REQUIRE(lancedb_table_count_rows(table) == row_num + 2);
    REQUIRE(first_data_value(table, "key_0") == 555.0F);
    REQUIRE(first_data_value(table, "key_2") == 20.0F);
    REQUIRE(first_data_value(table, "key_10") == 100.0F);
  }

  SECTION("NULL options keeps the default upsert") {
    auto bytes = write_ipc_file(
        schema,
        {create_batch_with_value(1, 0, 444.0F), create_test_record_batch(1, row_num)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertResultV2 result = {};

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_options(
        table, reader, on_columns, 1, nullptr, nullptr, nullptr, &result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(result.version == 3);
    REQUIRE(result.num_inserted_rows == 1);
    REQUIRE(result.num_updated_rows == 1);
    REQUIRE(result.num_deleted_rows == 0);
    REQUIRE(result.num_attempts == 1);
    REQUIRE(result.num_rows == 2);
    REQUIRE(lancedb_table_count_rows(table) == row_num + 1);
    REQUIRE(first_data_value(table, "key_0") == 444.0F);
    REQUIRE(first_data_value(table, "key_10") == 100.0F);
  }

  SECTION("Explicitly selecting no action fails through the SDK") {
    auto bytes = write_ipc_file(schema, {create_test_record_batch(1, row_num)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 0,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 0,
      .when_not_matched_by_source_delete = 0,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = -1,
      .timeout_ms = -1,
    };
    LanceDBMergeInsertResultV2 result = {};
    result.version = 123;

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_options(
        table, reader, on_columns, 1, &options, nullptr, nullptr, &result, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("not configured to change") != std::string::npos);
    lancedb_free_string(error_message);

    REQUIRE(result.version == 0);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
    REQUIRE(lancedb_table_version(table) == 2);
  }

  SECTION("Accepts a NULL result pointer") {
    auto bytes = write_ipc_file(schema, {create_test_record_batch(2, row_num)});
    auto reader = create_reader_from_ipc_file(bytes);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 0,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 1,
      .when_not_matched_by_source_delete = 0,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = -1,
      .timeout_ms = -1,
    };

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_options(
        table, reader, on_columns, 1, &options, nullptr, nullptr, nullptr, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num + 2);
    REQUIRE(lancedb_table_version(table) == 3);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Merge Insert with by-source delete expression", "[table][ipc]") {
  const std::string table_name = "test_merge_insert_delete_expr_table";
  auto schema = create_versioned_schema();
  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_create(
      db, table_name.c_str(), reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      create_versioned_reader({10, 20, 30, 40, 50, 60, 70, 80, 90, 100}), &table,
      &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  const char* on_columns[] = {"key"};

  SECTION("Deletes unmatched target rows selected by a nontrivial expression") {
    // A source over keys 0..4 leaves keys 5..9 unmatched. The expression keeps
    // only the unmatched rows at version 60 or 80, so keys 5 and 7 are deleted.
    auto reader = create_versioned_reader({10, 20, 30, 40, 50});
    REQUIRE(reader != nullptr);
    LanceDBExpr* expr = lancedb_expr_or(
        lancedb_expr_binary(
            lancedb_expr_column("version"), LANCEDB_BINARY_OP_EQ,
            lancedb_expr_literal_i64(60)),
        lancedb_expr_binary(
            lancedb_expr_column("version"), LANCEDB_BINARY_OP_EQ,
            lancedb_expr_literal_i64(80)));
    REQUIRE(expr != nullptr);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 0,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 0,
      .when_not_matched_by_source_delete = 1,
      .when_not_matched_by_source_delete_filter = nullptr,
      .use_index = -1,
      .timeout_ms = -1,
    };
    LanceDBMergeInsertResultV2 result = {};

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_options(
        table, reader, on_columns, 1, &options, nullptr, expr, &result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_expr_free(expr);

    REQUIRE(result.version == 2);
    REQUIRE(result.num_inserted_rows == 0);
    REQUIRE(result.num_updated_rows == 0);
    REQUIRE(result.num_deleted_rows == 2);
    REQUIRE(result.num_attempts == 1);
    REQUIRE(result.num_rows == 0);
    REQUIRE(lancedb_table_version(table) == result.version);
    REQUIRE(lancedb_table_count_rows(table) == 8);
    REQUIRE(version_value(table, "key_4") == 50);
    REQUIRE(version_value(table, "key_6") == 70);
    REQUIRE(version_value(table, "key_8") == 90);
  }

  SECTION("Rejects both delete condition forms and consumes the reader") {
    auto reader = create_versioned_reader({10, 20, 30, 40, 50});
    REQUIRE(reader != nullptr);
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("version"), LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_i64(60));
    REQUIRE(expr != nullptr);

    LanceDBMergeInsertOptions options = {
      .when_matched_update_all = 0,
      .when_matched_update_all_condition = nullptr,
      .when_not_matched_insert_all = 0,
      .when_not_matched_by_source_delete = 1,
      .when_not_matched_by_source_delete_filter = "version = 60",
      .use_index = -1,
      .timeout_ms = -1,
    };
    LanceDBMergeInsertResultV2 result = {};
    result.version = 123;

    error_message = nullptr;
    REQUIRE(lancedb_table_merge_insert_with_options(
        table, reader, on_columns, 1, &options, nullptr, expr, &result, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_expr_free(expr);

    REQUIRE(result.version == 0);
    REQUIRE(lancedb_table_count_rows(table) == 10);
    REQUIRE(lancedb_table_version(table) == 1);
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

  SECTION("Update with options reports an explicit zero retry budget") {
    const char* columns[] = {"version"};
    const char* expressions[] = {"version + 3"};
    LanceDBUpdateOptions options = {
      .set_fields = LANCEDB_UPDATE_RETRIES,
      .retries = 0,
    };
    LanceDBUpdateResult update_result = {};

    REQUIRE(lancedb_table_update_with_options(
        table, columns, expressions, 1, "key = 'key_1'", &options, &update_result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(update_result.rows_updated == 1);
    REQUIRE(update_result.version == initial_version + 1);
    REQUIRE(update_result.version == lancedb_table_version(table));

    REQUIRE(version_value(table, "key_0") == 10);
    REQUIRE(version_value(table, "key_1") == 13);
    REQUIRE(version_value(table, "key_2") == 10);
  }

  SECTION("Update with a positive retry budget succeeds") {
    const char* columns[] = {"version"};
    const char* expressions[] = {"version + 2"};
    LanceDBUpdateOptions options = {
      .set_fields = LANCEDB_UPDATE_RETRIES,
      .retries = 3,
    };
    LanceDBUpdateResult update_result = {};

    REQUIRE(lancedb_table_update_with_options(
        table, columns, expressions, 1, "key = 'key_2'", &options, &update_result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(update_result.rows_updated == 1);
    REQUIRE(update_result.version == initial_version + 1);
    REQUIRE(version_value(table, "key_2") == 12);
  }

  SECTION("Update with omitted options and a null result succeeds") {
    const char* columns[] = {"version"};
    const char* expressions[] = {"version + 4"};

    REQUIRE(lancedb_table_update_with_options(
        table, columns, expressions, 1, nullptr, nullptr, nullptr, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_version(table) == initial_version + 1);

    REQUIRE(version_value(table, "key_0") == 14);
    REQUIRE(version_value(table, "key_1") == 14);
    REQUIRE(version_value(table, "key_2") == 14);
  }

  SECTION("Update ignores a retry value whose option bit is unset") {
    const char* columns[] = {"version"};
    const char* expressions[] = {"version + 5"};
    LanceDBUpdateOptions options = {
      .set_fields = 0,
      .retries = 7,
    };
    LanceDBUpdateResult update_result = {};

    REQUIRE(lancedb_table_update_with_options(
        table, columns, expressions, 1, nullptr, &options, &update_result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(update_result.rows_updated == 3);
    REQUIRE(update_result.version == initial_version + 1);
    REQUIRE(version_value(table, "key_0") == 15);
  }

  SECTION("Update with an unknown option mask is rejected") {
    const char* columns[] = {"version"};
    const char* expressions[] = {"version + 1"};
    LanceDBUpdateOptions options = {
      .set_fields = 1U << 31,
      .retries = 0,
    };
    LanceDBUpdateResult update_result = {123, 456};

    REQUIRE(lancedb_table_update_with_options(
        table, columns, expressions, 1, nullptr, &options, &update_result, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("unknown update option bits") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;
    REQUIRE(update_result.rows_updated == 123);
    REQUIRE(update_result.version == 456);
    REQUIRE(lancedb_table_version(table) == initial_version);
    REQUIRE(version_value(table, "key_0") == 10);
  }

  SECTION("Invalid expression preserves the SDK error") {
    const char* columns[] = {"version"};
    const char* expressions[] = {"no_such_column + 1"};
    LanceDBUpdateResult update_result = {123, 456};

    REQUIRE(lancedb_table_update(
        table, columns, expressions, 1, "key = 'key_1'", &update_result, &error_message)
        == LANCEDB_COLUMN_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("no_such_column") != std::string::npos);
    lancedb_free_string(error_message);

    // The old entry point keeps its result unchanged when the SDK rejects the update.
    REQUIRE(update_result.rows_updated == 123);
    REQUIRE(update_result.version == 456);
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

  SECTION("The old update entry evaluates against the original row") {
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
  }

  SECTION("The options entry evaluates against the original row") {
    LanceDBUpdateResult update_result = {};
    REQUIRE(lancedb_table_update_with_options(
        table, columns, expressions, 2, nullptr, nullptr, &update_result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(update_result.rows_updated == 1);
    REQUIRE(update_result.version != 0);

    const auto values = pair_values(table);
    REQUIRE(values.first == 2);
    REQUIRE(values.second == 1);
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
  const auto initial_version = lancedb_table_version(table);

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

  SECTION("Delete with a result reports rows and the commit") {
    char* error_message = nullptr;
    LanceDBDeleteResult delete_result = {123, 456};
    LanceDBError result = lancedb_table_delete_with_result(
        table, "key IN ('key_0', 'key_1', 'key_2')", &delete_result, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(delete_result.num_deleted_rows == 3);
    REQUIRE(delete_result.version == initial_version + 1);
    REQUIRE(delete_result.version == lancedb_table_version(table));
    REQUIRE(lancedb_table_count_rows(table) == row_num - 3);
  }

  SECTION("Delete with a null result succeeds") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_delete_with_result(
        table, "key = 'key_0'", nullptr, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 1);
  }

  SECTION("Delete with a result leaves it unchanged on failure") {
    char* error_message = nullptr;
    LanceDBDeleteResult delete_result = {123, 456};

    REQUIRE(lancedb_table_delete_with_result(
        nullptr, "key = 'key_0'", &delete_result, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    REQUIRE(delete_result.num_deleted_rows == 123);
    REQUIRE(delete_result.version == 456);

    REQUIRE(lancedb_table_delete_with_result(
        table, nullptr, &delete_result, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    REQUIRE(delete_result.num_deleted_rows == 123);
    REQUIRE(delete_result.version == 456);

    REQUIRE(lancedb_table_delete_with_result(
        table, "\xff\xfe", &delete_result, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    REQUIRE(delete_result.num_deleted_rows == 123);
    REQUIRE(delete_result.version == 456);

    REQUIRE(lancedb_table_delete_with_result(
        table, "unknown = 'key_0'", &delete_result, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    REQUIRE(delete_result.num_deleted_rows == 123);
    REQUIRE(delete_result.version == 456);
    REQUIRE(lancedb_table_count_rows(table) == row_num);
    REQUIRE(lancedb_table_version(table) == initial_version);
  }

  SECTION("Delete with unknown column should fail") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_delete(
        table, "unknown = 'key_0'", &error_message);

    REQUIRE(result == LANCEDB_COLUMN_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("unknown") != std::string::npos);
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
  const auto initial_version = lancedb_table_version(table);

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

  SECTION("Delete with a result reports rows and the commit") {
    char* error_message = nullptr;
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_0"));
    REQUIRE(expr != nullptr);

    LanceDBDeleteResult delete_result = {123, 456};
    LanceDBError result = lancedb_table_df_delete_with_result(
        table, expr, &delete_result, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(delete_result.num_deleted_rows == 1);
    REQUIRE(delete_result.version == initial_version + 1);
    REQUIRE(delete_result.version == lancedb_table_version(table));
    REQUIRE(lancedb_table_count_rows(table) == row_num - 1);
  }

  SECTION("DF delete with a null result succeeds") {
    char* error_message = nullptr;
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_0"));
    REQUIRE(expr != nullptr);

    REQUIRE(lancedb_table_df_delete_with_result(table, expr, nullptr, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == row_num - 1);
  }

  SECTION("DF delete with a result leaves it unchanged on failure") {
    char* error_message = nullptr;
    LanceDBExpr* expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_0"));
    REQUIRE(expr != nullptr);

    LanceDBDeleteResult delete_result = {123, 456};
    REQUIRE(lancedb_table_df_delete_with_result(
        nullptr, expr, &delete_result, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    REQUIRE(delete_result.num_deleted_rows == 123);
    REQUIRE(delete_result.version == 456);
    REQUIRE(lancedb_table_version(table) == initial_version);

    error_message = nullptr;
    REQUIRE(lancedb_table_df_delete_with_result(table, nullptr, &delete_result, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    REQUIRE(delete_result.num_deleted_rows == 123);
    REQUIRE(delete_result.version == 456);
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

// Schema used by the schema-evolution tests. The key and score fields are
// initially non-nullable so that nullability changes have an observable effect.
static std::shared_ptr<arrow::Schema> create_schema_evolution_schema() {
  return arrow::schema({
      arrow::field("key", arrow::utf8(), /*nullable=*/false),
      arrow::field("score", arrow::int32(), /*nullable=*/false),
      arrow::field("note", arrow::utf8(), /*nullable=*/true),
  });
}

static LanceDBRecordBatchReader* create_schema_evolution_reader() {
  const std::vector<int32_t> scores = {10, 20, 30, 40, 50};
  arrow::StringBuilder key_builder;
  arrow::Int32Builder score_builder;
  arrow::StringBuilder note_builder;

  for (size_t i = 0; i < scores.size(); i++) {
    REQUIRE(key_builder.Append("key_" + std::to_string(i)).ok());
    REQUIRE(score_builder.Append(scores[i]).ok());
    // Keep one NULL so tightening note's nullability can be exercised.
    if (i == 2) {
      REQUIRE(note_builder.AppendNull().ok());
    } else {
      REQUIRE(note_builder.Append("note_" + std::to_string(i)).ok());
    }
  }

  std::shared_ptr<arrow::Array> key_array, score_array, note_array;
  REQUIRE(key_builder.Finish(&key_array).ok());
  REQUIRE(score_builder.Finish(&score_array).ok());
  REQUIRE(note_builder.Finish(&note_array).ok());

  auto batch = arrow::RecordBatch::Make(
      create_schema_evolution_schema(), static_cast<int64_t>(scores.size()),
      {key_array, score_array, note_array});
  return create_reader_from_batch(batch);
}

// Build the logical Blob v2 field shape used by the SDK: a nullable
// struct<data, uri> tagged with the lance.blob.v2 extension name.
static std::shared_ptr<arrow::Field> blob_v2_field(const std::string& name) {
  auto data_type = arrow::struct_(arrow::FieldVector{
      arrow::field("data", arrow::large_binary(), /*nullable=*/true),
      arrow::field("uri", arrow::utf8(), /*nullable=*/true),
  });
  return arrow::field(name, data_type, /*nullable=*/true)
      ->WithMetadata(arrow::key_value_metadata(
          {"ARROW:extension:name"}, {"lance.blob.v2"}));
}

static LanceDBTable* create_table_with_schema(
    LanceDBConnection* db,
    const std::string& table_name,
    const std::shared_ptr<arrow::Schema>& schema) {
  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_create(
      db, table_name.c_str(), reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      nullptr, &table, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }
  return table;
}

// The SDK accepts a plain LargeBinary batch for an existing Blob v2 column and
// materializes the stored value through the table's blob format.
static std::shared_ptr<arrow::RecordBatch> create_blob_input_batch(
    const std::vector<int64_t>& ids,
    const std::vector<std::optional<std::string>>& payloads) {
  REQUIRE(ids.size() == payloads.size());

  auto schema = arrow::schema({
      arrow::field("id", arrow::int64(), /*nullable=*/false),
      arrow::field("image", arrow::large_binary(), /*nullable=*/true),
  });
  arrow::Int64Builder id_builder;
  arrow::LargeBinaryBuilder image_builder;
  for (size_t i = 0; i < ids.size(); i++) {
    REQUIRE(id_builder.Append(ids[i]).ok());
    if (payloads[i].has_value()) {
      REQUIRE(image_builder.Append(payloads[i]->data(), payloads[i]->size()).ok());
    } else {
      REQUIRE(image_builder.AppendNull().ok());
    }
  }

  std::shared_ptr<arrow::Array> id_array, image_array;
  REQUIRE(id_builder.Finish(&id_array).ok());
  REQUIRE(image_builder.Finish(&image_array).ok());
  return arrow::RecordBatch::Make(
      schema, static_cast<int64_t>(ids.size()), {id_array, image_array});
}

// Return the table's id -> row id pairs so a blob test does not depend on
// physical row positions or on the order a scan returns rows in.
static std::vector<std::pair<int64_t, uint64_t>> query_id_row_ids(LanceDBTable* table) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);

  char* error_message = nullptr;
  REQUIRE(lancedb_query_with_row_id(query, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  LanceDBQueryResult* query_result = lancedb_query_execute(query);
  REQUIRE(query_result != nullptr);

  FFI_ArrowArray** result_arrays = nullptr;
  FFI_ArrowSchema* result_schema = nullptr;
  size_t count = 0;
  REQUIRE(lancedb_query_result_to_arrow(
      query_result, &result_arrays, &result_schema, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count > 0);

  auto imported_schema = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(result_schema));
  REQUIRE(imported_schema.ok());
  const auto id_index = imported_schema.ValueUnsafe()->GetFieldIndex("id");
  const auto row_id_index = imported_schema.ValueUnsafe()->GetFieldIndex("_rowid");
  REQUIRE(id_index >= 0);
  REQUIRE(row_id_index >= 0);

  std::vector<std::pair<int64_t, uint64_t>> pairs;
  for (size_t i = 0; i < count; i++) {
    auto imported_batch = arrow::ImportRecordBatch(
        reinterpret_cast<ArrowArray*>(result_arrays[i]), imported_schema.ValueUnsafe());
    REQUIRE(imported_batch.ok());
    auto ids = std::static_pointer_cast<arrow::Int64Array>(
        imported_batch.ValueUnsafe()->column(id_index));
    auto row_ids = std::static_pointer_cast<arrow::UInt64Array>(
        imported_batch.ValueUnsafe()->column(row_id_index));
    REQUIRE(ids->length() == row_ids->length());
    for (int64_t row = 0; row < ids->length(); row++) {
      REQUIRE_FALSE(ids->IsNull(row));
      REQUIRE_FALSE(row_ids->IsNull(row));
      pairs.emplace_back(ids->Value(row), row_ids->Value(row));
    }
  }

  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);
  return pairs;
}

static uint64_t row_id_for(
    const std::vector<std::pair<int64_t, uint64_t>>& pairs, int64_t id) {
  for (const auto& [entry_id, row_id] : pairs) {
    if (entry_id == id) {
      return row_id;
    }
  }
  FAIL("missing row id for test id " << id);
  return 0;
}

// Import an owned blob result, then release the Arrow C Data structs. The
// imported array keeps its own buffers and does not depend on the table.
static std::shared_ptr<arrow::Array> import_blob_array(
    FFI_ArrowArray* array, FFI_ArrowSchema* schema) {
  auto imported = arrow::ImportArray(
      reinterpret_cast<ArrowArray*>(array), reinterpret_cast<ArrowSchema*>(schema));
  REQUIRE(imported.ok());
  auto result = imported.ValueUnsafe();
  lancedb_free_arrow_array(array);
  lancedb_free_arrow_schema(schema);
  return result;
}

static std::shared_ptr<arrow::LargeBinaryArray> fetch_blob_array(
    LanceDBTable* table,
    const std::string& column,
    const std::vector<uint64_t>& row_ids) {
  FFI_ArrowArray* array = nullptr;
  FFI_ArrowSchema* schema = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_fetch_blobs(
      table, column.c_str(), row_ids.empty() ? nullptr : row_ids.data(), row_ids.size(),
      &array, &schema, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(array != nullptr);
  REQUIRE(schema != nullptr);
  auto imported = import_blob_array(array, schema);
  REQUIRE(imported->type()->Equals(arrow::large_binary()));
  return std::static_pointer_cast<arrow::LargeBinaryArray>(imported);
}

static std::shared_ptr<arrow::LargeBinaryArray> fetch_blob_range_array(
    LanceDBTable* table,
    const std::string& column,
    const LanceDBBlobRangeRequest* requests,
    size_t num_requests) {
  FFI_ArrowArray* array = nullptr;
  FFI_ArrowSchema* schema = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_fetch_blob_ranges(
      table, column.c_str(), requests, num_requests, &array, &schema, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(array != nullptr);
  REQUIRE(schema != nullptr);
  auto imported = import_blob_array(array, schema);
  REQUIRE(imported->type()->Equals(arrow::large_binary()));
  return std::static_pointer_cast<arrow::LargeBinaryArray>(imported);
}

static std::shared_ptr<arrow::Schema> read_table_schema(LanceDBTable* table) {
  FFI_ArrowSchema* schema_out = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_arrow_schema(table, &schema_out, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(schema_out != nullptr);

  auto imported_schema = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(schema_out));
  REQUIRE(imported_schema.ok());
  auto schema = imported_schema.ValueUnsafe();
  lancedb_free_arrow_schema(schema_out);
  return schema;
}

static std::shared_ptr<arrow::Field> schema_field(
    const std::shared_ptr<arrow::Schema>& schema, const std::string& name) {
  for (const auto& field : schema->fields()) {
    if (field->name() == name) {
      return field;
    }
  }
  return nullptr;
}

// Count the rows matching a SQL predicate through the public query API.
static size_t count_matching_rows(LanceDBTable* table, const std::string& filter) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);

  char* error_message = nullptr;
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

  size_t rows = 0;
  for (size_t i = 0; i < count; i++) {
    rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
  }

  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);
  return rows;
}

// Return one user metadata value from a field, or an empty string when the
// key is absent.
static std::string field_metadata_value(
    const std::shared_ptr<arrow::Field>& field, const std::string& key) {
  if (field == nullptr || field->metadata() == nullptr) {
    return {};
  }
  auto value = field->metadata()->Get(key);
  return value.ok() ? *value : std::string();
}

static bool field_metadata_has_key(
    const std::shared_ptr<arrow::Field>& field, const std::string& key) {
  return field != nullptr && field->metadata() != nullptr &&
         field->metadata()->Get(key).ok();
}

// Read every non-null value of an int64 column matching a SQL predicate through
// the public query API. The type check makes a cast that only changes the table
// schema, without rewriting the stored data, fail here.
static std::vector<int64_t> query_int64_values(
    LanceDBTable* table, const std::string& filter, const std::string& column) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);

  char* error_message = nullptr;
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
  REQUIRE(count > 0);

  auto imported_schema = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(result_schema));
  REQUIRE(imported_schema.ok());
  auto column_index = imported_schema.ValueUnsafe()->GetFieldIndex(column);
  REQUIRE(column_index >= 0);

  std::vector<int64_t> values;
  for (size_t i = 0; i < count; i++) {
    auto imported_batch = arrow::ImportRecordBatch(
        reinterpret_cast<ArrowArray*>(result_arrays[i]), imported_schema.ValueUnsafe());
    REQUIRE(imported_batch.ok());
    auto array = imported_batch.ValueUnsafe()->column(column_index);
    REQUIRE(array->type_id() == arrow::Type::INT64);
    auto int64_array = std::static_pointer_cast<arrow::Int64Array>(array);
    for (int64_t row = 0; row < int64_array->length(); row++) {
      REQUIRE_FALSE(int64_array->IsNull(row));
      values.push_back(int64_array->Value(row));
    }
  }

  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);
  return values;
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Schema Evolution", "[table]") {
  const std::string table_name = "schema_evolution_test";
  auto schema = create_schema_evolution_schema();
  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_create(
      db, table_name.c_str(), reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      create_schema_evolution_reader(), &table, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  const auto initial_version = lancedb_table_version(table);
  REQUIRE(initial_version == 1);
  REQUIRE(lancedb_table_count_rows(table) == 5);

  SECTION("Add columns evaluates SQL expressions and returns the commit version") {
    const char* names[] = {"score_x2", "key_upper"};
    const char* expressions[] = {"score * 2", "upper(key)"};
    uint64_t version = 0;

    REQUIRE(lancedb_table_add_columns(
        table, names, expressions, 2, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == initial_version + 1);
    REQUIRE(version == lancedb_table_version(table));

    auto updated_schema = read_table_schema(table);
    REQUIRE(schema_field(updated_schema, "score_x2") != nullptr);
    REQUIRE(schema_field(updated_schema, "key_upper") != nullptr);
    REQUIRE(schema_field(updated_schema, "key") != nullptr);
    REQUIRE(schema_field(updated_schema, "score") != nullptr);

    REQUIRE(count_matching_rows(table, "score_x2 = 40") == 1);
    REQUIRE(count_matching_rows(table, "key_upper = 'KEY_1'") == 1);
    REQUIRE(count_matching_rows(table, "key_upper = 'key_1'") == 0);
    REQUIRE(count_matching_rows(table, "score_x2 = 40 AND key_upper = 'KEY_1'") == 1);
    REQUIRE(lancedb_table_count_rows(table) == 5);
  }

  SECTION("Add columns rejects invalid input and preserves the SDK error") {
    const char* names[] = {"score_x2"};
    const char* expressions[] = {"score * 2"};
    const char* null_names[] = {nullptr};
    const char* null_expressions[] = {nullptr};
    const char* empty_names[] = {""};
    const char* whitespace_names[] = {"   "};
    const char* empty_expressions[] = {""};
    const char* invalid_names[] = {"\xff\xfe"};
    const char* invalid_expressions[] = {"\xff\xfe"};
    const char* duplicate_names[] = {"score"};
    const char* duplicate_expressions[] = {"score + 1"};
    uint64_t version = 42;

    REQUIRE(lancedb_table_add_columns(
        nullptr, names, expressions, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, nullptr, expressions, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, names, nullptr, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, names, expressions, 0, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, names, expressions, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, null_names, expressions, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, names, null_expressions, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, empty_names, expressions, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, whitespace_names, expressions, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, names, empty_expressions, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, invalid_names, expressions, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns(
        table, names, invalid_expressions, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);

    const char* bad_expressions[] = {"no_such_column + 1"};
    REQUIRE(lancedb_table_add_columns(
        table, names, bad_expressions, 1, &version, &error_message)
        == LANCEDB_COLUMN_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("no_such_column") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_add_columns(
        table, duplicate_names, duplicate_expressions, 1, &version, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("already exists") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(version == 42);
    REQUIRE(lancedb_table_version(table) == initial_version);
    REQUIRE(lancedb_table_count_rows(table) == 5);
  }

  SECTION("Alter columns renames a column and preserves its rows") {
    LanceDBColumnAlteration alterations[] = {
        {"key", "id", /*set_nullable=*/0, /*nullable=*/0},
    };
    uint64_t version = 0;

    REQUIRE(lancedb_table_alter_columns(
        table, alterations, 1, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == initial_version + 1);
    REQUIRE(version == lancedb_table_version(table));

    auto updated_schema = read_table_schema(table);
    REQUIRE(schema_field(updated_schema, "key") == nullptr);
    auto id_field = schema_field(updated_schema, "id");
    REQUIRE(id_field != nullptr);
    REQUIRE_FALSE(id_field->nullable());
    REQUIRE(schema_field(updated_schema, "score") != nullptr);

    REQUIRE(count_matching_rows(table, "id = 'key_1'") == 1);
    REQUIRE(lancedb_table_count_rows(table) == 5);
  }

  SECTION("Alter columns distinguishes omitted and explicit nullability") {
    // Rename with the nullable field present but not applied.
    LanceDBColumnAlteration rename_only[] = {
        {"key", "id", /*set_nullable=*/0, /*nullable=*/1},
    };
    uint64_t version = 0;
    REQUIRE(lancedb_table_alter_columns(
        table, rename_only, 1, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    auto renamed_schema = read_table_schema(table);
    auto id_field = schema_field(renamed_schema, "id");
    REQUIRE(id_field != nullptr);
    REQUIRE_FALSE(id_field->nullable());

    // Explicitly loosen score from NOT NULL to NULL.
    LanceDBColumnAlteration loosen[] = {
        {"score", nullptr, /*set_nullable=*/1, /*nullable=*/1},
    };
    const auto version_before_loosen = lancedb_table_version(table);
    REQUIRE(lancedb_table_alter_columns(
        table, loosen, 1, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == version_before_loosen + 1);
    auto loosened_schema = read_table_schema(table);
    auto score_field = schema_field(loosened_schema, "score");
    REQUIRE(score_field != nullptr);
    REQUIRE(score_field->nullable());

    // Explicitly tighten score, which has no NULL values.
    LanceDBColumnAlteration tighten_score[] = {
        {"score", nullptr, /*set_nullable=*/1, /*nullable=*/0},
    };
    const auto version_before_tighten = lancedb_table_version(table);
    REQUIRE(lancedb_table_alter_columns(
        table, tighten_score, 1, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == version_before_tighten + 1);
    auto tightened_schema = read_table_schema(table);
    auto tightened_score_field = schema_field(tightened_schema, "score");
    REQUIRE(tightened_score_field != nullptr);
    REQUIRE_FALSE(tightened_score_field->nullable());

    // Explicitly tightening note is rejected because one row stores NULL.
    LanceDBColumnAlteration tighten_note[] = {
        {"note", nullptr, /*set_nullable=*/1, /*nullable=*/0},
    };
    const auto version_before_rejected_tighten = lancedb_table_version(table);
    uint64_t failed_version = 42;
    REQUIRE(lancedb_table_alter_columns(
        table, tighten_note, 1, &failed_version, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    REQUIRE(failed_version == 42);
    REQUIRE(lancedb_table_version(table) == version_before_rejected_tighten);
  }

  SECTION("Alter columns rejects invalid and no-op input") {
    const char* new_name = "renamed";
    LanceDBColumnAlteration valid[] = {{"score", new_name, 0, 0}};
    LanceDBColumnAlteration no_op[] = {{"score", nullptr, 0, 1}};
    LanceDBColumnAlteration empty_path[] = {{"", new_name, 0, 0}};
    LanceDBColumnAlteration whitespace_path[] = {{"   ", new_name, 0, 0}};
    LanceDBColumnAlteration empty_rename[] = {{"score", "", 0, 0}};
    LanceDBColumnAlteration whitespace_rename[] = {{"score", "   ", 0, 0}};
    LanceDBColumnAlteration null_path[] = {{nullptr, new_name, 0, 0}};
    LanceDBColumnAlteration invalid_path[] = {{"\xff\xfe", new_name, 0, 0}};
    LanceDBColumnAlteration invalid_rename[] = {{"score", "\xff\xfe", 0, 0}};
    LanceDBColumnAlteration invalid_presence[] = {{"score", nullptr, 2, 1}};
    uint64_t version = 42;

    REQUIRE(lancedb_table_alter_columns(
        nullptr, valid, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, nullptr, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, valid, 0, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, valid, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, no_op, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, empty_path, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, whitespace_path, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, empty_rename, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, whitespace_rename, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, null_path, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, invalid_path, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, invalid_rename, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns(
        table, invalid_presence, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);

    // Unknown columns reach the SDK and surface its error.
    LanceDBColumnAlteration unknown[] = {{"ghost", new_name, 0, 0}};
    REQUIRE(lancedb_table_alter_columns(
        table, unknown, 1, &version, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(version == 42);
    REQUIRE(lancedb_table_version(table) == initial_version);
  }

  SECTION("Drop columns removes the named columns and preserves rows") {
    const char* columns[] = {"score", "note"};
    uint64_t version = 0;

    REQUIRE(lancedb_table_drop_columns(
        table, columns, 2, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == initial_version + 1);
    REQUIRE(version == lancedb_table_version(table));

    auto updated_schema = read_table_schema(table);
    REQUIRE(updated_schema->num_fields() == 1);
    REQUIRE(schema_field(updated_schema, "key") != nullptr);
    REQUIRE(schema_field(updated_schema, "score") == nullptr);
    REQUIRE(schema_field(updated_schema, "note") == nullptr);

    REQUIRE(count_matching_rows(table, "key = 'key_2'") == 1);
    REQUIRE(lancedb_table_count_rows(table) == 5);
  }

  SECTION("Drop columns rejects invalid input and unknown columns") {
    const char* columns[] = {"score"};
    const char* null_columns[] = {nullptr};
    const char* empty_columns[] = {""};
    const char* whitespace_columns[] = {"   "};
    const char* invalid_columns[] = {"\xff\xfe"};
    const char* unknown_columns[] = {"ghost"};
    uint64_t version = 42;

    REQUIRE(lancedb_table_drop_columns(
        nullptr, columns, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_drop_columns(
        table, nullptr, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_drop_columns(
        table, columns, 0, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_drop_columns(
        table, columns, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_drop_columns(
        table, null_columns, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_drop_columns(
        table, empty_columns, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_drop_columns(
        table, whitespace_columns, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_drop_columns(
        table, invalid_columns, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);

    REQUIRE(lancedb_table_drop_columns(
        table, unknown_columns, 1, &version, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(version == 42);
    REQUIRE(lancedb_table_version(table) == initial_version);
    REQUIRE(lancedb_table_count_rows(table) == 5);
  }


  SECTION("Add all-null typed columns from an Arrow schema") {
    auto all_nulls_schema = arrow::schema({
        arrow::field("all_null_tags", arrow::utf8(), /*nullable=*/true,
                     arrow::key_value_metadata({"unit"}, {"label"})),
        arrow::field("all_null_scores", arrow::list(arrow::int32()), /*nullable=*/true),
        arrow::field("all_null_embedding",
                     arrow::fixed_size_list(arrow::float32(), 4), /*nullable=*/true),
    });
    struct ArrowSchema c_all_nulls_schema;
    REQUIRE(arrow::ExportSchema(*all_nulls_schema, &c_all_nulls_schema).ok());

    uint64_t version = 0;
    REQUIRE(lancedb_table_add_columns_all_nulls(
        table, reinterpret_cast<FFI_ArrowSchema*>(&c_all_nulls_schema),
        &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == initial_version + 1);
    REQUIRE(version == lancedb_table_version(table));
    REQUIRE(c_all_nulls_schema.release != nullptr);
    if (c_all_nulls_schema.release) {
      c_all_nulls_schema.release(&c_all_nulls_schema);
    }

    auto updated_schema = read_table_schema(table);
    auto tags_field = schema_field(updated_schema, "all_null_tags");
    REQUIRE(tags_field != nullptr);
    REQUIRE(tags_field->type()->Equals(arrow::utf8()));
    REQUIRE(tags_field->nullable());
    REQUIRE(field_metadata_value(tags_field, "unit") == "label");
    auto scores_field = schema_field(updated_schema, "all_null_scores");
    REQUIRE(scores_field != nullptr);
    REQUIRE(scores_field->type()->Equals(arrow::list(arrow::int32())));
    REQUIRE(scores_field->nullable());
    auto embedding_field = schema_field(updated_schema, "all_null_embedding");
    REQUIRE(embedding_field != nullptr);
    REQUIRE(embedding_field->type()->Equals(arrow::fixed_size_list(arrow::float32(), 4)));
    REQUIRE(embedding_field->nullable());

    // The metadata-only commit must make every existing row read NULL.
    REQUIRE(count_matching_rows(table, "all_null_tags IS NULL") == 5);
    REQUIRE(count_matching_rows(table, "all_null_scores IS NULL") == 5);
    REQUIRE(count_matching_rows(table, "all_null_embedding IS NULL") == 5);
    REQUIRE(lancedb_table_count_rows(table) == 5);
  }

  SECTION("Add all-null columns rejects invalid schemas without committing") {
    uint64_t version = 42;
    REQUIRE(lancedb_table_add_columns_all_nulls(
        nullptr, nullptr, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_add_columns_all_nulls(
        table, nullptr, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(version == 42);

    auto empty_schema = arrow::schema({});
    struct ArrowSchema c_empty_schema;
    REQUIRE(arrow::ExportSchema(*empty_schema, &c_empty_schema).ok());
    REQUIRE(lancedb_table_add_columns_all_nulls(
        table, reinterpret_cast<FFI_ArrowSchema*>(&c_empty_schema), &version,
        &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    REQUIRE(c_empty_schema.release != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    REQUIRE(lancedb_table_add_columns_all_nulls(
        table, reinterpret_cast<FFI_ArrowSchema*>(&c_empty_schema), nullptr,
        nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(c_empty_schema.release != nullptr);
    if (c_empty_schema.release) {
      c_empty_schema.release(&c_empty_schema);
    }

    auto non_nullable_schema = arrow::schema({
        arrow::field("not_nullable", arrow::int32(), /*nullable=*/false),
    });
    struct ArrowSchema c_non_nullable_schema;
    REQUIRE(arrow::ExportSchema(*non_nullable_schema, &c_non_nullable_schema).ok());
    REQUIRE(lancedb_table_add_columns_all_nulls(
        table, reinterpret_cast<FFI_ArrowSchema*>(&c_non_nullable_schema), &version,
        &error_message) != LANCEDB_SUCCESS);
    REQUIRE(c_non_nullable_schema.release != nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("nullable") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;
    if (c_non_nullable_schema.release) {
      c_non_nullable_schema.release(&c_non_nullable_schema);
    }

    REQUIRE(version == 42);
    REQUIRE(lancedb_table_version(table) == initial_version);
    REQUIRE(lancedb_table_count_rows(table) == 5);
    REQUIRE(schema_field(read_table_schema(table), "not_nullable") == nullptr);
  }

  SECTION("Alter columns casts a column type with the commit version") {
    auto unsupported_cast = arrow::schema({
        arrow::field("score", arrow::float64()),
    });
    struct ArrowSchema c_unsupported_cast;
    REQUIRE(arrow::ExportSchema(*unsupported_cast, &c_unsupported_cast).ok());
    LanceDBColumnAlterationWithCast unsupported[] = {
        {"score", nullptr, /*set_nullable=*/0, /*nullable=*/0,
         reinterpret_cast<FFI_ArrowSchema*>(&c_unsupported_cast)},
    };
    uint64_t version = 42;
    REQUIRE(lancedb_table_alter_columns_with_cast(
        table, unsupported, 1, &version, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(c_unsupported_cast.release != nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("Cannot cast column") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;
    if (c_unsupported_cast.release) {
      c_unsupported_cast.release(&c_unsupported_cast);
    }
    REQUIRE(version == 42);
    REQUIRE(lancedb_table_version(table) == initial_version);

    auto int64_cast = arrow::schema({
        arrow::field("score", arrow::int64(), /*nullable=*/false),
    });
    struct ArrowSchema c_int64_cast;
    REQUIRE(arrow::ExportSchema(*int64_cast, &c_int64_cast).ok());
    LanceDBColumnAlterationWithCast alterations[] = {
        {"score", "score64", /*set_nullable=*/1, /*nullable=*/1,
         reinterpret_cast<FFI_ArrowSchema*>(&c_int64_cast)},
    };
    version = 0;
    REQUIRE(lancedb_table_alter_columns_with_cast(
        table, alterations, 1, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == initial_version + 1);
    REQUIRE(version == lancedb_table_version(table));
    REQUIRE(c_int64_cast.release != nullptr);
    if (c_int64_cast.release) {
      c_int64_cast.release(&c_int64_cast);
    }

    auto updated_schema = read_table_schema(table);
    REQUIRE(schema_field(updated_schema, "score") == nullptr);
    auto score64_field = schema_field(updated_schema, "score64");
    REQUIRE(score64_field != nullptr);
    REQUIRE(score64_field->type()->Equals(arrow::int64()));
    REQUIRE(score64_field->nullable());

    // The cast must rewrite the stored values, not only the schema.
    REQUIRE(query_int64_values(table, "key = 'key_1'", "score64") ==
            std::vector<int64_t>{20});
    REQUIRE(lancedb_table_count_rows(table) == 5);

    // Embedded Lance rejects a cast combined with tightening a nullable field
    // even when the data has no NULLs. Add a nullable column whose values are
    // all non-null, then ask for the cast and NOT NULL in one operation.
    const char* nullable_name[] = {"cast_tighten"};
    const char* nullable_expression[] = {"score64"};
    uint64_t add_version = 0;
    REQUIRE(lancedb_table_add_columns(
        table, nullable_name, nullable_expression, 1, &add_version, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(add_version == lancedb_table_version(table));
    auto nullable_schema = read_table_schema(table);
    auto tighten_field = schema_field(nullable_schema, "cast_tighten");
    REQUIRE(tighten_field != nullptr);
    REQUIRE(tighten_field->nullable());

    auto int32_cast = arrow::schema({
        arrow::field("cast_tighten", arrow::int32(), /*nullable=*/false),
    });
    struct ArrowSchema c_int32_cast;
    REQUIRE(arrow::ExportSchema(*int32_cast, &c_int32_cast).ok());
    LanceDBColumnAlterationWithCast tighten[] = {
        {"cast_tighten", nullptr, /*set_nullable=*/1, /*nullable=*/0,
         reinterpret_cast<FFI_ArrowSchema*>(&c_int32_cast)},
    };
    uint64_t failed_version = 42;
    REQUIRE(lancedb_table_alter_columns_with_cast(
        table, tighten, 1, &failed_version, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(c_int32_cast.release != nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("non-nullable and cast") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;
    if (c_int32_cast.release) {
      c_int32_cast.release(&c_int32_cast);
    }
    REQUIRE(failed_version == 42);
    REQUIRE(lancedb_table_version(table) == add_version);
  }

  SECTION("Alter columns with cast rejects invalid input and preserves state") {
    auto int64_cast = arrow::schema({
        arrow::field("score", arrow::int64(), /*nullable=*/false),
    });
    struct ArrowSchema c_int64_cast;
    REQUIRE(arrow::ExportSchema(*int64_cast, &c_int64_cast).ok());
    LanceDBColumnAlterationWithCast valid[] = {
        {"score", nullptr, /*set_nullable=*/0, /*nullable=*/0,
         reinterpret_cast<FFI_ArrowSchema*>(&c_int64_cast)},
    };
    LanceDBColumnAlterationWithCast no_change[] = {
        {"score", nullptr, /*set_nullable=*/0, /*nullable=*/0, nullptr},
    };

    auto two_fields = arrow::schema({
        arrow::field("first", arrow::int64()),
        arrow::field("second", arrow::int64()),
    });
    struct ArrowSchema c_two_fields;
    REQUIRE(arrow::ExportSchema(*two_fields, &c_two_fields).ok());
    LanceDBColumnAlterationWithCast two_field_cast[] = {
        {"score", nullptr, /*set_nullable=*/0, /*nullable=*/0,
         reinterpret_cast<FFI_ArrowSchema*>(&c_two_fields)},
    };

    uint64_t version = 42;
    REQUIRE(lancedb_table_alter_columns_with_cast(
        nullptr, valid, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns_with_cast(
        table, nullptr, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns_with_cast(
        table, valid, 0, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns_with_cast(
        table, valid, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns_with_cast(
        table, no_change, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_alter_columns_with_cast(
        table, two_field_cast, 1, &version, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(c_int64_cast.release != nullptr);
    REQUIRE(c_two_fields.release != nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("exactly one field") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(version == 42);
    REQUIRE(lancedb_table_version(table) == initial_version);
    REQUIRE(lancedb_table_count_rows(table) == 5);
    if (c_int64_cast.release) {
      c_int64_cast.release(&c_int64_cast);
    }
    if (c_two_fields.release) {
      c_two_fields.release(&c_two_fields);
    }
  }

  SECTION("Update field metadata merges, replaces, and clears committed state") {
    const LanceDBFieldMetadataEntry set_entries[] = {
        {"unit", "label"},
        {"pii", "false"},
    };
    LanceDBFieldMetadataUpdate set_update = {"note", set_entries, 2, /*replace=*/0};
    uint64_t version = 0;
    REQUIRE(lancedb_table_update_field_metadata(
        table, &set_update, 1, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == initial_version + 1);
    REQUIRE(version == lancedb_table_version(table));

    auto updated_schema = read_table_schema(table);
    auto note_field = schema_field(updated_schema, "note");
    REQUIRE(note_field != nullptr);
    REQUIRE(field_metadata_value(note_field, "unit") == "label");
    REQUIRE(field_metadata_value(note_field, "pii") == "false");

    const LanceDBFieldMetadataEntry merge_entries[] = {
        {"source", "import"},
        {"pii", nullptr},
    };
    LanceDBFieldMetadataUpdate merge_update = {"note", merge_entries, 2, /*replace=*/0};
    auto version_before_merge = lancedb_table_version(table);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &merge_update, 1, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == version_before_merge + 1);
    REQUIRE(version == lancedb_table_version(table));

    updated_schema = read_table_schema(table);
    note_field = schema_field(updated_schema, "note");
    REQUIRE(field_metadata_value(note_field, "unit") == "label");
    REQUIRE(field_metadata_value(note_field, "source") == "import");
    REQUIRE_FALSE(field_metadata_has_key(note_field, "pii"));

    const LanceDBFieldMetadataEntry replace_entries[] = {
        {"unit", "display"},
    };
    LanceDBFieldMetadataUpdate replace_update = {
        "note", replace_entries, 1, /*replace=*/1};
    auto version_before_replace = lancedb_table_version(table);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &replace_update, 1, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == version_before_replace + 1);
    REQUIRE(version == lancedb_table_version(table));

    updated_schema = read_table_schema(table);
    note_field = schema_field(updated_schema, "note");
    REQUIRE(field_metadata_value(note_field, "unit") == "display");
    REQUIRE_FALSE(field_metadata_has_key(note_field, "source"));
    REQUIRE_FALSE(field_metadata_has_key(note_field, "pii"));

    // Read the replacement back from a newly opened table rather than the
    // handle that issued the commit.
    LanceDBTable* reopened = open_table(table_name);
    auto persisted_schema = read_table_schema(reopened);
    auto persisted_note = schema_field(persisted_schema, "note");
    REQUIRE(field_metadata_value(persisted_note, "unit") == "display");
    REQUIRE_FALSE(field_metadata_has_key(persisted_note, "source"));
    lancedb_table_free(reopened);

    LanceDBFieldMetadataUpdate clear_update = {"note", nullptr, 0, /*replace=*/1};
    auto version_before_clear = lancedb_table_version(table);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &clear_update, 1, &version, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(version == version_before_clear + 1);
    REQUIRE(version == lancedb_table_version(table));

    updated_schema = read_table_schema(table);
    note_field = schema_field(updated_schema, "note");
    REQUIRE_FALSE(field_metadata_has_key(note_field, "unit"));
    REQUIRE_FALSE(field_metadata_has_key(note_field, "source"));
    REQUIRE_FALSE(field_metadata_has_key(note_field, "pii"));

    reopened = open_table(table_name);
    persisted_schema = read_table_schema(reopened);
    persisted_note = schema_field(persisted_schema, "note");
    REQUIRE_FALSE(field_metadata_has_key(persisted_note, "unit"));
    lancedb_table_free(reopened);
  }

  SECTION("Update field metadata rejects invalid input and backend errors") {
    const LanceDBFieldMetadataEntry valid_entries[] = {
        {"unit", "label"},
    };
    LanceDBFieldMetadataUpdate valid = {"note", valid_entries, 1, /*replace=*/0};
    const LanceDBFieldMetadataEntry duplicate_entries[] = {
        {"unit", "label"},
        {"unit", "other"},
    };
    LanceDBFieldMetadataUpdate duplicate = {
        "note", duplicate_entries, 2, /*replace=*/0};
    LanceDBFieldMetadataUpdate merge_empty = {
        "note", nullptr, 0, /*replace=*/0};
    LanceDBFieldMetadataUpdate entries_missing = {
        "note", nullptr, 1, /*replace=*/0};
    LanceDBFieldMetadataUpdate null_path = {
        nullptr, valid_entries, 1, /*replace=*/0};
    LanceDBFieldMetadataUpdate empty_path = {
        "", valid_entries, 1, /*replace=*/0};
    LanceDBFieldMetadataUpdate whitespace_path = {
        "   ", valid_entries, 1, /*replace=*/0};
    const char* invalid_path_text = "\xff\xfe";
    LanceDBFieldMetadataUpdate invalid_path = {
        invalid_path_text, valid_entries, 1, /*replace=*/0};
    const LanceDBFieldMetadataEntry null_key_entries[] = {
        {nullptr, "label"},
    };
    LanceDBFieldMetadataUpdate null_key = {
        "note", null_key_entries, 1, /*replace=*/0};
    const LanceDBFieldMetadataEntry empty_key_entries[] = {
        {"", "label"},
    };
    LanceDBFieldMetadataUpdate empty_key = {
        "note", empty_key_entries, 1, /*replace=*/0};
    const LanceDBFieldMetadataEntry invalid_key_entries[] = {
        {"\xff\xfe", "label"},
    };
    LanceDBFieldMetadataUpdate invalid_key = {
        "note", invalid_key_entries, 1, /*replace=*/0};
    const LanceDBFieldMetadataEntry invalid_value_entries[] = {
        {"unit", "\xff\xfe"},
    };
    LanceDBFieldMetadataUpdate invalid_value = {
        "note", invalid_value_entries, 1, /*replace=*/0};
    const LanceDBFieldMetadataEntry ghost_entries[] = {
        {"unit", "label"},
    };
    LanceDBFieldMetadataUpdate ghost = {
        "ghost", ghost_entries, 1, /*replace=*/0};

    uint64_t version = 42;
    REQUIRE(lancedb_table_update_field_metadata(
        nullptr, &valid, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, nullptr, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &valid, 0, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &valid, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &null_path, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &empty_path, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &whitespace_path, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &invalid_path, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &merge_empty, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &entries_missing, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &null_key, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &empty_key, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &invalid_key, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &invalid_value, 1, &version, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_update_field_metadata(
        table, &duplicate, 1, &version, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("unique") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_update_field_metadata(
        table, &ghost, 1, &version, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(version == 42);
    REQUIRE(lancedb_table_version(table) == initial_version);
    REQUIRE(lancedb_table_count_rows(table) == 5);
    auto note_field = schema_field(read_table_schema(table), "note");
    REQUIRE_FALSE(field_metadata_has_key(note_field, "unit"));
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table - branch lifecycle", "[table][branch]") {
  const std::string table_name = "test_branch_lifecycle";
  constexpr int main_rows = 3;
  LanceDBTable* main_table = create_table_with_data(table_name, main_rows, 0);
  const uint64_t main_version = lancedb_table_version(main_table);

  char* error_message = nullptr;
  char* current_branch = nullptr;
  REQUIRE(lancedb_table_current_branch(main_table, &current_branch, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(current_branch == nullptr);

  const LanceDBBranchSource source = {
      .kind = LANCEDB_BRANCH_SOURCE_VERSION,
      .name = nullptr,
      .has_version = 1,
      .version = main_version};
  LanceDBTable* branch = nullptr;
  REQUIRE(lancedb_table_create_branch(main_table, "exp", &source, &branch, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(branch != nullptr);

  REQUIRE(lancedb_table_current_branch(branch, &current_branch, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(current_branch != nullptr);
  REQUIRE(std::string(current_branch) == "exp");
  lancedb_free_string(current_branch);
  current_branch = nullptr;

  REQUIRE(lancedb_table_count_rows(branch) == main_rows);
  const uint64_t branch_fork_version = lancedb_table_version(branch);

  // A write on the branch must not leak into main.
  auto branch_batch = create_test_record_batch(2, main_rows);
  auto branch_reader = create_reader_from_batch(branch_batch);
  REQUIRE(lancedb_table_add(branch, branch_reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(branch) == main_rows + 2);
  REQUIRE(lancedb_table_count_rows(main_table) == main_rows);
  REQUIRE(first_data_value(branch, "key_3") == 30.0F);
  REQUIRE(count_matching_rows(main_table, "key = 'key_3'") == 0);

  // The source handle keeps its own branch and stays writable.
  auto main_batch = create_test_record_batch(1, 100);
  auto main_reader = create_reader_from_batch(main_batch);
  REQUIRE(lancedb_table_add(main_table, main_reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_version(main_table) == main_version + 1);
  REQUIRE(lancedb_table_count_rows(main_table) == main_rows + 1);
  REQUIRE(lancedb_table_count_rows(branch) == main_rows + 2);
  REQUIRE(first_data_value(main_table, "key_100") == 1000.0F);
  REQUIRE(count_matching_rows(branch, "key = 'key_100'") == 0);

  // A pinned checkout sees the branch at the fork version and is read-only.
  const LanceDBBranchCheckoutOptions pinned_options = {
      .has_version = 1,
      .version = branch_fork_version};
  LanceDBTable* pinned = nullptr;
  REQUIRE(lancedb_table_checkout_branch(main_table, "exp", &pinned_options, &pinned, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(pinned != nullptr);
  REQUIRE(lancedb_table_count_rows(pinned) == main_rows);
  REQUIRE(count_matching_rows(pinned, "key = 'key_3'") == 0);

  REQUIRE(lancedb_table_current_branch(pinned, &current_branch, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(current_branch != nullptr);
  REQUIRE(std::string(current_branch) == "exp");
  lancedb_free_string(current_branch);
  current_branch = nullptr;

  auto pinned_batch = create_test_record_batch(1, 200);
  auto pinned_reader = create_reader_from_batch(pinned_batch);
  REQUIRE(lancedb_table_add(pinned, pinned_reader, &error_message) != LANCEDB_SUCCESS);
  REQUIRE(error_message != nullptr);
  lancedb_free_string(error_message);
  error_message = nullptr;
  REQUIRE(lancedb_table_count_rows(branch) == main_rows + 2);

  // Without a version the checkout follows the branch head.
  const LanceDBBranchCheckoutOptions head_options = {.has_version = 0, .version = 0};
  LanceDBTable* branch_head = nullptr;
  REQUIRE(lancedb_table_checkout_branch(main_table, "exp", &head_options, &branch_head, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(branch_head != nullptr);
  REQUIRE(lancedb_table_count_rows(branch_head) == main_rows + 2);
  REQUIRE(first_data_value(branch_head, "key_3") == 30.0F);

  const LanceDBBranch* branches = nullptr;
  size_t branch_count = 0;
  REQUIRE(lancedb_table_list_branches(main_table, &branches, &branch_count, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(branch_count == 1);
  REQUIRE(branches != nullptr);
  REQUIRE(std::string(branches[0].name) == "exp");
  REQUIRE(branches[0].parent_branch == nullptr);
  REQUIRE(branches[0].parent_version == main_version);
  REQUIRE(branches[0].created_at > 0);
  REQUIRE(branches[0].manifest_size > 0);
  REQUIRE(branches[0].metadata_count == 0);
  REQUIRE(branches[0].metadata_keys == nullptr);
  REQUIRE(branches[0].metadata_values == nullptr);
  lancedb_free_branches(branches, branch_count);

  REQUIRE(lancedb_table_delete_branch(main_table, "exp", &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  branches = nullptr;
  branch_count = 0;
  REQUIRE(lancedb_table_list_branches(main_table, &branches, &branch_count, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(branch_count == 0);
  REQUIRE(branches == nullptr);
  lancedb_free_branches(branches, branch_count);

  LanceDBTable* missing = nullptr;
  REQUIRE(lancedb_table_checkout_branch(main_table, "exp", &head_options, &missing, &error_message)
      != LANCEDB_SUCCESS);
  REQUIRE(missing == nullptr);
  REQUIRE(error_message != nullptr);
  lancedb_free_string(error_message);
  error_message = nullptr;

  lancedb_table_free(branch_head);
  lancedb_table_free(pinned);
  lancedb_table_free(branch);
  lancedb_table_free(main_table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table - branch sources", "[table][branch]") {
  const std::string table_name = "test_branch_sources";
  constexpr int tagged_rows = 2;
  LanceDBTable* table = create_table_with_data(table_name, tagged_rows, 0);
  const uint64_t tagged_version = lancedb_table_version(table);

  char* error_message = nullptr;
  REQUIRE(lancedb_table_create_tag(table, "v1", tagged_version, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  // Advance main after the tag so a tag source is observably older than the head.
  auto later_batch = create_test_record_batch(2, tagged_rows);
  auto later_reader = create_reader_from_batch(later_batch);
  REQUIRE(lancedb_table_add(table, later_reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == tagged_rows + 2);

  const LanceDBBranchSource tag_source = {
      .kind = LANCEDB_BRANCH_SOURCE_TAG,
      .name = "v1",
      .has_version = 0,
      .version = 0};
  LanceDBTable* tagged = nullptr;
  REQUIRE(lancedb_table_create_branch(table, "tagged", &tag_source, &tagged, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(tagged != nullptr);
  REQUIRE(lancedb_table_count_rows(tagged) == tagged_rows);
  REQUIRE(count_matching_rows(tagged, "key = 'key_2'") == 0);

  const uint64_t tagged_fork_version = lancedb_table_version(tagged);
  auto tagged_batch = create_test_record_batch(1, 10);
  auto tagged_reader = create_reader_from_batch(tagged_batch);
  REQUIRE(lancedb_table_add(tagged, tagged_reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(tagged) == tagged_rows + 1);
  REQUIRE(first_data_value(tagged, "key_10") == 100.0F);

  // A branch source without a version follows its head.
  const LanceDBBranchSource head_source = {
      .kind = LANCEDB_BRANCH_SOURCE_BRANCH,
      .name = "tagged",
      .has_version = 0,
      .version = 0};
  LanceDBTable* head_copy = nullptr;
  REQUIRE(lancedb_table_create_branch(table, "head_copy", &head_source, &head_copy, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(head_copy != nullptr);
  REQUIRE(lancedb_table_count_rows(head_copy) == tagged_rows + 1);
  REQUIRE(first_data_value(head_copy, "key_10") == 100.0F);

  // A branch source with a version pins the fork point.
  const LanceDBBranchSource pinned_source = {
      .kind = LANCEDB_BRANCH_SOURCE_BRANCH,
      .name = "tagged",
      .has_version = 1,
      .version = tagged_fork_version};
  LanceDBTable* pinned_copy = nullptr;
  REQUIRE(lancedb_table_create_branch(table, "pinned_copy", &pinned_source, &pinned_copy, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(pinned_copy != nullptr);
  REQUIRE(lancedb_table_count_rows(pinned_copy) == tagged_rows);
  REQUIRE(count_matching_rows(pinned_copy, "key = 'key_10'") == 0);

  // Raw values outside the public enum are rejected before reaching the SDK.
  // The other source fields are valid, so only the kind can cause the failure.
  LanceDBTable* invalid = nullptr;
  REQUIRE(lancedb_test_create_branch_with_source_kind(
      table, "invalid", 999, "v1", 1, tagged_version, &invalid, &error_message)
      == LANCEDB_INVALID_ARGUMENT);
  REQUIRE(invalid == nullptr);
  REQUIRE(error_message != nullptr);
  REQUIRE(std::string(error_message).find("branch source kind") != std::string::npos);
  lancedb_free_string(error_message);
  error_message = nullptr;

  const LanceDBBranchSource missing_tag_name = {
      .kind = LANCEDB_BRANCH_SOURCE_TAG,
      .name = nullptr,
      .has_version = 0,
      .version = 0};
  invalid = nullptr;
  REQUIRE(lancedb_table_create_branch(table, "invalid", &missing_tag_name, &invalid, &error_message)
      == LANCEDB_INVALID_ARGUMENT);
  REQUIRE(invalid == nullptr);
  REQUIRE(error_message != nullptr);
  lancedb_free_string(error_message);
  error_message = nullptr;

  const LanceDBBranchSource missing_source_version = {
      .kind = LANCEDB_BRANCH_SOURCE_VERSION,
      .name = nullptr,
      .has_version = 0,
      .version = 0};
  invalid = nullptr;
  REQUIRE(lancedb_table_create_branch(table, "invalid", &missing_source_version, &invalid, &error_message)
      == LANCEDB_INVALID_ARGUMENT);
  REQUIRE(invalid == nullptr);
  REQUIRE(error_message != nullptr);
  lancedb_free_string(error_message);
  error_message = nullptr;

  lancedb_table_free(pinned_copy);
  lancedb_table_free(head_copy);
  lancedb_table_free(tagged);
  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Blob Array Reading", "[table][blob]") {
  const std::vector<int64_t> ids = {1, 2, 3, 4};
  const std::vector<std::optional<std::string>> payloads = {
      std::string("alpha"),
      std::nullopt,
      std::string(),
      std::string("pay\0load", 8),
  };
  auto make_blob_table = [&](const std::string& table_name) {
    auto schema = arrow::schema({
        arrow::field("id", arrow::int64(), /*nullable=*/false),
        blob_v2_field("image"),
    });
    LanceDBTable* table = create_table_with_schema(db, table_name, schema);
    auto batch = create_blob_input_batch(ids, payloads);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == ids.size());
    return table;
  };

  SECTION("blob_columns returns declaration order and owns its names") {
    auto schema = arrow::schema({
        blob_v2_field("thumbnail"),
        arrow::field("id", arrow::int64(), /*nullable=*/false),
        arrow::field("info", arrow::struct_(arrow::FieldVector{
            arrow::field("name", arrow::utf8(), /*nullable=*/true),
            blob_v2_field("blob"),
        }), /*nullable=*/true),
        blob_v2_field("image"),
    });
    LanceDBTable* table = create_table_with_schema(db, "blob_columns", schema);

    char** columns = nullptr;
    size_t count = 0;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_blob_columns(table, &columns, &count, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 3);
    REQUIRE(std::string(columns[0]) == "thumbnail");
    REQUIRE(std::string(columns[1]) == "info.blob");
    REQUIRE(std::string(columns[2]) == "image");

    // The names are independent of the table handle.
    lancedb_table_free(table);
    REQUIRE(std::string(columns[0]) == "thumbnail");
    REQUIRE(std::string(columns[2]) == "image");
    lancedb_free_blob_columns(columns, count);

    auto plain_schema = arrow::schema({
        arrow::field("id", arrow::int64(), /*nullable=*/false),
    });
    LanceDBTable* plain = create_table_with_schema(db, "blob_columns_plain", plain_schema);
    columns = reinterpret_cast<char**>(1);
    count = 99;
    REQUIRE(lancedb_table_blob_columns(plain, &columns, &count, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(columns == nullptr);
    REQUIRE(count == 0);
    lancedb_free_blob_columns(columns, count);

    REQUIRE(lancedb_table_blob_columns(nullptr, &columns, &count, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(columns == nullptr);
    REQUIRE(count == 0);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    lancedb_table_free(plain);
  }

  SECTION("fetch_blobs preserves SDK alignment and independent lifetime") {
    LanceDBTable* table = make_blob_table("blob_fetch");
    const auto pairs = query_id_row_ids(table);
    const auto row_1 = row_id_for(pairs, 1);
    const auto row_2 = row_id_for(pairs, 2);
    const auto row_3 = row_id_for(pairs, 3);
    const auto row_4 = row_id_for(pairs, 4);

    auto blobs = fetch_blob_array(table, "image", {row_4, row_1, row_2, row_3, row_1});
    REQUIRE(blobs->length() == 5);
    REQUIRE(blobs->GetView(0) == std::string_view("pay\0load", 8));
    REQUIRE(blobs->GetView(1) == std::string_view("alpha"));
    REQUIRE(blobs->IsNull(2));
    REQUIRE_FALSE(blobs->IsNull(3));
    REQUIRE(blobs->GetView(3).empty());
    REQUIRE(blobs->GetView(4) == std::string_view("alpha"));

    auto empty = fetch_blob_array(table, "image", {});
    REQUIRE(empty->length() == 0);

    FFI_ArrowArray* detached_array = nullptr;
    FFI_ArrowSchema* detached_schema = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_fetch_blobs(
        table, "image", &row_4, 1, &detached_array, &detached_schema, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(detached_array != nullptr);
    REQUIRE(detached_schema != nullptr);

    // The fetched result keeps its own buffers after the table is gone.
    lancedb_table_free(table);
    table = nullptr;
    auto detached = import_blob_array(detached_array, detached_schema);
    auto detached_blobs = std::static_pointer_cast<arrow::LargeBinaryArray>(detached);
    REQUIRE(detached_blobs->length() == 1);
    REQUIRE(detached_blobs->GetView(0) == std::string_view("pay\0load", 8));
  }

  SECTION("fetch_blobs reports invalid input and SDK errors without partial outputs") {
    LanceDBTable* table = make_blob_table("blob_fetch_errors");
    const auto pairs = query_id_row_ids(table);
    const auto row_1 = row_id_for(pairs, 1);

    FFI_ArrowArray* array = nullptr;
    FFI_ArrowSchema* schema = nullptr;
    char* error_message = nullptr;

    REQUIRE(lancedb_table_fetch_blobs(
        table, "missing", nullptr, 0, &array, &schema, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(array == nullptr);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("no column named 'missing'") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_fetch_blobs(
        table, "id", &row_1, 1, &array, &schema, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(array == nullptr);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("not a blob column") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const uint64_t missing_row = UINT64_MAX;
    REQUIRE(lancedb_table_fetch_blobs(
        table, "image", &missing_row, 1, &array, &schema, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(array == nullptr);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("do not exist") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_fetch_blobs(
        table, "image", nullptr, 1, &array, &schema, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(array == nullptr);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_fetch_blobs(
        table, nullptr, &row_1, 1, &array, &schema, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(array == nullptr);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_fetch_blobs(
        table, "image", &row_1, 1, nullptr, &schema, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    lancedb_table_free(table);
  }

  SECTION("fetch_blob_ranges preserves request alignment and validates ranges") {
    LanceDBTable* table = make_blob_table("blob_ranges");
    const auto pairs = query_id_row_ids(table);
    const auto row_1 = row_id_for(pairs, 1);
    const auto row_2 = row_id_for(pairs, 2);
    const auto row_3 = row_id_for(pairs, 3);
    const auto row_4 = row_id_for(pairs, 4);

    const LanceDBBlobRangeRequest requests[] = {
        {row_4, 3, 4},
        {row_1, 1, 3},
        {row_3, 0, 0},
        {row_2, 0, 1},
        {row_1, 5, 0},
        {row_1, 0, 2},
        {row_1, 0, 2},
    };
    auto ranges = fetch_blob_range_array(table, "image", requests, std::size(requests));
    REQUIRE(ranges->length() == static_cast<int64_t>(std::size(requests)));
    REQUIRE(ranges->GetView(0) == std::string_view("\0loa", 4));
    REQUIRE(ranges->GetView(1) == std::string_view("lph"));
    REQUIRE_FALSE(ranges->IsNull(2));
    REQUIRE(ranges->GetView(2).empty());
    REQUIRE(ranges->IsNull(3));
    REQUIRE_FALSE(ranges->IsNull(4));
    REQUIRE(ranges->GetView(4).empty());
    REQUIRE(ranges->GetView(5) == std::string_view("al"));
    REQUIRE(ranges->GetView(6) == std::string_view("al"));

    auto empty = fetch_blob_range_array(table, "image", nullptr, 0);
    REQUIRE(empty->length() == 0);

    FFI_ArrowArray* array = nullptr;
    FFI_ArrowSchema* schema = nullptr;
    char* error_message = nullptr;

    // The SDK validates the column even when the request list is empty.
    REQUIRE(lancedb_table_fetch_blob_ranges(
        table, "missing", nullptr, 0, &array, &schema, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(array == nullptr);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("no column named 'missing'") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const LanceDBBlobRangeRequest overflow[] = {{row_1, UINT64_MAX, 1}};
    REQUIRE(lancedb_table_fetch_blob_ranges(
        table, "image", overflow, 1, &array, &schema, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(array == nullptr);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("overflowed") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const LanceDBBlobRangeRequest out_of_bounds[] = {{row_1, 4, 2}};
    REQUIRE(lancedb_table_fetch_blob_ranges(
        table, "image", out_of_bounds, 1, &array, &schema, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(array == nullptr);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("exceeds blob size") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const uint64_t missing_row = UINT64_MAX;
    const LanceDBBlobRangeRequest missing[] = {{missing_row, 0, 1}};
    REQUIRE(lancedb_table_fetch_blob_ranges(
        table, "image", missing, 1, &array, &schema, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(array == nullptr);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("do not exist") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_fetch_blob_ranges(
        table, "image", nullptr, 1, &array, &schema, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(array == nullptr);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    lancedb_table_free(table);
  }
}
