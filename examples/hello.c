/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

/* Minimal C17 consumer: connect to a local database and print the version.
 * It intentionally uses no C++ or Apache Arrow C++ headers, so it also checks
 * that the public header is usable from a pure C translation unit. */

#include <lancedb.h>

#include <stdio.h>

int main(int argc, char** argv) {
  const char* uri = argc > 1 ? argv[1] : "./lancedb-c-example-db";

  printf("lancedb-c %s\n", lancedb_version());

  LanceDBConnectBuilder* builder = lancedb_connect(uri);
  if (builder == NULL) {
    fprintf(stderr, "failed to create a connection builder for %s\n", uri);
    return 1;
  }

  LanceDBConnection* connection = NULL;
  char* error_message = NULL;
  const LanceDBError status =
      lancedb_connect_builder_execute(builder, &connection, &error_message);
  if (status != LANCEDB_SUCCESS) {
    fprintf(stderr, "connect failed: %s\n",
            error_message != NULL ? error_message
                                  : lancedb_error_to_message(status));
    lancedb_free_string(error_message);
    return 1;
  }

  printf("connected to %s\n", lancedb_connection_uri(connection));
  lancedb_connection_free(connection);
  return 0;
}
