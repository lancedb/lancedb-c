/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

#include <map>

void verify_query_result(LanceDBQueryResult* query_result, size_t expected_rows) {
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

struct CollectedQueryRows {
  std::shared_ptr<arrow::Schema> schema;
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;

  size_t num_rows() const {
    size_t rows = 0;
    for (const auto& batch : batches) {
      rows += static_cast<size_t>(batch->num_rows());
    }
    return rows;
  }
};

CollectedQueryRows collect_query_rows(LanceDBQueryResult* query_result) {
  REQUIRE(query_result != nullptr);

  FFI_ArrowArray** result_arrays = nullptr;
  FFI_ArrowSchema* result_schema = nullptr;
  size_t count = 0;
  char* error_message = nullptr;
  REQUIRE(lancedb_query_result_to_arrow(
      query_result, &result_arrays, &result_schema, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  CollectedQueryRows collected;
  if (count == 0) {
    REQUIRE(result_arrays == nullptr);
    REQUIRE(result_schema == nullptr);
    return collected;
  }

  auto imported_schema = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(result_schema));
  REQUIRE(imported_schema.ok());
  collected.schema = imported_schema.ValueOrDie();

  for (size_t i = 0; i < count; i++) {
    auto imported_batch = arrow::ImportRecordBatch(
        reinterpret_cast<ArrowArray*>(result_arrays[i]), collected.schema);
    REQUIRE(imported_batch.ok());
    collected.batches.push_back(imported_batch.ValueOrDie());
  }

  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);
  return collected;
}

std::vector<std::string> keys_of(const CollectedQueryRows& rows) {
  std::vector<std::string> keys;
  for (const auto& batch : rows.batches) {
    auto key_column = batch->GetColumnByName("key");
    REQUIRE(key_column != nullptr);
    auto key_array = std::static_pointer_cast<arrow::StringArray>(key_column);
    for (int64_t i = 0; i < key_array->length(); i++) {
      keys.push_back(key_array->GetString(i));
    }
  }
  return keys;
}

std::map<std::string, uint64_t> row_ids_by_key(const CollectedQueryRows& rows) {
  std::map<std::string, uint64_t> row_ids;
  for (const auto& batch : rows.batches) {
    auto key_column = batch->GetColumnByName("key");
    auto row_id_column = batch->GetColumnByName("_rowid");
    REQUIRE(key_column != nullptr);
    REQUIRE(row_id_column != nullptr);
    auto key_array = std::static_pointer_cast<arrow::StringArray>(key_column);
    auto row_id_array = std::static_pointer_cast<arrow::UInt64Array>(row_id_column);
    for (int64_t i = 0; i < key_array->length(); i++) {
      REQUIRE(!row_id_array->IsNull(i));
      row_ids[key_array->GetString(i)] = row_id_array->Value(i);
    }
  }
  return row_ids;
}

std::vector<uint64_t> row_ids_of(const CollectedQueryRows& rows) {
  std::vector<uint64_t> row_ids;
  for (const auto& batch : rows.batches) {
    auto row_id_column = batch->GetColumnByName("_rowid");
    REQUIRE(row_id_column != nullptr);
    auto row_id_array = std::static_pointer_cast<arrow::UInt64Array>(row_id_column);
    for (int64_t i = 0; i < row_id_array->length(); i++) {
      REQUIRE(!row_id_array->IsNull(i));
      row_ids.push_back(row_id_array->Value(i));
    }
  }
  return row_ids;
}

LanceDBTakeQuery* take_offsets_checked(
    const LanceDBTable* table, const uint64_t* offsets, size_t count) {
  LanceDBTakeQuery* take = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_take_offsets(table, offsets, count, &take, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(take != nullptr);
  return take;
}

LanceDBTakeQuery* take_row_ids_checked(
    const LanceDBTable* table, const uint64_t* row_ids, size_t count) {
  LanceDBTakeQuery* take = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_take_row_ids(table, row_ids, count, &take, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(take != nullptr);
  return take;
}

CollectedQueryRows execute_take_query(LanceDBTakeQuery* take) {
  LanceDBQueryResult* result = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_take_query_execute(take, &result, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(result != nullptr);
  return collect_query_rows(result);
}

void create_key_index(LanceDBTable* table) {
  // Create BTREE index on "key" column
  const char* index_columns[] = {"key"};
  LanceDBScalarIndexConfig config = {
    .replace = 0

  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_scalar_index(
      table, index_columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - all entries", "[query]") {
  const std::string table_name = "query_test";

  constexpr size_t total_rows = 100;
  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);
  create_key_index(table);

  SECTION("Query all entries") {
    // Create query
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    // Select "key" and "data" columns
    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);
    verify_query_result(query_result, total_rows);
  }

  SECTION("Query all entries in pages") {
    // Query multiple pages - need to create a new query for each page
    // because lancedb_query_execute() consumes the query object
    constexpr size_t limit = 30;
    size_t offset = 0;

    while (offset < total_rows) {
      // Create a new query for each page
      LanceDBQuery* query = lancedb_query_new(table);
      REQUIRE(query != nullptr);

      // Set limit
      char* error_message = nullptr;
      LanceDBError result = lancedb_query_limit(query, limit, &error_message);
      REQUIRE(result == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);

      // Set offset
      error_message = nullptr;
      result = lancedb_query_offset(query, offset, &error_message);
      REQUIRE(result == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);

      // Select "key" and "data" columns
      const char* columns[] = {"key", "data"};
      error_message = nullptr;
      result = lancedb_query_select(query, columns, 2, &error_message);
      REQUIRE(result == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);

      // Execute query (consumes the query object)
      LanceDBQueryResult* query_result = lancedb_query_execute(query);
      REQUIRE(query_result != nullptr);

      // Verify this page has the expected number of rows
      size_t expected_rows = std::min(limit, total_rows - offset);
      verify_query_result(query_result, expected_rows);

      offset += limit;
    }
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - Where Filter", "[query]") {
  const std::string table_name = "query_filter_test";

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, 100, 0);
  REQUIRE(table != nullptr);

  create_key_index(table);

  SECTION("Filter by single key") {
    // Create query
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    // Select "key" and "data" columns FIRST (before filter)
    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Filter by key = 42 select
    error_message = nullptr;
    result = lancedb_query_where_filter(query, "key = 'key_42'", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);

    verify_query_result(query_result, 1);
  }

  SECTION("Filter with a double-quoted identifier") {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // SQL-standard double quotes delimit identifiers, not string values.
    result = lancedb_query_where_filter(query, "\"key\" = 'key_42'", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);

    verify_query_result(query_result, 1);
  }

  SECTION("Filter by list of keys using IN clause") {
    // Create query
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    // Filter by key IN (10, 20, 30, 40, 50)
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_where_filter(query, "key IN ('key_10', 'key_20', 'key_30', 'key_40', 'key_50')", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Select "key" and "data" columns
    const char* columns[] = {"key", "data"};
    error_message = nullptr;
    result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);

    verify_query_result(query_result, 5);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - Where Filter no Index", "[query]") {
  const std::string table_name = "query_filter_no_index_test";

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, 100, 0);
  REQUIRE(table != nullptr);

  SECTION("Filter by single key") {
    // Create query
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    // Select "key" and "data" columns FIRST (before filter)
    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Filter by key = 42 select
    error_message = nullptr;
    result = lancedb_query_where_filter(query, "key = 'key_42'", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);

    verify_query_result(query_result, 1);
  }

  SECTION("Filter by list of keys using IN clause") {
    // Create query
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    // Filter by key IN (10, 20, 30, 40, 50)
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_where_filter(query, "key IN ('key_10', 'key_20', 'key_30', 'key_40', 'key_50')", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Select "key" and "data" columns
    const char* columns[] = {"key", "data"};
    error_message = nullptr;
    result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);

    verify_query_result(query_result, 5);
  }

  SECTION("Filter by unknown key (should return empty result)") {
    // Create query
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    // Select "key" and "data" columns
    const char* columns[] = {"key", "data"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Filter by a key that doesn't exist (table has key_0 to key_99)
    error_message = nullptr;
    result = lancedb_query_where_filter(query, "key = 'key_999'", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 0);
    REQUIRE(result_arrays == nullptr);
    REQUIRE(result_schema == nullptr);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - Arrow conversion keeps stream errors", "[query]") {
  const std::string table_name = "query_arrow_stream_error_test";
  LanceDBTable* table = create_table_with_data(table_name, 10, 0);
  REQUIRE(table != nullptr);

  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);

  // The cast is valid SQL and the table exists; it fails only while the stream
  // evaluates the string keys as integers.
  char* error_message = nullptr;
  REQUIRE(lancedb_query_where_filter(query, "CAST(key AS INT) = 1", &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  LanceDBQueryResult* query_result = lancedb_query_execute(query);
  REQUIRE(query_result != nullptr);

  FFI_ArrowArray** result_arrays = reinterpret_cast<FFI_ArrowArray**>(uintptr_t{1});
  FFI_ArrowSchema* result_schema = reinterpret_cast<FFI_ArrowSchema*>(uintptr_t{1});
  size_t count = 42;

  const LanceDBError result = lancedb_query_result_to_arrow(
      query_result, &result_arrays, &result_schema, &count, &error_message);

  // The SDK typed the drain error; it is not replaced with LANCEDB_UNKNOWN.
  REQUIRE(result == LANCEDB_LANCE);
  REQUIRE(error_message != nullptr);
  REQUIRE(std::string(error_message).find("Cast error") != std::string::npos);
  lancedb_free_string(error_message);
  REQUIRE(result_arrays == nullptr);
  REQUIRE(result_schema == nullptr);
  REQUIRE(count == 0);

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - Filter on non-existent column", "[query]") {
  const std::string table_name = "query_filter_nonexistent_column_test";

  // Create table with data (columns: "key" and "data")
  LanceDBTable* table = create_table_with_data(table_name, 10, 0);
  REQUIRE(table != nullptr);

  // Create query
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);

  // Select existing columns
  const char* columns[] = {"key", "data"};
  char* error_message = nullptr;
  LanceDBError result = lancedb_query_select(query, columns, 2, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  // Filter on a column that does not exist in the table
  error_message = nullptr;
  result = lancedb_query_where_filter(query, "key = 'key_42' OR unknown = 'value'", &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  // error should be caught at execution time
  LanceDBQueryResult* query_result = nullptr;
  result = lancedb_query_execute_with_status(query, &query_result, &error_message);
  REQUIRE(result == LANCEDB_FIELD_NOT_FOUND);
  REQUIRE(query_result == nullptr);
  REQUIRE(error_message != nullptr);
  REQUIRE(std::string(error_message).find("unknown") != std::string::npos);
  lancedb_free_string(error_message);

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBSessionFixture, "LanceDB Query - repeated queries populate session cache stats", "[query][session]") {

  LanceDBSessionCacheStats initial_index_stats{};
  LanceDBSessionCacheStats final_index_stats{};

  char* error_message = nullptr;
  LanceDBError result = lancedb_session_index_cache_stats(session, &initial_index_stats, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  REQUIRE(initial_index_stats.hits == 0);
  REQUIRE(initial_index_stats.misses == 0);
  REQUIRE(initial_index_stats.num_entries == 0);
  REQUIRE(initial_index_stats.size_bytes == 0);

  const std::string table_name = "query_session_cache_stats_test";
  constexpr size_t total_rows = 100;
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);
  create_key_index(table);

  constexpr size_t repeat_count = 20;
  const char* columns[] = {"key", "data"};
  for (size_t i = 0; i < repeat_count; i++) {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const std::string filter = "key = 'key_" + std::to_string(i % total_rows) + "'";
    result = lancedb_query_where_filter(query, filter.c_str(), &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);
    lancedb_query_result_free(query_result);
  }

  result = lancedb_session_index_cache_stats(session, &final_index_stats, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  REQUIRE(final_index_stats.hits > initial_index_stats.hits);
  REQUIRE(final_index_stats.misses > initial_index_stats.misses);
  REQUIRE(final_index_stats.num_entries > initial_index_stats.num_entries);
  REQUIRE(final_index_stats.size_bytes > initial_index_stats.size_bytes);
  REQUIRE(final_index_stats.hits > final_index_stats.misses);
  
  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - row id and fast search flags", "[query]") {
  const std::string table_name = "query_flags_test";
  constexpr size_t indexed_rows = 8;
  constexpr size_t appended_rows = 8;

  LanceDBTable* table = create_table_with_data(
      table_name, static_cast<int>(indexed_rows), 0);
  REQUIRE(table != nullptr);
  create_key_index(table);

  auto appended = create_test_record_batch(
      static_cast<int>(appended_rows), static_cast<int>(indexed_rows));
  REQUIRE(lancedb_table_add(table, create_reader_from_batch(appended), nullptr) == LANCEDB_SUCCESS);

  auto run_query = [&](const std::string& filter, bool fast_search) {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key"};
    char* error_message = nullptr;
    REQUIRE(lancedb_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_where_filter(query, filter.c_str(), &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_with_row_id(query, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    if (fast_search) {
      REQUIRE(lancedb_query_fast_search(query, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }

    return collect_query_rows(lancedb_query_execute(query));
  };

  SECTION("with_row_id appends the actual _rowid column") {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key"};
    char* error_message = nullptr;
    REQUIRE(lancedb_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_with_row_id(query, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto rows = collect_query_rows(lancedb_query_execute(query));
    REQUIRE(rows.num_rows() == indexed_rows + appended_rows);
    REQUIRE(rows.schema->GetFieldByName("_rowid") != nullptr);
    REQUIRE(rows.schema->GetFieldByName("data") == nullptr);
    auto row_ids = row_ids_by_key(rows);
    REQUIRE(row_ids.size() == indexed_rows + appended_rows);

    auto appended_row = run_query("key = 'key_12'", false);
    REQUIRE(appended_row.num_rows() == 1);
    auto appended_row_ids = row_ids_by_key(appended_row);
    REQUIRE(appended_row_ids.at("key_12") == row_ids.at("key_12"));
  }

  SECTION("fast_search skips rows covered only by the appended fragment") {
    auto unfiltered = run_query("key = 'key_12'", false);
    REQUIRE(unfiltered.num_rows() == 1);
    REQUIRE(keys_of(unfiltered) == std::vector<std::string>{"key_12"});

    auto fast = run_query("key = 'key_12'", true);
    REQUIRE(fast.num_rows() == 0);

    auto indexed = run_query("key = 'key_3'", true);
    REQUIRE(indexed.num_rows() == 1);
    REQUIRE(keys_of(indexed) == std::vector<std::string>{"key_3"});
    REQUIRE(row_ids_by_key(indexed).count("key_3") == 1);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - invalid arguments", "[query]") {
  const std::string table_name = "query_invalid_args_test";
  LanceDBTable* table = create_table_with_data(table_name, 10, 0);
  REQUIRE(table != nullptr);

  SECTION("Query setters with null query should fail") {
    const char* columns[] = {"key"};

    REQUIRE(lancedb_query_limit(nullptr, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_offset(nullptr, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_select(nullptr, columns, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_where_filter(nullptr, "key = 'key_0'", nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_with_row_id(nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_fast_search(nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Query setters with null arguments should fail") {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    REQUIRE(lancedb_query_select(query, nullptr, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_where_filter(query, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);


    lancedb_query_free(query);
  }

  SECTION("Query setters with strings that are not valid UTF-8 should fail") {
    // A string that is not valid UTF-8, to exercise the string conversion failures
    const char* const invalid_utf8 = "\xff\xfe";
    const char* invalid_columns[] = {invalid_utf8};
    const char* null_columns[] = {nullptr};

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    REQUIRE(lancedb_query_select(query, null_columns, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_select(query, invalid_columns, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_where_filter(query, invalid_utf8, nullptr) == LANCEDB_INVALID_ARGUMENT);

    lancedb_query_free(query);
  }

  SECTION("Query on a null table returns no query") {
    REQUIRE(lancedb_query_new(nullptr) == nullptr);
    REQUIRE(lancedb_query_execute(nullptr) == nullptr);
  }

  SECTION("Converting results with invalid arguments should fail") {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);
    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);

    // Sentinel values prove that every non-NULL output slot is cleared on entry.
    FFI_ArrowArray** result_arrays = reinterpret_cast<FFI_ArrowArray**>(uintptr_t{1});
    FFI_ArrowSchema* result_schema = reinterpret_cast<FFI_ArrowSchema*>(uintptr_t{1});
    size_t count = 42;
    char* error_message = nullptr;

    REQUIRE(lancedb_query_result_to_arrow(
        nullptr, &result_arrays, &result_schema, &count, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(result_arrays == nullptr);
    REQUIRE(result_schema == nullptr);
    REQUIRE(count == 0);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    // A missing output pointer is rejected without consuming the result.
    error_message = nullptr;
    REQUIRE(lancedb_query_result_to_arrow(
        query_result, nullptr, &result_schema, &count, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(result_schema == nullptr);
    REQUIRE(count == 0);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    error_message = nullptr;
    REQUIRE(lancedb_query_result_to_arrow(
        query_result, &result_arrays, nullptr, &count, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(result_arrays == nullptr);
    REQUIRE(count == 0);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    error_message = nullptr;
    REQUIRE(lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, nullptr, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(result_arrays == nullptr);
    REQUIRE(result_schema == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    // The rejected calls left the result owned by the caller, so it can still
    // be converted and then freed by releasing the returned arrays.
    error_message = nullptr;
    REQUIRE(lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count > 0);
    REQUIRE(result_arrays != nullptr);
    REQUIRE(result_schema != nullptr);
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  lancedb_table_free(table);
}

// Tests for lancedb_query_explain_plan (explain-plan FFI).
// explain_plan does NOT consume the query: it can still be executed afterwards.
TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - explain plan", "[query][explain_plan]") {
  const std::string table_name = "query_explain_plan_test";
  LanceDBTable* table = create_table_with_data(table_name, 100, 0);
  REQUIRE(table != nullptr);

  const char* columns[] = {"key", "data"};

  SECTION("Explain plan for a basic query, non-verbose and verbose") {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_select(query, columns, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_limit(query, 10, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_query_offset(query, 5, &error_message) == LANCEDB_SUCCESS);

    char* plan = nullptr;
    LanceDBError result = lancedb_query_explain_plan(query, false, &plan, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);
    REQUIRE(std::string(plan).length() > 0);
    lancedb_free_string(plan);

    plan = nullptr;
    result = lancedb_query_explain_plan(query, true, &plan, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);
    REQUIRE(std::string(plan).length() > 0);
    lancedb_free_string(plan);

    // explain_plan does not consume the query: it can still be executed
    LanceDBQueryResult* query_result = lancedb_query_execute(query);
    REQUIRE(query_result != nullptr);
    lancedb_query_result_free(query_result);
  }

  SECTION("Explain plan reflects a DataFusion filter") {
    LanceDBExpr* col_expr = lancedb_expr_column("key");
    LanceDBExpr* val_expr = lancedb_expr_literal_string("key_42");
    LanceDBExpr* eq_expr = lancedb_expr_binary(col_expr, LANCEDB_BINARY_OP_EQ, val_expr);
    REQUIRE(eq_expr != nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_select(query, columns, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_query_df_filter(query, eq_expr, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char* plan = nullptr;
    LanceDBError result = lancedb_query_explain_plan(query, true, &plan, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);
    REQUIRE(std::string(plan).length() > 0);
    lancedb_free_string(plan);

    lancedb_query_free(query);
  }

  SECTION("Explain plan reflects a SQL filter") {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_select(query, columns, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_query_where_filter(query, "key = 'key_42'", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char* plan = nullptr;
    LanceDBError result = lancedb_query_explain_plan(query, true, &plan, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);
    // The filter literal only appears in the plan when the SQL predicate is applied.
    REQUIRE(std::string(plan).find("key_42") != std::string::npos);
    lancedb_free_string(plan);

    lancedb_query_free(query);
  }

  SECTION("Explain plan rejects a SQL filter on a missing column") {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_where_filter(query, "unknown = 'value'", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char* plan = nullptr;
    LanceDBError result = lancedb_query_explain_plan(query, true, &plan, &error_message);
    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(plan == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_query_free(query);
  }

  SECTION("Explain plan rejects null arguments") {
    char* plan = nullptr;
    REQUIRE(lancedb_query_explain_plan(nullptr, true, &plan, nullptr) == LANCEDB_INVALID_ARGUMENT);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);
    REQUIRE(lancedb_query_explain_plan(query, true, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    lancedb_query_free(query);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query - Arrow IPC output", "[query][ipc]") {
  const std::string table_name = "query_ipc_test";
  constexpr size_t total_rows = 100;
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  SECTION("A started query exports every projected batch as an IPC file") {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key"};
    char* error_message = nullptr;
    REQUIRE(lancedb_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_limit(query, 25, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(result != nullptr);

    LanceDBIpcBuffer buffer = {};
    REQUIRE(lancedb_query_result_to_ipc_file(result, &buffer, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto contents = read_ipc_file(buffer.data, buffer.len);
    REQUIRE(contents.schema->num_fields() == 1);
    REQUIRE(contents.schema->field(0)->name() == "key");
    REQUIRE(contents.batches.size() >= 1);
    REQUIRE(read_ipc_file_strings(contents, 0).size() == 25);

    // The file does not depend on the table handle that produced it
    lancedb_table_free(table);
    table = nullptr;

    auto reread = read_ipc_file(buffer.data, buffer.len);
    REQUIRE(reread.schema->num_fields() == 1);
    REQUIRE(read_ipc_file_strings(reread, 0).size() == 25);

    lancedb_ipc_buffer_free(&buffer);
    REQUIRE(buffer.data == nullptr);
    REQUIRE(buffer.len == 0);
    // Freeing an already freed buffer is a no-op
    lancedb_ipc_buffer_free(&buffer);
  }

  SECTION("An empty result keeps the projected schema and no batch") {
    const std::string empty_name = "query_ipc_empty_table";
    create_empty_table(empty_name);
    LanceDBTable* empty_table = open_table(empty_name);

    LanceDBQuery* query = lancedb_query_new(empty_table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key"};
    char* error_message = nullptr;
    REQUIRE(lancedb_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBIpcBuffer buffer = {};
    REQUIRE(lancedb_query_result_to_ipc_file(result, &buffer, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto contents = read_ipc_file(buffer.data, buffer.len);
    REQUIRE(contents.schema->num_fields() == 1);
    REQUIRE(contents.schema->field(0)->name() == "key");
    REQUIRE(contents.batches.empty());

    lancedb_ipc_buffer_free(&buffer);
    lancedb_table_free(empty_table);
  }

  SECTION("A filter that matches nothing keeps the schema and invents no row") {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key"};
    char* error_message = nullptr;
    REQUIRE(lancedb_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_where_filter(query, "key = 'no_such_key'", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBIpcBuffer buffer = {};
    REQUIRE(lancedb_query_result_to_ipc_file(result, &buffer, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto contents = read_ipc_file(buffer.data, buffer.len);
    REQUIRE(contents.schema->num_fields() == 1);
    REQUIRE(contents.schema->field(0)->name() == "key");
    REQUIRE(read_ipc_file_strings(contents, 0).empty());
    for (const auto& batch : contents.batches) {
      // A batch the stream did keep stays a zero-row batch
      REQUIRE(batch->num_rows() == 0);
    }

    lancedb_ipc_buffer_free(&buffer);
  }

  SECTION("A query that cannot start reports the SDK error instead of a null result") {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    // The filter is only resolved once the query runs
    REQUIRE(lancedb_query_where_filter(query, "no_such_column = 'x'", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* result = reinterpret_cast<LanceDBQueryResult*>(1);
    const LanceDBError status = lancedb_query_execute_with_status(query, &result, &error_message);
    // The SDK reports the unresolved column, not a NULL pointer and a lost error
    REQUIRE(status != LANCEDB_SUCCESS);
    REQUIRE(status != LANCEDB_UNKNOWN);
    REQUIRE(result == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("no_such_column") != std::string::npos);
    lancedb_free_string(error_message);

    // A null builder is rejected without a query
    error_message = nullptr;
    REQUIRE(lancedb_query_execute_with_status(nullptr, &result, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    // The legacy entry point keeps its null-pointer return convention
    query = lancedb_query_new(table);
    REQUIRE(query != nullptr);
    REQUIRE(lancedb_query_where_filter(query, "no_such_column = 'x'", nullptr)
        == LANCEDB_SUCCESS);
    REQUIRE(lancedb_query_execute(query) == nullptr);
  }

  SECTION("Converting without a result or without a buffer is rejected") {
    // A missing result pointer is rejected and the builder is still consumed
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_query_execute_with_status(query, nullptr, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    LanceDBIpcBuffer buffer = {};
    error_message = nullptr;
    REQUIRE(lancedb_query_result_to_ipc_file(nullptr, &buffer, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(buffer.data == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    query = lancedb_query_new(table);
    REQUIRE(query != nullptr);
    LanceDBQueryResult* result = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // The result is consumed even when the call cannot write its output
    error_message = nullptr;
    REQUIRE(lancedb_query_result_to_ipc_file(result, nullptr, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB TakeQuery - offsets and row IDs", "[query][take]") {
  const std::string table_name = "take_query_test";
  constexpr int initial_rows = 8;
  constexpr int appended_rows = 8;

  LanceDBTable* table = create_table_with_data(table_name, initial_rows, 0);
  REQUIRE(table != nullptr);

  // A second fragment makes the appended rows' row IDs differ from their
  // dataset offsets, so the tests can tell the two selectors apart.
  auto appended = create_test_record_batch(appended_rows, initial_rows);
  REQUIRE(lancedb_table_add(table, create_reader_from_batch(appended), nullptr)
      == LANCEDB_SUCCESS);

  auto all_rows = [&] {
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    const char* columns[] = {"key"};
    char* error_message = nullptr;
    REQUIRE(lancedb_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_with_row_id(query, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    return collect_query_rows(lancedb_query_execute(query));
  };

  const auto row_ids = row_ids_by_key(all_rows());
  REQUIRE(row_ids.size() == initial_rows + appended_rows);
  // key_12 is at dataset offset 12, but its row ID belongs to the second
  // fragment and must not be mistaken for that offset.
  REQUIRE(row_ids.at("key_12") != 12);

  SECTION("Offset take copies its input and preserves unsorted duplicate order") {
    std::vector<uint64_t> offsets = {12, 8, 12, 10};
    LanceDBTakeQuery* take = take_offsets_checked(table, offsets.data(), offsets.size());
    char* error_message = nullptr;
    REQUIRE(lancedb_take_query_preserve_order(take, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // The constructor copied the offsets, so changing the caller's array after
    // the call must not change the rows this take selects.
    std::fill(offsets.begin(), offsets.end(), 0);

    auto rows = execute_take_query(take);
    REQUIRE(keys_of(rows) == std::vector<std::string>{
        "key_12", "key_8", "key_12", "key_10"});
  }

  SECTION("Offset take keeps duplicate rows without promising request order") {
    const std::vector<uint64_t> offsets = {12, 8, 12, 10};
    auto rows = execute_take_query(
        take_offsets_checked(table, offsets.data(), offsets.size()));

    auto keys = keys_of(rows);
    std::sort(keys.begin(), keys.end());
    REQUIRE(keys == std::vector<std::string>{
        "key_10", "key_12", "key_12", "key_8"});
  }

  SECTION("Missing offsets and row IDs are omitted") {
    const std::vector<uint64_t> offsets = {12, 1000};
    LanceDBTakeQuery* offset_take =
        take_offsets_checked(table, offsets.data(), offsets.size());
    char* error_message = nullptr;
    REQUIRE(lancedb_take_query_preserve_order(offset_take, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(keys_of(execute_take_query(offset_take)) == std::vector<std::string>{"key_12"});

    const std::vector<uint64_t> requested = {
        row_ids.at("key_12"), UINT64_MAX, row_ids.at("key_8")};
    auto rows = execute_take_query(
        take_row_ids_checked(table, requested.data(), requested.size()));
    REQUIRE(keys_of(rows) == std::vector<std::string>{"key_12", "key_8"});
  }

  SECTION("Row ID take follows the requested row ID order") {
    const std::vector<uint64_t> requested = {
        row_ids.at("key_13"), row_ids.at("key_1"),
        row_ids.at("key_13"), row_ids.at("key_9")};
    LanceDBTakeQuery* take =
        take_row_ids_checked(table, requested.data(), requested.size());
    char* error_message = nullptr;
    REQUIRE(lancedb_take_query_with_row_id(take, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto rows = execute_take_query(take);
    REQUIRE(keys_of(rows) == std::vector<std::string>{
        "key_13", "key_1", "key_13", "key_9"});
    REQUIRE(row_ids_of(rows) == requested);
  }

  SECTION("Projection and with_row_id apply to take results") {
    const std::vector<uint64_t> requested = {row_ids.at("key_13"), row_ids.at("key_1")};
    LanceDBTakeQuery* take =
        take_row_ids_checked(table, requested.data(), requested.size());

    const char* columns[] = {"key"};
    char* error_message = nullptr;
    REQUIRE(lancedb_take_query_select(take, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_take_query_with_row_id(take, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto rows = execute_take_query(take);
    REQUIRE(rows.schema->num_fields() == 2);
    REQUIRE(rows.schema->field(0)->name() == "key");
    REQUIRE(rows.schema->field(1)->name() == "_rowid");
    REQUIRE(rows.schema->GetFieldByName("data") == nullptr);
    REQUIRE(keys_of(rows) == std::vector<std::string>{"key_13", "key_1"});
    REQUIRE(row_ids_of(rows) == requested);
  }

  SECTION("An empty take selection preserves the projected schema") {
    LanceDBTakeQuery* empty_take = take_offsets_checked(table, nullptr, 0);
    LanceDBQueryResult* result = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_take_query_execute(empty_take, &result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(result != nullptr);

    // The existing empty-result representation has no batches and no schema.
    FFI_ArrowArray** arrays = nullptr;
    FFI_ArrowSchema* schema = nullptr;
    size_t count = 42;
    REQUIRE(lancedb_query_result_to_arrow(
        result, &arrays, &schema, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 0);
    REQUIRE(arrays == nullptr);
    REQUIRE(schema == nullptr);

    empty_take = take_offsets_checked(table, nullptr, 0);
    result = nullptr;
    REQUIRE(lancedb_take_query_execute(empty_take, &result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBIpcBuffer buffer = {};
    REQUIRE(lancedb_query_result_to_ipc_file(result, &buffer, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto contents = read_ipc_file(buffer.data, buffer.len);
    REQUIRE(contents.schema->num_fields() == 2);
    REQUIRE(contents.schema->field(0)->name() == "key");
    REQUIRE(contents.schema->field(1)->name() == "data");
    REQUIRE(contents.batches.empty());

    lancedb_ipc_buffer_free(&buffer);
  }

  SECTION("An empty column selection keeps an empty schema") {
    const std::vector<uint64_t> requested = {row_ids.at("key_12"), row_ids.at("key_8")};
    LanceDBTakeQuery* take =
        take_row_ids_checked(table, requested.data(), requested.size());
    REQUIRE(lancedb_take_query_select(take, nullptr, 0, nullptr) == LANCEDB_SUCCESS);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_take_query_execute(take, &result, nullptr) == LANCEDB_SUCCESS);
    REQUIRE(result != nullptr);

    FFI_ArrowArray** arrays = nullptr;
    FFI_ArrowSchema* schema = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_query_result_to_arrow(result, &arrays, &schema, &count, nullptr)
        == LANCEDB_SUCCESS);
    REQUIRE(count > 0);
    REQUIRE(arrays != nullptr);
    REQUIRE(schema != nullptr);
    REQUIRE(reinterpret_cast<ArrowSchema*>(schema)->n_children == 0);

    size_t total_rows = 0;
    for (size_t i = 0; i < count; i++) {
      auto* array = reinterpret_cast<ArrowArray*>(arrays[i]);
      REQUIRE(array->n_children == 0);
      total_rows += static_cast<size_t>(array->length);
    }
    REQUIRE(total_rows == requested.size());

    lancedb_free_arrow_arrays(arrays, count);
    lancedb_free_arrow_schema(schema);
  }

  SECTION("Invalid arguments are rejected without consuming a valid builder") {
    const uint64_t offsets[] = {0};
    LanceDBTakeQuery* take = reinterpret_cast<LanceDBTakeQuery*>(uintptr_t{1});
    char* error_message = nullptr;

    REQUIRE(lancedb_table_take_offsets(nullptr, offsets, 1, &take, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(take == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    error_message = nullptr;
    take = reinterpret_cast<LanceDBTakeQuery*>(uintptr_t{1});
    REQUIRE(lancedb_table_take_row_ids(table, nullptr, 1, &take, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(take == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    error_message = nullptr;
    take = reinterpret_cast<LanceDBTakeQuery*>(uintptr_t{1});
    REQUIRE(lancedb_table_take_offsets(table, offsets, 1, nullptr, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    REQUIRE(lancedb_take_query_select(nullptr, nullptr, 0, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_take_query_with_row_id(nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_take_query_preserve_order(nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);

    take = take_offsets_checked(table, offsets, 1);
    const char* null_columns[] = {nullptr};
    error_message = nullptr;
    REQUIRE(lancedb_take_query_select(take, null_columns, 1, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_take_query_free(take);

    take = take_offsets_checked(table, offsets, 1);
    error_message = nullptr;
    REQUIRE(lancedb_take_query_execute(take, nullptr, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    LanceDBQueryResult* result = reinterpret_cast<LanceDBQueryResult*>(uintptr_t{1});
    error_message = nullptr;
    REQUIRE(lancedb_take_query_execute(nullptr, &result, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(result == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_take_query_free(nullptr);

    // A zero count accepts a NULL selector array and creates an empty take.
    take = take_offsets_checked(table, nullptr, 0);
    lancedb_take_query_free(take);
  }

  lancedb_table_free(table);
}
