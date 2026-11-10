/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

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
