/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

// Helper function to check if directory exists
int directory_exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Helper function to remove directory recursively
int remove_directory(const char* path) {
  char command[256];
  snprintf(command, sizeof(command), "rm -rf %s", path);
  return system(command);
}

void LanceDBFixture::create_empty_table(const std::string& table_name) {
  // Create schema
  auto schema = create_test_schema();

  // Convert to C ABI
  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  // Create empty table
  LanceDBTable* table = nullptr;
  char* error_message = nullptr;

  LanceDBError result = lancedb_table_create(
      db,
      table_name.c_str(),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      nullptr,
      &table,
      &error_message
      );

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(table != nullptr);

  if (error_message) {
    lancedb_free_string(error_message);
  }

  lancedb_table_free(table);

  // Clean up schema
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }
}

std::shared_ptr<arrow::Schema> create_test_schema() {
  auto key_field = arrow::field("key", arrow::utf8());
  auto data_field = arrow::field("data", arrow::fixed_size_list(arrow::float32(), TEST_SCHEMA_DIMENSIONS));
  return arrow::schema({key_field, data_field});
}

std::shared_ptr<arrow::RecordBatch> create_test_record_batch(int num_rows, int start_index) {
  auto schema = create_test_schema();

  // Create builders
  arrow::StringBuilder key_builder;
  arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
      std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

  // Add rows
  for (int i = 0; i < num_rows; i++) {
    int idx = start_index + i;
    key_builder.Append("key_" + std::to_string(idx)).ok();

    auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
      list_builder->Append(static_cast<float>(idx * 10 + j)).ok();
    }
    data_builder.Append().ok();
  }

  // Build arrays
  std::shared_ptr<arrow::Array> key_array, data_array;
  key_builder.Finish(&key_array).ok();
  data_builder.Finish(&data_array).ok();

  return arrow::RecordBatch::Make(schema, num_rows, {key_array, data_array});
}

LanceDBRecordBatchReader* create_reader_from_batch(const std::shared_ptr<arrow::RecordBatch>& batch) {
  struct ArrowArray c_array;
  struct ArrowSchema c_schema;

  REQUIRE(arrow::ExportRecordBatch(*batch, &c_array, &c_schema).ok());

  auto reader = lancedb_record_batch_reader_from_arrow(
      reinterpret_cast<FFI_ArrowArray*>(&c_array),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema));

  // Schema is only read by the function, so we must release it
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  return reader;
}

