/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <memory>
#include <string>

#include "test_common.h"

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Index Info", "[index]") {
  const std::string table_name = "index_info_test";

  SECTION("List rich metadata for two scalar index kinds") {
    auto schema = arrow::schema({
        arrow::field("key", arrow::utf8()),
        arrow::field("category", arrow::utf8()),
    });

    arrow::StringBuilder key_builder;
    arrow::StringBuilder category_builder;
    for (int i = 0; i < 20; i++) {
      REQUIRE(key_builder.Append("key_" + std::to_string(i)).ok());
      REQUIRE(category_builder.Append(i % 2 == 0 ? "even" : "odd").ok());
    }

    std::shared_ptr<arrow::Array> key_array;
    std::shared_ptr<arrow::Array> category_array;
    REQUIRE(key_builder.Finish(&key_array).ok());
    REQUIRE(category_builder.Finish(&category_array).ok());

    auto batch = arrow::RecordBatch::Make(schema, 20, {key_array, category_array});
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    struct ArrowSchema c_schema;
    REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

    LanceDBTable* table = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create(
        db,
        table_name.c_str(),
        reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
        reader,
        &table,
        &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(table != nullptr);

    // Clean up schema
    if (c_schema.release) {
      c_schema.release(&c_schema);
    }

    const char* key_columns[] = {"key"};
    LanceDBScalarIndexConfig scalar_config = {
      .replace = 0
    };
    REQUIRE(lancedb_table_create_scalar_index(
        table, key_columns, 1, LANCEDB_INDEX_BTREE, &scalar_config, &error_message) ==
        LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    const char* category_columns[] = {"category"};
    REQUIRE(lancedb_table_create_scalar_index(
        table, category_columns, 1, LANCEDB_INDEX_BITMAP, &scalar_config, &error_message) ==
        LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBIndexInfo* infos = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices_info(table, &infos, &count, &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 2);
    REQUIRE(infos != nullptr);

    bool found_btree = false;
    bool found_bitmap = false;
    for (size_t i = 0; i < count; i++) {
      const LanceDBIndexInfo& info = infos[i];
      REQUIRE(info.name != nullptr);
      REQUIRE(info.index_type != nullptr);
      REQUIRE(info.num_columns == 1);
      REQUIRE(info.columns != nullptr);
      REQUIRE(info.columns[0] != nullptr);

      const std::string name = info.name;
      const std::string column = info.columns[0];
      const std::string index_type = info.index_type;
      INFO("Index " << i << ": name=" << name << ", column=" << column
                    << ", type=" << index_type);

      // The SDK names an index after its column and adds "_idx", so the index
      // name and the indexed column are distinct.
      REQUIRE(name == column + "_idx");
      REQUIRE(name != column);

      if (column == "key") {
        found_btree = true;
        REQUIRE(index_type == "BTREE");
      } else if (column == "category") {
        found_bitmap = true;
        REQUIRE(index_type == "BITMAP");
      } else {
        FAIL("unexpected indexed column: " << column);
      }
    }
    REQUIRE(found_btree);
    REQUIRE(found_bitmap);

    // The names-only list still reports the same indices.
    char** names = nullptr;
    size_t names_count = 0;
    REQUIRE(lancedb_table_list_indices(table, &names, &names_count, &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(names_count == count);
    REQUIRE(names != nullptr);

    for (size_t i = 0; i < names_count; i++) {
      bool found = false;
      for (size_t j = 0; j < count; j++) {
        if (std::string(names[i]) == infos[j].name) {
          found = true;
          break;
        }
      }
      REQUIRE(found);
    }

    lancedb_free_index_list(names, names_count);
    lancedb_free_index_info(infos, count);

    lancedb_table_free(table);
  }

  SECTION("An empty index list has a NULL array") {
    LanceDBTable* table = create_table_with_data(table_name, 10, 0);
    REQUIRE(table != nullptr);

    LanceDBIndexInfo* infos = nullptr;
    size_t count = 1;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_list_indices_info(table, &infos, &count, &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 0);
    REQUIRE(infos == nullptr);

    lancedb_free_index_info(infos, count);

    lancedb_table_free(table);
  }

  SECTION("Invalid arguments are rejected") {
    LanceDBTable* table = create_table_with_data(table_name, 10, 0);
    REQUIRE(table != nullptr);

    LanceDBIndexInfo* infos = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices_info(nullptr, &infos, &count, nullptr) ==
            LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_list_indices_info(table, nullptr, &count, nullptr) ==
            LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_list_indices_info(table, &infos, nullptr, nullptr) ==
            LANCEDB_INVALID_ARGUMENT);

    lancedb_free_index_info(nullptr, 0);

    lancedb_table_free(table);
  }
}
