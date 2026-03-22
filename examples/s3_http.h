/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * S3 HTTP client — all S3 operations (object and bucket) using libcurl + SigV4.
 *
 * Provides a uniform interface for S3 interactions. Used by:
 *   - S3Lock (s3_lock.cpp) for lock object operations
 *   - Main application for bucket operations (CreateVectorBucket)
 *
 * No dependency on the main application code.
 * Dependencies: s3_sigv4.h, libcurl, OpenSSL.
 */

#pragma once

#include <string>

// S3 connection configuration — shared across all S3 HTTP operations
struct S3HttpConfig {
    std::string endpoint;           // S3 endpoint URL (e.g. "http://localhost:8000")
    std::string region;             // AWS region (e.g. "us-east-1")
    std::string access_key_id;      // AWS access key
    std::string secret_access_key;  // AWS secret key
    bool use_path_style = true;     // Path-style addressing (required for Ceph/RGW)
};

// Result of an S3 HTTP operation
struct S3HttpResult {
    long http_code = 0;
    std::string body;
    bool ok = false;                // true if curl succeeded and HTTP status is 2xx
};

// ============================================================================
// Object operations
// ============================================================================

// PUT an object. If if_none_match is true, adds "If-None-Match: *" header
// (conditional write — fails with 412 if the object already exists).
S3HttpResult s3_put_object(const S3HttpConfig& config,
                           const std::string& bucket, const std::string& key,
                           const std::string& body,
                           const std::string& content_type = "",
                           bool if_none_match = false);

// GET an object. Returns body in result.body.
S3HttpResult s3_get_object(const S3HttpConfig& config,
                           const std::string& bucket, const std::string& key);

// DELETE an object.
S3HttpResult s3_delete_object(const S3HttpConfig& config,
                              const std::string& bucket, const std::string& key);

// ============================================================================
// Bucket operations
// ============================================================================

// Create an S3 bucket.
S3HttpResult s3_create_bucket(const S3HttpConfig& config, const std::string& bucket);

// Check if an S3 bucket exists (HEAD bucket).
S3HttpResult s3_head_bucket(const S3HttpConfig& config, const std::string& bucket);
