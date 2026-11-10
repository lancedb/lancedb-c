/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 *
 * Runtime configuration and failure-path tests. The library under test is
 * built with the `test-hooks` feature, which exports two entry points that
 * deliberately misbehave inside the runtime.
 */

#include <cstring>
#include <limits>
#include <string>
#include "test_common.h"

namespace {

struct ErrorMessage {
  char* text = nullptr;
  ~ErrorMessage() { if (text) lancedb_free_string(text); }
  std::string str() const { return text ? std::string(text) : std::string(); }
};

}  // namespace

TEST_CASE("LanceDB runtime configuration", "[runtime]") {
  SECTION("NULL options are rejected") {
    ErrorMessage err;
    REQUIRE(lancedb_runtime_configure(nullptr, &err.text) == LANCEDB_INVALID_ARGUMENT);
  }

  SECTION("a stack below the minimum is rejected") {
    LanceDBRuntimeOptions opts = {};
    opts.worker_stack_size = LANCEDB_MIN_WORKER_STACK_SIZE / 2;
    ErrorMessage err;
    REQUIRE(lancedb_runtime_configure(&opts, &err.text) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(err.str().find("minimum") != std::string::npos);
  }

  SECTION("an unrepresentable worker count is rejected") {
    LanceDBRuntimeOptions opts = {};
    opts.worker_threads = std::numeric_limits<size_t>::max();
    ErrorMessage err;
    REQUIRE(lancedb_runtime_configure(&opts, &err.text) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE_FALSE(err.str().empty());
  }

  SECTION("an unrepresentable stack size is rejected") {
    LanceDBRuntimeOptions opts = {};
    opts.worker_stack_size = std::numeric_limits<size_t>::max();
    ErrorMessage err;
    REQUIRE(lancedb_runtime_configure(&opts, &err.text) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE_FALSE(err.str().empty());
  }

  SECTION("an unrepresentable blocking thread limit is rejected") {
    LanceDBRuntimeOptions opts = {};
    opts.max_blocking_threads = std::numeric_limits<size_t>::max();
    ErrorMessage err;
    REQUIRE(lancedb_runtime_configure(&opts, &err.text) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE_FALSE(err.str().empty());
  }
}

TEST_CASE_METHOD(LanceDBFixture, "LanceDB runtime failure reporting", "[runtime]") {
  // The fixture opened a connection, so the runtime is running by now.
  SECTION("configuration after the runtime started is refused") {
    LanceDBRuntimeOptions opts = {};
    opts.worker_threads = 2;
    ErrorMessage err;
    REQUIRE(lancedb_runtime_configure(&opts, &err.text) == LANCEDB_RUNTIME);
    REQUIRE(err.str().find("already running") != std::string::npos);
  }

  SECTION("a panic inside a task is reported, not fatal") {
    ErrorMessage err;
    REQUIRE(lancedb_test_panic_in_task(&err.text) == LANCEDB_RUNTIME);
    REQUIRE(err.str().find("panicked") != std::string::npos);
    REQUIRE(lancedb_test_panic_in_task(nullptr) == LANCEDB_RUNTIME);
    // An ordinary database call must still succeed after the panic.
    ErrorMessage next_err;
    char** names = nullptr;
    size_t count = 0;
    const auto result = lancedb_connection_table_names(db, &names, &count, &next_err.text);
    lancedb_free_table_names(names, count);
    REQUIRE(result == LANCEDB_SUCCESS);
    REQUIRE(next_err.text == nullptr);
  }

  SECTION("calling the API from inside the runtime is reported, not fatal") {
    ErrorMessage err;
    REQUIRE(lancedb_test_call_from_runtime(&err.text) == LANCEDB_RUNTIME);
    REQUIRE_FALSE(err.str().empty());
  }
}
