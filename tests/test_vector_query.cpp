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

struct CollectedQueryRows {
  std::shared_ptr<arrow::Schema> schema;
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;

  size_t num_rows() const {
    size_t rows = 0;
    for (const auto& batch : batches) {
      rows += static_cast<size_t>(batch->num_rows());
    }
    return rows;
  }
};

static CollectedQueryRows collect_query_rows(LanceDBQueryResult* query_result) {
  REQUIRE(query_result != nullptr);

  FFI_ArrowArray** result_arrays = nullptr;
  FFI_ArrowSchema* result_schema = nullptr;
  size_t count = 0;
  char* error_message = nullptr;
  REQUIRE(lancedb_query_result_to_arrow(
      query_result, &result_arrays, &result_schema, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  CollectedQueryRows collected;
  if (count == 0) {
    REQUIRE(result_arrays == nullptr);
    REQUIRE(result_schema == nullptr);
    return collected;
  }

  auto imported_schema = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(result_schema));
  REQUIRE(imported_schema.ok());
  collected.schema = imported_schema.ValueOrDie();

  for (size_t i = 0; i < count; i++) {
    auto imported_batch = arrow::ImportRecordBatch(
        reinterpret_cast<ArrowArray*>(result_arrays[i]), collected.schema);
    REQUIRE(imported_batch.ok());
    collected.batches.push_back(imported_batch.ValueOrDie());
  }

  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);
  return collected;
}

static std::vector<std::string> keys_of(const CollectedQueryRows& rows) {
  std::vector<std::string> keys;
  for (const auto& batch : rows.batches) {
    auto key_column = batch->GetColumnByName("key");
    REQUIRE(key_column != nullptr);
    auto key_array = std::static_pointer_cast<arrow::StringArray>(key_column);
    for (int64_t i = 0; i < key_array->length(); i++) {
      keys.push_back(key_array->GetString(i));
    }
  }
  return keys;
}

static std::vector<uint64_t> row_ids_of(const CollectedQueryRows& rows) {
  std::vector<uint64_t> row_ids;
  for (const auto& batch : rows.batches) {
    auto row_id_column = batch->GetColumnByName("_rowid");
    REQUIRE(row_id_column != nullptr);
    auto row_id_array = std::static_pointer_cast<arrow::UInt64Array>(row_id_column);
    for (int64_t i = 0; i < row_id_array->length(); i++) {
      REQUIRE(!row_id_array->IsNull(i));
      row_ids.push_back(row_id_array->Value(i));
    }
  }
  return row_ids;
}

static std::vector<float> vector_for_row(int row) {
  std::vector<float> vector(TEST_SCHEMA_DIMENSIONS);
  for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
    vector[j] = static_cast<float>(row * 10 + static_cast<int>(j));
  }
  return vector;
}

// Count the rows across all batches in a query result and free the result.
static size_t count_rows_and_free(LanceDBQueryResult* query_result) {
  REQUIRE(query_result != nullptr);

  FFI_ArrowArray** arrays = nullptr;
  FFI_ArrowSchema* schema = nullptr;
  size_t batch_count = 0;
  char* error_message = nullptr;
  REQUIRE(lancedb_query_result_to_arrow(
      query_result, &arrays, &schema, &batch_count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  size_t row_count = 0;
  for (size_t i = 0; i < batch_count; i++) {
    row_count += reinterpret_cast<ArrowArray*>(arrays[i])->length;
  }

  lancedb_free_arrow_arrays(arrays, batch_count);
  lancedb_free_arrow_schema(schema);
  return row_count;
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
  result = lancedb_vector_query_where_filter(query, "key = 'key_42' OR unknown = 'value'", &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  // error should be caught at execution time
  LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
  REQUIRE(query_result == nullptr);

  lancedb_table_free(table);
}

// Post-filtering matches the Rust SDK: the filter runs on the vector search
// results instead of restricting the search, so it can drop rows and return
// fewer than the requested limit.
TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - post-filter after vector search", "[vector_query]") {
  const std::string table_name = "vector_query_postfilter_test";
  constexpr size_t total_rows = 100;
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  // With this fixture's ascending vectors, key_0 .. key_4 are the five nearest
  // rows to an all-zero query vector and key_99 is the farthest.
  std::vector<float> query_vector(TEST_SCHEMA_DIMENSIONS, 0.0f);

  SECTION("Default pre-filtering finds a match regardless of distance") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_where_filter(query, "key = 'key_99'", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_limit(query, 5, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(count_rows_and_free(lancedb_vector_query_execute(query)) == 1);
  }

  SECTION("Post-filtering applies the filter to the nearest rows") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_where_filter(query, "key = 'key_99'", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_postfilter(query, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_limit(query, 5, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(count_rows_and_free(lancedb_vector_query_execute(query)) == 0);
  }

  SECTION("Explain plan reflects post-filtering") {
    auto make_filtered_query = [&](bool postfilter) {
      LanceDBVectorQuery* query = lancedb_vector_query_new(
          table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
      REQUIRE(query != nullptr);

      char* error_message = nullptr;
      REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
      LanceDBExpr* expr = lancedb_expr_binary(
          lancedb_expr_column("key"),
          LANCEDB_BINARY_OP_EQ,
          lancedb_expr_literal_string("key_99"));
      REQUIRE(expr != nullptr);
      REQUIRE(lancedb_vector_query_df_filter(query, expr, &error_message) == LANCEDB_SUCCESS);
      if (postfilter) {
        REQUIRE(lancedb_vector_query_postfilter(query, &error_message) == LANCEDB_SUCCESS);
      }
      REQUIRE(lancedb_vector_query_limit(query, 5, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
      return query;
    };

    LanceDBVectorQuery* prefilter_query = make_filtered_query(false);
    char* prefilter_plan = nullptr;
    REQUIRE(lancedb_vector_query_explain_plan(
        prefilter_query, true, &prefilter_plan, nullptr) == LANCEDB_SUCCESS);
    REQUIRE(prefilter_plan != nullptr);
    lancedb_vector_query_free(prefilter_query);

    LanceDBVectorQuery* postfilter_query = make_filtered_query(true);
    char* postfilter_plan = nullptr;
    REQUIRE(lancedb_vector_query_explain_plan(
        postfilter_query, true, &postfilter_plan, nullptr) == LANCEDB_SUCCESS);
    REQUIRE(postfilter_plan != nullptr);
    lancedb_vector_query_free(postfilter_query);

    // Post-filtering moves the filter after the vector search, so the two
    // plans must not be identical.
    REQUIRE(std::string(prefilter_plan) != std::string(postfilter_plan));

    lancedb_free_string(prefilter_plan);
    lancedb_free_string(postfilter_plan);
  }

  SECTION("Explain plan keeps a SQL filter with post-filtering") {
    auto make_sql_filtered_query = [&](bool postfilter) {
      LanceDBVectorQuery* query = lancedb_vector_query_new(
          table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
      REQUIRE(query != nullptr);

      char* error_message = nullptr;
      REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
      REQUIRE(lancedb_vector_query_where_filter(query, "key = 'key_99'", &error_message) == LANCEDB_SUCCESS);
      if (postfilter) {
        REQUIRE(lancedb_vector_query_postfilter(query, &error_message) == LANCEDB_SUCCESS);
      }
      REQUIRE(lancedb_vector_query_limit(query, 5, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
      return query;
    };

    LanceDBVectorQuery* prefilter_query = make_sql_filtered_query(false);
    char* prefilter_plan = nullptr;
    REQUIRE(lancedb_vector_query_explain_plan(
        prefilter_query, true, &prefilter_plan, nullptr) == LANCEDB_SUCCESS);
    REQUIRE(prefilter_plan != nullptr);
    lancedb_vector_query_free(prefilter_query);

    LanceDBVectorQuery* postfilter_query = make_sql_filtered_query(true);
    char* postfilter_plan = nullptr;
    REQUIRE(lancedb_vector_query_explain_plan(
        postfilter_query, true, &postfilter_plan, nullptr) == LANCEDB_SUCCESS);
    REQUIRE(postfilter_plan != nullptr);
    lancedb_vector_query_free(postfilter_query);

    // The SQL predicate must reach the plan in both modes, and post-filtering
    // must still move the filter after the vector search.
    REQUIRE(std::string(prefilter_plan).find("key_99") != std::string::npos);
    REQUIRE(std::string(postfilter_plan).find("key_99") != std::string::npos);
    REQUIRE(std::string(prefilter_plan) != std::string(postfilter_plan));

    lancedb_free_string(prefilter_plan);
    lancedb_free_string(postfilter_plan);
  }

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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - row id, fast search and bypass flags", "[vector_query]") {
  const std::string table_name = "vector_query_flags_test";
  constexpr int indexed_rows = 128;
  constexpr int appended_rows = 128;

  LanceDBTable* table = create_table_with_data(table_name, indexed_rows, 0);
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
  char* error_message = nullptr;
  REQUIRE(lancedb_table_create_vector_index(
      table, vector_columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  auto appended = create_test_record_batch(appended_rows, indexed_rows);
  REQUIRE(lancedb_table_add(table, create_reader_from_batch(appended), nullptr) == LANCEDB_SUCCESS);

  const auto query_vector = vector_for_row(indexed_rows + 72);

  auto make_query = [&](bool fast_search, bool bypass_vector_index) {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);

    const char* columns[] = {"key"};
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_with_row_id(query, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    if (fast_search) {
      REQUIRE(lancedb_vector_query_fast_search(query, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }
    if (bypass_vector_index) {
      REQUIRE(lancedb_vector_query_bypass_vector_index(query, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }
    return query;
  };

  auto run_query = [&](bool fast_search, bool bypass_vector_index) {
    return collect_query_rows(
        lancedb_vector_query_execute(make_query(fast_search, bypass_vector_index)));
  };

  auto plan_of = [&](bool fast_search, bool bypass_vector_index) {
    LanceDBVectorQuery* query = make_query(fast_search, bypass_vector_index);
    char* plan = nullptr;
    char* plan_error = nullptr;
    const auto status =
        lancedb_vector_query_explain_plan(query, false, &plan, &plan_error);
    lancedb_vector_query_free(query);

    std::string plan_text;
    if (plan != nullptr) {
      plan_text = plan;
      lancedb_free_string(plan);
    }
    if (plan_error != nullptr) {
      lancedb_free_string(plan_error);
    }
    REQUIRE(status == LANCEDB_SUCCESS);
    REQUIRE(plan_error == nullptr);
    REQUIRE(!plan_text.empty());
    return plan_text;
  };

  SECTION("with_row_id returns the actual metadata column beside the nearest row") {
    auto rows = run_query(false, false);
    REQUIRE(rows.num_rows() == 1);
    REQUIRE(keys_of(rows) == std::vector<std::string>{"key_200"});
    REQUIRE(rows.schema->GetFieldByName("_rowid") != nullptr);
    REQUIRE(row_ids_of(rows).size() == 1);
  }

  SECTION("fast_search returns only rows covered by the vector index") {
    auto rows = run_query(true, false);
    REQUIRE(rows.num_rows() == 1);
    REQUIRE(keys_of(rows) == std::vector<std::string>{"key_127"});
    REQUIRE(row_ids_of(rows).size() == 1);
  }

  SECTION("bypass_vector_index removes the ANN node and returns the flat row") {
    const auto indexed_plan = plan_of(false, false);
    const auto bypassed_plan = plan_of(false, true);
    REQUIRE(indexed_plan.find("ANNIvfPartition") != std::string::npos);
    REQUIRE(indexed_plan.find("ANNSubIndex") != std::string::npos);
    REQUIRE(bypassed_plan.find("ANNIvfPartition") == std::string::npos);
    REQUIRE(bypassed_plan.find("ANNSubIndex") == std::string::npos);
    REQUIRE(bypassed_plan.find("KNNVectorDistance") != std::string::npos);

    auto indexed_rows_result = run_query(false, false);
    auto bypassed = run_query(false, true);
    REQUIRE(bypassed.num_rows() == 1);
    REQUIRE(keys_of(bypassed) == std::vector<std::string>{"key_200"});
    REQUIRE(row_ids_of(bypassed) == row_ids_of(indexed_rows_result));
  }

  SECTION("fast_search takes precedence over bypass_vector_index") {
    auto rows = run_query(true, true);
    REQUIRE(rows.num_rows() == 1);
    REQUIRE(keys_of(rows) == std::vector<std::string>{"key_127"});
  }

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
    const char* expressions[] = {"key"};

    REQUIRE(lancedb_vector_query_column(nullptr, "data", nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_limit(nullptr, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_offset(nullptr, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select(nullptr, columns, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select_all(nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select_dynamic(nullptr, columns, expressions, 1, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_where_filter(nullptr, "key = 'key_0'", nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_nprobes(nullptr, 10, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_refine_factor(nullptr, 2, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_ef(nullptr, 10, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_distance_type(nullptr, LANCEDB_DISTANCE_L2, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_with_row_id(nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_fast_search(nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_bypass_vector_index(nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_rerank_rrf(nullptr, 12.0F, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_norm(nullptr, LANCEDB_NORMALIZE_RANK, nullptr) == LANCEDB_INVALID_ARGUMENT);
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

  SECTION("Invalid normalization method values are rejected") {
    const int invalid_normalize_methods[] = {-1, 2};

    for (const int normalize_method : invalid_normalize_methods) {
      LanceDBVectorQuery* query = lancedb_vector_query_new(
          table, vector.data(), vector.size());
      REQUIRE(query != nullptr);

      char* error_message = nullptr;
      REQUIRE(lancedb_test_set_normalize_method(
          query, normalize_method, &error_message) == LANCEDB_INVALID_ARGUMENT);
      REQUIRE(error_message != nullptr);
      lancedb_free_string(error_message);

      // A rejected setter leaves the query usable with a valid method.
      REQUIRE(lancedb_vector_query_norm(
          query, LANCEDB_NORMALIZE_RANK, nullptr) == LANCEDB_SUCCESS);

      lancedb_vector_query_free(query);
    }
  }

  SECTION("Invalid distance type values are rejected") {
    const int invalid_distance_types[] = {-1, 4};

    for (const int distance_type : invalid_distance_types) {
      LanceDBVectorQuery* query = lancedb_vector_query_new(
          table, vector.data(), vector.size());
      REQUIRE(query != nullptr);

      char* error_message = nullptr;
      REQUIRE(lancedb_test_set_distance_type(
          query, distance_type, &error_message) == LANCEDB_INVALID_ARGUMENT);
      REQUIRE(error_message != nullptr);
      lancedb_free_string(error_message);

      // A rejected setter leaves the query usable with a valid distance.
      REQUIRE(lancedb_vector_query_distance_type(
          query, LANCEDB_DISTANCE_L2, nullptr) == LANCEDB_SUCCESS);

      lancedb_vector_query_free(query);
    }
  }

  SECTION("Vector query setters with strings that are not valid UTF-8 should fail") {
    // A string that is not valid UTF-8, to exercise the string conversion failures
    const char* const invalid_utf8 = "\xff\xfe";
    const char* invalid_columns[] = {invalid_utf8};
    const char* null_columns[] = {nullptr};
    const char* columns[] = {"key"};
    const char* expressions[] = {"key"};
    const char* invalid_expressions[] = {invalid_utf8};

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, vector.data(), vector.size());
    REQUIRE(query != nullptr);

    REQUIRE(lancedb_vector_query_column(query, invalid_utf8, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select(query, null_columns, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select(query, invalid_columns, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select_dynamic(query, null_columns, expressions, 1, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select_dynamic(query, invalid_columns, expressions, 1, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select_dynamic(query, columns, invalid_expressions, 1, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
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

    const char* expressions[] = {"key"};
    REQUIRE(lancedb_vector_query_column(query, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select(query, nullptr, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select_dynamic(query, nullptr, expressions, 1, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_select_dynamic(query, expressions, nullptr, 1, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_where_filter(query, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_postfilter(nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);


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

  SECTION("nprobes zero is rejected at execute and explain") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_nprobes(query, 0, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char* plan = nullptr;
    REQUIRE(lancedb_vector_query_explain_plan(query, false, &plan, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(plan == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("nprobes must be greater than 0") != std::string::npos);
    lancedb_free_string(error_message);

    // execute consumes the query and reports the same failure by returning NULL
    REQUIRE(lancedb_vector_query_execute(query) == nullptr);
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

  SECTION("Explain plan reflects a SQL filter") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_select(query, columns, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_where_filter(query, "key = 'key_42'", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_limit(query, 5, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char* plan = nullptr;
    LanceDBError result = lancedb_vector_query_explain_plan(query, true, &plan, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);
    // The filter literal only appears in the plan when the SQL predicate is applied.
    REQUIRE(std::string(plan).find("key_42") != std::string::npos);
    lancedb_free_string(plan);

    lancedb_vector_query_free(query);
  }

  SECTION("Explain plan rejects a SQL filter on a missing column") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_where_filter(query, "unknown = 'value'", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char* plan = nullptr;
    LanceDBError result = lancedb_vector_query_explain_plan(query, true, &plan, &error_message);
    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(plan == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - dynamic projection and inspection", "[vector_query]") {
  const std::string table_name = "vector_query_dynamic_projection_test";
  constexpr int total_rows = 10;
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  const std::vector<float> query_vector = vector_for_row(0);
  const char* names[] = {"key_length", "key_copy", "is_key_three"};
  const char* expressions[] = {
      "cast(length(key) as bigint)", "key", "key = 'key_3'"};

  auto query_with_dynamic_projection = [&] {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_select_dynamic(
        query, names, expressions, 3, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    return query;
  };

  auto output_schema_of = [&](LanceDBVectorQuery* query) {
    FFI_ArrowSchema* schema = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_output_schema(query, &schema, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(schema != nullptr);

    auto imported = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(schema));
    REQUIRE(imported.ok());
    lancedb_free_arrow_schema(schema);
    return imported.ValueOrDie();
  };

  auto analyze_plan_of = [&](LanceDBVectorQuery* query) {
    char* plan = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_analyze_plan(query, &plan, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);

    std::string result(plan);
    lancedb_free_string(plan);
    return result;
  };

  SECTION("Dynamic projection order and types drive the schema and rows") {
    LanceDBVectorQuery* query = query_with_dynamic_projection();
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_where_filter(query, "key = 'key_3'", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto schema = output_schema_of(query);
    REQUIRE(schema->num_fields() >= 3);
    REQUIRE(schema->field(0)->name() == "key_length");
    REQUIRE(schema->field(0)->type()->id() == arrow::Type::INT64);
    REQUIRE(schema->field(1)->name() == "key_copy");
    REQUIRE(schema->field(1)->type()->id() == arrow::Type::STRING);
    REQUIRE(schema->field(2)->name() == "is_key_three");
    REQUIRE(schema->field(2)->type()->id() == arrow::Type::BOOL);

    auto rows = collect_query_rows(lancedb_vector_query_execute(query));
    REQUIRE(rows.schema->Equals(*schema));
    REQUIRE(rows.num_rows() == 1);

    auto batch = rows.batches.front();
    auto key_length =
        std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("key_length"));
    REQUIRE(key_length != nullptr);
    REQUIRE(key_length->Value(0) == 5);

    auto key_copy =
        std::static_pointer_cast<arrow::StringArray>(batch->GetColumnByName("key_copy"));
    REQUIRE(key_copy != nullptr);
    REQUIRE(key_copy->GetString(0) == "key_3");

    auto is_key_three = std::static_pointer_cast<arrow::BooleanArray>(
        batch->GetColumnByName("is_key_three"));
    REQUIRE(is_key_three != nullptr);
    REQUIRE(is_key_three->Value(0));
  }

  SECTION("Select all resets a static or dynamic projection") {
    LanceDBVectorQuery* query = query_with_dynamic_projection();
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_select_all(query, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto schema = output_schema_of(query);
    REQUIRE(schema->num_fields() == 3);
    REQUIRE(schema->field(0)->name() == "key");
    REQUIRE(schema->field(1)->name() == "data");
    REQUIRE(schema->field(2)->name() == "_distance");
    REQUIRE(schema->field(2)->type()->id() == arrow::Type::FLOAT);
    REQUIRE(schema->GetFieldByName("key_length") == nullptr);

    const char* key_column[] = {"key"};
    REQUIRE(lancedb_vector_query_select(query, key_column, 1, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    schema = output_schema_of(query);
    REQUIRE(schema->num_fields() == 2);
    REQUIRE(schema->field(0)->name() == "key");
    REQUIRE(schema->field(1)->name() == "_distance");
    REQUIRE(schema->field(1)->type()->id() == arrow::Type::FLOAT);
    REQUIRE(schema->GetFieldByName("data") == nullptr);

    REQUIRE(lancedb_vector_query_select_all(query, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    schema = output_schema_of(query);
    REQUIRE(schema->num_fields() == 3);
    REQUIRE(schema->field(0)->name() == "key");
    REQUIRE(schema->field(1)->name() == "data");
    REQUIRE(schema->field(2)->name() == "_distance");

    lancedb_vector_query_free(query);
  }

  SECTION("Analyze plan reflects the configured query and executes it") {
    LanceDBVectorQuery* query = query_with_dynamic_projection();
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_where_filter(query, "key = 'key_7'", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto plan = analyze_plan_of(query);
    REQUIRE(plan.length() > 0);
    // key_7 only appears in the predicate; the projection uses key_3.
    REQUIRE(plan.find("key_7") != std::string::npos);
    REQUIRE(plan.find("metrics=[output_rows=") != std::string::npos);

    // analyze_plan does not consume the query.
    auto rows = collect_query_rows(lancedb_vector_query_execute(query));
    REQUIRE(rows.num_rows() == 1);
    auto batch = rows.batches.front();
    auto key_copy =
        std::static_pointer_cast<arrow::StringArray>(batch->GetColumnByName("key_copy"));
    REQUIRE(key_copy != nullptr);
    REQUIRE(key_copy->GetString(0) == "key_7");
    auto is_key_three = std::static_pointer_cast<arrow::BooleanArray>(
        batch->GetColumnByName("is_key_three"));
    REQUIRE(is_key_three != nullptr);
    REQUIRE(!is_key_three->Value(0));
  }

  SECTION("Inspection preserves SDK errors and clears outputs on failure") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);

    const char* bad_names[] = {"bad"};
    const char* bad_expressions[] = {"missing_column + 1"};
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_select_dynamic(
        query, bad_names, bad_expressions, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    FFI_ArrowSchema* schema = reinterpret_cast<FFI_ArrowSchema*>(uintptr_t{1});
    REQUIRE(lancedb_vector_query_output_schema(query, &schema, &error_message)
        != LANCEDB_SUCCESS);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    char* plan = reinterpret_cast<char*>(uintptr_t{1});
    error_message = nullptr;
    REQUIRE(lancedb_vector_query_analyze_plan(query, &plan, &error_message)
        != LANCEDB_SUCCESS);
    REQUIRE(plan == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_vector_query_free(query);
  }

  SECTION("Inspection rejects null arguments and clears outputs") {
    FFI_ArrowSchema* schema = reinterpret_cast<FFI_ArrowSchema*>(uintptr_t{1});
    char* plan = reinterpret_cast<char*>(uintptr_t{1});
    char* error_message = nullptr;

    REQUIRE(lancedb_vector_query_output_schema(nullptr, &schema, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(schema == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    error_message = nullptr;
    REQUIRE(lancedb_vector_query_analyze_plan(nullptr, &plan, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(plan == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);
    REQUIRE(lancedb_vector_query_output_schema(query, nullptr, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_vector_query_analyze_plan(query, nullptr, nullptr)
        == LANCEDB_INVALID_ARGUMENT);
    lancedb_vector_query_free(query);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Query - Arrow IPC output", "[vector_query][ipc]") {
  const std::string table_name = "vector_query_ipc_test";
  constexpr size_t total_rows = 50;
  LanceDBTable* table = create_table_with_data(table_name, total_rows, 0);
  REQUIRE(table != nullptr);

  SECTION("A started vector query exports its projected batches") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 5, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    const char* columns[] = {"key"};
    REQUIRE(lancedb_vector_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // A missing result pointer is rejected and the builder is still consumed
    error_message = nullptr;
    REQUIRE(lancedb_vector_query_execute_with_status(query, nullptr, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    query = lancedb_vector_query_new(table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_limit(query, 5, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);

    LanceDBQueryResult* result = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_vector_query_execute_with_status(query, &result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(result != nullptr);

    LanceDBIpcBuffer buffer = {};
    REQUIRE(lancedb_query_result_to_ipc_file(result, &buffer, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto contents = read_ipc_file(buffer.data, buffer.len);
    // The projection keeps the requested column and the SDK keeps the distance
    // column it added for a vector query
    REQUIRE(contents.schema->num_fields() == 2);
    REQUIRE(contents.schema->field(0)->name() == "key");
    REQUIRE(contents.schema->field(1)->name() == "_distance");
    REQUIRE(contents.batches.size() >= 1);
    REQUIRE(read_ipc_file_strings(contents, 0).size() == 5);

    lancedb_ipc_buffer_free(&buffer);
  }

  SECTION("A vector query that cannot start reports the SDK error") {
    std::vector<float> query_vector = generate_random_query_vector(TEST_SCHEMA_DIMENSIONS);
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    // The column is only resolved once the query runs
    REQUIRE(lancedb_vector_query_column(query, "no_such_column", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // A null builder is rejected without a query
    LanceDBQueryResult* result = reinterpret_cast<LanceDBQueryResult*>(1);
    error_message = nullptr;
    REQUIRE(lancedb_vector_query_execute_with_status(nullptr, &result, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(result == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    result = reinterpret_cast<LanceDBQueryResult*>(1);
    error_message = nullptr;
    const LanceDBError status =
        lancedb_vector_query_execute_with_status(query, &result, &error_message);
    // The SDK reports the unresolved column, not a NULL pointer and a lost error
    REQUIRE(status != LANCEDB_SUCCESS);
    REQUIRE(status != LANCEDB_UNKNOWN);
    REQUIRE(result == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("no_such_column") != std::string::npos);
    lancedb_free_string(error_message);

    // The legacy entry point keeps its null-pointer return convention
    query = lancedb_vector_query_new(table, query_vector.data(), TEST_SCHEMA_DIMENSIONS);
    REQUIRE(query != nullptr);
    REQUIRE(lancedb_vector_query_column(query, "no_such_column", nullptr) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_vector_query_execute(query) == nullptr);
  }

  lancedb_table_free(table);
}
