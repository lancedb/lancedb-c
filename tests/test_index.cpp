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
      .replace = 0

    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // List indices (should have one index)
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
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

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Verify total row count
    REQUIRE(lancedb_table_count_rows(table) == 150);

    lancedb_table_free(table);
  }

  SECTION("Create BTREE index on empty table then add data") {
    // Create empty table
    create_empty_table(table_name);
    LanceDBTable* table = open_table(table_name);

    // Create BTREE index on the "key" column
    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {
      .replace = 0

    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // List indices (should have one index)
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
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

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

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
      .replace = 0

    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Replace the index
    config.replace = 1;
    result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // List indices (should still have one index after replacement)
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Index Stats", "[index]") {
  const std::string table_name = "index_stats_test";

  SECTION("Get stats for a scalar index") {
    // Create table with data and a BTREE index
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {
      .replace = 0

    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    // Get the index name
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count == 1);
    std::string index_name = indices[0];
    lancedb_free_index_list(indices, count);

    // Get index stats
    LanceDBIndexStats stats = {};
    result = lancedb_table_index_stats(table, index_name.c_str(), &stats, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    INFO("Indexed rows: " << stats.num_indexed_rows);
    INFO("Unindexed rows: " << stats.num_unindexed_rows);
    INFO("Num indices: " << stats.num_indices);

    // After creating the index, all rows should be indexed
    REQUIRE(stats.num_indexed_rows == 100);
    REQUIRE(stats.num_unindexed_rows == 0);

    lancedb_table_free(table);
  }

  SECTION("Stats reflect unindexed rows after adding data") {
    // Create table with data and a BTREE index
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {
      .replace = 0

    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    // Get the index name
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count == 1);
    std::string index_name = indices[0];
    lancedb_free_index_list(indices, count);

    // Add more data (these rows won't be indexed yet)
    auto batch = create_test_record_batch(50, 100);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);
    result = lancedb_table_add(table, reader, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    // Get index stats - should show unindexed rows
    LanceDBIndexStats stats = {};
    result = lancedb_table_index_stats(table, index_name.c_str(), &stats, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    INFO("Indexed rows: " << stats.num_indexed_rows);
    INFO("Unindexed rows: " << stats.num_unindexed_rows);

    REQUIRE(stats.num_indexed_rows == 100);
    REQUIRE(stats.num_unindexed_rows == 50);

    lancedb_table_free(table);
  }

  SECTION("Stats for non-existent index returns INDEX_NOT_FOUND") {
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    LanceDBIndexStats stats = {};
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_index_stats(
        table, "no_such_index", &stats, &error_message);

    REQUIRE(result == LANCEDB_INDEX_NOT_FOUND);

    if (error_message) {
      lancedb_free_string(error_message);
    }

    lancedb_table_free(table);
  }

  SECTION("Stats with null table returns INVALID_ARGUMENT") {
    LanceDBIndexStats stats = {};
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_index_stats(
        nullptr, "some_index", &stats, &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);

    if (error_message) {
      lancedb_free_string(error_message);
    }
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

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
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
      .replace = 0

    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // List indices to get the index name
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(indices != nullptr);

    // Save the index name
    std::string index_name = indices[0];
    INFO("Dropping index: " << index_name);

    // Free the index list
    lancedb_free_index_list(indices, count);

    // Drop the index
    result = lancedb_table_drop_index(table, index_name.c_str(), &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // List indices again (should be empty)
    indices = nullptr;
    count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
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

    // lance reports a missing index without its original error variant,
    // so it is recovered from the message
    REQUIRE(result == LANCEDB_INDEX_NOT_FOUND);
    REQUIRE(error_message != nullptr);

    INFO("Error message: " << error_message);
    lancedb_free_string(error_message);

    lancedb_table_free(table);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB FTS Index", "[index]") {
  const std::string table_name = "fts_index_test";

  SECTION("Create FTS index with default configuration") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_fts_index(
        table, columns, 1, nullptr, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // The index is listed on the table
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }

  SECTION("Create FTS index with full configuration") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBFtsIndexConfig config = {
      .base_tokenizer = "simple",
      .language = "English",
      .max_token_length = 100,
      .lowercase = 1,
      .stem = 1,
      .remove_stop_words = 1,
      .ascii_folding = 1,
      .replace = 1
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_fts_index(
        table, columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    lancedb_table_free(table);
  }

  SECTION("Create FTS index without replacing an existing one should fail") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBFtsIndexConfig config = {
      .base_tokenizer = nullptr,
      .language = nullptr,
      .max_token_length = -1,
      .lowercase = 1,
      .stem = 0,
      .remove_stop_words = 0,
      .ascii_folding = 0,
      .replace = 0
    };

    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_fts_index(
        table, columns, 1, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Creating the index again without replace is rejected
    LanceDBError result = lancedb_table_create_fts_index(
        table, columns, 1, &config, &error_message);
    REQUIRE(result != LANCEDB_SUCCESS);
    if (error_message) {
      lancedb_free_string(error_message);
    }

    lancedb_table_free(table);
  }

  SECTION("Create FTS index with an unknown language should fail") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBFtsIndexConfig config = {
      .base_tokenizer = nullptr,
      .language = "Klingon",
      .max_token_length = -1,
      .lowercase = 1,
      .stem = 0,
      .remove_stop_words = 0,
      .ascii_folding = 0,
      .replace = 1
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_fts_index(
        table, columns, 1, &config, &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    if (error_message) {
      lancedb_free_string(error_message);
    }

    lancedb_table_free(table);
  }

  SECTION("Create FTS index with invalid arguments should fail") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    const char* null_columns[] = {nullptr};

    REQUIRE(lancedb_table_create_fts_index(
        nullptr, columns, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_fts_index(
        table, nullptr, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_fts_index(
        table, columns, 0, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_fts_index(
        table, null_columns, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);

    lancedb_table_free(table);
  }
}

// A string that is not valid UTF-8, to exercise the string conversion failures
static const char* const INVALID_UTF8 = "\xff\xfe";

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Create Index Options", "[index]") {
  const std::string table_name = "create_index_options_test";

  SECTION("Scalar index uses a name distinct from the column and explicit replace") {
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBIndexOptions options = lancedb_index_options_default();
    options.name = "key_lookup";
    options.replace = 0;
    options.wait_timeout_ms = 1000;

    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(std::string(indices[0]) == "key_lookup");
    lancedb_free_index_list(indices, count);

    // replace = 0 is an explicit false, not the SDK default true.
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &options, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    options.replace = 1;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    lancedb_table_free(table);
  }

  SECTION("FTS index forwards tuning options and custom name") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBIndexOptions options = lancedb_index_options_default();
    options.name = "key_text";
    options.replace = 0;
    options.wait_timeout_ms = 1000;
    options.fts_base_tokenizer = "simple";
    options.fts_language = "English";
    options.fts_max_token_length = 64;
    options.fts_lower_case = 0; // explicit false, not the SDK default
    options.fts_stem = 1;
    options.fts_remove_stop_words = 1;
    options.fts_ascii_folding = 1;
    options.fts_with_position = 1;

    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_FTS, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(std::string(indices[0]) == "key_text");
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }

  SECTION("FTS ngram options are forwarded") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBIndexOptions options = lancedb_index_options_default();
    options.name = "key_ngram";
    options.fts_base_tokenizer = "ngram";
    options.fts_ngram_min_length = 2;
    options.fts_ngram_max_length = 4;
    options.fts_ngram_prefix_only = 1;

    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_FTS, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    lancedb_table_free(table);
  }

  SECTION("NULL options use the SDK defaults") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, nullptr, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    lancedb_table_free(table);
  }

  SECTION("Auto index type uses the SDK Auto index") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_AUTO, nullptr, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }

  SECTION("Bitmap index type creates a bitmap index") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBIndexOptions options = lancedb_index_options_default();
    options.name = "key_bitmap";

    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_BITMAP, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(std::string(indices[0]) == "key_bitmap");
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }

  SECTION("FTS rejects an unknown language through the generic path") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBIndexOptions options = lancedb_index_options_default();
    options.fts_language = "Klingon";

    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_FTS, &options, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_table_free(table);
  }

  SECTION("Invalid options are rejected before the SDK call") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    const char* null_columns[] = {nullptr};
    const char* invalid_columns[] = {INVALID_UTF8};
    LanceDBIndexOptions options = lancedb_index_options_default();
    char* error_message = nullptr;

    REQUIRE(lancedb_table_create_index(
        nullptr, columns, 1, LANCEDB_INDEX_BTREE, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_index(
        table, nullptr, 1, LANCEDB_INDEX_BTREE, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_index(
        table, columns, 0, LANCEDB_INDEX_BTREE, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_index(
        table, null_columns, 1, LANCEDB_INDEX_BTREE, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_index(
        table, invalid_columns, 1, LANCEDB_INDEX_BTREE, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);

    options.replace = 2;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &options, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    options = lancedb_index_options_default();
    options.distance_type = 99;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &options, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    options = lancedb_index_options_default();
    options.num_partitions = -2;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &options, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    options = lancedb_index_options_default();
    options.num_partitions = static_cast<int64_t>(UINT32_MAX) + 1;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &options, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    options = lancedb_index_options_default();
    options.fts_lower_case = 2;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_FTS, &options, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    options = lancedb_index_options_default();
    options.name = INVALID_UTF8;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &options, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_table_free(table);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Create Index Train and Exist OK", "[index]") {
  const std::string table_name = "create_index_train_exist_ok_test";
  const size_t num_rows = 100;
  LanceDBTable* table = create_table_with_data(table_name, static_cast<int>(num_rows), 0);
  REQUIRE(table != nullptr);

  const char* columns[] = {"key"};
  LanceDBIndexOptions options = lancedb_index_options_default();
  options.name = "key_untrained";
  char* error_message = nullptr;

  auto require_index_stats = [&](size_t indexed_rows, size_t unindexed_rows) {
    const LanceDBIndexStatistics* stats = nullptr;
    REQUIRE(lancedb_table_index_statistics(
        table, options.name, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats != nullptr);
    REQUIRE(stats->index_type != nullptr);
    CHECK(std::string(stats->index_type) == "BTREE");
    CHECK(stats->distance_type == nullptr);
    CHECK(stats->num_indexed_rows == indexed_rows);
    CHECK(stats->num_unindexed_rows == unindexed_rows);
    lancedb_free_index_statistics(stats);
  };

  // An embedded table can create a named scalar index without training it.
  REQUIRE(lancedb_table_create_index_with_builder_options(
      table, columns, 1, LANCEDB_INDEX_BTREE, &options, 0, -1, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  require_index_stats(0, num_rows);

  // exist_ok preserves the existing name even with explicit replace and train.
  options.replace = 1;
  REQUIRE(lancedb_table_create_index_with_builder_options(
      table, columns, 1, LANCEDB_INDEX_BTREE, &options, 1, 1, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  require_index_stats(0, num_rows);

  // The legacy entry leaves both choices unset, so the SDK defaults replace and
  // train the existing index.
  options.replace = -1;
  REQUIRE(lancedb_table_create_index(
      table, columns, 1, LANCEDB_INDEX_BTREE, &options, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  require_index_stats(num_rows, 0);

  // The new arguments are validated before any SDK call.
  REQUIRE(lancedb_table_create_index_with_builder_options(
      table, columns, 1, LANCEDB_INDEX_BTREE, &options, 2, -1, &error_message) == LANCEDB_INVALID_ARGUMENT);
  REQUIRE(error_message != nullptr);
  lancedb_free_string(error_message);
  error_message = nullptr;
  REQUIRE(lancedb_table_create_index_with_builder_options(
      table, columns, 1, LANCEDB_INDEX_BTREE, &options, -1, 2, &error_message) == LANCEDB_INVALID_ARGUMENT);
  REQUIRE(error_message != nullptr);
  lancedb_free_string(error_message);
  error_message = nullptr;
  require_index_stats(num_rows, 0);

  char** indices = nullptr;
  size_t count = 0;
  REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count == 1);
  lancedb_free_index_list(indices, count);

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Optimize", "[index]") {
  const std::string table_name = "optimize_test";

  SECTION("Optimize with all the optimization types") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    // Add more data, so that there is something to compact
    auto reader = create_reader_from_batch(create_test_record_batch(20, 20));
    REQUIRE(reader != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    const LanceDBOptimizeType types[] = {
      LANCEDB_OPTIMIZE_COMPACT,
      LANCEDB_OPTIMIZE_INDEX,
      LANCEDB_OPTIMIZE_PRUNE,
      LANCEDB_OPTIMIZE_ALL
    };

    for (const auto type : types) {
      error_message = nullptr;
      REQUIRE(lancedb_table_optimize(table, type, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }

    // No rows were lost by the optimizations
    REQUIRE(lancedb_table_count_rows(table) == 40);

    lancedb_table_free(table);
  }

  SECTION("Optimize with null table should fail") {
    char* error_message = nullptr;
    REQUIRE(lancedb_table_optimize(
        nullptr, LANCEDB_OPTIMIZE_ALL, &error_message) == LANCEDB_INVALID_ARGUMENT);

    if (error_message) {
      lancedb_free_string(error_message);
    }
  }
}


TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Optimize With Action", "[index]") {
  const std::string table_name = "optimize_action_test";

  // Four fragments of 20 rows each, so compaction has fragments to merge and
  // pruning has three old versions to remove.
  auto create_fragmented_table = [&]() {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);
    for (const int start_index : {20, 40, 60}) {
      auto reader = create_reader_from_batch(create_test_record_batch(20, start_index));
      REQUIRE(reader != nullptr);
      char* error_message = nullptr;
      REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }
    return table;
  };

  auto read_stats = [](const LanceDBTable* table) {
    LanceDBTableStats stats = {};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_stats(table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    return stats;
  };

  SECTION("Compact merges the small fragments and reports them") {
    LanceDBTable* table = create_fragmented_table();
    REQUIRE(read_stats(table).fragment_stats.num_fragments == 4);

    // Every compaction override is set at once, so each one is converted on
    // the way to the SDK. A 1000-row target merges the four 20-row fragments.
    LanceDBOptimizeConfig config = {};
    config.action = LANCEDB_OPTIMIZE_COMPACT;
    config.compaction.set_fields =
        LANCEDB_COMPACTION_TARGET_ROWS_PER_FRAGMENT |
        LANCEDB_COMPACTION_MAX_ROWS_PER_GROUP |
        LANCEDB_COMPACTION_MAX_BYTES_PER_FILE |
        LANCEDB_COMPACTION_MATERIALIZE_DELETIONS |
        LANCEDB_COMPACTION_MATERIALIZE_DELETIONS_THRESHOLD |
        LANCEDB_COMPACTION_NUM_THREADS |
        LANCEDB_COMPACTION_BATCH_SIZE;
    config.compaction.target_rows_per_fragment = 1000;
    config.compaction.max_rows_per_group = 64;
    config.compaction.max_bytes_per_file = 1024 * 1024;
    config.compaction.materialize_deletions = 0;
    config.compaction.materialize_deletions_threshold = 0.5f;
    config.compaction.num_threads = 1;
    config.compaction.batch_size = 32;

    LanceDBOptimizeStats stats = {};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_optimize_with_action(
        table, &config, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.has_compaction == 1);
    REQUIRE(stats.has_prune == 0);
    REQUIRE(stats.compaction.fragments_removed == 4);
    REQUIRE(stats.compaction.fragments_added == 1);
    REQUIRE(stats.compaction.files_removed > 0);
    REQUIRE(stats.compaction.files_added > 0);
    REQUIRE(read_stats(table).fragment_stats.num_fragments == 1);
    REQUIRE(lancedb_table_count_rows(table) == 80);

    // Nothing is left to compact, so the same call rewrites nothing.
    LanceDBOptimizeStats second = {};
    REQUIRE(lancedb_table_optimize_with_action(
        table, &config, &second, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(second.has_compaction == 1);
    REQUIRE(second.compaction.fragments_removed == 0);
    REQUIRE(second.compaction.fragments_added == 0);
    REQUIRE(second.compaction.files_removed == 0);
    REQUIRE(second.compaction.files_added == 0);
    REQUIRE(lancedb_table_count_rows(table) == 80);

    lancedb_table_free(table);
  }

  SECTION("An explicit materialize_deletions=false differs from the omitted default") {
    LanceDBTable* table = create_fragmented_table();
    char* error_message = nullptr;
    REQUIRE(lancedb_table_delete(table, "key = 'key_7'", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == 79);
    REQUIRE(read_stats(table).num_deleted_rows == 1);

    // A zero target keeps row-count consolidation away, so only the deletion
    // trigger can rewrite a fragment.
    LanceDBOptimizeConfig keep_deletions = {};
    keep_deletions.action = LANCEDB_OPTIMIZE_COMPACT;
    keep_deletions.compaction.set_fields =
        LANCEDB_COMPACTION_TARGET_ROWS_PER_FRAGMENT |
        LANCEDB_COMPACTION_MATERIALIZE_DELETIONS |
        LANCEDB_COMPACTION_MATERIALIZE_DELETIONS_THRESHOLD;
    keep_deletions.compaction.target_rows_per_fragment = 0;
    keep_deletions.compaction.materialize_deletions = 0;
    keep_deletions.compaction.materialize_deletions_threshold = 0.0f;

    LanceDBOptimizeStats explicit_off = {};
    REQUIRE(lancedb_table_optimize_with_action(
        table, &keep_deletions, &explicit_off, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(explicit_off.has_compaction == 1);
    REQUIRE(explicit_off.compaction.fragments_removed == 0);
    REQUIRE(explicit_off.compaction.files_removed == 0);
    REQUIRE(read_stats(table).num_deleted_rows == 1);

    // Omitting materialize_deletions keeps the SDK default of true, which
    // materializes the deletion and drops the deletion file.
    LanceDBOptimizeConfig materialize = {};
    materialize.action = LANCEDB_OPTIMIZE_COMPACT;
    materialize.compaction.set_fields =
        LANCEDB_COMPACTION_TARGET_ROWS_PER_FRAGMENT |
        LANCEDB_COMPACTION_MATERIALIZE_DELETIONS_THRESHOLD;
    materialize.compaction.target_rows_per_fragment = 0;
    materialize.compaction.materialize_deletions_threshold = 0.0f;

    LanceDBOptimizeStats default_on = {};
    REQUIRE(lancedb_table_optimize_with_action(
        table, &materialize, &default_on, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(default_on.has_compaction == 1);
    REQUIRE(default_on.compaction.fragments_removed == 1);
    REQUIRE(default_on.compaction.fragments_added == 1);
    REQUIRE(read_stats(table).num_deleted_rows == 0);
    REQUIRE(lancedb_table_count_rows(table) == 79);

    lancedb_table_free(table);
  }

  SECTION("Prune keeps versions by default and prunes them for a sub-second age") {
    LanceDBTable* table = create_fragmented_table();
    REQUIRE(lancedb_table_version(table) == 4);

    // The default age is a week, so a table written just now loses nothing.
    LanceDBOptimizeConfig default_age = {};
    default_age.action = LANCEDB_OPTIMIZE_PRUNE;

    LanceDBOptimizeStats kept = {};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_optimize_with_action(
        table, &default_age, &kept, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(kept.has_prune == 1);
    REQUIRE(kept.has_compaction == 0);
    REQUIRE(kept.prune.old_versions == 0);
    REQUIRE(kept.prune.bytes_removed == 0);
    REQUIRE(lancedb_table_version(table) == 4);

    // A microsecond keeps this young fixture prunable, so a whole-second
    // round-up would leave its old versions and fail here. It cannot catch a
    // truncation to zero, because a cutoff at now also removes them; the Rust
    // conversion test pins the exact duration.
    LanceDBOptimizeConfig sub_second = {};
    sub_second.action = LANCEDB_OPTIMIZE_PRUNE;
    sub_second.prune.set_fields =
        LANCEDB_PRUNE_OLDER_THAN |
        LANCEDB_PRUNE_DELETE_UNVERIFIED |
        LANCEDB_PRUNE_ERROR_IF_TAGGED_OLD_VERSIONS;
    sub_second.prune.older_than_nanos = 1000;
    sub_second.prune.delete_unverified = 1;
    sub_second.prune.error_if_tagged_old_versions = 0;

    LanceDBOptimizeStats pruned = {};
    REQUIRE(lancedb_table_optimize_with_action(
        table, &sub_second, &pruned, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(pruned.has_prune == 1);
    REQUIRE(pruned.prune.old_versions == 3);
    REQUIRE(pruned.prune.bytes_removed > 0);
    // The current version and its rows survive the prune.
    REQUIRE(lancedb_table_version(table) == 4);
    REQUIRE(lancedb_table_count_rows(table) == 80);

    // Only the current version is left, so a second prune removes nothing.
    LanceDBOptimizeStats again = {};
    REQUIRE(lancedb_table_optimize_with_action(
        table, &sub_second, &again, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(again.prune.old_versions == 0);
    REQUIRE(again.prune.bytes_removed == 0);
    REQUIRE(lancedb_table_count_rows(table) == 80);

    lancedb_table_free(table);
  }

  SECTION("All and Index report the metric groups of their action") {
    LanceDBTable* table = create_fragmented_table();

    // Index optimization has no index to fold rows into here, and the SDK
    // reports neither metric group for it.
    LanceDBOptimizeConfig index_config = {};
    index_config.action = LANCEDB_OPTIMIZE_INDEX;
    LanceDBOptimizeStats index_stats = {};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_optimize_with_action(
        table, &index_config, &index_stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(index_stats.has_compaction == 0);
    REQUIRE(index_stats.has_prune == 0);
    REQUIRE(lancedb_table_count_rows(table) == 80);

    // ALL compacts and prunes with the action defaults.
    LanceDBOptimizeConfig all_config = {};
    all_config.action = LANCEDB_OPTIMIZE_ALL;
    LanceDBOptimizeStats all_stats = {};
    REQUIRE(lancedb_table_optimize_with_action(
        table, &all_config, &all_stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(all_stats.has_compaction == 1);
    REQUIRE(all_stats.compaction.fragments_removed == 4);
    REQUIRE(all_stats.compaction.fragments_added == 1);
    REQUIRE(all_stats.has_prune == 1);
    // ALL prunes with the default week-long age, which keeps the versions above.
    REQUIRE(all_stats.prune.old_versions == 0);
    REQUIRE(lancedb_table_count_rows(table) == 80);

    lancedb_table_free(table);
  }

  SECTION("Invalid options are rejected before any optimization runs") {
    LanceDBTable* table = create_fragmented_table();
    REQUIRE(read_stats(table).fragment_stats.num_fragments == 4);
    REQUIRE(lancedb_table_version(table) == 4);

    auto require_invalid = [&](const LanceDBTable* rejected_table,
                               const LanceDBOptimizeConfig* rejected_config,
                               const char* expected_field) {
      LanceDBOptimizeStats rejected_stats = {};
      char* message = nullptr;
      REQUIRE(lancedb_table_optimize_with_action(
          rejected_table, rejected_config, &rejected_stats, &message) == LANCEDB_INVALID_ARGUMENT);
      REQUIRE(message != nullptr);
      if (expected_field != nullptr) {
        INFO("message: " << message);
        REQUIRE(std::string(message).find(expected_field) != std::string::npos);
      }
      // A rejected call reports no statistics of an action that never ran.
      REQUIRE(rejected_stats.has_compaction == 0);
      REQUIRE(rejected_stats.has_prune == 0);
      lancedb_free_string(message);
    };

    LanceDBOptimizeConfig config = {};
    config.action = 99;
    require_invalid(table, &config, "action");

    // NULL arguments that a caller cannot recover from
    LanceDBOptimizeStats stats = {};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_optimize_with_action(
        nullptr, &config, &stats, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    REQUIRE(lancedb_table_optimize_with_action(
        table, nullptr, &stats, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    REQUIRE(lancedb_table_optimize_with_action(
        table, &config, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    config.action = LANCEDB_OPTIMIZE_COMPACT;
    config.compaction.set_fields = 1u << 20;
    require_invalid(table, &config, "compaction");

    config.compaction.set_fields = LANCEDB_COMPACTION_MATERIALIZE_DELETIONS;
    config.compaction.materialize_deletions = 2;
    require_invalid(table, &config, "materialize_deletions");

    config = {};
    config.action = LANCEDB_OPTIMIZE_PRUNE;
    config.prune.set_fields = LANCEDB_PRUNE_OLDER_THAN;
    config.prune.older_than_nanos = -1;
    require_invalid(table, &config, "older_than_nanos");

    config.prune.set_fields = LANCEDB_PRUNE_DELETE_UNVERIFIED;
    config.prune.delete_unverified = 2;
    require_invalid(table, &config, "delete_unverified");

    config.prune.set_fields = LANCEDB_PRUNE_ERROR_IF_TAGGED_OLD_VERSIONS;
    config.prune.error_if_tagged_old_versions = -1;
    require_invalid(table, &config, "error_if_tagged_old_versions");

    // None of the rejected calls changed the table.
    REQUIRE(read_stats(table).fragment_stats.num_fragments == 4);
    REQUIRE(lancedb_table_version(table) == 4);
    REQUIRE(lancedb_table_count_rows(table) == 80);

    lancedb_table_free(table);
  }
}


TEST_CASE_METHOD(LanceDBFixture, "LanceDB Index - invalid arguments", "[index]") {
  const std::string table_name = "index_invalid_args_test";
  LanceDBTable* table = create_table_with_data(table_name, 20, 0);
  REQUIRE(table != nullptr);

  const char* columns[] = {"key"};
  const char* null_columns[] = {nullptr};
  const char* invalid_columns[] = {INVALID_UTF8};
  LanceDBScalarIndexConfig config = {
    .replace = 1

  };

  SECTION("Scalar index with invalid arguments should fail") {
    REQUIRE(lancedb_table_create_scalar_index(
        nullptr, columns, 1, LANCEDB_INDEX_BTREE, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_scalar_index(
        table, nullptr, 1, LANCEDB_INDEX_BTREE, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 0, LANCEDB_INDEX_BTREE, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_scalar_index(
        table, null_columns, 1, LANCEDB_INDEX_BTREE, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_scalar_index(
        table, invalid_columns, 1, LANCEDB_INDEX_BTREE, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
    // A vector index type is not a scalar index type
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Scalar index of every supported type") {
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BITMAP, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    // Only the scalar index types are supported, AUTO is not one of them
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_AUTO, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("FTS index with an invalid column name should fail") {
    REQUIRE(lancedb_table_create_fts_index(
        table, invalid_columns, 1, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Listing indices with invalid arguments should fail") {
    char** indices = nullptr;
    size_t count = 0;

    REQUIRE(lancedb_table_list_indices(
        nullptr, &indices, &count, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_list_indices(
        table, nullptr, &count, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_list_indices(
        table, &indices, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);

    // Freeing an empty index list is safe
    lancedb_free_index_list(nullptr, 0);
  }

  SECTION("Dropping an index with invalid arguments should fail") {
    REQUIRE(lancedb_table_drop_index(nullptr, "idx", nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_drop_index(table, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_drop_index(table, INVALID_UTF8, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Index statistics with invalid arguments should fail") {
    LanceDBIndexStats stats = {};
    REQUIRE(lancedb_table_index_stats(table, nullptr, &stats, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_index_stats(table, "idx", nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_index_stats(table, INVALID_UTF8, &stats, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Wait For Index", "[index]") {
  const std::string table_name = "wait_for_index_test";
  LanceDBTable* table = create_table_with_data(table_name, 100, 0);
  REQUIRE(table != nullptr);

  const char* columns[] = {"key"};
  LanceDBScalarIndexConfig config = {
    .replace = 0
  };

  char* error_message = nullptr;
  REQUIRE(lancedb_table_create_scalar_index(
      table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  // the wait is addressed by name, so read back the name the index was created with
  char** indices = nullptr;
  size_t count = 0;
  REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count == 1);
  const std::string index_name = indices[0];
  lancedb_free_index_list(indices, count);

  SECTION("Waiting for a fully indexed index succeeds") {
    const char* index_names[] = {index_name.c_str()};
    REQUIRE(lancedb_table_wait_for_index(
        table, index_names, 1, 30 * 1000, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("Waiting for an index that does not exist times out") {
    const char* index_names[] = {"no_such_index"};
    REQUIRE(lancedb_table_wait_for_index(
        table, index_names, 1, 100, &error_message) == LANCEDB_TIMEOUT);
    REQUIRE(error_message != nullptr);
    INFO("Error message: " << error_message);
    // the SDK names the indices it was still waiting for
    REQUIRE(std::string(error_message).find("no_such_index") != std::string::npos);
    lancedb_free_string(error_message);
  }

  SECTION("A budget above the SDK maximum is rejected by the SDK") {
    const char* index_names[] = {index_name.c_str()};
    // the maximum belongs to the SDK: ask for three hours and expect its own report
    REQUIRE(lancedb_table_wait_for_index(
        table, index_names, 1, 3 * 60 * 60 * 1000, &error_message) == LANCEDB_INVALID_INPUT);
    REQUIRE(error_message != nullptr);
    INFO("Error message: " << error_message);
    REQUIRE(std::string(error_message).find("timeout must be less than") != std::string::npos);
    lancedb_free_string(error_message);
  }

  SECTION("Waiting with invalid arguments fails") {
    const char* index_names[] = {index_name.c_str()};
    const char* null_names[] = {nullptr};
    const char* invalid_names[] = {INVALID_UTF8};

    REQUIRE(lancedb_table_wait_for_index(
        nullptr, index_names, 1, 1000, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_wait_for_index(
        table, nullptr, 1, 1000, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_wait_for_index(
        table, index_names, 0, 1000, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_wait_for_index(
        table, null_names, 1, 1000, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_wait_for_index(
        table, invalid_names, 1, 1000, nullptr) == LANCEDB_INVALID_ARGUMENT);
    // a zero budget is a missing one, not an unbounded wait
    REQUIRE(lancedb_table_wait_for_index(
        table, index_names, 1, 0, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB C enum inputs are validated", "[index]") {
  const std::string table_name = "c_enum_validation_test";
  LanceDBTable* table = create_table_with_data(table_name, 20, 0);
  REQUIRE(table != nullptr);

  const char* scalar_columns[] = {"key"};
  const char* vector_columns[] = {"data"};
  const int invalid_index_types[] = {-1, 42};
  const int invalid_distance_types[] = {-1, 4};
  const int invalid_optimize_types[] = {-1, 4};

  SECTION("Scalar index rejects an out-of-range index type") {
    for (const int index_type : invalid_index_types) {
      char* error_message = nullptr;
      REQUIRE(lancedb_test_create_scalar_index_with_index_type(
          table, scalar_columns, 1, index_type, &error_message) == LANCEDB_INVALID_ARGUMENT);
      REQUIRE(error_message != nullptr);
      lancedb_free_string(error_message);
    }
  }

  SECTION("Vector index rejects an out-of-range index type") {
    for (const int index_type : invalid_index_types) {
      char* error_message = nullptr;
      REQUIRE(lancedb_test_create_vector_index_with_index_type(
          table, vector_columns, 1, index_type, &error_message) == LANCEDB_INVALID_ARGUMENT);
      REQUIRE(error_message != nullptr);
      lancedb_free_string(error_message);
    }
  }

  SECTION("Vector index rejects an out-of-range distance type") {
    for (const int distance_type : invalid_distance_types) {
      char* error_message = nullptr;
      REQUIRE(lancedb_test_create_vector_index_with_distance_type(
          table, vector_columns, 1, distance_type, &error_message) == LANCEDB_INVALID_ARGUMENT);
      REQUIRE(error_message != nullptr);
      lancedb_free_string(error_message);
    }
  }

  SECTION("Generic create rejects an out-of-range index type") {
    for (const int index_type : invalid_index_types) {
      char* error_message = nullptr;
      REQUIRE(lancedb_test_create_index_with_index_type(
          table, scalar_columns, 1, index_type, &error_message) == LANCEDB_INVALID_ARGUMENT);
      REQUIRE(error_message != nullptr);
      lancedb_free_string(error_message);
    }

    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, nullptr) == LANCEDB_SUCCESS);
    REQUIRE(count == 0);
    lancedb_free_index_list(indices, count);
  }

  SECTION("Optimize rejects an out-of-range type without changing the table") {
    const auto version_before = lancedb_table_version(table);
    for (const int optimize_type : invalid_optimize_types) {
      char* error_message = nullptr;
      REQUIRE(lancedb_test_optimize_with_type(
          table, optimize_type, &error_message) == LANCEDB_INVALID_ARGUMENT);
      REQUIRE(error_message != nullptr);
      lancedb_free_string(error_message);
    }
    REQUIRE(lancedb_table_version(table) == version_before);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Prewarm Index", "[index]") {
  const std::string table_name = "prewarm_index_test";

  // Create a BTREE index over the "key" column and return the index name the
  // SDK assigned to it.
  auto create_btree_index = [](LanceDBTable* table) {
    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {
      .replace = 0
    };

    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(indices != nullptr);

    std::string index_name = indices[0];
    lancedb_free_index_list(indices, count);
    return index_name;
  };

  SECTION("Prewarm an existing index") {
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    const std::string index_name = create_btree_index(table);
    INFO("Prewarming index: " << index_name);

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_prewarm_index(table, index_name.c_str(), &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Prewarming must not change the index list, and the index stays usable.
    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(std::string(indices[0]) == index_name);
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }

  SECTION("Prewarm a missing index returns INDEX_NOT_FOUND") {
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    // The table carries a real index, so the failure is about the requested name.
    create_btree_index(table);

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_prewarm_index(table, "no_such_index", &error_message);

    REQUIRE(result == LANCEDB_INDEX_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_table_free(table);
  }

  SECTION("Prewarm with invalid arguments fails") {
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    REQUIRE(lancedb_table_prewarm_index(nullptr, "idx", nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_prewarm_index(table, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_prewarm_index(table, INVALID_UTF8, nullptr) == LANCEDB_INVALID_ARGUMENT);

    lancedb_table_free(table);
  }
}
