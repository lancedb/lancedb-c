/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * S3Lock - Distributed lock using S3 conditional writes (If-None-Match: *)
 *
 * Implements Base_lock using an S3 object as a lock.
 * - lock_exclusive():     PUT object with If-None-Match: * (fails if object exists)
 * - try_lock_exclusive():  Same as lock_exclusive() but non-blocking (single attempt)
 * - unlock():             DELETE the lock object
 * - Crash recovery:       Lock objects include a timestamp; stale locks (exceeding TTL)
 *                         are deleted and re-acquired automatically.
 *
 * Dependencies: libcurl, OpenSSL (for SigV4 signing)
 * No dependency on the main application code — communicates only via Base_lock interface.
 */

#pragma once

#include "base_lock.h"
#include <string>

// Configuration for S3Lock — self-contained, no dependency on application config types
struct S3LockConfig {
    std::string endpoint;           // S3 endpoint URL (e.g. "http://localhost:8000")
    std::string region;             // AWS region (e.g. "us-east-1")
    std::string access_key_id;      // AWS access key
    std::string secret_access_key;  // AWS secret key
    std::string bucket;             // S3 bucket name
    std::string lock_key;           // Object key for the lock (e.g. ".locks/my-index.lock")
    bool use_path_style = true;     // Path-style addressing (required for Ceph/RGW)
    bool allow_http = false;        // Allow HTTP (non-HTTPS) endpoints
    int lock_ttl_seconds = 300;     // Stale lock TTL for crash recovery (default: 5 min)
    int retry_interval_ms = 500;    // Retry interval when lock is held (for lock_exclusive)
    int max_retries = 60;           // Max retries for lock_exclusive (0 = single attempt)
};

class S3Lock : public Base_lock {
public:
    explicit S3Lock(const S3LockConfig& config);
    ~S3Lock() override;

    // Non-copyable, non-movable
    S3Lock(const S3Lock&) = delete;
    S3Lock& operator=(const S3Lock&) = delete;

    bool lock_exclusive() override;
    bool try_lock_exclusive() override;
    void unlock() override;
    bool is_locked() const override;
    bool is_valid() const override;

private:
    S3LockConfig config_;
    bool locked_ = false;

    // S3 operations
    bool put_lock_object();     // PUT with If-None-Match: *
    bool delete_lock_object();  // DELETE
    bool check_stale_and_reclaim(); // HEAD + check TTL + DELETE if stale
};
