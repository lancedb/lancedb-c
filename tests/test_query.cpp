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
    result = lancedb_query_where_filter(query, "key = \"key_42\"", &error_message);
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
    result = lancedb_query_where_filter(query, "key = \"key_42\"", &error_message);
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
    result = lancedb_query_where_filter(query, "key = \"key_999\"", &error_message);
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
  LanceDBQueryResult* query_result = lancedb_query_execute(query);
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

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;

    REQUIRE(lancedb_query_result_to_arrow(
        nullptr, &result_arrays, &result_schema, &count, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_result_to_arrow(
        query_result, nullptr, &result_schema, &count, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_result_to_arrow(
        query_result, &result_arrays, nullptr, &count, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);


    // The result was not consumed by the rejected conversions
    lancedb_query_result_free(query_result);
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
    REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(result == nullptr);
    REQUIRE(error_message != nullptr);
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
    LanceDBIpcBuffer buffer = {};
    char* error_message = nullptr;
    REQUIRE(lancedb_query_result_to_ipc_file(nullptr, &buffer, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(buffer.data == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    LanceDBQuery* query = lancedb_query_new(table);
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
