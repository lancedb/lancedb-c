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

  if (error_message) {
    INFO("Error converting to Arrow: " << error_message);
    lancedb_free_string(error_message);
  }
  REQUIRE(result == LANCEDB_SUCCESS);
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

  if (error_message) {
    INFO("Error creating index: " << error_message);
    lancedb_free_string(error_message);
  }
  REQUIRE(result == LANCEDB_SUCCESS);
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
    if (error_message) {
      INFO("Error setting select: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

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
      if (error_message) {
        INFO("Error setting limit: " << error_message);
        lancedb_free_string(error_message);
      }
      REQUIRE(result == LANCEDB_SUCCESS);

      // Set offset
      error_message = nullptr;
      result = lancedb_query_offset(query, offset, &error_message);
      if (error_message) {
        INFO("Error setting offset: " << error_message);
        lancedb_free_string(error_message);
      }
      REQUIRE(result == LANCEDB_SUCCESS);

      // Select "key" and "data" columns
      const char* columns[] = {"key", "data"};
      error_message = nullptr;
      result = lancedb_query_select(query, columns, 2, &error_message);
      if (error_message) {
        INFO("Error setting select: " << error_message);
        lancedb_free_string(error_message);
      }
      REQUIRE(result == LANCEDB_SUCCESS);

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
    if (error_message) {
      INFO("Error setting select: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Filter by key = 42 select
    error_message = nullptr;
    result = lancedb_query_where_filter(query, "key = \"key_42\"", &error_message);
    if (error_message) {
      INFO("Error setting where filter: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

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
    if (error_message) {
      INFO("Error setting where filter: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Select "key" and "data" columns
    const char* columns[] = {"key", "data"};
    error_message = nullptr;
    result = lancedb_query_select(query, columns, 2, &error_message);
    if (error_message) {
      INFO("Error setting select: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

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
    if (error_message) {
      INFO("Error setting select: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Filter by key = 42 select
    error_message = nullptr;
    result = lancedb_query_where_filter(query, "key = \"key_42\"", &error_message);
    if (error_message) {
      INFO("Error setting where filter: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

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
    if (error_message) {
      INFO("Error setting where filter: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Select "key" and "data" columns
    const char* columns[] = {"key", "data"};
    error_message = nullptr;
    result = lancedb_query_select(query, columns, 2, &error_message);
    if (error_message) {
      INFO("Error setting select: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

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
    if (error_message) {
      INFO("Error setting select: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Filter by a key that doesn't exist (table has key_0 to key_99)
    error_message = nullptr;
    result = lancedb_query_where_filter(query, "key = \"key_999\"", &error_message);
    if (error_message) {
      INFO("Error setting where filter: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

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

    if (error_message) {
      INFO("Error converting to Arrow: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count == 0);
    REQUIRE(result_arrays == nullptr);
    REQUIRE(result_schema == nullptr);
  }

  lancedb_table_free(table);
}

