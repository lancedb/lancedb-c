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

    if (error_message) {
      INFO("Error: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
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

    if (error_message) {
      INFO("Error: " << error_message);
      lancedb_free_string(error_message);
    }

    // Should succeed because the API finds the "data" vector column
    REQUIRE(result == LANCEDB_SUCCESS);
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
      if (error_message) {
        INFO("Error setting limit: " << error_message);
        lancedb_free_string(error_message);
      }
      REQUIRE(result == LANCEDB_SUCCESS);

      // Set offset
      error_message = nullptr;
      result = lancedb_vector_query_offset(query, offset, &error_message);
      if (error_message) {
        INFO("Error setting offset: " << error_message);
        lancedb_free_string(error_message);
      }
      REQUIRE(result == LANCEDB_SUCCESS);

      // Select "key" and "data" columns
      const char* columns[] = {"key", "data"};
      error_message = nullptr;
      result = lancedb_vector_query_select(query, columns, 2, &error_message);
      if (error_message) {
        INFO("Error setting select: " << error_message);
        lancedb_free_string(error_message);
      }
      REQUIRE(result == LANCEDB_SUCCESS);

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

      if (error_message) {
        INFO("Error converting to Arrow: " << error_message);
        lancedb_free_string(error_message);
      }
      REQUIRE(result == LANCEDB_SUCCESS);
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

  if (error_message) {
    INFO("Error creating index: " << error_message);
    lancedb_free_string(error_message);
  }
  REQUIRE(result == LANCEDB_SUCCESS);

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

    if (error_message) {
      INFO("Error: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
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

    if (error_message) {
      INFO("Error: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);
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
    .accelerator = nullptr,
    .replace = 0
  };

  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message);

  if (error_message) {
    INFO("Error creating index: " << error_message);
    lancedb_free_string(error_message);
  }
  REQUIRE(result == LANCEDB_SUCCESS);

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
    if (error_message) {
      INFO("Error setting distance type: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 5, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

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
    if (error_message) {
      INFO("Error setting distance type: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 5, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

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
    if (error_message) {
      INFO("Error setting nprobes: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 10, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

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
    if (error_message) {
      INFO("Error setting refine_factor: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 5, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

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

    // Set refine_factor
    error_message = nullptr;
    result = lancedb_vector_query_refine_factor(query, 5, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    // Set distance type
    error_message = nullptr;
    result = lancedb_vector_query_distance_type(query, LANCEDB_DISTANCE_L2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 8, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

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
      table, vector_columns, 1, LANCEDB_INDEX_IVF_HNSW_SQ, &config, &error_message);

  if (error_message) {
    INFO("Error creating HNSW index: " << error_message);
    lancedb_free_string(error_message);
  }
  REQUIRE(result == LANCEDB_SUCCESS);

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
    if (error_message) {
      INFO("Error setting ef: " << error_message);
      lancedb_free_string(error_message);
    }
    REQUIRE(result == LANCEDB_SUCCESS);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 10, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

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

    // Set nprobes (IVF_HNSW_SQ has IVF component too)
    error_message = nullptr;
    result = lancedb_vector_query_nprobes(query, 2, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

    // Set limit
    error_message = nullptr;
    result = lancedb_vector_query_limit(query, 5, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);

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

