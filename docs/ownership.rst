Memory ownership
================

Ownership is per object. The comment above each function in
``include/lancedb.h`` is the authoritative contract; this page summarizes the
rules that apply across the API.

Opaque handles
--------------

Most API objects are opaque handles created by a constructor and released by a
matching function:

.. list-table::
   :header-rows: 1

   * - Handle
     - Created by
     - Released by
   * - ``LanceDBConnectBuilder``
     - ``lancedb_connect()``
     - ``lancedb_connect_builder_execute()`` or ``lancedb_connect_builder_free()``
   * - ``LanceDBConnectNamespaceBuilder``
     - ``lancedb_connect_namespace()``
     - ``lancedb_connect_namespace_builder_execute()`` or ``lancedb_connect_namespace_builder_free()``
   * - ``LanceDBConnection``
     - ``lancedb_connect_builder_execute()``
     - ``lancedb_connection_free()``
   * - ``LanceDBTable``
     - an open or create function
     - ``lancedb_table_free()``
   * - ``LanceDBSession``
     - ``lancedb_session_new()``
     - ``lancedb_session_free()``
   * - ``LanceDBObjectStoreRegistry``
     - ``lancedb_registry_new()``
     - ``lancedb_registry_free()``, or transferred to a session
   * - ``LanceDBQuery`` / ``LanceDBVectorQuery``
     - ``lancedb_query_new()`` / ``lancedb_vector_query_new()``
     - ``lancedb_query_free()`` / ``lancedb_vector_query_free()``, or consumed by execute
   * - ``LanceDBTakeQuery``
     - ``lancedb_table_take_offsets()`` / ``lancedb_table_take_row_ids()``
     - ``lancedb_take_query_free()``, or consumed by execute
   * - ``LanceDBQueryResult``
     - a query execute function
     - ``lancedb_query_result_free()``, or consumed by a conversion
   * - ``LanceDBTableDelta``
     - ``lancedb_table_delta()``
     - ``lancedb_table_delta_free()``
   * - ``LanceDBBlobFile``
     - ``lancedb_table_fetch_blob_files()`` / ``lancedb_table_open_blob_file()``
     - ``lancedb_blob_file_free()``
   * - ``LanceDBExpr``
     - an expression constructor
     - ``lancedb_expr_free()``, or consumed by an entry point that documents it

A handle is not reference counted. Do not free a handle while another call is
still using it, and do not keep using a handle after its free function returns.
The builder setters consume the builder and return its replacement, so assign
the return value back to the same variable. Executing a builder consumes it,
including on failure. The same rule applies to query builders and to the
record-batch readers documented below.

Strings and arrays
------------------

A function that writes an error through a ``char**`` parameter gives the caller
ownership of that string. Free it with ``lancedb_free_string()``. A ``const
char*`` returned directly, such as ``lancedb_connection_uri()`` or
``lancedb_version()``, is borrowed and must not be freed.

Functions that return an array of strings or structs also return a count. Free
the result with its matching function, such as ``lancedb_free_table_names()``,
``lancedb_free_versions()``, ``lancedb_free_tags()``, or
``lancedb_free_index_info()``. The library owns the array and its elements;
callers must not modify entries, free them individually, or pass a different
count from the one returned with the array.

Inputs that are described as borrowed, including names, SQL filters, option
strings and row-id arrays, only need to stay valid for the duration of the call.
Inputs copied by the call may be changed or freed after it returns.

Query results and Arrow data
----------------------------

A query execute function consumes its query builder and returns a result. Free
the result with ``lancedb_query_result_free()`` when it is no longer needed.

``lancedb_query_result_to_arrow()`` consumes the result only when the result and
all three output pointers are non-NULL. On entry it clears non-NULL output slots.
On success the caller frees the arrays with ``lancedb_free_arrow_arrays()`` and
the schema with ``lancedb_free_arrow_schema()``. An empty result succeeds with
NULL arrays, a NULL schema and a zero count. On failure all outputs stay clear.
If any required pointer is NULL, the call returns ``LANCEDB_INVALID_ARGUMENT``
and does not consume a non-NULL result, so the caller can retry or free it.

``lancedb_query_result_to_ipc_file()`` consumes the result and leaves the file
bytes in a ``LanceDBIpcBuffer``. Free the bytes with
``lancedb_ipc_buffer_free()``.

``lancedb_record_batch_reader_from_arrow()`` imports the schema first. If the
schema import fails, the Arrow array is still caller-owned and must be released.
Once the schema imports, the array is handed to Arrow's importer and must not be
released again, even when a later batch conversion fails. The schema itself is
only read and remains owned by the caller.

``lancedb_record_batch_reader_from_ipc_file()`` borrows the input bytes and
decodes every batch before returning. ``lancedb_record_batch_reader_from_arrow_stream()``
validates the required callbacks before moving the producer; if a callback is
missing, the stream stays with the caller. After a successful move, the binding
owns the producer and releases it even when the schema callback fails. The
caller keeps the outer struct storage and must not release the moved stream
again.

A reader is not transferred just by being passed. The entry point decides when
it takes ownership:

- ``lancedb_table_create()`` takes a non-NULL reader only after its required
  pointers are non-NULL, the table name is valid UTF-8, and the Arrow schema
  imports. Failures before that point leave the reader caller-owned; after the
  schema imports, the reader is consumed even if table-name validation or table
  creation fails. Passing NULL creates an empty table.
- ``lancedb_table_add()`` consumes a non-NULL reader after checking the table
  and reader pointers for NULL. A NULL table or reader returns without consuming
  the reader.
- ``lancedb_table_add_with_options()`` consumes a non-NULL reader before
  validating the table or options. A NULL reader returns without consuming
  anything; every non-NULL reader is consumed on every later path, including a
  NULL table and rejected options.
- Every ``lancedb_table_merge_insert*`` entry point consumes a non-NULL reader
  before validating the table, columns or options. A NULL data pointer returns
  without consuming it; any non-NULL data is consumed on every later path.

After one of these calls consumes a reader, do not call
``lancedb_record_batch_reader_free()`` on it. Set the caller's pointer to NULL
if the caller needs a single cleanup path.

Blob results
------------

A ``LanceDBBlobBuffer`` returned by a blob read owns its bytes until
``lancedb_blob_buffer_free()`` is called. The bytes stay valid after the blob file
handle and the table are freed. Freeing an empty or already freed buffer is a
no-op and zeroes both fields.

A ``LanceDBBlobFile`` is a seekable handle. Its size stays available after
``lancedb_blob_file_close()``; reads, range reads and seeks on a closed handle
fail with ``LANCEDB_INVALID_INPUT``. Closing is idempotent.

``lancedb_table_fetch_blob_files()`` returns an array of optional handles. Free
the whole result with ``lancedb_free_blob_files(files, count)``, which releases
every non-NULL handle and the array. If a handle was already freed with
``lancedb_blob_file_free()``, set that array entry to NULL before the bulk free.

Failure and output slots
------------------------

Unless a function documents otherwise, an error leaves its output slots clear
or unchanged as stated in that function's comment, and the optional error
message is set only when the caller supplied the pointer. Check the return code
before using an output. Free any non-NULL error message the call returns. If you
reuse an error slot across calls, rely on the clearing behavior documented by
that function rather than assuming it is always cleared.
