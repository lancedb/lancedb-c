/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <map>
#include "test_common.h"

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Index", "[vector_index]") {
  const std::string table_name = "vector_index_test";

  SECTION("Create IVF_FLAT index on table with existing data") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);

    // Create IVF_FLAT index on the "data" column
    const char* columns[] = {"data"};
    LanceDBVectorIndexConfig config = {
      .num_partitions = -1,
      .num_sub_vectors = -1,
      .max_iterations = -1,
      .sample_rate = 0.0f,
      .distance_type = LANCEDB_DISTANCE_L2,
      .replace = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message);

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
      INFO("Created vector index: " << indices[0]);
    }

    // Free the index list
    lancedb_free_index_list(indices, count);

    // Add more data after index creation
    auto batch = create_test_record_batch(50, 256);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    result = lancedb_table_add(table, reader, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Verify total row count
    REQUIRE(lancedb_table_count_rows(table) == 306);

    lancedb_table_free(table);
  }

  SECTION("Create IVF_PQ index on table with existing data") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);

    // Create IVF_PQ index on the "data" column
    const char* columns[] = {"data"};
    LanceDBVectorIndexConfig config = {
      .num_partitions = -1,
      .num_sub_vectors = -1,
      .max_iterations = -1,
      .sample_rate = 0.0f,
      .distance_type = LANCEDB_DISTANCE_L2,
      .replace = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_PQ, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Add more data after index creation
    auto batch = create_test_record_batch(50, 256);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    result = lancedb_table_add(table, reader, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Verify total row count
    REQUIRE(lancedb_table_count_rows(table) == 306);

    lancedb_table_free(table);
  }

  SECTION("Create IVF_HNSW_PQ index on table with existing data") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);

    // Create IVF_HNSW_PQ index on the "data" column
    const char* columns[] = {"data"};
    LanceDBVectorIndexConfig config = {
      .num_partitions = -1,
      .num_sub_vectors = -1,
      .max_iterations = -1,
      .sample_rate = 0.0f,
      .distance_type = LANCEDB_DISTANCE_L2,
      .replace = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_HNSW_PQ, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Add more data after index creation
    auto batch = create_test_record_batch(50, 256);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    result = lancedb_table_add(table, reader, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Verify total row count
    REQUIRE(lancedb_table_count_rows(table) == 306);

    lancedb_table_free(table);
  }

  SECTION("Create IVF_HNSW_SQ index on table with existing data") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);

    // Create IVF_HNSW_SQ index on the "data" column
    const char* columns[] = {"data"};
    LanceDBVectorIndexConfig config = {
      .num_partitions = -1,
      .num_sub_vectors = -1,
      .max_iterations = -1,
      .sample_rate = 0.0f,
      .distance_type = LANCEDB_DISTANCE_L2,
      .replace = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_HNSW_SQ, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Add more data after index creation
    auto batch = create_test_record_batch(50, 256);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    result = lancedb_table_add(table, reader, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Verify total row count
    REQUIRE(lancedb_table_count_rows(table) == 306);

    lancedb_table_free(table);
  }

  SECTION("Create IVF_FLAT index on empty table should fail") {
    // Create empty table
    create_empty_table(table_name);
    LanceDBTable* table = open_table(table_name);

    // Try to create IVF_FLAT index on empty table (should fail - needs training data)
    const char* columns[] = {"data"};
    LanceDBVectorIndexConfig config = {
      .num_partitions = -1,
      .num_sub_vectors = -1,
      .max_iterations = -1,
      .sample_rate = 0.0f,
      .distance_type = LANCEDB_DISTANCE_L2,
      .replace = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message);

    // Vector index creation on empty table should fail
    REQUIRE(result != LANCEDB_SUCCESS);

    if (error_message) {
      INFO("Error message: " << error_message);
      lancedb_free_string(error_message);
    }

    lancedb_table_free(table);
  }

  SECTION("Replace existing vector index") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);

    // Create initial IVF_FLAT index
    const char* columns[] = {"data"};
    LanceDBVectorIndexConfig config = {
      .num_partitions = -1,
      .num_sub_vectors = -1,
      .max_iterations = -1,
      .sample_rate = 0.0f,
      .distance_type = LANCEDB_DISTANCE_L2,
      .replace = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Replace with IVF_PQ index
    config.replace = 1;
    result = lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_PQ, &config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    lancedb_table_free(table);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Index List and Drop", "[vector_index]") {
  const std::string table_name = "vector_index_list_drop_test";

  SECTION("Drop vector index") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);

    // Create IVF_FLAT index
    const char* columns[] = {"data"};
    LanceDBVectorIndexConfig config = {
      .num_partitions = -1,
      .num_sub_vectors = -1,
      .max_iterations = -1,
      .sample_rate = 0.0f,
      .distance_type = LANCEDB_DISTANCE_L2,
      .replace = 0
    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message);

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

  SECTION("Drop one index when table has both scalar and vector indices") {
    // Create table with data
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);

    // Create BTREE index on key column
    const char* scalar_columns[] = {"key"};
    LanceDBScalarIndexConfig scalar_config = {
      .replace = 0

    };

    char* error_message = nullptr;
    LanceDBError result = lancedb_table_create_scalar_index(
        table, scalar_columns, 1, LANCEDB_INDEX_BTREE, &scalar_config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Create IVF_FLAT index on data column
    const char* vector_columns[] = {"data"};
    LanceDBVectorIndexConfig vector_config = {
      .num_partitions = -1,
      .num_sub_vectors = -1,
      .max_iterations = -1,
      .sample_rate = 0.0f,
      .distance_type = LANCEDB_DISTANCE_L2,
      .replace = 0
    };

    result = lancedb_table_create_vector_index(
        table, vector_columns, 1, LANCEDB_INDEX_IVF_FLAT, &vector_config, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // List indices (should have two indices)
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 2);
    REQUIRE(indices != nullptr);

    // Print both indices for debugging
    for (size_t i = 0; i < count; i++) {
      INFO("Index " << i << ": " << indices[i]);
    }

    // Save the first index name and drop it
    std::string first_index_name = indices[0];
    INFO("Dropping first index: " << first_index_name);

    // Free the index list
    lancedb_free_index_list(indices, count);

    // Drop the first index
    result = lancedb_table_drop_index(table, first_index_name.c_str(), &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // List indices again (should have one index remaining)
    indices = nullptr;
    count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(indices != nullptr);

    INFO("Remaining index: " << indices[0]);

    // Free the index list
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }
}


TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Index - explicit tuning parameters", "[vector_index]") {
  const std::string table_name = "vector_index_tuning_test";
  LanceDBTable* table = create_table_with_data(table_name, 300, 0);
  REQUIRE(table != nullptr);

  const char* columns[] = {"data"};
  LanceDBVectorIndexConfig config = {
    .num_partitions = 2,
    .num_sub_vectors = 2,
    .max_iterations = 2,
    .sample_rate = 64.0f,
    .distance_type = LANCEDB_DISTANCE_COSINE,
    .replace = 1
  };

  SECTION("IVF_FLAT index with all the tuning parameters") {
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("IVF_PQ index with all the tuning parameters") {
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_PQ, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("IVF_HNSW_PQ index with all the tuning parameters") {
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_HNSW_PQ, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("IVF_HNSW_SQ index with all the tuning parameters") {
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_HNSW_SQ, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("IVF_SQ index with all the tuning parameters") {
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_SQ, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("IVF_RQ index with all the tuning parameters") {
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_RQ, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("IVF_HNSW_FLAT index with all the tuning parameters") {
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_HNSW_FLAT, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("Vector index of every type with the default configuration") {
    for (const LanceDBIndexType index_type : {
        LANCEDB_INDEX_IVF_FLAT, LANCEDB_INDEX_IVF_SQ, LANCEDB_INDEX_IVF_PQ, LANCEDB_INDEX_IVF_RQ,
        LANCEDB_INDEX_IVF_HNSW_PQ, LANCEDB_INDEX_IVF_HNSW_SQ, LANCEDB_INDEX_IVF_HNSW_FLAT}) {
      CAPTURE(index_type);
      char* error_message = nullptr;
      REQUIRE(lancedb_table_create_vector_index(
          table, columns, 1, index_type, nullptr, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }
  }

  SECTION("Vector index with invalid arguments should fail") {
    REQUIRE(lancedb_table_create_vector_index(
        nullptr, columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_vector_index(
        table, nullptr, 1, LANCEDB_INDEX_IVF_FLAT, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 0, LANCEDB_INDEX_IVF_FLAT, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);

    const char* null_columns[] = {nullptr};
    REQUIRE(lancedb_table_create_vector_index(
        table, null_columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);

    // A scalar index type is not a vector index type
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, nullptr) != LANCEDB_SUCCESS);
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_FM, &config, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  lancedb_table_free(table);
}

// Number of IVF partitions of the index created by create_vector_index()
static constexpr int REINDEX_NUM_PARTITIONS = 4;

// Create a vector index on the "data" column
static void create_vector_index(LanceDBTable* table, LanceDBIndexType index_type = LANCEDB_INDEX_IVF_PQ) {
  const char* columns[] = {"data"};
  LanceDBVectorIndexConfig config = {
    .num_partitions = REINDEX_NUM_PARTITIONS,
    .num_sub_vectors = 2,
    .max_iterations = -1,
    .sample_rate = 0.0f,
    .distance_type = LANCEDB_DISTANCE_L2,
    .replace = 0
  };
  char* error_message = nullptr;
  REQUIRE(lancedb_table_create_vector_index(
      table, columns, 1, index_type, &config, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
}

// Create a BTREE index on the "key" column
static void create_btree_index(LanceDBTable* table) {
  const char* columns[] = {"key"};
  LanceDBScalarIndexConfig config = {.replace = 0};
  char* error_message = nullptr;
  REQUIRE(lancedb_table_create_scalar_index(
      table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
}

// The names of the indices on the table
static std::vector<std::string> list_index_names(LanceDBTable* table) {
  char** indices = nullptr;
  size_t count = 0;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  std::vector<std::string> names;
  for (size_t i = 0; i < count; ++i) {
    names.emplace_back(indices[i]);
  }
  lancedb_free_index_list(indices, count);
  return names;
}

// The name of the only index on the table
static std::string single_index_name(LanceDBTable* table) {
  const auto names = list_index_names(table);
  REQUIRE(names.size() == 1);
  return names[0];
}

static void add_rows(LanceDBTable* table, int num_rows, int start_index) {
  auto reader = create_reader_from_batch(create_test_record_batch(num_rows, start_index));
  REQUIRE(reader != nullptr);
  char* error_message = nullptr;
  REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
}

static LanceDBIndexStats index_stats(LanceDBTable* table, const std::string& index_name) {
  LanceDBIndexStats stats = {};
  char* error_message = nullptr;
  REQUIRE(lancedb_table_index_stats(table, index_name.c_str(), &stats, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  return stats;
}

static void optimize_indices(LanceDBTable* table, const LanceDBOptimizeIndexOptions* options) {
  char* error_message = nullptr;
  REQUIRE(lancedb_table_optimize_indices(table, options, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
}

static void optimize_indices(LanceDBTable* table, LanceDBReindexMode mode) {
  LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
  options.mode = mode;
  optimize_indices(table, &options);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Optimize Indices", "[vector_index]") {
  const std::string table_name = "optimize_indices_test";
  // Every reindex mode is exercised on every vector index type
  const LanceDBIndexType index_type = GENERATE(
      LANCEDB_INDEX_IVF_FLAT, LANCEDB_INDEX_IVF_SQ, LANCEDB_INDEX_IVF_PQ, LANCEDB_INDEX_IVF_RQ,
      LANCEDB_INDEX_IVF_HNSW_PQ, LANCEDB_INDEX_IVF_HNSW_SQ, LANCEDB_INDEX_IVF_HNSW_FLAT);
  CAPTURE(index_type);

  SECTION("APPEND adds a delta index covering the new rows") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);
    create_vector_index(table, index_type);
    const std::string index_name = single_index_name(table);
    add_rows(table, 50, 256);

    LanceDBIndexStats stats = index_stats(table, index_name);
    REQUIRE(stats.num_indices == 1);
    REQUIRE(stats.num_indexed_rows == 256);
    REQUIRE(stats.num_unindexed_rows == 50);

    optimize_indices(table, LANCEDB_REINDEX_APPEND);

    stats = index_stats(table, index_name);
    REQUIRE(stats.num_indices == 2);
    REQUIRE(stats.num_indexed_rows == 306);
    REQUIRE(stats.num_unindexed_rows == 0);

    lancedb_table_free(table);
  }

  SECTION("MERGE collapses the delta indices") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);
    create_vector_index(table, index_type);
    const std::string index_name = single_index_name(table);

    // Two appends leave three segments
    add_rows(table, 50, 256);
    optimize_indices(table, LANCEDB_REINDEX_APPEND);
    add_rows(table, 50, 306);
    optimize_indices(table, LANCEDB_REINDEX_APPEND);
    REQUIRE(index_stats(table, index_name).num_indices == 3);

    // Merging one segment folds the latest segment into a new one
    LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
    options.mode = LANCEDB_REINDEX_MERGE;
    options.num_indices_to_merge = 1;
    optimize_indices(table, &options);
    REQUIRE(index_stats(table, index_name).num_indices == 3);

    // Merging more segments than there are folds all of them into one
    options.num_indices_to_merge = 100;
    optimize_indices(table, &options);
    LanceDBIndexStats stats = index_stats(table, index_name);
    REQUIRE(stats.num_indices == 1);
    REQUIRE(stats.num_indexed_rows == 356);
    REQUIRE(stats.num_unindexed_rows == 0);

    lancedb_table_free(table);
  }

  SECTION("MERGE with -1 lets Lance pick the segments to merge") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);
    create_vector_index(table, index_type);
    const std::string index_name = single_index_name(table);
    add_rows(table, 50, 256);
    optimize_indices(table, LANCEDB_REINDEX_APPEND);
    add_rows(table, 50, 306);

    LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
    options.mode = LANCEDB_REINDEX_MERGE;
    options.num_indices_to_merge = -1;
    optimize_indices(table, &options);

    // Everything is indexed, how many segments remain is up to Lance
    LanceDBIndexStats stats = index_stats(table, index_name);
    INFO("Segments after merging with -1: " << stats.num_indices);
    REQUIRE(stats.num_indices >= 1);
    REQUIRE(stats.num_indexed_rows == 356);
    REQUIRE(stats.num_unindexed_rows == 0);

    lancedb_table_free(table);
  }

  SECTION("RETRAIN collapses a vector index to a single segment") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);
    create_vector_index(table, index_type);
    const std::string index_name = single_index_name(table);
    add_rows(table, 50, 256);
    optimize_indices(table, LANCEDB_REINDEX_APPEND);
    add_rows(table, 50, 306);
    REQUIRE(index_stats(table, index_name).num_indices == 2);

    optimize_indices(table, LANCEDB_REINDEX_RETRAIN);

    LanceDBIndexStats stats = index_stats(table, index_name);
    REQUIRE(stats.num_indices == 1);
    REQUIRE(stats.num_indexed_rows == 356);
    REQUIRE(stats.num_unindexed_rows == 0);
    // The retrain re-fits the partitions to the data, so their count may change
    REQUIRE(stats.num_partitions > 0);

    lancedb_table_free(table);
  }

}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Optimize Indices Options", "[vector_index]") {
  const std::string table_name = "optimize_indices_options_test";

  SECTION("The default options match the legacy index optimization") {
    const LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
    REQUIRE(options.mode == LANCEDB_REINDEX_DEFAULT);
    REQUIRE(options.num_indices_to_merge == -1);
    REQUIRE(options.index_names == nullptr);
    REQUIRE(options.num_index_names == 0);
    REQUIRE(options.transaction_property_keys == nullptr);
    REQUIRE(options.transaction_property_values == nullptr);
    REQUIRE(options.num_transaction_properties == 0);
    REQUIRE(options.progress_callback == nullptr);
    REQUIRE(options.progress_user_data == nullptr);
  }

  SECTION("Progress of building a vector index is reported") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);
    create_vector_index(table);
    const std::string index_name = single_index_name(table);
    add_rows(table, 50, 256);

    IndexProgressRecorder recorder;
    LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
    options.progress_callback = IndexProgressRecorder::callback;
    options.progress_user_data = &recorder;

    // The names of the stages are up to Lance, so only their pairing is verified
    for (const LanceDBReindexMode mode : {LANCEDB_REINDEX_APPEND, LANCEDB_REINDEX_RETRAIN}) {
      CAPTURE(mode);
      recorder.started.clear();
      recorder.completed.clear();
      recorder.num_progress = 0;
      options.mode = mode;
      optimize_indices(table, &options);

      REQUIRE(recorder.valid);
      REQUIRE(!recorder.started.empty());
      REQUIRE(recorder.all_completed());
      REQUIRE(recorder.num_progress > 0);
    }
    REQUIRE(index_stats(table, index_name).num_unindexed_rows == 0);

    lancedb_table_free(table);
  }

  SECTION("Only the named indices are optimized") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);
    create_vector_index(table);
    const std::string vector_index_name = single_index_name(table);
    create_btree_index(table);
    std::string btree_index_name;
    for (const auto& name : list_index_names(table)) {
      if (name != vector_index_name) {
        btree_index_name = name;
      }
    }
    REQUIRE(!btree_index_name.empty());
    add_rows(table, 50, 256);

    const char* names[] = {vector_index_name.c_str()};
    LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
    options.mode = LANCEDB_REINDEX_APPEND;
    options.index_names = names;
    options.num_index_names = 1;
    optimize_indices(table, &options);

    REQUIRE(index_stats(table, vector_index_name).num_unindexed_rows == 0);
    REQUIRE(index_stats(table, btree_index_name).num_unindexed_rows == 50);

    lancedb_table_free(table);
  }

  SECTION("An unknown index name is reported") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);
    create_vector_index(table);

    const char* names[] = {"no_such_index"};
    LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
    options.index_names = names;
    options.num_index_names = 1;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_optimize_indices(table, &options, &error_message) == LANCEDB_INDEX_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("no_such_index") != std::string::npos);
    lancedb_free_string(error_message);

    lancedb_table_free(table);
  }

  SECTION("NULL options behave like the legacy index optimization") {
    LanceDBTable* legacy_table = create_table_with_data(table_name + "_legacy", 256, 0);
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(legacy_table != nullptr);
    REQUIRE(table != nullptr);
    for (auto* t : {legacy_table, table}) {
      create_vector_index(t);
      add_rows(t, 50, 256);
    }

    char* error_message = nullptr;
    REQUIRE(lancedb_table_optimize(legacy_table, LANCEDB_OPTIMIZE_INDEX, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    optimize_indices(table, nullptr);

    const LanceDBIndexStats legacy_stats = index_stats(legacy_table, single_index_name(legacy_table));
    const LanceDBIndexStats stats = index_stats(table, single_index_name(table));
    REQUIRE(stats.num_indices == legacy_stats.num_indices);
    REQUIRE(stats.num_indexed_rows == legacy_stats.num_indexed_rows);
    REQUIRE(stats.num_unindexed_rows == legacy_stats.num_unindexed_rows);
    REQUIRE(stats.num_unindexed_rows == 0);

    lancedb_table_free(legacy_table);
    lancedb_table_free(table);
  }

  SECTION("Transaction properties are stored with the commit") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);
    create_vector_index(table);
    const std::string index_name = single_index_name(table);
    add_rows(table, 50, 256);
    const unsigned long long version_before = lancedb_table_version(table);

    const char* keys[] = {"job_id", "source"};
    const char* values[] = {"42", "test"};
    LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
    options.mode = LANCEDB_REINDEX_APPEND;
    options.transaction_property_keys = keys;
    options.transaction_property_values = values;
    options.num_transaction_properties = 2;
    optimize_indices(table, &options);

    // The reindex committed a new version
    const unsigned long long version_after = lancedb_table_version(table);
    REQUIRE(version_after > version_before);
    REQUIRE(index_stats(table, index_name).num_unindexed_rows == 0);

    // The properties are read back, in no particular order, from the current version
    // and by its number
    for (const unsigned long long version : {0ULL, version_after}) {
      CAPTURE(version);
      char** keys_out = nullptr;
      char** values_out = nullptr;
      size_t count = 0;
      char* error_message = nullptr;
      REQUIRE(lancedb_table_get_transaction_properties(
          table, version, &keys_out, &values_out, &count, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
      REQUIRE(count == 2);
      const std::map<std::string, std::string> properties = {
        {keys_out[0], values_out[0]}, {keys_out[1], values_out[1]}};
      REQUIRE(properties == std::map<std::string, std::string>{{"job_id", "42"}, {"source", "test"}});
      lancedb_free_metadata(keys_out, values_out, count);
    }

    // The version before the reindex was committed with no properties
    char** keys_out = nullptr;
    char** values_out = nullptr;
    size_t count = 1;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_get_transaction_properties(
        table, version_before, &keys_out, &values_out, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 0);
    REQUIRE(keys_out == nullptr);
    REQUIRE(values_out == nullptr);

    // A version that does not exist is an error
    REQUIRE(lancedb_table_get_transaction_properties(
        table, version_after + 100, &keys_out, &values_out, &count, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    INFO("Error for a version that does not exist: " << error_message);
    lancedb_free_string(error_message);

    REQUIRE(lancedb_table_get_transaction_properties(
        nullptr, 0, &keys_out, &values_out, &count, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_get_transaction_properties(
        table, 0, nullptr, &values_out, &count, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_get_transaction_properties(
        table, 0, &keys_out, nullptr, &count, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_get_transaction_properties(
        table, 0, &keys_out, &values_out, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);

    lancedb_table_free(table);
  }

  SECTION("Invalid arguments are rejected") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);
    create_vector_index(table);
    const std::string index_name = single_index_name(table);
    const char* names[] = {index_name.c_str()};
    const char* invalid_names[] = {"\xff\xfe"};
    const char* null_names[] = {nullptr};

    REQUIRE(lancedb_table_optimize_indices(nullptr, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);

    LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
    options.mode = LANCEDB_REINDEX_MERGE;
    options.num_indices_to_merge = -2;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_optimize_indices(table, &options, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    // A negative merge count is ignored outside of the MERGE mode
    options.mode = LANCEDB_REINDEX_APPEND;
    optimize_indices(table, &options);

    options = lancedb_optimize_index_options_default();
    options.num_index_names = 1;
    REQUIRE(lancedb_table_optimize_indices(table, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);
    options.index_names = invalid_names;
    REQUIRE(lancedb_table_optimize_indices(table, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);
    options.index_names = null_names;
    REQUIRE(lancedb_table_optimize_indices(table, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);

    options = lancedb_optimize_index_options_default();
    options.num_transaction_properties = 1;
    options.transaction_property_keys = names;
    REQUIRE(lancedb_table_optimize_indices(table, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);
    options.transaction_property_keys = nullptr;
    options.transaction_property_values = names;
    REQUIRE(lancedb_table_optimize_indices(table, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);
    options.transaction_property_keys = invalid_names;
    REQUIRE(lancedb_table_optimize_indices(table, &options, nullptr) == LANCEDB_INVALID_ARGUMENT);

    lancedb_table_free(table);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Index Stats", "[vector_index]") {
  const std::string table_name = "vector_index_stats_test";

  SECTION("Stats describe the index and its coverage") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);
    // An index records the table version it was trained on, which is the version
    // before the commit that added it
    const unsigned long long index_version = lancedb_table_version(table);
    create_vector_index(table);
    const std::string index_name = single_index_name(table);
    REQUIRE(lancedb_table_version(table) == index_version + 1);

    LanceDBIndexStats stats = index_stats(table, index_name);
    REQUIRE(stats.index_type == LANCEDB_INDEX_IVF_PQ);
    REQUIRE(stats.num_partitions == REINDEX_NUM_PARTITIONS);
    REQUIRE(stats.num_indices == 1);
    REQUIRE(stats.num_indexed_fragments == 1);
    REQUIRE(stats.num_unindexed_fragments == 0);
    REQUIRE(stats.num_deleted_indexed_rows == 0);
    REQUIRE(stats.dataset_version == index_version);

    // New rows land in a fragment the index does not cover
    add_rows(table, 50, 256);
    stats = index_stats(table, index_name);
    REQUIRE(stats.num_indexed_fragments == 1);
    REQUIRE(stats.num_unindexed_fragments == 1);
    REQUIRE(stats.dataset_version == index_version);

    // Deleting indexed rows is counted, deleting unindexed rows is not
    char* error_message = nullptr;
    REQUIRE(lancedb_table_delete(table, "key IN ('key_0', 'key_1', 'key_300')", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    stats = index_stats(table, index_name);
    REQUIRE(stats.num_deleted_indexed_rows == 2);

    // Optimizing the index covers the new fragment and moves the index version forward
    REQUIRE(lancedb_table_optimize(table, LANCEDB_OPTIMIZE_INDEX, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    stats = index_stats(table, index_name);
    REQUIRE(stats.num_indexed_fragments == 2);
    REQUIRE(stats.num_unindexed_fragments == 0);
    REQUIRE(stats.num_deleted_indexed_rows == 3);
    REQUIRE(stats.dataset_version > index_version);
    REQUIRE(stats.dataset_version <= lancedb_table_version(table));

    lancedb_table_free(table);
  }
}
