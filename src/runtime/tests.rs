use super::*;
use crate::connection::{lancedb_runtime_configure, LanceDBRuntimeOptions};
use crate::error::LanceDBError;
use std::sync::{mpsc, Arc};
use std::time::Duration;

// Runtime configuration is process-wide. Each configuration test runs itself
// in a fresh process so the normal parallel tests cannot initialize it first.
fn isolated_process(test_name: &str, runtime_env: &[(&str, &str)]) -> bool {
    const CHILD_TEST: &str = "LANCEDB_C_RUNTIME_TEST_CHILD";
    if std::env::var(CHILD_TEST).as_deref() == Ok(test_name) {
        return true;
    }
    let mut child = std::process::Command::new(std::env::current_exe().unwrap());
    child
        .args(["--exact", test_name, "--nocapture"])
        .env(CHILD_TEST, test_name)
        .env_remove("TOKIO_WORKER_THREADS");
    for (name, value) in runtime_env {
        child.env(name, value);
    }
    let output = child.output().unwrap();
    assert!(
        output.status.success(),
        "isolated test {test_name} failed with {}\nstdout:\n{}\nstderr:\n{}",
        output.status,
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    false
}

fn configure_from_c(options: LanceDBRuntimeOptions) {
    let mut message = std::ptr::null_mut();
    let result = unsafe { lancedb_runtime_configure(&options, &mut message) };
    let detail = if message.is_null() {
        String::new()
    } else {
        unsafe {
            let detail = std::ffi::CStr::from_ptr(message)
                .to_string_lossy()
                .into_owned();
            crate::error::lancedb_free_string(message);
            detail
        }
    };
    assert_eq!(result, LanceDBError::Success, "{detail}");
}

// The error code and message a C caller receives for `error`.
fn c_error(error: &lancedb::error::Error) -> (LanceDBError, String) {
    let mut message = std::ptr::null_mut();
    let code = unsafe { crate::error::handle_error(error, &mut message) };
    let detail = if message.is_null() {
        String::new()
    } else {
        unsafe {
            let detail = std::ffi::CStr::from_ptr(message)
                .to_string_lossy()
                .into_owned();
            crate::error::lancedb_free_string(message);
            detail
        }
    };
    (code, detail)
}

#[test]
fn work_runs_on_a_worker_thread_not_the_caller() {
    let caller = std::thread::current().id();
    let worker = run_blocking(async move { Ok(std::thread::current().id()) }).unwrap();
    assert_ne!(caller, worker);
    let name = run_blocking_infallible(async { std::thread::current().name().map(str::to_owned) })
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
    // The C entry points report it as LANCEDB_RUNTIME with a message, and
    // the runtime stays usable.
    let error = run_blocking(async {
        panic!("boom");
        #[allow(unreachable_code)]
        Ok(())
    })
    .unwrap_err();
    let (code, detail) = c_error(&error);
    assert_eq!(code, LanceDBError::Runtime);
    assert!(detail.contains("panicked"), "{detail}");
    assert_eq!(
        unsafe { crate::error::handle_error(&error, std::ptr::null_mut()) },
        LanceDBError::Runtime
    );
    assert_eq!(run_blocking(async { Ok(42) }).unwrap(), 42);
}

#[test]
fn nested_runtime_call_rejects_and_drops_work_synchronously() {
    if !isolated_process(
        "runtime::tests::nested_runtime_call_rejects_and_drops_work_synchronously",
        &[],
    ) {
        return;
    }
    configure_from_c(LanceDBRuntimeOptions {
        worker_threads: 1,
        ..Default::default()
    });

    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
    struct Probe {
        polls: Arc<AtomicUsize>,
        dropped: Arc<AtomicBool>,
    }
    impl Future for Probe {
        type Output = lancedb::error::Result<()>;
        fn poll(
            self: std::pin::Pin<&mut Self>,
            _: &mut std::task::Context<'_>,
        ) -> std::task::Poll<Self::Output> {
            self.polls.fetch_add(1, Ordering::SeqCst);
            std::task::Poll::Ready(Ok(()))
        }
    }
    impl Drop for Probe {
        fn drop(&mut self) {
            self.dropped.store(true, Ordering::SeqCst);
        }
    }

    run_blocking(async {
        let polls = Arc::new(AtomicUsize::new(0));
        let dropped = Arc::new(AtomicBool::new(false));
        let result = run_blocking(Probe {
            polls: polls.clone(),
            dropped: dropped.clone(),
        });
        assert!(matches!(result, Err(lancedb::error::Error::Runtime { .. })));
        let (code, detail) = c_error(result.as_ref().unwrap_err());
        assert_eq!(code, LanceDBError::Runtime);
        assert!(!detail.is_empty());
        assert_eq!(polls.load(Ordering::SeqCst), 0);
        // With only one worker, an incorrectly spawned/aborted task cannot be
        // dropped by another worker before this assertion. Rejection must own
        // cleanup synchronously, before reporting failure to the C caller.
        assert!(dropped.load(Ordering::SeqCst));
        Ok(())
    })
    .unwrap();
}

#[test]
fn entered_handle_allows_a_blocking_call() {
    let runtime = get_runtime().unwrap();
    let _entered = runtime.enter();
    assert_eq!(run_blocking(async { Ok(42) }).unwrap(), 42);
}

#[test]
fn block_in_place_allows_a_blocking_call() {
    assert_eq!(
        run_blocking(async { tokio::task::block_in_place(|| run_blocking(async { Ok(42) })) })
            .unwrap(),
        42
    );
}

#[cfg(target_os = "linux")]
fn current_thread_stack_size() -> usize {
    unsafe {
        let mut attr = std::mem::MaybeUninit::<libc::pthread_attr_t>::uninit();
        assert_eq!(
            libc::pthread_getattr_np(libc::pthread_self(), attr.as_mut_ptr()),
            0
        );
        let mut attr = attr.assume_init();
        let mut stack_size = 0;
        let result = libc::pthread_attr_getstacksize(&attr, &mut stack_size);
        assert_eq!(libc::pthread_attr_destroy(&mut attr), 0);
        assert_eq!(result, 0);
        stack_size
    }
}

fn assert_runtime_configuration_applied() {
    let runtime = get_runtime().unwrap();
    assert_eq!(runtime.metrics().num_workers(), 1);
    #[cfg(target_os = "linux")]
    {
        let size = run_blocking(async { Ok(current_thread_stack_size()) }).unwrap();
        let requested = 2 * 1024 * 1024;
        assert!(
            (requested..=requested + 64 * 1024).contains(&size),
            "{size}"
        );
    }

    // Hold the one configured blocking slot, then verify a second task is
    // queued until it is released. Always release it even if an assertion fails.
    struct ReleaseOnDrop(Option<mpsc::Sender<()>>);
    impl Drop for ReleaseOnDrop {
        fn drop(&mut self) {
            if let Some(tx) = self.0.take() {
                let _ = tx.send(());
            }
        }
    }
    let (release_tx, release_rx) = mpsc::channel();
    let release = ReleaseOnDrop(Some(release_tx));
    let (started_tx, started_rx) = mpsc::channel();
    let first = runtime.spawn_blocking(move || {
        started_tx.send(()).unwrap();
        release_rx.recv_timeout(Duration::from_secs(10)).unwrap();
    });
    started_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    let (second_tx, second_rx) = mpsc::channel();
    let second = runtime.spawn_blocking(move || second_tx.send(()).unwrap());
    assert_eq!(
        second_rx.recv_timeout(Duration::from_millis(100)),
        Err(mpsc::RecvTimeoutError::Timeout)
    );
    drop(release);
    second_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    runtime.block_on(first).unwrap();
    runtime.block_on(second).unwrap();
}

#[test]
fn c_configuration_is_applied() {
    if !isolated_process("runtime::tests::c_configuration_is_applied", &[]) {
        return;
    }
    configure_from_c(LanceDBRuntimeOptions {
        worker_threads: 1,
        worker_stack_size: 2 * 1024 * 1024,
        max_blocking_threads: 1,
    });
    assert_eq!(
        effective_options(),
        RuntimeOptions {
            worker_threads: 1,
            worker_stack_size: 2 * 1024 * 1024,
            max_blocking_threads: 1,
        }
    );
    assert_runtime_configuration_applied();
}

#[test]
fn zero_c_configuration_uses_defaults() {
    if !isolated_process("runtime::tests::zero_c_configuration_uses_defaults", &[]) {
        return;
    }
    configure_from_c(LanceDBRuntimeOptions::default());
    assert_eq!(
        effective_options(),
        RuntimeOptions {
            worker_threads: 0,
            worker_stack_size: DEFAULT_WORKER_STACK_BYTES,
            max_blocking_threads: 0,
        }
    );
    let runtime = get_runtime().unwrap();
    assert_eq!(
        runtime.metrics().num_workers(),
        default_worker_threads().unwrap()
    );
}

#[test]
fn tokio_worker_environment_sets_the_default_worker_count() {
    if !isolated_process(
        "runtime::tests::tokio_worker_environment_sets_the_default_worker_count",
        &[("TOKIO_WORKER_THREADS", "1")],
    ) {
        return;
    }
    configure_from_c(LanceDBRuntimeOptions {
        worker_stack_size: 2 * 1024 * 1024,
        max_blocking_threads: 1,
        ..Default::default()
    });
    assert_runtime_configuration_applied();
}

#[test]
fn c_worker_count_overrides_tokio_worker_environment() {
    if !isolated_process(
        "runtime::tests::c_worker_count_overrides_tokio_worker_environment",
        &[("TOKIO_WORKER_THREADS", "not-a-number")],
    ) {
        return;
    }
    configure_from_c(LanceDBRuntimeOptions {
        worker_threads: 1,
        worker_stack_size: 2 * 1024 * 1024,
        max_blocking_threads: 1,
    });
    assert_runtime_configuration_applied();
}

#[test]
fn invalid_tokio_worker_environment_is_reported() {
    if !isolated_process(
        "runtime::tests::invalid_tokio_worker_environment_is_reported",
        &[("TOKIO_WORKER_THREADS", "not-a-number")],
    ) {
        return;
    }
    let result = run_blocking(async { Ok(()) });
    match result {
        Err(lancedb::error::Error::Runtime { message }) => {
            assert!(message.contains("TOKIO_WORKER_THREADS"), "{message}");
        }
        other => panic!("expected a TOKIO_WORKER_THREADS error, got {other:?}"),
    }
    assert!(RUNTIME.get().is_none());
}

#[test]
fn oversized_tokio_worker_environment_is_rejected() {
    let too_many_workers = usize::MAX.to_string();
    if !isolated_process(
        "runtime::tests::oversized_tokio_worker_environment_is_rejected",
        &[("TOKIO_WORKER_THREADS", &too_many_workers)],
    ) {
        return;
    }
    let result = run_blocking(async { Ok(()) });
    match result {
        Err(lancedb::error::Error::Runtime { message }) => {
            assert!(message.contains("worker thread count"), "{message}");
        }
        other => panic!("expected a worker-count validation error, got {other:?}"),
    }
    assert!(RUNTIME.get().is_none());
}

#[test]
fn resolved_thread_count_overflow_is_reported() {
    if !isolated_process(
        "runtime::tests::resolved_thread_count_overflow_is_reported",
        &[("TOKIO_WORKER_THREADS", "2")],
    ) {
        return;
    }
    // The worker default is deferred during configuration. Its actual value
    // must participate in the overflow check before building the runtime.
    configure_from_c(LanceDBRuntimeOptions {
        max_blocking_threads: usize::MAX - 1,
        ..Default::default()
    });
    let result = run_blocking(async { Ok(()) });
    match result {
        Err(lancedb::error::Error::Runtime { message }) => {
            assert!(message.contains("overflows"), "{message}");
        }
        other => panic!("expected a thread-count overflow error, got {other:?}"),
    }
    assert!(RUNTIME.get().is_none());
}

#[test]
#[cfg(all(target_os = "linux", target_pointer_width = "64"))]
fn initialization_failure_is_reported_through_c() {
    if !isolated_process(
        "runtime::tests::initialization_failure_is_reported_through_c",
        &[],
    ) {
        return;
    }
    // Representable by the API, but far beyond a Linux process's address
    // space: pthread creation fails without allocating or touching that memory.
    configure_from_c(LanceDBRuntimeOptions {
        worker_threads: 1,
        worker_stack_size: (isize::MAX as usize) / 2,
        max_blocking_threads: 1,
    });
    unsafe {
        let builder = crate::connection::lancedb_connect(c"memory://runtime-init-test".as_ptr());
        assert!(!builder.is_null());
        let mut connection = std::ptr::null_mut();
        let mut message = std::ptr::null_mut();
        let result = crate::connection::lancedb_connect_builder_execute(
            builder,
            &mut connection,
            &mut message,
        );
        let detail = if message.is_null() {
            String::new()
        } else {
            let detail = std::ffi::CStr::from_ptr(message)
                .to_string_lossy()
                .into_owned();
            crate::error::lancedb_free_string(message);
            detail
        };
        if !connection.is_null() {
            crate::connection::lancedb_connection_free(connection);
        }
        assert_eq!(result, LanceDBError::Runtime, "{detail}");
        assert!(!detail.is_empty());
        assert!(connection.is_null());
    }
}

#[test]
fn stack_size_floor_is_enforced() {
    let too_small = RuntimeOptions {
        worker_stack_size: MIN_WORKER_STACK_BYTES / 2,
        ..Default::default()
    };
    assert!(matches!(
        configure(too_small),
        Err(lancedb::error::Error::InvalidInput { .. })
    ));
    // the runtime is already running in this test binary (other tests use it),
    // so a valid configuration is refused as too late rather than applied
    let _ = get_runtime().unwrap();
    assert!(matches!(
        configure(RuntimeOptions {
            worker_threads: 2,
            ..Default::default()
        }),
        Err(lancedb::error::Error::Runtime { .. })
    ));
    assert!(effective_options().worker_stack_size >= MIN_WORKER_STACK_BYTES);
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
