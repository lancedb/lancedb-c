/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <string>

#include "test_common.h"

namespace {

// Return the name of the only index on the table
std::string single_index_name(const LanceDBTable* table) {
  char** indices = nullptr;
  size_t count = 0;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_list_indices(table, &indices, &count, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(count == 1);
  std::string name(indices[0]);
  lancedb_free_index_list(indices, count);
  return name;
}

// Read the statistics of an existing index, requiring the call to succeed
const LanceDBIndexStatistics* get_index_statistics(const LanceDBTable* table, const std::string& index_name) {
  char* error_message = nullptr;
  const LanceDBIndexStatistics* stats = nullptr;
  REQUIRE(lancedb_table_index_statistics(table, index_name.c_str(), &stats, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(stats != nullptr);
  return stats;
}

}  // namespace

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Index Statistics", "[index]") {
  const std::string table_name = "index_stats_test";

  SECTION("A scalar index reports its type and indexed rows") {
    LanceDBTable* table = create_table_with_data(table_name, 100, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {
      .replace = 0
    };
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    const std::string index_name = single_index_name(table);

    const LanceDBIndexStatistics* stats = get_index_statistics(table, index_name);
    REQUIRE(stats->index_type != nullptr);
    CHECK(std::string(stats->index_type) == "BTREE");
    // A scalar index carries no distance metric
    CHECK(stats->distance_type == nullptr);
    CHECK(stats->num_indexed_rows == 100);
    CHECK(stats->num_unindexed_rows == 0);
    REQUIRE(stats->has_num_indices);
    CHECK(stats->num_indices >= 1);
    lancedb_free_index_statistics(stats);

    // Rows added after the index was built are not covered by it yet
    auto batch = create_test_record_batch(25, 100);
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);
    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    stats = get_index_statistics(table, index_name);
    CHECK(stats->num_indexed_rows == 100);
    CHECK(stats->num_unindexed_rows == 25);
    lancedb_free_index_statistics(stats);

    lancedb_table_free(table);
  }

  SECTION("A missing index is a successful call without statistics") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    // The table has an index, but not the one that is asked for
    const char* columns[] = {"key"};
    LanceDBScalarIndexConfig config = {
      .replace = 0
    };
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_scalar_index(
        table, columns, 1, LANCEDB_INDEX_BTREE, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    const LanceDBIndexStatistics* stats = nullptr;
    REQUIRE(lancedb_table_index_statistics(
        table, "missing_index", &stats, &error_message) == LANCEDB_SUCCESS);
    CHECK(stats == nullptr);
    CHECK(error_message == nullptr);
    // Freeing the absent result is allowed
    lancedb_free_index_statistics(stats);

    lancedb_table_free(table);
  }

  SECTION("Invalid arguments are reported as errors") {
    LanceDBTable* table = create_table_with_data(table_name, 20, 0);
    REQUIRE(table != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_table_index_statistics(
        table, "any_index", nullptr, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const LanceDBIndexStatistics* stats = nullptr;
    REQUIRE(lancedb_table_index_statistics(
        nullptr, "any_index", &stats, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    CHECK(stats == nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // A NULL index name is rejected before any lookup
    REQUIRE(lancedb_table_index_statistics(
        table, nullptr, &stats, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    CHECK(stats == nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // An index name that is not valid UTF-8 is rejected, not looked up
    const char invalid_utf8_name[] = {'b', 'a', 'd', '\xFF', 'n', 'a', 'm', 'e', '\0'};
    REQUIRE(lancedb_table_index_statistics(
        table, invalid_utf8_name, &stats, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    CHECK(stats == nullptr);
    lancedb_free_string(error_message);

    lancedb_table_free(table);
  }
}
