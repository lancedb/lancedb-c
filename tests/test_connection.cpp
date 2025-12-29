/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"
#include <set>

static const char* NON_UTF8 = "\x80\xFF\xFE\xAB";

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Connection", "[connection]") {
  SECTION("Connect to a database and get the URI") {
    REQUIRE(db != nullptr);
    const char* connected_uri = lancedb_connection_uri(db);
    REQUIRE(connected_uri != nullptr);
    REQUIRE(std::string(connected_uri) == uri);
  }
}

TEST_CASE_METHOD(BaseFixture, "LanceDB Connection Builder", "[connection]") {
  SECTION("Use connection builder to set options") {
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    builder = lancedb_connect_builder_storage_option(builder, "hello", "world");
    REQUIRE(builder != nullptr);
    LanceDBConnection* db = lancedb_connect_builder_execute(builder);
    REQUIRE(db != nullptr);
    lancedb_connection_free(db);
  }
  SECTION("Free connection builder") {
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    lancedb_connect_builder_free(builder);
  }
  SECTION("NULL connection builder should fail") {
    LanceDBConnectBuilder* builder = lancedb_connect_builder_storage_option(nullptr, "hello", "world");
    REQUIRE(builder == nullptr);
  }
  SECTION("NULL option name should fail") {
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    builder = lancedb_connect_builder_storage_option(builder, nullptr, "world");
    REQUIRE(builder == nullptr);
  }
  SECTION("NULL option value should fail") {
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    builder = lancedb_connect_builder_storage_option(builder, "hello", nullptr);
    REQUIRE(builder == nullptr);
  }
  SECTION("Invalid option name should fail") {
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    builder = lancedb_connect_builder_storage_option(builder, NON_UTF8, "world");
    REQUIRE(builder == nullptr);
  }
  SECTION("Invalid option value should fail") {
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    builder = lancedb_connect_builder_storage_option(builder, "hello", NON_UTF8);
    REQUIRE(builder == nullptr);
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

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Names Builder", "[connection]") {
  // Create test tables with predictable names for pagination testing
  constexpr size_t num_tables = 20;
  for (size_t i = 0; i < num_tables; ++i) {
    create_empty_table("table_" + std::to_string(i));
  }

  SECTION("Basic builder usage") {
    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);

    char** names_out = nullptr;
    size_t count_out = 0;
    char* error_message = nullptr;
    auto result = lancedb_table_names_builder_execute(builder, &names_out, &count_out, &error_message);

    REQUIRE(error_message == nullptr);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count_out == num_tables);
    REQUIRE(names_out != nullptr);

    lancedb_free_table_names(names_out, count_out);
  }

  SECTION("Builder with limit") {
    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);

    constexpr unsigned int limit = 5;
    builder = lancedb_table_names_builder_limit(builder, limit);
    REQUIRE(builder != nullptr);

    char** names_out = nullptr;
    size_t count_out = 0;
    char* error_message = nullptr;
    auto result = lancedb_table_names_builder_execute(builder, &names_out, &count_out, &error_message);

    REQUIRE(error_message == nullptr);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count_out == limit);
    REQUIRE(names_out != nullptr);

    lancedb_free_table_names(names_out, count_out);
  }

  SECTION("Builder with start_after for pagination") {
    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);

    builder = lancedb_table_names_builder_start_after(builder, "table_10");
    REQUIRE(builder != nullptr);

    char** names_out = nullptr;
    size_t count_out = 0;
    char* error_message = nullptr;
    auto result = lancedb_table_names_builder_execute(builder, &names_out, &count_out, &error_message);

    REQUIRE(error_message == nullptr);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count_out > 0);
    REQUIRE(names_out != nullptr);

    // Verify all returned tables come after "table_10" alphabetically
    for (size_t i = 0; i < count_out; ++i) {
      REQUIRE(std::string(names_out[i]) > std::string("table_10"));
    }

    lancedb_free_table_names(names_out, count_out);
  }

  SECTION("Builder with start_after set to unknown table") {
    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);

    builder = lancedb_table_names_builder_start_after(builder, "table_999");
    REQUIRE(builder != nullptr);

    char** names_out = nullptr;
    size_t count_out = 0;
    char* error_message = nullptr;
    auto result = lancedb_table_names_builder_execute(builder, &names_out, &count_out, &error_message);

    REQUIRE(error_message == nullptr);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count_out == 0);
    REQUIRE(names_out == nullptr);

    lancedb_free_table_names(names_out, count_out);
  }

  SECTION("Builder with limit and start_after") {
    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);

    constexpr unsigned int limit = 3;
    builder = lancedb_table_names_builder_limit(builder, limit);
    REQUIRE(builder != nullptr);

    builder = lancedb_table_names_builder_start_after(builder, "table_5");
    REQUIRE(builder != nullptr);

    char** names_out = nullptr;
    size_t count_out = 0;
    char* error_message = nullptr;
    auto result = lancedb_table_names_builder_execute(builder, &names_out, &count_out, &error_message);

    REQUIRE(error_message == nullptr);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(count_out <= limit);
    REQUIRE(names_out != nullptr);

    // Verify all returned tables come after "table_5" alphabetically
    for (size_t i = 0; i < count_out; ++i) {
      REQUIRE(std::string(names_out[i]) > std::string("table_5"));
    }

    lancedb_free_table_names(names_out, count_out);
  }

  SECTION("Builder with NULL connection should fail") {
    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(nullptr);
    REQUIRE(builder == nullptr);
  }

  SECTION("Execute with NULL builder should fail") {
    char** names_out = nullptr;
    size_t count_out = 0;
    char* error_message = nullptr;
    auto result = lancedb_table_names_builder_execute(nullptr, &names_out, &count_out, &error_message);

    REQUIRE(result != LANCEDB_SUCCESS);

    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  SECTION("Limit with NULL builder should fail") {
    LanceDBTableNamesBuilder* builder = lancedb_table_names_builder_limit(nullptr, 10);
    REQUIRE(builder == nullptr);
  }

  SECTION("Start after with NULL builder should fail") {
    LanceDBTableNamesBuilder* builder = lancedb_table_names_builder_start_after(nullptr, "table_0");
    REQUIRE(builder == nullptr);
  }

  SECTION("NULL start after should fail") {
    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);
    builder = lancedb_table_names_builder_start_after(builder, nullptr);
    REQUIRE(builder == nullptr);
  }

  SECTION("Invalid start after should fail") {
    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);
    builder = lancedb_table_names_builder_start_after(builder, NON_UTF8);
    REQUIRE(builder == nullptr);
  }

  SECTION("Free builder without executing") {
    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);

    builder = lancedb_table_names_builder_limit(builder, 5);
    REQUIRE(builder != nullptr);

    // Free without executing
    lancedb_table_names_builder_free(builder);
    // No crash means success
  }

  SECTION("Pagination through all tables") {
    // Get all table names first to have a reference
    char** all_names = nullptr;
    size_t all_count = 0;
    char* error_message = nullptr;
    auto result = lancedb_connection_table_names(db, &all_names, &all_count, &error_message);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(all_count == num_tables);

    std::set<std::string> all_tables_via_pagination;
    constexpr unsigned int page_size = 7;
    std::string last_table_name = "";

    // Paginate through all tables
    bool has_more = true;
    while (has_more) {
      LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
      REQUIRE(builder != nullptr);

      builder = lancedb_table_names_builder_limit(builder, page_size);
      REQUIRE(builder != nullptr);

      if (!last_table_name.empty()) {
        builder = lancedb_table_names_builder_start_after(builder, last_table_name.c_str());
        REQUIRE(builder != nullptr);
      }

      char** page_names = nullptr;
      size_t page_count = 0;
      char* page_error = nullptr;
      result = lancedb_table_names_builder_execute(builder, &page_names, &page_count, &page_error);

      REQUIRE(result == LANCEDB_SUCCESS);
      REQUIRE(page_error == nullptr);

      if (page_count == 0) {
        has_more = false;
      } else {
        for (size_t i = 0; i < page_count; ++i) {
          REQUIRE(all_tables_via_pagination.insert(std::string(page_names[i])).second);
          last_table_name = page_names[i];
        }
        has_more = (page_count == page_size);
      }

      lancedb_free_table_names(page_names, page_count);
    }

    // Verify we got all tables through pagination
    REQUIRE(all_tables_via_pagination.size() == num_tables);
    for (size_t i = 0; i < all_count; ++i) {
      REQUIRE(all_tables_via_pagination.find(std::string(all_names[i])) != all_tables_via_pagination.end());
    }

    lancedb_free_table_names(all_names, all_count);
  }
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

