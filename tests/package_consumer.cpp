/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "lancedb.h"

#include <iostream>
#include <string>

namespace {

bool check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    return false;
  }
  return true;
}

std::string describe(LanceDBError result) {
  return std::string(lancedb_error_to_message(result)) + " (" +
         std::to_string(static_cast<int>(result)) + ")";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "usage: " << argv[0]
              << " <temporary-database-path> [expected-version]" << std::endl;
    return 2;
  }

  const std::string uri = argv[1];
  const std::string expected_version = argc == 3 ? argv[2] : LANCEDB_VERSION_STRING;
  const char* runtime_version = lancedb_version();
  if (!check(runtime_version != nullptr && runtime_version[0] != '\0',
             "lancedb_version() must return a non-empty string") ||
      !check(expected_version == LANCEDB_VERSION_STRING,
             "the packaged header version must match the expected version: " +
                 std::string(LANCEDB_VERSION_STRING) + " != " + expected_version) ||
      !check(expected_version == runtime_version,
             "the loaded library version must match the expected version: " +
                 std::string(runtime_version) + " != " + expected_version)) {
    return 1;
  }
  std::cout << "version: " << runtime_version << std::endl;
  char* error_message = nullptr;
  LanceDBTable* table = nullptr;

  if (!check(lancedb_connect(nullptr) == nullptr,
             "lancedb_connect(NULL) must return NULL")) {
    return 1;
  }

  LanceDBError result = lancedb_connection_open_table(
      nullptr, "missing_table", &table, &error_message);
  if (!check(result == LANCEDB_INVALID_ARGUMENT,
             "opening a table on a NULL connection must fail: " + describe(result)) ||
      !check(table == nullptr, "a failed table open must not return a table") ||
      !check(error_message != nullptr,
             "a failed table open must return an error message")) {
    lancedb_free_string(error_message);
    return 1;
  }
  lancedb_free_string(error_message);
  error_message = nullptr;

  LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
  if (!check(builder != nullptr, "lancedb_connect() must return a builder")) {
    return 1;
  }

  LanceDBConnection* db = nullptr;
  result = lancedb_connect_builder_execute(builder, &db, &error_message);
  if (!check(result == LANCEDB_SUCCESS,
             "connecting to the temporary database failed: " + describe(result)) ||
      !check(db != nullptr, "a successful connect must return a connection")) {
    lancedb_free_string(error_message);
    return 1;
  }

  const char* connected_uri = lancedb_connection_uri(db);
  if (!check(connected_uri != nullptr, "the connection must expose its URI") ||
      !check(uri == connected_uri, "the connection URI must match the requested URI")) {
    lancedb_connection_free(db);
    return 1;
  }
  std::cout << "connected: " << connected_uri << std::endl;

  char** names = nullptr;
  size_t count = 0;
  result = lancedb_connection_table_names(db, &names, &count, &error_message);
  if (!check(result == LANCEDB_SUCCESS,
             "listing tables in the new database failed: " + describe(result)) ||
      !check(count == 0, "a new database must not contain tables") ||
      !check(names == nullptr, "an empty table list must not allocate names")) {
    lancedb_free_table_names(names, count);
    lancedb_free_string(error_message);
    lancedb_connection_free(db);
    return 1;
  }
  lancedb_free_table_names(names, count);

  result = lancedb_connection_open_table(
      db, "missing_table", &table, &error_message);
  if (!check(result == LANCEDB_TABLE_NOT_FOUND,
             "opening a missing table must report LANCEDB_TABLE_NOT_FOUND: " +
                 describe(result)) ||
      !check(table == nullptr, "a failed table open must not return a table") ||
      !check(error_message != nullptr && error_message[0] != '\0',
             "a failed table open must return a non-empty error message")) {
    lancedb_table_free(table);
    lancedb_free_string(error_message);
    lancedb_connection_free(db);
    return 1;
  }
  std::cout << "expected failure: " << error_message << std::endl;
  lancedb_free_string(error_message);
  error_message = nullptr;
  lancedb_connection_free(db);

  builder = lancedb_connect(uri.c_str());
  db = nullptr;
  result = builder ? lancedb_connect_builder_execute(builder, &db, &error_message)
                   : LANCEDB_INVALID_ARGUMENT;
  if (!check(result == LANCEDB_SUCCESS,
             "reopening the temporary database failed: " + describe(result)) ||
      !check(db != nullptr, "a successful reconnect must return a connection")) {
    lancedb_free_string(error_message);
    return 1;
  }
  lancedb_connection_free(db);

  std::cout << "package consumer: OK" << std::endl;
  return 0;
}
