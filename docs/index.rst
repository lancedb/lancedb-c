LanceDB C API Documentation
===========================

Welcome to the LanceDB C API documentation. This library provides C bindings for LanceDB,
a developer-friendly, serverless vector database for AI applications.

.. toctree::
   :maxdepth: 2
   :caption: Contents:

   api/index

Overview
--------

The LanceDB C API provides a complete interface to LanceDB functionality including:

- Database connections and configuration
- Table creation and management
- Data insertion and querying
- Vector similarity search
- Index creation and management

Quick Start
-----------

This example shows how to create a LanceDB table with vector data using Apache Arrow.

**1. Connect to a database:**

.. code-block:: c

   #include <lancedb.h>
   #include <arrow/c/bridge.h>
   #include <arrow/api.h>

   // Create a connection to a local database
   LanceDBConnectBuilder* builder = lancedb_connect("./my_database");
   LanceDBConnection* db = lancedb_connect_builder_execute(builder);

.. note::

   You can optionally configure session cache sizes and attach the session to
   the connection builder:

   .. code-block:: c

      LanceDBSessionOptions options = {0};
      options.index_cache_bytes = 512 * 1024 * 1024;
      options.metadata_cache_bytes = 256 * 1024 * 1024;

      LanceDBSession* session = lancedb_session_new(&options);

      LanceDBConnectBuilder* builder = lancedb_connect("./my_database");
      builder = lancedb_connect_builder_session(builder, session);
      LanceDBConnection* db = lancedb_connect_builder_execute(builder);

**2. Create Arrow schema (using Arrow C++ API):**

.. code-block:: cpp

   // Define schema with id and vector columns
   auto id_field = arrow::field("id", arrow::int32());
   auto vector_field = arrow::field("vector",
                                    arrow::fixed_size_list(arrow::float32(), 128));
   auto schema = arrow::schema({id_field, vector_field});

   // Export to C ABI
   struct ArrowSchema c_schema;
   arrow::ExportSchema(*schema, &c_schema);

**3. Create data (using Arrow builders):**

.. code-block:: cpp

   arrow::Int32Builder id_builder;
   arrow::FixedSizeListBuilder vector_builder(
       arrow::default_memory_pool(),
       std::make_unique<arrow::FloatBuilder>(),
       128  // dimension
   );

   // Add 100 rows
   for (int i = 0; i < 100; i++) {
       id_builder.Append(i);

       auto* float_builder = static_cast<arrow::FloatBuilder*>(
           vector_builder.value_builder());
       for (int j = 0; j < 128; j++) {
           float_builder->Append(i * 0.1f + j);
       }
       vector_builder.Append();
   }

   // Build arrays
   std::shared_ptr<arrow::Array> id_array, vector_array;
   id_builder.Finish(&id_array);
   vector_builder.Finish(&vector_array);

   // Create RecordBatch
   auto batch = arrow::RecordBatch::Make(schema, 100, {id_array, vector_array});

   // Export to C ABI
   struct ArrowArray c_array;
   arrow::ExportRecordBatch(*batch, &c_array, &c_schema);

**4. Create table with schema and data:**

.. code-block:: c

   LanceDBError result;
   char* error_message = NULL;

   // Create reader from Arrow C ABI
   LanceDBRecordBatchReader* reader;

   result = lancedb_record_batch_reader_from_arrow(
       (FFI_ArrowArray*)&c_array,
       (FFI_ArrowSchema*)&c_schema,
       &reader,
       &error_message
   );

   if (result != LANCEDB_SUCCESS) {
       fprintf(stderr, "Error: %s\n", error_message);
       lancedb_free_string(error_message);
       if (c_array.release) {
         c_array.release(&c_array);
       }
   }

   // Create table
   LanceDBTable* table;

   result = lancedb_table_create(
       db,
       "my_vectors",
       (FFI_ArrowSchema*)&c_schema,
       reader,
       &table,
       &error_message
   );

   if (result == LANCEDB_SUCCESS) {
       printf("Table created with %lu rows\n", lancedb_table_count_rows(table));
       lancedb_table_free(table);
   } else {
       fprintf(stderr, "Error: %s\n", error_message);
       lancedb_free_string(error_message);
   }

   // Clean up
   lancedb_connection_free(db);

Waiting for an index
--------------------

An index becomes usable when its build commits, and a remote build may continue after
the request returns. ``lancedb_table_wait_for_index()`` blocks until every index named by
the caller reports no unindexed rows, so a process does not have to guess how long the
build takes:

.. code-block:: c

   const char* index_names[] = {"data_idx"};
   char* error_message = NULL;
   const LanceDBError result = lancedb_table_wait_for_index(
       table, index_names, 1, 30 * 1000, &error_message);
   if (result == LANCEDB_TIMEOUT) {
       fprintf(stderr, "the index is not fully built yet\n");
   } else if (result != LANCEDB_SUCCESS) {
       fprintf(stderr, "Error: %s\n", error_message);
       lancedb_free_string(error_message);
   }

The budget is a limit on how long the call waits, in milliseconds, and must be greater
than zero. The SDK checks the statistics once per second and caps the budget at two
hours, rejecting a longer one with the maximum it accepts. Only the named indices are
waited for, and ``lancedb_table_list_indices()`` returns the names of all of them.
Waiting does not build or extend an index: an index built before new rows were added
never becomes fully indexed by waiting, so reindex it first with the index creation or
optimization functions. The call has no cancellation, and its budget is checked between
polls, so it can return up to one poll interval after the budget runs out.
