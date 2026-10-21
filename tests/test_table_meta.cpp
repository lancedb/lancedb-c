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

    LanceDBError result = lancedb_table_list_versions(table, &versions, nullptr, &count, &error_message);

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
    LanceDBTable* table = open_table(table_name);

    // Add data 3 times to create versions 2, 3, 4
    char* error_message = nullptr;
    for (int i = 0; i < 3; i++) {
      auto batch = create_test_record_batch(5, i * 5);
      auto reader = create_reader_from_batch(batch);
      REQUIRE(reader != nullptr);

      LanceDBError add_result = lancedb_table_add(table, reader, &error_message);
      REQUIRE(add_result == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }

    LanceDBVersion* versions = nullptr;
    size_t count = 0;

    LanceDBError result = lancedb_table_list_versions(table, &versions, nullptr, &count, &error_message);

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

    LanceDBError result = lancedb_table_list_versions(table, &versions, &metadata, &count, &error_message);

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

    LanceDBError result = lancedb_table_list_versions(nullptr, &versions, nullptr, &count, &error_message);

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

    LanceDBError result = lancedb_table_list_versions(table, nullptr, nullptr, &count, &error_message);
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

    LanceDBError result = lancedb_table_arrow_schema(table, &schema_out, &error_message);

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

    LanceDBError result = lancedb_table_arrow_schema(nullptr, &schema_out, &error_message);

    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Null schema_out should fail") {
    constexpr auto row_num = 5;
    LanceDBTable* table = create_table_with_data("schema_null_out", row_num, 0);
    REQUIRE(table != nullptr);

    char* error_message = nullptr;

    LanceDBError result = lancedb_table_arrow_schema(table, nullptr, &error_message);

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

    LanceDBError result = lancedb_table_get_metadata(table, nullptr, 0, &keys, &values, &count, &error_message);

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

    LanceDBError result = lancedb_table_set_metadata(table, set_keys.get(), set_values.get(), expected.size(), &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Now get the metadata back
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, nullptr, 0, &keys, &values, &count, &error_message);
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

    LanceDBError result = lancedb_table_set_metadata(table, set_keys, set_values, set_length, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Filter for specific keys
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, filter_keys, filter_length, &keys, &values, &count, &error_message);
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

    LanceDBError result = lancedb_table_set_metadata(table, set_keys, set_values, set_length, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, filter_keys, filter_length, &keys, &values, &count, &error_message);
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

    LanceDBError result = lancedb_table_set_metadata(table, set_keys, set_values, 1, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Create a filter key with invalid UTF-8 bytes
    const char invalid_utf8[] = {'\xc0', '\xaf', '\0'};
    const char* filter_keys[] = {invalid_utf8};
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, filter_keys, 1, &keys, &values, &count, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Update existing metadata") {
    const char* set_keys[] = {"color"};
    const char* set_values[] = {"red"};
    char* error_message = nullptr;

    LanceDBError result = lancedb_table_set_metadata(table, set_keys, set_values, 1, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Update the value
    const char* update_values[] = {"blue"};
    result = lancedb_table_set_metadata(table, set_keys, update_values, 1, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Verify the updated value
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, nullptr, 0, &keys, &values, &count, &error_message);
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

    LanceDBError result = lancedb_table_set_metadata(table, set_keys, set_values, set_length, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    result = lancedb_table_delete_metadata(table, del_keys, del_length, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // Verify remaining metadata
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    result = lancedb_table_get_metadata(table, nullptr, 0, &keys, &values, &count, &error_message);
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

    LanceDBError result = lancedb_table_delete_metadata(table, del_keys, 1, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("Set metadata with null arguments should fail") {
    char* error_message = nullptr;

    // Null table
    const char* keys[] = {"k"};
    const char* values[] = {"v"};
    REQUIRE(lancedb_table_set_metadata(nullptr, keys, values, 1, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Null keys
    REQUIRE(lancedb_table_set_metadata(table, nullptr, values, 1, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Null values
    REQUIRE(lancedb_table_set_metadata(table, keys, nullptr, 1, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Zero count
    REQUIRE(lancedb_table_set_metadata(table, keys, values, 0, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Get metadata with null arguments should fail") {
    char* error_message = nullptr;
    char** keys = nullptr;
    char** values = nullptr;
    size_t count = 0;

    REQUIRE(lancedb_table_get_metadata(nullptr, nullptr, 0, &keys, &values, &count, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_get_metadata(table, nullptr, 0, nullptr, &values, &count, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Null filter_keys with non-zero filter_count
    REQUIRE(lancedb_table_get_metadata(table, nullptr, 1, &keys, &values, &count, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Non-null filter_keys with zero filter_count
    const char* filter[] = {"k"};
    REQUIRE(lancedb_table_get_metadata(table, filter, 0, &keys, &values, &count, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Delete metadata with null arguments should fail") {
    char* error_message = nullptr;

    REQUIRE(lancedb_table_delete_metadata(nullptr, nullptr, 1, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_delete_metadata(table, nullptr, 1, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Free null metadata is safe") {
    lancedb_free_metadata(nullptr, nullptr, 0);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Metadata - invalid arguments", "[table]") {
  const std::string table_name = "metadata_invalid_args_test";
  LanceDBTable* table = create_table_with_data(table_name, 5, 0);
  REQUIRE(table != nullptr);

  // A string that is not valid UTF-8, to exercise the string conversion failures
  const char* const invalid_utf8 = "\xff\xfe";
  const char* keys[] = {"key1"};
  const char* values[] = {"value1"};
  const char* null_keys[] = {nullptr};
  const char* invalid_keys[] = {invalid_utf8};
  const char* invalid_values[] = {invalid_utf8};

  SECTION("Setting metadata with invalid arguments should fail") {
    REQUIRE(lancedb_table_set_metadata(
        table, null_keys, values, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_set_metadata(
        table, invalid_keys, values, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_set_metadata(
        table, keys, invalid_values, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Deleting metadata with invalid arguments should fail") {
    REQUIRE(lancedb_table_delete_metadata(
        table, null_keys, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_delete_metadata(
        table, invalid_keys, 1, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Getting metadata with invalid arguments should fail") {
    char** keys_out = nullptr;
    char** values_out = nullptr;
    size_t count_out = 0;

    REQUIRE(lancedb_table_get_metadata(
        table, nullptr, 0, &keys_out, nullptr, &count_out, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_get_metadata(
        table, nullptr, 0, &keys_out, &values_out, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_get_metadata(
        table, null_keys, 1, &keys_out, &values_out, &count_out, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_table_get_metadata(
        table, invalid_keys, 1, &keys_out, &values_out, &count_out, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Getting metadata that was not set returns nothing") {
    char** keys_out = nullptr;
    char** values_out = nullptr;
    size_t count_out = 0;
    char* error_message = nullptr;
    const char* missing_keys[] = {"no_such_key"};

    REQUIRE(lancedb_table_get_metadata(
        table, missing_keys, 1, &keys_out, &values_out, &count_out, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count_out == 0);
  }

  SECTION("Listing versions without a count should fail") {
    LanceDBVersion* versions = nullptr;
    LanceDBVersionMetadata* metadata = nullptr;

    REQUIRE(lancedb_table_list_versions(
        table, &versions, &metadata, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Stats", "[table]") {
  LanceDBTable* table = create_table_with_data("stats_table", 10, 0);
  REQUIRE(table != nullptr);

  SECTION("Stats on freshly created table") {
    LanceDBTableStats stats{};
    char* error_message = nullptr;

    REQUIRE(lancedb_table_stats(table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.total_bytes > 0);
    REQUIRE(stats.num_rows == 10);
    REQUIRE(stats.num_deleted_rows == 0);
    REQUIRE(stats.num_indices == 0);
    REQUIRE(stats.fragment_stats.num_fragments == 1);
    REQUIRE(stats.fragment_stats.num_small_fragments == 1);
    REQUIRE(stats.fragment_stats.lengths.min == 10);
    REQUIRE(stats.fragment_stats.lengths.max == 10);
    REQUIRE(stats.fragment_stats.lengths.mean == 10);
    REQUIRE(stats.fragment_stats.lengths.p25 == 10);
    REQUIRE(stats.fragment_stats.lengths.p50 == 10);
    REQUIRE(stats.fragment_stats.lengths.p75 == 10);
    REQUIRE(stats.fragment_stats.lengths.p99 == 10);
  }

  SECTION("Deleted rows are counted until the table is compacted") {
    // A second fragment, holding only rows that the delete below removes
    auto batch = create_test_record_batch(3, 20);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBTableStats stats{};
    REQUIRE(lancedb_table_stats(table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.num_rows == 13);
    REQUIRE(stats.num_deleted_rows == 0);
    REQUIRE(stats.fragment_stats.num_fragments == 2);
    REQUIRE(stats.fragment_stats.lengths.min == 3);
    REQUIRE(stats.fragment_stats.lengths.max == 10);
    const size_t total_bytes_before_delete = stats.total_bytes;

    // Delete every row of the second fragment and 3 rows of the first one.
    // The emptied fragment is dropped outright rather than kept with a
    // deletion file, so only the 3 rows marked in the surviving fragment
    // are counted as deleted.
    REQUIRE(lancedb_table_delete(table,
        "key IN ('key_20', 'key_21', 'key_22', 'key_1', 'key_2', 'key_3')",
        &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(lancedb_table_stats(table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.num_rows == 7);
    REQUIRE(stats.num_deleted_rows == 3);
    REQUIRE(stats.fragment_stats.num_fragments == 1);
    // The marked rows are physically still in the fragment, so they count
    // towards its length even though they are excluded from num_rows
    REQUIRE(stats.fragment_stats.lengths.max == 10);
    // Deletion files do not count toward total_bytes
    REQUIRE(stats.total_bytes < total_bytes_before_delete);

    // Compaction materializes the deletions
    REQUIRE(lancedb_table_optimize(table, LANCEDB_OPTIMIZE_COMPACT, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(lancedb_table_stats(table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.num_rows == 7);
    REQUIRE(stats.num_deleted_rows == 0);
    REQUIRE(stats.fragment_stats.num_fragments == 1);
    REQUIRE(stats.fragment_stats.lengths.max == 7);
  }

  SECTION("Indices are counted") {
    LanceDBTableStats stats{};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_stats(table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.num_indices == 0);

    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {.replace = 0};
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(lancedb_table_stats(table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.num_indices == 1);
    REQUIRE(stats.num_rows == 10);
    REQUIRE(stats.num_deleted_rows == 0);

    // Find the index name and drop it
    char** indices = nullptr;
    size_t count = 0;
    REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count == 1);
    REQUIRE(lancedb_table_drop_index(table, indices[0], &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_free_index_list(indices, count);

    REQUIRE(lancedb_table_stats(table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.num_indices == 0);
    REQUIRE(stats.num_rows == 10);
    REQUIRE(stats.num_deleted_rows == 0);
  }

  SECTION("Fragments with at least 100000 rows are not small") {
    // A single fragment holding 100000 rows is right at the threshold
    constexpr auto row_num1 = 100000;
    LanceDBTable* large_table = create_table_with_data("large_stats_table", row_num1, 0);
    REQUIRE(large_table != nullptr);

    LanceDBTableStats stats{};
    char* error_message = nullptr;
    REQUIRE(lancedb_table_stats(large_table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.num_rows == row_num1);
    REQUIRE(stats.fragment_stats.num_fragments == 1);
    REQUIRE(stats.fragment_stats.num_small_fragments == 0);
    REQUIRE(stats.fragment_stats.lengths.min == row_num1);
    REQUIRE(stats.fragment_stats.lengths.max == row_num1);

    // Adding a small fragment next to the large one is counted as small
    constexpr auto row_num2 = 10;
    auto batch = create_test_record_batch(row_num2, row_num1);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);
    REQUIRE(lancedb_table_add(large_table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    REQUIRE(lancedb_table_stats(large_table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.num_rows == row_num1 + row_num2);
    REQUIRE(stats.fragment_stats.num_fragments == 2);
    REQUIRE(stats.fragment_stats.num_small_fragments == 1);
    REQUIRE(stats.fragment_stats.lengths.min == row_num2);
    REQUIRE(stats.fragment_stats.lengths.max == row_num1);
    REQUIRE(stats.fragment_stats.lengths.mean == (row_num1 + row_num2) / 2);

    lancedb_table_free(large_table);
  }

  SECTION("Fragment length percentiles") {
    // Add fragments of 1, 2, 4 and 20 rows next to the existing 10 row one,
    // so that the sorted fragment lengths are 1, 2, 4, 10, 20
    char* error_message = nullptr;
    constexpr auto existing_batch_size = 10;
    constexpr auto num_batches = 4;
    constexpr std::array<unsigned, num_batches> num_rows_arr = {1, 2, 4, 20};
    constexpr auto total_rows = std::accumulate(num_rows_arr.begin(),
        num_rows_arr.end(),
        0) + 10; // include the 10 rows already in the first fragment
    constexpr auto total_num_batches = num_batches + 1; // include the first fragment
    auto start_index = existing_batch_size;
    for (int num_rows : num_rows_arr) {
      auto batch = create_test_record_batch(num_rows, start_index);
      auto reader = create_reader_from_batch(batch);
      REQUIRE(reader != nullptr);
      REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
      start_index += num_rows;
    }

    LanceDBTableStats stats{};
    REQUIRE(lancedb_table_stats(table, &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats.num_rows == total_rows);
    REQUIRE(stats.fragment_stats.num_fragments == total_num_batches);
    REQUIRE(stats.fragment_stats.num_small_fragments == total_num_batches);
    REQUIRE(stats.fragment_stats.lengths.min == num_rows_arr[0]);
    REQUIRE(stats.fragment_stats.lengths.max == num_rows_arr[3]);
    REQUIRE(stats.fragment_stats.lengths.mean == (total_rows / total_num_batches));
    REQUIRE(stats.fragment_stats.lengths.p25 == num_rows_arr[1]);
    REQUIRE(stats.fragment_stats.lengths.p50 == num_rows_arr[2]);
    REQUIRE(stats.fragment_stats.lengths.p75 == existing_batch_size);
    REQUIRE(stats.fragment_stats.lengths.p99 == num_rows_arr[3]);
  }

  SECTION("Stats with null arguments should fail") {
    LanceDBTableStats stats{};
    char* error_message = nullptr;

    REQUIRE(lancedb_table_stats(nullptr, &stats, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_stats(table, nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    // Null error message is allowed
    REQUIRE(lancedb_table_stats(nullptr, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  lancedb_table_free(table);
}
