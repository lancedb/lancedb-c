/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * AWS SigV4 signing utilities and libcurl helpers for S3 operations.
 * Header-only — shared by s3_lock.cpp and s3_ops.cpp.
 *
 * the use of libcurl instead of aws-sdk-cpp is intentional to minimize dependencies and keep the code lightweight.
 * Dependencies: libcurl, OpenSSL (HMAC-SHA256, SHA256)
 */

#pragma once

#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace sigv4 {

inline std::string hex_encode(const unsigned char* data, size_t len) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++) {
        ss << std::setw(2) << static_cast<int>(data[i]);
    }
    return ss.str();
}

inline std::string sha256_hex(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash);
    return hex_encode(hash, SHA256_DIGEST_LENGTH);
}

inline std::string hmac_sha256_raw(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;
    HMAC(EVP_sha256(),
         key.c_str(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.size(),
         result, &result_len);
    return std::string(reinterpret_cast<char*>(result), result_len);
}

inline std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;
    HMAC(EVP_sha256(),
         key.c_str(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.size(),
         result, &result_len);
    return hex_encode(result, result_len);
}

inline std::string uri_encode_path(const std::string& path) {
    std::ostringstream ss;
    for (char c : path) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            ss << c;
        } else {
            ss << '%' << std::uppercase << std::hex << std::setfill('0')
               << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
        }
    }
    return ss.str();
}

struct TimestampInfo {
    std::string iso8601;    // 20260321T120000Z
    std::string date_stamp; // 20260321
};

inline TimestampInfo get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    gmtime_r(&time, &tm_buf);

    TimestampInfo ts;
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm_buf);
    ts.iso8601 = buf;
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm_buf);
    ts.date_stamp = buf;
    return ts;
}

inline std::string extract_host(const std::string& endpoint) {
    std::string host = endpoint;
    auto pos = host.find("://");
    if (pos != std::string::npos) {
        host = host.substr(pos + 3);
    }
    if (!host.empty() && host.back() == '/') {
        host.pop_back();
    }
    return host;
}

struct SignedRequest {
    std::string authorization;
    std::string x_amz_date;
    std::string x_amz_content_sha256;
};

inline SignedRequest sign_request(
    const std::string& method,
    const std::string& canonical_uri,
    const std::string& host,
    const std::string& payload_hash,
    const std::string& region,
    const std::string& access_key,
    const std::string& secret_key,
    const std::string& extra_headers_canonical = "",
    const std::string& extra_signed_headers = "")
{
    auto ts = get_timestamp();
    std::string service = "s3";
    std::string scope = ts.date_stamp + "/" + region + "/" + service + "/aws4_request";

    std::string canonical_headers = "host:" + host + "\n";
    canonical_headers += "x-amz-content-sha256:" + payload_hash + "\n";
    canonical_headers += "x-amz-date:" + ts.iso8601 + "\n";
    if (!extra_headers_canonical.empty()) {
        canonical_headers += extra_headers_canonical;
    }

    std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
    if (!extra_signed_headers.empty()) {
        signed_headers += ";" + extra_signed_headers;
    }

    std::string canonical_request =
        method + "\n" +
        uri_encode_path(canonical_uri) + "\n" +
        "\n" +
        canonical_headers + "\n" +
        signed_headers + "\n" +
        payload_hash;

    std::string string_to_sign =
        "AWS4-HMAC-SHA256\n" +
        ts.iso8601 + "\n" +
        scope + "\n" +
        sha256_hex(canonical_request);

    std::string date_key = hmac_sha256_raw("AWS4" + secret_key, ts.date_stamp);
    std::string date_region_key = hmac_sha256_raw(date_key, region);
    std::string date_region_service_key = hmac_sha256_raw(date_region_key, service);
    std::string signing_key = hmac_sha256_raw(date_region_service_key, "aws4_request");

    std::string signature = hmac_sha256_hex(signing_key, string_to_sign);

    SignedRequest result;
    result.authorization = "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
                          ", SignedHeaders=" + signed_headers +
                          ", Signature=" + signature;
    result.x_amz_date = ts.iso8601;
    result.x_amz_content_sha256 = payload_hash;
    return result;
}

// libcurl write callback for capturing response body
inline size_t curl_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

} // namespace sigv4
