/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

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

void create_key_index(LanceDBTable* table) {
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
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
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
      LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
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
    result = lancedb_query_where_filter(query, "key = \"key_42\"", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    verify_query_result(query_result, 1);
  }

  SECTION("Filter by list of keys using IN clause") {
    // Create query
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    // Filter by key IN (10, 20, 30, 40, 50)
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_where_filter(query, "key IN (\"key_10\", \"key_20\", \"key_30\", \"key_40\", \"key_50\")", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Select "key" and "data" columns
    const char* columns[] = {"key", "data"};
    error_message = nullptr;
    result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
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
    result = lancedb_query_where_filter(query, "key = \"key_42\"", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    verify_query_result(query_result, 1);
  }

  SECTION("Filter by list of keys using IN clause") {
    // Create query
    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    // Filter by key IN (10, 20, 30, 40, 50)
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_where_filter(query, "key IN (\"key_10\", \"key_20\", \"key_30\", \"key_40\", \"key_50\")", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Select "key" and "data" columns
    const char* columns[] = {"key", "data"};
    error_message = nullptr;
    result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
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
    result = lancedb_query_where_filter(query, "key = \"key_999\"", &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
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
  result = lancedb_query_where_filter(query, "key = \"key_42\" OR unknown = \"value\"", &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  // error should be caught at execution time
  LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
  REQUIRE(query_result == nullptr);
  error_message = nullptr;

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

    const std::string filter = "key = \"key_" + std::to_string(i % total_rows) + "\"";
    result = lancedb_query_where_filter(query, filter.c_str(), &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_query_execute(query, nullptr);
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query Execution Options - Basic operations", "[query]") {
  // Create a table with test data
  LanceDBTable* table = create_table_with_data("query_options_test_basic", 1024, 0);
  REQUIRE(table != nullptr);

  SECTION("Test setting max_batch_length") {
    LanceDBQueryExecutionOptions* options = lancedb_query_execution_options_new();
    REQUIRE(options != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_query_execution_options_set_max_batch_length(
        options,
        1000,
        &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    lancedb_query_execution_options_free(options);
  }

  SECTION("Test setting timeout") {
    LanceDBQueryExecutionOptions* options = lancedb_query_execution_options_new();
    REQUIRE(options != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_query_execution_options_set_timeout(
        options,
        30,  // 30 seconds
        &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    lancedb_query_execution_options_free(options);
  }

  SECTION("Test setting zero timeout") {
    LanceDBQueryExecutionOptions* options = lancedb_query_execution_options_new();
    REQUIRE(options != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_query_execution_options_set_timeout(
        options,
        0,  // Zero timeout means no timeout
        &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    constexpr unsigned int query_limit = 10;
    LanceDBError limit_result = lancedb_query_limit(query, query_limit, &error_message);
    REQUIRE(limit_result == LANCEDB_SUCCESS);

    const char* columns[] = {"key", "data"};
    result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    LanceDBQueryResult* query_result = lancedb_query_execute(query, options);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;

    LanceDBError collect_result = lancedb_query_result_to_arrow(
        query_result,
        &result_arrays,
        &result_schema,
        &count,
        &error_message);

    REQUIRE(collect_result == LANCEDB_SUCCESS);
    REQUIRE(count > 0);

    size_t total_rows = 0;
    for (size_t i = 0; i < count; i++) {
      const auto length = reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
      INFO("Batch " << i << ": " << length << " rows");
      total_rows += length;
    }

    INFO("Got " << total_rows << " rows with zero timeout");
    REQUIRE(total_rows == query_limit);

    lancedb_free_arrow_arrays(result_arrays, count);
    if (result_schema != nullptr) {
      lancedb_free_arrow_schema(result_schema);
    }
    lancedb_query_execution_options_free(options);
  }

  SECTION("Test using options with query") {
    LanceDBQueryExecutionOptions* options = lancedb_query_execution_options_new();
    REQUIRE(options != nullptr);

    char* error_message = nullptr;

    constexpr unsigned int batch_size = 10;
    LanceDBError result = lancedb_query_execution_options_set_max_batch_length(
        options,
        batch_size,
        &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    LanceDBQuery* query = lancedb_query_new(table);
    REQUIRE(query != nullptr);

    constexpr unsigned int query_limit = 500;
    LanceDBError limit_result = lancedb_query_limit(query, query_limit, &error_message);
    REQUIRE(limit_result == LANCEDB_SUCCESS);

    const char* columns[] = {"key", "data"};
    result = lancedb_query_select(query, columns, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    // Execute query with options
    LanceDBQueryResult* query_result = lancedb_query_execute(query, options);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;

    LanceDBError collect_result = lancedb_query_result_to_arrow(
        query_result,
        &result_arrays,
        &result_schema,
        &count,
        &error_message);

    REQUIRE(collect_result == LANCEDB_SUCCESS);
    REQUIRE(count > 0);

    size_t total_rows = 0;
    for (size_t i = 0; i < count; i++) {
      const auto length = reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
      INFO("Batch " << i << ": " << length << " rows (target was " << batch_size << ")");
      REQUIRE(length <= batch_size);
      total_rows += length;
    }

    REQUIRE(total_rows == query_limit);

    lancedb_free_arrow_arrays(result_arrays, count);
    if (result_schema != nullptr) {
      lancedb_free_arrow_schema(result_schema);
    }
    lancedb_query_execution_options_free(options);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query Execution Options - Error cases", "[query]") {
  SECTION("Test setting max_batch_length with NULL options") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_execution_options_set_max_batch_length(
        nullptr,  // NULL options
        1000,
        &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    INFO("Expected error: " << error_message);
    lancedb_free_string(error_message);
  }

  SECTION("Test setting timeout with NULL options") {
    char* error_message = nullptr;
    LanceDBError result = lancedb_query_execution_options_set_timeout(
        nullptr,  // NULL options
        30,
        &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    INFO("Expected error: " << error_message);
    lancedb_free_string(error_message);
  }

  SECTION("Test freeing NULL options (should not crash)") {
    // This should be safe and not crash
    lancedb_query_execution_options_free(nullptr);
  }

  SECTION("Test setting zero max_batch_length") {
    LanceDBQueryExecutionOptions* options = lancedb_query_execution_options_new();
    REQUIRE(options != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_query_execution_options_set_max_batch_length(
        options,
        0,  // Zero batch length - should be rejected
        &error_message);

    // Setting zero should fail
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    INFO("Expected error: " << error_message);
    lancedb_free_string(error_message);

    lancedb_query_execution_options_free(options);
  }
}

