/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"
#include <set>
#include <fstream>
#include <filesystem>

static const char* NON_UTF8 = "\x80\xFF\xFE\xAB";

// the messages are indexed by the error code, so a missing message would go unnoticed
static_assert(sizeof(LANCEDB_ERROR_MESSAGES)/sizeof(LANCEDB_ERROR_MESSAGES[0]) == LANCEDB_EXTERNAL + 1,
    "every error code below LANCEDB_UNKNOWN must have a message");

TEST_CASE("LanceDB Error Messages", "[connection]") {
  SECTION("Error codes are mapped to their own message") {
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_SUCCESS)) == "Success");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_TABLE_NOT_FOUND)) == "Table not found");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_NAMESPACE)) == "Namespace error");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_PERMISSION_DENIED)) == "Permission denied");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_NOT_FOUND)) == "Not found");
  }
  SECTION("SDK error codes added after 0.33 map to their own messages") {
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_TABLE_CORRUPTED)) == "Table corrupted");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_COLUMN_NOT_FOUND)) == "Column not found");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_FIELD_NOT_FOUND)) == "Field not found");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_COLUMN_ALREADY_EXISTS)) == "Column already exists");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_NOT_A_COMPUTED_COLUMN)) == "Not a computed column");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_NOT_A_MATERIALIZED_VIEW)) == "Not a materialized view");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_INVALID_EXPRESSION)) == "Invalid expression");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_SQL)) == "SQL error");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_JOB_FAILED)) == "Job failed");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_JOB_CANCELLED)) == "Job cancelled");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_JOB_NOT_FOUND)) == "Job not found");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_RESPONSE_INTERRUPTED)) == "Response interrupted");
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_EXTERNAL)) == "External error");
  }
  SECTION("The unknown error is kept out of the range of the other codes") {
    REQUIRE(LANCEDB_UNKNOWN > LANCEDB_EXTERNAL);
    REQUIRE(std::string(lancedb_error_to_message(LANCEDB_UNKNOWN)) == "Unknown error");
  }
  SECTION("Codes with no message are reported as invalid") {
    for (const auto invalid : {LANCEDB_EXTERNAL + 1, LANCEDB_UNKNOWN - 1, LANCEDB_UNKNOWN + 1}) {
      REQUIRE(std::string(lancedb_error_to_message(static_cast<LanceDBError>(invalid)))
          == "Invalid error code");
    }
  }
}

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
    builder = lancedb_connect_builder_session(builder, nullptr);
    REQUIRE(builder != nullptr);
    LanceDBConnection* db = nullptr;
    REQUIRE(lancedb_connect_builder_execute(builder, &db, nullptr) == LANCEDB_SUCCESS);
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
  SECTION("Attach session to connect builder") {
    LanceDBSession* session = lancedb_session_new(nullptr);
    REQUIRE(session != nullptr);
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    builder = lancedb_connect_builder_session(builder, session);
    REQUIRE(builder != nullptr);
    LanceDBConnection* db = nullptr;
    REQUIRE(lancedb_connect_builder_execute(builder, &db, nullptr) == LANCEDB_SUCCESS);
    REQUIRE(db != nullptr);
    lancedb_connection_free(db);
    lancedb_session_free(session);
  }
  SECTION("Enterprise connections reject a custom session") {
    LanceDBSession* session = lancedb_session_new(nullptr);
    REQUIRE(session != nullptr);
    LanceDBConnectBuilder* builder = lancedb_connect("db://test");
    REQUIRE(builder != nullptr);
    builder = lancedb_connect_builder_session(builder, session);
    REQUIRE(builder != nullptr);

    LanceDBConnection* db = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_connect_builder_execute(builder, &db, &error_message)
        == LANCEDB_NOT_SUPPORTED);
    REQUIRE(db == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("session is not supported") != std::string::npos);
    lancedb_free_string(error_message);
    lancedb_session_free(session);
  }

  SECTION("NULL session on builder should be allowed and use default session") {
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    builder = lancedb_connect_builder_session(builder, nullptr);
    REQUIRE(builder != nullptr);
    LanceDBConnection* db = nullptr;
    REQUIRE(lancedb_connect_builder_execute(builder, &db, nullptr) == LANCEDB_SUCCESS);
    REQUIRE(db != nullptr);
    lancedb_connection_free(db);
  }
}

TEST_CASE_METHOD(BaseFixture, "LanceDB Session", "[connection]") {
  SECTION("Create and free default session") {
    LanceDBSession* session = lancedb_session_new(nullptr);
    REQUIRE(session != nullptr);
    lancedb_session_free(session);
  }
  SECTION("Create session with options") {
    LanceDBSessionOptions options{};
    options.index_cache_bytes = 1024 * 1024;
    options.metadata_cache_bytes = 2 * 1024 * 1024;
    LanceDBSession* session = lancedb_session_new(&options);
    REQUIRE(session != nullptr);
    lancedb_session_free(session);
  }
  SECTION("Zero options fallback to defaults") {
    LanceDBSessionOptions options{};
    options.index_cache_bytes = 0;
    options.metadata_cache_bytes = 0;
    LanceDBSession* session = lancedb_session_new(&options);
    REQUIRE(session != nullptr);
    lancedb_session_free(session);
  }
  SECTION("Get session cache stats") {
    LanceDBSession* session = lancedb_session_new(nullptr);
    REQUIRE(session != nullptr);
    LanceDBSessionCacheStats index_stats{};
    LanceDBSessionCacheStats metadata_stats{};
    char* error_message = nullptr;
    auto index_result = lancedb_session_index_cache_stats(session, &index_stats, &error_message);
    REQUIRE(index_result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    auto metadata_result = lancedb_session_metadata_cache_stats(session, &metadata_stats, &error_message);
    REQUIRE(metadata_result == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_session_free(session);
  }
  SECTION("Get session cache stats with invalid args should fail") {
    LanceDBSession* session = lancedb_session_new(nullptr);
    REQUIRE(session != nullptr);
    LanceDBSessionCacheStats stats{};
    char* error_message = nullptr;
    auto result = lancedb_session_index_cache_stats(nullptr, &stats, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    result = lancedb_session_metadata_cache_stats(session, nullptr, &error_message);
    REQUIRE(result == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_session_free(session);
  }
  SECTION("Connect failure") {
    // create a regular file at data_dir so that creating the
    // subdirectory "test-lancedb" inside it fails on all platforms.
    std::ofstream blocker(data_dir);
    blocker.close();

    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    LanceDBConnection* db = nullptr;
    char* error_message = nullptr;
    // the directory cannot be created, so the connection fails before the
    // listing database connects to its internal namespace
    REQUIRE(lancedb_connect_builder_execute(builder, &db, &error_message) == LANCEDB_CREATE_DIR);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }
  SECTION("Connect failure of the internal namespace") {
    // the directory already exists, so it is not created again, and the connection
    // gets as far as the "dir" namespace client that the listing database always
    // builds for itself. that client fails to list the directory.
    std::filesystem::create_directories(uri);
    std::filesystem::permissions(uri, std::filesystem::perms::none);

    std::error_code ec;
    const std::filesystem::directory_iterator locked_dir(uri, ec);
    if (!ec) {
      // the directory is still readable (e.g. when running as root),
      // so the failure cannot be triggered
      std::filesystem::permissions(uri, std::filesystem::perms::owner_all);
      SUCCEED("directory permissions are not enforced");
      return;
    }

    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    LanceDBConnection* db = nullptr;
    char* error_message = nullptr;
    const LanceDBError result = lancedb_connect_builder_execute(builder, &db, &error_message);

    // restore the permissions before asserting, so that the fixture can clean up
    std::filesystem::permissions(uri, std::filesystem::perms::owner_all);

    // lancedb flattens any failure of the namespace client into an invalid input
    // error, so the reason is recovered from the message
    REQUIRE(result == LANCEDB_PERMISSION_DENIED);
    REQUIRE(db == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("Failed to connect to namespace") != std::string::npos);
    lancedb_free_string(error_message);
  }
  SECTION("Create and free registry") {
    // Test creating and freeing a registry without using it
    LanceDBObjectStoreRegistry* registry = lancedb_registry_new();
    REQUIRE(registry != nullptr);
    lancedb_registry_free(registry);
  }
  SECTION("Create session with registry - NULL registry uses default") {
    // NULL registry should use default ObjectStoreRegistry
    LanceDBSession* session = lancedb_session_new_with_registry(nullptr, nullptr);
    REQUIRE(session != nullptr);
    lancedb_session_free(session);
  }
  SECTION("Create session with options and NULL registry") {
    LanceDBSessionOptions options{};
    options.index_cache_bytes = 1024 * 1024;
    options.metadata_cache_bytes = 2 * 1024 * 1024;
    LanceDBSession* session = lancedb_session_new_with_registry(&options, nullptr);
    REQUIRE(session != nullptr);
    lancedb_session_free(session);
  }
  SECTION("Create session with custom registry") {
    // Create registry using C API
    LanceDBObjectStoreRegistry* registry = lancedb_registry_new();
    REQUIRE(registry != nullptr);
    // Create session with custom registry (transfers ownership)
    LanceDBSession* session = lancedb_session_new_with_registry(nullptr, registry);
    REQUIRE(session != nullptr);
    // Note: registry ownership transferred, don't free it
    lancedb_session_free(session);
  }
  SECTION("Create session with custom registry and options") {
    LanceDBSessionOptions options{};
    options.index_cache_bytes = 512 * 1024 * 1024;
    options.metadata_cache_bytes = 256 * 1024 * 1024;
    // Create registry using C API
    LanceDBObjectStoreRegistry* registry = lancedb_registry_new();
    REQUIRE(registry != nullptr);
    // Create session with custom registry and options (transfers ownership)
    LanceDBSession* session = lancedb_session_new_with_registry(&options, registry);
    REQUIRE(session != nullptr);
    lancedb_session_free(session);
  }
  SECTION("Session with registry can be used with connection builder") {
    LanceDBObjectStoreRegistry* registry = lancedb_registry_new();
    REQUIRE(registry != nullptr);
    LanceDBSession* session = lancedb_session_new_with_registry(nullptr, registry);
    REQUIRE(session != nullptr);
    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    builder = lancedb_connect_builder_session(builder, session);
    REQUIRE(builder != nullptr);
    LanceDBConnection* db = nullptr;
    REQUIRE(lancedb_connect_builder_execute(builder, &db, nullptr) == LANCEDB_SUCCESS);
    REQUIRE(db != nullptr);
    lancedb_connection_free(db);
    lancedb_session_free(session);
  }
  SECTION("Insert NULL provider into registry returns error") {
    LanceDBObjectStoreRegistry* registry = lancedb_registry_new();
    REQUIRE(registry != nullptr);
    char* error_message = nullptr;
    LanceDBError rc = lancedb_registry_insert_provider(registry, "s3", nullptr, &error_message);
    REQUIRE(rc == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_registry_free(registry);
  }
  SECTION("Insert provider with NULL registry returns error") {
    char* error_message = nullptr;
    LanceDBError rc = lancedb_registry_insert_provider(nullptr, "s3", nullptr, &error_message);
    REQUIRE(rc == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }
  SECTION("Insert provider with NULL scheme returns error") {
    LanceDBObjectStoreRegistry* registry = lancedb_registry_new();
    REQUIRE(registry != nullptr);
    char* error_message = nullptr;
    LanceDBError rc = lancedb_registry_insert_provider(registry, nullptr, nullptr, &error_message);
    REQUIRE(rc == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_registry_free(registry);
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
      lancedb_table_free(open_table(names_out[i]));
    }
  }
  SECTION("Drop Tables") {
    for (size_t i = 0; i < count_out; ++i) {
      char* error_message = nullptr;
      auto result = lancedb_connection_drop_table(db, names_out[i], _namespace, &error_message);
      REQUIRE(error_message == nullptr);
      REQUIRE(result == LANCEDB_SUCCESS);
      require_table_not_found(names_out[i]);
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
      require_table_not_found(new_name);
      lancedb_table_free(open_table(names_out[i]));
    }
  }
  SECTION("Drop All Tables") {
    char* error_message = nullptr;
    auto result = lancedb_connection_drop_all_tables(db, _namespace, &error_message);
    REQUIRE(error_message == nullptr);
    REQUIRE(result == LANCEDB_SUCCESS);
    for (size_t i = 0; i < count_out; ++i) {
      require_table_not_found(names_out[i]);
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
    REQUIRE(error_message == nullptr);
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
  REQUIRE(error_message == nullptr);
  REQUIRE(result == LANCEDB_SUCCESS);

  SECTION("List Namespaces") {
    char* error_message = nullptr;
    char** names_out = nullptr;
    size_t count_out = 0;
    auto result = lancedb_connection_list_namespaces(db,
        _namespace,
        &names_out,
        &count_out,
        &error_message);
    REQUIRE(error_message == nullptr);
    REQUIRE(result == LANCEDB_SUCCESS);
    lancedb_free_namespace_list(names_out, count_out);
  }
  SECTION("Drop Namespace") {
    char* error_message = nullptr;
    auto result = lancedb_connection_drop_namespace(db,
        _namespace,
        &error_message);
    REQUIRE(error_message == nullptr);
    REQUIRE(result == LANCEDB_SUCCESS);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Namespaces - listing and invalid arguments", "[connection]") {
  char* error_message = nullptr;

  SECTION("List the namespaces of the root namespace") {
    REQUIRE(lancedb_connection_create_namespace(db, "space_a", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_connection_create_namespace(db, "space_b", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // A NULL parent lists the namespaces of the root namespace
    char** names_out = nullptr;
    size_t count_out = 0;
    REQUIRE(lancedb_connection_list_namespaces(
        db, nullptr, &names_out, &count_out, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count_out == 2);
    REQUIRE(names_out != nullptr);

    std::set<std::string> names;
    for (size_t i = 0; i < count_out; i++) {
      REQUIRE(names_out[i] != nullptr);
      names.insert(names_out[i]);
    }
    REQUIRE(names.count("space_a") == 1);
    REQUIRE(names.count("space_b") == 1);

    lancedb_free_namespace_list(names_out, count_out);

    REQUIRE(lancedb_connection_drop_namespace(db, "space_a", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(lancedb_connection_drop_namespace(db, "space_b", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("Namespace operations with invalid arguments should fail") {
    char** names_out = nullptr;
    size_t count_out = 0;

    REQUIRE(lancedb_connection_create_namespace(
        nullptr, "space", nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_create_namespace(
        db, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_drop_namespace(
        nullptr, "space", nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_drop_namespace(
        db, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_list_namespaces(
        nullptr, nullptr, &names_out, &count_out, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_list_namespaces(
        db, nullptr, nullptr, &count_out, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_list_namespaces(
        db, nullptr, &names_out, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Freeing a namespace list with no namespaces is safe") {
    lancedb_free_namespace_list(nullptr, 0);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Namespace paths", "[connection]") {
  char* error_message = nullptr;

  const char* parent_path[] = {"parent"};
  const char* child_path[] = {"parent", "child"};

  SECTION("Create, list, describe, and drop nested namespaces") {
    REQUIRE(lancedb_connection_create_namespace_path(
        db, parent_path, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_connection_create_namespace_path(
        db, child_path, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // The root path lists the top-level namespace. A stale error slot is
    // cleared on success.
    char** names_out = nullptr;
    size_t count_out = 0;
    error_message = const_cast<char*>("stale error");
    REQUIRE(lancedb_connection_list_namespaces_path(
        db, nullptr, 0, &names_out, &count_out, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count_out == 1);
    REQUIRE(std::string(names_out[0]) == "parent");
    lancedb_free_namespace_list(names_out, count_out);

    // The parent path lists only its child namespace.
    names_out = nullptr;
    count_out = 0;
    REQUIRE(lancedb_connection_list_namespaces_path(
        db, parent_path, 1, &names_out, &count_out, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count_out == 1);
    REQUIRE(std::string(names_out[0]) == "child");
    lancedb_free_namespace_list(names_out, count_out);

    // The root reports an empty property map. The child was created without
    // properties, so the SDK omits that map. A stale error slot is cleared on
    // success.
    LanceDBNamespaceDescription description{};
    error_message = const_cast<char*>("stale error");
    REQUIRE(lancedb_connection_describe_namespace_path(
        db, nullptr, 0, &description, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(description.has_properties);
    REQUIRE(description.property_count == 0);
    REQUIRE(description.property_keys == nullptr);
    REQUIRE(description.property_values == nullptr);
    REQUIRE_FALSE(description.has_context);
    lancedb_free_namespace_description(&description);

    REQUIRE(lancedb_connection_describe_namespace_path(
        db, child_path, 2, &description, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE_FALSE(description.has_properties);
    REQUIRE(description.property_count == 0);
    REQUIRE(description.has_context == false);
    lancedb_free_namespace_description(&description);

    // A missing nested namespace keeps the SDK's not-found classification and
    // leaves the provided description empty.
    const char* missing_path[] = {"parent", "missing"};
    char* stale_names[] = {nullptr};
    description.has_properties = true;
    description.property_keys = stale_names;
    description.property_values = stale_names;
    description.property_count = 1;
    description.has_context = true;
    description.context_keys = stale_names;
    description.context_values = stale_names;
    description.context_count = 1;
    error_message = const_cast<char*>("stale error");
    REQUIRE(lancedb_connection_describe_namespace_path(
        db, missing_path, 2, &description, &error_message) == LANCEDB_DATABASE_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    REQUIRE_FALSE(description.has_properties);
    REQUIRE(description.property_keys == nullptr);
    REQUIRE(description.property_values == nullptr);
    REQUIRE(description.property_count == 0);
    REQUIRE_FALSE(description.has_context);
    REQUIRE(description.context_keys == nullptr);
    REQUIRE(description.context_values == nullptr);
    REQUIRE(description.context_count == 0);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Dropping the child leaves the parent, then dropping the parent removes it.
    REQUIRE(lancedb_connection_drop_namespace_path(
        db, child_path, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    names_out = nullptr;
    count_out = 0;
    REQUIRE(lancedb_connection_list_namespaces_path(
        db, parent_path, 1, &names_out, &count_out, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count_out == 0);
    REQUIRE(names_out == nullptr);
    lancedb_free_namespace_list(names_out, count_out);
    REQUIRE(lancedb_connection_drop_namespace_path(
        db, parent_path, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }

  SECTION("Namespace path arguments are validated") {
    const char* null_component[] = {"parent", nullptr};
    const char* invalid_utf8[] = {NON_UTF8};
    char** names_out = nullptr;
    size_t count_out = 0;
    char* stale_names[] = {nullptr};
    LanceDBNamespaceDescription description{};

    auto set_stale_description = [&]() {
      description.has_properties = true;
      description.property_keys = stale_names;
      description.property_values = stale_names;
      description.property_count = 1;
      description.has_context = true;
      description.context_keys = stale_names;
      description.context_values = stale_names;
      description.context_count = 1;
    };

    auto require_empty_description = [&]() {
      REQUIRE_FALSE(description.has_properties);
      REQUIRE(description.property_keys == nullptr);
      REQUIRE(description.property_values == nullptr);
      REQUIRE(description.property_count == 0);
      REQUIRE_FALSE(description.has_context);
      REQUIRE(description.context_keys == nullptr);
      REQUIRE(description.context_values == nullptr);
      REQUIRE(description.context_count == 0);
    };

    REQUIRE(lancedb_connection_create_namespace_path(
        db, nullptr, 1, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_connection_create_namespace_path(
        db, null_component, 2, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_connection_create_namespace_path(
        db, invalid_utf8, 1, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    names_out = stale_names;
    count_out = 42;
    error_message = const_cast<char*>("stale error");
    REQUIRE(lancedb_connection_list_namespaces_path(
        db, nullptr, 1, &names_out, &count_out, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(names_out == nullptr);
    REQUIRE(count_out == 0);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const char* invalid_component[] = {"bad name"};
    names_out = stale_names;
    count_out = 42;
    error_message = const_cast<char*>("stale error");
    REQUIRE(lancedb_connection_list_namespaces_path(
        db, invalid_component, 1, &names_out, &count_out, &error_message) == LANCEDB_INVALID_INPUT);
    REQUIRE(names_out == nullptr);
    REQUIRE(count_out == 0);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    set_stale_description();
    error_message = const_cast<char*>("stale error");
    REQUIRE(lancedb_connection_describe_namespace_path(
        db, nullptr, 1, &description, &error_message) == LANCEDB_INVALID_ARGUMENT);
    require_empty_description();
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_connection_drop_namespace_path(
        db, nullptr, 1, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_connection_drop_table_path(
        db, "table", nullptr, 1, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_connection_rename_table_path(
        db, "old", "new", nullptr, 1, nullptr, 0, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_connection_drop_all_tables_path(
        db, nullptr, 1, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // A zero length is the root namespace even when a non-NULL pointer is passed.
    REQUIRE(lancedb_connection_list_namespaces_path(
        db, invalid_utf8, 0, &names_out, &count_out, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_free_namespace_list(names_out, count_out);

    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);
    REQUIRE(lancedb_table_names_builder_namespace(builder, nullptr, 1) == nullptr);

    // Zero length with a non-NULL pointer is the root namespace for the builder too.
    builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);
    builder = lancedb_table_names_builder_namespace(builder, invalid_utf8, 0);
    REQUIRE(builder != nullptr);
    names_out = nullptr;
    count_out = 0;
    REQUIRE(lancedb_table_names_builder_execute(
        builder, &names_out, &count_out, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_free_table_names(names_out, count_out);

    lancedb_free_namespace_description(nullptr);
  }

  SECTION("Table management paths target the selected namespace") {
    const char* workspace_path[] = {"workspace"};
    const char* nested_path[] = {"workspace", "nested"};

    REQUIRE(lancedb_connection_create_namespace_path(
        db, workspace_path, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_connection_create_namespace_path(
        db, nested_path, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    create_empty_table("shared");

    // The root sees the table while the nested namespace does not.
    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);
    char** names_out = nullptr;
    size_t count_out = 0;
    REQUIRE(lancedb_table_names_builder_execute(
        builder, &names_out, &count_out, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count_out == 1);
    REQUIRE(std::string(names_out[0]) == "shared");
    lancedb_free_table_names(names_out, count_out);

    builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);
    builder = lancedb_table_names_builder_namespace(builder, nested_path, 2);
    REQUIRE(builder != nullptr);
    names_out = nullptr;
    count_out = 0;
    REQUIRE(lancedb_table_names_builder_execute(
        builder, &names_out, &count_out, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(count_out == 0);
    REQUIRE(names_out == nullptr);
    lancedb_free_table_names(names_out, count_out);

    // Dropping a root table through the nested namespace must not remove it.
    REQUIRE(lancedb_connection_drop_table_path(
        db, "shared", nested_path, 2, &error_message) == LANCEDB_TABLE_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    LanceDBTable* table = nullptr;
    REQUIRE(lancedb_connection_open_table(
        db, "shared", &table, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(table != nullptr);
    lancedb_table_free(table);

    // Dropping all tables in the nested namespace leaves the root table alone.
    REQUIRE(lancedb_connection_drop_all_tables_path(
        db, nested_path, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_connection_open_table(
        db, "shared", &table, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_table_free(table);

    // The root path drops the root table.
    REQUIRE(lancedb_connection_drop_all_tables_path(
        db, nullptr, 0, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_connection_open_table(
        db, "shared", &table, &error_message) == LANCEDB_TABLE_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_connection_drop_namespace_path(
        db, nested_path, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_connection_drop_namespace_path(
        db, workspace_path, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Open Table Options", "[connection]") {
  const std::string table_name = "open_options_table";
  constexpr size_t initial_rows = 6;
  constexpr size_t added_rows = 3;

  auto open_with_options = [&](const char* name,
                               const LanceDBOpenTableOptions* options,
                               LanceDBTable** table,
                               char** error_message) {
    return lancedb_connection_open_table_with_options(
        db, name, options, table, error_message);
  };

  auto require_count_and_version = [](LanceDBTable* table, size_t rows, uint64_t version) {
    REQUIRE(table != nullptr);
    REQUIRE(lancedb_table_count_rows(table) == rows);
    REQUIRE(lancedb_table_version(table) == version);
  };

  char* error_message = nullptr;
  LanceDBTable* table = create_table_with_data(table_name, initial_rows, 0);
  REQUIRE(table != nullptr);
  auto reader = create_reader_from_batch(
      create_test_record_batch(added_rows, initial_rows));
  REQUIRE(reader != nullptr);
  REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  require_count_and_version(table, initial_rows + added_rows, 2);
  lancedb_table_free(table);

  SECTION("Zero-initialized and NULL options open the latest table") {
    LanceDBOpenTableOptions options{};
    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(opened, initial_rows + added_rows, 2);
    lancedb_table_free(opened);

    opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), nullptr, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(opened, initial_rows + added_rows, 2);
    lancedb_table_free(opened);
  }

  SECTION("Unused initialized fields do not change the latest open") {
    LanceDBOpenTableOptions options{};
    options.version = 1;
    options.managed_versioning = 2;

    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(opened, initial_rows + added_rows, 2);
    lancedb_table_free(opened);
  }

  SECTION("A selected version opens a read-only snapshot") {
    LanceDBOpenTableOptions options{};
    options.set_fields = LANCEDB_OPEN_TABLE_VERSION;
    options.version = 1;

    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(opened, initial_rows, 1);

    auto pinned_reader = create_reader_from_batch(create_test_record_batch(1, 20));
    REQUIRE(pinned_reader != nullptr);
    REQUIRE(lancedb_table_add(opened, pinned_reader, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    require_count_and_version(opened, initial_rows, 1);
    lancedb_table_free(opened);
  }

  SECTION("Version zero is selected instead of treated as omitted") {
    LanceDBOpenTableOptions options{};
    options.set_fields = LANCEDB_OPEN_TABLE_VERSION;
    options.version = 0;

    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        != LANCEDB_SUCCESS);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
  }

  SECTION("The main branch composes with a selected version") {
    LanceDBOpenTableOptions options{};
    options.set_fields = LANCEDB_OPEN_TABLE_VERSION;
    options.version = 1;
    options.branch = "main";

    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(opened, initial_rows, 1);
    lancedb_table_free(opened);
  }

  SECTION("A named branch opens its head and a pinned branch version") {
    LanceDBTable* main = open_table(table_name);
    LanceDBBranchSource source{};
    source.kind = LANCEDB_BRANCH_SOURCE_VERSION;
    source.has_version = 1;
    source.version = 2;

    LanceDBTable* branch = nullptr;
    REQUIRE(lancedb_table_create_branch(
        main, "open_options_branch", &source, &branch, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(branch != nullptr);
    lancedb_table_free(main);

    const uint64_t branch_fork_version = lancedb_table_version(branch);
    require_count_and_version(branch, initial_rows + added_rows, branch_fork_version);

    auto branch_reader = create_reader_from_batch(
        create_test_record_batch(2, initial_rows + added_rows));
    REQUIRE(branch_reader != nullptr);
    REQUIRE(lancedb_table_add(branch, branch_reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    const uint64_t branch_head_version = lancedb_table_version(branch);
    require_count_and_version(
        branch, initial_rows + added_rows + 2, branch_head_version);
    lancedb_table_free(branch);

    LanceDBOpenTableOptions options{};
    options.branch = "open_options_branch";
    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(
        opened, initial_rows + added_rows + 2, branch_head_version);
    lancedb_table_free(opened);

    options.set_fields = LANCEDB_OPEN_TABLE_VERSION;
    options.version = branch_fork_version;
    opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(
        opened, initial_rows + added_rows, branch_fork_version);

    auto pinned_reader = create_reader_from_batch(create_test_record_batch(1, 30));
    REQUIRE(pinned_reader != nullptr);
    REQUIRE(lancedb_table_add(opened, pinned_reader, &error_message) != LANCEDB_SUCCESS);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    require_count_and_version(
        opened, initial_rows + added_rows, branch_fork_version);
    lancedb_table_free(opened);
  }

  SECTION("A missing branch keeps the output NULL") {
    LanceDBOpenTableOptions options{};
    options.branch = "open_options_missing_branch";

    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        != LANCEDB_SUCCESS);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
  }

  SECTION("Explicit index cache sizes are accepted") {
    LanceDBOpenTableOptions options{};
    options.set_fields = LANCEDB_OPEN_TABLE_INDEX_CACHE_SIZE;
    options.index_cache_size = 0;

    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(opened, initial_rows + added_rows, 2);
    lancedb_table_free(opened);

    options.index_cache_size = 16;
    opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(opened, initial_rows + added_rows, 2);
    lancedb_table_free(opened);
  }

  SECTION("A custom location replaces the derived table path") {
    const std::string source_name = "open_options_location_source";
    const std::string moved_name = "open_options_location_moved";
    LanceDBTable* source = create_table_with_data(source_name, 4, 0);
    REQUIRE(source != nullptr);
    lancedb_table_free(source);

    const auto source_dir = std::filesystem::path(uri) / (source_name + ".lance");
    const auto moved_dir = std::filesystem::path(uri) / (moved_name + ".lance");
    std::error_code rename_error;
    std::filesystem::rename(source_dir, moved_dir, rename_error);
    REQUIRE_FALSE(rename_error);
    const std::string moved_location = moved_dir.string();

    LanceDBOpenTableOptions options{};
    options.location = moved_location.c_str();
    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(source_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(opened, 4, 1);
    lancedb_table_free(opened);

    // The derived path no longer exists, so the legacy open cannot find it.
    opened = nullptr;
    REQUIRE(lancedb_connection_open_table(db, source_name.c_str(), &opened, &error_message)
        == LANCEDB_TABLE_NOT_FOUND);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
  }

  SECTION("Per-table storage options are borrowed and validated") {
    const std::string key_storage = "scheduler_error_mode";
    const std::string value_storage = "best_effort";
    const char* keys[] = {key_storage.c_str()};
    const char* values[] = {value_storage.c_str()};

    LanceDBOpenTableOptions options{};
    options.storage_keys = keys;
    options.storage_values = values;
    options.storage_count = 1;

    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(opened, initial_rows + added_rows, 2);
    lancedb_table_free(opened);

    // The options and their strings are only borrowed for the call.
    options.storage_keys = nullptr;
    opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    options.storage_keys = keys;
    const char* null_values[] = {nullptr};
    options.storage_values = null_values;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const char* invalid_keys[] = {NON_UTF8};
    options.storage_keys = invalid_keys;
    options.storage_values = values;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
  }

  SECTION("An invalid scheduler_error_mode is diagnosed by the SDK") {
    const char* keys[] = {"scheduler_error_mode"};
    const char* values[] = {"invalid"};
    LanceDBOpenTableOptions options{};
    options.storage_keys = keys;
    options.storage_values = values;
    options.storage_count = 1;

    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::string(error_message).find("scheduler_error_mode") != std::string::npos);
    lancedb_free_string(error_message);
    error_message = nullptr;
  }

  SECTION("Managed versioning distinguishes unset, false and true") {
    LanceDBOpenTableOptions options{};
    options.set_fields = LANCEDB_OPEN_TABLE_MANAGED_VERSIONING;

    for (const int value : {0, 1}) {
      options.managed_versioning = value;
      LanceDBTable* opened = nullptr;
      REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
          == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
      require_count_and_version(opened, initial_rows + added_rows, 2);
      lancedb_table_free(opened);
    }

    options.managed_versioning = 2;
    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
  }

  SECTION("A multi-component namespace path selects that namespace") {
    const char* parent_path[] = {"open_options_parent"};
    const char* child_path[] = {"open_options_parent", "child"};
    REQUIRE(lancedb_connection_create_namespace_path(
        db, parent_path, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_connection_create_namespace_path(
        db, child_path, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    const std::string namespaced_name = "open_options_namespaced";
    LanceDBTable* root_table = create_table_with_data(namespaced_name, 4, 0);
    REQUIRE(root_table != nullptr);
    lancedb_table_free(root_table);

    LanceDBOpenTableOptions options{};
    options.namespace_path = child_path;
    options.namespace_path_len = 2;
    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(namespaced_name.c_str(), &options, &opened, &error_message)
        != LANCEDB_SUCCESS);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    // Zero length is the root namespace even when a non-NULL array is passed.
    options.namespace_path_len = 0;
    opened = nullptr;
    REQUIRE(open_with_options(namespaced_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_count_and_version(opened, 4, 1);
    lancedb_table_free(opened);
  }

  SECTION("Invalid open options are rejected") {
    LanceDBOpenTableOptions options{};
    options.set_fields = 1u << 31;
    LanceDBTable* opened = nullptr;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    options = {};
    options.location = NON_UTF8;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    options = {};
    options.branch = NON_UTF8;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const char* null_component[] = {"parent", nullptr};
    options = {};
    options.namespace_path_len = 2;
    options.namespace_path = null_component;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    options = {};
    options.namespace_path_len = 1;
    REQUIRE(open_with_options(table_name.c_str(), &options, &opened, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(open_with_options("invalid table name", nullptr, &opened, &error_message)
        == LANCEDB_INVALID_TABLE_NAME);
    REQUIRE(opened == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
  }

  SECTION("The new entry clears outputs on failure while the legacy entry keeps its own") {
    LanceDBTable* previous = open_table(table_name);
    LanceDBTable* output = previous;
    error_message = const_cast<char*>("stale error");
    REQUIRE(open_with_options("open_options_missing", nullptr, &output, &error_message)
        == LANCEDB_TABLE_NOT_FOUND);
    REQUIRE(output == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;
    lancedb_table_free(previous);

    // The legacy entry does not write its output on failure.
    previous = open_table(table_name);
    output = previous;
    REQUIRE(lancedb_connection_open_table(
        db, "open_options_missing", &output, &error_message) == LANCEDB_TABLE_NOT_FOUND);
    REQUIRE(output == previous);
    if (error_message != nullptr) {
      lancedb_free_string(error_message);
      error_message = nullptr;
    }
    lancedb_table_free(previous);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Connection - table operations with invalid arguments", "[connection]") {
  char* error_message = nullptr;
  const char* _namespace = nullptr;

  SECTION("Dropping a table that does not exist should fail") {
    LanceDBError result = lancedb_connection_drop_table(
        db, "no_such_table", _namespace, &error_message);
    REQUIRE(result == LANCEDB_TABLE_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Dropping a table that does not exist inside a namespace should fail") {
    REQUIRE(lancedb_connection_create_namespace(db, "myspace", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    // the namespace backend reports a missing table as a generic runtime error,
    // so this verifies that it is still reported as a missing table
    LanceDBError result = lancedb_connection_drop_table(
        db, "no_such_table", "myspace", &error_message);
    REQUIRE(result == LANCEDB_TABLE_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Dropping a namespace that does not exist should fail") {
    // the namespace backend reports a missing namespace without its original error
    // variant, so this verifies that it is still reported as a missing database
    LanceDBError result = lancedb_connection_drop_namespace(db, "no_such_namespace", &error_message);
    REQUIRE(result == LANCEDB_DATABASE_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Dropping a nested namespace that does not exist should fail") {
    REQUIRE(lancedb_connection_create_namespace(db, "parent", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    LanceDBError result = lancedb_connection_drop_namespace(db, "parent/no_such_child", &error_message);
    REQUIRE(result == LANCEDB_DATABASE_NOT_FOUND);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Creating a namespace that already exists should fail") {
    // a namespace failure that is not a missing namespace keeps its own error code
    REQUIRE(lancedb_connection_create_namespace(db, "myspace", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    LanceDBError result = lancedb_connection_create_namespace(db, "myspace", &error_message);
    REQUIRE(result == LANCEDB_NAMESPACE);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Opening a table that does not exist should fail") {
    require_table_not_found("no_such_table");
  }

  SECTION("Opening a table that was dropped should fail") {
    const std::string table_name = "dropped_table";
    create_empty_table(table_name);
    lancedb_table_free(open_table(table_name));
    REQUIRE(lancedb_connection_drop_table(db, table_name.c_str(), _namespace, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    require_table_not_found(table_name);
  }

  SECTION("Opening a table with an invalid name should fail") {
    // the name is validated before the table is opened, since lancedb panics
    // on an invalid table name instead of returning an error
    for (const auto* invalid_name : {"", "invalid table name", "../escape"}) {
      LanceDBTable* table = nullptr;
      REQUIRE(lancedb_connection_open_table(db, invalid_name, &table, &error_message)
          == LANCEDB_INVALID_TABLE_NAME);
      REQUIRE(table == nullptr);
      REQUIRE(error_message != nullptr);
      lancedb_free_string(error_message);
      error_message = nullptr;
    }
  }

  SECTION("Opening a table that cannot be loaded should fail") {
    const std::string table_name = "corrupted_table";
    create_empty_table(table_name);

    // overwrite the manifests, so that the table exists but cannot be loaded
    const auto versions_dir = std::filesystem::path(uri) / (table_name + ".lance") / "_versions";
    size_t manifests = 0;
    for (const auto& entry : std::filesystem::directory_iterator(versions_dir)) {
      if (entry.path().extension() == ".manifest") {
        std::ofstream manifest(entry.path(), std::ios::binary | std::ios::trunc);
        manifest << "not a manifest";
        ++manifests;
      }
    }
    REQUIRE(manifests > 0);

    LanceDBTable* table = nullptr;
    REQUIRE(lancedb_connection_open_table(db, table_name.c_str(), &table, &error_message)
        == LANCEDB_LANCE);
    REQUIRE(table == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Opening a table that cannot be read should fail") {
    const std::string table_name = "unreadable_table";
    create_empty_table(table_name);
    const auto table_dir = std::filesystem::path(uri) / (table_name + ".lance");
    std::filesystem::permissions(table_dir, std::filesystem::perms::none);

    std::error_code ec;
    const std::filesystem::directory_iterator locked_dir(table_dir, ec);
    if (!ec) {
      // the directory is still readable (e.g. when running as root),
      // so the failure cannot be triggered
      std::filesystem::permissions(table_dir, std::filesystem::perms::owner_all);
      SUCCEED("directory permissions are not enforced");
      return;
    }

    LanceDBTable* table = nullptr;
    const LanceDBError result =
        lancedb_connection_open_table(db, table_name.c_str(), &table, &error_message);

    // restore the permissions before asserting, so that the fixture can clean up
    std::filesystem::permissions(table_dir, std::filesystem::perms::owner_all);

    // the object store error is wrapped by lance without its original variant,
    // so the reason is recovered from the message
    REQUIRE(result == LANCEDB_PERMISSION_DENIED);
    REQUIRE(table == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("Renaming a table that does not exist should fail") {
    LanceDBError result = lancedb_connection_rename_table(
        db, "no_such_table", "new_name", _namespace, _namespace, &error_message);
    REQUIRE(result != LANCEDB_SUCCESS);
    if (error_message) {
      lancedb_free_string(error_message);
    }
  }

  SECTION("Table operations with null arguments should fail") {
    REQUIRE(lancedb_connection_drop_table(
        nullptr, "table", _namespace, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_drop_table(
        db, nullptr, _namespace, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_rename_table(
        nullptr, "old", "new", _namespace, _namespace, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_rename_table(
        db, nullptr, "new", _namespace, _namespace, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_rename_table(
        db, "old", nullptr, _namespace, _namespace, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_drop_all_tables(
        nullptr, _namespace, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Freeing a provider that was never created is safe") {
    lancedb_object_store_provider_free(nullptr);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Connection - invalid strings and null outputs", "[connection]") {
  // A string that is not valid UTF-8, to exercise the string conversion failures
  const char* const invalid_utf8 = "\xff\xfe";

  SECTION("Connecting with an invalid URI returns no builder") {
    REQUIRE(lancedb_connect(nullptr) == nullptr);
    REQUIRE(lancedb_connect(invalid_utf8) == nullptr);
  }

  SECTION("Executing a null builder should fail") {
    LanceDBConnection* connection = nullptr;
    REQUIRE(lancedb_connect_builder_execute(nullptr, &connection, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(connection == nullptr);
  }

  SECTION("Builder options with invalid arguments return no builder") {
    REQUIRE(lancedb_connect_builder_storage_option(nullptr, "key", "value") == nullptr);
    REQUIRE(lancedb_connect_builder_session(nullptr, nullptr) == nullptr);

    LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    REQUIRE(lancedb_connect_builder_storage_option(builder, nullptr, "value") == nullptr);

    builder = lancedb_connect(uri.c_str());
    REQUIRE(builder != nullptr);
    REQUIRE(lancedb_connect_builder_storage_option(builder, invalid_utf8, "value") == nullptr);
  }

  SECTION("Opening a table with an invalid argument returns no table") {
    LanceDBTable* table = nullptr;
    REQUIRE(lancedb_connection_open_table(nullptr, "table", &table, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_open_table(db, nullptr, &table, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_open_table(db, "table", nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_open_table(db, invalid_utf8, &table, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(table == nullptr);
  }

  SECTION("Creating a table with an invalid name should fail") {
    auto schema = create_test_schema();
    struct ArrowSchema c_schema;
    REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

    LanceDBTable* table = nullptr;
    REQUIRE(lancedb_table_create(
        db, invalid_utf8, reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
        nullptr, &table, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(table == nullptr);

    if (c_schema.release) {
      c_schema.release(&c_schema);
    }
  }

  SECTION("Listing table names with invalid arguments should fail") {
    char** names_out = nullptr;
    size_t count_out = 0;

    REQUIRE(lancedb_connection_table_names(
        nullptr, &names_out, &count_out, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_table_names(
        db, nullptr, &count_out, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_table_names(
        db, &names_out, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);

    // Freeing an empty name list is safe
    lancedb_free_table_names(nullptr, 0);
  }

  SECTION("Table name builder with invalid arguments should fail") {
    char** names_out = nullptr;
    size_t count_out = 0;

    LanceDBTableNamesBuilder* builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);
    REQUIRE(lancedb_table_names_builder_execute(
        builder, nullptr, &count_out, nullptr) == LANCEDB_INVALID_ARGUMENT);

    builder = lancedb_connection_table_names_builder(db);
    REQUIRE(builder != nullptr);
    REQUIRE(lancedb_table_names_builder_execute(
        builder, &names_out, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);

    REQUIRE(lancedb_table_names_builder_execute(
        nullptr, &names_out, &count_out, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("Operations with names that are not valid UTF-8 should fail") {
    REQUIRE(lancedb_connection_drop_table(db, invalid_utf8, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_rename_table(
        db, invalid_utf8, "new_name", nullptr, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_rename_table(
        db, "old_name", invalid_utf8, nullptr, nullptr, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_drop_all_tables(db, invalid_utf8, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_create_namespace(db, invalid_utf8, nullptr) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(lancedb_connection_drop_namespace(db, invalid_utf8, nullptr) == LANCEDB_INVALID_ARGUMENT);

    char** names_out = nullptr;
    size_t count_out = 0;
    REQUIRE(lancedb_connection_list_namespaces(
        db, invalid_utf8, &names_out, &count_out, nullptr) == LANCEDB_INVALID_ARGUMENT);
  }
}
