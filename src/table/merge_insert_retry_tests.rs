use super::*;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use arrow_array::{Int32Array, RecordBatch, RecordBatchIterator};
use arrow_schema::{DataType, Field, Schema};

use crate::connection::{
    lancedb_connect, lancedb_connect_builder_execute, lancedb_connect_builder_storage_option,
    lancedb_connection_free, lancedb_connection_open_table, lancedb_table_free, LanceDBConnection,
    LanceDBTable,
};
use crate::error::lancedb_free_string;

struct LocalMergeServer {
    uri: String,
    merge_requests: Arc<AtomicUsize>,
    stop: Arc<AtomicBool>,
    handle: Option<thread::JoinHandle<()>>,
}

impl LocalMergeServer {
    fn start() -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind local merge fixture");
        listener
            .set_nonblocking(true)
            .expect("set fixture listener nonblocking");
        let uri = format!("http://{}", listener.local_addr().expect("fixture address"));
        let merge_requests = Arc::new(AtomicUsize::new(0));
        let stop = Arc::new(AtomicBool::new(false));

        let thread_count = Arc::clone(&merge_requests);
        let thread_stop = Arc::clone(&stop);
        let handle = thread::spawn(move || {
            while !thread_stop.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((stream, _)) => {
                        let _ = serve_request(stream, &thread_count);
                    }
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                        thread::sleep(Duration::from_millis(1));
                    }
                    Err(_) => break,
                }
            }
        });

        Self {
            uri,
            merge_requests,
            stop,
            handle: Some(handle),
        }
    }

    fn merge_requests(&self) -> usize {
        self.merge_requests.load(Ordering::SeqCst)
    }
}

impl Drop for LocalMergeServer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

fn serve_request(mut stream: TcpStream, merge_requests: &AtomicUsize) -> std::io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    let mut reader = BufReader::new(stream.try_clone()?);

    let mut request_line = String::new();
    reader.read_line(&mut request_line)?;
    let path = request_line
        .split_whitespace()
        .nth(1)
        .unwrap_or_default()
        .to_string();

    let mut content_length = 0usize;
    let mut chunked = false;
    loop {
        let mut header = String::new();
        if reader.read_line(&mut header)? == 0 || header == "\r\n" {
            break;
        }
        let lower = header.to_ascii_lowercase();
        if let Some(value) = lower.strip_prefix("content-length:") {
            content_length = value.trim().parse().unwrap_or(0);
        }
        if lower.starts_with("transfer-encoding:") && lower.contains("chunked") {
            chunked = true;
        }
    }

    // Drain the request body so the client can finish its upload before the
    // response closes the connection.
    if content_length > 0 {
        let mut body = vec![0; content_length];
        reader.read_exact(&mut body)?;
    } else if chunked {
        loop {
            let mut size_line = String::new();
            reader.read_line(&mut size_line)?;
            let size = usize::from_str_radix(size_line.trim().split(';').next().unwrap_or("0"), 16)
                .unwrap_or(0);
            let mut chunk = vec![0; size];
            reader.read_exact(&mut chunk)?;
            let mut trailing = [0; 2];
            reader.read_exact(&mut trailing)?;
            if size == 0 {
                break;
            }
        }
    }

    let (status, body) = if path.ends_with("/describe/") {
        (
            "200 OK",
            r#"{"version":1,"schema":{"fields":[{"name":"id","type":{"type":"int32"},"nullable":false}]}}"#,
        )
    } else if path.contains("/merge_insert/") {
        merge_requests.fetch_add(1, Ordering::SeqCst);
        ("409 Conflict", "{}")
    } else {
        ("404 Not Found", "{}")
    };

    let response = format!(
        "HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len(),
    );
    stream.write_all(response.as_bytes())?;
    stream.flush()
}

unsafe fn open_remote_table(server: &LocalMergeServer) -> *mut LanceDBTable {
    let uri = CString::new("db://").expect("fixture uri");
    let builder = lancedb_connect(uri.as_ptr());
    assert!(!builder.is_null());

    let mut builder = builder;
    for (key, value) in [
        ("remote_database_api_key", "fixture"),
        ("remote_database_region", "fixture"),
        ("remote_database_host_override", server.uri.as_str()),
    ] {
        let key = CString::new(key).expect("fixture option key");
        let value = CString::new(value).expect("fixture option value");
        builder = lancedb_connect_builder_storage_option(builder, key.as_ptr(), value.as_ptr());
        assert!(!builder.is_null());
    }

    let mut connection: *mut LanceDBConnection = ptr::null_mut();
    let mut error_message: *mut c_char = ptr::null_mut();
    assert_eq!(
        lancedb_connect_builder_execute(builder, &mut connection, &mut error_message),
        LanceDBError::Success
    );
    assert!(error_message.is_null());

    let table_name = CString::new("t").expect("fixture table name");
    let mut table: *mut LanceDBTable = ptr::null_mut();
    assert_eq!(
        lancedb_connection_open_table(
            connection,
            table_name.as_ptr(),
            &mut table,
            &mut error_message,
        ),
        LanceDBError::Success
    );
    assert!(error_message.is_null());
    lancedb_connection_free(connection);
    table
}

fn fixture_reader() -> *mut LanceDBRecordBatchReader {
    let schema = Arc::new(Schema::new(vec![Field::new("id", DataType::Int32, false)]));
    let batch = RecordBatch::try_new(
        Arc::clone(&schema),
        vec![Arc::new(Int32Array::from(vec![1]))],
    )
    .expect("fixture batch");
    let reader = RecordBatchIterator::new(vec![Ok(batch)], schema);
    Box::into_raw(Box::new(LanceDBRecordBatchReader::new(Box::new(reader))))
}

unsafe fn run_merge(retries: Option<u8>) -> (usize, LanceDBError, bool) {
    let server = LocalMergeServer::start();
    let table = open_remote_table(&server);
    let data = fixture_reader();
    let on_columns = [CString::new("id").expect("fixture join key")];
    let on_column_ptrs = [on_columns[0].as_ptr()];
    let options = LanceDBMergeInsertOptions {
        when_matched_update_all: 1,
        when_matched_update_all_condition: ptr::null(),
        when_not_matched_insert_all: 1,
        when_not_matched_by_source_delete: 0,
        when_not_matched_by_source_delete_filter: ptr::null(),
        use_index: -1,
        timeout_ms: -1,
    };
    let retries_pointer = retries
        .as_ref()
        .map_or(ptr::null(), |retries| retries as *const u8);
    let mut result = LanceDBMergeInsertResultV2::default();
    let mut error_message: *mut c_char = ptr::null_mut();

    let status = lancedb_table_merge_insert_with_options(
        table,
        data,
        on_column_ptrs.as_ptr(),
        1,
        &options,
        retries_pointer,
        ptr::null(),
        &mut result,
        &mut error_message,
    );
    let has_error = !error_message.is_null();
    if has_error {
        lancedb_free_string(error_message);
    }
    lancedb_table_free(table);

    (server.merge_requests(), status, has_error)
}

// The raw default and an explicit zero are distinct: the SDK keeps its own
// retry budget for the former and sends no retry override for the latter.
// A local 409 fixture observes the actual merge requests, so this reaches the
// SDK transport decision instead of only checking our pointer conversion.
#[test]
fn merge_insert_retry_override_reaches_the_remote_transport() {
    let (default_requests, default_status, default_error) = unsafe { run_merge(None) };
    assert_ne!(default_status, LanceDBError::Success);
    assert!(default_error);
    assert!(
        default_requests > 1,
        "the SDK default should retry a 409, saw {default_requests} request(s)"
    );

    let (zero_requests, zero_status, zero_error) = unsafe { run_merge(Some(0)) };
    assert_ne!(zero_status, LanceDBError::Success);
    assert!(zero_error);
    assert_eq!(
        zero_requests, 1,
        "an explicit zero retry budget must disable transport retries"
    );
}
