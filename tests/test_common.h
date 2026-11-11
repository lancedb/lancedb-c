/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#pragma once

#include <catch2/catch.hpp>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include "lancedb.h"
#include <random>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

// Helper function to check if directory exists
int directory_exists(const char* path);

// Helper function to remove directory recursively
int remove_directory(const char* path);

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

// Records the events reported by the progress callback of an index optimization
struct IndexProgressRecorder {
  std::mutex mutex;
  std::vector<std::string> started;    // the stages that were started
  std::vector<std::string> completed;  // the stages that were completed
  size_t num_progress = 0;             // the progress reports within the stages
  bool valid = true;                   // no report had a missing stage name or unit

  // The progress callback, expecting the recorder as its user data. The callback may be
  // called from several threads at the same time
  static void callback(LanceDBIndexProgressEvent event, const char* stage, uint64_t, const char* unit, void* user_data) {
    auto* recorder = static_cast<IndexProgressRecorder*>(user_data);
    const std::lock_guard<std::mutex> lock(recorder->mutex);
    if (stage == nullptr || unit == nullptr || std::string(stage).empty()) {
      recorder->valid = false;
      return;
    }
    switch (event) {
      case LANCEDB_INDEX_PROGRESS_STAGE_START:
        recorder->started.emplace_back(stage);
        break;
      case LANCEDB_INDEX_PROGRESS_STAGE_PROGRESS:
        ++recorder->num_progress;
        break;
      case LANCEDB_INDEX_PROGRESS_STAGE_COMPLETE:
        recorder->completed.emplace_back(stage);
        break;
    }
  }

  // Every stage that was started was also completed
  bool all_completed() {
    const std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> sorted_started = started;
    std::vector<std::string> sorted_completed = completed;
    std::sort(sorted_started.begin(), sorted_started.end());
    std::sort(sorted_completed.begin(), sorted_completed.end());
    return sorted_started == sorted_completed;
  }
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

