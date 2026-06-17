/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"
#include <limits>
#include <random>

// Helper function to generate random query vector
static std::vector<float> generate_random_query_vector(size_t dimensions) {
  std::vector<float> query_vector(dimensions);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<float> dis(0.0f, 10.0f);

  for (size_t i = 0; i < dimensions; i++) {
    query_vector[i] = dis(gen);
  }

  return query_vector;
}

// Helper function to generate deterministic query vector for a fixed seed
static std::vector<float> generate_random_query_vector(size_t dimensions, uint32_t seed) {
  std::vector<float> query_vector(dimensions);
  std::mt19937 gen(seed);
  std::uniform_real_distribution<float> dis(0.0f, 10.0f);

  for (size_t i = 0; i < dimensions; i++) {
    query_vector[i] = dis(gen);
  }

  return query_vector;
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - nearest_to without index", "[vector_query]") {
  const std::string table_name = "vector_query_test";
  constexpr size_t total_rows = 100;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  SECTION("Query nearest neighbors with limit 5") {
    // Create random query vector
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    char* error_message = nullptr;
    constexpr size_t limit = 5;

    LanceDBError result = lancedb_table_nearest_to(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS,
        limit,  // limit
        "data",  // vector column name
        &result_arrays,
        &result_schema,
        &count,
        &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(result_arrays != nullptr);
    REQUIRE(result_schema != nullptr);
    REQUIRE(count > 0);

    // Count total rows across all batches
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == limit);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Query with NULL column name (should use default and succeed)") {
    // Create random query vector
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    char* error_message = nullptr;
    constexpr size_t limit = 5;

    // NULL column name should succeed - the API finds the "data" vector column
    LanceDBError result = lancedb_table_nearest_to(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS,
        limit,
        nullptr,  // NULL column name - API will find vector column
        &result_arrays,
        &result_schema,
        &count,
        &error_message);

    // Should succeed because the API finds the "data" vector column
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(result_arrays != nullptr);
    REQUIRE(result_schema != nullptr);
    REQUIRE(count > 0);

    // Count total rows
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == limit);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - paged query with limit and offset", "[vector_query]") {
  const std::string table_name = "vector_query_paged_test";
  constexpr size_t total_rows = 100;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  SECTION("Query all entries in pages using vector query API") {
    // Create random query vector once for all pages
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    // Query multiple pages - need to create a new query for each page
    // because lancedb_vector_query_execute() consumes the query object
    constexpr size_t limit = 30;
    size_t offset = 0;
    size_t total_fetched = 0;

    while (offset < total_rows) {
      // Create a new vector query for each page
      LanceDBVectorQuery* query = lancedb_vector_query_new(
          table,
          query_vector.data(),
          TEST_SCHEMA_DIMENSIONS
      );
      REQUIRE(query != nullptr);

      // Set limit
      char* error_message = nullptr;
      LanceDBError result = lancedb_vector_query_limit(query, limit, &error_message);
      REQUIRE(result == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);

      // Set offset
      error_message = nullptr;
      result = lancedb_vector_query_offset(query, offset, &error_message);
      REQUIRE(result == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);

      // Select "key" and "data" columns
      const char* columns[] = {"key", "data"};
      error_message = nullptr;
      result = lancedb_vector_query_select(query, columns, 2, &error_message);
      REQUIRE(result == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);

      // Execute query (consumes the query object)
      LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
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
      REQUIRE(count > 0);
      REQUIRE(result_arrays != nullptr);
      REQUIRE(result_schema != nullptr);

      // Count total rows in this page
      size_t page_rows = 0;
      for (size_t i = 0; i < count; i++) {
        page_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
      }

      // Verify this page has the expected number of rows
      size_t expected_rows = std::min(limit, total_rows - offset);
      REQUIRE(page_rows == expected_rows);

      total_fetched += page_rows;

      // Verify schema has 3 columns (key, data, and distance/score)
      REQUIRE(reinterpret_cast<ArrowSchema*>(result_schema)->n_children == 3);

      // Clean up
      lancedb_free_arrow_arrays(result_arrays, count);
      lancedb_free_arrow_schema(result_schema);

      offset += limit;
    }

    // Verify we fetched all rows
    REQUIRE(total_fetched == total_rows);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - nearest_to with IVF_FLAT index", "[vector_query]") {
  const std::string table_name = "vector_query_indexed_test";
  constexpr size_t total_rows = 256;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  // Create IVF_FLAT vector index on "data" column
  const char* vector_columns[] = {"data"};
  LanceDBVectorIndexConfig config = {
    .num_partitions = 4,
    .num_sub_vectors = -1,
    .max_iterations = -1,
    .sample_rate = 0.0f,
    .distance_type = LANCEDB_DISTANCE_L2,
    .accelerator = nullptr,
    .replace = 0
  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message);

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  SECTION("Query with vector index") {
    // Create random query vector
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    constexpr size_t limit = 5;

    result = lancedb_table_nearest_to(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS,
        limit,
        "data",
        &result_arrays,
        &result_schema,
        &count,
        &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(result_arrays != nullptr);
    REQUIRE(result_schema != nullptr);
    REQUIRE(count > 0);

    // Count total rows
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == limit);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Query more results than available") {
    // Create random query vector
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    constexpr size_t limit = 500;

    // Request more rows than exist in table
    result = lancedb_table_nearest_to(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS,
        limit,  // more than the 256 rows in table
        "data",
        &result_arrays,
        &result_schema,
        &count,
        &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(result_arrays != nullptr);
    REQUIRE(result_schema != nullptr);
    REQUIRE(count > 0);

    // Should return all 256 rows
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == total_rows);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  lancedb_table_free(table);
}

void verify_vector_row_count(size_t limit, LanceDBVectorQuery* query) {
    char* error_message = nullptr;
    auto result = lancedb_vector_query_limit(query, limit, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));
    REQUIRE(count > 0);
    REQUIRE(result_arrays != nullptr);

    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == limit);

    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
}


TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - configuration parameters", "[vector_query]") {
  const std::string table_name = "vector_query_config_test";
  constexpr size_t total_rows = 2048;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  // Create vector index on "data" column for testing nprobes
  const char* vector_columns[] = {"data"};
  LanceDBVectorIndexConfig config = {
    .num_partitions = 4,
    .num_sub_vectors = -1,
    .max_iterations = -1,
    .sample_rate = 0.0f,
    .distance_type = LANCEDB_DISTANCE_L2,
    .accelerator = nullptr,
    .replace = 0
  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_PQ, &config, &error_message);

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  SECTION("Test distance_type parameter with L2 distance") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    error_message = nullptr;
    result = lancedb_vector_query_distance_type(query, LANCEDB_DISTANCE_L2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 5, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count > 0);
    REQUIRE(result_arrays != nullptr);

    // Count total rows
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == 5);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Test distance_type parameter with Cosine distance") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set Cosine distance type
    error_message = nullptr;
    result = lancedb_vector_query_distance_type(query, LANCEDB_DISTANCE_COSINE, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 5, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count > 0);

    // Count total rows
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == 5);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Test nprobes parameter with IVF index") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set nprobes to 2 (we have 4 partitions in the index)
    error_message = nullptr;
    result = lancedb_vector_query_nprobes(query, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 10, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count > 0);

    // Count total rows
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == 10);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Test refine_factor parameter") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set refine_factor to 10 (will fetch 10x more results and refine)
    error_message = nullptr;
    result = lancedb_vector_query_refine_factor(query, 10, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 5, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count > 0);

    // Count total rows
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == 5);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Test combined parameters - nprobes and refine_factor") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set nprobes to 3 (smaller then 4 partitions)
    error_message = nullptr;
    result = lancedb_vector_query_nprobes(query, 3, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set refine_factor to 5
    error_message = nullptr;
    result = lancedb_vector_query_refine_factor(query, 5, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set distance type
    error_message = nullptr;
    result = lancedb_vector_query_distance_type(query, LANCEDB_DISTANCE_L2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 8, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count > 0);

    // Count total rows
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == 8);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - HNSW parameters", "[vector_query]") {
  const std::string table_name = "vector_query_hnsw_test";
  constexpr size_t total_rows = 256;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  // Create IVF_HNSW_SQ vector index on "data" column for testing ef parameter
  const char* vector_columns[] = {"data"};
  LanceDBVectorIndexConfig config = {
    .num_partitions = 4,
    .num_sub_vectors = -1,
    .max_iterations = -1,
    .sample_rate = 0.0f,
    .distance_type = LANCEDB_DISTANCE_L2,
    .accelerator = nullptr,
    .replace = 0
  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_HNSW_PQ, &config, &error_message);

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  SECTION("Test ef parameter with HNSW index") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set ef parameter (exploration factor for HNSW)
    error_message = nullptr;
    result = lancedb_vector_query_ef(query, 100, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 10, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count > 0);

    // Count total rows
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == 10);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Test combined HNSW parameters - ef and nprobes") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set ef parameter
    error_message = nullptr;
    result = lancedb_vector_query_ef(query, 50, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set nprobes (IVF_HNSW_PQ has IVF component too)
    error_message = nullptr;
    result = lancedb_vector_query_nprobes(query, 2, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    constexpr size_t limit = 50;
    verify_vector_row_count(limit, query);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - min_nprobes and max_nprobes parameters", "[vector_query]") {
  const std::string table_name = "vector_query_minmax_nprobes_test";
  constexpr size_t total_rows = 256;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  // Create vector index on "data" column for testing min/max nprobes
  const char* vector_columns[] = {"data"};
  LanceDBVectorIndexConfig config = {
    .num_partitions = 8,  // Use 8 partitions to allow meaningful min/max range
    .num_sub_vectors = -1,
    .max_iterations = -1,
    .sample_rate = 0.0f,
    .distance_type = LANCEDB_DISTANCE_L2,
    .accelerator = nullptr,
    .replace = 0
  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_PQ, &config, &error_message);
  REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

  SECTION("Test min_nprobes parameter") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(error_message == nullptr);
    REQUIRE(query != nullptr);

    // Set min_nprobes to 2
    error_message = nullptr;
    result = lancedb_vector_query_nprobes_range(query, 2, 0, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    constexpr size_t limit = 10;
    verify_vector_row_count(limit, query);
  }

  SECTION("Test max_nprobes parameter") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set max_nprobes to 6
    error_message = nullptr;
    result = lancedb_vector_query_nprobes_range(query, 0, 6, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    constexpr size_t limit = 10;
    verify_vector_row_count(limit, query);
  }

  SECTION("Test combined min_nprobes and max_nprobes parameters") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    error_message = nullptr;
    result = lancedb_vector_query_nprobes_range(query, 3, 7, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    constexpr size_t limit = 15;
    verify_vector_row_count(limit, query);
  }

  SECTION("Test combined min_nprobes, max_nprobes, and refine_factor parameters") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    error_message = nullptr;
    result = lancedb_vector_query_nprobes_range(query, 2, 6, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    error_message = nullptr;
    result = lancedb_vector_query_refine_factor(query, 5, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    constexpr size_t limit = 8;
    verify_vector_row_count(limit, query);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - min_nprobes and max_nprobes error/edge cases", "[vector_query]") {
  const std::string table_name = "vector_query_nprobes_error_test";
  constexpr size_t total_rows = 256;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  // Create vector index on "data" column with 8 partitions
  const char* vector_columns[] = {"data"};
  LanceDBVectorIndexConfig config = {
    .num_partitions = 8,
    .num_sub_vectors = -1,
    .max_iterations = -1,
    .sample_rate = 0.0f,
    .distance_type = LANCEDB_DISTANCE_L2,
    .accelerator = nullptr,
    .replace = 0
  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_PQ, &config, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);

  SECTION("Test max_nprobes < min_nprobes") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    error_message = nullptr;
    result = lancedb_vector_query_nprobes_range(query, 5, 3, &error_message);
    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Test valid boundary case - min_nprobes equals max_nprobes") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    error_message = nullptr;
    result = lancedb_vector_query_nprobes_range(query, 4, 4, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    constexpr size_t limit = 10;
    verify_vector_row_count(limit, query);
  }

  SECTION("Test min/max_nprobes values against num_partitions (index has 8 partitions)") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set min_nprobes to 12 and max_nprobes to 20 (index has 8 partitions)
    error_message = nullptr;
    result = lancedb_vector_query_nprobes_range(query, 12, 20,  &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    constexpr size_t limit = 5;
    verify_vector_row_count(limit, query);
  }

  SECTION("Test nprobes values against num_partitions (index has 8 partitions)") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set nprobes to 20 (index has 8 partitions)
    error_message = nullptr;
    result = lancedb_vector_query_nprobes(query, 20,  &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    constexpr size_t limit = 5;
    verify_vector_row_count(limit, query);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - distance_range parameter", "[vector_query]") {
  const std::string table_name = "vector_query_distance_range_test";
  constexpr size_t total_rows = 100;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  // Create IVF_FLAT vector index on "data" column
  const char* vector_columns[] = {"data"};
  LanceDBVectorIndexConfig config = {
    .num_partitions = 4,
    .num_sub_vectors = -1,
    .max_iterations = -1,
    .sample_rate = 0.0f,
    .distance_type = LANCEDB_DISTANCE_L2,
    .accelerator = nullptr,
    .replace = 0
  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message);
  REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

  // Run baseline query without distance range to determine actual min/max distances
  std::vector<float> baseline_query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);
  LanceDBVectorQuery* baseline_query = lancedb_vector_query_new(
      table,
      baseline_query_vector.data(),
      TEST_SCHEMA_DIMENSIONS
  );
  REQUIRE(baseline_query != nullptr);

  error_message = nullptr;
  result = lancedb_vector_query_limit(baseline_query, total_rows, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);

  LanceDBQueryResult* baseline_result = lancedb_vector_query_execute(baseline_query, nullptr);
  REQUIRE(baseline_result != nullptr);

  FFI_ArrowArray** baseline_arrays = nullptr;
  FFI_ArrowSchema* baseline_schema = nullptr;
  size_t batch_count = 0;
  error_message = nullptr;
  result = lancedb_query_result_to_arrow(
      baseline_result, &baseline_arrays, &baseline_schema, &batch_count, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(batch_count > 0);

  // Extract min and max distances from baseline results
  float min_distance = std::numeric_limits<float>::max();
  float max_distance = std::numeric_limits<float>::min();

  for (size_t batch_idx = 0; batch_idx < batch_count; batch_idx++) {
    ArrowArray* batch_array = reinterpret_cast<ArrowArray*>(baseline_arrays[batch_idx]);
    ArrowSchema* schema = reinterpret_cast<ArrowSchema*>(baseline_schema);

    // Find the _distance column (typically the last column in vector query results)
    int distance_col_idx = -1;
    for (int64_t i = 0; i < schema->n_children; i++) {
      if (std::string(schema->children[i]->name) == "_distance") {
        distance_col_idx = i;
        break;
      }
    }
    REQUIRE(distance_col_idx >= 0);

    ArrowArray* distance_array = batch_array->children[distance_col_idx];
    const float* distance_data = static_cast<const float*>(distance_array->buffers[1]);

    // find min/max within thebatch
    for (int64_t i = 0; i < distance_array->length; i++) {
      float dist = distance_data[i];
      min_distance = std::min(min_distance, dist);
      max_distance = std::max(max_distance, dist);
    }
  }

  INFO("Baseline query found distances in range [" << min_distance << ", " << max_distance << "]");

  lancedb_free_arrow_arrays(baseline_arrays, batch_count);
  lancedb_free_arrow_schema(baseline_schema);

  SECTION("Test distance_range with both lower and upper bounds") {
    std::vector<float> query_vector = baseline_query_vector;  // Use same query vector

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set distance range to min/max distance values
    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_distance_range(query, min_distance, max_distance, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    // Set limit high to see all results in range
    error_message = nullptr;
    constexpr size_t limit = 50;
    result = lancedb_vector_query_limit(query, limit, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count > 0);

    // Verify that the returned distances fall within the range [min_distance, max_distance)
    size_t total_rows = 0;
    for (size_t batch_idx = 0; batch_idx < count; batch_idx++) {
      ArrowArray* batch_array = reinterpret_cast<ArrowArray*>(result_arrays[batch_idx]);
      ArrowSchema* schema = reinterpret_cast<ArrowSchema*>(result_schema);

      // Find the _distance column
      int distance_col_idx = -1;
      for (int64_t i = 0; i < schema->n_children; i++) {
        if (std::string(schema->children[i]->name) == "_distance") {
          distance_col_idx = i;
          break;
        }
      }
      REQUIRE(distance_col_idx >= 0);

      ArrowArray* distance_array = batch_array->children[distance_col_idx];
      const float* distance_data = static_cast<const float*>(distance_array->buffers[1]);

      for (int64_t i = 0; i < distance_array->length; i++) {
        float dist = distance_data[i];
        REQUIRE(dist >= min_distance);
        REQUIRE(dist < max_distance);  // Upper bound is exclusive
        total_rows++;
      }
    }

    // With range [min, max) we should get all results up to the limit
    REQUIRE(total_rows == limit);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Test distance_range with only lower bound") {
    std::vector<float> query_vector = baseline_query_vector;  // Use same query vector

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set only lower bound (filter out closest results)
    const float lower_bound = min_distance + (max_distance - min_distance) * 0.3f;
    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_distance_range(query, lower_bound, -1.0f, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count > 0);

    // Verify that the returned distances are above the lower bound
    size_t total_rows = 0;
    for (size_t batch_idx = 0; batch_idx < count; batch_idx++) {
      ArrowArray* batch_array = reinterpret_cast<ArrowArray*>(result_arrays[batch_idx]);
      ArrowSchema* schema = reinterpret_cast<ArrowSchema*>(result_schema);

      // Find the _distance column
      int distance_col_idx = -1;
      for (int64_t i = 0; i < schema->n_children; i++) {
        if (std::string(schema->children[i]->name) == "_distance") {
          distance_col_idx = i;
          break;
        }
      }
      REQUIRE(distance_col_idx >= 0);

      ArrowArray* distance_array = batch_array->children[distance_col_idx];
      const float* distance_data = static_cast<const float*>(distance_array->buffers[1]);

      for (int64_t i = 0; i < distance_array->length; i++) {
        float dist = distance_data[i];
        REQUIRE(dist >= lower_bound);
        total_rows++;
      }
    }

    INFO("Got " << total_rows << " results with distance >= " << lower_bound);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Test distance_range with only upper bound") {
    std::vector<float> query_vector = baseline_query_vector;  // Use same query vector

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set only upper bound (filter out furthest results)
    const float upper_bound = min_distance + (max_distance - min_distance) * 0.7f;
    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_distance_range(query, -1.0f, upper_bound, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count > 0);

    // Verify that the returned distances are below the upper bound
    size_t total_rows = 0;
    for (size_t batch_idx = 0; batch_idx < count; batch_idx++) {
      ArrowArray* batch_array = reinterpret_cast<ArrowArray*>(result_arrays[batch_idx]);
      ArrowSchema* schema = reinterpret_cast<ArrowSchema*>(result_schema);

      // Find the _distance column
      int distance_col_idx = -1;
      for (int64_t i = 0; i < schema->n_children; i++) {
        if (std::string(schema->children[i]->name) == "_distance") {
          distance_col_idx = i;
          break;
        }
      }
      REQUIRE(distance_col_idx >= 0);

      ArrowArray* distance_array = batch_array->children[distance_col_idx];
      const float* distance_data = static_cast<const float*>(distance_array->buffers[1]);

      for (int64_t i = 0; i < distance_array->length; i++) {
        float dist = distance_data[i];
        REQUIRE(dist < upper_bound);  // Upper bound is exclusive
        total_rows++;
      }
    }

    INFO("Got " << total_rows << " results with distance < " << upper_bound);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Test distance_range with lower and upper bound") {
    std::vector<float> query_vector = baseline_query_vector;  // Use same query vector

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set lower and upper bounds (middle range)
    const float lower_bound = min_distance + (max_distance - min_distance) * 0.3f;
    const float upper_bound = min_distance + (max_distance - min_distance) * 0.7f;
    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_distance_range(query, lower_bound, upper_bound, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count > 0);

    // Verify that the returned distances fall within [lower_bound, upper_bound)
    size_t total_rows = 0;
    for (size_t batch_idx = 0; batch_idx < count; batch_idx++) {
      ArrowArray* batch_array = reinterpret_cast<ArrowArray*>(result_arrays[batch_idx]);
      ArrowSchema* schema = reinterpret_cast<ArrowSchema*>(result_schema);

      // Find the _distance column
      int distance_col_idx = -1;
      for (int64_t i = 0; i < schema->n_children; i++) {
        if (std::string(schema->children[i]->name) == "_distance") {
          distance_col_idx = i;
          break;
        }
      }
      REQUIRE(distance_col_idx >= 0);

      ArrowArray* distance_array = batch_array->children[distance_col_idx];
      const float* distance_data = static_cast<const float*>(distance_array->buffers[1]);

      for (int64_t i = 0; i < distance_array->length; i++) {
        float dist = distance_data[i];
        REQUIRE(dist >= lower_bound);
        REQUIRE(dist < upper_bound);  // Upper bound is exclusive
        total_rows++;
      }
    }

    INFO("Got " << total_rows << " results with distance in [" << lower_bound << ", " << upper_bound << ")");

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Test distance_range with neither bound set") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set neither bound (both -1.0) - should return all results
    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_distance_range(query, -1.0f, -1.0f, &error_message);
    REQUIRE((error_message == nullptr && result == LANCEDB_SUCCESS));

    // Set limit
    error_message = nullptr;
    constexpr size_t limit = 50;
    result = lancedb_vector_query_limit(query, limit, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    // Convert to Arrow and verify results
    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    error_message = nullptr;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count > 0);

    // Count total rows
    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == limit);

    // Clean up
    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - distance_range error cases", "[vector_query]") {
  const std::string table_name = "vector_query_distance_range_error_test";
  constexpr size_t total_rows = 100;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  SECTION("Test distance_range with lower_bound > upper_bound") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set invalid range: lower > upper (10.0 > 5.0)
    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_distance_range(query, 10.0f, 5.0f, &error_message);
    REQUIRE(error_message != nullptr);
    REQUIRE(result != LANCEDB_SUCCESS);
  }

  SECTION("Test distance_range with lower_bound == upper_bound") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS
    );
    REQUIRE(query != nullptr);

    // Set empty range (upper is exclusive)
    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_distance_range(query, 10.0f, 10.0f, &error_message);
    REQUIRE(error_message != nullptr);
    REQUIRE(result != LANCEDB_SUCCESS);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - error cases", "[vector_query]") {
  const std::string table_name = "vector_query_error_test";

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, 50, 0);
  REQUIRE(table != nullptr);

  SECTION("Query with wrong vector dimension") {
    // Create random query vector with wrong dimension
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS + 1);  // Wrong size!

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_nearest_to(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS + 1,  // Wrong dimension
        5,
        "data",
        &result_arrays,
        &result_schema,
        &count,
        &error_message);

    // Should fail
    REQUIRE(result != LANCEDB_SUCCESS);

    if (error_message) {
      INFO("Expected error: " << error_message);
      lancedb_free_string(error_message);
    }
  }

  SECTION("Query with non-existent column") {
    // Create random query vector
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_nearest_to(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS,
        5,
        "nonexistent_column",  // Wrong column name
        &result_arrays,
        &result_schema,
        &count,
        &error_message);

    // Should fail
    REQUIRE(result != LANCEDB_SUCCESS);

    if (error_message) {
      INFO("Expected error: " << error_message);
      lancedb_free_string(error_message);
    }
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - Filter on non-existent column", "[vector_query]") {
  const std::string table_name = "vector_query_filter_nonexistent_column_test";

  // Create table with data (columns: "key" and "data")
  LanceDBTable* table = create_table_with_data(table_name, 10, 0);
  REQUIRE(table != nullptr);

  // Create vector query
  std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);
  LanceDBVectorQuery* query = lancedb_vector_query_new(
      table,
      query_vector.data(),
      TEST_SCHEMA_DIMENSIONS
  );
  REQUIRE(query != nullptr);

  // Set limit
  char* error_message = nullptr;
  LanceDBError result = lancedb_vector_query_limit(query, 5, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  // Filter on a column that does not exist in the table
  error_message = nullptr;
  result = lancedb_vector_query_where_filter(query, "key = \"key_42\" OR unknown = \"value\"", &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  // error should be caught at execution time
  LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
  REQUIRE(query_result == nullptr);

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - DataFusion Expr Filter", "[vector_query][expr]") {
  const std::string table_name = "vector_query_df_filter_test";
  constexpr size_t total_rows = 100;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  SECTION("Vector query with Expr filter by single key") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    // Build expr: key = "key_42"
    LanceDBExpr* eq_expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_42"));
    REQUIRE(eq_expr != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_df_filter(query, eq_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_vector_query_limit(query, 10, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count > 0);

    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == 1);

    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Vector query with Expr IN list filter") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    // Build expr: key IN ("key_10", "key_20", "key_30")
    LanceDBExpr* col_expr = lancedb_expr_column("key");
    LanceDBExpr* list_items[3];
    list_items[0] = lancedb_expr_literal_string("key_10");
    list_items[1] = lancedb_expr_literal_string("key_20");
    list_items[2] = lancedb_expr_literal_string("key_30");

    char* error_message = nullptr;
    LanceDBExpr* in_expr = lancedb_expr_in_list(col_expr, list_items, 3, false, &error_message);
    REQUIRE(in_expr != nullptr);

    LanceDBError result = lancedb_vector_query_df_filter(query, in_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    result = lancedb_vector_query_limit(query, 10, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count > 0);

    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == 3);

    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Vector query with Expr OR filter") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    // Build expr: key = "key_10" OR key = "key_20"
    LanceDBExpr* or_expr = lancedb_expr_or(
        lancedb_expr_binary(
            lancedb_expr_column("key"),
            LANCEDB_BINARY_OP_EQ,
            lancedb_expr_literal_string("key_10")),
        lancedb_expr_binary(
            lancedb_expr_column("key"),
            LANCEDB_BINARY_OP_EQ,
            lancedb_expr_literal_string("key_20")));
    REQUIRE(or_expr != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_df_filter(query, or_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    result = lancedb_vector_query_limit(query, 10, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count > 0);

    size_t sum_rows = 0;
    for (size_t i = 0; i < count; i++) {
      sum_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }
    REQUIRE(sum_rows == 2);

    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Vector query Expr filter with unknown key (should return empty result)") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    // Build expr: key = "key_999" (doesn't exist)
    LanceDBExpr* eq_expr = lancedb_expr_binary(
        lancedb_expr_column("key"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("key_999"));
    REQUIRE(eq_expr != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_df_filter(query, eq_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    result = lancedb_vector_query_limit(query, 10, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    result = lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count == 0);
    REQUIRE(result_arrays == nullptr);
    REQUIRE(result_schema == nullptr);
  }

  SECTION("Vector query Expr filter on non-existent column (should fail at execution)") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    // Build expr: nonexistent_column = "value"
    LanceDBExpr* eq_expr = lancedb_expr_binary(
        lancedb_expr_column("nonexistent_column"),
        LANCEDB_BINARY_OP_EQ,
        lancedb_expr_literal_string("value"));
    REQUIRE(eq_expr != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_vector_query_df_filter(query, eq_expr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    result = lancedb_vector_query_limit(query, 10, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    // Error should be caught at execution time
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
    REQUIRE(query_result == nullptr);
  }

  SECTION("Vector query df_filter null arguments") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;

    // Null expr
    LanceDBError result = lancedb_vector_query_df_filter(query, nullptr, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Null query
    LanceDBExpr* expr = lancedb_expr_column("key");
    result = lancedb_vector_query_df_filter(nullptr, expr, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_expr_free(expr);

    lancedb_vector_query_free(query);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBSessionFixture, "LanceDB Vector Query - repeated queries populate session cache stats", "[vector_query][session]") {
  LanceDBSessionCacheStats initial_index_stats{};
  LanceDBSessionCacheStats final_index_stats{};
  char* error_message = nullptr;

  LanceDBError result = lancedb_session_index_cache_stats(session, &initial_index_stats, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  const std::string table_name = "vector_query_session_cache_stats_test";
  constexpr size_t total_rows = 256;
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  const char* vector_columns[] = {"data"};
  LanceDBVectorIndexConfig config = {
    .num_partitions = 4,
    .num_sub_vectors = -1,
    .max_iterations = -1,
    .sample_rate = 0.0f,
    .distance_type = LANCEDB_DISTANCE_L2,
    .accelerator = nullptr,
    .replace = 0
  };

  result = lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  constexpr size_t repeat_count = 20;
  constexpr size_t limit = 10;
  for (size_t i = 0; i < repeat_count; i++) {
    uint32_t query_seed = static_cast<uint32_t>(i);
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS, query_seed);
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    result = lancedb_vector_query_limit(query, limit, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, nullptr);
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query Execution Options - Basic operations", "[vector_query]") {
  // Create a table with test data
  LanceDBTable* table = create_table_with_data("options_test_basic", 100, 0);
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

  SECTION("Test setting both max_batch_length and timeout") {
    LanceDBQueryExecutionOptions* options = lancedb_query_execution_options_new();
    REQUIRE(options != nullptr);

    char* error_message = nullptr;

    LanceDBError result = lancedb_query_execution_options_set_max_batch_length(
        options,
        500,
        &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    result = lancedb_query_execution_options_set_timeout(
        options,
        60,
        &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    lancedb_query_execution_options_free(options);
  }

  SECTION("Test using options with vector query") {
    SUCCEED("This is currently failing, See: https://github.com/lancedb/lance/issues/3220");
    return;
    LanceDBQueryExecutionOptions* options = lancedb_query_execution_options_new();
    REQUIRE(options != nullptr);

    char* error_message = nullptr;

    constexpr unsigned int batch_size = 10;
    LanceDBError result = lancedb_query_execution_options_set_max_batch_length(
        options,
        batch_size,
        &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    constexpr unsigned int query_limit = 500;
    LanceDBError limit_result = lancedb_vector_query_limit(query, query_limit, &error_message);
    REQUIRE(limit_result == LANCEDB_SUCCESS);

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, options);
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

  SECTION("Test setting zero timeout") {
    LanceDBQueryExecutionOptions* options = lancedb_query_execution_options_new();
    REQUIRE(options != nullptr);

    char* error_message = nullptr;
    LanceDBError result = lancedb_query_execution_options_set_timeout(
        options,
        0,  // Zero timeout
        &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table,
        query_vector.data(),
        TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    constexpr unsigned int query_limit = 10;
    LanceDBError limit_result = lancedb_vector_query_limit(query, query_limit, &error_message);
    REQUIRE(limit_result == LANCEDB_SUCCESS);

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query, options);
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
      total_rows += reinterpret_cast<ArrowArray*>(result_arrays[i])->length;
    }

    INFO("Got " << total_rows << " rows with zero timeout");
    REQUIRE(total_rows == query_limit);

    lancedb_free_arrow_arrays(result_arrays, count);
    if (result_schema != nullptr) {
      lancedb_free_arrow_schema(result_schema);
    }
    lancedb_query_execution_options_free(options);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Query Execution Options - Error cases", "[vector_query]") {
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

