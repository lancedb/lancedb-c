/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#pragma once

#include <catch2/catch.hpp>
#include <sys/stat.h>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include "lancedb.h"

// Helper function to check if directory exists
int directory_exists(const char* path);

// Helper function to remove directory recursively
int remove_directory(const char* path);

class BaseFixture {
protected:
  const std::string data_dir;
  const std::string uri;

public:
  BaseFixture() : data_dir("test_data"), uri(data_dir + "/test-lancedb") {
    if (directory_exists(data_dir.c_str())) {
      remove_directory(data_dir.c_str());
    }
  }

  ~BaseFixture() {
    if (directory_exists(data_dir.c_str())) {
      remove_directory(data_dir.c_str());
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
    db = lancedb_connect_builder_execute(builder);
    REQUIRE(db != nullptr);
  }

  ~LanceDBFixture() {
    lancedb_connection_free(db);
  }

  void create_empty_table(const std::string& table_name);
};

// Test schema dimensions constant
constexpr size_t TEST_SCHEMA_DIMENSIONS = 8;

// Helper function to create the standard test schema (key, data)
std::shared_ptr<arrow::Schema> create_test_schema();

// Helper function to create a record batch with test data
std::shared_ptr<arrow::RecordBatch> create_test_record_batch(int num_rows, int start_index);

// Helper function to create RecordBatchReader from RecordBatch
LanceDBRecordBatchReader* create_reader_from_batch(const std::shared_ptr<arrow::RecordBatch>& batch);

