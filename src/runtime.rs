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
use std::sync::{Mutex, OnceLock};

use tokio::runtime::{Builder, Runtime};

/// Default stack size for runtime worker (and blocking-pool) threads.
///
/// Upstream measured ~552 KB of stack for a merge_insert in
/// `examples/asio_coroutine.cpp`; reserve 8 MiB to leave headroom for deeper
/// plans and debug builds. Stack pages become resident as they are touched;
/// the reservation alone does not determine resident memory use.
const DEFAULT_WORKER_STACK_BYTES: usize = 8 * 1024 * 1024;

/// Smallest worker stack the runtime accepts. Query planning needs several
/// hundred KB; a stack that is too small fails with a segmentation fault, not
/// an error code. The regression suite overflows 1 MiB during query
/// planning on Linux arm64 Debug; 2 MiB passes. This floor is not a guarantee
/// for every workload, platform or build configuration.
pub const MIN_WORKER_STACK_BYTES: usize = 2 * 1024 * 1024;

// Tokio's multi-thread scheduler packs its searching-worker count into
// 16 bits. Bound the worker count before it allocates per-worker state.
const MAX_WORKER_THREADS: usize = u16::MAX as usize;
const DEFAULT_MAX_BLOCKING_THREADS: usize = 512;

/// Environment override for the worker stack size (bytes).
const ENV_WORKER_STACK_SIZE: &str = "LANCEDB_C_WORKER_STACK_SIZE";
/// Environment override for the number of runtime worker threads.
const ENV_WORKER_THREADS: &str = "LANCEDB_C_WORKER_THREADS";
/// Environment override for the blocking-thread pool limit.
const ENV_MAX_BLOCKING_THREADS: &str = "LANCEDB_C_MAX_BLOCKING_THREADS";

/// Runtime parameters, settable from C before the runtime starts.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RuntimeOptions {
    /// Number of worker threads (0 = tokio's default, the number of CPUs).
    pub worker_threads: usize,
    /// Worker (and blocking-pool) thread stack size in bytes (0 = default).
    pub worker_stack_size: usize,
    /// Upper bound on blocking-pool threads (0 = tokio's default, 512).
    pub max_blocking_threads: usize,
}

static OPTIONS: OnceLock<RuntimeOptions> = OnceLock::new();
static RUNTIME: OnceLock<Runtime> = OnceLock::new();
// Serialize configuration with initialization, including failed attempts.
static INITIALIZATION: Mutex<()> = Mutex::new(());

fn env_usize(name: &str) -> Option<usize> {
    std::env::var(name)
        .ok()?
        .trim()
        .parse()
        .ok()
        .filter(|v| *v > 0)
}

fn validate_options(options: RuntimeOptions) -> lancedb::error::Result<()> {
    if options.worker_stack_size != 0 && options.worker_stack_size < MIN_WORKER_STACK_BYTES {
        return Err(lancedb::error::Error::InvalidInput {
            message: format!(
                "worker stack size {} is below the minimum of {} bytes",
                options.worker_stack_size, MIN_WORKER_STACK_BYTES
            ),
        });
    }
    if options.worker_stack_size > isize::MAX as usize {
        return Err(lancedb::error::Error::InvalidInput {
            message: "worker stack size exceeds the addressable allocation limit".to_string(),
        });
    }
    if options.worker_threads > MAX_WORKER_THREADS {
        return Err(lancedb::error::Error::InvalidInput {
            message: format!("worker thread count exceeds the maximum of {MAX_WORKER_THREADS}"),
        });
    }
    let blocking = if options.max_blocking_threads == 0 {
        DEFAULT_MAX_BLOCKING_THREADS
    } else {
        options.max_blocking_threads
    };
    // A zero worker count will resolve to at least one worker at startup.
    // Validate again after resolving environment settings and defaults.
    if blocking
        .checked_add(options.worker_threads.max(1))
        .is_none()
    {
        return Err(lancedb::error::Error::InvalidInput {
            message: "combined worker and blocking thread count overflows usize".to_string(),
        });
    }
    Ok(())
}

/// Record the options the runtime will be built with. Fails for invalid
/// settings, repeated configuration, or an already running runtime.
pub(crate) fn configure(options: RuntimeOptions) -> lancedb::error::Result<()> {
    validate_options(options)?;
    // A caught builder panic can poison this lock, but the OnceLocks below
    // are only updated atomically after successful configuration/building.
    let _initialization = INITIALIZATION.lock().unwrap_or_else(|e| e.into_inner());
    if RUNTIME.get().is_some() {
        return Err(runtime_error(
            "lancedb runtime is already running; configure it before the first database call"
                .to_string(),
        ));
    }
    OPTIONS
        .set(options)
        .map_err(|_| runtime_error("lancedb runtime options were already set".to_string()))
}

/// The options the runtime is (or will be) built with: explicit
/// configuration first, then environment overrides, then defaults.
pub(crate) fn effective_options() -> RuntimeOptions {
    let configured = OPTIONS.get().copied().unwrap_or_default();
    let pick = |explicit: usize, env: &str| {
        if explicit != 0 {
            explicit
        } else {
            env_usize(env).unwrap_or(0)
        }
    };
    let stack = pick(configured.worker_stack_size, ENV_WORKER_STACK_SIZE);
    RuntimeOptions {
        worker_threads: pick(configured.worker_threads, ENV_WORKER_THREADS),
        worker_stack_size: if stack == 0 {
            DEFAULT_WORKER_STACK_BYTES
        } else {
            stack.max(MIN_WORKER_STACK_BYTES)
        },
        max_blocking_threads: pick(configured.max_blocking_threads, ENV_MAX_BLOCKING_THREADS),
    }
}

// Match Tokio's default selection, but validate before its per-worker
// allocations. An explicit C/LanceDB setting still takes precedence.
fn default_worker_threads() -> lancedb::error::Result<usize> {
    match std::env::var("TOKIO_WORKER_THREADS") {
        Ok(value) => value.parse().ok().filter(|n| *n > 0).ok_or_else(|| {
            runtime_error("TOKIO_WORKER_THREADS must be a positive integer".to_string())
        }),
        Err(std::env::VarError::NotPresent) => {
            Ok(std::thread::available_parallelism().map_or(1, |n| n.get()))
        }
        Err(std::env::VarError::NotUnicode(_)) => Err(runtime_error(
            "TOKIO_WORKER_THREADS must be valid Unicode".to_string(),
        )),
    }
}

/// The process-wide runtime. Failed initialization is not cached: subsequent
/// calls may retry after a transient resource failure or an environment fix.
pub(crate) fn get_runtime() -> lancedb::error::Result<&'static Runtime> {
    if let Some(runtime) = RUNTIME.get() {
        return Ok(runtime);
    }
    let _initialization = INITIALIZATION.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(runtime) = RUNTIME.get() {
        return Ok(runtime);
    }
    let mut opts = effective_options();
    if opts.worker_threads == 0 {
        opts.worker_threads = default_worker_threads()?;
    }
    validate_options(opts).map_err(|e| runtime_error(format!("invalid runtime settings: {e}")))?;
    let mut builder = Builder::new_multi_thread();
    builder
        .enable_all()
        // 14 chars: Linux truncates thread names to 15, keep it visible in ps/gdb
        .thread_name("lancedb-worker")
        .thread_stack_size(opts.worker_stack_size)
        .worker_threads(opts.worker_threads);
    if opts.max_blocking_threads != 0 {
        builder.max_blocking_threads(opts.max_blocking_threads);
    }
    let runtime = builder
        .build()
        .map_err(|e| runtime_error(format!("failed to create tokio runtime: {e}")))?;
    Ok(RUNTIME.get_or_init(|| runtime))
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
/// panic is caught before any work is spawned. Initialization failures are
/// reported as `Error::Runtime` as well.
pub(crate) fn run_blocking<F, T>(fut: F) -> lancedb::error::Result<T>
where
    F: Future<Output = lancedb::error::Result<T>> + Send + 'static,
    T: Send + 'static,
{
    let mut abort = None;
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        let runtime = get_runtime()?;
        // Runtime::block_on validates entry before polling this bridge. The
        // operation itself is only ever polled by a spawned worker task.
        runtime.block_on(async {
            let handle = runtime.spawn(fut);
            abort = Some(handle.abort_handle());
            handle.await.map_err(|join_error| {
                runtime_error(if join_error.is_panic() {
                    format!("lancedb task panicked: {join_error}")
                } else {
                    format!("lancedb task did not complete: {join_error}")
                })
            })?
        })
    }));
    match outcome {
        Ok(result) => result,
        Err(payload) => {
            if let Some(abort) = abort {
                abort.abort();
            }
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
mod tests;
