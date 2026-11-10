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

Reindexing
----------

An index only covers the rows that existed when it was built. New rows are still
searched, but by a scan of the unindexed fragments, so the index needs to be brought up
to date as the table grows. ``lancedb_table_optimize_indices()`` does that, with a
``LanceDBOptimizeIndexOptions`` struct choosing how:

- ``LANCEDB_REINDEX_APPEND`` builds a small delta index over the new rows and leaves the
  existing segments alone. It is the cheap option to run after every batch of writes.
- ``LANCEDB_REINDEX_MERGE`` folds the new rows and the newest ``num_indices_to_merge``
  segments into one segment. A count above the number of segments merges them all, and
  ``-1`` behaves like ``LANCEDB_REINDEX_DEFAULT``. Every segment costs search latency, so
  merge once there are too many of them.
- ``LANCEDB_REINDEX_RETRAIN`` retrains a vector index on the current data and collapses
  it to a single segment. Use it when the data distribution has drifted away from the
  trained centroids. A scalar or full-text index has nothing to train, so for these
  indices it behaves like ``LANCEDB_REINDEX_DEFAULT``. To change the parameters of an
  index, or its type, use ``lancedb_table_create_*_index()`` with ``replace = 1`` instead.
- ``LANCEDB_REINDEX_DEFAULT`` leaves the choice to Lance, and is what
  ``lancedb_table_optimize(LANCEDB_OPTIMIZE_INDEX)`` does. The outcome depends on the
  index type:

  - In case of a vector index, a new delta segment is added, as with ``APPEND``,
    unless one of its partitions has grown too large or is too small.
    In that case the partition is split, or joined into its neighbours, and all the
    segments are merged into one.
  - A scalar or full-text index has the new rows merged into its newest segment, so its
    number of segments does not grow.

``index_names`` limits the work to some of the indices, and the transaction properties
are stored with the commit, for example to record the job that ran the reindex.

``lancedb_table_index_stats()`` provides the inputs for that decision:

- ``num_unindexed_rows`` and ``num_unindexed_fragments`` above zero: run ``APPEND``.
- ``num_indices`` above a threshold: run ``MERGE``.
- ``num_deleted_indexed_rows`` large relative to ``num_indexed_rows``, or many table
  versions since ``dataset_version``: run ``RETRAIN``.

.. code-block:: c

   LanceDBIndexStats stats;
   lancedb_table_index_stats(table, "data_idx", &stats, NULL);

   LanceDBOptimizeIndexOptions options = lancedb_optimize_index_options_default();
   if (stats.num_indices > 8) {
       options.mode = LANCEDB_REINDEX_MERGE;
       options.num_indices_to_merge = (int)stats.num_indices; // merge all the segments
   } else if (stats.num_unindexed_rows > 0) {
       options.mode = LANCEDB_REINDEX_APPEND;
   }

   char* error_message = NULL;
   if (lancedb_table_optimize_indices(table, &options, &error_message) != LANCEDB_SUCCESS) {
       fprintf(stderr, "Error: %s\n", error_message);
       lancedb_free_string(error_message);
   }

Every call is blocking and commits its own table version; the options passed to one call
do not affect the next.
