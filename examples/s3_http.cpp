/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * S3 HTTP client implementation using libcurl + AWS SigV4.
 *
 * No dependency on the main application code.
 * Dependencies: s3_http.h, s3_sigv4.h, libcurl, OpenSSL.
 */

#include "s3_http.h"
#include "s3_sigv4.h"

#include <string>


// ============================================================================
// Internal helpers
// ============================================================================

namespace {

// Build URL and canonical URI for an object operation (path-style only for now)
struct S3Url {
    std::string url;
    std::string canonical_uri;
    std::string host;
};

S3Url build_object_url(const S3HttpConfig& config, const std::string& bucket,
                       const std::string& key) {
    S3Url result;
    result.host = sigv4::extract_host(config.endpoint);

    if (config.use_path_style) {
        result.url = config.endpoint + "/" + bucket + "/" + key;
        result.canonical_uri = "/" + bucket + "/" + key;
    } else {
        auto scheme_end = config.endpoint.find("://");
        if (scheme_end != std::string::npos) {
            result.url = config.endpoint.substr(0, scheme_end + 3) + bucket + "." +
                         config.endpoint.substr(scheme_end + 3) + "/" + key;
        }
        result.canonical_uri = "/" + key;
        result.host = bucket + "." + result.host;
    }
    return result;
}

S3Url build_bucket_url(const S3HttpConfig& config, const std::string& bucket) {
    S3Url result;
    result.host = sigv4::extract_host(config.endpoint);
    result.url = config.endpoint + "/" + bucket + "/";
    result.canonical_uri = "/" + bucket + "/";
    return result;
}

// Execute a signed S3 request
S3HttpResult execute_request(const std::string& method,
                             const S3Url& s3url,
                             const S3HttpConfig& config,
                             const std::string& body = "",
                             const std::string& content_type = "",
                             bool if_none_match = false,
                             bool head_only = false) {
    S3HttpResult result;

    CURL* curl = curl_easy_init();
    if (!curl) return result;

    std::string payload_hash = sigv4::sha256_hex(body);

    // For conditional PUT (If-None-Match: *), we need to sign the extra header
    std::string extra_headers_canonical;
    std::string extra_signed_headers;
    if (if_none_match) {
        extra_headers_canonical = "if-none-match:*\n";
        extra_signed_headers = "if-none-match";

        // if-none-match sorts between host and x-amz-*, so we must build
        // the canonical headers manually with correct sort order
        auto ts = sigv4::get_timestamp();
        std::string service = "s3";
        std::string scope = ts.date_stamp + "/" + config.region + "/" + service + "/aws4_request";

        std::string canonical_headers =
            "host:" + s3url.host + "\n"
            "if-none-match:*\n"
            "x-amz-content-sha256:" + payload_hash + "\n"
            "x-amz-date:" + ts.iso8601 + "\n";
        std::string signed_headers = "host;if-none-match;x-amz-content-sha256;x-amz-date";

        std::string canonical_request =
            method + "\n" +
            sigv4::uri_encode_path(s3url.canonical_uri) + "\n"
            "\n" +
            canonical_headers + "\n" +
            signed_headers + "\n" +
            payload_hash;

        std::string string_to_sign =
            "AWS4-HMAC-SHA256\n" +
            ts.iso8601 + "\n" +
            scope + "\n" +
            sigv4::sha256_hex(canonical_request);

        std::string date_key = sigv4::hmac_sha256_raw("AWS4" + config.secret_access_key, ts.date_stamp);
        std::string date_region_key = sigv4::hmac_sha256_raw(date_key, config.region);
        std::string date_region_service_key = sigv4::hmac_sha256_raw(date_region_key, service);
        std::string signing_key = sigv4::hmac_sha256_raw(date_region_service_key, "aws4_request");
        std::string signature = sigv4::hmac_sha256_hex(signing_key, string_to_sign);

        std::string authorization = "AWS4-HMAC-SHA256 Credential=" + config.access_key_id + "/" + scope +
                                   ", SignedHeaders=" + signed_headers +
                                   ", Signature=" + signature;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Host: " + s3url.host).c_str());
        headers = curl_slist_append(headers, ("Authorization: " + authorization).c_str());
        headers = curl_slist_append(headers, ("x-amz-date: " + ts.iso8601).c_str());
        headers = curl_slist_append(headers, ("x-amz-content-sha256: " + payload_hash).c_str());
        headers = curl_slist_append(headers, "If-None-Match: *");
        if (!content_type.empty()) {
            headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, s3url.url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sigv4::curl_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        result.ok = (res == CURLE_OK && result.http_code >= 200 && result.http_code < 300);
        return result;
    }

    // Standard request (no If-None-Match)
    auto signed_req = sigv4::sign_request(
        method, s3url.canonical_uri, s3url.host, payload_hash,
        config.region, config.access_key_id, config.secret_access_key);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Host: " + s3url.host).c_str());
    headers = curl_slist_append(headers, ("Authorization: " + signed_req.authorization).c_str());
    headers = curl_slist_append(headers, ("x-amz-date: " + signed_req.x_amz_date).c_str());
    headers = curl_slist_append(headers, ("x-amz-content-sha256: " + payload_hash).c_str());
    if (!content_type.empty()) {
        headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());
    }
    if (body.empty() && method == "PUT") {
        headers = curl_slist_append(headers, "Content-Length: 0");
    }

    curl_easy_setopt(curl, CURLOPT_URL, s3url.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (head_only) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sigv4::curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    result.ok = (res == CURLE_OK && result.http_code >= 200 && result.http_code < 300);
    return result;
}

} // anonymous namespace

// ============================================================================
// Object operations
// ============================================================================

S3HttpResult s3_put_object(const S3HttpConfig& config,
                           const std::string& bucket, const std::string& key,
                           const std::string& body,
                           const std::string& content_type,
                           bool if_none_match) {
    auto s3url = build_object_url(config, bucket, key);
    return execute_request("PUT", s3url, config, body, content_type, if_none_match);
}

S3HttpResult s3_get_object(const S3HttpConfig& config,
                           const std::string& bucket, const std::string& key) {
    auto s3url = build_object_url(config, bucket, key);
    return execute_request("GET", s3url, config);
}

S3HttpResult s3_delete_object(const S3HttpConfig& config,
                              const std::string& bucket, const std::string& key) {
    auto s3url = build_object_url(config, bucket, key);
    auto result = execute_request("DELETE", s3url, config);
    // 204 and 404 are both acceptable for delete
    if (result.http_code == 204 || result.http_code == 404) {
        result.ok = true;
    }
    return result;
}

// ============================================================================
// Bucket operations
// ============================================================================

S3HttpResult s3_create_bucket(const S3HttpConfig& config, const std::string& bucket) {
    auto s3url = build_bucket_url(config, bucket);
    auto result = execute_request("PUT", s3url, config);
    // 409 = BucketAlreadyOwnedByYou — treat as success
    if (result.http_code == 409) {
        result.ok = true;
    }
    return result;
}

S3HttpResult s3_head_bucket(const S3HttpConfig& config, const std::string& bucket) {
    auto s3url = build_bucket_url(config, bucket);
    return execute_request("HEAD", s3url, config, "", "", false, true);
}
