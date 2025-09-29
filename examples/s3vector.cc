#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include "s3vector.h"
#include "lancedb.h"


int result_to_errno(LanceDBError result) {
  switch (result) {
    // TODO
    default:
      return -1;
  }
}

// connect to a db
LanceDBConnection* connect(const std::string& uri) {
  LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
  if (!builder) {
    std::cerr << "failed to create connection builder" << std::endl;
    return nullptr;
  }
  LanceDBConnection* db = lancedb_connect_builder_execute(builder);
  if (!db) {
    lancedb_connect_builder_free(builder);
    std::cerr << "failed to connect to database" << std::endl;
    return nullptr;
  }
  return db;
}

int CreateVectorBucket(const std::string& vectorBucketName, std::string& vectorBucketArn) {
  std::cout << "=== CreateVectorBucket ===" << std::endl;
  // TODO convert name to URI
  if (auto db = connect(vectorBucketName); db) {
    vectorBucketArn = vectorBucketName; // TODO: create ARN
    lancedb_connection_free(db);
    return 0;
  } else {
    std::cerr << "failed to connect to database" << std::endl;
    return -EIO ;
  }
}

int CreateIndex(const CreateIndexInput& input, std::string& indexArn) {
  std::cout << "=== CreateIndex ===" << std::endl;
  auto db = connect(input.vectorBucketName);
  if (!db) {
    std::cerr << "failed to connect to database" << std::endl;
    return -EIO ;
  }
  // use arrow to define schema: [key, vector, metadata...]
  const auto dimension = input.dimension;
  auto schema = arrow::schema({
      arrow::field("key", arrow::utf8()),
      arrow::field("data", arrow::fixed_size_list(arrow::float32(), dimension))});
  for (const auto& metadata_name : input.metadataConfiguration) {
    std::ignore = schema->AddField(schema->num_fields(),
        arrow::field(metadata_name, arrow::utf8()));
  }
  // convert arrow C++ schema to arrow C ABI
  struct ArrowSchema c_schema;
  if (const auto status = arrow::ExportSchema(*schema, &c_schema); !status.ok()) {
    std::cerr << "failed to export schema to C ABI: " << status.ToString() << std::endl;
    lancedb_connection_free(db);
    return -EINVAL;
  }
  // create an empty table based on the schema
  const std::string table_name = input.indexName;
  LanceDBTable* table = nullptr;
  if (const LanceDBError result = lancedb_table_create(db, table_name.c_str(),
        reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
        nullptr, &table, nullptr); result != LANCEDB_SUCCESS) {
    std::cerr << "error creating table: " << table_name << ", error: " << lancedb_error_to_message(result) << std::endl;
    lancedb_connection_free(db);
    return result_to_errno(result);
  }
  // define a scalar index on the "key" column
  const char* key_columns[] = {"key"};
  LanceDBScalarIndexConfig scalar_config = {
    .replace = 1,                    // replace existing index
    .force_update_statistics = 0     // don't force update statistics
  };
  if (const LanceDBError result = lancedb_table_create_scalar_index(
        table, key_columns, 1, LANCEDB_INDEX_BTREE, &scalar_config, nullptr); result != LANCEDB_SUCCESS) {
    std::cerr << "failed to create scalar index on 'key' column, error: '" <<
      lancedb_error_to_message(result) << "'" << std::endl;
      return result_to_errno(result);
  }
  std::cout << "created index: " << table_name << " for table: " << input.vectorBucketName  << std::endl;
  lancedb_table_free(table);
  lancedb_connection_free(db);
  indexArn = input.vectorBucketName + "/" + input.indexName; // TODO: create ARN
  return 0;
}

int DeleteVectorBucket(const DeleteVectorBucketInput& input) {
  std::cout << "=== DeleteVectorBucket ===" << std::endl;
  auto db = connect(input.vectorBucketName);
  if (!db) {
    std::cerr << "failed to connect to database" << std::endl;
    return -EIO ;
  }
  if (const LanceDBError result = lancedb_connection_drop_all_tables(db, nullptr); result != LANCEDB_SUCCESS) {
    std::cerr << "error deleting table, error: " << lancedb_error_to_message(result) << std::endl;
    return result_to_errno(result);
  }

  lancedb_connection_free(db);
  return 0;
}

int DeleteIndex(const DeleteIndexInput& input) {
  std::cout << "=== DeleteIndex ===" << std::endl;
  return 0;
}

int DeleteVectorBucketPolicy(const DeleteVectorBucketPolicyInput& input) {
  std::cout << "=== DeleteVectorBucketPolicy ===" << std::endl;
  return 0;
}

int DeleteVectors(const DeleteVectorsInput& input) {
  std::cout << "=== DeleteVectors ===" << std::endl;
  return 0;
}
int GetVectorBucket(const GetVectorBucketInput& input, GetVectorBucketOutput& output) {
  std::cout << "=== GetVectorBucket ===" << std::endl;
  return 0;
}

int GetIndex(const GetIndexInput& input, GetIndexOutput& output) {
  std::cout << "=== GetIndex ===" << std::endl;
  return 0;
}

int GetVectors(const GetVectorsInput& input, std::vector<VectorResult>& vectors) {
  std::cout << "=== GetVectors ===" << std::endl;
  return 0;
}

int ListVectorBuckets(const ListVectorBucketsInput& input, ListVectorBucketsOutput& output) {
  std::cout << "=== ListVectorBuckets ===" << std::endl;
  return 0;
}
int ListIndexes(const ListIndexesInput& input, ListIndexesOutput& output) {
  std::cout << "=== ListIndexes ===" << std::endl;
  return 0;
}

int ListVectors(const ListVectorsInput& input, ListVectorsOutput& output) {
  std::cout << "=== ListVectors ===" << std::endl;
  return 0;
}

int PutVectorBucketPolicy(const PutVectorBucketPolicyInput& input) {
  std::cout << "=== PutVectorBucketPolicy ===" << std::endl;
  return 0;
}

int PutVectors(const PutVectorsInput& input) {
  std::cout << "=== PutVectors ===" << std::endl;
  if (input.vectors.empty()) {
    std::cerr << "no vectors to put" << std::endl;
    return -EINVAL;
  }
  auto db = connect(input.vectorBucketName);
  if (!db) {
    std::cerr << "failed to connect to database" << std::endl;
    return -EIO ;
  }
  const auto dimension = input.vectors[0].float32.size();
  arrow::StringBuilder key_builder;
  arrow::FloatBuilder float_builder;
  arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
      std::make_unique<arrow::FloatBuilder>(),
      dimensions);
  arrow::StringBuilder tag1_builder, tag2_builder, tag3_builder;
  // add sample rows
  const int num_rows = 100;
  for (int i = 0; i < num_rows; i++) {
    // key column
    key_builder.Append("key_" + std::to_string(i)).ok();
    // vector data column
    auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    for (size_t j = 0; j < dimensions; j++) {
      list_builder->Append(static_cast<float>(rand()%100)).ok();
    }
    data_builder.Append().ok();
    // tag columns
    tag1_builder.Append("category_" + std::to_string(i % 3)).ok();
    tag2_builder.Append("type_" + std::to_string(i % 2)).ok();
    tag3_builder.Append("label_" + std::to_string(i % 5)).ok();
  }
  // build arrays
  std::shared_ptr<arrow::Array> key_array, data_array, tag1_array, tag2_array, tag3_array;
  key_builder.Finish(&key_array).ok();
  data_builder.Finish(&data_array).ok();
  tag1_builder.Finish(&tag1_array).ok();
  tag2_builder.Finish(&tag2_array).ok();
  tag3_builder.Finish(&tag3_array).ok();
  return 0;
}

int QueryVectors(const QueryVectorsInput& input, std::vector<VectorResult>& vectors) {
  std::cout << "=== QueryVectors ===" << std::endl;
  return 0;
}

