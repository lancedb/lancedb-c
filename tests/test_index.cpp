/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Scalar Index", "[index]") {
  const std::string table_name = "scalar_index_test";

  SECTION("Create BTREE index on table with existing data") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    // Create BTREE index on the "key" column
    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {
      .replace = 0,
      .force_update_statistics = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

    if (error_message) {
      INFO("Error message: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // List indices (should have one index)
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count == 1);
    REQUIRE(indices != nullptr);

    if (count > 0) {
      INFO("Created index: " << indices[0]);
    }

    // Free the index list
    lancedb_free_index_list(indices, count);

    // Add more data after index creation
    auto batch = create_test_record_batch(50, 100);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    result = lancedb_table_add(table, reader, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Verify total row count
    REQUIRE(lancedb_table_count_rows(table) == 150);

    lancedb_table_free(table);
  }

  SECTION("Create BTREE index on empty table then add data") {
    // Create empty table
    create_empty_table(table_name);
    LanceDBTable* table = lancedb_connection_open_table(db, table_name.c_str());
    REQUIRE(table != nullptr);

    // Create BTREE index on the "key" column
    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {
      .replace = 0,
      .force_update_statistics = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

    if (error_message) {
      INFO("Error message: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // List indices (should have one index)
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count == 1);
    REQUIRE(indices != nullptr);

    if (count > 0) {
      INFO("Created index: " << indices[0]);
    }

    // Free the index list
    lancedb_free_index_list(indices, count);

    // Add data after index creation
    auto batch = create_test_record_batch(100, 0);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    result = lancedb_table_add(table, reader, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Verify row count
    REQUIRE(lancedb_table_count_rows(table) == 100);

    lancedb_table_free(table);
  }

  SECTION("Replace existing BTREE index") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    // Create initial BTREE index
    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {
      .replace = 0,
      .force_update_statistics = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Replace the index
    config.replace = 1;
    result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // List indices (should still have one index after replacement)
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count == 1);
    REQUIRE(indices != nullptr);

    if (count > 0) {
      INFO("Index after replacement: " << indices[0]);
    }

    // Free the index list
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Scalar Index List and Drop", "[index]") {
  const std::string table_name = "index_list_drop_test";

  SECTION("List indices on table with no indices") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    // List indices (should be empty)
    char** indices = nullptr;
    size_t count = 0;
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    if (error_message) {
      INFO("Error message: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count == 0);
    REQUIRE(indices == nullptr);

    lancedb_table_free(table);
  }

  SECTION("Drop scalar index") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    // Create BTREE index
    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {
      .replace = 0,
      .force_update_statistics = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // List indices to get the index name
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count == 1);
    REQUIRE(indices != nullptr);

    // Save the index name
    std::string index_name = indices[0];
    INFO("Dropping index: " << index_name);

    // Free the index list
    lancedb_free_index_list(indices, count);

    // Drop the index
    result = lancedb_table_drop_index(table, index_name.c_str(), &error_message);

    if (error_message) {
      INFO("Error message: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // List indices again (should be empty)
    indices = nullptr;
    count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    if (error_message) {
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count == 0);

    lancedb_table_free(table);
  }

  SECTION("Drop non-existent index should fail") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    // Try to drop an index that doesn't exist
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_drop_index(table, "non_existent_index", &error_message);

    // Should fail
    REQUIRE(result != LANCEDB_SUCCESS);

    if (error_message) {
      INFO("Error message: " << error_message);
      lancedb_free_string(error_message);
    }

    lancedb_table_free(table);
  }
}

