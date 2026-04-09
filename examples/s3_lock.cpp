/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * S3Lock — distributed lock using S3 conditional writes.
 *
 * Lock protocol only — all S3 HTTP operations delegated to s3_http.
 * No dependency on the main application code.
 * Dependencies: base_lock.h, s3_lock.h, s3_http.h
 */

#include "s3_lock.h"
#include "s3_http.h"

#include <chrono>
#include <thread>
#include <iostream>
#include <unistd.h>

// ============================================================================
// S3Lock implementation
// ============================================================================

// Build an S3HttpConfig from S3LockConfig
static S3HttpConfig make_http_config(const S3LockConfig& cfg) {
    return S3HttpConfig{
        cfg.endpoint,
        cfg.region,
        cfg.access_key_id,
        cfg.secret_access_key,
        cfg.use_path_style
    };
}

S3Lock::S3Lock(const S3LockConfig& config) : config_(config) {}

S3Lock::~S3Lock() {
    if (locked_) {
        unlock();
    }
}

bool S3Lock::is_locked() const {
    return locked_;
}

bool S3Lock::is_valid() const {
    return !config_.endpoint.empty() &&
           !config_.bucket.empty() &&
           !config_.lock_key.empty() &&
           !config_.access_key_id.empty() &&
           !config_.secret_access_key.empty();
}

bool S3Lock::lock_exclusive() {
    if (locked_) return true;

    for (int attempt = 0; attempt <= config_.max_retries; attempt++) {
        if (put_lock_object()) {
            locked_ = true;
            return true;
        }

        if (check_stale_and_reclaim()) {
            locked_ = true;
            return true;
        }

        if (attempt < config_.max_retries) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.retry_interval_ms));
        }
    }

    return false;
}

bool S3Lock::try_lock_exclusive() {
    if (locked_) return true;

    if (put_lock_object()) {
        locked_ = true;
        return true;
    }

    if (check_stale_and_reclaim()) {
	//does the lock is stale and we can reclaim it?
        locked_ = true;
        return true;
    }

    return false;
}

void S3Lock::unlock() {
    if (!locked_) return;
    delete_lock_object();
    locked_ = false;
}

// ============================================================================
// Lock protocol operations
// ============================================================================

bool S3Lock::put_lock_object() {
    auto now = std::chrono::system_clock::now();
    auto epoch_secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    std::string body = "{\"pid\":" + std::to_string(getpid()) +
                       ",\"timestamp\":" + std::to_string(epoch_secs) +
                       ",\"host\":\"local\"}";

    auto result = s3_put_object(make_http_config(config_),
                                config_.bucket, config_.lock_key,
                                body, "application/json",
                                /*if_none_match=*/true);
    return result.ok;
}

bool S3Lock::delete_lock_object() {
    auto result = s3_delete_object(make_http_config(config_),
                                   config_.bucket, config_.lock_key);
    return result.ok;
}

bool S3Lock::check_stale_and_reclaim() {
    auto result = s3_get_object(make_http_config(config_),
                                config_.bucket, config_.lock_key);
    if (!result.ok) return false;

    // Parse timestamp from lock body: {"pid":123,"timestamp":1234567890,...}
    auto ts_pos = result.body.find("\"timestamp\":");
    if (ts_pos == std::string::npos) return false;
    ts_pos += 12;
    long long lock_timestamp = 0;
    try {
        lock_timestamp = std::stoll(result.body.substr(ts_pos));
    } catch (...) {
        return false;
    }

    auto now = std::chrono::system_clock::now();
    auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    long long age = now_secs - lock_timestamp;
    if (age < config_.lock_ttl_seconds) {
        return false; // Lock is still fresh
    }

    std::cerr << "S3Lock: reclaiming stale lock (age=" << age << "s, ttl="
              << config_.lock_ttl_seconds << "s)" << std::endl;
    //long time since the lock was created exceeds TTL, we consider it stale and try to reclaim it
    delete_lock_object();
    return put_lock_object();
}
