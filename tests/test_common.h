/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#pragma once

#include <catch2/catch.hpp>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include "lancedb.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <sstream>
#include <chrono>
#include <string>
#include <vector>

// Helpers compiled as C. The C++ tests cannot create an out-of-range C enum
// value without undefined behavior, but a C caller can pass one through the
// public header. These helpers do that and let the tests check the result.
extern "C" {
LanceDBError lancedb_test_create_vector_index_with_index_type(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    int index_type,
    char** error_message);
LanceDBError lancedb_test_create_vector_index_with_distance_type(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    int distance_type,
    char** error_message);
LanceDBError lancedb_test_create_index_with_index_type(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    int index_type,
    char** error_message);
LanceDBError lancedb_test_create_scalar_index_with_index_type(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    int index_type,
    char** error_message);
LanceDBError lancedb_test_optimize_with_type(
    const LanceDBTable* table,
    int optimize_type,
    char** error_message);
LanceDBError lancedb_test_set_distance_type(
    LanceDBVectorQuery* query,
    int distance_type,
    char** error_message);
LanceDBError lancedb_test_create_branch_with_source_kind(
    const LanceDBTable* table,
    const char* name,
    int source_kind,
    const char* source_name,
    int has_version,
    uint64_t version,
    LanceDBTable** table_out,
    char** error_message);
LanceDBError lancedb_test_set_normalize_method(
    LanceDBVectorQuery* query,
    int normalize_method,
    char** error_message);
LanceDBExpr* lancedb_test_expr_binary(
    LanceDBExpr* left,
    int op,
    LanceDBExpr* right);
LanceDBError lancedb_test_fts_query_new_match_with_operator(
    const char* text,
    const char* column,
    int operator_value,
    LanceDBFtsQuery** query_out,
    char** error_message);
}

// Helper function to check if directory exists
int directory_exists(const char* path);

// Helper function to remove directory recursively
int remove_directory(const char* path);

// Create an empty table on any connection, including a namespace connection
void create_empty_table_on(LanceDBConnection* db, const std::string& table_name);

// Open an existing table on any connection and require success
LanceDBTable* open_table_on(LanceDBConnection* db, const std::string& table_name);

// Require that a table cannot be opened on any connection because it is missing
void require_table_not_found_on(LanceDBConnection* db, const std::string& table_name);

class BaseFixture {
protected:
  const std::string data_dir;
  const std::string uri;

  static std::string generate_random_dir() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    std::ostringstream oss;
    oss << "test_data_" << timestamp << "_" << dis(gen);
    return oss.str();
  }

public:
  BaseFixture() : data_dir(generate_random_dir()), uri(data_dir + "/test-lancedb") {
    if (directory_exists(data_dir.c_str())) {
      remove_directory(data_dir.c_str());
    }
  }

  ~BaseFixture() {
    if (directory_exists(data_dir.c_str())) {
      remove_directory(data_dir.c_str());
    } else {
      // Remove regular file (e.g. blocker file from connect failure test)
      std::remove(data_dir.c_str());
    }
  }
};

// Test fixture for LanceDB connection
class LanceDBFixture : public BaseFixture {
protected:
  LanceDBConnection* db = nullptr;

public:
  LanceDBFixture() {
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    REQUIRE(lancedb_connect_builder_execute(builder, &db, nullptr) == LANCEDB_SUCCESS);
    REQUIRE(db != nullptr);
  }

  ~LanceDBFixture() {
    lancedb_connection_free(db);
  }

  void create_empty_table(const std::string& table_name);
  LanceDBTable* create_table_with_data(const std::string& table_name, int num_rows, int start_index);
  // Open an existing table, requiring the table to be opened successfully
  LanceDBTable* open_table(const std::string& table_name);
  // Require that a table cannot be opened because it does not exist
  void require_table_not_found(const std::string& table_name);
};

class LanceDBSessionFixture : public LanceDBFixture {
  protected:
    LanceDBSession* session = nullptr;
    LanceDBSessionOptions session_options = {.index_cache_bytes = 1024 * 1024, .metadata_cache_bytes = 512 * 1024};
  public:
    LanceDBSessionFixture() {
      lancedb_connection_free(db);
      LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
      REQUIRE(builder != nullptr);
      session = lancedb_session_new(&session_options);
      REQUIRE(session != nullptr);
      builder = lancedb_connect_builder_session(builder, session);
      char* error_message = nullptr;
      REQUIRE(lancedb_connect_builder_execute(builder, &db, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
      REQUIRE(db != nullptr);
    }

    ~LanceDBSessionFixture() {
      lancedb_session_free(session);
    }
};

// Test schema dimensions constant
constexpr size_t TEST_SCHEMA_DIMENSIONS = 8;

// Helper function to create the standard test schema (key, data)
std::shared_ptr<arrow::Schema> create_test_schema();

// Helper function to create a record batch with test data
std::shared_ptr<arrow::RecordBatch> create_test_record_batch(int num_rows, int start_index);

// Helper function to create RecordBatchReader from RecordBatch
LanceDBRecordBatchReader* create_reader_from_batch(const std::shared_ptr<arrow::RecordBatch>& batch);

// Contents of an Arrow IPC file read back with the Arrow C++ reader
struct IpcFileContents {
  std::shared_ptr<arrow::Schema> schema;
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
};

// Helper function to write an Arrow IPC file with the given batches
std::vector<uint8_t> write_ipc_file(
    const std::shared_ptr<arrow::Schema>& schema,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches);

// Helper function to write an Arrow IPC stream, which is not an IPC file
std::vector<uint8_t> write_ipc_stream(
    const std::shared_ptr<arrow::Schema>& schema,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches);

// Helper function to write a two-batch IPC file whose second batch metadata is
// corrupt: the footer and the first batch stay readable, the second batch does not
std::vector<uint8_t> write_ipc_file_with_corrupt_second_batch(
    const std::shared_ptr<arrow::Schema>& schema,
    const std::shared_ptr<arrow::RecordBatch>& first,
    const std::shared_ptr<arrow::RecordBatch>& second);

// Helper function to write an Arrow IPC file whose field name cannot be carried
// by the Arrow C data interface, because it contains a NUL byte
std::vector<uint8_t> write_ipc_file_with_unrepresentable_field_name();

// Helper function to write an Arrow IPC file with a dictionary-encoded column,
// so the file carries dictionary blocks beside its record batches
std::vector<uint8_t> write_ipc_file_with_dictionary_column();

// Helper function to write an Arrow IPC file whose announced footer length is
// valid for the input but whose footer bytes are not a footer
std::vector<uint8_t> write_ipc_file_with_unreadable_footer();

// Helper function to write an Arrow IPC file whose footer parses but whose
// declared record blocks no longer fit the shortened input
std::vector<uint8_t> write_ipc_file_with_blocks_outside_the_file();

// Helper function to read an Arrow IPC file with the Arrow C++ reader
IpcFileContents read_ipc_file(const uint8_t* data, size_t len);

// Helper function to read every value of a string column, sorted, so that a
// check does not depend on the order the query returned the batches in
std::vector<std::string> read_ipc_file_strings(const IpcFileContents& file, int column_index);

// Helper function to join strings with a comma, so that a test can compare them
// in a single assertion
std::string join_strings(const std::vector<std::string>& values);

