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

}  // namespace

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Vector Index Statistics", "[vector_index]") {
  const std::string table_name = "vector_index_stats_test";

  SECTION("A vector index reports its distance metric") {
    LanceDBTable* table = create_table_with_data(table_name, 256, 0);
    REQUIRE(table != nullptr);

    const char* columns[] = {"data"};
    LanceDBVectorIndexConfig config = {
      .num_partitions = -1,
      .num_sub_vectors = -1,
      .max_iterations = -1,
      .sample_rate = 0.0f,
      .distance_type = LANCEDB_DISTANCE_COSINE,
      .replace = 0
    };
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create_vector_index(
        table, columns, 1, LANCEDB_INDEX_IVF_FLAT, &config, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    const std::string index_name = single_index_name(table);

    const LanceDBIndexStatistics* stats = nullptr;
    REQUIRE(lancedb_table_index_statistics(
        table, index_name.c_str(), &stats, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(stats != nullptr);
    REQUIRE(stats->index_type != nullptr);
    CHECK(std::string(stats->index_type) == "IVF_FLAT");
    REQUIRE(stats->distance_type != nullptr);
    // The SDK reports the metric by its own name, not the C enum ordinal
    CHECK(std::string(stats->distance_type) == "cosine");
    CHECK(stats->num_indexed_rows == 256);
    CHECK(stats->num_unindexed_rows == 0);
    REQUIRE(stats->has_num_indices);
    CHECK(stats->num_indices >= 1);
    lancedb_free_index_statistics(stats);

    lancedb_table_free(table);
  }
}
