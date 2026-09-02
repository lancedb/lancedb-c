/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 *
 * Regression tests for callers with small fixed-size stacks (coroutines,
 * fibers, small pthreads). Every database-executing C entry point must run
 * its work on the runtime's worker threads, never on the caller's stack.
 * Each section runs on a pthread with a 256 KiB stack — well below the
 * ~552 KB a merge_insert needs when polled on the caller's thread (see
 * examples/asio_coroutine.cpp) — so it crashes on the old block_on design
 * and passes with the spawn-hop.
 */

#include <pthread.h>
#include <cstring>
#include <memory>
#include <string>
#include "test_common.h"

namespace {

constexpr size_t SMALL_STACK_BYTES = 256 * 1024;

struct StackJob {
  void (*fn)(StackJob&);
  LanceDBTable* table;
  LanceDBError result = LANCEDB_UNKNOWN;
  char* error_message = nullptr;
  uint64_t count = 0;
};

extern "C" void* stack_job_trampoline(void* p) {
  auto* job = static_cast<StackJob*>(p);
  try {
    job->fn(*job);
  } catch (...) {
    job->result = LANCEDB_UNKNOWN;
  }
  return nullptr;
}

std::string error_text(const StackJob& job) {
  return job.error_message ? std::string(job.error_message) : std::string("(no error message)");
}

// Run `job` on a thread whose stack is only SMALL_STACK_BYTES.
void run_on_small_stack(StackJob& job) {
  pthread_attr_t attr;
  REQUIRE(pthread_attr_init(&attr) == 0);
  REQUIRE(pthread_attr_setstacksize(&attr, SMALL_STACK_BYTES) == 0);
  pthread_t thread;
  REQUIRE(pthread_create(&thread, &attr, stack_job_trampoline, &job) == 0);
  REQUIRE(pthread_join(thread, nullptr) == 0);
  pthread_attr_destroy(&attr);
}

void merge_insert_job(StackJob& job) {
  auto schema = create_test_schema();
  arrow::StringBuilder key_builder;
  arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
      std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);
  for (int i = 0; i < 5; i++) {
    key_builder.Append("key_" + std::to_string(i + 100)).ok();
    auto* fb = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
      fb->Append(static_cast<float>(i)).ok();
    }
    data_builder.Append().ok();
  }
  std::shared_ptr<arrow::Array> key_array, data_array;
  key_builder.Finish(&key_array).ok();
  data_builder.Finish(&data_array).ok();
  auto batch = arrow::RecordBatch::Make(schema, 5, {key_array, data_array});
  auto reader = create_reader_from_batch(batch);

  const char* on_columns[] = {"key"};
  LanceDBMergeInsertConfig config = {
    .when_matched_update_all = 1,
    .when_not_matched_insert_all = 1
  };
  job.result = lancedb_table_merge_insert(job.table, reader, on_columns, 1, &config, &job.error_message);
  job.count = lancedb_table_count_rows(job.table);
}

void nested_sql_delete_job(StackJob& job) {
  // A deeply nested predicate exercises sqlparser's recursive descent.
  std::string predicate = "key = 'key_0'";
  for (int i = 0; i < 20; i++) {
    predicate = "(" + predicate + " OR key = 'no_such_key_" + std::to_string(i) + "')";
  }
  job.result = lancedb_table_delete(job.table, predicate.c_str(), &job.error_message);
  job.count = lancedb_table_count_rows(job.table);
}

void vector_search_job(StackJob& job) {
  float query[TEST_SCHEMA_DIMENSIONS];
  for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) query[j] = 1.0f;
  struct FFI_ArrowArray** arrays = nullptr;
  struct FFI_ArrowSchema* schema = nullptr;
  size_t count = 0;
  job.result = lancedb_table_nearest_to(job.table, query, TEST_SCHEMA_DIMENSIONS, 3, "data",
                                        &arrays, &schema, &count, &job.error_message);
  job.count = count;
  if (arrays) lancedb_free_arrow_arrays(arrays, count);
  if (schema) lancedb_free_arrow_schema(schema);
}

void scalar_index_job(StackJob& job) {
  const char* columns[] = {"key"};
  job.result = lancedb_table_create_scalar_index(job.table, columns, 1, LANCEDB_INDEX_BTREE,
                                                 nullptr, &job.error_message);
}

void metadata_roundtrip_job(StackJob& job) {
  const char* keys[] = {"owner", "purpose"};
  const char* values[] = {"stack-test", "regression"};
  job.result = lancedb_table_set_metadata(job.table, keys, values, 2, &job.error_message);
  if (job.result != LANCEDB_SUCCESS) return;

  char** got_keys = nullptr;
  char** got_values = nullptr;
  size_t count = 0;
  const char* filter[] = {"purpose"};
  job.result = lancedb_table_get_metadata(job.table, filter, 1, &got_keys, &got_values, &count,
                                          &job.error_message);
  if (job.result != LANCEDB_SUCCESS) return;
  job.count = count;
  if (count != 1 || std::strcmp(got_keys[0], "purpose") != 0 ||
      std::strcmp(got_values[0], "regression") != 0) {
    job.result = LANCEDB_UNKNOWN;
  }
  lancedb_free_metadata(got_keys, got_values, count);
  if (job.result != LANCEDB_SUCCESS) return;

  const char* del[] = {"owner"};
  job.result = lancedb_table_delete_metadata(job.table, del, 1, &job.error_message);
}

}  // namespace

TEST_CASE_METHOD(LanceDBFixture, "LanceDB operations from a 256 KiB stack", "[stack]") {
  constexpr int row_num = 10;
  LanceDBTable* table = create_table_with_data("stack_test", row_num, 0);
  REQUIRE(table != nullptr);

  SECTION("merge insert") {
    StackJob job{merge_insert_job, table};
    run_on_small_stack(job);
    INFO(error_text(job));
    REQUIRE(job.result == LANCEDB_SUCCESS);
    REQUIRE(job.count == row_num + 5);
  }

  SECTION("nested SQL delete") {
    StackJob job{nested_sql_delete_job, table};
    run_on_small_stack(job);
    INFO(error_text(job));
    REQUIRE(job.result == LANCEDB_SUCCESS);
    REQUIRE(job.count == row_num - 1);
  }

  SECTION("vector search") {
    StackJob job{vector_search_job, table};
    run_on_small_stack(job);
    INFO(error_text(job));
    REQUIRE(job.result == LANCEDB_SUCCESS);
    REQUIRE(job.count >= 1);
  }

  SECTION("scalar index creation") {
    StackJob job{scalar_index_job, table};
    run_on_small_stack(job);
    INFO(error_text(job));
    REQUIRE(job.result == LANCEDB_SUCCESS);
  }

  SECTION("table metadata round trip") {
    StackJob job{metadata_roundtrip_job, table};
    run_on_small_stack(job);
    INFO(error_text(job));
    REQUIRE(job.result == LANCEDB_SUCCESS);
    REQUIRE(job.count == 1);
  }

  lancedb_table_free(table);
}
