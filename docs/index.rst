LanceDB C API Documentation
===========================

Welcome to the LanceDB C API documentation. This library provides C bindings for LanceDB,
a developer-friendly, serverless vector database for AI applications.

.. toctree::
   :maxdepth: 2
   :caption: Contents:

   api/index
   versioning
   ownership
   threading

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

The smallest C17 program includes only ``lancedb.h``. The repository's
``examples/hello.c`` connects to a local database and prints the runtime
version:

.. literalinclude:: ../examples/hello.c
   :language: c
   :linenos:

Build it against an installed package:

.. code-block:: bash

   lancedb_libdir="$(pkg-config --variable=libdir lancedb)"
   cc -std=c17 -Wall -Wextra -Werror hello.c \
     $(pkg-config --cflags --libs lancedb) \
     -Wl,-rpath,"$lancedb_libdir" -o hello

See the repository README for build and installation commands.

Creating a table with Arrow C++
-------------------------------

The program below uses Arrow C++ to build one record batch, exports it through
the Arrow C Data interface, and adds it to a new table. It is a complete C++20
translation unit; compile it with the same compiler and Arrow installation used
to build the examples. Link it against both the installed ``lancedb::lancedb``
target and Arrow C++ (for example, ``pkg-config --libs arrow``). The lancedb
package does not carry the Arrow C++ link dependencies. Use the same runtime
loader setup as the Quick Start.

.. code-block:: cpp

   #include <lancedb.h>

   #include <arrow/api.h>
   #include <arrow/c/bridge.h>

   #include <cstdio>
   #include <memory>

   namespace {

   void print_error(const char* step, LanceDBError code,
                    const char* error_message) {
     std::fprintf(stderr, "%s: %s\n", step,
                  error_message != nullptr ? error_message
                                           : lancedb_error_to_message(code));
   }

   }  // namespace

   int main() {
     LanceDBConnectBuilder* builder = lancedb_connect("./my_database");
     if (builder == nullptr) {
       std::fprintf(stderr, "failed to create a connection builder\n");
       return 1;
     }

     LanceDBConnection* db = nullptr;
     char* error_message = nullptr;
     LanceDBError result =
         lancedb_connect_builder_execute(builder, &db, &error_message);
     if (result != LANCEDB_SUCCESS) {
       print_error("connect", result, error_message);
       lancedb_free_string(error_message);
       return 1;
     }

     auto id_field = arrow::field("id", arrow::int32());
     auto vector_field = arrow::field(
         "vector", arrow::fixed_size_list(arrow::float32(), 128));
     auto schema = arrow::schema({id_field, vector_field});

     arrow::Int32Builder id_builder;
     arrow::FixedSizeListBuilder vector_builder(
         arrow::default_memory_pool(),
         std::make_unique<arrow::FloatBuilder>(), 128);
     for (int i = 0; i < 100; ++i) {
       id_builder.Append(i).ok();
       auto* values =
           static_cast<arrow::FloatBuilder*>(vector_builder.value_builder());
       for (int j = 0; j < 128; ++j) {
         values->Append(static_cast<float>(i) * 0.1f +
                        static_cast<float>(j)).ok();
       }
       vector_builder.Append().ok();
     }

     std::shared_ptr<arrow::Array> id_array;
     std::shared_ptr<arrow::Array> vector_array;
     id_builder.Finish(&id_array).ok();
     vector_builder.Finish(&vector_array).ok();
     auto batch = arrow::RecordBatch::Make(
         schema, 100, {id_array, vector_array});

     struct ArrowArray c_array{};
     struct ArrowSchema c_schema{};
     const auto export_status =
         arrow::ExportRecordBatch(*batch, &c_array, &c_schema);
     if (!export_status.ok()) {
       std::fprintf(stderr, "failed to export Arrow record batch: %s\n",
                    export_status.ToString().c_str());
       if (c_array.release != nullptr) {
         c_array.release(&c_array);
       }
       if (c_schema.release != nullptr) {
         c_schema.release(&c_schema);
       }
       lancedb_connection_free(db);
       return 1;
     }

     LanceDBRecordBatchReader* reader = nullptr;
     result = lancedb_record_batch_reader_from_arrow(
         reinterpret_cast<FFI_ArrowArray*>(&c_array),
         reinterpret_cast<FFI_ArrowSchema*>(&c_schema), &reader,
         &error_message);
     if (result != LANCEDB_SUCCESS) {
       // This schema comes from Arrow C++, so the C API accepts it and owns
       // c_array once this call starts converting the batch. Do not release
       // c_array again, and do not continue with the null reader.
       print_error("import record batch", result, error_message);
       lancedb_free_string(error_message);
       if (c_schema.release != nullptr) {
         c_schema.release(&c_schema);
       }
       lancedb_connection_free(db);
       return 1;
     }

     LanceDBTable* table = nullptr;
     result = lancedb_table_create(
         db, "my_vectors", reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
         nullptr, &table, &error_message);
     if (c_schema.release != nullptr) {
       c_schema.release(&c_schema);
     }
     if (result != LANCEDB_SUCCESS) {
       print_error("create table", result, error_message);
       lancedb_free_string(error_message);
       lancedb_record_batch_reader_free(reader);
       lancedb_connection_free(db);
       return 1;
     }

     result = lancedb_table_add(table, reader, &error_message);
     reader = nullptr;  // add consumes a non-NULL reader on every path
     if (result != LANCEDB_SUCCESS) {
       print_error("add rows", result, error_message);
       lancedb_free_string(error_message);
       lancedb_table_free(table);
       lancedb_connection_free(db);
       return 1;
     }

     std::printf("table has %llu rows\n", lancedb_table_count_rows(table));
     lancedb_table_free(table);
     lancedb_connection_free(db);
     return 0;
   }

The reader import splits ownership at schema import. If the schema import fails,
``c_array`` is still caller-owned and must be released. Once the schema imports,
the C API owns the array and a later batch-conversion failure still consumes it.
The schema itself is only read and remains caller-owned. The example creates an
empty table first and then calls ``lancedb_table_add()``, whose non-NULL reader
is consumed on every path after its null check. Creating the table with a reader
instead is also supported; its ownership split is documented in
:doc:`ownership`.

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
   }
   lancedb_free_string(error_message);

Free the optional message after either branch; ``lancedb_free_string()`` accepts
NULL. The budget is a polling budget in milliseconds and must be greater than zero: the SDK
starts a polling round while less than that much time has elapsed, sleeping one second
between rounds, and caps the budget at two hours, rejecting a longer one with the maximum
it accepts. It is not a wall-clock bound: the listing and statistics requests of a round,
and the diagnostics of the last round, finish after the budget has run out, and the call
does not interrupt them. Only the named indices are waited for, and
``lancedb_table_list_indices()`` returns the names of all of them. Waiting does not build
or extend an index: an index built before new rows were added never becomes fully
indexed by waiting, so reindex it first with the index creation or optimization functions.
There is no cancellation.
