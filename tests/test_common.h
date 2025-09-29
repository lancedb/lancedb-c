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

// Test fixture for LanceDB connection
class LanceDBFixture {
protected:
  const std::string data_dir;
  const std::string uri;
  LanceDBConnection* db = nullptr;

public:
  LanceDBFixture() : data_dir("test_data"), uri(data_dir + "/test-lancedb") {
    if (directory_exists(data_dir.c_str())) {
      remove_directory(data_dir.c_str());
    }
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    db = lancedb_connect_builder_execute(builder);
    REQUIRE(db != nullptr);
  }

  ~LanceDBFixture() {
    lancedb_connection_free(db);
    // Cleanup test data
    if (directory_exists(data_dir.c_str())) {
      remove_directory(data_dir.c_str());
    }
  }

  void create_empty_table(const std::string& table_name);
};
