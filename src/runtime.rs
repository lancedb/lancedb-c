//! Tokio runtime ownership and the blocking bridge used by the C entry points.
//!
//! Every database-executing C function used to call `Runtime::block_on` on the
//! caller's thread, which polls the future — and therefore runs lance /
//! datafusion query planning — on whatever stack the caller happens to have.
//! Hosts that call the C API from small fixed-size stacks (boost coroutines
//! in Ceph RGW use 512 KiB) overflow deterministically, and stack-growth guards
//! such as `stacker` cannot see a coroutine's separately mapped stack.
//!
//! `run_blocking` instead spawns the future onto the runtime's worker threads
//! (which have their own, generously sized stacks) and only parks the calling
//! thread until the task completes.  This is the same approach Lance's Python
//! bindings take with `BackgroundExecutor::spawn`.

use std::future::Future;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::OnceLock;

use tokio::runtime::{Builder, Runtime};

/// Default stack size for runtime worker (and blocking-pool) threads.
///
/// Upstream measured ~552 KB of stack for a merge_insert in
/// `examples/asio_coroutine.cpp`; tokio's default of 2 MiB leaves little margin
/// for debug builds, so we allocate 8 MiB.  Stacks are mmap'd and committed
/// lazily, so the cost is virtual address space, not RSS.
const DEFAULT_WORKER_STACK_BYTES: usize = 8 * 1024 * 1024;

/// Environment override for the worker stack size (bytes).
const ENV_WORKER_STACK_SIZE: &str = "LANCEDB_C_WORKER_STACK_SIZE";
/// Environment override for the number of runtime worker threads.
const ENV_WORKER_THREADS: &str = "LANCEDB_C_WORKER_THREADS";

static RUNTIME: OnceLock<Runtime> = OnceLock::new();

fn env_usize(name: &str) -> Option<usize> {
    std::env::var(name)
        .ok()?
        .trim()
        .parse()
        .ok()
        .filter(|v| *v > 0)
}

/// The process-wide tokio runtime used by all C entry points.
pub(crate) fn get_runtime() -> &'static Runtime {
    RUNTIME.get_or_init(|| {
        let mut builder = Builder::new_multi_thread();
        builder
            .enable_all()
            // 14 chars: Linux truncates thread names to 15, keep it visible in ps/gdb
            .thread_name("lancedb-worker")
            .thread_stack_size(
                env_usize(ENV_WORKER_STACK_SIZE).unwrap_or(DEFAULT_WORKER_STACK_BYTES),
            );
        if let Some(threads) = env_usize(ENV_WORKER_THREADS) {
            builder.worker_threads(threads);
        }
        builder.build().expect("Failed to create tokio runtime")
    })
}

fn runtime_error(message: String) -> lancedb::error::Error {
    lancedb::error::Error::Runtime { message }
}

fn panic_message(payload: &Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "non-string panic payload".to_string()
    }
}

/// Run `fut` to completion on the runtime's worker threads and block the
/// calling thread until it finishes.
///
/// The future is never polled on the caller's stack, so callers with small
/// stacks (coroutines, fibers) are safe.  A panic inside `fut` is reported as
/// `Error::Runtime` instead of unwinding across the `extern "C"` boundary
/// (which would abort the process).  Calling this from inside a tokio async
/// context is a misuse that `Runtime::block_on` reports by panicking; that
/// panic is caught and reported as `Error::Runtime` as well.
pub(crate) fn run_blocking<F, T>(fut: F) -> lancedb::error::Result<T>
where
    F: Future<Output = lancedb::error::Result<T>> + Send + 'static,
    T: Send + 'static,
{
    let runtime = get_runtime();
    let handle = runtime.spawn(fut);
    let abort = handle.abort_handle();
    match catch_unwind(AssertUnwindSafe(|| runtime.block_on(handle))) {
        Ok(Ok(result)) => result,
        Ok(Err(join_error)) => Err(runtime_error(if join_error.is_panic() {
            format!("lancedb task panicked: {join_error}")
        } else {
            format!("lancedb task did not complete: {join_error}")
        })),
        Err(payload) => {
            abort.abort();
            Err(runtime_error(format!(
                "lancedb runtime entry failed: {}",
                panic_message(&payload)
            )))
        }
    }
}

/// `run_blocking` for futures whose output is not a `Result`.
pub(crate) fn run_blocking_infallible<F, T>(fut: F) -> lancedb::error::Result<T>
where
    F: Future<Output = T> + Send + 'static,
    T: Send + 'static,
{
    run_blocking(async move { Ok(fut.await) })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn work_runs_on_a_worker_thread_not_the_caller() {
        let caller = std::thread::current().id();
        let worker = run_blocking(async move { Ok(std::thread::current().id()) }).unwrap();
        assert_ne!(caller, worker);
        let name =
            run_blocking_infallible(async { std::thread::current().name().map(str::to_owned) })
                .unwrap();
        assert_eq!(name.as_deref(), Some("lancedb-worker"));
    }

    #[test]
    fn panic_inside_task_becomes_an_error() {
        let result: lancedb::error::Result<()> = run_blocking(async { panic!("boom") });
        match result {
            Err(lancedb::error::Error::Runtime { message }) => {
                assert!(message.contains("boom"), "{message}")
            }
            other => panic!("expected Error::Runtime, got {other:?}"),
        }
    }

    #[test]
    fn small_stack_caller_can_run_deep_work() {
        // A recursive future that would overflow a 64 KiB stack if it were
        // polled on the caller's thread.
        fn depth(n: u32) -> std::pin::Pin<Box<dyn Future<Output = u32> + Send>> {
            Box::pin(async move {
                let pad = [0u8; 2048];
                std::hint::black_box(&pad);
                if n == 0 {
                    0
                } else {
                    depth(n - 1).await + 1
                }
            })
        }
        let handle = std::thread::Builder::new()
            .stack_size(64 * 1024)
            .spawn(|| run_blocking(async { Ok(depth(512).await) }).unwrap())
            .unwrap();
        assert_eq!(handle.join().unwrap(), 512);
    }
}
