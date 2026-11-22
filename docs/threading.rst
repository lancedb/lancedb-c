Threading and unwinding
=======================

The current preview does not define a general thread-safety contract for API
handles. Treat a handle as owned by one caller at a time unless the function
comment in ``include/lancedb.h`` says that a particular operation is safe to use
concurrently with another one. Distinct objects may be used from different
threads when the caller provides its own synchronization and does not share
mutable input buffers.

Blocking calls
--------------

Calls that perform I/O or query execution block the calling thread until the
operation returns or the SDK reports an error. ``lancedb_table_wait_for_index()``
blocks and polls for up to the requested budget; the budget is not a wall-clock
bound on the requests already in progress. See its comment for the exact
polling and timeout behavior.

``lancedb_run_on_stack()`` runs a callback on an extended stack when the
remaining stack is below the requested red zone. The call invokes the callback
synchronously.

Arrow C Stream callbacks
------------------------

An Arrow C Stream imported with
``lancedb_record_batch_reader_from_arrow_stream()`` keeps the producer alive
until its release callback runs. The schema and each returned array are
independently owned. Arrays may be released after the stream or on a different
thread, and callbacks may run on SDK worker threads over time. A producer must
therefore keep its private state alive until release and must not assume that
every callback runs on the thread that called the import function.

Unwinding across the boundary
-----------------------------

Callbacks passed to ``lancedb_run_on_stack()``, and any other callback the
caller supplies, must not unwind across the C ABI boundary. Do not let a C++
exception or ``longjmp`` escape a callback; catch it in the callback body.

The API does not promise that a panic or foreign unwind crossing the boundary
will be caught and converted into a ``LanceDBError``. Do not rely on such a
conversion as an error-handling strategy.

A broader threading and panic-recovery contract is outside this preview. The
function comments and this page record the guarantees that are currently made.
