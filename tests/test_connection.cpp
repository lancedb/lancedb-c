/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"
#include <set>

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Connection", "[connection]") {
  SECTION("Connect to a database and get the URI") {
    REQUIRE(db != nullptr);
    const char* connected_uri = lancedb_connection_uri(db);
    REQUIRE(connected_uri != nullptr);
    REQUIRE(std::string(connected_uri) == uri);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Tables", "[connection]") {
  constexpr size_t num_tables = 20;
  for (size_t i = 0; i < num_tables; ++i) {
    create_empty_table("table_" + std::to_string(i));
  }
  const char* _namespace = nullptr;
  char** names_out = nullptr;
  size_t count_out = 0;
  char* error_message = nullptr;
  auto result = lancedb_connection_table_names(db, &names_out, &count_out, &error_message);
  REQUIRE(error_message == nullptr);
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(count_out == num_tables);

  SECTION("List Tables") {
    std::set<std::string> table_names;
    for (size_t i = 0; i < count_out; ++i) {
      table_names.insert(std::string(names_out[i]));
    }
    for (size_t i = 0; i < num_tables; ++i) {
      REQUIRE(table_names.find("table_" + std::to_string(i)) != table_names.end());
    }
  }
  SECTION("Open Tables") {
    for (size_t i = 0; i < count_out; ++i) {
      auto tbl = lancedb_connection_open_table(db, names_out[i]);
      REQUIRE(tbl != nullptr);
      lancedb_table_free(tbl);
    }
  }
  SECTION("Drop Tables") {
    for (size_t i = 0; i < count_out; ++i) {
      char* error_message = nullptr;
      auto result = lancedb_connection_drop_table(db, names_out[i], _namespace, &error_message);
      REQUIRE(error_message == nullptr);
      REQUIRE(result == LANCEDB_SUCCESS);
      auto tbl = lancedb_connection_open_table(db, names_out[i]);
      REQUIRE(tbl == nullptr);
    }
  }
  SECTION("Rename Tables (not supported for OSS") {
    for (size_t i = 0; i < count_out; ++i) {
      char* error_message = nullptr;
      const auto new_name = std::string("new_") + names_out[i];
      auto result = lancedb_connection_rename_table(db,
          names_out[i],
          new_name.c_str(),
          _namespace,
          _namespace,
          &error_message);
      REQUIRE(error_message != nullptr);
      lancedb_free_string(error_message);
      REQUIRE(result == LANCEDB_NOT_SUPPORTED);
      auto tbl = lancedb_connection_open_table(db, new_name.c_str());
      REQUIRE(tbl == nullptr);
      tbl = lancedb_connection_open_table(db, names_out[i]);
      REQUIRE(tbl != nullptr);
      lancedb_table_free(tbl);
    }
  }
  SECTION("Drop All Tables") {
    char* error_message = nullptr;
    auto result = lancedb_connection_drop_all_tables(db, _namespace, &error_message);
    REQUIRE(error_message == nullptr);
    REQUIRE(result == LANCEDB_SUCCESS);
    for (size_t i = 0; i < count_out; ++i) {
      auto tbl = lancedb_connection_open_table(db, names_out[i]);
      REQUIRE(tbl == nullptr);
    }
  }
  lancedb_free_table_names(names_out, count_out);
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Namespaces", "[connection]") {
  char* error_message = nullptr;
  const char* _namespace = "myspace";
  auto result = lancedb_connection_create_namespace(db, _namespace, &error_message);
  REQUIRE(error_message != nullptr);
  REQUIRE(result == LANCEDB_NOT_SUPPORTED);
  lancedb_free_string(error_message);

  SECTION("List Namespaces") {
    char* error_message = nullptr;
    char** names_out = nullptr;
    size_t count_out = 0;
    auto result = lancedb_connection_list_namespaces(db,
        _namespace,
        &names_out,
        &count_out,
        &error_message);
    REQUIRE(error_message != nullptr);
    REQUIRE(result == LANCEDB_NOT_SUPPORTED);
    REQUIRE(count_out == 0);
    REQUIRE(names_out == nullptr);
    lancedb_free_string(error_message);
    lancedb_free_namespace_list(names_out, count_out);
  }
  SECTION("Drop Namespace") {
    char* error_message = nullptr;
    auto result = lancedb_connection_drop_namespace(db,
        _namespace,
        &error_message);
    REQUIRE(error_message != nullptr);
    REQUIRE(result == LANCEDB_NOT_SUPPORTED);
    lancedb_free_string(error_message);
  }
}

