/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <cstring>
#include <map>
#include <string>
#include "test_common.h"

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table List Versions", "[table]") {
  SECTION("List versions on freshly created table") {
    constexpr auto row_num = 10;
    LanceDBTable* table = create_table_with_data("versions_test", row_num, 0);
    REQUIRE(table != nullptr);

    LanceDBVersion* versions = nullptr;
    size_t count = 0;
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_list_versions(table, &versions, nullptr, &count, nullptr, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(versions != nullptr);

    // Verify version 1 has a valid timestamp within 5 seconds of now
    auto now = std::chrono::system_clock::now();
    auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    REQUIRE(versions[0].version == 1);
    REQUIRE(versions[0].timestamp_nanos < 1000000000);
    REQUIRE(std::abs(now_seconds - versions[0].timestamp_seconds) <= 5);

    lancedb_free_versions(versions, count);
    lancedb_table_free(table);
  }

  SECTION("List versions after adding data") {
    const std::string table_name = "versions_add_test";
    create_empty_table(table_name);
    LanceDBTable* table = lancedb_connection_open_table(db, table_name.c_str(), nullptr);
    REQUIRE(table != nullptr);

    // Add data 3 times to create versions 2, 3, 4
    char* error_message = nullptr;
    for (int i = 0; i < 3; i++) {
      auto batch = create_test_record_batch(5, i * 5);
      auto reader = create_reader_from_batch(batch);
      REQUIRE(reader != nullptr);

      LanceDBError add_result = lancedb_table_add(table, reader, nullptr, &error_message);
      REQUIRE(add_result == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }

    LanceDBVersion* versions = nullptr;
    size_t count = 0;

    LanceDBError result = lancedb_table_list_versions(table, &versions, nullptr, &count, nullptr, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    // 1 (empty table creation) + 3 (adds) = 4 versions
    REQUIRE(count == 4);
    REQUIRE(versions != nullptr);

    lancedb_free_versions(versions, count);
    lancedb_table_free(table);
  }

  SECTION("List versions with metadata") {
    constexpr auto row_num = 10;
    LanceDBTable* table = create_table_with_data("versions_metadata_test", row_num, 0);
    REQUIRE(table != nullptr);

    LanceDBVersion* versions = nullptr;
    LanceDBVersionMetadata* metadata = nullptr;
    size_t count = 0;
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_list_versions(table, &versions, &metadata, &count, nullptr, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(versions != nullptr);
    REQUIRE(metadata != nullptr);
    REQUIRE(metadata[0].count > 0);
    REQUIRE(metadata[0].keys != nullptr);
    REQUIRE(metadata[0].values != nullptr);

    lancedb_free_version_metadata(metadata, count);
    lancedb_free_versions(versions, count);
    lancedb_table_free(table);
  }

  SECTION("List versions with null table should fail") {
    LanceDBVersion* versions = nullptr;
    size_t count = 0;
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_list_versions(nullptr, &versions, nullptr, &count, nullptr, &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("List versions with null output should fail") {
    constexpr auto row_num = 5;
    LanceDBTable* table = create_table_with_data("versions_null_out", row_num, 0);
    REQUIRE(table != nullptr);

    char* error_message = nullptr;
    size_t count = 0;

    LanceDBError result = lancedb_table_list_versions(table, nullptr, nullptr, &count, nullptr, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_table_free(table);
  }

  SECTION("Free null versions is safe") {
    lancedb_free_versions(nullptr, 0);
  }

  SECTION("Free null version metadata is safe") {
    lancedb_free_version_metadata(nullptr, 0);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Schema", "[table]") {
  SECTION("Get schema from table") {
    constexpr auto row_num = 5;
    LanceDBTable* table = create_table_with_data("schema_test", row_num, 0);
    REQUIRE(table != nullptr);

    FFI_ArrowSchema* schema_out = nullptr;
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_arrow_schema(table, &schema_out, nullptr, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(schema_out != nullptr);

    // Import the FFI schema into Arrow C++ to verify its contents
    auto import_result = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(schema_out));
    REQUIRE(import_result.ok());
    auto schema = import_result.ValueUnsafe();

    REQUIRE(schema->num_fields() == 2);
    REQUIRE(schema->field(0)->name() == "key");
    REQUIRE(schema->field(0)->type()->Equals(arrow::utf8()));
    REQUIRE(schema->field(1)->name() == "data");
    REQUIRE(schema->field(1)->type()->Equals(arrow::fixed_size_list(arrow::float32(), TEST_SCHEMA_DIMENSIONS)));

    // ImportSchema releases the schema contents, but we still need to free the outer allocation
    lancedb_free_arrow_schema(schema_out);
    lancedb_table_free(table);
  }

  SECTION("Null table should fail") {
    FFI_ArrowSchema* schema_out = nullptr;
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_arrow_schema(nullptr, &schema_out, nullptr, &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Null schema_out should fail") {
    constexpr auto row_num = 5;
    LanceDBTable* table = create_table_with_data("schema_null_out", row_num, 0);
    REQUIRE(table != nullptr);

    char* error_message = nullptr;

    LanceDBError result = lancedb_table_arrow_schema(table, nullptr, nullptr, &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_table_free(table);
  }

  SECTION("Free null schema is safe") {
    lancedb_free_arrow_schema(nullptr);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Metadata", "[table]") {
  // Create a table with data
  const std::string table_name = "metadata_test";
  constexpr auto row_num = 5;
  LanceDBTable* table = create_table_with_data(table_name, row_num, 0);
  REQUIRE(table != nullptr);

  SECTION("Get metadata on table with no metadata returns empty") {
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_get_metadata(table, nullptr, 0, &keys, &values, &count, nullptr, &error_message);

    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 0);
    REQUIRE(keys == nullptr);
    REQUIRE(values == nullptr);
  }

  SECTION("Set and get metadata") {
    const std::map<std::string, std::string> expected = {{"author", "test_user"}, {"version", "1.0"}, {"metric", "cosine"}};
    auto set_keys   = std::make_unique<const char*[]>(expected.size());
    auto set_values = std::make_unique<const char*[]>(expected.size());

    int i = 0;
    std::for_each(expected.begin(), expected.end(), [&](const auto& kv) {
      set_keys[i]   = kv.first.c_str();
      set_values[i] = kv.second.c_str();
      ++i;
    });
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_set_metadata(table, set_keys.get(), set_values.get(), expected.size(), nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Now get the metadata back
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, nullptr, 0, &keys, &values, &count, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == expected.size());
    REQUIRE(keys != nullptr);
    REQUIRE(values != nullptr);

    std::map<std::string, std::string> metadata;
    for (size_t i = 0; i < count; i++) {
      metadata.emplace(keys[i], values[i]);
    }
    REQUIRE(metadata == expected);

    lancedb_free_metadata(keys, values, count);
  }

  SECTION("Get metadata with filter") {
    const char* set_keys[] = {"abc", "bcd", "cde"};
    const char* set_values[] = {"1", "2", "3"};
    constexpr size_t set_length = 3;
    const char* filter_keys[] = {"abc", "bbb", "cde"};
    constexpr size_t filter_length = 3;
    std::map<std::string, std::string> expected;
    for (size_t i = 0; i < set_length; ++i) {
      for (size_t j = 0; j < filter_length; ++j) {
        if (std::strcmp(set_keys[i], filter_keys[j]) == 0) {
          expected.emplace(set_keys[i], set_values[i]);
          break;
        }
      }
    }
    // only 2 out of the 3 filters match
    REQUIRE(expected.size() == filter_length-1);
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_set_metadata(table, set_keys, set_values, set_length, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Filter for specific keys
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, filter_keys, filter_length, &keys, &values, &count, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == expected.size());

    std::map<std::string, std::string> metadata;
    for (size_t i = 0; i < count; i++) {
      metadata.emplace(keys[i], values[i]);
    }
    REQUIRE(metadata == expected);

    lancedb_free_metadata(keys, values, count);
  }

  SECTION("Get metadata with filter skips nonexistent keys") {
    const char* set_keys[] = {"w", "x", "y", "z"};
    const char* set_values[] = {"0", "10", "20", "30"};
    constexpr size_t set_length = 4;
    const char* filter_keys[] = {"x", "nonexistent"};
    constexpr size_t filter_length = 2;
    std::map<std::string, std::string> expected;
    for (size_t i = 0; i < set_length; ++i) {
      for (size_t j = 0; j < filter_length; ++j) {
        if (std::strcmp(set_keys[i], filter_keys[j]) == 0) {
          expected.emplace(set_keys[i], set_values[i]);
          break;
        }
      }
    }
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_set_metadata(table, set_keys, set_values, set_length, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, filter_keys, filter_length, &keys, &values, &count, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == expected.size());

    std::map<std::string, std::string> metadata;
    for (size_t i = 0; i < count; i++) {
      metadata.emplace(keys[i], values[i]);
    }
    REQUIRE(metadata == expected);

    lancedb_free_metadata(keys, values, count);
  }

  SECTION("Get metadata with invalid UTF-8 filter key fails") {
    const char* set_keys[] = {"valid_key"};
    const char* set_values[] = {"valid_value"};
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_set_metadata(table, set_keys, set_values, 1, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Create a filter key with invalid UTF-8 bytes
    const char invalid_utf8[] = {'\xc0', '\xaf', '\0'};
    const char* filter_keys[] = {invalid_utf8};
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, filter_keys, 1, &keys, &values, &count, nullptr, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Update existing metadata") {
    const char* set_keys[] = {"color"};
    const char* set_values[] = {"red"};
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_set_metadata(table, set_keys, set_values, 1, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Update the value
    const char* update_values[] = {"blue"};
    result = lancedb_table_set_metadata(table, set_keys, update_values, 1, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Verify the updated value
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, nullptr, 0, &keys, &values, &count, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);

    REQUIRE(std::string(keys[0]) == "color");
    REQUIRE(std::string(values[0]) == "blue");

    lancedb_free_metadata(keys, values, count);
  }

  SECTION("Delete metadata") {
    const char* set_keys[] = {"key1", "key2", "key3", "key4", "key5"};
    const char* set_values[] = {"val1", "val2", "val3", "val4", "val5"};
    constexpr size_t set_length = 5;
    const char* del_keys[] = {"key2", "nonexistent", "key1"};
    constexpr size_t del_length = 3;
    std::map<std::string, std::string> expected;
    for (size_t i = 0; i < set_length; ++i) {
      bool is_expected = true;
      for (size_t j = 0; j < del_length; ++j) {
        if (set_keys[i] == del_keys[j]) {
          is_expected = false;
          break;
        }
      }
      if (is_expected) {
        expected.emplace(set_keys[i], set_values[i]);
      }
    }

    char* error_message = nullptr;

    LanceDBError result = lancedb_table_set_metadata(table, set_keys, set_values, set_length, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_table_delete_metadata(table, del_keys, del_length, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Verify remaining metadata
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, nullptr, 0, &keys, &values, &count, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == expected.size());

    std::map<std::string, std::string> metadata;
    for (size_t i = 0; i < count; i++) {
      metadata.emplace(keys[i], values[i]);
    }
    REQUIRE(metadata == expected);


    lancedb_free_metadata(keys, values, count);
  }

  SECTION("Delete nonexistent key is silent") {
    const char* del_keys[] = {"nonexistent"};
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_delete_metadata(table, del_keys, 1, nullptr, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("Set metadata with null arguments should fail") {
    char* error_message = nullptr;

    // Null table
    const char* keys[] = {"k"};
    const char* values[] = {"v"};
    REQUIRE(lancedb_table_set_metadata(nullptr, keys, values, 1, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Null keys
    REQUIRE(lancedb_table_set_metadata(table, nullptr, values, 1, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Null values
    REQUIRE(lancedb_table_set_metadata(table, keys, nullptr, 1, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Zero count
    REQUIRE(lancedb_table_set_metadata(table, keys, values, 0, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Get metadata with null arguments should fail") {
    char* error_message = nullptr;
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    REQUIRE(lancedb_table_get_metadata(nullptr, nullptr, 0, &keys, &values, &count, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_get_metadata(table, nullptr, 0, nullptr, &values, &count, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Null filter_keys with non-zero filter_count
    REQUIRE(lancedb_table_get_metadata(table, nullptr, 1, &keys, &values, &count, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Non-null filter_keys with zero filter_count
    const char* filter[] = {"k"};
    REQUIRE(lancedb_table_get_metadata(table, filter, 0, &keys, &values, &count, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Delete metadata with null arguments should fail") {
    char* error_message = nullptr;

    REQUIRE(lancedb_table_delete_metadata(nullptr, nullptr, 1, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_delete_metadata(table, nullptr, 1, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Free null metadata is safe") {
    lancedb_free_metadata(nullptr, nullptr, 0);
  }

  lancedb_table_free(table);
}

