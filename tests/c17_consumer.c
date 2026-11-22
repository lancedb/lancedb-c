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

static void release_arrow_schema(struct ArrowSchema* schema) {
  schema->release = NULL;
}

static void release_arrow_array(struct ArrowArray* array) {
  array->release = NULL;
}

/* Build one table row with a text column and a one-element vector column.
 * This keeps the phrase API checks in a real table without a C++ dependency. */
static int create_phrase_table(LanceDBConnection* connection, LanceDBTable** table_out) {
  const char text[] = "alpha beta";
  const int32_t text_offsets[] = {0, (int32_t)(sizeof(text) - 1)};
  const float vector[] = {0.0f};

  struct ArrowSchema item_schema = {
      .format = "f",
      .name = "item",
      .metadata = NULL,
      .flags = ARROW_FLAG_NULLABLE,
      .n_children = 0,
      .children = NULL,
      .dictionary = NULL,
      .release = release_arrow_schema,
      .private_data = NULL,
  };
  struct ArrowSchema* vector_children[] = {&item_schema};
  struct ArrowSchema text_schema = {
      .format = "u",
      .name = "text",
      .metadata = NULL,
      .flags = ARROW_FLAG_NULLABLE,
      .n_children = 0,
      .children = NULL,
      .dictionary = NULL,
      .release = release_arrow_schema,
      .private_data = NULL,
  };
  struct ArrowSchema vector_schema = {
      .format = "+w:1",
      .name = "data",
      .metadata = NULL,
      .flags = ARROW_FLAG_NULLABLE,
      .n_children = 1,
      .children = vector_children,
      .dictionary = NULL,
      .release = release_arrow_schema,
      .private_data = NULL,
  };
  struct ArrowSchema* fields[] = {&text_schema, &vector_schema};
  struct ArrowSchema schema = {
      .format = "+s",
      .name = "",
      .metadata = NULL,
      .flags = 0,
      .n_children = 2,
      .children = fields,
      .dictionary = NULL,
      .release = release_arrow_schema,
      .private_data = NULL,
  };

  const void* text_buffers[] = {NULL, text_offsets, text};
  struct ArrowArray text_array = {
      .length = 1,
      .null_count = 0,
      .offset = 0,
      .n_buffers = 3,
      .n_children = 0,
      .buffers = text_buffers,
      .children = NULL,
      .dictionary = NULL,
      .release = release_arrow_array,
      .private_data = NULL,
  };
  const void* vector_value_buffers[] = {NULL, vector};
  struct ArrowArray vector_values = {
      .length = 1,
      .null_count = 0,
      .offset = 0,
      .n_buffers = 2,
      .n_children = 0,
      .buffers = vector_value_buffers,
      .children = NULL,
      .dictionary = NULL,
      .release = release_arrow_array,
      .private_data = NULL,
  };
  struct ArrowArray* vector_array_children[] = {&vector_values};
  const void* vector_buffers[] = {NULL};
  struct ArrowArray vector_array = {
      .length = 1,
      .null_count = 0,
      .offset = 0,
      .n_buffers = 1,
      .n_children = 1,
      .buffers = vector_buffers,
      .children = vector_array_children,
      .dictionary = NULL,
      .release = release_arrow_array,
      .private_data = NULL,
  };
  struct ArrowArray* children[] = {&text_array, &vector_array};
  const void* struct_buffers[] = {NULL};
  struct ArrowArray batch = {
      .length = 1,
      .null_count = 0,
      .offset = 0,
      .n_buffers = 1,
      .n_children = 2,
      .buffers = struct_buffers,
      .children = children,
      .dictionary = NULL,
      .release = release_arrow_array,
      .private_data = NULL,
  };

  LanceDBRecordBatchReader* reader = NULL;
  char* error_message = NULL;
  LanceDBError status = lancedb_record_batch_reader_from_arrow(
      (const FFI_ArrowArray*)&batch,
      (const FFI_ArrowSchema*)&schema,
      &reader,
      &error_message);
  if (status != LANCEDB_SUCCESS || reader == NULL || error_message != NULL) {
    lancedb_free_string(error_message);
    lancedb_record_batch_reader_free(reader);
    return fail("creating the C17 phrase table reader failed");
  }

  status = lancedb_table_create(connection, "phrase_query_test",
                                (const FFI_ArrowSchema*)&schema, reader,
                                table_out, &error_message);
  if (status != LANCEDB_SUCCESS || *table_out == NULL || error_message != NULL) {
    lancedb_free_string(error_message);
    return fail("creating the C17 phrase table failed");
  }
  return 0;
}

static int create_phrase_fts_index(LanceDBTable* table) {
  const char* columns[] = {"text"};
  LanceDBIndexOptions options = lancedb_index_options_default();
  options.fts_with_position = 1;

  char* error_message = NULL;
  const LanceDBError status = lancedb_table_create_index(
      table, columns, 1, LANCEDB_INDEX_FTS, &options, &error_message);
  if (status != LANCEDB_SUCCESS || error_message != NULL) {
    lancedb_free_string(error_message);
    return fail("creating the C17 phrase full-text index failed");
  }
  return 0;
}

static int query_result_rows(LanceDBQueryResult* result, size_t* rows_out) {
  struct FFI_ArrowArray** arrays = NULL;
  struct FFI_ArrowSchema* schema = NULL;
  size_t count = 0;
  char* error_message = NULL;
  LanceDBError status = lancedb_query_result_to_arrow(
      result, &arrays, &schema, &count, &error_message);
  if (status != LANCEDB_SUCCESS || error_message != NULL) {
    lancedb_free_string(error_message);
    lancedb_free_arrow_arrays(arrays, count);
    lancedb_free_arrow_schema(schema);
    return fail("reading the C17 phrase query result failed");
  }

  size_t rows = 0;
  for (size_t i = 0; i < count; i++) {
    const struct ArrowArray* batch = (const struct ArrowArray*)arrays[i];
    rows += (size_t)batch->length;
  }
  lancedb_free_arrow_arrays(arrays, count);
  lancedb_free_arrow_schema(schema);
  *rows_out = rows;
  return 0;
}

static int execute_query_rows(LanceDBQuery* query, size_t* rows_out) {
  LanceDBQueryResult* result = NULL;
  char* error_message = NULL;
  const LanceDBError status = lancedb_query_execute_with_status(
      query, &result, &error_message);
  if (status != LANCEDB_SUCCESS || result == NULL || error_message != NULL) {
    lancedb_free_string(error_message);
    lancedb_query_result_free(result);
    return fail("executing the C17 phrase query failed");
  }
  return query_result_rows(result, rows_out);
}

static int execute_vector_query_rows(LanceDBVectorQuery* query, size_t* rows_out) {
  LanceDBQueryResult* result = NULL;
  char* error_message = NULL;
  const LanceDBError status = lancedb_vector_query_execute_with_status(
      query, &result, &error_message);
  if (status != LANCEDB_SUCCESS || result == NULL || error_message != NULL) {
    lancedb_free_string(error_message);
    lancedb_query_result_free(result);
    return fail("executing the C17 vector phrase query failed");
  }
  return query_result_rows(result, rows_out);
}

static int run_fts_query_cases(LanceDBConnection* connection) {
  LanceDBTable* table = NULL;
  if (create_phrase_table(connection, &table) != 0) {
    return 1;
  }
  if (create_phrase_fts_index(table) != 0) {
    lancedb_table_free(table);
    return 1;
  }

  int result = 0;
  char* error_message = NULL;
  LanceDBFtsQuery* fts_query = NULL;
  LanceDBQuery* first = NULL;
  LanceDBQuery* second = NULL;
  LanceDBVectorQuery* vector_query = NULL;
  size_t rows = 0;

  /* The constructor copies its inputs. Attach the same handle to two builders,
   * free it, then execute both to prove the setters cloned it. */
  char phrase_text[] = "alpha beta";
  if (lancedb_fts_query_new_phrase(
          phrase_text, "text", 0, &fts_query, &error_message) != LANCEDB_SUCCESS ||
      fts_query == NULL || error_message != NULL) {
    result = fail("creating the C17 phrase query failed");
    goto cleanup;
  }
  phrase_text[0] = 'z';

  first = lancedb_query_new(table);
  second = lancedb_query_new(table);
  if (first == NULL || second == NULL) {
    result = fail("creating C17 phrase builders failed");
    goto cleanup;
  }
  if (lancedb_query_full_text_search_query(first, fts_query, &error_message)
          != LANCEDB_SUCCESS ||
      error_message != NULL ||
      lancedb_query_full_text_search_query(second, fts_query, &error_message)
          != LANCEDB_SUCCESS ||
      error_message != NULL) {
    result = fail("attaching the C17 phrase query to both builders failed");
    goto cleanup;
  }
  lancedb_fts_query_free(fts_query);
  fts_query = NULL;

  if (execute_query_rows(first, &rows) != 0 || rows != 1) {
    first = NULL;
    result = fail("the first C17 phrase builder did not copy the phrase text");
    goto cleanup;
  }
  first = NULL;
  if (execute_query_rows(second, &rows) != 0 || rows != 1) {
    second = NULL;
    result = fail("the second C17 phrase builder did not copy the phrase text");
    goto cleanup;
  }
  second = NULL;

  /* Attach the same handle to a hybrid vector builder before freeing it. */
  if (lancedb_fts_query_new_phrase(
          "alpha beta", "text", 0, &fts_query, &error_message) != LANCEDB_SUCCESS ||
      fts_query == NULL || error_message != NULL) {
    result = fail("creating the C17 vector phrase query failed");
    goto cleanup;
  }
  const float query_vector[] = {0.0f};
  vector_query = lancedb_vector_query_new(table, query_vector, 1);
  if (vector_query == NULL ||
      lancedb_vector_query_column(vector_query, "data", &error_message)
          != LANCEDB_SUCCESS ||
      error_message != NULL ||
      lancedb_vector_query_limit(vector_query, 1, &error_message)
          != LANCEDB_SUCCESS ||
      error_message != NULL ||
      lancedb_vector_query_full_text_search_query(
          vector_query, fts_query, &error_message) != LANCEDB_SUCCESS ||
      error_message != NULL) {
    result = fail("attaching the C17 phrase query to a vector builder failed");
    goto cleanup;
  }
  lancedb_fts_query_free(fts_query);
  fts_query = NULL;
  if (execute_vector_query_rows(vector_query, &rows) != 0 || rows != 1) {
    vector_query = NULL;
    result = fail("the C17 vector phrase query did not return its row");
    goto cleanup;
  }
  vector_query = NULL;

  /* A failed setter must leave the previous phrase in place. The phrase
   * matches no row, while a plain scan of this table returns one row. */
  first = lancedb_query_new(table);
  if (first == NULL ||
      lancedb_fts_query_new_phrase(
          "gamma", "text", 0, &fts_query, &error_message) != LANCEDB_SUCCESS ||
      fts_query == NULL || error_message != NULL ||
      lancedb_query_full_text_search_query(first, fts_query, &error_message)
          != LANCEDB_SUCCESS ||
      error_message != NULL) {
    result = fail("setting the C17 preservation phrase failed");
    goto cleanup;
  }
  lancedb_fts_query_free(fts_query);
  fts_query = NULL;

  error_message = NULL;
  if (lancedb_query_full_text_search_query(first, NULL, &error_message)
          != LANCEDB_INVALID_ARGUMENT ||
      error_message == NULL) {
    result = fail("a NULL C17 phrase handle must be rejected");
    goto cleanup;
  }
  lancedb_free_string(error_message);
  error_message = NULL;

  if (execute_query_rows(first, &rows) != 0 || rows != 0) {
    first = NULL;
    result = fail("a failed phrase setter changed the previous builder setting");
    goto cleanup;
  }
  first = NULL;

  /* Invalid constructors clear the output handle and return INVALID_ARGUMENT. */
  const char invalid_utf8[] = {(char)0xff, '\0'};
  fts_query = NULL;
  if (lancedb_fts_query_new_phrase(
          "alpha beta", "text", 0, &fts_query, &error_message) != LANCEDB_SUCCESS ||
      fts_query == NULL || error_message != NULL) {
    result = fail("creating the C17 invalid-input sentinel failed");
    goto cleanup;
  }
  LanceDBFtsQuery* valid = fts_query;
  if (lancedb_fts_query_new_phrase(
          NULL, "text", 0, &fts_query, &error_message) != LANCEDB_INVALID_ARGUMENT ||
      fts_query != NULL || error_message == NULL) {
    result = fail("a NULL C17 phrase text must be rejected and clear the output");
    goto cleanup;
  }
  lancedb_free_string(error_message);
  error_message = NULL;
  lancedb_fts_query_free(valid);

  fts_query = NULL;
  if (lancedb_fts_query_new_phrase(
          invalid_utf8, "text", 0, &fts_query, &error_message)
          != LANCEDB_INVALID_ARGUMENT ||
      fts_query != NULL || error_message == NULL) {
    result = fail("invalid UTF-8 C17 phrase text must be rejected");
    goto cleanup;
  }
  lancedb_free_string(error_message);
  error_message = NULL;

  fts_query = NULL;
  if (lancedb_fts_query_new_phrase(
          "alpha beta", invalid_utf8, 0, &fts_query, &error_message)
          != LANCEDB_INVALID_ARGUMENT ||
      fts_query != NULL || error_message == NULL) {
    result = fail("invalid UTF-8 C17 phrase column must be rejected");
    goto cleanup;
  }
  lancedb_free_string(error_message);
  error_message = NULL;

  lancedb_fts_query_free(NULL);

cleanup:
  lancedb_free_string(error_message);
  lancedb_fts_query_free(fts_query);
  lancedb_query_free(first);
  lancedb_query_free(second);
  lancedb_vector_query_free(vector_query);
  lancedb_table_free(table);
  return result;
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

  if (run_fts_query_cases(connection) != 0) {
    lancedb_connection_free(connection);
    return 1;
  }

  lancedb_connection_free(connection);
  printf("C17 consumer: OK (version %s)\n", runtime_version);
  return 0;
}
