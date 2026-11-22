/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

/* A minimal pure-C consumer used to compile-check the public header with a
 * strict C17 compiler and to exercise the C ABI without any C++ runtime. */

#include "lancedb.h"

#include <stdio.h>
#include <string.h>

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
  int major = 0;
  int minor = 0;
  int patch = 0;
  if (sscanf(runtime_version, "%d.%d.%d", &major, &minor, &patch) != 3 ||
      major != LANCEDB_VERSION_MAJOR || minor != LANCEDB_VERSION_MINOR ||
      patch != LANCEDB_VERSION_PATCH) {
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
  LanceDBError status =
      lancedb_connect_builder_execute(builder, &connection, &error_message);
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
  printf("C17 consumer: OK (version %s)\n", runtime_version);
  return 0;
}
