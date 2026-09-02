/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#ifndef LANCEDB_H
#define LANCEDB_H

#include <stddef.h>
#include <stdint.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque handle to a LanceDB ConnectBuilder
 */
typedef struct LanceDBConnectBuilder LanceDBConnectBuilder;

/**
 * Opaque handle to a LanceDB Connection
 */
typedef struct LanceDBConnection LanceDBConnection;

/**
 * Opaque handle to a LanceDB Table
 */
typedef struct LanceDBTable LanceDBTable;

/**
 * Opaque handle to a LanceDB TableNamesBuilder
 */
typedef struct LanceDBTableNamesBuilder LanceDBTableNamesBuilder;

/**
 * Opaque handle to a DataFusion Expr (for building filter expressions)
 */
typedef struct LanceDBExpr LanceDBExpr;

/**
 * Opaque handle to a LanceDB Query
 */
typedef struct LanceDBQuery LanceDBQuery;

/**
 * Opaque handle to a LanceDB VectorQuery
 */
typedef struct LanceDBVectorQuery LanceDBVectorQuery;

/**
 * Opaque handle to a LanceDB QueryResult
 */
typedef struct LanceDBQueryResult LanceDBQueryResult;

/**
 * Opaque handle to a LanceDB Session
 */
typedef struct LanceDBSession LanceDBSession;

/**
 * Opaque handle to a LanceDB ObjectStoreRegistry
 */
typedef struct LanceDBObjectStoreRegistry LanceDBRegistry;

/**
 * Opaque handle to a LanceDB ObjectStoreProvider
 */
typedef struct LanceDBObjectStoreProvider LanceDBObjectStoreProvider;

/**
 * Opaque handle to Arrow RecordBatchReader
 */
typedef struct LanceDBRecordBatchReader LanceDBRecordBatchReader;

/**
 * Arrow C ABI Schema structure (opaque)
 */
typedef struct FFI_ArrowSchema FFI_ArrowSchema;

/**
 * Arrow C ABI Array structure (opaque)
 */
typedef struct FFI_ArrowArray FFI_ArrowArray;

/**
 * Error codes for LanceDB C API
 */
typedef enum {
    LANCEDB_SUCCESS = 0,
    LANCEDB_INVALID_ARGUMENT = 1,
    LANCEDB_INVALID_TABLE_NAME = 2,
    LANCEDB_INVALID_INPUT = 3,
    LANCEDB_TABLE_NOT_FOUND = 4,
    LANCEDB_DATABASE_NOT_FOUND = 5,
    LANCEDB_DATABASE_ALREADY_EXISTS = 6,
    LANCEDB_INDEX_NOT_FOUND = 7,
    LANCEDB_EMBEDDING_FUNCTION_NOT_FOUND = 8,
    LANCEDB_TABLE_ALREADY_EXISTS = 9,
    LANCEDB_CREATE_DIR = 10,
    LANCEDB_SCHEMA = 11,
    LANCEDB_RUNTIME = 12,
    LANCEDB_TIMEOUT = 13,
    LANCEDB_OBJECT_STORE = 14,
    LANCEDB_LANCE = 15,
    LANCEDB_HTTP = 16,
    LANCEDB_RETRY = 17,
    LANCEDB_ARROW = 18,
    LANCEDB_NOT_SUPPORTED = 19,
    LANCEDB_OTHER = 20,
    LANCEDB_NAMESPACE = 21,
    LANCEDB_PERMISSION_DENIED = 22,
    LANCEDB_NOT_FOUND = 23,
    /* keep last, so that new error codes can be added without renumbering */
    LANCEDB_UNKNOWN = 99
} LanceDBError;

/**
 * Error messages for LanceDB C API
 */
static const char* LANCEDB_ERROR_MESSAGES[] = {
    "Success",
    "Invalid argument",
    "Invalid table name",
    "Invalid input",
    "Table not found",
    "Database not found",
    "Database already exists",
    "Index not found",
    "Embedding function not found",
    "Table already exists",
    "Failed to create directory",
    "Schema error",
    "Runtime error",
    "Operation timed out",
    "Object store error",
    "Lance format error",
    "HTTP error",
    "Retryable error",
    "Arrow error",
    "Operation not supported",
    "Other error",
    "Namespace error",
    "Permission denied",
    "Not found"
};

/**
 * Error message for LANCEDB_UNKNOWN, which is not part of LANCEDB_ERROR_MESSAGES
 * since it is deliberately kept out of the range of the other error codes
 */
static const char* LANCEDB_UNKNOWN_ERROR_MESSAGE = "Unknown error";

/**
 * Convert error code to error message
 *
 * @param error - error code
 * @return Pointer to null-terminated C string containing the error message
 *
 * The returned string is valid for the lifetime of the program.
 * The caller must not free the returned string.
 */
[[maybe_unused]] static const char* lancedb_error_to_message(LanceDBError error) {
    if (error == LANCEDB_UNKNOWN) {
        return LANCEDB_UNKNOWN_ERROR_MESSAGE;
    }
    if (error >= sizeof(LANCEDB_ERROR_MESSAGES)/sizeof(LANCEDB_ERROR_MESSAGES[0])) {
        return "Invalid error code";
    }
    return LANCEDB_ERROR_MESSAGES[error];
}

/**
 * Distance type enum for vector search
 */
typedef enum {
    LANCEDB_DISTANCE_L2 = 0,
    LANCEDB_DISTANCE_COSINE = 1,
    LANCEDB_DISTANCE_DOT = 2,
    LANCEDB_DISTANCE_HAMMING = 3
} LanceDBDistanceType;

/**
 * Index type enum
 */
typedef enum {
    LANCEDB_INDEX_AUTO = 0,
    LANCEDB_INDEX_BTREE = 1,
    LANCEDB_INDEX_BITMAP = 2,
    LANCEDB_INDEX_LABELLIST = 3,
    LANCEDB_INDEX_FTS = 4,
    LANCEDB_INDEX_IVF_FLAT = 5,
    LANCEDB_INDEX_IVF_PQ = 6,
    LANCEDB_INDEX_IVF_HNSW_PQ = 7,
    LANCEDB_INDEX_IVF_HNSW_SQ = 8
} LanceDBIndexType;

/**
 * Optimize type enum
 */
typedef enum {
    LANCEDB_OPTIMIZE_ALL = 0,     // Compact files and prune old versions
    LANCEDB_OPTIMIZE_COMPACT = 1, // Only compact files
    LANCEDB_OPTIMIZE_PRUNE = 2,   // Only prune old versions
    LANCEDB_OPTIMIZE_INDEX = 3    // Only rebuild indices
} LanceDBOptimizeType;

/**
 * Binary operator enum for DataFusion expressions
 */
typedef enum {
    LANCEDB_BINARY_OP_EQ = 0,
    LANCEDB_BINARY_OP_NOT_EQ = 1,
    LANCEDB_BINARY_OP_LT = 2,
    LANCEDB_BINARY_OP_LT_EQ = 3,
    LANCEDB_BINARY_OP_GT = 4,
    LANCEDB_BINARY_OP_GT_EQ = 5,
    LANCEDB_BINARY_OP_AND = 6,
    LANCEDB_BINARY_OP_OR = 7,
    LANCEDB_BINARY_OP_PLUS = 8,
    LANCEDB_BINARY_OP_MINUS = 9,
    LANCEDB_BINARY_OP_MULTIPLY = 10,
    LANCEDB_BINARY_OP_DIVIDE = 11,
    LANCEDB_BINARY_OP_MODULO = 12
} LanceDBBinaryOp;

/**
 * Vector index configuration
 */
typedef struct {
    int num_partitions;         // Number of partitions for IVF indices (-1 = auto)
    int num_sub_vectors;        // Number of sub-vectors for PQ indices (-1 = auto)
    int max_iterations;         // Maximum training iterations (-1 = default)
    float sample_rate;          // Sampling rate for training (0.0 = default)
    LanceDBDistanceType distance_type; // Distance metric
    int replace;               // Replace existing index (1 = true, 0 = false)
} LanceDBVectorIndexConfig;

/**
 * Scalar index configuration
 */
typedef struct {
    int replace;                      // Replace existing index (1 = true, 0 = false)
} LanceDBScalarIndexConfig;

/**
 * Full-text search index configuration
 */
typedef struct {
    const char* base_tokenizer;   // Base tokenizer ("simple", "whitespace", etc.)
    const char* language;         // Language for stemming ("en", "es", etc.)
    int max_token_length;        // Maximum length of a token (-1 = no limit)
    int lowercase;               // Convert to lowercase (1 = true, 0 = false)
    int stem;                    // Apply stemming (1 = true, 0 = false)
    int remove_stop_words;       // Remove stop words (1 = true, 0 = false)
    int ascii_folding;           // Apply ASCII folding (1 = true, 0 = false)
    int replace;                 // Replace existing index (1 = true, 0 = false)
} LanceDBFtsIndexConfig;

/**
 * Merge insert configuration
 *
 * The conditions restricting which matched records are updated are only used when
 * when_matched_update_all is set, and are otherwise ignored. The SQL condition and the
 * DataFusion expression are mutually exclusive: when matched records are updated, setting
 * both of them fails with LANCEDB_INVALID_ARGUMENT.
 * In both flavors, use the prefix "target." to refer to rows in the table, and the prefix
 * "source." to refer to rows in the merged data. For example: "target.version < source.version"
 */
typedef struct {
    int when_matched_update_all;     // Update all columns for matched records (1 = true, 0 = false)
    int when_not_matched_insert_all; // Insert all new records (1 = true, 0 = false)
    const char* when_matched_update_all_condition;   // SQL condition on the updated records (NULL for no condition)
    LanceDBExpr* when_matched_update_all_expr;       // DataFusion condition on the updated records (NULL for no condition)
} LanceDBMergeInsertConfig;

/**
 * Session creation options
 */
typedef struct {
    size_t index_cache_bytes;        // Index cache size in bytes (0 = default)
    size_t metadata_cache_bytes;     // Metadata cache size in bytes (0 = default)
} LanceDBSessionOptions;

/**
 * Session cache statistics
 */
typedef struct {
    uint64_t hits;          // Number of cache hits
    uint64_t misses;        // Number of cache misses
    size_t num_entries;     // Number of entries in cache
    size_t size_bytes;      // Cache size in bytes
} LanceDBSessionCacheStats;

/**
 * Version information for a table
 */
typedef struct {
    uint64_t version;            // Version number
    int64_t timestamp_seconds;   // Seconds since Unix epoch (UTC)
    uint32_t timestamp_nanos;    // Sub-second nanoseconds
} LanceDBVersion;

/**
 * Per-version metadata (key-value pairs)
 */
typedef struct {
    char** keys;                 // Array of metadata key strings
    char** values;               // Array of metadata value strings
    size_t count;                // Number of metadata key-value pairs
} LanceDBVersionMetadata;

/**
 * Create a ConnectBuilder for the given URI
 *
 * @param uri - null-terminated C string containing the database URI
 * @return Non-null pointer to LanceDBConnectBuilder on success, NULL on failure
 *
 * The URI can be:
 * - "/path/to/database" - local database on file system
 * - "s3://bucket/path/to/database" or "gs://bucket/path/to/database" - database on cloud object store
 * - "db://dbname" - LanceDB Cloud
 *
 * The returned pointer must be freed with lancedb_connect_builder_free().
 */
LanceDBConnectBuilder* lancedb_connect(const char* uri);

/**
 * Execute the connection and return a Connection handle
 *
 * @param builder - pointer to LanceDBConnectBuilder returned from lancedb_connect()
 * @param connection - pointer to receive the LanceDBConnection pointer
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * On success, the builder is consumed by this function and must not be used after calling.
 * The returned connection must be freed with lancedb_connection_free().
 */
LanceDBError lancedb_connect_builder_execute( LanceDBConnectBuilder* builder,
		    LanceDBConnection** connection, char** error_message);


/**
 * Set an option for the storage layer
 * @param builder - pointer to LanceDBConnectBuilder returned from lancedb_connect()
 * @param key -  null-terminated C string containing the name of the option
 * @param value -  null-terminated C string containing the value of the option
 * @return Non-null pointer to LanceDBConnectBuilder on success, NULL on failure
 *
 * The builder is consumed by this function and must not be used after calling.
 *
 * The key and value are going through basic validation at this point, and error will happen when trying to execute a builder
 * with unsupported key or value.
 */
LanceDBConnectBuilder* lancedb_connect_builder_storage_option(LanceDBConnectBuilder* builder, const char* key, const char* value);

/**
 * Set session for the connection builder
 *
 * @param builder - pointer to LanceDBConnectBuilder returned from lancedb_connect()
 * @param session - pointer to LanceDBSession, or NULL to keep current/default session behavior
 * @return Non-null pointer to LanceDBConnectBuilder on success, NULL on failure
 *
 * The builder is consumed by this function and must not be used after calling.
 * Passing NULL session is a no-op.
 */
LanceDBConnectBuilder* lancedb_connect_builder_session(LanceDBConnectBuilder* builder, const LanceDBSession* session);

/**
 * Free a ConnectBuilder
 *
 * @param builder - pointer to LanceDBConnectBuilder returned from lancedb_connect()
 *
 * After calling this function, the builder pointer must not be used.
 */
void lancedb_connect_builder_free(LanceDBConnectBuilder* builder);

/**
 * Get the URI of the connection
 *
 * @param connection - pointer to LanceDBConnection
 * @return Pointer to null-terminated C string containing the URI, or NULL on failure
 *
 * The returned string is valid until the connection is freed.
 * The caller must not free the returned string.
 */
const char* lancedb_connection_uri(const LanceDBConnection* connection);

/**
 * Get table names from the connection
 *
 * @param connection - pointer to LanceDBConnection
 * @param names_out - pointer to receive array of string pointers
 * @param count_out - pointer to receive count of table names
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * The caller is responsible for freeing the returned strings and array
 * using lancedb_free_table_names(). If error_message is provided and an error
 * occurs, the caller must free the error message with lancedb_free_string().
 */
LanceDBError lancedb_connection_table_names(
    const LanceDBConnection* connection,
    char*** names_out,
    size_t* count_out,
    char** error_message
);

/**
 * Free table names array returned by lancedb_connection_table_names
 *
 * @param names - array of string pointers returned by lancedb_connection_table_names
 * @param count - number of strings in the array
 */
void lancedb_free_table_names(char** names, size_t count);

/**
 * Create a TableNamesBuilder for paginated table listing
 *
 * @param connection - pointer to LanceDBConnection
 * @return Non-null pointer to LanceDBTableNamesBuilder on success, NULL on failure
 *
 * The returned pointer must be freed with lancedb_table_names_builder_free().
 * Use lancedb_table_names_builder_limit() and lancedb_table_names_builder_start_after()
 * to configure pagination before calling lancedb_table_names_builder_execute().
 */
LanceDBTableNamesBuilder* lancedb_connection_table_names_builder(
    const LanceDBConnection* connection
);

/**
 * Set limit on TableNamesBuilder
 *
 * @param builder - pointer to LanceDBTableNamesBuilder
 * @param limit - maximum number of table names to return
 * @return Non-null pointer to LanceDBTableNamesBuilder on success, NULL on failure
 *
 * The builder is consumed by this function and must not be used after calling.
 */
LanceDBTableNamesBuilder* lancedb_table_names_builder_limit(
    LanceDBTableNamesBuilder* builder,
    unsigned int limit
);

/**
 * Set start_after on TableNamesBuilder for pagination
 *
 * @param builder - pointer to LanceDBTableNamesBuilder
 * @param start_after - null-terminated C string containing the table name to start after
 * @return Non-null pointer to LanceDBTableNamesBuilder on success, NULL on failure
 *
 * The builder is consumed by this function and must not be used after calling.
 * This is used for pagination - returns table names alphabetically after the specified name.
 */
LanceDBTableNamesBuilder* lancedb_table_names_builder_start_after(
    LanceDBTableNamesBuilder* builder,
    const char* start_after
);

/**
 * Execute TableNamesBuilder and return table names
 *
 * @param builder - pointer to LanceDBTableNamesBuilder (consumed by this function)
 * @param names_out - pointer to receive array of string pointers
 * @param count_out - pointer to receive count of table names
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * The builder is consumed by this function and must not be used after calling.
 * The caller is responsible for freeing the returned strings and array
 * using lancedb_free_table_names(). If error_message is provided and an error
 * occurs, the caller must free the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_names_builder_execute(
    LanceDBTableNamesBuilder* builder,
    char*** names_out,
    size_t* count_out,
    char** error_message
);

/**
 * Free a TableNamesBuilder
 *
 * @param builder - pointer to LanceDBTableNamesBuilder
 *
 * After calling this function, the builder pointer must not be used.
 */
void lancedb_table_names_builder_free(LanceDBTableNamesBuilder* builder);

/**
 * Open an existing table
 *
 * @param connection - pointer to LanceDBConnection
 * @param table_name - null-terminated C string containing the table name
 * @param table_out - pointer to receive the opened table
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure.
 *
 * On success, the table returned in table_out must be freed with lancedb_table_free().
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_connection_open_table(
    const LanceDBConnection* connection,
    const char* table_name,
    LanceDBTable** table_out,
    char** error_message
);

/**
 * Drop a table from the database
 *
 * @param connection - pointer to LanceDBConnection
 * @param table_name - null-terminated C string containing the table name
 * @param _namespace - null-terminated C string containing the namespace (NULL for no namespace)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_connection_drop_table(
    const LanceDBConnection* connection,
    const char* table_name,
    const char* _namespace,
    char** error_message
);

/**
 * Rename a table in the database
 *
 * @param connection - pointer to LanceDBConnection
 * @param old_name - null-terminated C string containing the current table name
 * @param new_name - null-terminated C string containing the new table name
 * @param cur_namespace - null-terminated C string containing the current namespace (NULL for no namespace)
 * @param new_namespace - null-terminated C string containing the new namespace (NULL for no namespace)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * This operation is only supported in LanceDB Cloud.
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_connection_rename_table(
    const LanceDBConnection* connection,
    const char* old_name,
    const char* new_name,
    const char* cur_namespace,
    const char* new_namespace,
    char** error_message
);

/**
 * Drop all tables in the database
 *
 * @param connection - pointer to LanceDBConnection
 * @param _namespace - null-terminated C string containing the namespace (NULL for no namespace)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_connection_drop_all_tables(
    const LanceDBConnection* connection,
    const char* _namespace,
    char** error_message
);

/**
 * Create a new namespace
 *
 * @param connection - pointer to LanceDBConnection
 * @param namespace_name - null-terminated C string containing the namespace name
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_connection_create_namespace(
    const LanceDBConnection* connection,
    const char* namespace_name,
    char** error_message
);

/**
 * Drop a namespace
 *
 * @param connection - pointer to LanceDBConnection
 * @param namespace_name - null-terminated C string containing the namespace name
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_connection_drop_namespace(
    const LanceDBConnection* connection,
    const char* namespace_name,
    char** error_message
);

/**
 * List all namespaces in the database
 *
 * @param connection - pointer to LanceDBConnection
 * @param namespace_parent - null-terminated C string containing the parent namespace (NULL for root)
 * @param namespaces_out - pointer to receive array of string pointers
 * @param count_out - pointer to receive count of namespace names
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * The caller is responsible for freeing the returned strings and array
 * using lancedb_free_namespace_list(). If error_message is provided and an error
 * occurs, the caller must free the error message with lancedb_free_string().
 */
LanceDBError lancedb_connection_list_namespaces(
    const LanceDBConnection* connection,
    const char* namespace_parent,
    char*** namespaces_out,
    size_t* count_out,
    char** error_message
);

/**
 * Free namespace list array returned by lancedb_connection_list_namespaces
 *
 * @param namespaces - pointer to array returned by lancedb_connection_list_namespaces()
 * @param count - count returned by lancedb_connection_list_namespaces()
 *
 * After calling this function, the namespaces pointer must not be used.
 */
void lancedb_free_namespace_list(char** namespaces, size_t count);

/**
 * Free a Connection
 *
 * @param connection - pointer to LanceDBConnection returned from lancedb_connect_builder_execute()
 *
 * After calling this function, the connection pointer must not be used.
 */
void lancedb_connection_free(LanceDBConnection* connection);

/**
 * Create a new session
 *
 * @param options - pointer to LanceDBSessionOptions, or NULL for defaults
 * @return Non-null pointer to LanceDBSession on success, NULL on failure
 *
 * The returned session must be freed with lancedb_session_free().
 */
LanceDBSession* lancedb_session_new(const LanceDBSessionOptions* options);

/**
 * Create a new session with a custom ObjectStoreRegistry
 *
 * @param options - pointer to LanceDBSessionOptions, or NULL for defaults
 * @param registry - pointer to LanceDBObjectStoreRegistry, or NULL to use a default registry.
 *
 * - The registry must be created using "lancedb_registry_new()" API
 * - When registry is non-NULL, this function takes ownership of it and it
 *   will be released when the session is freed.
 * - If registry is NULL, a default ObjectStoreRegistry is created internally.
 *
 * @return Non-null pointer to LanceDBSession on success, NULL on failure
 *
 * The returned session must be freed with lancedb_session_free().
 */
LanceDBSession* lancedb_session_new_with_registry( const LanceDBSessionOptions* options,
					    const LanceDBObjectStoreRegistry* registry);

/**
 * Create a new ObjectStoreRegistry
 *
 * Creates a new registry with default ObjectStore providers (local filesystem, S3, etc.).
 * After creation, custom providers can be registered which can even override the
 * default ones for existing scheme using "lancedb_registry_insert_provider()"
 *
 * - If the registry created is passed to a session, ownership will be transferred
 * - If NOT passed to a session, it must be freed with lancedb_registry_free()
 *
 * @return Non-null pointer to LanceDBObjectStoreRegistry on success, NULL on failure
 */
LanceDBObjectStoreRegistry* lancedb_registry_new(void);

/**
 * Free an ObjectStoreRegistry
 *
 * @param registry - pointer to LanceDBObjectStoreRegistry to free
 *
 * Only call this if the registry was NOT passed to any session.
 * If the registry was passed to a session, the session owns it and will free it.
 */
void lancedb_registry_free(LanceDBObjectStoreRegistry* registry);

/**
 * Insert or override an ObjectStoreProvider into an ObjectStoreRegistry for a given URL scheme
 *
 * @param registry - pointer to LanceDBObjectStoreRegistry
 * @param scheme - URL scheme to register the provider for (e.g., "s3")
 * @param provider - pointer to LanceDBObjectStoreProvider. Ownership is transferred
 *                   to the registry; do not free the provider after this call.
 *
 * @return 0 on success, -1 on failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_registry_insert_provider( LanceDBObjectStoreRegistry* registry,
			    const char* scheme, LanceDBObjectStoreProvider* provider,
    			    char** error_message);

/**
 * Free an ObjectStoreProvider
 *
 * @param provider - pointer to LanceDBObjectStoreProvider to free
 *
 * Only call this if the provider was NOT passed to lancedb_registry_insert_provider().
 * If the provider was inserted into a registry, the registry owns it.
 */
void lancedb_object_store_provider_free(LanceDBObjectStoreProvider* provider);

/**
 * Get index cache stats for a session
 *
 * @param session - pointer to LanceDBSession
 * @param out_stats - pointer to receive session cache statistics
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_session_index_cache_stats(
    const LanceDBSession* session,
    LanceDBSessionCacheStats* out_stats,
    char** error_message
);

/**
 * Get metadata cache stats for a session
 *
 * @param session - pointer to LanceDBSession
 * @param out_stats - pointer to receive session cache statistics
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_session_metadata_cache_stats(
    const LanceDBSession* session,
    LanceDBSessionCacheStats* out_stats,
    char** error_message
);

/**
 * Free a Session
 *
 * @param session - pointer to LanceDBSession
 *
 * After calling this function, the session pointer must not be used.
 */
void lancedb_session_free(LanceDBSession* session);

/**
 * Free a Table
 *
 * @param table - pointer to LanceDBTable returned from lancedb_connection_open_table()
 *
 * After calling this function, the table pointer must not be used.
 */
void lancedb_table_free(LanceDBTable* table);

/**
 * Threading model
 *
 * Every database-executing function in this API is synchronous from the
 * caller's point of view, but the work itself runs on the library's own
 * tokio worker threads: the calling thread only blocks until the operation
 * completes. The caller's stack is therefore never used for query planning
 * or execution, so the API is safe to call from threads, coroutines or
 * fibers with small fixed-size stacks. The worker stack size defaults to
 * 8 MiB and can be overridden with the LANCEDB_C_WORKER_STACK_SIZE
 * environment variable (bytes); LANCEDB_C_WORKER_THREADS overrides the
 * worker count.
 *
 * Do not call this API from within a tokio async context (e.g. from a task
 * running on the library's own runtime); doing so is reported as
 * LANCEDB_ERROR_RUNTIME.
 *
 * Arrow objects handed to the library (record batch readers, schemas) may be
 * consumed and released on those worker threads, i.e. on a different thread
 * than the one that produced them. Producers must therefore tolerate
 * release callbacks from another thread — the Arrow C data interface does
 * not require this by itself.
 *
 * lancedb_run_on_stack() below is no longer required for the above and is
 * retained for compatibility with callers that already use it.
 */
/**
 * Run a callback on an extended stack.
 *
 * If the remaining stack is below red_zone bytes, a new stack segment
 * of stack_size bytes is allocated before calling the callback.
 * Use this to wrap coroutine bodies so that inner FFI calls can reuse
 * the same stack segment instead of allocating one per call.
 *
 * The callback must not unwind across the FFI boundary (no C++ exceptions,
 * no longjmp). In C++ code, wrap the callback body in try/catch.
 * Using an extern "C" function is recommended, though a non-capturing
 * lambda will compile on all major compilers as well.
 *
 * @param callback - function pointer to call on the extended stack (NULL is a safe no-op)
 * @param user_data - opaque pointer passed through to the callback
 * @param red_zone - minimum remaining stack (bytes) before growing
 * @param stack_size - size of the new stack segment (bytes) if growth is needed (clamped to at least red_zone)
 */
void lancedb_run_on_stack(void (*callback)(void*), void* user_data, size_t red_zone, size_t stack_size);

/**
 * Create a new table with Arrow schema
 *
 * @param connection - pointer to LanceDBConnection
 * @param table_name - null-terminated C string containing the table name
 * @param schema_ptr - pointer to Arrow C ABI schema
 * @param reader - pointer to LanceDBRecordBatchReader or NULL for empty table
 * @param table_out - pointer to receive the created table
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * The caller is responsible for freeing the returned table with lancedb_table_free().
 * This function takes ownership of the reader (when not NULL) and frees it automatically.
 * Do NOT call lancedb_record_batch_reader_free() after calling this function.
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_create(
    const LanceDBConnection* connection,
    const char* table_name,
    const FFI_ArrowSchema* schema_ptr,
    LanceDBRecordBatchReader* reader,
    LanceDBTable** table_out,
    char** error_message
);

/**
 * Get table schema as Arrow C ABI
 *
 * @param table - pointer to LanceDBTable
 * @param schema_out - pointer to receive the Arrow schema
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *         Caller is responsible for releasing the schema using Arrow C ABI
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_arrow_schema(
    const LanceDBTable* table,
    FFI_ArrowSchema** schema_out,
    char** error_message
);

/**
 * Get table version
 *
 * @param table - pointer to LanceDBTable
 * @return Table version number on success, 0 on failure
 */
unsigned long long lancedb_table_version(const LanceDBTable* table);

/**
 * Count rows in table
 *
 * @param table - pointer to LanceDBTable
 * @return Number of rows in table on success, 0 on failure (or empty table)
 */
unsigned long long lancedb_table_count_rows(const LanceDBTable* table);

/**
 * Add data to table using Arrow RecordBatchReader
 *
 * @param table - pointer to LanceDBTable
 * @param reader - pointer to LanceDBRecordBatchReader
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * This function takes ownership of the reader and frees it automatically.
 * Do NOT call lancedb_record_batch_reader_free() after calling this function.
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_add(
    const LanceDBTable* table,
    LanceDBRecordBatchReader* reader,
    char** error_message
);

/**
 * Merge insert data into table (upsert operation)
 *
 * @param table - pointer to LanceDBTable
 * @param data - pointer to LanceDBRecordBatchReader containing data to merge
 * @param on_columns - array of column names to join on (typically key/id columns)
 * @param num_columns - number of columns in the on_columns array
 * @param config - pointer to LanceDBMergeInsertConfig for operation behavior (NULL for defaults)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * This function performs an upsert operation by joining on the specified columns.
 * New records are inserted, existing records can be updated based on configuration.
 * This function takes ownership of the reader and frees it automatically.
 * Do NOT call lancedb_record_batch_reader_free() after calling this function.
 * The expression in the config is not consumed, and must still be freed by the caller
 * with lancedb_expr_free().
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_merge_insert(
    const LanceDBTable* table,
    LanceDBRecordBatchReader* data,
    const char* const* on_columns,
    size_t num_columns,
    const LanceDBMergeInsertConfig* config,
    char** error_message
);

/**
 * Delete rows from table based on predicate
 *
 * @param table - pointer to LanceDBTable
 * @param predicate - null-terminated C string containing SQL WHERE clause
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_delete(
    const LanceDBTable* table,
    const char* predicate,
    char** error_message
);

/**
 * Delete rows from table using a DataFusion expression
 *
 * @param table - pointer to LanceDBTable
 * @param expr - pointer to LanceDBExpr (consumed by this function; do not use or free after calling)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_df_delete(
    const LanceDBTable* table,
    LanceDBExpr* expr,
    char** error_message
);

/**
 * Create a new query for the given table
 *
 * @param table - pointer to LanceDBTable
 * @return Pointer to LanceDBQuery on success, NULL on failure
 *         Caller must free with lancedb_query_free()
 */
LanceDBQuery* lancedb_query_new(const LanceDBTable* table);

/**
 * Create a vector query from table with query vector
 *
 * @param table - pointer to LanceDBTable
 * @param vector - array of floats representing the query vector
 * @param dimension - dimension of the vector
 * @return Pointer to LanceDBVectorQuery on success, NULL on failure
 *         Caller must free with lancedb_vector_query_free()
 */
LanceDBVectorQuery* lancedb_vector_query_new(
    const LanceDBTable* table,
    const float* vector,
    size_t dimension
);

/**
 * Set limit for query
 *
 * @param query - pointer to LanceDBQuery
 * @param limit - maximum number of results
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_query_limit(
    LanceDBQuery* query,
    size_t limit,
    char** error_message
);

/**
 * Set offset for query
 *
 * @param query - pointer to LanceDBQuery
 * @param offset - number of results to skip
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_query_offset(
    LanceDBQuery* query,
    size_t offset,
    char** error_message
);

/**
 * Set columns to select for query
 *
 * @param query - pointer to LanceDBQuery
 * @param columns - array of column name strings
 * @param num_columns - number of columns
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_query_select(
    LanceDBQuery* query,
    const char* const* columns,
    size_t num_columns,
    char** error_message
);

/**
 * Set WHERE filter for query
 *
 * If both a SQL WHERE clause and a DataFusion filters are set, the DataFusion expression
 * takes precedence.
 *
 * @param query - pointer to LanceDBQuery
 * @param filter - SQL WHERE clause string
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_query_where_filter(
    LanceDBQuery* query,
    const char* filter,
    char** error_message
);

/**
 * Set DataFusion Expr filter for query
 *
 * If both a DataFusion and a SQL WHERE clause filters are set, the DataFusion expression
 * takes precedence.
 *
 * @param query - pointer to LanceDBQuery
 * @param expr - pointer to LanceDBExpr (consumed by this function; do not use or free after calling)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 */
LanceDBError lancedb_query_df_filter(
    LanceDBQuery* query,
    LanceDBExpr* expr,
    char** error_message
);

/**
 * Set limit for vector query
 *
 * @param query - pointer to LanceDBVectorQuery
 * @param limit - maximum number of results
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_vector_query_limit(
    LanceDBVectorQuery* query,
    size_t limit,
    char** error_message
);

/**
 * Set offset for vector query
 *
 * @param query - pointer to LanceDBVectorQuery
 * @param offset - number of results to skip
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_vector_query_offset(
    LanceDBVectorQuery* query,
    size_t offset,
    char** error_message
);

/**
 * Set vector column for vector query
 *
 * @param query - pointer to LanceDBVectorQuery
 * @param column - vector column name
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_vector_query_column(
    LanceDBVectorQuery* query,
    const char* column,
    char** error_message
);

/**
 * Set columns to select for vector query
 *
 * @param query - pointer to LanceDBVectorQuery
 * @param columns - array of column name strings
 * @param num_columns - number of columns
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_vector_query_select(
    LanceDBVectorQuery* query,
    const char* const* columns,
    size_t num_columns,
    char** error_message
);

/**
 * Set WHERE filter for vector query
 *
 * If both a SQL WHERE clause and a DataFusion filters are set, the DataFusion expression
 * takes precedence.
 *
 * @param query - pointer to LanceDBVectorQuery
 * @param filter - SQL WHERE clause string
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_vector_query_where_filter(
    LanceDBVectorQuery* query,
    const char* filter,
    char** error_message
);

/**
 * Set DataFusion Expr filter for vector query
 *
 * If both a DataFusion and a SQL WHERE clause filters are set, the DataFusion expression
 * takes precedence.
 *
 * @param query - pointer to LanceDBVectorQuery
 * @param expr - pointer to LanceDBExpr (consumed by this function; do not use or free after calling)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 */
LanceDBError lancedb_vector_query_df_filter(
    LanceDBVectorQuery* query,
    LanceDBExpr* expr,
    char** error_message
);

/**
 * Set distance type for vector query
 *
 * @param query - pointer to LanceDBVectorQuery
 * @param distance_type - distance metric to use
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_vector_query_distance_type(
    LanceDBVectorQuery* query,
    LanceDBDistanceType distance_type,
    char** error_message
);

/**
 * Set number of probes for vector query
 *
 * This parameter controls how many partitions to search in IVF (Inverted File) based indices
 * such as IVF_FLAT, IVF_PQ, IVF_HNSW_PQ, and IVF_HNSW_SQ.
 *
 * IVF indices partition the vector space into clusters. During search, nprobes determines
 * how many of these partitions are searched. Higher values improve recall (find more relevant
 * results) at the cost of slower queries. Lower values make queries faster but may miss
 * relevant results that are in non-searched partitions.
 *
 * Typical values:
 * - Default is usually 20
 * - Range: 1 to num_partitions (specified during index creation)
 * - Higher nprobes (e.g., num_partitions/2) → better recall, slower queries
 * - Lower nprobes (e.g., 1-10) → faster queries, lower recall
 *
 * @param query - pointer to LanceDBVectorQuery
 * @param nprobes - number of IVF partitions to search (must be <= num_partitions)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_vector_query_nprobes(
    LanceDBVectorQuery* query,
    size_t nprobes,
    char** error_message
);

/**
 * Set refine factor for vector query
 *
 * This parameter enables a refinement step to improve result accuracy for approximate
 * vector indices (IVF_PQ, IVF_HNSW_PQ, IVF_HNSW_SQ).
 *
 * When refine_factor is set, the search process works in two stages:
 * 1. Fetch (limit × refine_factor) approximate nearest neighbors using the index
 * 2. Re-rank these candidates using exact distance calculations on original vectors
 * 3. Return the top 'limit' results after refinement
 *
 * This improves accuracy because approximate indices (especially quantized ones like PQ/SQ)
 * can have ranking errors. Refinement corrects these errors by recalculating exact distances.
 *
 * Typical values:
 * - Default: No refinement (refine_factor = 1 or not set)
 * - Range: 1 to ~100
 * - refine_factor = 1 → no refinement (fastest, least accurate)
 * - refine_factor = 10 → fetch 10x results and refine (good balance)
 * - refine_factor = 50+ → very accurate but slower
 *
 * Note: Higher values increase query latency proportionally. Not useful for exact indices
 * like IVF_FLAT which already use exact distances.
 *
 * @param query - pointer to LanceDBVectorQuery
 * @param refine_factor - multiplier for candidate set size (fetches limit × refine_factor results)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_vector_query_refine_factor(
    LanceDBVectorQuery* query,
    unsigned int refine_factor,
    char** error_message
);

/**
 * Set ef parameter for HNSW vector query
 *
 * This parameter controls the exploration factor for HNSW (Hierarchical Navigable Small World)
 * based indices such as IVF_HNSW_PQ and IVF_HNSW_SQ.
 *
 * HNSW is a graph-based index that navigates through a multi-layer graph to find nearest
 * neighbors. The 'ef' parameter determines the size of the dynamic candidate list maintained
 * during the graph traversal. A larger candidate list explores more of the graph, leading to
 * better recall but slower queries.
 *
 * How it works:
 * - During search, HNSW maintains a priority queue of size 'ef' with candidate neighbors
 * - The algorithm explores neighbors of candidates in this queue
 * - Larger ef → explores more paths through the graph → better recall
 * - After exploration, the top 'limit' results are returned
 *
 * Typical values:
 * - Default is usually 100
 * - Must be >= limit (query result size)
 * - Range: limit to ~1000
 * - ef = limit → fastest, lowest recall
 * - ef = 100-200 → good balance for most use cases
 * - ef = 500+ → very high recall, slower queries
 *
 * Note: The ef parameter at query time is different from ef_construction used during
 * index building. Query-time ef controls search quality, while ef_construction controls
 * index quality.
 *
 * @param query - pointer to LanceDBVectorQuery
 * @param ef - exploration factor for HNSW search (must be >= query limit)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_vector_query_ef(
    LanceDBVectorQuery* query,
    size_t ef,
    char** error_message
);

/**
 * Execute query and return streaming result
 *
 * @param query - pointer to LanceDBQuery (consumed by this function)
 * @return Pointer to LanceDBQueryResult on success, NULL on failure
 *         Caller must free with lancedb_query_result_free()
 */
LanceDBQueryResult* lancedb_query_execute(LanceDBQuery* query);

/**
 * Execute vector query and return streaming result
 *
 * @param query - pointer to LanceDBVectorQuery (consumed by this function)
 * @return Pointer to LanceDBQueryResult on success, NULL on failure
 *         Caller must free with lancedb_query_result_free()
 */
LanceDBQueryResult* lancedb_vector_query_execute(LanceDBVectorQuery* query);

/**
 * Convert query result to Arrow RecordBatch arrays
 *
 * @param result - pointer to LanceDBQueryResult (consumed by this function)
 * @param result_arrays - pointer to receive array of Arrow C ABI arrays
 * @param result_schema - pointer to receive single Arrow C ABI schema
 * @param count_out - pointer to receive number of result batches
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *         Caller must free arrays with lancedb_free_arrow_arrays() and schema with lancedb_free_arrow_schema()
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_query_result_to_arrow(
    LanceDBQueryResult* result,
    struct FFI_ArrowArray*** result_arrays,
    struct FFI_ArrowSchema** result_schema,
    size_t* count_out,
    char** error_message
);

/**
 * Free a Query
 *
 * @param query - pointer to LanceDBQuery
 */
void lancedb_query_free(LanceDBQuery* query);

/**
 * Free a VectorQuery
 *
 * @param query - pointer to LanceDBVectorQuery
 */
void lancedb_vector_query_free(LanceDBVectorQuery* query);

/**
 * Free a QueryResult
 *
 * @param result - pointer to LanceDBQueryResult
 */
void lancedb_query_result_free(LanceDBQueryResult* result);

/**
 * Vector search using nearest_to with full result conversion
 *
 * @param table - pointer to LanceDBTable
 * @param vector - array of floats representing the query vector
 * @param dimension - dimension of the vector
 * @param limit - maximum number of results to return
 * @param column - vector column name (NULL for default "vector" column)
 * @param result_arrays - pointer to receive array of Arrow C ABI arrays
 * @param result_schema - pointer to receive single Arrow C ABI schema (shared by all arrays)
 * @param count_out - pointer to receive number of result batches
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *         Caller must free arrays with lancedb_free_arrow_arrays() and schema with lancedb_free_arrow_schema()
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_nearest_to(
    const LanceDBTable* table,
    const float* vector,
    size_t dimension,
    size_t limit,
    const char* column,
    struct FFI_ArrowArray*** result_arrays,
    struct FFI_ArrowSchema** result_schema,
    size_t* count_out,
    char** error_message
);

/**
 * Create a RecordBatchReader from Arrow C ABI structures
 *
 * @param array - pointer to FFI_ArrowArray containing record batch data
 * @param schema - pointer to FFI_ArrowSchema containing the schema
 * @param reader_out - pointer to receive the created reader
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * This function consumes the array according to Arrow C ABI specification.
 * The caller should NOT call the array's release function after passing it here.
 * The schema is only read and should still be released by the caller.
 * The caller is responsible for freeing the returned reader with
 * lancedb_record_batch_reader_free(). If error_message is provided and an error
 * occurs, the caller must free the error message with lancedb_free_string().
 */
LanceDBError lancedb_record_batch_reader_from_arrow(
    const struct FFI_ArrowArray* array,
    const struct FFI_ArrowSchema* schema,
    LanceDBRecordBatchReader** reader_out,
    char** error_message
);

/**
 * Free RecordBatchReader
 *
 * @param reader - pointer to LanceDBRecordBatchReader
 */
void lancedb_record_batch_reader_free(LanceDBRecordBatchReader* reader);

/**
 * Free Arrow schema
 *
 * @param schema - pointer to Arrow schema
 */
void lancedb_free_arrow_schema(FFI_ArrowSchema* schema);

/**
 * Create a vector index on table columns
 *
 * @param table - pointer to LanceDBTable
 * @param columns - array of null-terminated C strings containing column names
 * @param num_columns - number of columns in the array
 * @param index_type - type of vector index to create
 * @param config - pointer to LanceDBVectorIndexConfig or NULL for defaults
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_create_vector_index(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    LanceDBIndexType index_type,
    const LanceDBVectorIndexConfig* config,
    char** error_message
);

/**
 * Create a scalar index on table columns
 *
 * @param table - pointer to LanceDBTable
 * @param columns - array of null-terminated C strings containing column names
 * @param num_columns - number of columns in the array
 * @param index_type - type of scalar index to create
 * @param config - pointer to LanceDBScalarIndexConfig or NULL for defaults
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_create_scalar_index(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    LanceDBIndexType index_type,
    const LanceDBScalarIndexConfig* config,
    char** error_message
);

/**
 * Create a full-text search index on table columns
 *
 * @param table - pointer to LanceDBTable
 * @param columns - array of null-terminated C strings containing column names
 * @param num_columns - number of columns in the array
 * @param config - pointer to LanceDBFtsIndexConfig or NULL for defaults
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_create_fts_index(
    const LanceDBTable* table,
    const char* const* columns,
    size_t num_columns,
    const LanceDBFtsIndexConfig* config,
    char** error_message
);

/**
 * List all indices on the table
 *
 * @param table - pointer to LanceDBTable
 * @param indices_out - pointer to receive array of index info strings
 * @param count_out - pointer to receive the count of indices
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * The caller is responsible for freeing the returned strings and array
 * using lancedb_free_index_list(). If error_message is provided and an error
 * occurs, the caller must free the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_list_indices(
    const LanceDBTable* table,
    char*** indices_out,
    size_t* count_out,
    char** error_message
);

/**
 * Drop an index
 *
 * @param table - pointer to LanceDBTable
 * @param index_name - null-terminated C string containing the index name
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_drop_index(
    const LanceDBTable* table,
    const char* index_name,
    char** error_message
);

/**
 * Optimize table (rebuild indices and compact files)
 *
 * @param table - pointer to LanceDBTable
 * @param optimize_type - type of optimization to perform
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_optimize(
    const LanceDBTable* table,
    LanceDBOptimizeType optimize_type,
    char** error_message
);

/**
 * Free index list array returned by lancedb_table_list_indices
 *
 * @param indices - array of string pointers returned by lancedb_table_list_indices
 * @param count - number of strings in the array
 */
void lancedb_free_index_list(char** indices, size_t count);

/**
 * Index statistics structure
 */
typedef struct {
    size_t num_indexed_rows;     // Rows covered by the index
    size_t num_unindexed_rows;   // Rows not yet indexed
    unsigned int num_indices;    // Number of index parts (0 if unknown)
} LanceDBIndexStats;

/**
 * Get statistics for a named index on the table
 *
 * @param table - pointer to LanceDBTable
 * @param index_name - null-terminated C string containing the index name
 * @param stats_out - pointer to receive the index statistics
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return LANCEDB_SUCCESS if stats retrieved, LANCEDB_INDEX_NOT_FOUND if index doesn't exist
 *
 * The stats are read directly from the LanceDB manifest (no external state needed).
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_index_stats(
    const LanceDBTable* table,
    const char* index_name,
    LanceDBIndexStats* stats_out,
    char** error_message
);

/**
 * Free Arrow arrays returned by vector search functions
 *
 * @param arrays - array of Arrow C ABI array pointers
 * @param count - number of arrays
 */
void lancedb_free_arrow_arrays(
    struct FFI_ArrowArray** arrays,
    size_t count
);

/**
 * List all versions of the table
 *
 * @param table - pointer to LanceDBTable
 * @param versions_out - pointer to receive the array of LanceDBVersion structs
 * @param metadata_out - optional pointer to receive per-version metadata (NULL to skip)
 * @param count_out - pointer to receive the number of versions
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * On success, versions_out will point to an array of LanceDBVersion structs.
 * If metadata_out is non-NULL, it will point to a parallel array of
 * LanceDBVersionMetadata structs (one per version).
 * The caller must free the returned arrays using lancedb_free_versions()
 * and lancedb_free_version_metadata() respectively.
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_list_versions(
    const LanceDBTable* table,
    LanceDBVersion** versions_out,
    LanceDBVersionMetadata** metadata_out,
    size_t* count_out,
    char** error_message
);


/**
 * Get table metadata as key-value pairs
 *
 * @param table - pointer to LanceDBTable
 * @param filter_keys - optional array of key strings to filter by (NULL to get all metadata)
 * @param filter_count - number of keys in filter_keys (0 to get all metadata)
 * @param keys_out - pointer to receive array of metadata key strings
 * @param values_out - pointer to receive array of metadata value strings
 * @param count_out - pointer to receive number of metadata entries
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * To return all metadata, please set filter_keys to NULL and filter_count to zero.
 * Any other combination of NULL and zero is considered an error.
 * Otherwise, only metadata matching the specified keys is returned.
 * Keys in filter_keys that do not exist in the metadata are silently skipped.
 * On success, keys_out and values_out will point to parallel arrays of strings.
 * The caller must free the returned arrays using lancedb_free_metadata().
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_get_metadata(
    const LanceDBTable* table,
    const char* const* filter_keys,
    size_t filter_count,
    char*** keys_out,
    char*** values_out,
    size_t* count_out,
    char** error_message
);

/**
 * Set (upsert) table metadata key-value pairs
 *
 * @param table - pointer to LanceDBTable
 * @param keys - array of null-terminated key strings
 * @param values - array of null-terminated value strings
 * @param count - number of key-value pairs
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * Existing keys will be updated with new values. New keys will be added.
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_set_metadata(
    const LanceDBTable* table,
    const char* const* keys,
    const char* const* values,
    size_t count,
    char** error_message
);

/**
 * Delete table metadata keys
 *
 * @param table - pointer to LanceDBTable
 * @param keys - array of null-terminated key strings to delete
 * @param count - number of keys to delete
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Error code indicating success or failure
 *
 * Keys that do not exist are silently ignored.
 * If error_message is provided and an error occurs, the caller must free
 * the error message with lancedb_free_string().
 */
LanceDBError lancedb_table_delete_metadata(
    const LanceDBTable* table,
    const char* const* keys,
    size_t count,
    char** error_message
);

/**
 * Free versions array returned by lancedb_table_list_versions
 *
 * @param versions - array of LanceDBVersion structs returned by lancedb_table_list_versions
 * @param count - number of versions in the array
 */
void lancedb_free_versions(LanceDBVersion* versions, size_t count);

/**
 * Free version metadata array returned by lancedb_table_list_versions
 *
 * @param metadata - array of LanceDBVersionMetadata structs returned by lancedb_table_list_versions
 * @param count - number of entries in the array
 *
 * Safe to call with NULL pointer.
 */
void lancedb_free_version_metadata(LanceDBVersionMetadata* metadata, size_t count);

/**
 * Free metadata arrays returned by lancedb_table_get_metadata
 *
 * @param keys - array of key strings returned by lancedb_table_get_metadata
 * @param values - array of value strings returned by lancedb_table_get_metadata
 * @param count - number of entries in the arrays
 *
 * Safe to call with NULL pointers.
 */
void lancedb_free_metadata(char** keys, char** values, size_t count);

/**
 * Free string returned by LanceDB functions
 *
 * @param str - string pointer returned by LanceDB functions
 */
void lancedb_free_string(char* str);

/* ==================== DataFusion Expression Builder API ==================== */

/**
 * Create a column reference expression
 *
 * @param name - null-terminated C string containing the column name
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         Caller must free with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_column(const char* name);

/**
 * Create a string literal expression
 *
 * @param value - null-terminated C string containing the literal value
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         Caller must free with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_literal_string(const char* value);

/**
 * Create an integer literal expression (i64)
 *
 * @param value - the integer value
 * @return Non-null pointer to LanceDBExpr
 *         Caller must free with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_literal_i64(int64_t value);

/**
 * Create a float literal expression (f64)
 *
 * @param value - the float value
 * @return Non-null pointer to LanceDBExpr
 *         Caller must free with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_literal_f64(double value);

/**
 * Create a boolean literal expression
 *
 * @param value - the boolean value
 * @return Non-null pointer to LanceDBExpr
 *         Caller must free with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_literal_bool(bool value);

/**
 * Create a binary expression (left op right)
 *
 * @param left - pointer to LanceDBExpr for left operand (consumed)
 * @param op - binary operator
 * @param right - pointer to LanceDBExpr for right operand (consumed)
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         Both left and right are consumed; do not use or free them after calling
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_binary(
    LanceDBExpr* left,
    LanceDBBinaryOp op,
    LanceDBExpr* right
);

/**
 * Create a NOT expression
 *
 * @param expr - pointer to LanceDBExpr (consumed)
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         The input expr is consumed; do not use or free it after calling
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_not(LanceDBExpr* expr);

/**
 * Create an IS NULL expression
 *
 * @param expr - pointer to LanceDBExpr (consumed)
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         The input expr is consumed; do not use or free it after calling
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_is_null(LanceDBExpr* expr);

/**
 * Create an IS NOT NULL expression
 *
 * @param expr - pointer to LanceDBExpr (consumed)
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         The input expr is consumed; do not use or free it after calling
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_is_not_null(LanceDBExpr* expr);

/**
 * Create an AND expression (convenience for binary AND)
 *
 * @param left - pointer to LanceDBExpr (consumed)
 * @param right - pointer to LanceDBExpr (consumed)
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         Both inputs are consumed; do not use or free them after calling
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_and(LanceDBExpr* left, LanceDBExpr* right);

/**
 * Create an OR expression (convenience for binary OR)
 *
 * @param left - pointer to LanceDBExpr (consumed)
 * @param right - pointer to LanceDBExpr (consumed)
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         Both inputs are consumed; do not use or free them after calling
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_or(LanceDBExpr* left, LanceDBExpr* right);

/**
 * Create an IN list expression (expr IN (value1, value2, ...))
 *
 * @param expr - pointer to LanceDBExpr for the expression to check (consumed)
 * @param list - array of pointers to LanceDBExpr for list values (all consumed)
 * @param list_len - number of elements in the list
 * @param negated - if true, creates NOT IN instead of IN
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         The input expr and all list elements are consumed; do not use or free them after calling
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_in_list(
    LanceDBExpr* expr,
    LanceDBExpr* const* list,
    size_t list_len,
    bool negated,
    char** error_message
);

/**
 * Create an array_has expression: check if an array column contains a value
 *
 * Returns a boolean expression that is true when the array contains the element.
 *
 * @param array_expr - pointer to LanceDBExpr for the array column (consumed)
 * @param value_expr - pointer to LanceDBExpr for the value to search for (consumed)
 * @param error_message - optional pointer to receive detailed error message (NULL to ignore)
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         Both inputs are consumed; do not use or free them after calling
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_array_has(
    LanceDBExpr* array_expr,
    LanceDBExpr* value_expr,
    char** error_message
);

/**
 * Clone an expression (creates an independent copy)
 *
 * @param expr - pointer to LanceDBExpr to clone (not consumed)
 * @return Non-null pointer to a new LanceDBExpr on success, NULL on failure
 *         The original expr remains valid and must still be freed separately
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_clone(const LanceDBExpr* expr);

/**
 * Free an expression
 *
 * @param expr - pointer to LanceDBExpr
 *
 * After calling this function, the expr pointer must not be used.
 */
void lancedb_expr_free(LanceDBExpr* expr);

/* ==================== JSON Expression API ==================== */

/**
 * Create a json_get_str expression: extract a string value from JSON by path
 *
 * For nested access, pass multiple path segments (e.g., ["user", "name"] for {"user":{"name":"alice"}}).
 *
 * @param json_expr - pointer to LanceDBExpr for the JSON column (consumed)
 * @param path - array of null-terminated C strings forming the JSON path
 * @param path_len - number of path segments
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         The json_expr is consumed; do not use or free it after calling.
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_json_get_str(LanceDBExpr* json_expr, const char* const* path, size_t path_len);

/**
 * Create a json_get_int expression: extract an integer value from JSON by path
 *
 * For nested access, pass multiple path segments (e.g., ["stats", "count"] for {"stats":{"count":42}}).
 *
 * @param json_expr - pointer to LanceDBExpr for the JSON column (consumed)
 * @param path - array of null-terminated C strings forming the JSON path
 * @param path_len - number of path segments
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         The json_expr is consumed; do not use or free it after calling.
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_json_get_int(LanceDBExpr* json_expr, const char* const* path, size_t path_len);

/**
 * Create a json_get_float expression: extract a float value from JSON by path
 *
 * For nested access, pass multiple path segments (e.g., ["stats", "score"] for {"stats":{"score":4.2}}).
 *
 * @param json_expr - pointer to LanceDBExpr for the JSON column (consumed)
 * @param path - array of null-terminated C strings forming the JSON path
 * @param path_len - number of path segments
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         The json_expr is consumed; do not use or free it after calling.
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_json_get_float(LanceDBExpr* json_expr, const char* const* path, size_t path_len);

/**
 * Create a json_get_bool expression: extract a boolean value from JSON by path
 *
 * For nested access, pass multiple path segments (e.g., ["user", "active"] for {"user":{"active":true}}).
 *
 * @param json_expr - pointer to LanceDBExpr for the JSON column (consumed)
 * @param path - array of null-terminated C strings forming the JSON path
 * @param path_len - number of path segments
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         The json_expr is consumed; do not use or free it after calling.
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_json_get_bool(LanceDBExpr* json_expr, const char* const* path, size_t path_len);

/**
 * Create a json_contains expression: check if a key exists in JSON
 *
 * For nested access, pass multiple path segments (e.g., ["user", "name"] to check
 * if {"user":{"name":"alice"}} has the nested key).
 *
 * @param json_expr - pointer to LanceDBExpr for the JSON column (consumed)
 * @param path - array of null-terminated C strings forming the JSON path
 * @param path_len - number of path segments
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         The json_expr is consumed; do not use or free it after calling.
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_json_contains(LanceDBExpr* json_expr, const char* const* path, size_t path_len);

/**
 * Create a json_array_has expression: check if a JSON array contains a value
 *
 * Navigates to a JSON array field using the path, then checks whether the given
 * value expression appears as an element. The quote_value flag controls how the
 * value is adapted:
 * - true: for JSON string arrays: value is wrapped in JSON quotes for matching.
 * - false: for JSON number/boolean arrays: value is cast to Utf8.
 *
 * For arrays of non-scalar values (objects, nested arrays), it falls
 * back to exact string matching against the raw JSON text, which is not JSON-aware
 *
 * @param json_expr - pointer to LanceDBExpr for the JSON column (consumed)
 * @param path - array of null-terminated C strings forming the JSON path to the array
 * @param path_len - number of path segments
 * @param value_expr - pointer to LanceDBExpr for the value to search for (consumed)
 * @param quote_value - true for string arrays, false for number/boolean arrays
 * @return Non-null pointer to LanceDBExpr on success, NULL on failure
 *         Both json_expr and value_expr are consumed; do not use or free them after calling.
 *         Caller must free result with lancedb_expr_free()
 */
LanceDBExpr* lancedb_expr_json_array_has(LanceDBExpr* json_expr, const char* const* path, size_t path_len, LanceDBExpr* value_expr, bool quote_value);

/**
 * Evaluate a JSON filter expression against Arrow RecordBatches.
 *
 * Takes the Arrow FFI arrays and schema from lancedb_query_result_to_arrow
 * and a filter expression. Returns a boolean array indicating which rows
 * matched. The arrays are NOT consumed and remain valid after the call.
 *
 * Must be called while the arrays are valid (before consuming them with
 * arrow::ImportRecordBatch or freeing with lancedb_free_arrow_arrays).
 *
 * Example: to filter rows where metadata.name = "alice":
 *   col  = lancedb_expr_column("metadata")
 *   path = (const char*[]){"name"}
 *   name = lancedb_expr_json_get_str(col, path, 1)
 *   val  = lancedb_expr_literal_string("alice")
 *   expr = lancedb_expr_binary(name, LANCEDB_BINARY_OP_EQ, val)
 *   lancedb_json_matches(arrays, schema, count, expr,
 *                        &results, &nrows, &error_message)
 *
 * @param arrays - array of FFI_ArrowArray pointers (NOT consumed)
 * @param schema - pointer to FFI_ArrowSchema (NOT consumed)
 * @param batch_count - number of batches in the arrays
 * @param expr - pointer to LanceDBExpr filter expression (consumed)
 * @param results_out - pointer to receive allocated bool array (one per row)
 * @param count_out - pointer to receive total row count across all batches
 * @param error_message - optional pointer to receive detailed error message
 * @return Error code indicating success or failure.
 *         The expr is consumed; do not use or free it after calling.
 *         Caller must free *results_out with lancedb_free_json_matches().
 *         If error_message is set, caller must free with lancedb_free_string().
 */
LanceDBError lancedb_json_matches(
    FFI_ArrowArray** arrays,
    FFI_ArrowSchema* schema,
    size_t batch_count,
    LanceDBExpr* expr,
    bool** results_out,
    size_t* count_out,
    char** error_message
);

/**
 * Free results array returned by lancedb_json_matches
 *
 * @param results - pointer returned by lancedb_json_matches, or NULL
 */
void lancedb_free_json_matches(bool* results);

#ifdef __cplusplus
}
#endif

#endif /* LANCEDB_H */
