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
    REQUIRE(stats.num_indices == 1);
    REQUIRE(stats.num_indexed_fragments == 1);
    REQUIRE(stats.num_unindexed_fragments == 0);
    REQUIRE(stats.num_deleted_indexed_rows == 0);
    REQUIRE(stats.index_type == LANCEDB_INDEX_BTREE);
    // A scalar index has no partitions
    REQUIRE(stats.num_partitions == 0);
    // The index was trained on the version before the commit that added it
    REQUIRE(stats.dataset_version + 1 == lancedb_table_version(table));

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
    REQUIRE(stats.num_indexed_fragments == 1);
    REQUIRE(stats.num_unindexed_fragments == 1);

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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Optimize", "[index]") {
  const std::string table_name = "optimize_test";

  SECTION("Optimize a table with fragments of unindexed rows") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {.replace = 0};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(count == 1);
    const std::string index_name = indices[0];
    lancedb_free_index_list(indices, count);

    // Add more rows in two batches. Each batch is written to a fragment of its own, that
    // is not covered by the index, so that there is something to compact and to index.
    // Lance does not compact an indexed fragment with an unindexed one, so that two
    // unindexed fragments are needed for a compaction
    for (const int start_index : {20, 40}) {
      auto reader = create_reader_from_batch(create_test_record_batch(20, start_index));
      REQUIRE(reader != nullptr);
      REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }

    const auto num_fragments = [&] {
      LanceDBTableStats stats = {};
      REQUIRE(lancedb_table_stats(table, &stats, nullptr) == LANCEDB_SUCCESS);
      return stats.fragment_stats.num_fragments;
    };
    const auto num_unindexed_rows = [&] {
      LanceDBIndexStats stats = {};
      REQUIRE(lancedb_table_index_stats(table, index_name.c_str(), &stats, nullptr) == LANCEDB_SUCCESS);
      return stats.num_unindexed_rows;
    };
    const auto num_versions = [&] {
      LanceDBVersion* versions = nullptr;
      size_t num_versions = 0;
      REQUIRE(lancedb_table_list_versions(table, &versions, nullptr, &num_versions, nullptr) == LANCEDB_SUCCESS);
      lancedb_free_versions(versions, num_versions);
      return num_versions;
    };
    const auto optimize = [&](LanceDBOptimizeType type) {
      REQUIRE(lancedb_table_optimize(table, type, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    };

    REQUIRE(num_fragments() == 3);
    REQUIRE(num_unindexed_rows() == 40);

    SECTION("COMPACT merges the unindexed fragments and leaves the index alone") {
      optimize(LANCEDB_OPTIMIZE_COMPACT);
      REQUIRE(num_fragments() == 2);
      REQUIRE(num_unindexed_rows() == 40);
    }

    SECTION("INDEX covers the new rows and leaves the fragments alone") {
      optimize(LANCEDB_OPTIMIZE_INDEX);
      REQUIRE(num_unindexed_rows() == 0);
      REQUIRE(num_fragments() == 3);
    }

    SECTION("PRUNE keeps the recent versions") {
      // Only the versions older than a week are removed, and the table has none
      const size_t versions_before = num_versions();
      optimize(LANCEDB_OPTIMIZE_PRUNE);
      REQUIRE(num_versions() == versions_before);
      REQUIRE(num_fragments() == 3);
      REQUIRE(num_unindexed_rows() == 40);
    }

    SECTION("ALL compacts the unindexed fragments and covers the new rows") {
      optimize(LANCEDB_OPTIMIZE_ALL);
      REQUIRE(num_fragments() == 2);
      REQUIRE(num_unindexed_rows() == 0);
    }

    // No rows were lost by the optimization
    REQUIRE(lancedb_table_count_rows(table) == 60);

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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Optimize Scalar Indices", "[index]") {
  const std::string table_name = "optimize_scalar_indices_test";
  LanceDBTable* table = create_table_with_data(table_name, 20, 0);
  REQUIRE(table != nullptr);

  const char* columns[] = {"key"};
  LanceDBScalarIndexConfig config = {.replace = 0};
  char* error_message = nullptr;
  REQUIRE(lancedb_table_create_scalar_index(
      table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  char** indices = nullptr;
  size_t count = 0;
  REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(count == 1);
  const std::string index_name = indices[0];
  lancedb_free_index_list(indices, count);

  const auto add_rows = [&](int num_rows, int start_index) {
    auto reader = create_reader_from_batch(create_test_record_batch(num_rows, start_index));
    REQUIRE(reader != nullptr);
    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  };
  const auto index_stats = [&] {
    LanceDBIndexStats stats = {};
    REQUIRE(lancedb_table_index_stats(table, index_name.c_str(), &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    return stats;
  };
  LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
  const auto optimize_indices = [&] {
    REQUIRE(lancedb_table_optimize_indices(table, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  };

  // Add rows the index does not cover
  add_rows(20, 20);

  LanceDBIndexStats stats = index_stats();
  REQUIRE(stats.num_indices == 1);
  REQUIRE(stats.num_indexed_rows == 20);
  REQUIRE(stats.num_unindexed_rows == 20);
  REQUIRE(stats.num_indexed_fragments == 1);
  REQUIRE(stats.num_unindexed_fragments == 1);

  SECTION("DEFAULT merges the new rows into the existing segment") {
    REQUIRE(options.mode == LANCEDB_REINDEX_DEFAULT);
    optimize_indices();

    stats = index_stats();
    REQUIRE(stats.num_indices == 1);
    REQUIRE(stats.num_indexed_rows == 40);
    REQUIRE(stats.num_unindexed_rows == 0);
    REQUIRE(stats.num_indexed_fragments == 2);
    REQUIRE(stats.num_unindexed_fragments == 0);
  }

  SECTION("APPEND covers the new rows with a new segment") {
    options.mode = LANCEDB_REINDEX_APPEND;
    optimize_indices();

    stats = index_stats();
    REQUIRE(stats.num_indices == 2);
    REQUIRE(stats.num_indexed_rows == 40);
    REQUIRE(stats.num_unindexed_rows == 0);
    REQUIRE(stats.num_indexed_fragments == 2);
    REQUIRE(stats.num_unindexed_fragments == 0);
  }

  SECTION("MERGE collapses the segments") {
    // Append a segment, and add more rows that no segment covers
    options.mode = LANCEDB_REINDEX_APPEND;
    optimize_indices();
    add_rows(20, 40);

    stats = index_stats();
    REQUIRE(stats.num_indices == 2);
    REQUIRE(stats.num_indexed_rows == 40);
    REQUIRE(stats.num_unindexed_rows == 20);

    // Merge the new rows with both segments
    options.mode = LANCEDB_REINDEX_MERGE;
    options.num_indices_to_merge = 2;
    optimize_indices();

    stats = index_stats();
    REQUIRE(stats.num_indices == 1);
    REQUIRE(stats.num_indexed_rows == 60);
    REQUIRE(stats.num_unindexed_rows == 0);
    REQUIRE(stats.num_indexed_fragments == 3);
    REQUIRE(stats.num_unindexed_fragments == 0);
  }

  SECTION("RETRAIN of a scalar index behaves like DEFAULT") {
    // Append a segment, and add more rows that no segment covers
    options.mode = LANCEDB_REINDEX_APPEND;
    optimize_indices();
    add_rows(20, 40);

    stats = index_stats();
    REQUIRE(stats.num_indices == 2);
    REQUIRE(stats.num_indexed_rows == 40);
    REQUIRE(stats.num_unindexed_rows == 20);

    // A scalar index has nothing to train, so the new rows are merged into the newest
    // segment, and the segments are not collapsed
    options.mode = LANCEDB_REINDEX_RETRAIN;
    optimize_indices();

    stats = index_stats();
    REQUIRE(stats.num_indices == 2);
    REQUIRE(stats.num_indexed_rows == 60);
    REQUIRE(stats.num_unindexed_rows == 0);
    REQUIRE(stats.num_indexed_fragments == 3);
    REQUIRE(stats.num_unindexed_fragments == 0);
  }

  lancedb_table_free(table);
}

// A string that is not valid UTF-8, to exercise the string conversion failures
static const char* const INVALID_UTF8 = "\xff\xfe";

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
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_IVF_SQ, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Scalar index of every supported type") {
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BITMAP, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    // FM is a substring search index over a string column
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_FM, &config, &error_message) == LANCEDB_SUCCESS);
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
