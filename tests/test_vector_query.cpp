/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"
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
      LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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
    char* error_message = nullptr;
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
    char* error_message = nullptr;
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - configuration parameters", "[vector_query]") {
  const std::string table_name = "vector_query_config_test";
  constexpr size_t total_rows = 256;

  // Create table with data
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  // Create IVF_FLAT vector index on "data" column for testing nprobes
  const char* vector_columns[] = {"data"};
  LanceDBVectorIndexConfig config = {
    .num_partitions = 4,
    .num_sub_vectors = -1,
    .max_iterations = -1,
    .sample_rate = 0.0f,
    .distance_type = LANCEDB_DISTANCE_L2,
    .replace = 0
  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message);

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

    // Set L2 distance type
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
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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

    // Set nprobes
    error_message = nullptr;
    result = lancedb_vector_query_nprobes(query, 3, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set refine_factor
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
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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
    .replace = 0
  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_HNSW_SQ, &config, &error_message);

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
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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

    // Set nprobes (IVF_HNSW_SQ has IVF component too)
    error_message = nullptr;
    result = lancedb_vector_query_nprobes(query, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 5, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Execute query
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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
  LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - column selection and invalid arguments", "[vector_query]") {
  const std::string table_name = "vector_query_column_test";
  LanceDBTable* table = create_table_with_data(table_name, 20, 0);
  REQUIRE(table != nullptr);

  const std::vector<float> vector(TEST_SCHEMA_DIMENSIONS, 1.0F);

  SECTION("Query the vector column by name") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, vector.data(), vector.size());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 5, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
    REQUIRE(query_result != nullptr);

    FFI_ArrowArray** result_arrays = nullptr;
    FFI_ArrowSchema* result_schema = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_query_result_to_arrow(
        query_result, &result_arrays, &result_schema, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count > 0);

    lancedb_free_arrow_arrays(result_arrays, count);
    lancedb_free_arrow_schema(result_schema);
  }

  SECTION("Query an unknown vector column should fail at execution") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, vector.data(), vector.size());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "no_such_column", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(lancedb_vector_query_execute(query) == nullptr);
  }

  SECTION("Vector query setters with null query should fail") {
    const char* columns[] = {"key"};

    REQUIRE(lancedb_vector_query_column(nullptr, "data", nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_limit(nullptr, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_offset(nullptr, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select(nullptr, columns, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_where_filter(nullptr, "key = 'key_0'", nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_nprobes(nullptr, 10, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_refine_factor(nullptr, 2, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_ef(nullptr, 10, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_distance_type(nullptr, LANCEDB_DISTANCE_L2, nullptr) == LANCEDB_INVALID_ARGUMENT);

  }

  SECTION("Vector query with every distance type") {
    const LanceDBDistanceType types[] = {
      LANCEDB_DISTANCE_L2,
      LANCEDB_DISTANCE_COSINE,
      LANCEDB_DISTANCE_DOT,
      LANCEDB_DISTANCE_HAMMING
    };

    for (const auto type : types) {
      LanceDBVectorQuery* query = lancedb_vector_query_new(
          table, vector.data(), vector.size());
      REQUIRE(query != nullptr);

      char* error_message = nullptr;
      REQUIRE(lancedb_vector_query_distance_type(query, type, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);

      lancedb_vector_query_free(query);
    }
  }

  SECTION("Vector query setters with strings that are not valid UTF-8 should fail") {
    // A string that is not valid UTF-8, to exercise the string conversion failures
    const char* const invalid_utf8 = "\xff\xfe";
    const char* invalid_columns[] = {invalid_utf8};
    const char* null_columns[] = {nullptr};

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, vector.data(), vector.size());
    REQUIRE(query != nullptr);

    REQUIRE(lancedb_vector_query_column(query, invalid_utf8, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select(query, null_columns, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select(query, invalid_columns, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_where_filter(query, invalid_utf8, nullptr) == LANCEDB_INVALID_ARGUMENT);

    lancedb_vector_query_free(query);
  }

  SECTION("Creating a vector query with invalid arguments returns no query") {
    REQUIRE(lancedb_vector_query_new(nullptr, vector.data(), vector.size()) == nullptr);
    REQUIRE(lancedb_vector_query_new(table, nullptr, vector.size()) == nullptr);
    REQUIRE(lancedb_vector_query_new(table, vector.data(), 0) == nullptr);
    REQUIRE(lancedb_vector_query_execute(nullptr) == nullptr);
  }

  SECTION("Vector query setters with null arguments should fail") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, vector.data(), vector.size());
    REQUIRE(query != nullptr);

    REQUIRE(lancedb_vector_query_column(query, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select(query, nullptr, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_where_filter(query, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);


    lancedb_vector_query_free(query);
  }

  lancedb_table_free(table);
}

// Tests for lancedb_vector_query_explain_plan (explain-plan FFI).
// explain_plan does NOT consume the query: it can still be executed afterwards.
TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - explain plan", "[vector_query][explain_plan]") {
  const std::string table_name = "vector_query_explain_plan_test";
  constexpr size_t total_rows = 100;
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS, 42);
  const char* columns[] = {"key", "data"};

  SECTION("Explain plan for a basic vector query, non-verbose and verbose") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_select(query, columns, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_limit(query, 5, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char* plan = nullptr;
    LanceDBError result = lancedb_vector_query_explain_plan(query, false, &plan, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);
    REQUIRE(std::string(plan).length() > 0);
    lancedb_free_string(plan);

    plan = nullptr;
    result = lancedb_vector_query_explain_plan(query, true, &plan, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);
    REQUIRE(std::string(plan).length() > 0);
    lancedb_free_string(plan);

    // explain_plan does not consume the query: it can still be executed
    LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
    REQUIRE(query_result != nullptr);
    lancedb_query_result_free(query_result);
  }

  SECTION("Explain plan with distance type and tuning options") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_distance_type(query, LANCEDB_DISTANCE_L2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_nprobes(query, 3, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_refine_factor(query, 5, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_ef(query, 50, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_limit(query, 8, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char* plan = nullptr;
    LanceDBError result = lancedb_vector_query_explain_plan(query, true, &plan, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);
    REQUIRE(std::string(plan).length() > 0);
    lancedb_free_string(plan);

    lancedb_vector_query_free(query);
  }

  SECTION("Explain plan reflects a DataFusion filter") {
    LanceDBExpr* col_expr = lancedb_expr_column("key");
    LanceDBExpr* val_expr = lancedb_expr_literal_string("key_42");
    LanceDBExpr* eq_expr = lancedb_expr_binary(col_expr, LANCEDB_BINARY_OP_EQ, val_expr);
    REQUIRE(eq_expr != nullptr);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_select(query, columns, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_df_filter(query, eq_expr, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_limit(query, 5, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char* plan = nullptr;
    LanceDBError result = lancedb_vector_query_explain_plan(query, true, &plan, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);
    REQUIRE(std::string(plan).length() > 0);
    lancedb_free_string(plan);

    lancedb_vector_query_free(query);
  }

  SECTION("Explain plan rejects null arguments") {
    char* plan = nullptr;
    REQUIRE(lancedb_vector_query_explain_plan(nullptr, true, &plan, nullptr) == LANCEDB_INVALID_ARGUMENT);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);
    REQUIRE(lancedb_vector_query_explain_plan(query, true, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    lancedb_vector_query_free(query);
  }

  lancedb_table_free(table);
}
