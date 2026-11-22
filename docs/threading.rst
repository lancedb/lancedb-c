Threading and unwinding
=======================

The public C API does not define a general rule for sharing handles between
threads. This page records the cross-thread behavior that is currently
documented; do not share a handle or mutable input across threads unless the
function comment in ``include/lancedb.h`` explicitly permits it.

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

The schema is acquired synchronously on the calling thread before
``lancedb_record_batch_reader_from_arrow_stream()`` returns. Calls on one stream
are serialized. After the import, callbacks may run on SDK worker threads, and
release may run on a worker or on the thread that frees the reader. The schema
and each returned array are independently owned. Arrays may be released after
the stream or on a different thread. A producer must keep its private state
alive until release and must not assume that every callback runs on the thread
that called the import function.

Unwinding across the boundary
-----------------------------

The exported C entry points are non-unwinding ``extern "C"`` functions. If a
Rust panic reaches the FFI boundary, the process aborts; the panic is not
converted into a ``LanceDBError``. The same rule applies to callbacks passed to
``lancedb_run_on_stack()`` and to any other caller-provided callback: do not let
a C++ exception or ``longjmp`` escape, and catch exceptions in the callback
body.

The completed FFI audit in #99 covers these entry points. The runtime behavior
of work that leaves the caller's stack, and any broader recovery policy, belongs
to the active runtime redesign (#76), not this preview. Do not rely on panic
recovery as an error-handling strategy.
