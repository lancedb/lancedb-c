/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 *
 * Runtime configuration tests. Task panics and nested entry are covered by
 * the Rust unit tests in src/runtime/tests.rs.
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
}
