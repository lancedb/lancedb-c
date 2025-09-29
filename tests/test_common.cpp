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
  auto key_field = arrow::field("key", arrow::utf8());
  auto data_field = arrow::field("data", arrow::fixed_size_list(arrow::float32(), 8));
  auto schema = arrow::schema({key_field, data_field});

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

