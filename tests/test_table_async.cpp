/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <future>
#include <vector>
#include <tuple>
#include <string>
#include "test_common.h"

using AsyncResult = std::tuple<LanceDBError, char*>;

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Concurrent Table Add", "[table][async]") {
  const std::string table_name = "test_concurrent_add";
  constexpr auto initial_rows = 10;
  LanceDBTable* table = create_table_with_data(table_name, initial_rows, 0);
  REQUIRE(table != nullptr);
  REQUIRE(lancedb_table_count_rows(table, nullptr) == initial_rows);

  SECTION("Multiple batched adds via std::async") {
    constexpr int num_batches = 8;
    constexpr int rows_per_batch = 10;

    std::vector<std::future<AsyncResult>> futures;
    futures.reserve(num_batches);

    for (int i = 0; i < num_batches; i++) {
      int start_index = initial_rows + i * rows_per_batch;
      futures.push_back(std::async(std::launch::async, [&table, start_index]() -> AsyncResult {
        char* error_message = nullptr;
        auto batch = create_test_record_batch(rows_per_batch, start_index);
        auto reader = create_reader_from_batch(batch);
        auto error = lancedb_table_add(table, reader, nullptr, &error_message);
        return {error, error_message};
      }));
    }

    for (auto& f : futures) {
      auto [error, error_message] = f.get();
      REQUIRE(error == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }

    auto total_rows = lancedb_table_count_rows(table, nullptr);
    REQUIRE(total_rows == initial_rows + num_batches * rows_per_batch);
  }

  SECTION("Multiple batched merge inserts with overlapping keys via std::async") {
    constexpr int num_batches = 4;
    constexpr int rows_per_batch = 10;
    // Each batch overlaps the previous by half
    constexpr int stride = rows_per_batch / 2;

    std::vector<std::future<AsyncResult>> futures;
    futures.reserve(num_batches);

    for (int i = 0; i < num_batches; i++) {
      int start_index = initial_rows + i * stride;
      futures.push_back(std::async(std::launch::async, [&table, start_index]() -> AsyncResult {
        char* error_message = nullptr;
        auto batch = create_test_record_batch(rows_per_batch, start_index);
        auto reader = create_reader_from_batch(batch);

        const char* on_columns[] = {"key"};
        LanceDBMergeInsertConfig config = {
          .when_matched_update_all = 1,
          .when_not_matched_insert_all = 1
        };

        auto error = lancedb_table_merge_insert(
            table, reader, on_columns, 1, &config, nullptr, &error_message);
        return {error, error_message};
      }));
    }

    for (auto& f : futures) {
      auto [error, error_message] = f.get();
      REQUIRE(error == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }

    // Concurrent merge inserts don't see each other's in-flight changes,
    // so overlapping keys between batches are not deduplicated
    auto total_rows = lancedb_table_count_rows(table, nullptr);
    REQUIRE(total_rows == initial_rows + num_batches * rows_per_batch);
  }

  lancedb_table_free(table);
}
