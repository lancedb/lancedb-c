/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

// Example: using Boost.Asio spawn coroutines to structure
// LanceDB upsert, delete, and query operations as coroutine tasks.
// No real async I/O or yielding — the coroutine bodies run
// synchronously, but the skeleton shows how to integrate
// LanceDB operations into an asio::spawn-based architecture.

#include <filesystem>
#include <iostream>
#include <array>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/version.hpp>
#if BOOST_VERSION >= 108000
#include <boost/context/protected_fixedsize_stack.hpp>
#include <boost/asio/detached.hpp>
#else
#include <boost/coroutine/attributes.hpp>
#endif
#include "lancedb.h"

constexpr size_t COROUTINE_STACK_SIZE = 512 * 1024;
// according to measurements, the following stack sizes are used by the different operations:
// [upsert] merge_insert used ~552KB of stack
// [delete] df_delete used ~377KB of stack
// [delete_sql] delete used ~498KB of stack
// [query] vector_query_execute used ~218KB of stack
// however, since we allow the stack to grow dynamically, using 512KB should be sufficient

constexpr size_t DIM = 8;

auto create_schema() {
  return arrow::schema({
    arrow::field("id", arrow::int32()),
    arrow::field("vector", arrow::fixed_size_list(arrow::float32(), DIM)),
  });
}

std::shared_ptr<arrow::RecordBatch> make_records(
    const std::vector<int32_t>& ids) {
  auto schema = create_schema();

  arrow::Int32Builder id_builder;
  arrow::FixedSizeListBuilder vec_builder(
      arrow::default_memory_pool(),
      std::make_unique<arrow::FloatBuilder>(), DIM);

  for (auto id : ids) {
    id_builder.Append(id).ok();
    auto* fb = static_cast<arrow::FloatBuilder*>(vec_builder.value_builder());
    for (size_t j = 0; j < DIM; j++) {
      fb->Append(static_cast<float>(rand() % 100)).ok();
    }
    vec_builder.Append().ok();
  }

  std::shared_ptr<arrow::Array> id_arr, vec_arr;
  id_builder.Finish(&id_arr).ok();
  vec_builder.Finish(&vec_arr).ok();

  return arrow::RecordBatch::Make(schema, static_cast<int64_t>(ids.size()),
                                  {id_arr, vec_arr});
}

LanceDBRecordBatchReader* batch_to_reader(
    const std::shared_ptr<arrow::RecordBatch>& batch) {
  struct ArrowArray c_array;
  struct ArrowSchema c_schema;
  if (!arrow::ExportRecordBatch(*batch, &c_array, &c_schema).ok()) {
    return nullptr;
  }

  LanceDBRecordBatchReader* reader = nullptr;
  if (lancedb_record_batch_reader_from_arrow(
          reinterpret_cast<FFI_ArrowArray*>(&c_array),
          reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
          &reader, nullptr) != LANCEDB_SUCCESS) {
    if (c_array.release) c_array.release(&c_array);
    reader = nullptr;
  }
  if (c_schema.release) c_schema.release(&c_schema);
  return reader;
}

void do_upsert(LanceDBTable* tbl, int num_vectors, boost::asio::yield_context /*yield*/) {
  try {
    const int upsert_count = num_vectors / 2;
    std::cout << "[upsert] building records (ids 0.." << upsert_count - 1
              << " with updated vectors)" << std::endl;
    std::vector<int32_t> upsert_ids(upsert_count);
    std::iota(upsert_ids.begin(), upsert_ids.end(), 0);
    auto batch = make_records(upsert_ids);
    auto* reader = batch_to_reader(batch);
    if (!reader) {
      std::cerr << "[upsert] failed to create reader" << std::endl;
      return;
    }

    std::array<const char*, 1> on_columns = {"id"};
    const LanceDBMergeInsertConfig config{
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr,
    };

    char* err = nullptr;
    if (auto rc = lancedb_table_merge_insert(
            tbl, reader, on_columns.data(), 1, &config, &err);
        rc != LANCEDB_SUCCESS) {
      std::cerr << "[upsert] error: " << lancedb_error_to_message(rc);
      if (err) { std::cerr << " — " << err; lancedb_free_string(err); }
      std::cerr << std::endl;
      return;
    }

    std::cout << "[upsert] done, row count = "
              << lancedb_table_count_rows(tbl) << std::endl;
  } catch (...) {
    std::cerr << "[upsert] exception" << std::endl;
  }
}

void do_delete(LanceDBTable* tbl, int num_vectors, boost::asio::yield_context /*yield*/) {
  try {
    const int delete_count = num_vectors / 5;
    const int start_id = num_vectors - delete_count;

    std::vector<LanceDBExpr*> id_literals(delete_count);
    for (int i = 0; i < delete_count; i++) {
      id_literals[i] = lancedb_expr_literal_i64(start_id + i);
    }
    auto* expr = lancedb_expr_in_list(
        lancedb_expr_column("id"),
        id_literals.data(), static_cast<size_t>(delete_count), false, nullptr);
    if (!expr) {
      std::cerr << "[delete] failed to build expression" << std::endl;
      return;
    }

    std::cout << "[delete] deleting ids " << start_id << ".."
              << num_vectors - 1 << std::endl;

    char* err = nullptr;
    if (auto rc = lancedb_table_df_delete(tbl, expr, &err);
        rc != LANCEDB_SUCCESS) {
      std::cerr << "[delete] error: " << lancedb_error_to_message(rc);
      if (err) { std::cerr << " — " << err; lancedb_free_string(err); }
      std::cerr << std::endl;
      return;
    }

    std::cout << "[delete] done, row count = "
              << lancedb_table_count_rows(tbl) << std::endl;
  } catch (...) {
    std::cerr << "[delete] exception" << std::endl;
  }
}

void do_delete_sql(LanceDBTable* tbl, int num_vectors, boost::asio::yield_context /*yield*/) {
  try {
    const int delete_count = num_vectors / 5;
    const int start_id = num_vectors - delete_count;

    std::string predicate = "id IN (";
    for (int i = start_id; i < num_vectors; i++) {
      if (i > start_id) predicate += ", ";
      predicate += std::to_string(i);
    }
    predicate += ")";

    std::cout << "[delete_sql] deleting ids " << start_id << ".."
              << num_vectors - 1 << std::endl;

    char* err = nullptr;
    if (auto rc = lancedb_table_delete(tbl, predicate.c_str(), &err);
        rc != LANCEDB_SUCCESS) {
      std::cerr << "[delete_sql] error: " << lancedb_error_to_message(rc);
      if (err) { std::cerr << " — " << err; lancedb_free_string(err); }
      std::cerr << std::endl;
      return;
    }

    std::cout << "[delete_sql] done, row count = "
              << lancedb_table_count_rows(tbl) << std::endl;
  } catch (...) {
    std::cerr << "[delete_sql] exception" << std::endl;
  }
}

void do_query(LanceDBTable* tbl, boost::asio::yield_context /*yield*/) {
  try {
    std::vector<float> query_vec(DIM);
    for (size_t i = 0; i < DIM; i++) {
      query_vec[i] = static_cast<float>(rand() % 100);
    }

    auto* vq = lancedb_vector_query_new(tbl, query_vec.data(), DIM);
    if (!vq) {
      std::cerr << "[query] failed to create vector query" << std::endl;
      return;
    }
    lancedb_vector_query_limit(vq, 5, nullptr);

    auto* result = lancedb_vector_query_execute(vq);
    if (!result) {
      std::cerr << "[query] execute failed" << std::endl;
      return;
    }

    FFI_ArrowArray** arrays = nullptr;
    FFI_ArrowSchema* schema = nullptr;
    size_t count = 0;
    char* err = nullptr;
    if (auto rc = lancedb_query_result_to_arrow(result, &arrays, &schema, &count, &err);
        rc != LANCEDB_SUCCESS) {
      std::cerr << "[query] result_to_arrow error: " << lancedb_error_to_message(rc);
      if (err) { std::cerr << " — " << err; lancedb_free_string(err); }
      std::cerr << std::endl;
      return;
    }

    auto imported_schema = arrow::ImportSchema(
        reinterpret_cast<ArrowSchema*>(schema));
    lancedb_free_arrow_schema(schema);
    if (!imported_schema.ok()) {
      std::cerr << "[query] failed to import schema" << std::endl;
      if (arrays) lancedb_free_arrow_arrays(arrays, count);
      return;
    }

    size_t total_rows = 0;
    for (size_t i = 0; i < count; i++) {
      auto imported = arrow::ImportRecordBatch(
          reinterpret_cast<ArrowArray*>(arrays[i]),
          *imported_schema);
      if (imported.ok()) {
        total_rows += (*imported)->num_rows();
      }
    }

    if (arrays) lancedb_free_arrow_arrays(arrays, count);

    std::cout << "[query] found " << total_rows << " rows in "
              << count << " batches" << std::endl;
  } catch (...) {
    std::cerr << "[query] exception" << std::endl;
  }
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <num_vectors>" << std::endl;
    return 1;
  }
  const int num_vectors = std::atoi(argv[1]);
  if (num_vectors < 5) {
    std::cerr << "num_vectors must be at least 5" << std::endl;
    return 1;
  }

  const std::string data_dir = "data";
  if (std::filesystem::is_directory(data_dir)) {
    std::filesystem::remove_all(data_dir);
  }

  // connect
  auto* builder = lancedb_connect((data_dir + "/asio-example").c_str());
  if (!builder) { std::cerr << "connect builder failed" << std::endl; return 1; }
  LanceDBConnection* db = nullptr;
  char* error_msg = nullptr;
  if (lancedb_connect_builder_execute(builder, &db, &error_msg) != LANCEDB_SUCCESS) {
    std::cerr << "connect failed: " << (error_msg ? error_msg : "unknown") << std::endl;
    lancedb_free_string(error_msg);
    return 1;
  }

  // create table with initial data
  std::cout << "num_vectors = " << num_vectors << std::endl;
  std::vector<int32_t> initial_ids(num_vectors);
  std::iota(initial_ids.begin(), initial_ids.end(), 0);
  auto initial = make_records(initial_ids);
  struct ArrowSchema c_schema;
  struct ArrowArray c_array;
  if (!arrow::ExportRecordBatch(*initial, &c_array, &c_schema).ok()) {
    std::cerr << "ExportRecordBatch failed" << std::endl;
    lancedb_connection_free(db);
    return 1;
  }

  LanceDBRecordBatchReader* init_reader = nullptr;
  if (lancedb_record_batch_reader_from_arrow(
          reinterpret_cast<FFI_ArrowArray*>(&c_array),
          reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
          &init_reader, nullptr) != LANCEDB_SUCCESS) {
    std::cerr << "record_batch_reader_from_arrow failed" << std::endl;
    if (c_array.release) c_array.release(&c_array);
    if (c_schema.release) c_schema.release(&c_schema);
    lancedb_connection_free(db);
    return 1;
  }

  LanceDBTable* tbl = nullptr;
  if (lancedb_table_create(db, "demo",
          reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
          init_reader, &tbl, nullptr) != LANCEDB_SUCCESS) {
    std::cerr << "table create failed" << std::endl;
    if (c_schema.release) c_schema.release(&c_schema);
    lancedb_connection_free(db);
    return 1;
  }
  if (c_schema.release) c_schema.release(&c_schema);

  std::cout << "created table with " << lancedb_table_count_rows(tbl)
            << " rows" << std::endl;

  // run upsert, delete, and query as asio spawn coroutines
  boost::asio::io_context io;

#if BOOST_VERSION >= 108000
  auto stack_alloc = boost::context::protected_fixedsize_stack(COROUTINE_STACK_SIZE);

  boost::asio::spawn(io, std::allocator_arg, stack_alloc,
    [tbl, num_vectors](boost::asio::yield_context yield) {
      std::cout << "--- all operations in one coroutine ---" << std::endl;
      do_upsert(tbl, num_vectors, yield);
      do_delete(tbl, num_vectors, yield);
      do_delete_sql(tbl, num_vectors, yield);
      do_query(tbl, yield);
    }, boost::asio::detached);

  boost::asio::spawn(io, std::allocator_arg, stack_alloc,
    [tbl, num_vectors](boost::asio::yield_context yield) {
      std::cout << "--- per-operation coroutines ---" << std::endl;
      do_upsert(tbl, num_vectors, yield);
    }, boost::asio::detached);

  boost::asio::spawn(io, std::allocator_arg, stack_alloc,
    [tbl, num_vectors](boost::asio::yield_context yield) {
      do_delete(tbl, num_vectors, yield);
    }, boost::asio::detached);

  boost::asio::spawn(io, std::allocator_arg, stack_alloc,
    [tbl, num_vectors](boost::asio::yield_context yield) {
      do_delete_sql(tbl, num_vectors, yield);
    }, boost::asio::detached);

  boost::asio::spawn(io, std::allocator_arg, stack_alloc,
    [tbl](boost::asio::yield_context yield) {
      do_query(tbl, yield);
    }, boost::asio::detached);
#else
  boost::coroutines::attributes attrs(COROUTINE_STACK_SIZE);

  boost::asio::spawn(io, [tbl, num_vectors](boost::asio::yield_context yield) {
    std::cout << "--- all operations in one coroutine ---" << std::endl;
    do_upsert(tbl, num_vectors, yield);
    do_delete(tbl, num_vectors, yield);
    do_delete_sql(tbl, num_vectors, yield);
    do_query(tbl, yield);
  }, attrs);

  boost::asio::spawn(io, [tbl, num_vectors](boost::asio::yield_context yield) {
    std::cout << "--- per-operation coroutines ---" << std::endl;
    do_upsert(tbl, num_vectors, yield);
  }, attrs);

  boost::asio::spawn(io, [tbl, num_vectors](boost::asio::yield_context yield) {
    do_delete(tbl, num_vectors, yield);
  }, attrs);

  boost::asio::spawn(io, [tbl, num_vectors](boost::asio::yield_context yield) {
        do_delete_sql(tbl, num_vectors, yield);
  }, attrs);

  boost::asio::spawn(io, [tbl](boost::asio::yield_context yield) {
    do_query(tbl, yield);
  }, attrs);
#endif

  io.run();

  std::cout << "final row count = " << lancedb_table_count_rows(tbl) << std::endl;

  lancedb_table_free(tbl);
  lancedb_connection_drop_table(db, "demo", nullptr, nullptr);
  lancedb_connection_free(db);
  return 0;
}

