/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 *
 * Tests for scalar index management: create, list_indices_detailed, drop
 */

#include "test_common.h"
#include <map>
#include <string>

// Helper: create a schema with key, data, and extra scalar columns
static std::shared_ptr<arrow::Schema> create_scalar_test_schema() {
  return arrow::schema({
    arrow::field("key", arrow::utf8()),
    arrow::field("data", arrow::fixed_size_list(arrow::float32(), TEST_SCHEMA_DIMENSIONS)),
    arrow::field("category", arrow::utf8()),
    arrow::field("price", arrow::float64()),
    arrow::field("color", arrow::utf8()),
  });
}

// Helper: create a record batch with scalar column data
static std::shared_ptr<arrow::RecordBatch> create_scalar_test_batch(int num_rows, int start_index) {
  auto schema = create_scalar_test_schema();

  arrow::StringBuilder key_builder;
  arrow::FixedSizeListBuilder data_builder(
      arrow::default_memory_pool(),
      std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);
  arrow::StringBuilder category_builder;
  arrow::DoubleBuilder price_builder;
  arrow::StringBuilder color_builder;

  const char* categories[] = {"electronics", "books", "toys"};
  const char* colors[] = {"red", "blue", "green"};

  for (int i = 0; i < num_rows; i++) {
    int idx = start_index + i;
    REQUIRE(key_builder.Append("key_" + std::to_string(idx)).ok());

    auto* float_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
      REQUIRE(float_builder->Append(static_cast<float>(idx * 10 + j)).ok());
    }
    REQUIRE(data_builder.Append().ok());

    REQUIRE(category_builder.Append(categories[idx % 3]).ok());
    REQUIRE(price_builder.Append(10.0 + idx * 5.5).ok());
    REQUIRE(color_builder.Append(colors[idx % 3]).ok());
  }

  std::shared_ptr<arrow::Array> key_arr, data_arr, cat_arr, price_arr, color_arr;
  REQUIRE(key_builder.Finish(&key_arr).ok());
  REQUIRE(data_builder.Finish(&data_arr).ok());
  REQUIRE(category_builder.Finish(&cat_arr).ok());
  REQUIRE(price_builder.Finish(&price_arr).ok());
  REQUIRE(color_builder.Finish(&color_arr).ok());

  return arrow::RecordBatch::Make(schema, num_rows,
      {key_arr, data_arr, cat_arr, price_arr, color_arr});
}

// Helper: create a table with scalar columns and data
static LanceDBTable* create_scalar_table(LanceDBConnection* db,
                                          const std::string& table_name,
                                          int num_rows) {
  auto schema = create_scalar_test_schema();
  auto batch = create_scalar_test_batch(num_rows, 0);
  auto reader = create_reader_from_batch(batch);
  REQUIRE(reader != nullptr);

  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create(
      db, table_name.c_str(),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      reader, &table, &error_message);

  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  return table;
}

// ============================================================================
// Tests for lancedb_table_list_indices_detailed
// ============================================================================

TEST_CASE_METHOD(LanceDBFixture, "List indices detailed - no indices", "[scalar_index]") {
  LanceDBTable* table = create_scalar_table(db, "detailed_empty", 50);

  LanceDBIndexInfo* indices = nullptr;
  size_t count = 0;
  char* error_message = nullptr;
  LanceDBError result = lancedb_table_list_indices_detailed(
      table, &indices, &count, &error_message);

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count == 0);
  REQUIRE(indices == nullptr);

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "List indices detailed - single BTREE index", "[scalar_index]") {
  LanceDBTable* table = create_scalar_table(db, "detailed_single", 50);

  // Create BTREE index on price
  const char* columns[] = {"price"};
  LanceDBScalarIndexConfig cfg = {.replace = 0, .force_update_statistics = 0};
  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create_scalar_index(
      table, columns, 1, LANCEDB_INDEX_BTREE, &cfg, &error_message);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  // List detailed
  LanceDBIndexInfo* indices = nullptr;
  size_t count = 0;
  result = lancedb_table_list_indices_detailed(
      table, &indices, &count, &error_message);

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count == 1);
  REQUIRE(indices != nullptr);

  // Verify fields
  REQUIRE(std::string(indices[0].name) == "price_idx");
  REQUIRE(indices[0].index_type == LANCEDB_INDEX_BTREE);
  REQUIRE(indices[0].num_columns == 1);
  REQUIRE(std::string(indices[0].columns[0]) == "price");

  lancedb_free_index_list_detailed(indices, count);
  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "List indices detailed - multiple scalar indexes with different types", "[scalar_index]") {
  LanceDBTable* table = create_scalar_table(db, "detailed_multi", 50);

  char* error_message = nullptr;
  LanceDBScalarIndexConfig cfg = {.replace = 0, .force_update_statistics = 0};

  // Create BITMAP on category
  const char* col1[] = {"category"};
  REQUIRE(lancedb_table_create_scalar_index(
      table, col1, 1, LANCEDB_INDEX_BITMAP, &cfg, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  // Create BTREE on price
  const char* col2[] = {"price"};
  REQUIRE(lancedb_table_create_scalar_index(
      table, col2, 1, LANCEDB_INDEX_BTREE, &cfg, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  // List detailed
  LanceDBIndexInfo* indices = nullptr;
  size_t count = 0;
  LanceDBError result = lancedb_table_list_indices_detailed(
      table, &indices, &count, &error_message);

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count == 2);
  REQUIRE(indices != nullptr);

  // Collect into a map for order-independent verification
  std::map<std::string, LanceDBIndexInfo*> by_column;
  for (size_t i = 0; i < count; i++) {
    REQUIRE(indices[i].num_columns == 1);
    by_column[std::string(indices[i].columns[0])] = &indices[i];
  }

  REQUIRE(by_column.count("category") == 1);
  REQUIRE(by_column.count("price") == 1);

  REQUIRE(by_column["category"]->index_type == LANCEDB_INDEX_BITMAP);
  REQUIRE(std::string(by_column["category"]->name) == "category_idx");

  REQUIRE(by_column["price"]->index_type == LANCEDB_INDEX_BTREE);
  REQUIRE(std::string(by_column["price"]->name) == "price_idx");

  lancedb_free_index_list_detailed(indices, count);
  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "Drop scalar index and verify via list detailed", "[scalar_index]") {
  LanceDBTable* table = create_scalar_table(db, "detailed_drop", 50);

  char* error_message = nullptr;
  LanceDBScalarIndexConfig cfg = {.replace = 0, .force_update_statistics = 0};

  // Create two indexes
  const char* col1[] = {"category"};
  REQUIRE(lancedb_table_create_scalar_index(
      table, col1, 1, LANCEDB_INDEX_BITMAP, &cfg, &error_message) == LANCEDB_SUCCESS);

  const char* col2[] = {"price"};
  REQUIRE(lancedb_table_create_scalar_index(
      table, col2, 1, LANCEDB_INDEX_BTREE, &cfg, &error_message) == LANCEDB_SUCCESS);

  // Verify both exist
  LanceDBIndexInfo* indices = nullptr;
  size_t count = 0;
  REQUIRE(lancedb_table_list_indices_detailed(
      table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(count == 2);
  lancedb_free_index_list_detailed(indices, count);

  // Drop category_idx
  REQUIRE(lancedb_table_drop_index(
      table, "category_idx", &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  // Verify only price_idx remains
  indices = nullptr;
  count = 0;
  REQUIRE(lancedb_table_list_indices_detailed(
      table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(count == 1);
  REQUIRE(std::string(indices[0].name) == "price_idx");
  REQUIRE(indices[0].index_type == LANCEDB_INDEX_BTREE);
  REQUIRE(indices[0].num_columns == 1);
  REQUIRE(std::string(indices[0].columns[0]) == "price");

  lancedb_free_index_list_detailed(indices, count);
  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "Create scalar index with replace", "[scalar_index]") {
  LanceDBTable* table = create_scalar_table(db, "detailed_replace", 50);

  char* error_message = nullptr;

  // Create BTREE on category
  const char* col[] = {"category"};
  LanceDBScalarIndexConfig cfg = {.replace = 0, .force_update_statistics = 0};
  REQUIRE(lancedb_table_create_scalar_index(
      table, col, 1, LANCEDB_INDEX_BTREE, &cfg, &error_message) == LANCEDB_SUCCESS);

  // Replace with BITMAP
  cfg.replace = 1;
  REQUIRE(lancedb_table_create_scalar_index(
      table, col, 1, LANCEDB_INDEX_BITMAP, &cfg, &error_message) == LANCEDB_SUCCESS);

  // Verify it's now BITMAP
  LanceDBIndexInfo* indices = nullptr;
  size_t count = 0;
  REQUIRE(lancedb_table_list_indices_detailed(
      table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(count == 1);
  REQUIRE(std::string(indices[0].columns[0]) == "category");
  REQUIRE(indices[0].index_type == LANCEDB_INDEX_BITMAP);

  lancedb_free_index_list_detailed(indices, count);
  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "List indices detailed - null parameter handling", "[scalar_index]") {
  LanceDBTable* table = create_scalar_table(db, "detailed_null", 50);

  // Null indices_out
  size_t count = 0;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_list_indices_detailed(
      table, nullptr, &count, &error_message) == LANCEDB_INVALID_ARGUMENT);

  // Null count_out
  LanceDBIndexInfo* indices = nullptr;
  REQUIRE(lancedb_table_list_indices_detailed(
      table, &indices, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);

  // Null table
  REQUIRE(lancedb_table_list_indices_detailed(
      nullptr, &indices, &count, &error_message) == LANCEDB_INVALID_ARGUMENT);

  lancedb_table_free(table);
}
