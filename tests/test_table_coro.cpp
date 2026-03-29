/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <vector>
#include <tuple>
#include <string>
#include <thread>
#include <boost/version.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/spawn.hpp>
#if BOOST_VERSION >= 108400
#include <boost/asio/detached.hpp>
#include <boost/context/fixedsize_stack.hpp>
#else
#include <boost/coroutine/attributes.hpp>
#endif
#include "test_common.h"

// the default coroutine stack is not enough when block_on runs inside a coroutine
constexpr size_t CORO_STACK_SIZE = 8 * 1024 * 1024; // 8 MB

// Helper to spawn a coroutine with a custom stack size across Boost versions
template <typename Executor, typename F>
void spawn_with_stack(Executor& ex, F&& f) {
#if BOOST_VERSION >= 108400
    boost::asio::spawn(ex, boost::asio::allocator_arg_t{},
        boost::context::fixedsize_stack(CORO_STACK_SIZE),
        std::forward<F>(f), boost::asio::detached);
#else
    boost::asio::spawn(ex, std::forward<F>(f),
        boost::coroutines::attributes(CORO_STACK_SIZE));
#endif
}

using AsyncResult = std::tuple<LanceDBError, char*>;

TEST_CASE_METHOD(BaseFixture, "LanceDB Coroutine Table Add", "[table][coro]") {
  // program crash when asio coroutines are used with the global tokio runtime
  LanceDBRuntime* runtime = nullptr; // lancedb_runtime_new();
  //REQUIRE(runtime != nullptr);

  // Create connection with explicit runtime
  LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
  REQUIRE(builder != nullptr);
  LanceDBConnection* db = lancedb_connect_builder_execute(builder, runtime);
  REQUIRE(db != nullptr);

  // Create table with data using explicit runtime
  const std::string table_name = "test_coro_add";
  constexpr auto initial_rows = 10;

  auto schema = create_test_schema();
  auto batch = create_test_record_batch(initial_rows, 0);
  auto reader = create_reader_from_batch(batch);
  REQUIRE(reader != nullptr);

  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create(
      db,
      table_name.c_str(),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      reader,
      &table,
      runtime,
      &error_message
  );
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);

  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  REQUIRE(lancedb_table_count_rows(table, runtime) == initial_rows);

  constexpr int num_threads = 4;

  SECTION("Multiple batched adds via boost::asio::spawn") {
    constexpr int num_batches = 8;
    constexpr int rows_per_batch = 10;

    boost::asio::io_context io;
    auto work_guard = boost::asio::make_work_guard(io);
    std::vector<AsyncResult> results(num_batches);

    // Start threads first so they are waiting for work
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
      threads.emplace_back([&io]() { io.run(); });
    }

    // Spawn coroutines — threads will pick them up concurrently
    for (int i = 0; i < num_batches; i++) {
      int start_index = initial_rows + i * rows_per_batch;
      spawn_with_stack(io, [&table, runtime, &results, i, start_index](boost::asio::yield_context) {
        char* error_message = nullptr;
        auto batch = create_test_record_batch(rows_per_batch, start_index);
        auto reader = create_reader_from_batch(batch);
        auto error = lancedb_table_add(table, reader, runtime, &error_message);
        results[i] = {error, error_message};
      });
    }

    // Release the guard so threads can finish once all coroutines complete
    work_guard.reset();
    for (auto& t : threads) {
      t.join();
    }

    for (const auto& [error, error_message] : results) {
      REQUIRE(error == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }

    auto total_rows = lancedb_table_count_rows(table, runtime);
    REQUIRE(total_rows == initial_rows + num_batches * rows_per_batch);
  }

  SECTION("Multiple batched merge inserts with overlapping keys via boost::asio::spawn") {
    constexpr int num_batches = 4;
    constexpr int rows_per_batch = 10;
    // Each batch overlaps the previous by half
    constexpr int stride = rows_per_batch / 2;

    boost::asio::io_context io;
    auto work_guard = boost::asio::make_work_guard(io);
    std::vector<AsyncResult> results(num_batches);

    // Start threads first so they are waiting for work
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
      threads.emplace_back([&io]() { io.run(); });
    }

    // Spawn coroutines — threads will pick them up concurrently
    for (int i = 0; i < num_batches; i++) {
      int start_index = initial_rows + i * stride;
      spawn_with_stack(io, [&table, runtime, &results, i, start_index](boost::asio::yield_context) {
        char* error_message = nullptr;
        auto batch = create_test_record_batch(rows_per_batch, start_index);
        auto reader = create_reader_from_batch(batch);

        const char* on_columns[] = {"key"};
        LanceDBMergeInsertConfig config = {
          .when_matched_update_all = 1,
          .when_not_matched_insert_all = 1
        };

        auto error = lancedb_table_merge_insert(
            table, reader, on_columns, 1, &config, runtime, &error_message);
        results[i] = {error, error_message};
      });
    }

    // Release the guard so threads can finish once all coroutines complete
    work_guard.reset();
    for (auto& t : threads) {
      t.join();
    }

    for (const auto& [error, error_message] : results) {
      REQUIRE(error == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }

    // Concurrent merge inserts don't see each other's in-flight changes,
    // so overlapping keys between batches are not deduplicated
    auto total_rows = lancedb_table_count_rows(table, runtime);
    REQUIRE(total_rows == initial_rows + num_batches * rows_per_batch);
  }

  lancedb_table_free(table);
  lancedb_connection_free(db);
  lancedb_runtime_free(runtime);
}
