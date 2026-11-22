/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

/* A minimal pure-C consumer used to compile-check the public header with a
 * strict C17 compiler and to exercise the C ABI without any C++ runtime. */

#include "lancedb.h"

#include <stdio.h>
#include <string.h>

#define LANCEDB_STRINGIFY_IMPL(value) #value
#define LANCEDB_STRINGIFY(value) LANCEDB_STRINGIFY_IMPL(value)
#define LANCEDB_VERSION_CORE                     \
  LANCEDB_STRINGIFY(LANCEDB_VERSION_MAJOR) "."  \
  LANCEDB_STRINGIFY(LANCEDB_VERSION_MINOR) "."  \
  LANCEDB_STRINGIFY(LANCEDB_VERSION_PATCH)

static int fail(const char* message) {
  fprintf(stderr, "FAIL: %s\n", message);
  return 1;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <temporary-database-path>\n", argv[0]);
    return 2;
  }

  const char* uri = argv[1];

  const char* runtime_version = lancedb_version();
  if (runtime_version == NULL || runtime_version[0] == '\0') {
    return fail("lancedb_version() must return a non-empty string");
  }
  if (strcmp(runtime_version, LANCEDB_VERSION_STRING) != 0) {
    fprintf(stderr, "header version %s does not match runtime version %s\n",
            LANCEDB_VERSION_STRING, runtime_version);
    return fail("the public header and runtime library report different versions");
  }
  const char* version_core = LANCEDB_VERSION_CORE;
  const size_t version_core_length = strlen(version_core);
  if (strncmp(runtime_version, version_core, version_core_length) != 0 ||
      (runtime_version[version_core_length] != '\0' &&
       runtime_version[version_core_length] != '-' &&
       runtime_version[version_core_length] != '+')) {
    return fail("the version macros do not match the runtime version string");
  }

  if (lancedb_connect(NULL) != NULL) {
    return fail("lancedb_connect(NULL) must return NULL");
  }

  LanceDBConnectBuilder* builder = lancedb_connect(uri);
  if (builder == NULL) {
    return fail("lancedb_connect() must return a builder");
  }

  char* error_message = NULL;
  LanceDBConnection* connection = NULL;
  LanceDBError status = LANCEDB_SUCCESS;

  /* The branch diff and cherry-pick entry points clear their outputs before
   * validating arguments, and the free functions accept NULL. */
  LanceDBBranchDiff branch_diff_storage;
  LanceDBBranchDiff* branch_diff = &branch_diff_storage;
  status = lancedb_table_diff_branch(NULL, "branch", &branch_diff,
                                     &error_message);
  if (status != LANCEDB_INVALID_ARGUMENT || branch_diff != NULL ||
      error_message == NULL) {
    lancedb_free_string(error_message);
    return fail("diff_branch must clear its output on invalid arguments");
  }
  lancedb_free_string(error_message);
  error_message = NULL;
  lancedb_free_branch_diff(NULL);

  LanceDBCherryPickResult cherry_pick_storage;
  LanceDBCherryPickResult* cherry_pick = &cherry_pick_storage;
  status = lancedb_table_cherry_pick(NULL, "branch", 1, &cherry_pick,
                                     &error_message);
  if (status != LANCEDB_INVALID_ARGUMENT || cherry_pick != NULL ||
      error_message == NULL) {
    lancedb_free_string(error_message);
    return fail("cherry_pick must clear its output on invalid arguments");
  }
  lancedb_free_string(error_message);
  error_message = NULL;
  lancedb_free_cherry_pick_result(NULL);

  status = lancedb_connect_builder_execute(builder, &connection, &error_message);
  if (status != LANCEDB_SUCCESS || connection == NULL) {
    fprintf(stderr, "connecting to %s failed: %s\n", uri,
            error_message != NULL ? error_message : "no error message");
    lancedb_free_string(error_message);
    return 1;
  }

  const char* connected_uri = lancedb_connection_uri(connection);
  if (connected_uri == NULL || strcmp(connected_uri, uri) != 0) {
    lancedb_connection_free(connection);
    return fail("the connection must expose the requested URI");
  }

  LanceDBReadConsistency consistency = {0};
  status = lancedb_connection_read_consistency(connection, &consistency, &error_message);
  if (status != LANCEDB_SUCCESS) {
    fprintf(stderr, "reading consistency failed: %s\n",
            error_message != NULL ? error_message : "no error message");
    lancedb_free_string(error_message);
    lancedb_connection_free(connection);
    return 1;
  }
  if (consistency.kind != LANCEDB_READ_CONSISTENCY_MANUAL ||
      consistency.interval_ms != 0) {
    lancedb_connection_free(connection);
    return fail("a connection without an interval must report manual consistency");
  }

  char** names = NULL;
  size_t count = 0;
  status =
      lancedb_connection_table_names(connection, &names, &count, &error_message);
  if (status != LANCEDB_SUCCESS) {
    fprintf(stderr, "listing tables failed: %s\n",
            error_message != NULL ? error_message : "no error message");
    lancedb_free_string(error_message);
    lancedb_connection_free(connection);
    return 1;
  }
  if (count != 0 || names != NULL) {
    lancedb_free_table_names(names, count);
    lancedb_connection_free(connection);
    return fail("a new database must not contain tables");
  }

  lancedb_free_table_names(names, count);
  lancedb_connection_free(connection);

  builder = lancedb_connect(uri);
  if (builder == NULL) {
    return fail("lancedb_connect() must return a builder");
  }
  builder = lancedb_connect_builder_api_key(builder, "ignored");
  builder = lancedb_connect_builder_region(builder, "ignored");
  builder = lancedb_connect_builder_host_override(builder, "http://127.0.0.1:1");
  builder = lancedb_connect_builder_sql_host_override(builder, "http://127.0.0.1:2");
  builder = lancedb_connect_builder_namespace_client_property(builder, "ignored", "value");
  builder = lancedb_connect_builder_manifest_enabled(builder, false);
  builder = lancedb_connect_builder_read_consistency_interval_ms(builder, 0);
  if (builder == NULL) {
    return fail("the connection builder setters must return a builder");
  }

  connection = NULL;
  status = lancedb_connect_builder_execute(builder, &connection, &error_message);
  if (status != LANCEDB_SUCCESS || connection == NULL) {
    fprintf(stderr, "connecting with settings failed: %s\n",
            error_message != NULL ? error_message : "no error message");
    lancedb_free_string(error_message);
    return 1;
  }

  consistency.kind = LANCEDB_READ_CONSISTENCY_MANUAL;
  consistency.interval_ms = 1;
  status = lancedb_connection_read_consistency(connection, &consistency, &error_message);
  if (status != LANCEDB_SUCCESS) {
    fprintf(stderr, "reading configured consistency failed: %s\n",
            error_message != NULL ? error_message : "no error message");
    lancedb_free_string(error_message);
    lancedb_connection_free(connection);
    return 1;
  }
  if (consistency.kind != LANCEDB_READ_CONSISTENCY_STRONG ||
      consistency.interval_ms != 0) {
    lancedb_connection_free(connection);
    return fail("an explicit zero interval must report strong consistency");
  }

  lancedb_connection_free(connection);

  /* A direct directory namespace connection uses the same C connection
   * entry points and needs no URI-specific setup. */
  char namespace_root[4096];
  const int namespace_root_len = snprintf(
      namespace_root, sizeof(namespace_root), "%s-namespace", uri);
  if (namespace_root_len < 0 ||
      (size_t)namespace_root_len >= sizeof(namespace_root)) {
    return fail("the namespace root path does not fit the C17 buffer");
  }

  const char* namespace_keys[] = {"root"};
  const char* namespace_values[] = {namespace_root};
  LanceDBConnectNamespaceBuilder* namespace_builder =
      lancedb_connect_namespace("dir", namespace_keys, namespace_values, 1);
  if (namespace_builder == NULL) {
    return fail("lancedb_connect_namespace() must return a builder");
  }
  namespace_builder = lancedb_connect_namespace_builder_storage_option(
      namespace_builder, "client_retry_timeout", "10");
  namespace_builder = lancedb_connect_namespace_builder_namespace_client_property(
      namespace_builder, "manifest_enabled", "true");
  namespace_builder = lancedb_connect_namespace_builder_read_consistency_interval_ms(
      namespace_builder, 0);
  namespace_builder = lancedb_connect_namespace_builder_pushdown_operation(
      namespace_builder, LANCEDB_NAMESPACE_PUSHDOWN_QUERY_TABLE);
  if (namespace_builder == NULL) {
    return fail("the namespace builder setters must return a builder");
  }

  connection = NULL;
  status =
      lancedb_connect_namespace_builder_execute(namespace_builder, &connection, &error_message);
  if (status != LANCEDB_SUCCESS || connection == NULL) {
    fprintf(stderr, "connecting to the dir namespace failed: %s\n",
            error_message != NULL ? error_message : "no error message");
    lancedb_free_string(error_message);
    return 1;
  }

  connected_uri = lancedb_connection_uri(connection);
  if (connected_uri == NULL || strcmp(connected_uri, "namespace://dir") != 0) {
    lancedb_connection_free(connection);
    return fail("the namespace connection must expose namespace://dir");
  }

  consistency.kind = LANCEDB_READ_CONSISTENCY_MANUAL;
  consistency.interval_ms = 1;
  status = lancedb_connection_read_consistency(connection, &consistency, &error_message);
  if (status != LANCEDB_SUCCESS) {
    fprintf(stderr, "reading namespace consistency failed: %s\n",
            error_message != NULL ? error_message : "no error message");
    lancedb_free_string(error_message);
    lancedb_connection_free(connection);
    return 1;
  }
  if (consistency.kind != LANCEDB_READ_CONSISTENCY_STRONG ||
      consistency.interval_ms != 0) {
    lancedb_connection_free(connection);
    return fail("an explicit zero namespace interval must report strong consistency");
  }

  names = NULL;
  count = 0;
  status =
      lancedb_connection_table_names(connection, &names, &count, &error_message);
  if (status != LANCEDB_SUCCESS) {
    fprintf(stderr, "listing namespace tables failed: %s\n",
            error_message != NULL ? error_message : "no error message");
    lancedb_free_string(error_message);
    lancedb_connection_free(connection);
    return 1;
  }
  if (count != 0 || names != NULL) {
    lancedb_free_table_names(names, count);
    lancedb_connection_free(connection);
    return fail("a new dir namespace must not contain tables");
  }

  lancedb_free_table_names(names, count);
  lancedb_connection_free(connection);

  printf("C17 consumer: OK (version %s)\n", runtime_version);
  return 0;
}
