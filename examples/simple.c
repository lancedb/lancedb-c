/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 *
 * Simple C example demonstrating LanceDB C bindings with nanoarrow
 *
 */

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <nanoarrow/nanoarrow.h>
#include "lancedb.h"

// helper function to check if directory exists
int directory_exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// helper function to remove directory recursively
int remove_directory(const char* path) {
  char command[256];
  snprintf(command, sizeof(command), "rm -rf %s", path);
  return system(command);
}

#define DIM 128

int create_schema(struct ArrowSchema* schema) {
  // Create root schema with 2 fields
  ArrowSchemaInit(schema);
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, 2));

  // Field 0: id (int32)
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(schema->children[0], "id"));

  // Field 1: item (128 float32 list)
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeFixedSize(schema->children[1], NANOARROW_TYPE_FIXED_SIZE_LIST, DIM));
  NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(schema->children[1], "item"));

  return 0;
}

LanceDBTable* create_empty_table(LanceDBConnection* db) {
  const char* table_name = "my_table";
  struct ArrowSchema schema;

  if (create_schema(&schema) != 0) {
    fprintf(stderr, "failed to create schema\n");
    return NULL;
  }

  LanceDBTable* tbl;
  LanceDBError result = lancedb_table_create(db, table_name,
      (FFI_ArrowSchema*)&schema, NULL, &tbl, NULL);

  if (result != LANCEDB_SUCCESS) {
    fprintf(stderr, "error creating table %s: %s\n", table_name, lancedb_error_to_message(result));
    ArrowSchemaRelease(&schema);
    return NULL;
  }

  printf("created table: %s (empty)\n", table_name);
  return tbl;
}

int main() {
  // initial cleanup
  const char* data_dir = "data";
  if (directory_exists(data_dir)) {
    printf("removing existing directory: %s\n", data_dir);
    remove_directory(data_dir);
  }

  // connect to database
  char uri[256];
  snprintf(uri, sizeof(uri), "%s/sample-lancedb", data_dir);

  LanceDBConnectBuilder* builder = lancedb_connect(uri);
  if (!builder) {
    fprintf(stderr, "failed to create connection builder\n");
    return 1;
  }

  LanceDBConnection* db = lancedb_connect_builder_execute(builder);
  if (!db) {
    lancedb_connect_builder_free(builder);
    fprintf(stderr, "failed to connect to database\n");
    return 1;
  }

  // create empty table
  LanceDBTable* tbl = create_empty_table(db);
  lancedb_table_free(tbl);

  // drop table
  LanceDBError result = lancedb_connection_drop_table(db, "my_table", NULL, NULL);
  if (result != LANCEDB_SUCCESS) {
    fprintf(stderr, "error dropping table: %s\n", lancedb_error_to_message(result));
  } else {
    printf("dropped table 'my_table'\n");
  }

  lancedb_connection_free(db);
  return 0;
}

