/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <sys/stat.h>
#include <iostream>
#include <memory>
#include <vector>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include "lancedb.h"

constexpr size_t DIM = 128;

auto create_schema() {
  // use arrow to define schema: [id, item]
  auto id_field = arrow::field("id", arrow::int32());
  auto item_field = arrow::field("item", arrow::fixed_size_list(arrow::float32(), DIM));
  return arrow::schema({id_field, item_field});
}

LanceDBTable* create_empty_table(LanceDBConnection* db) {
  // convert arrow C++ schema to arrow C ABI
  auto schema = create_schema();
  struct ArrowSchema c_schema;
  if (const auto status = arrow::ExportSchema(*schema, &c_schema); !status.ok()) {
    std::cerr << "failed to export schema to C ABI: " << status.ToString() << std::endl;
    return nullptr;
  }

  // create an empty table based on the schema
  const std::string table_name = "empty_table";
  LanceDBTable* tbl;
  char* error_message = nullptr;
  if (const LanceDBError result = lancedb_table_create(db, table_name.c_str(),
        reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
        nullptr, &tbl, &error_message); result != LANCEDB_SUCCESS) {
    std::cerr << "error creating table: " << table_name << ", error: " << error_message << std::endl;
    lancedb_connection_free(db);
    lancedb_free_string(error_message);
    return nullptr;
  }
  std::cout << "created table: " << table_name << " (empty)" << std::endl;
  return tbl;
}


// the bucket must already exist on the S3 endpoint
// for example, create the S3 bucket using aws cli:
//
// export AWS_ACCESS_KEY_ID=apple
// export AWS_SECRET_ACCESS_KEY=banana
// export AWS_DEFAULT_REGION=orange
// aws s3 mb s3://my-bucket --endpoint-url http://localhost:9000
//
// then run the example:
// ./s3_example http://localhost:9000 orange apple banana my-bucket

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "Usage: " << argv[0] << " <s3_endpoint> <aws_region> <aws_access_key_id> <aws_secret_access_key> <bucket_name>" << std::endl;
    return 1;
  }
  const std::string s3_endpoint = argv[1];
  const std::string aws_region = argv[2];
  const std::string aws_access_key_id = argv[3];
  const std::string aws_secret_access_key = argv[4];
  const std::string bucket_name = argv[5];
  const std::string uri = "s3://" + bucket_name + "/sample-lancedb";
  // connect to a db
  LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
  if (!builder) {
    std::cerr << "failed to create connection builder" << std::endl;
    return 1;
  }
  builder = lancedb_connect_builder_storage_option(builder, "endpoint", s3_endpoint.c_str());
  builder = lancedb_connect_builder_storage_option(builder, "aws_region", aws_region.c_str());
  builder = lancedb_connect_builder_storage_option(builder, "aws_access_key_id", aws_access_key_id.c_str());
  builder = lancedb_connect_builder_storage_option(builder, "aws_secret_access_key", aws_secret_access_key.c_str());
  builder = lancedb_connect_builder_storage_option(builder, "allow_http", "true");
  builder = lancedb_connect_builder_storage_option(builder, "aws_s3_addressing_style", "path");

  LanceDBConnection* db = lancedb_connect_builder_execute(builder);
  if (!db) {
    std::cerr << "failed to connect to database" << std::endl;
    return 1;
  }

  auto empty_table = create_empty_table(db);
  if (!empty_table) {
    std::cerr << "failed to create empty table" << std::endl;
    return 1;
  }
  lancedb_table_free(empty_table);

  // list table names
  char** table_names;
  size_t name_count;
  char* error_message = nullptr;
  if (const LanceDBError result = lancedb_connection_table_names(db, &table_names, &name_count, &error_message); result != LANCEDB_SUCCESS) {
    std::cerr << "error listing table names, error: " << error_message << std::endl;
    lancedb_free_string(error_message);
  } else {
    std::cout << name_count << " tables found" << std::endl;
    for (size_t i = 0; i < name_count; i++) {
      std::cout << "table: " << table_names[i] << std::endl;
    }
    lancedb_free_table_names(table_names, name_count);
  }

  if (const LanceDBError result = lancedb_connection_drop_table(db, "empty_table", nullptr, &error_message); result != LANCEDB_SUCCESS) {
    std::cerr << "error dropping table, error: " << error_message << std::endl;
    lancedb_free_string(error_message);
  } else {
    std::cout << "dropped table empty_table" << std::endl;
  }
  lancedb_connection_free(db);

  return 0;
}

