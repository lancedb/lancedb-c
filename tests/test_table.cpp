/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

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
    LanceDBTable* reopened_table = lancedb_connection_open_table(db, table_name.c_str());
    REQUIRE(reopened_table != nullptr);
    REQUIRE(lancedb_table_count_rows(reopened_table) == row_num);
    lancedb_table_free(reopened_table);
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
  LanceDBTable* table = lancedb_connection_open_table(db, table_name.c_str());
  REQUIRE(table != nullptr);

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

    if (error_message) {
      INFO("Error message: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

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

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
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

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
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

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(lancedb_table_count_rows(table) == row_num);

    // Add data with overlapping keys (5-14)
    // Keys 5-9 already exist in the table
    constexpr auto overlap_start = 5;
    constexpr auto overlap_count = 10;
    auto batch2 = create_test_record_batch(overlap_count, overlap_start);
    auto reader2 = create_reader_from_batch(batch2);
    REQUIRE(reader2 != nullptr);

    result = lancedb_table_add(table, reader2, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

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
  LanceDBTable* table = lancedb_connection_open_table(db, table_name.c_str());
  REQUIRE(table != nullptr);

  // Add initial data
  constexpr auto row_num = 10;
  auto initial_batch = create_test_record_batch(row_num, 0);
  auto initial_reader = create_reader_from_batch(initial_batch);
  REQUIRE(initial_reader != nullptr);

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_add(table, initial_reader, &error_message);

  if (error_message) {
    lancedb_free_string(error_message);
  }
  REQUIRE(result == LANCEDB_SUCCESS);
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
      key_builder.Append("key_" + std::to_string(i)).ok();
      auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        list_builder->Append(static_cast<float>(999 + i)).ok();  // Different values
      }
      data_builder.Append().ok();
    }

    // Add new keys (should insert)
    for (int i = 10; i < 15; i++) {
      key_builder.Append("key_" + std::to_string(i)).ok();
      auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        list_builder->Append(static_cast<float>(i * 10 + j)).ok();
      }
      data_builder.Append().ok();
    }

    std::shared_ptr<arrow::Array> key_array, data_array;
    key_builder.Finish(&key_array).ok();
    data_builder.Finish(&data_array).ok();

    auto merge_batch = arrow::RecordBatch::Make(schema, 10, {key_array, data_array});
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 1
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    if (error_message) {
      INFO("Error message: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Should have 10 (original) - 5 (overlapping) + 10 (total in merge) = 15 rows
    REQUIRE(lancedb_table_count_rows(table) == 15);

    // Version should increment to 3 (was 2 before merge insert)
    auto  version = lancedb_table_version(table);
    REQUIRE(version == 3);
  }

  SECTION("Merge insert with update only") {
    // Create data with only overlapping keys
    auto schema = create_test_schema();

    arrow::StringBuilder key_builder;
    arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
        std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

    for (int i = 0; i < 5; i++) {
      key_builder.Append("key_" + std::to_string(i)).ok();
      auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        list_builder->Append(static_cast<float>(888 + i)).ok();
      }
      data_builder.Append().ok();
    }

    std::shared_ptr<arrow::Array> key_array, data_array;
    key_builder.Finish(&key_array).ok();
    data_builder.Finish(&data_array).ok();

    auto merge_batch = arrow::RecordBatch::Make(schema, 5, {key_array, data_array});
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0  // Don't insert new rows
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Should still have 10 rows (only updates, no inserts)
    REQUIRE(lancedb_table_count_rows(table) == 10);

    // Version should increment to 3 (was 2 before merge insert)
    auto  version = lancedb_table_version(table);
    REQUIRE(version == 3);
  }

  SECTION("Merge insert with insert only") {
    // Create data with only new keys
    auto schema = create_test_schema();

    arrow::StringBuilder key_builder;
    arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
        std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

    for (int i = 20; i < 25; i++) {
      key_builder.Append("key_" + std::to_string(i)).ok();
      auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        list_builder->Append(static_cast<float>(i * 10 + j)).ok();
      }
      data_builder.Append().ok();
    }

    std::shared_ptr<arrow::Array> key_array, data_array;
    key_builder.Finish(&key_array).ok();
    data_builder.Finish(&data_array).ok();

    auto merge_batch = arrow::RecordBatch::Make(schema, 5, {key_array, data_array});
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 0,  // Don't update existing rows
      .when_not_matched_insert_all = 1
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Should have 10 + 5 = 15 rows (only inserts, no updates)
    REQUIRE(lancedb_table_count_rows(table) == 15);

    // Version should increment to 3 (was 2 before merge insert)
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

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

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
      key_builder.Append("key_" + std::to_string(i)).ok();
      auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
      for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
        list_builder->Append(static_cast<float>(i * 10 + j)).ok();
      }
      data_builder.Append().ok();
    }

    std::shared_ptr<arrow::Array> key_array, data_array;
    key_builder.Finish(&key_array).ok();
    data_builder.Finish(&data_array).ok();

    auto merge_batch = arrow::RecordBatch::Make(schema, 5, {key_array, data_array});
    auto merge_reader = create_reader_from_batch(merge_batch);
    REQUIRE(merge_reader != nullptr);

    const char* on_columns[] = {"key"};
    LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, on_columns, 1, &config, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

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
      .when_not_matched_insert_all = 1
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
      .when_not_matched_insert_all = 1
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        nullptr, merge_reader, on_columns, 1, &config, &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);

    // Reader was not consumed due to error, must free it
    lancedb_record_batch_reader_free(merge_reader);

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
      .when_not_matched_insert_all = 1
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_merge_insert(
        table, merge_reader, nullptr, 1, &config, &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);

    // Reader was not consumed due to error, must free it
    lancedb_record_batch_reader_free(merge_reader);

    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  lancedb_table_free(table);
}

