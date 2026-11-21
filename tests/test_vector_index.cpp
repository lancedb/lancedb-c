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

  SECTION("Create IVF_FLAT index on empty table") {
    // Create empty table
    create_empty_table(table_name);
    LanceDBTable* table = open_table(table_name);

    // IVF_FLAT index creation is allowed before any data is added.
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

    // The newly created index must be listed on the still-empty table.
    char** indices = nullptr;
    size_t count = 0;
    result = lancedb_table_list_indices(table, &indices, &count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }

  SECTION("Create IVF_FLAT index on a missing column fails") {
    LanceDBTable* table = create_table_with_data(table_name, 16, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"missing"};
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

    REQUIRE(result != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
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
  }

  lancedb_table_free(table);
}

// Build a record batch with controlled vectors. The fixture keeps the L2 and
// cosine winners far apart so the ordering does not depend on float rounding.
static std::shared_ptr<arrow::RecordBatch> create_vector_record_batch(
    const std::vector<std::vector<float>>& vectors) {
  auto schema = create_test_schema();
  arrow::StringBuilder key_builder;
  arrow::FixedSizeListBuilder data_builder(
      arrow::default_memory_pool(), std::make_unique<arrow::FloatBuilder>(),
      TEST_SCHEMA_DIMENSIONS);

  for (size_t i = 0; i < vectors.size(); ++i) {
    REQUIRE(vectors[i].size() == TEST_SCHEMA_DIMENSIONS);
    REQUIRE(key_builder.Append("row_" + std::to_string(i)).ok());

    auto* values = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    for (const float value : vectors[i]) {
      REQUIRE(values->Append(value).ok());
    }
    REQUIRE(data_builder.Append().ok());
  }

  std::shared_ptr<arrow::Array> keys, data;
  REQUIRE(key_builder.Finish(&keys).ok());
  REQUIRE(data_builder.Finish(&data).ok());

  return arrow::RecordBatch::Make(schema, static_cast<int64_t>(vectors.size()),
                                  {keys, data});
}

// Run a nearest-neighbour query without setting a metric, so the SDK uses the
// metric of the vector index it chooses.
static std::string first_key_for_vector_query(
    LanceDBTable* table, const std::vector<float>& query_vector) {
  LanceDBVectorQuery* query = lancedb_vector_query_new(
      table, query_vector.data(), query_vector.size());
  REQUIRE(query != nullptr);

  char* error_message = nullptr;
  REQUIRE(lancedb_vector_query_limit(query, 1, &error_message) == LANCEDB_SUCCESS);
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

  auto imported_schema = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(result_schema));
  REQUIRE(imported_schema.ok());
  auto imported_batch = arrow::ImportRecordBatch(
      reinterpret_cast<ArrowArray*>(result_arrays[0]), imported_schema.ValueUnsafe());
  REQUIRE(imported_batch.ok());

  auto batch = imported_batch.ValueUnsafe();
  REQUIRE(batch->num_rows() == 1);
  auto key_array = std::static_pointer_cast<arrow::StringArray>(batch->column(0));
  std::string key = key_array->GetString(0);

  lancedb_free_arrow_arrays(result_arrays, count);
  lancedb_free_arrow_schema(result_schema);
  return key;
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Index - create index options", "[vector_index]") {
  const std::string table_name = "vector_index_create_options_test";
  const char* columns[] = {"data"};

  SECTION("Cosine index metric changes the query result") {
    create_empty_table(table_name);
    LanceDBTable* table = open_table(table_name);
    REQUIRE(table != nullptr);

    auto batch = create_vector_record_batch({
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {-5.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, -3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    });
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBIndexOptions options = lancedb_index_options_default();
    options.name = "data_cosine";
    options.replace = 0;
    options.num_partitions = 1;
    options.sample_rate = 4;
    options.max_iterations = 2;
    options.distance_type = LANCEDB_DISTANCE_COSINE;

    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // L2 prefers row_0 (distance 4); cosine prefers row_1 (0.894 vs 0.447).
    // Querying without a metric makes the SDK use the index metric, so dropping
    // the creation option returns row_0 and fails this assertion.
    const std::vector<float> query_vector =
        {1.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    REQUIRE(first_key_for_vector_query(table, query_vector) == "row_1");

    lancedb_table_free(table);
  }

  SECTION("L2 index metric changes the query result") {
    create_empty_table(table_name);
    LanceDBTable* table = open_table(table_name);
    REQUIRE(table != nullptr);

    auto batch = create_vector_record_batch({
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {-5.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, -3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    });
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBIndexOptions options = lancedb_index_options_default();
    options.name = "data_l2";
    options.replace = 0;
    options.num_partitions = 1;
    options.sample_rate = 4;
    options.max_iterations = 2;
    options.distance_type = LANCEDB_DISTANCE_L2;

    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    const std::vector<float> query_vector =
        {1.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    REQUIRE(first_key_for_vector_query(table, query_vector) == "row_0");

    lancedb_table_free(table);
  }

  SECTION("IVF_PQ tuning options are forwarded") {
    LanceDBTable* table = create_table_with_data(table_name, 300, 0);
    REQUIRE(table != nullptr);

    LanceDBIndexOptions options = lancedb_index_options_default();
    options.name = "data_ivf_pq";
    options.replace = 0;
    options.wait_timeout_ms = 1000;
    options.target_partition_size = 64;
    options.sample_rate = 64;
    options.max_iterations = 2;
    options.num_sub_vectors = 2;
    options.num_bits = 8;
    options.distance_type = LANCEDB_DISTANCE_L2;

    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_IVF_PQ, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(std::string(indices[0]) == "data_ivf_pq");
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }

  SECTION("IVF_HNSW_PQ tuning options are forwarded") {
    LanceDBTable* table = create_table_with_data(table_name, 300, 0);
    REQUIRE(table != nullptr);

    LanceDBIndexOptions options = lancedb_index_options_default();
    options.name = "data_hnsw_pq";
    options.replace = 0;
    options.wait_timeout_ms = 1000;
    options.num_partitions = 2;
    options.sample_rate = 64;
    options.max_iterations = 2;
    options.m = 8;
    options.ef_construction = 64;
    options.num_sub_vectors = 2;
    options.num_bits = 8;
    options.distance_type = LANCEDB_DISTANCE_L2;

    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_IVF_HNSW_PQ, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(std::string(indices[0]) == "data_hnsw_pq");
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }

  SECTION("IVF_HNSW_SQ tuning options are forwarded") {
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    LanceDBIndexOptions options = lancedb_index_options_default();
    options.name = "data_hnsw_sq";
    options.replace = 0;
    options.num_partitions = 1;
    options.sample_rate = 32;
    options.max_iterations = 2;
    options.m = 8;
    options.ef_construction = 64;
    options.distance_type = LANCEDB_DISTANCE_COSINE;

    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_index(
        table, columns, 1, LANCEDB_INDEX_IVF_HNSW_SQ, &options, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(std::string(indices[0]) == "data_hnsw_sq");
    lancedb_free_index_list(indices, count);

    lancedb_table_free(table);
  }
}
