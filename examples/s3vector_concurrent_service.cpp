/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 *
 * S3 Vector Concurrent Service with File-Locked Index Rebuilding
 * Note: This is a prototype for simulating RGW/Ceph vector index operations.
 *       Limited to single node only.
 *
 * This application provides a concurrent vector service that supports:
 * - Concurrent put-vector, remove-vector, and search-vector operations
 * - Synchronous index building with file-based locking (flock) *NOTE* : the use of flock is for the purpose of this prototype, on s3 storage system it can be achived by s3-conditional-write.
 * - index rebuild triggers checked on each put/remove operation
 * - LanceDB's ability to use old index + brute force on unindexed data
 *
 * Architecture:
 * - PUT/DELETE operations complete first, then check rebuild thresholds
 * - File locking (flock) ensures only one process modifies index state(its very short operation) //NOTE on s3-system it needs to achieve concurrent and fast update of the index state by using s3-conditional-write or other mechanism. it needs to consider a stream of put and remove vectors. one way to achive that is by "lazy" updates of the index state, which means we can allow some level of inconsistency in the index state for a short period of time, and we can update the index state asynchronously in the background. this way we can avoid the contention on the index state update and achieve better performance.
 * - index state is stored in LanceDB table metadata (key: s3v_index_state), it contains the index configuration, build coordination flags, and rebuild thresholds.
 * - If rebuild is needed, the process runs the build synchronously
 * - Search operations are lock-free (use LanceDB manifest for consistency)
 * - Crash detection via builder PID tracking(its for this prototype only)
 *
 * Index Rebuild Triggers (checked on each put/remove):
 * - Insertion threshold: when unindexed vector count exceeds threshold
 * - Deletion ratio: when deleted vectors exceed percentage threshold
 * - Manual trigger via API
 *
 * Concurrency Model:
 * - PUT/DELETE: Execute operation, then flock(LOCK_EX) for state update
 * - Query: No lock needed (LanceDB handles via manifest)
 * - Rebuild: Holds lock only during state updates, not during build
 *
 * Logging:
 * - All operations logged with timestamp, operation type, table name, vector ID
 */

#include <sys/stat.h>
#include <sys/file.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <iomanip>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <nlohmann/json.hpp>
#include "lancedb.h"
#include "base_lock.h"
#include "s3_lock.h"
#include "s3_http.h"

using json = nlohmann::json;

// ============================================================================
// Constants and Configuration
// ============================================================================

const std::string S3_VECTORS_ROOT = "/tmp/s3vectors";
const std::string METADATA_DIR_NAME = ".s3v_metadata";
const std::string INDEX_LOCK_SUFFIX = "_index.lock";
const std::string LOG_FILE_NAME = "operations.log"; //contain timestamped, operation, on what table and other details
const std::string TABLE_METADATA_STATE_KEY = "s3v_index_state"; // key used in LanceDB table metadata

// Default index rebuild thresholds
// Note: IVF_PQ requires minimum 256 vectors to build, so threshold should be >= 256
const size_t DEFAULT_UNINDEXED_THRESHOLD = 256;     // Rebuild when this many new vectors (min 256 for IVF_PQ)
const double DEFAULT_DELETION_RATIO = 0.20;          // Rebuild when 20% deleted

// Local directory for lock files and logs (used regardless of backend)
const std::string LOCAL_STATE_ROOT = "/tmp/s3vectors";

// ============================================================================
// Backend Configuration - Supports local filesystem or S3
// ============================================================================

enum class BackendType { LOCAL, S3 };

struct S3StorageConfig {
    std::string endpoint;           // S3 endpoint URL (e.g. "http://localhost:8000")
    std::string region;             // AWS region (e.g. "us-east-1")
    std::string access_key_id;      // AWS access key
    std::string secret_access_key;  // AWS secret key
    std::string addressing_style;   // "path" for Ceph/RGW, empty for virtual-hosted
    bool allow_http = false;        // Allow HTTP (non-HTTPS) endpoints
};

class BackendConfig {
public:
    static BackendConfig& instance() {
        static BackendConfig config;
        return config;
    }

    BackendType type = BackendType::LOCAL;
    S3StorageConfig s3;

    bool is_s3() const { return type == BackendType::S3; }
    bool is_local() const { return type == BackendType::LOCAL; }

    // Initialize from environment variables:
    //   S3V_BACKEND=s3         (enables S3 mode)
    //   S3V_ENDPOINT           (S3 endpoint URL)
    //   S3V_REGION             (AWS region)
    //   S3V_ACCESS_KEY_ID      (AWS access key)
    //   S3V_SECRET_ACCESS_KEY  (AWS secret key)
    //   S3V_ALLOW_HTTP=true    (allow HTTP endpoints)
    //   S3V_ADDRESSING_STYLE   (e.g. "path" for Ceph/RGW)
    void init_from_env() {
        auto get_env = [](const char* name) -> std::string {
            const char* val = std::getenv(name);
            return val ? val : "";
        };

        std::string backend = get_env("S3V_BACKEND");
        if (backend == "s3" || backend == "S3") {
            type = BackendType::S3;
            s3.endpoint = get_env("S3V_ENDPOINT");
            s3.region = get_env("S3V_REGION");
            s3.access_key_id = get_env("S3V_ACCESS_KEY_ID");
            s3.secret_access_key = get_env("S3V_SECRET_ACCESS_KEY");
            s3.addressing_style = get_env("S3V_ADDRESSING_STYLE");
            s3.allow_http = (get_env("S3V_ALLOW_HTTP") == "true");
        }
    }

    std::string describe() const {
        if (is_local()) return "local (" + S3_VECTORS_ROOT + ")";
        return "s3 (endpoint=" + s3.endpoint + ")";
    }
};

// ============================================================================
// Logging
// ============================================================================

class Logger {
public:
    enum Level { DEBUG, INFO, WARN, ERROR };

private:
    std::mutex mutex_;
    std::ofstream log_file_;
    Level min_level_ = INFO;
    bool console_output_ = true;

    std::string level_to_string(Level level) {
        switch (level) {
            case DEBUG: return "DEBUG";
            case INFO:  return "INFO";
            case WARN:  return "WARN";
            case ERROR: return "ERROR";
            default:    return "UNKNOWN";
        }
    }

    std::string get_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3) << ms.count() << "Z";
        return ss.str();
    }

public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void init(const std::string& log_path, Level level = INFO, bool console = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_file_.open(log_path, std::ios::app);
        min_level_ = level;
        console_output_ = console;
    }

    void log(Level level, const std::string& operation, const std::string& table_name,
             const std::string& vector_id, const std::string& message) {
        if (level < min_level_) return;

        std::lock_guard<std::mutex> lock(mutex_);

        std::stringstream ss;
        ss << get_timestamp() << " [" << level_to_string(level) << "] "
           << "op=" << operation;
        if (!table_name.empty()) ss << " table=" << table_name;
        if (!vector_id.empty()) ss << " vector_id=" << vector_id;
        ss << " " << message;

        std::string log_line = ss.str();

        if (console_output_) {
            std::cout << log_line << std::endl;
        }

        if (log_file_.is_open()) {
            log_file_ << log_line << std::endl;
            log_file_.flush();
        }
    }

    void log(Level level, const std::string& operation, const std::string& table_name,
             const std::string& message) {
        log(level, operation, table_name, "", message);
    }

    void log(Level level, const std::string& message) {
        log(level, "", "", "", message);
    }
};

#define LOG_DEBUG(op, table, msg) Logger::instance().log(Logger::DEBUG, op, table, msg)
#define LOG_INFO(op, table, msg) Logger::instance().log(Logger::INFO, op, table, msg)
#define LOG_WARN(op, table, msg) Logger::instance().log(Logger::WARN, op, table, msg)
#define LOG_ERROR(op, table, msg) Logger::instance().log(Logger::ERROR, op, table, msg)
#define LOG_OP(op, table, vid, msg) Logger::instance().log(Logger::INFO, op, table, vid, msg)

// ============================================================================
// Scalar Field Definition - user-defined columns beyond key/data/metadata
// ============================================================================

struct ScalarFieldDef {
    //basic definition of a scalar field (a column in a table), it is used to define the user-defined columns in the index, which can be used for filtering and other purposes. 
    //it is defined in the index configuration and stored in the index state.
    std::string name;
    std::string type;  // "string", "int", "float", "bool"

    json to_json() const {
        return {{"name", name}, {"type", type}};
    }

    static ScalarFieldDef from_json(const json& j) {
        ScalarFieldDef f;
        f.name = j.value("name", "");
        f.type = j.value("type", "string");
        return f;
    }

    std::shared_ptr<arrow::DataType> to_arrow_type() const {
        if (type == "int")    return arrow::int64();
        if (type == "float")  return arrow::float64();
        if (type == "bool")   return arrow::boolean();
        return arrow::utf8();  // default: string
    }
};

// ============================================================================
// Index Configuration
// ============================================================================

struct IndexConfig {
    std::string index_type = "IVF_PQ";  // IVF_FLAT, IVF_PQ, IVF_HNSW_PQ, IVF_HNSW_SQ
    std::string distance_metric = "euclidean";  // euclidean, cosine
    int num_partitions = -1;      // -1 = auto
    int num_sub_vectors = -1;     // -1 = auto
    int max_iterations = -1;      // -1 = default
    float sample_rate = 0.0f;     // 0.0 = default
    std::string accelerator = ""; // "cuda", "mps", or empty for CPU

    // Query parameters (used at search time)
    int nprobes = -1;             // -1 = default
    int refine_factor = -1;       // -1 = default
    int ef = -1;                  // -1 = default (for HNSW)

    // User-defined scalar columns (beyond fixed key/data/metadata)
    // this sclar schema is defined at index creation time, and it is stored in the index state. 
    // it is used to create the corresponding columns in the LanceDB table.
    std::vector<ScalarFieldDef> scalar_schema;

    json to_json() const {
        json j = {
            {"indexType", index_type},
            {"distanceMetric", distance_metric},
            {"numPartitions", num_partitions},
            {"numSubVectors", num_sub_vectors},
            {"maxIterations", max_iterations},
            {"sampleRate", sample_rate},
            {"accelerator", accelerator},
            {"nprobes", nprobes},
            {"refineFactor", refine_factor},
            {"ef", ef}
        };
        json schema_arr = json::array();
        for (const auto& f : scalar_schema) {
            schema_arr.push_back(f.to_json());
        }
        j["scalarSchema"] = schema_arr;
        return j;
    }

    static IndexConfig from_json(const json& j) {
        IndexConfig config;
        if (j.contains("indexType")) config.index_type = j["indexType"];
        if (j.contains("distanceMetric")) config.distance_metric = j["distanceMetric"];
        if (j.contains("numPartitions")) config.num_partitions = j["numPartitions"];
        if (j.contains("numSubVectors")) config.num_sub_vectors = j["numSubVectors"];
        if (j.contains("maxIterations")) config.max_iterations = j["maxIterations"];
        if (j.contains("sampleRate")) config.sample_rate = j["sampleRate"];
        if (j.contains("accelerator")) config.accelerator = j["accelerator"];
        if (j.contains("nprobes")) config.nprobes = j["nprobes"];
        if (j.contains("refineFactor")) config.refine_factor = j["refineFactor"];
        if (j.contains("ef")) config.ef = j["ef"];
        if (j.contains("scalarSchema") && j["scalarSchema"].is_array()) {
            for (const auto& item : j["scalarSchema"]) {
                config.scalar_schema.push_back(ScalarFieldDef::from_json(item));
            }
        }
        return config;
    }

    LanceDBIndexType to_lancedb_type() const {
        if (index_type == "IVF_FLAT") return LANCEDB_INDEX_IVF_FLAT;
        if (index_type == "IVF_PQ") return LANCEDB_INDEX_IVF_PQ;
        if (index_type == "IVF_HNSW_PQ") return LANCEDB_INDEX_IVF_HNSW_PQ;
        if (index_type == "IVF_HNSW_SQ") return LANCEDB_INDEX_IVF_HNSW_SQ;
        return LANCEDB_INDEX_IVF_PQ;  // default
    }

    LanceDBDistanceType to_lancedb_distance() const {
        if (distance_metric == "cosine") return LANCEDB_DISTANCE_COSINE;
        if (distance_metric == "dot") return LANCEDB_DISTANCE_DOT;
        return LANCEDB_DISTANCE_L2;  // default (euclidean)
    }
};

// ============================================================================
// Table Index State - Tracks changes since last index build
// ============================================================================

class TableIndexState {
    // Index configuration and rebuild coordination.
    // Index statistics (indexed/unindexed row counts) are queried live from LanceDB
    // via lancedb_table_index_stats() — no external counters needed.
    // Only build-coordination flags (in_progress, builder_pid) are persisted.
public:
    std::string bucket_name;
    std::string index_name;

    // Index configuration
    IndexConfig config;
    int dimension = 0;

    // Build coordination (persisted for crash detection across processes)
    bool index_build_in_progress = false;
    pid_t builder_pid = 0;

    // Thresholds for auto-rebuild
    size_t unindexed_threshold = DEFAULT_UNINDEXED_THRESHOLD;
    double deletion_ratio_threshold = DEFAULT_DELETION_RATIO;

    // Mutex for in-process thread safety (not cross-process - use flock for that)
    mutable std::mutex mutex;

    bool is_builder_alive() const {
        if (!index_build_in_progress || builder_pid == 0) {
            return false;
        }
        if (kill(builder_pid, 0) == -1) {
            if (errno == ESRCH) return false;
        }
        return true;
    }

    void reset_crashed_builder() {
        if (index_build_in_progress && !is_builder_alive()) {
            LOG_WARN("INDEX_STATE", index_name,
                     "Detected crashed builder (PID " + std::to_string(builder_pid) + "), resetting state");
            index_build_in_progress = false;
            builder_pid = 0;
        }
    }

    void mark_build_started() {
        index_build_in_progress = true;
        builder_pid = getpid();
    }

    void mark_build_complete() {
        index_build_in_progress = false;
        builder_pid = 0;
    }

    json to_json() const {
        std::lock_guard<std::mutex> lock(mutex);
        return {
            {"bucketName", bucket_name},
            {"indexName", index_name},
            {"dimension", dimension},
            {"config", config.to_json()},
            {"indexBuildInProgress", index_build_in_progress},
            {"builderPid", static_cast<int>(builder_pid)},
            {"unindexedThreshold", unindexed_threshold},
            {"deletionRatioThreshold", deletion_ratio_threshold}
        };
    }

    void load_from_json(const json& j) {
        std::lock_guard<std::mutex> lock(mutex);
        if (j.contains("bucketName")) bucket_name = j["bucketName"];
        if (j.contains("indexName")) index_name = j["indexName"];
        if (j.contains("dimension")) dimension = j["dimension"];
        if (j.contains("config")) config = IndexConfig::from_json(j["config"]);
        if (j.contains("indexBuildInProgress")) index_build_in_progress = j["indexBuildInProgress"].get<bool>();
        if (j.contains("builderPid")) builder_pid = static_cast<pid_t>(j["builderPid"].get<int>());
        if (j.contains("unindexedThreshold")) unindexed_threshold = j["unindexedThreshold"];
        if (j.contains("deletionRatioThreshold")) deletion_ratio_threshold = j["deletionRatioThreshold"];
    }
};

// ============================================================================
// Utility Functions
// ============================================================================

namespace utils {
//for the purpose of this prototype(it is not relevant to s3 system)
bool directory_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool create_directory(const std::string& path) {
    return mkdir(path.c_str(), 0755) == 0;
}

bool create_directories(const std::string& path) {
    std::string current;
    std::istringstream stream(path);
    std::string segment;

    while (std::getline(stream, segment, '/')) {
        if (segment.empty()) {
            current = "/";
            continue;
        }
        current += segment + "/";
        if (!directory_exists(current)) {
            if (!create_directory(current)) {
                return false;
            }
        }
    }
    return true;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return file.good();
}

std::string get_bucket_path(const std::string& bucket_name) {
    if (BackendConfig::instance().is_s3()) {
        // vectorBucketName IS the S3 bucket — 1:1 mapping with S3 Vectors API
        return "s3://" + bucket_name;
    }
    return S3_VECTORS_ROOT + "/" + bucket_name;
}

std::string get_metadata_path(const std::string& bucket_name) {
    // Metadata path is always local (used for lock files only)
    return LOCAL_STATE_ROOT + "/" + bucket_name + "/" + METADATA_DIR_NAME;
}

std::string get_index_db_path(const std::string& bucket_name, const std::string& index_name) {
    return get_bucket_path(bucket_name) + "/" + index_name + "_lancedb";
}

std::string get_index_lock_path(const std::string& bucket_name, const std::string& index_name) {
    return get_metadata_path(bucket_name) + "/" + index_name + INDEX_LOCK_SUFFIX;
}

} // namespace utils

// ============================================================================
// File Lock RAII Class - Uses flock for cross-process synchronization
// Base_lock interface is defined in base_lock.h
// S3Lock implementation is in s3_lock.h / s3_lock.cpp
// ============================================================================

class FileLock : public Base_lock {
//NOTE: it is for the purpose of this prototype, no need for that in S3.
private:
    int fd_ = -1;
    bool locked_ = false;
    std::string path_;
    const std::string defult_lock_path_ = "/tmp/s3vectors/default_index.lock";
public:
    FileLock() = default;

    explicit FileLock(const std::string& path) : path_(path) {
        // Create parent directory if needed
	if(path.empty()){
	    path_ = defult_lock_path_;
	}
        size_t last_slash = path.rfind('/');
        if (last_slash != std::string::npos) {
            std::string dir = path.substr(0, last_slash);
            utils::create_directories(dir);
        }

        // Open or create the lock file
        fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) {
            LOG_ERROR("FILE_LOCK", path, "Failed to open lock file: " + std::string(strerror(errno)));
        }
    }

    ~FileLock() {
        unlock();
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    // Non-copyable
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    // Movable
    FileLock(FileLock&& other) noexcept
        : fd_(other.fd_), locked_(other.locked_), path_(std::move(other.path_)) {
        other.fd_ = -1;
        other.locked_ = false;
    }

    FileLock& operator=(FileLock&& other) noexcept {
        if (this != &other) {
            unlock();
            if (fd_ >= 0) close(fd_);
            fd_ = other.fd_;
            locked_ = other.locked_;
            path_ = std::move(other.path_);
            other.fd_ = -1;
            other.locked_ = false;
        }
        return *this;
    }

    bool lock_exclusive() {
        if (fd_ < 0) return false;
        if (locked_) return true;

        if (flock(fd_, LOCK_EX) == 0) {
            locked_ = true;
            return true;
        }
        LOG_ERROR("FILE_LOCK", path_, "Failed to acquire exclusive lock: " + std::string(strerror(errno)));
        return false;
    }

    bool try_lock_exclusive() {
        if (fd_ < 0) return false;
        if (locked_) return true;

        if (flock(fd_, LOCK_EX | LOCK_NB) == 0) {
            locked_ = true;
            return true;
        }
        // EWOULDBLOCK means lock is held by another process
        return false;
    }

    void unlock() {
        if (fd_ >= 0 && locked_) {
            flock(fd_, LOCK_UN);
            locked_ = false;
        }
    }

    bool is_locked() const { return locked_; }
    bool is_valid() const { return fd_ >= 0; }
};

// FrontendLocker selects the lock implementation based on the backend:
// - LOCAL: FileLock (flock-based, for testing/debugging)
// - S3:   S3Lock (distributed lock using S3 conditional writes)
class FrontendLocker {
private:
    std::unique_ptr<Base_lock> lock_;
public:
    FrontendLocker(const std::string& bucket_name, const std::string& index_name) {
	if (BackendConfig::instance().is_s3()) {
	    const auto& cfg = BackendConfig::instance();
	    S3LockConfig s3cfg;
	    s3cfg.endpoint = cfg.s3.endpoint;
	    s3cfg.region = cfg.s3.region;
	    s3cfg.access_key_id = cfg.s3.access_key_id;
	    s3cfg.secret_access_key = cfg.s3.secret_access_key;
	    s3cfg.bucket = bucket_name;  // lock object lives in the vector bucket
	    s3cfg.lock_key = ".locks/" + index_name + INDEX_LOCK_SUFFIX;
	    s3cfg.use_path_style = !cfg.s3.addressing_style.empty();
	    s3cfg.allow_http = cfg.s3.allow_http;
	    lock_ = std::make_unique<S3Lock>(s3cfg);
	} else {
	    std::string lock_path = utils::get_index_lock_path(bucket_name, index_name);
	    lock_ = std::make_unique<FileLock>(lock_path);
	}
    }

    bool lock_exclusive() {
	return lock_->lock_exclusive();
    }

    bool try_lock_exclusive() {
	return lock_->try_lock_exclusive();
    }

    void unlock() {
	lock_->unlock();
    }

    bool is_locked() const {
	return lock_->is_locked();
    }

    bool is_valid() const {
	return lock_->is_valid();
    }
};

// ============================================================================
// Response Types
// ============================================================================

struct ApiResponse {
    int status_code = 200;
    json body;
    std::string error_type;
    std::string error_message;

    bool is_success() const { return status_code >= 200 && status_code < 300; }

    std::string to_string() const {
        if (is_success()) {
            return body.dump(2);
        } else {
            json error_response = {
                {"error", {
                    {"type", error_type},
                    {"message", error_message}
                }}
            };
            return error_response.dump(2);
        }
    }
};

ApiResponse make_error(int code, const std::string& type, const std::string& message) {
    ApiResponse resp;
    resp.status_code = code;
    resp.error_type = type;
    resp.error_message = message;
    return resp;
}

ApiResponse make_success(const json& body = json::object()) {
    ApiResponse resp;
    resp.status_code = 200;
    resp.body = body;
    return resp;
}

// ============================================================================
// LanceDB Helper Class
// ============================================================================

class LanceDBHelper {
public:
    static std::shared_ptr<arrow::Schema> create_vector_schema(
            int dimension, const std::vector<ScalarFieldDef>& scalar_schema = {}) {
        std::vector<std::shared_ptr<arrow::Field>> fields;
        // Fixed columns
        fields.push_back(arrow::field("key", arrow::utf8()));
        fields.push_back(arrow::field("data", arrow::fixed_size_list(arrow::float32(), dimension)));
        fields.push_back(arrow::field("metadata", arrow::utf8()));
        // Dynamic scalar columns from user-defined schema
        for (const auto& sf : scalar_schema) {
            fields.push_back(arrow::field(sf.name, sf.to_arrow_type()));
        }
        return arrow::schema(fields);
    }

    static LanceDBConnection* connect(const std::string& db_path) {
        LanceDBConnectBuilder* builder = lancedb_connect(db_path.c_str());
        if (!builder) {
            return nullptr;
        }

        // Add S3 storage options when running with S3 backend
        const auto& cfg = BackendConfig::instance();
        if (cfg.is_s3()) {
            const auto& s3 = cfg.s3;
            if (!s3.endpoint.empty())
                builder = lancedb_connect_builder_storage_option(builder, "endpoint", s3.endpoint.c_str());
            if (!s3.region.empty())
                builder = lancedb_connect_builder_storage_option(builder, "aws_region", s3.region.c_str());
            if (!s3.access_key_id.empty())
                builder = lancedb_connect_builder_storage_option(builder, "aws_access_key_id", s3.access_key_id.c_str());
            if (!s3.secret_access_key.empty())
                builder = lancedb_connect_builder_storage_option(builder, "aws_secret_access_key", s3.secret_access_key.c_str());
            if (s3.allow_http)
                builder = lancedb_connect_builder_storage_option(builder, "allow_http", "true");
            if (!s3.addressing_style.empty())
                builder = lancedb_connect_builder_storage_option(builder, "aws_s3_addressing_style", s3.addressing_style.c_str());
            if (!builder) {
                return nullptr;
            }
        }

        LanceDBConnection* conn = lancedb_connect_builder_execute(builder);
        return conn;
    }

    static LanceDBTable* create_table(LanceDBConnection* conn, const std::string& table_name,
                                       int dimension,
                                       const std::vector<ScalarFieldDef>& scalar_schema,
                                       char** error_msg) {
	 //the scalar schema includes the fixed columns (key, data, metadata) and the user-defined scalar columns. 
	 //it is used to create the corresponding columns in the LanceDB table.
        auto schema = create_vector_schema(dimension, scalar_schema);
        struct ArrowSchema c_schema;
        if (!arrow::ExportSchema(*schema, &c_schema).ok()) {
            return nullptr;
        }

        LanceDBTable* table = nullptr;
        LanceDBError result = lancedb_table_create(
            conn, table_name.c_str(),
            reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
            nullptr, &table, error_msg);

        if (c_schema.release) {
            c_schema.release(&c_schema);
        }

        if (result != LANCEDB_SUCCESS) {
            return nullptr;
        }
        return table;
    }

    static LanceDBTable* open_table(LanceDBConnection* conn, const std::string& table_name) {
        return lancedb_connection_open_table(conn, table_name.c_str());
    }

    static bool add_vectors(LanceDBTable* table,
                           const std::vector<std::string>& keys,
                           const std::vector<std::vector<float>>& vectors,
                           const std::vector<std::string>& metadata_list,
                           const std::vector<ScalarFieldDef>& scalar_schema,
                           const std::map<std::string, std::vector<json>>& scalar_values,
                           int dimension,
                           std::string& error) {
        if (keys.empty()) {
            error = "No vectors to add";
            return false;
        }

	 //the scalar schema includes the fixed columns (key, data, metadata) and the user-defined scalar columns. 
	 //it is used to create the corresponding columns in the LanceDB table.
        auto schema = create_vector_schema(dimension, scalar_schema);

        // Fixed builders
        arrow::StringBuilder key_builder;
        arrow::FixedSizeListBuilder data_builder(
            arrow::default_memory_pool(),
            std::make_unique<arrow::FloatBuilder>(),
            dimension);
        arrow::StringBuilder metadata_builder;

        // Dynamic builders - one per scalar field - searchable columns defined by user in index configuration
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> scalar_builders;
        for (const auto& sf : scalar_schema) {
            if (sf.type == "int") {
                scalar_builders.push_back(std::make_unique<arrow::Int64Builder>());
            } else if (sf.type == "float") {
                scalar_builders.push_back(std::make_unique<arrow::DoubleBuilder>());
            } else if (sf.type == "bool") {
                scalar_builders.push_back(std::make_unique<arrow::BooleanBuilder>());
            } else {
                scalar_builders.push_back(std::make_unique<arrow::StringBuilder>());
            }
        }

        for (size_t i = 0; i < keys.size(); i++) {
            if (!key_builder.Append(keys[i]).ok()) {
                error = "Failed to append key";
                return false;
            }

            auto* float_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
            for (int j = 0; j < dimension; j++) {
                if (!float_builder->Append(vectors[i][j]).ok()) {
                    error = "Failed to append vector value";
                    return false;
                }
            }
            if (!data_builder.Append().ok()) {
                error = "Failed to append vector";
                return false;
            }

            if (!metadata_builder.Append(metadata_list[i]).ok()) {
                error = "Failed to append metadata";
                return false;
            }

            // Append dynamic scalar values
            for (size_t col = 0; col < scalar_schema.size(); col++) {
                const auto& sf = scalar_schema[col];
                auto it = scalar_values.find(sf.name);
                bool has_value = (it != scalar_values.end() && i < it->second.size()
                                  && !it->second[i].is_null());

                if (sf.type == "string") {
                    auto* sb = static_cast<arrow::StringBuilder*>(scalar_builders[col].get());
                    if (has_value && it->second[i].is_string()) {
                        if (!sb->Append(it->second[i].get<std::string>()).ok()) {
                            error = "Failed to append " + sf.name;
                            return false;
                        }
                    } else {
                        if (!sb->AppendNull().ok()) {
                            error = "Failed to append null " + sf.name;
                            return false;
                        }
                    }
                } else if (sf.type == "int") {
                    auto* ib = static_cast<arrow::Int64Builder*>(scalar_builders[col].get());
                    if (has_value && it->second[i].is_number_integer()) {
                        if (!ib->Append(it->second[i].get<int64_t>()).ok()) {
                            error = "Failed to append " + sf.name;
                            return false;
                        }
                    } else {
                        if (!ib->AppendNull().ok()) {
                            error = "Failed to append null " + sf.name;
                            return false;
                        }
                    }
                } else if (sf.type == "float") {
                    auto* fb = static_cast<arrow::DoubleBuilder*>(scalar_builders[col].get());
                    if (has_value && it->second[i].is_number()) {
                        if (!fb->Append(it->second[i].get<double>()).ok()) {
                            error = "Failed to append " + sf.name;
                            return false;
                        }
                    } else {
                        if (!fb->AppendNull().ok()) {
                            error = "Failed to append null " + sf.name;
                            return false;
                        }
                    }
                } else if (sf.type == "bool") {
                    auto* bb = static_cast<arrow::BooleanBuilder*>(scalar_builders[col].get());
                    if (has_value && it->second[i].is_boolean()) {
                        if (!bb->Append(it->second[i].get<bool>()).ok()) {
                            error = "Failed to append " + sf.name;
                            return false;
                        }
                    } else {
                        if (!bb->AppendNull().ok()) {
                            error = "Failed to append null " + sf.name;
                            return false;
                        }
                    }
                }
            }
        }

        // Finish fixed arrays
        std::shared_ptr<arrow::Array> key_array, data_array, metadata_array;
        (void)key_builder.Finish(&key_array);
        (void)data_builder.Finish(&data_array);
        (void)metadata_builder.Finish(&metadata_array);

        // Build dynamic column arrays
        std::vector<std::shared_ptr<arrow::Array>> all_arrays = {
            key_array, data_array, metadata_array
        };
        for (auto& builder : scalar_builders) {
            std::shared_ptr<arrow::Array> arr;
            (void)builder->Finish(&arr);
            all_arrays.push_back(arr);
        }

        auto record_batch = arrow::RecordBatch::Make(schema, keys.size(), all_arrays);

        struct ArrowSchema c_schema;
        struct ArrowArray c_array;
        if (!arrow::ExportRecordBatch(*record_batch, &c_array, &c_schema).ok()) {
            error = "Failed to export record batch";
            return false;
        }

        LanceDBRecordBatchReader* reader = nullptr;
        char* err_msg = nullptr;
        LanceDBError reader_result = lancedb_record_batch_reader_from_arrow(
            reinterpret_cast<FFI_ArrowArray*>(&c_array),
            reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
            &reader,
            &err_msg);

        if (reader_result != LANCEDB_SUCCESS || !reader) {
            if (c_schema.release) c_schema.release(&c_schema);
            if (c_array.release) c_array.release(&c_array);
            error = err_msg ? err_msg : "Failed to create record batch reader";
            if (err_msg) lancedb_free_string(err_msg);
            return false;
        }

        err_msg = nullptr;
        LanceDBError result = lancedb_table_add(table, reader, &err_msg);

        if (c_schema.release) c_schema.release(&c_schema);

        if (result != LANCEDB_SUCCESS) {
            error = err_msg ? err_msg : lancedb_error_to_message(result);
            if (err_msg) lancedb_free_string(err_msg);
            return false;
        }

        return true;
    }

    struct QueryResult {
        std::string key;
        std::vector<float> data;
        std::string metadata;
        float distance = 0.0f;
        std::map<std::string, json> scalar_fields;  // dynamic columns
    };

    static std::vector<QueryResult> query_vectors(
        LanceDBTable* table,
        const std::vector<float>& query_vector,
        int top_k,
        const std::string& filter,
        bool return_distance,
        bool return_metadata,
        LanceDBDistanceType distance_type,
        const IndexConfig& config,
        bool explain_plan,
        std::string& explain_plan_output,
        std::string& error) {

        std::vector<QueryResult> results;

        LanceDBVectorQuery* query = lancedb_vector_query_new(
            table, query_vector.data(), query_vector.size());

        if (!query) {
            error = "Failed to create vector query";
            return results;
        }

        if (lancedb_vector_query_limit(query, top_k, nullptr) != LANCEDB_SUCCESS) {
            lancedb_vector_query_free(query);
            error = "Failed to set query limit";
            return results;
        }

        if (lancedb_vector_query_column(query, "data", nullptr) != LANCEDB_SUCCESS) {
            lancedb_vector_query_free(query);
            error = "Failed to set query column";
            return results;
        }

        if (lancedb_vector_query_distance_type(query, distance_type, nullptr) != LANCEDB_SUCCESS) {
            lancedb_vector_query_free(query);
            error = "Failed to set distance type";
            return results;
        }

        // Apply index-specific query parameters
        if (config.nprobes > 0) {
            lancedb_vector_query_nprobes(query, config.nprobes, nullptr);
        }
        if (config.refine_factor > 0) {
            lancedb_vector_query_refine_factor(query, config.refine_factor, nullptr);
        }
        if (config.ef > 0 && (config.index_type == "IVF_HNSW_PQ" || config.index_type == "IVF_HNSW_SQ")) {
            lancedb_vector_query_ef(query, config.ef, nullptr);
        }

        if (!filter.empty()) {
            if (lancedb_vector_query_where_filter(query, filter.c_str(), nullptr) != LANCEDB_SUCCESS) {
                lancedb_vector_query_free(query);
                error = "Failed to set query filter";
                return results;
            }
        }

        // Get the execution plan before executing (does not consume the query)
        if (explain_plan) {
            char* plan = nullptr;
            char* err_msg = nullptr;
            if (lancedb_vector_query_explain_plan(query, true, &plan, &err_msg) == LANCEDB_SUCCESS && plan) {
                explain_plan_output = plan;
                lancedb_free_string(plan);
            } else {
                explain_plan_output = err_msg ? std::string("explain_plan failed: ") + err_msg
                                              : "explain_plan failed";
                if (err_msg) lancedb_free_string(err_msg);
            }
        }

        LanceDBQueryResult* query_result = lancedb_vector_query_execute(query);
        if (!query_result) {
            error = "Failed to execute query";
            return results;
        }

        struct ArrowArray** c_arrays = nullptr;
        struct ArrowSchema* c_schema = nullptr;
        size_t count = 0;

        if (lancedb_query_result_to_arrow(
                query_result,
                reinterpret_cast<FFI_ArrowArray***>(&c_arrays),
                reinterpret_cast<FFI_ArrowSchema**>(&c_schema),
                &count, nullptr) != LANCEDB_SUCCESS) {
            lancedb_query_result_free(query_result);
            error = "Failed to convert query result to arrow";
            return results;
        }

        if (count > 0 && c_arrays && c_schema) {
            auto schema_result = arrow::ImportSchema(c_schema);
            if (schema_result.ok()) {
                auto schema = *schema_result;
                auto batch_result = arrow::ImportRecordBatch(
                    reinterpret_cast<struct ArrowArray*>(*c_arrays), schema);

                if (batch_result.ok()) {
                    auto batch = *batch_result;

                    int key_idx = schema->GetFieldIndex("key");
                    int data_idx = schema->GetFieldIndex("data");
                    int metadata_idx = schema->GetFieldIndex("metadata");
                    int distance_idx = schema->GetFieldIndex("_distance");

                    for (int64_t row = 0; row < batch->num_rows(); row++) {
                        QueryResult result;

                        if (key_idx >= 0) {
                            auto key_array = std::static_pointer_cast<arrow::StringArray>(
                                batch->column(key_idx));
                            if (!key_array->IsNull(row)) {
                                result.key = key_array->GetString(row);
                            }
                        }

                        if (data_idx >= 0) {
                            auto data_array = std::static_pointer_cast<arrow::FixedSizeListArray>(
                                batch->column(data_idx));
                            if (!data_array->IsNull(row)) {
                                auto values = std::static_pointer_cast<arrow::FloatArray>(
                                    data_array->values());
                                int32_t start = data_array->value_offset(row);
                                int32_t length = data_array->value_length();
                                for (int32_t i = 0; i < length; i++) {
                                    result.data.push_back(values->Value(start + i));
                                }
                            }
                        }

                        if (metadata_idx >= 0 && return_metadata) {
                            auto metadata_array = std::static_pointer_cast<arrow::StringArray>(
                                batch->column(metadata_idx));
                            if (!metadata_array->IsNull(row)) {
                                result.metadata = metadata_array->GetString(row);
                            }
                        }

                        if (distance_idx >= 0 && return_distance) {
                            auto distance_array = std::static_pointer_cast<arrow::FloatArray>(
                                batch->column(distance_idx));
                            if (!distance_array->IsNull(row)) {
                                result.distance = distance_array->Value(row);
                            }
                        }

                        // Extract dynamic scalar columns
                        for (const auto& sf : config.scalar_schema) {
                            int col_idx = schema->GetFieldIndex(sf.name);
                            if (col_idx >= 0 && !batch->column(col_idx)->IsNull(row)) {
                                if (sf.type == "string") {
                                    auto arr = std::static_pointer_cast<arrow::StringArray>(
                                        batch->column(col_idx));
                                    result.scalar_fields[sf.name] = arr->GetString(row);
                                } else if (sf.type == "int") {
                                    auto arr = std::static_pointer_cast<arrow::Int64Array>(
                                        batch->column(col_idx));
                                    result.scalar_fields[sf.name] = arr->Value(row);
                                } else if (sf.type == "float") {
                                    auto arr = std::static_pointer_cast<arrow::DoubleArray>(
                                        batch->column(col_idx));
                                    result.scalar_fields[sf.name] = arr->Value(row);
                                } else if (sf.type == "bool") {
                                    auto arr = std::static_pointer_cast<arrow::BooleanArray>(
                                        batch->column(col_idx));
                                    result.scalar_fields[sf.name] = arr->Value(row);
                                }
                            }
                        }

                        results.push_back(result);
                    }
                }
            }
        }

        if (c_arrays) lancedb_free_arrow_arrays(reinterpret_cast<FFI_ArrowArray**>(c_arrays), count);
        if (c_schema) lancedb_free_arrow_schema(reinterpret_cast<FFI_ArrowSchema*>(c_schema));

        return results;
    }

    // Filter-only query — no vector similarity search, just scalar filters
    static std::vector<QueryResult> filter_only_query(
        LanceDBTable* table,
        int limit,
        const std::string& filter,
        bool return_metadata,
        const IndexConfig& config,
        bool explain_plan,
        std::string& explain_plan_output,
        std::string& error) {

        std::vector<QueryResult> results;

        if (filter.empty()) {
            error = "filterOnly mode requires a filter";
            return results;
        }

        LanceDBQuery* query = lancedb_query_new(table);
        if (!query) {
            error = "Failed to create query";
            return results;
        }

        if (lancedb_query_limit(query, limit, nullptr) != LANCEDB_SUCCESS) {
            lancedb_query_free(query);
            error = "Failed to set query limit";
            return results;
        }

        if (lancedb_query_where_filter(query, filter.c_str(), nullptr) != LANCEDB_SUCCESS) {
            lancedb_query_free(query);
            error = "Failed to set query filter";
            return results;
        }

        // Get the execution plan before executing (does not consume the query)
        if (explain_plan) {
            char* plan = nullptr;
            char* err_msg = nullptr;
            if (lancedb_query_explain_plan(query, true, &plan, &err_msg) == LANCEDB_SUCCESS && plan) {
                explain_plan_output = plan;
                lancedb_free_string(plan);
            } else {
                explain_plan_output = err_msg ? std::string("explain_plan failed: ") + err_msg
                                              : "explain_plan failed";
                if (err_msg) lancedb_free_string(err_msg);
            }
        }

        LanceDBQueryResult* query_result = lancedb_query_execute(query);
        if (!query_result) {
            error = "Failed to execute filter-only query";
            return results;
        }

        struct ArrowArray** c_arrays = nullptr;
        struct ArrowSchema* c_schema = nullptr;
        size_t count = 0;

        if (lancedb_query_result_to_arrow(
                query_result,
                reinterpret_cast<FFI_ArrowArray***>(&c_arrays),
                reinterpret_cast<FFI_ArrowSchema**>(&c_schema),
                &count, nullptr) != LANCEDB_SUCCESS) {
            lancedb_query_result_free(query_result);
            error = "Failed to convert query result to arrow";
            return results;
        }

        if (count > 0 && c_arrays && c_schema) {
            auto schema_result = arrow::ImportSchema(c_schema);
            if (schema_result.ok()) {
                auto schema = *schema_result;
                auto batch_result = arrow::ImportRecordBatch(
                    reinterpret_cast<struct ArrowArray*>(*c_arrays), schema);

                if (batch_result.ok()) {
                    auto batch = *batch_result;

                    int key_idx = schema->GetFieldIndex("key");
                    int metadata_idx = schema->GetFieldIndex("metadata");

                    for (int64_t row = 0; row < batch->num_rows(); row++) {
                        QueryResult result;

                        if (key_idx >= 0) {
                            auto key_array = std::static_pointer_cast<arrow::StringArray>(
                                batch->column(key_idx));
                            if (!key_array->IsNull(row)) {
                                result.key = key_array->GetString(row);
                            }
                        }

                        if (metadata_idx >= 0 && return_metadata) {
                            auto metadata_array = std::static_pointer_cast<arrow::StringArray>(
                                batch->column(metadata_idx));
                            if (!metadata_array->IsNull(row)) {
                                result.metadata = metadata_array->GetString(row);
                            }
                        }

                        // Extract dynamic scalar columns
                        for (const auto& sf : config.scalar_schema) {
                            int col_idx = schema->GetFieldIndex(sf.name);
                            if (col_idx >= 0 && !batch->column(col_idx)->IsNull(row)) {
                                if (sf.type == "string") {
                                    auto arr = std::static_pointer_cast<arrow::StringArray>(
                                        batch->column(col_idx));
                                    result.scalar_fields[sf.name] = arr->GetString(row);
                                } else if (sf.type == "int") {
                                    auto arr = std::static_pointer_cast<arrow::Int64Array>(
                                        batch->column(col_idx));
                                    result.scalar_fields[sf.name] = arr->Value(row);
                                } else if (sf.type == "float") {
                                    auto arr = std::static_pointer_cast<arrow::DoubleArray>(
                                        batch->column(col_idx));
                                    result.scalar_fields[sf.name] = arr->Value(row);
                                } else if (sf.type == "bool") {
                                    auto arr = std::static_pointer_cast<arrow::BooleanArray>(
                                        batch->column(col_idx));
                                    result.scalar_fields[sf.name] = arr->Value(row);
                                }
                            }
                        }

                        results.push_back(result);
                    }
                }
            }
        }

        if (c_arrays) lancedb_free_arrow_arrays(reinterpret_cast<FFI_ArrowArray**>(c_arrays), count);
        if (c_schema) lancedb_free_arrow_schema(reinterpret_cast<FFI_ArrowSchema*>(c_schema));

        return results;
    }

    // Get the first index name from the table (LanceDB auto-generates names like "data_idx")
    static std::string get_first_index_name(LanceDBTable* table) {
        char** indices = nullptr;
        size_t count = 0;
        if (lancedb_table_list_indices(table, &indices, &count, nullptr) == LANCEDB_SUCCESS && count > 0 && indices) {
            std::string name(indices[0]);
            lancedb_free_index_list(indices, count);
            return name;
        }
        return "";
    }

    // Query index statistics directly from LanceDB engine
    static bool get_index_stats(LanceDBTable* table, LanceDBIndexStats& stats) {
        std::string idx_name = get_first_index_name(table);
        if (idx_name.empty()) {
            // No index exists yet
            stats.num_indexed_rows = 0;
            stats.num_unindexed_rows = lancedb_table_count_rows(table);
            stats.num_indices = 0;
            return true;
        }
        char* err_msg = nullptr;
        LanceDBError result = lancedb_table_index_stats(table, idx_name.c_str(), &stats, &err_msg);
        if (err_msg) lancedb_free_string(err_msg);
        if (result == LANCEDB_INDEX_NOT_FOUND) {
            stats.num_indexed_rows = 0;
            stats.num_unindexed_rows = lancedb_table_count_rows(table);
            stats.num_indices = 0;
            return true;
        }
        return result == LANCEDB_SUCCESS;
    }

    // Save a metadata key-value pair to the LanceDB table
    static bool save_table_metadata(const std::string& bucket, const std::string& index,
                                     const std::string& meta_key, const std::string& meta_value) {
        std::string db_path = utils::get_index_db_path(bucket, index);
        LanceDBConnection* conn = connect(db_path);
        if (!conn) return false;

        LanceDBTable* table = open_table(conn, "vectors");
        if (!table) {
            lancedb_connection_free(conn);
            return false;
        }

        const char* keys[] = {meta_key.c_str()};
        const char* values[] = {meta_value.c_str()};
        char* err_msg = nullptr;
        LanceDBError result = lancedb_table_set_metadata(table, keys, values, 1, &err_msg);
        if (err_msg) lancedb_free_string(err_msg);

        lancedb_table_free(table);
        lancedb_connection_free(conn);
        return result == LANCEDB_SUCCESS;
    }

    // Load a metadata value by key from the LanceDB table
    static std::string load_table_metadata(const std::string& bucket, const std::string& index,
                                            const std::string& meta_key) {
        std::string db_path = utils::get_index_db_path(bucket, index);
        LanceDBConnection* conn = connect(db_path);
        if (!conn) return "";

        LanceDBTable* table = open_table(conn, "vectors");
        if (!table) {
            lancedb_connection_free(conn);
            return "";
        }

        const char* filter_keys[] = {meta_key.c_str()};
        char** keys_out = nullptr;
        char** values_out = nullptr;
        size_t count = 0;
        char* err_msg = nullptr;
        LanceDBError result = lancedb_table_get_metadata(
            table, filter_keys, 1, &keys_out, &values_out, &count, &err_msg);
        if (err_msg) lancedb_free_string(err_msg);

        std::string value;
        if (result == LANCEDB_SUCCESS && count > 0 && values_out) {
            value = values_out[0];
        }

        if (keys_out || values_out) lancedb_free_metadata(keys_out, values_out, count);
        lancedb_table_free(table);
        lancedb_connection_free(conn);
        return value;
    }

    // Get index stats by opening a connection to the DB
    static bool get_index_stats_for(const std::string& bucket, const std::string& index,
                                     LanceDBIndexStats& stats) {
        std::string db_path = utils::get_index_db_path(bucket, index);
        LanceDBConnection* conn = connect(db_path);
        if (!conn) return false;

        LanceDBTable* table = open_table(conn, "vectors");
        if (!table) {
            lancedb_connection_free(conn);
            return false;
        }

        bool ok = get_index_stats(table, stats);
        lancedb_table_free(table);
        lancedb_connection_free(conn);
        return ok;
    }

    // Check if an index (LanceDB database with "vectors" table) exists.
    // Works for both local and S3 backends.
    static bool index_exists(const std::string& bucket, const std::string& index) {
        std::string db_path = utils::get_index_db_path(bucket, index);

        if (BackendConfig::instance().is_local()) {
            return utils::directory_exists(db_path);
        }

        // S3 mode: try to connect and check for the "vectors" table
        LanceDBConnection* conn = connect(db_path);
        if (!conn) return false;

        char** table_names = nullptr;
        size_t count = 0;
        char* err_msg = nullptr;
        bool exists = false;
        if (lancedb_connection_table_names(conn, &table_names, &count, &err_msg) == LANCEDB_SUCCESS) {
            for (size_t i = 0; i < count; i++) {
                if (std::string(table_names[i]) == "vectors") {
                    exists = true;
                    break;
                }
            }
            if (table_names) lancedb_free_table_names(table_names, count);
        }
        if (err_msg) lancedb_free_string(err_msg);
        lancedb_connection_free(conn);
        return exists;
    }
};

// ============================================================================
// Table State Manager - Manages per-table index state
// ============================================================================

class TableStateManager {
private:
    std::map<std::string, std::shared_ptr<TableIndexState>> states_;
    mutable std::mutex mutex_;

    std::string make_key(const std::string& bucket, const std::string& index) const {
        return bucket + "/" + index;
    }

public:
    static TableStateManager& instance() {
        static TableStateManager manager;
        return manager;
    }

    std::shared_ptr<TableIndexState> get_or_create(const std::string& bucket,
                                                     const std::string& index) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = make_key(bucket, index);

        auto it = states_.find(key);
        if (it != states_.end()) {
            return it->second;
        }

        // Try to load from LanceDB table metadata
        std::string state_str = LanceDBHelper::load_table_metadata(
            bucket, index, TABLE_METADATA_STATE_KEY);

        auto state = std::make_shared<TableIndexState>();
        state->bucket_name = bucket;
        state->index_name = index;

        if (!state_str.empty()) {
            try {
                json j = json::parse(state_str);
                state->load_from_json(j);
            } catch (...) {
                // Ignore parse errors, use default state
            }
        }

        states_[key] = state;
        return state;
    }

    void save_state(const std::shared_ptr<TableIndexState>& state) {
        std::string json_str = state->to_json().dump(2);
        if (!LanceDBHelper::save_table_metadata(
                state->bucket_name, state->index_name,
                TABLE_METADATA_STATE_KEY, json_str)) {
            LOG_ERROR("TABLE_STATE", state->index_name,
                      "Failed to save index state to table metadata");
        }
    }

    void remove(const std::string& bucket, const std::string& index) {
        std::lock_guard<std::mutex> lock(mutex_);
        states_.erase(make_key(bucket, index));
    }
};

// ============================================================================
// Index Builder - Synchronous build with file locking
// ============================================================================

//TODO: IndexBuilder should not use Filelock directly, it should use the Base_lock abstraction, so that we can replace the locking mechanism when we move to s3 system. the FileLock is for the purpose of this prototype, on s3 storage system it can be achived by s3-conditional-write.

class IndexBuilder {
public:
    static IndexBuilder& instance() {
        static IndexBuilder builder;
        return builder;
    }

    // Check if rebuild is needed (using LanceDB index_stats) and trigger if so
    // Returns true if rebuild was triggered and completed
    bool check_and_rebuild_if_needed(std::shared_ptr<TableIndexState> state) {
        // Query live index stats from LanceDB
        LanceDBIndexStats stats;
        if (!LanceDBHelper::get_index_stats_for(state->bucket_name, state->index_name, stats)) {
            return false;
        }

        // Check thresholds against live stats
        bool needs = false;
        if (stats.num_unindexed_rows >= state->unindexed_threshold) {
            needs = true;
        }

        if (!needs) return false;

        return do_rebuild(state, stats, false);
    }

    // Force trigger rebuild (for manual trigger)
    bool force_rebuild(std::shared_ptr<TableIndexState> state) {
        LanceDBIndexStats stats;
        LanceDBHelper::get_index_stats_for(state->bucket_name, state->index_name, stats);
        return do_rebuild(state, stats, true);
    }

private:
    bool do_rebuild(std::shared_ptr<TableIndexState> state,
                    const LanceDBIndexStats& stats, bool forced) {
        FrontendLocker lock(state->bucket_name, state->index_name);

        if (!lock.lock_exclusive()) {
            LOG_ERROR("INDEX_BUILDER", state->index_name, "Failed to acquire lock for rebuild");
            return false;
        }
        LOG_INFO("INDEX_LOCK", state->index_name, "Lock acquired for rebuild check (pid=" + std::to_string(getpid()) + ")");

        // Reload build-coordination state from disk
        reload_state_from_metadata(state);
        state->reset_crashed_builder();

        // Check if another process is already building
        if (state->index_build_in_progress && state->is_builder_alive()) {
            LOG_INFO("INDEX_BUILDER", state->index_name,
                     "Build already in progress by PID " + std::to_string(state->builder_pid));
            return false;
        }

        // Mark as building and save
        state->mark_build_started();
        TableStateManager::instance().save_state(state);

        std::string reason = forced ? "manual" : "threshold";
        LOG_INFO("BUILD_INDEX", state->index_name,
                 "Starting index build (" + reason + ", type=" + state->config.index_type +
                 ", unindexed=" + std::to_string(stats.num_unindexed_rows) +
                 ", indexed=" + std::to_string(stats.num_indexed_rows) +
                 ", pid=" + std::to_string(getpid()) + ")");

        // Release lock during the actual build
        lock.unlock();

        bool success = run_build(state);

        // Re-acquire lock to update build-coordination state
        if (!lock.lock_exclusive()) {
            LOG_ERROR("INDEX_BUILDER", state->index_name, "Failed to acquire lock after build");
            return success;
        }

        reload_state_from_metadata(state);

        if (success) {
            state->mark_build_complete();
            // Log post-build stats from LanceDB
            LanceDBIndexStats post_stats;
            if (LanceDBHelper::get_index_stats_for(state->bucket_name, state->index_name, post_stats)) {
                LOG_INFO("BUILD_INDEX", state->index_name,
                         "Index build complete (indexed=" + std::to_string(post_stats.num_indexed_rows) +
                         ", unindexed=" + std::to_string(post_stats.num_unindexed_rows) + ")");
            }
        } else {
            state->mark_build_complete();
            LOG_ERROR("BUILD_INDEX", state->index_name, "Index build failed");
        }

        TableStateManager::instance().save_state(state);
        return success;
    }

    void reload_state_from_metadata(std::shared_ptr<TableIndexState> state) {
        std::string state_str = LanceDBHelper::load_table_metadata(
            state->bucket_name, state->index_name, TABLE_METADATA_STATE_KEY);
        if (!state_str.empty()) {
            try {
                json j = json::parse(state_str);
                state->load_from_json(j);
            } catch (...) {}
        }
    }

    bool run_build(std::shared_ptr<TableIndexState> state) {
        std::string db_path = utils::get_index_db_path(state->bucket_name, state->index_name);
        LanceDBConnection* conn = LanceDBHelper::connect(db_path);
        if (!conn) {
            LOG_ERROR("BUILD_INDEX", state->index_name, "Failed to connect to database");
            return false;
        }

        LanceDBTable* table = LanceDBHelper::open_table(conn, "vectors");
        if (!table) {
            lancedb_connection_free(conn);
            LOG_ERROR("BUILD_INDEX", state->index_name, "Failed to open table");
            return false;
        }

        LanceDBVectorIndexConfig vec_config;
        vec_config.num_partitions = state->config.num_partitions;
        vec_config.num_sub_vectors = state->config.num_sub_vectors;
        vec_config.max_iterations = state->config.max_iterations;
        vec_config.sample_rate = state->config.sample_rate;
        vec_config.distance_type = state->config.to_lancedb_distance();
        vec_config.accelerator = state->config.accelerator.empty() ?
                                  nullptr : state->config.accelerator.c_str();
        vec_config.replace = 1;

        const char* columns[] = {"data"};
        char* err_msg = nullptr;
        LanceDBError result = lancedb_table_create_vector_index(
            table, columns, 1, state->config.to_lancedb_type(), &vec_config, &err_msg);

        lancedb_table_free(table);
        lancedb_connection_free(conn);

        if (result != LANCEDB_SUCCESS) {
            std::string error = err_msg ? err_msg : lancedb_error_to_message(result);
            if (err_msg) lancedb_free_string(err_msg);
            LOG_ERROR("BUILD_INDEX", state->index_name, "Index build failed: " + error);
            return false;
        }

        return true;
    }
};

// ============================================================================
// S3 Vector API Implementations
// ============================================================================

// CreateVectorBucket
ApiResponse CreateVectorBucket(const json& request) {
    if (!request.contains("vectorBucketName")) {
        return make_error(400, "ValidationException", "vectorBucketName is required");
    }

    std::string bucket_name = request["vectorBucketName"].get<std::string>();

    if (BackendConfig::instance().is_s3()) {
        // S3 mode: vectorBucketName IS the S3 bucket — create it
        const auto& cfg = BackendConfig::instance().s3;
        S3HttpConfig http_cfg{cfg.endpoint, cfg.region, cfg.access_key_id,
                              cfg.secret_access_key, !cfg.addressing_style.empty()};

        if (s3_head_bucket(http_cfg, bucket_name).ok) {
            return make_error(409, "ConflictException",
                "Vector bucket '" + bucket_name + "' already exists");
        }

        auto result = s3_create_bucket(http_cfg, bucket_name);
        if (!result.ok) {
            return make_error(500, "InternalServerException",
                "Failed to create S3 bucket (HTTP " + std::to_string(result.http_code) + "): " + result.body);
        }
    } else {
        // Local mode: create directory
        std::string bucket_path = utils::get_bucket_path(bucket_name);
        if (utils::directory_exists(bucket_path)) {
            return make_error(409, "ConflictException",
                "Vector bucket '" + bucket_name + "' already exists");
        }
        if (!utils::create_directories(bucket_path)) {
            return make_error(500, "InternalServerException",
                "Failed to create vector bucket directory");
        }
    }

    // Ensure local metadata dir exists for lock files
    std::string metadata_path = utils::get_metadata_path(bucket_name);
    utils::create_directories(metadata_path);

    LOG_INFO("CREATE_BUCKET", bucket_name, "Bucket created");

    return make_success({{"vectorBucketName", bucket_name}});
}

// CreateIndex with index configuration
ApiResponse CreateIndex(const json& request) {
    if (!request.contains("indexName")) {
        return make_error(400, "ValidationException", "indexName is required");
    }
    if (!request.contains("dimension")) {
        return make_error(400, "ValidationException", "dimension is required");
    }
    if (!request.contains("vectorBucketName")) {
        return make_error(400, "ValidationException", "vectorBucketName is required");
    }

    std::string bucket_name = request["vectorBucketName"].get<std::string>();
    std::string index_name = request["indexName"].get<std::string>();
    int dimension = request["dimension"].get<int>();

    // Parse index configuration
    IndexConfig config;
    if (request.contains("indexType")) config.index_type = request["indexType"];
    if (request.contains("distanceMetric")) config.distance_metric = request["distanceMetric"];
    if (request.contains("numPartitions")) config.num_partitions = request["numPartitions"];
    if (request.contains("numSubVectors")) config.num_sub_vectors = request["numSubVectors"];
    if (request.contains("maxIterations")) config.max_iterations = request["maxIterations"];
    if (request.contains("sampleRate")) config.sample_rate = request["sampleRate"];
    if (request.contains("accelerator")) config.accelerator = request["accelerator"];
    if (request.contains("nprobes")) config.nprobes = request["nprobes"];
    if (request.contains("refineFactor")) config.refine_factor = request["refineFactor"];
    if (request.contains("ef")) config.ef = request["ef"];

    // Parse dynamic scalar schema
    if (request.contains("scalarSchema") && request["scalarSchema"].is_array()) {
        for (const auto& item : request["scalarSchema"]) {
            ScalarFieldDef field;
            field.name = item.value("name", "");
            field.type = item.value("type", "string");
            if (field.name.empty()) continue;
            // Reject reserved column names
            if (field.name == "key" || field.name == "data" ||
                field.name == "metadata" || field.name == "_distance") {
                return make_error(400, "ValidationException",
                    "'" + field.name + "' is a reserved column name");
            }
            // Validate type
            if (field.type != "string" && field.type != "int" &&
                field.type != "float" && field.type != "bool") {
                return make_error(400, "ValidationException",
                    "Invalid type '" + field.type + "' for field '" + field.name +
                    "'. Must be string, int, float, or bool");
            }
            config.scalar_schema.push_back(field);
        }
    }

    // Default distance metric
    if (!request.contains("distanceMetric")) {
        config.distance_metric = "euclidean";
    }

    if (BackendConfig::instance().is_s3()) {
        const auto& cfg = BackendConfig::instance().s3;
        S3HttpConfig http_cfg{cfg.endpoint, cfg.region, cfg.access_key_id,
                              cfg.secret_access_key, !cfg.addressing_style.empty()};
        if (!s3_head_bucket(http_cfg, bucket_name).ok) {
            return make_error(404, "NotFoundException",
                "Vector bucket '" + bucket_name + "' not found");
        }
    } else {
        std::string bucket_path = utils::get_bucket_path(bucket_name);
        if (!utils::directory_exists(bucket_path)) {
            return make_error(404, "NotFoundException",
                "Vector bucket '" + bucket_name + "' not found");
        }
    }

    if (LanceDBHelper::index_exists(bucket_name, index_name)) {
        return make_error(409, "ConflictException",
            "Index '" + index_name + "' already exists");
    }

    std::string db_path = utils::get_index_db_path(bucket_name, index_name);
    LanceDBConnection* conn = LanceDBHelper::connect(db_path);
    if (!conn) {
        return make_error(500, "InternalServerException",
            "Failed to create LanceDB connection");
    }

    char* err_msg = nullptr;
    //TODO : why the table name is hardcoded? we should allow custom table name in the request. it limits the number of tables.
    LanceDBTable* table = LanceDBHelper::create_table(conn, "vectors", dimension,
                                                       config.scalar_schema, &err_msg);
    if (!table) {
        std::string error_str = err_msg ? err_msg : "Failed to create table";
        if (err_msg) lancedb_free_string(err_msg);
        lancedb_connection_free(conn);
        return make_error(500, "InternalServerException", error_str);
    }

    lancedb_table_free(table);
    lancedb_connection_free(conn);

    // Create and save table state
    auto state = TableStateManager::instance().get_or_create(bucket_name, index_name);
    state->dimension = dimension;
    state->config = config;

    // Parse rebuild thresholds if provided
    if (request.contains("unindexedThreshold")) {
        state->unindexed_threshold = request["unindexedThreshold"];
    }
    if (request.contains("deletionRatioThreshold")) {
        state->deletion_ratio_threshold = request["deletionRatioThreshold"];
    }

    TableStateManager::instance().save_state(state);

    LOG_INFO("CREATE_INDEX", index_name,
             "Index created (dimension=" + std::to_string(dimension) +
             ", type=" + config.index_type + ")");

    return make_success({
        {"indexName", index_name},
        {"vectorBucketName", bucket_name},
        {"dimension", dimension},
        {"config", config.to_json()}
    });
}

// PutVectors - Add vectors, update state, check if rebuild needed
ApiResponse PutVectors(const json& request) {
    if (!request.contains("vectors") || !request["vectors"].is_array()) {
        return make_error(400, "ValidationException", "vectors array is required");
    }
    if (!request.contains("indexName")) {
        return make_error(400, "ValidationException", "indexName is required");
    }
    if (!request.contains("vectorBucketName")) {
        return make_error(400, "ValidationException", "vectorBucketName is required");
    }

    std::string bucket_name = request["vectorBucketName"].get<std::string>();
    std::string index_name = request["indexName"].get<std::string>();

    // Get table state
    auto state = TableStateManager::instance().get_or_create(bucket_name, index_name);
    int dimension = state->dimension;

    if (dimension == 0) {
        return make_error(404, "NotFoundException",
            "Index '" + index_name + "' not found or dimension not configured");
    }

    // Get dynamic scalar schema from stored config
    const auto& scalar_schema = state->config.scalar_schema;

    // Parse vectors
    std::vector<std::string> keys;
    std::vector<std::vector<float>> vectors;
    std::vector<std::string> metadata_list;
    std::map<std::string, std::vector<json>> scalar_values;
    for (const auto& sf : scalar_schema) {
        scalar_values[sf.name] = {};
    }

    const auto& vectors_array = request["vectors"];
    if (vectors_array.size() > 500) {
        return make_error(400, "ValidationException",
            "Maximum 500 vectors per request");
    }

    for (const auto& vec : vectors_array) {
        if (!vec.contains("key")) {
            return make_error(400, "ValidationException",
                "Each vector must have a 'key' field");
        }
        if (!vec.contains("data")) {
            return make_error(400, "ValidationException",
                "Each vector must have a 'data' field");
        }

        keys.push_back(vec["key"].get<std::string>());

        std::vector<float> data;
        if (vec["data"].contains("float32")) {
            for (const auto& val : vec["data"]["float32"]) {
                data.push_back(val.get<float>());
            }
        } else if (vec["data"].is_array()) {
            for (const auto& val : vec["data"]) {
                data.push_back(val.get<float>());
            }
        }

        if (static_cast<int>(data.size()) != dimension) {
            return make_error(400, "ValidationException",
                "Vector dimension mismatch. Expected " + std::to_string(dimension) +
                ", got " + std::to_string(data.size()));
        }

        vectors.push_back(data);

        std::string metadata = "{}";
        if (vec.contains("metadata")) {
            metadata = vec["metadata"].dump();
        }
        metadata_list.push_back(metadata);

        // Extract dynamic scalar field values
        for (const auto& sf : scalar_schema) {
            json value = nullptr;

            // 1. Check top-level vector JSON
            if (vec.contains(sf.name) && !vec[sf.name].is_null()) {
                value = vec[sf.name];
            }
            // 2. Check metadata object
            else if (vec.contains("metadata") && vec["metadata"].is_object()) {
                const auto& meta = vec["metadata"];
                if (meta.contains(sf.name) && !meta[sf.name].is_null()) {
                    value = meta[sf.name];
                }
                // 3. Check nested metadata.metadata (test data uses this structure)
                else if (meta.contains("metadata") && meta["metadata"].is_object()) {
                    const auto& nested = meta["metadata"];
                    if (nested.contains(sf.name) && !nested[sf.name].is_null()) {
                        value = nested[sf.name];
                    }
                }
            }
            scalar_values[sf.name].push_back(value);
        }
    }

    // Connect to LanceDB
    std::string db_path = utils::get_index_db_path(bucket_name, index_name);
    LanceDBConnection* conn = LanceDBHelper::connect(db_path);
    if (!conn) {
        return make_error(500, "InternalServerException",
            "Failed to connect to index database");
    }

    LanceDBTable* table = LanceDBHelper::open_table(conn, "vectors");//TODO : its hardcoded to open "vectors" table, we should allow custom table name in the request. it limits the number of tables.
    if (!table) {
        lancedb_connection_free(conn);
        return make_error(500, "InternalServerException",
            "Failed to open vectors table");
    }

    std::string error;
    bool success = LanceDBHelper::add_vectors(table, keys, vectors, metadata_list,
                                               scalar_schema, scalar_values, dimension, error);

    lancedb_table_free(table);
    lancedb_connection_free(conn);

    if (!success) {
        return make_error(500, "InternalServerException", error);
    }

    // Log each vector insertion
    for (const auto& key : keys) {
        LOG_OP("PUT_VECTOR", index_name, key, "Vector inserted");
    }

    // Check if rebuild is needed (queries live index stats from LanceDB)
    bool rebuild_triggered = IndexBuilder::instance().check_and_rebuild_if_needed(state);
    if (rebuild_triggered) {
        LOG_INFO("PUT_VECTOR", index_name, "Index rebuild completed (threshold reached)");
    }

    // Get live stats for response
    LanceDBIndexStats stats;
    LanceDBHelper::get_index_stats_for(bucket_name, index_name, stats);

    return make_success({
        {"inserted", keys.size()},
        {"rebuildTriggered", rebuild_triggered},
        {"indexState", {
            {"numIndexedRows", stats.num_indexed_rows},
            {"numUnindexedRows", stats.num_unindexed_rows},
            {"indexBuildInProgress", state->index_build_in_progress}
        }}
    });
}

// DeleteVectors - Delete vectors, update state, check if rebuild needed
ApiResponse DeleteVectors(const json& request) {
    if (!request.contains("keys") || !request["keys"].is_array()) {
        return make_error(400, "ValidationException", "keys array is required");
    }
    if (!request.contains("indexName")) {
        return make_error(400, "ValidationException", "indexName is required");
    }
    if (!request.contains("vectorBucketName")) {
        return make_error(400, "ValidationException", "vectorBucketName is required");
    }

    std::string bucket_name = request["vectorBucketName"].get<std::string>();
    std::string index_name = request["indexName"].get<std::string>();

    if (!LanceDBHelper::index_exists(bucket_name, index_name)) {
        return make_error(404, "NotFoundException",
            "Index '" + index_name + "' not found");
    }

    std::string db_path = utils::get_index_db_path(bucket_name, index_name);
    LanceDBConnection* conn = LanceDBHelper::connect(db_path);
    if (!conn) {
        return make_error(500, "InternalServerException",
            "Failed to connect to index database");
    }

    LanceDBTable* table = LanceDBHelper::open_table(conn, "vectors");
    if (!table) {
        lancedb_connection_free(conn);
        return make_error(500, "InternalServerException",
            "Failed to open vectors table");
    }

    // Build predicate for deletion
    std::string predicate;
    std::vector<std::string> keys;
    for (size_t i = 0; i < request["keys"].size(); i++) {
        if (i > 0) predicate += " OR ";
        std::string key = request["keys"][i].get<std::string>();
        predicate += "key = \"" + key + "\"";
        keys.push_back(key);
    }

    char* err_msg = nullptr;
    LanceDBError result = lancedb_table_delete(table, predicate.c_str(), &err_msg);

    lancedb_table_free(table);
    lancedb_connection_free(conn);

    if (result != LANCEDB_SUCCESS) {
        std::string error = err_msg ? err_msg : lancedb_error_to_message(result);
        if (err_msg) lancedb_free_string(err_msg);
        return make_error(500, "InternalServerException", error);
    }

    // Log each vector deletion
    for (const auto& key : keys) {
        LOG_OP("DELETE_VECTOR", index_name, key, "Vector deleted");
    }

    // Check if rebuild is needed (queries live index stats from LanceDB)
    auto state = TableStateManager::instance().get_or_create(bucket_name, index_name);
    bool rebuild_triggered = IndexBuilder::instance().check_and_rebuild_if_needed(state);
    if (rebuild_triggered) {
        LOG_INFO("DELETE_VECTOR", index_name, "Index rebuild completed (deletion ratio threshold reached)");
    }

    LanceDBIndexStats stats;
    LanceDBHelper::get_index_stats_for(bucket_name, index_name, stats);

    return make_success({
        {"deleted", keys.size()},
        {"rebuildTriggered", rebuild_triggered},
        {"indexState", {
            {"numIndexedRows", stats.num_indexed_rows},
            {"numUnindexedRows", stats.num_unindexed_rows},
            {"indexBuildInProgress", state->index_build_in_progress}
        }}
    });
}

// QueryVectors - Search using index configuration
// Supports two modes:
//   - Vector search (default): requires queryVector, finds nearest neighbors
//   - Filter-only mode: "filterOnly": true, skips vector similarity, uses scalar filters only
ApiResponse QueryVectors(const json& request) {
    if (!request.contains("topK")) {
        return make_error(400, "ValidationException", "topK is required");
    }
    if (!request.contains("indexName")) {
        return make_error(400, "ValidationException", "indexName is required");
    }
    if (!request.contains("vectorBucketName")) {
        return make_error(400, "ValidationException", "vectorBucketName is required");
    }

    std::string bucket_name = request["vectorBucketName"].get<std::string>();
    std::string index_name = request["indexName"].get<std::string>();

    int top_k = request["topK"].get<int>();
    bool return_distance = request.value("returnDistance", false);
    bool return_metadata = request.value("returnMetadata", false);
    bool explain_plan = request.value("explainPlan", false);
    bool filter_only = request.value("filterOnly", false);

    // Get table state and configuration
    auto state = TableStateManager::instance().get_or_create(bucket_name, index_name);
    IndexConfig config = state->config;

    // Override config with request-level query parameters
    if (request.contains("nprobes")) config.nprobes = request["nprobes"];
    if (request.contains("refineFactor")) config.refine_factor = request["refineFactor"];
    if (request.contains("ef")) config.ef = request["ef"];

    // Parse filter if provided
    std::string filter;
    if (request.contains("filter")) {
        filter = request["filter"].dump();
        if (filter.front() == '"' && filter.back() == '"') {
            filter = filter.substr(1, filter.length() - 2);
        }
    }

    if (filter_only && filter.empty()) {
        return make_error(400, "ValidationException",
            "filterOnly mode requires a filter");
    }

    if (!filter_only && !request.contains("queryVector")) {
        return make_error(400, "ValidationException",
            "queryVector is required (use filterOnly:true for filter-only queries)");
    }

    // Parse query vector (only needed for vector search mode)
    std::vector<float> query_vector;
    if (!filter_only) {
        if (request["queryVector"].contains("float32")) {
            for (const auto& val : request["queryVector"]["float32"]) {
                query_vector.push_back(val.get<float>());
            }
        } else if (request["queryVector"].is_array()) {
            for (const auto& val : request["queryVector"]) {
                query_vector.push_back(val.get<float>());
            }
        }

        int dimension = state->dimension;
        if (static_cast<int>(query_vector.size()) != dimension && dimension > 0) {
            return make_error(400, "ValidationException",
                "Query vector dimension mismatch. Expected " + std::to_string(dimension) +
                ", got " + std::to_string(query_vector.size()));
        }
    }

    // Connect to LanceDB
    std::string db_path = utils::get_index_db_path(bucket_name, index_name);
    LanceDBConnection* conn = LanceDBHelper::connect(db_path);
    if (!conn) {
        return make_error(500, "InternalServerException",
            "Failed to connect to index database");
    }

    LanceDBTable* table = LanceDBHelper::open_table(conn, "vectors");
    if (!table) {
        lancedb_connection_free(conn);
        return make_error(500, "InternalServerException",
            "Failed to open vectors table");
    }

    std::string error;
    std::string explain_plan_output;
    std::vector<LanceDBHelper::QueryResult> results;

    if (filter_only) {
        results = LanceDBHelper::filter_only_query(
            table, top_k, filter, return_metadata,
            config, explain_plan, explain_plan_output, error);
    } else {
        results = LanceDBHelper::query_vectors(
            table, query_vector, top_k, filter,
            return_distance, return_metadata, config.to_lancedb_distance(),
            config, explain_plan, explain_plan_output, error);
    }

    lancedb_table_free(table);
    lancedb_connection_free(conn);

    if (!error.empty() && results.empty()) {
        return make_error(500, "InternalServerException", error);
    }

    std::string mode = filter_only ? "filter-only" : "vector";
    LOG_INFO("SEARCH_VECTOR", index_name,
             "Query executed (mode=" + mode +
             ", topK=" + std::to_string(top_k) +
             ", results=" + std::to_string(results.size()) + ")");

    json vectors_response = json::array();
    for (const auto& result : results) {
        json vec;
        vec["key"] = result.key;

        if (return_distance && !filter_only) {
            vec["distance"] = result.distance;
        }

        if (return_metadata && !result.metadata.empty()) {
            try {
                vec["metadata"] = json::parse(result.metadata);
            } catch (...) {
                vec["metadata"] = result.metadata;
            }
        }

        // Include dynamic scalar fields in response
        for (const auto& [fname, fval] : result.scalar_fields) {
            vec[fname] = fval;
        }

        vectors_response.push_back(vec);
    }

    json response = {
        {"indexType", config.index_type},
        {"vectors", vectors_response},
        {"indexState", {
            {"indexBuildInProgress", state->index_build_in_progress}
        }}
    };

    if (!filter_only) {
        response["distanceMetric"] = config.distance_metric;
    }

    if (explain_plan && !explain_plan_output.empty()) {
        response["queryPlan"] = explain_plan_output;
    }

    return make_success(response);
}

// TriggerRebuild - Manually trigger index rebuild
ApiResponse TriggerRebuild(const json& request) {
    if (!request.contains("indexName")) {
        return make_error(400, "ValidationException", "indexName is required");
    }
    if (!request.contains("vectorBucketName")) {
        return make_error(400, "ValidationException", "vectorBucketName is required");
    }

    std::string bucket_name = request["vectorBucketName"].get<std::string>();
    std::string index_name = request["indexName"].get<std::string>();

    auto state = TableStateManager::instance().get_or_create(bucket_name, index_name);

    // Update config if provided
    if (request.contains("indexType")) state->config.index_type = request["indexType"];
    if (request.contains("numPartitions")) state->config.num_partitions = request["numPartitions"];
    if (request.contains("numSubVectors")) state->config.num_sub_vectors = request["numSubVectors"];
    if (request.contains("accelerator")) state->config.accelerator = request["accelerator"];

    TableStateManager::instance().save_state(state);

    // Use the new synchronous IndexBuilder with file locking
    bool success = IndexBuilder::instance().force_rebuild(state);

    // Reload state to get latest values after rebuild
    auto updated_state = TableStateManager::instance().get_or_create(bucket_name, index_name);

    if (success) {
        LOG_INFO("TRIGGER_REBUILD", index_name, "Manual rebuild completed");
        return make_success({
            {"message", "Index rebuild completed"},
            {"indexState", updated_state->to_json()}
        });
    } else {
        return make_success({
            {"message", "Index build already in progress or failed"},
            {"indexState", updated_state->to_json()}
        });
    }
}

// GetIndexState - Get current index state
ApiResponse GetIndexState(const json& request) {
    if (!request.contains("indexName")) {
        return make_error(400, "ValidationException", "indexName is required");
    }
    if (!request.contains("vectorBucketName")) {
        return make_error(400, "ValidationException", "vectorBucketName is required");
    }

    std::string bucket_name = request["vectorBucketName"].get<std::string>();
    std::string index_name = request["indexName"].get<std::string>();

    if (!LanceDBHelper::index_exists(bucket_name, index_name)) {
        return make_error(404, "NotFoundException",
            "Index '" + index_name + "' not found");
    }

    auto state = TableStateManager::instance().get_or_create(bucket_name, index_name);

    // Get live stats from LanceDB engine
    std::string db_path = utils::get_index_db_path(bucket_name, index_name);
    LanceDBConnection* conn = LanceDBHelper::connect(db_path);
    if (conn) {
        LanceDBTable* table = LanceDBHelper::open_table(conn, "vectors");
        if (table) {
            unsigned long long current_rows = lancedb_table_count_rows(table);
            unsigned long long current_version = lancedb_table_version(table);

            LanceDBIndexStats stats;
            LanceDBHelper::get_index_stats(table, stats);

            bool needs_rebuild = (stats.num_unindexed_rows >= state->unindexed_threshold);

            json response = state->to_json();
            response["currentRows"] = current_rows;
            response["currentVersion"] = current_version;
            response["numIndexedRows"] = stats.num_indexed_rows;
            response["numUnindexedRows"] = stats.num_unindexed_rows;
            response["numIndices"] = stats.num_indices;
            response["needsRebuild"] = needs_rebuild;

            lancedb_table_free(table);
            lancedb_connection_free(conn);

            return make_success(response);
        }
        lancedb_connection_free(conn);
    }

    return make_success(state->to_json());
}

// UpdateIndexConfig - Update index configuration
ApiResponse UpdateIndexConfig(const json& request) {
    if (!request.contains("indexName")) {
        return make_error(400, "ValidationException", "indexName is required");
    }
    if (!request.contains("vectorBucketName")) {
        return make_error(400, "ValidationException", "vectorBucketName is required");
    }

    std::string bucket_name = request["vectorBucketName"].get<std::string>();
    std::string index_name = request["indexName"].get<std::string>();

    auto state = TableStateManager::instance().get_or_create(bucket_name, index_name);

    // Update config fields
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (request.contains("indexType")) state->config.index_type = request["indexType"];
        if (request.contains("numPartitions")) state->config.num_partitions = request["numPartitions"];
        if (request.contains("numSubVectors")) state->config.num_sub_vectors = request["numSubVectors"];
        if (request.contains("maxIterations")) state->config.max_iterations = request["maxIterations"];
        if (request.contains("sampleRate")) state->config.sample_rate = request["sampleRate"];
        if (request.contains("accelerator")) state->config.accelerator = request["accelerator"];
        if (request.contains("nprobes")) state->config.nprobes = request["nprobes"];
        if (request.contains("refineFactor")) state->config.refine_factor = request["refineFactor"];
        if (request.contains("ef")) state->config.ef = request["ef"];

        // Update thresholds
        if (request.contains("unindexedThreshold")) {
            state->unindexed_threshold = request["unindexedThreshold"];
        }
        if (request.contains("deletionRatioThreshold")) {
            state->deletion_ratio_threshold = request["deletionRatioThreshold"];
        }
    }

    TableStateManager::instance().save_state(state);

    LOG_INFO("UPDATE_CONFIG", index_name, "Index configuration updated");

    return make_success({
        {"message", "Configuration updated"},
        {"config", state->config.to_json()},
        {"thresholds", {
            {"unindexedThreshold", state->unindexed_threshold},
            {"deletionRatioThreshold", state->deletion_ratio_threshold}
        }}
    });
}

// ============================================================================
// Command Line Interface
// ============================================================================

void print_help() {
    std::cout << R"(
S3 Vector Concurrent Service with Background Indexing

USAGE:
    s3vector_concurrent_service <command> <json>

COMMANDS:
    CreateVectorBucket      Create a new vector bucket
    CreateIndex             Create an index with configuration
    PutVectors              Add vectors (updates state, may trigger rebuild)
    DeleteVectors           Delete vectors (updates state, may trigger rebuild)
    QueryVectors            Search vectors using index
    TriggerRebuild          Manually trigger index rebuild
    GetIndexState           Get current index state and stats
    UpdateIndexConfig       Update index configuration and thresholds
    --help, -h              Show this help message

DESIGN:
    - On each PutVectors/DeleteVectors:
      1. Execute the operation
      2. Acquire file lock (flock), update state counters
      3. Check if rebuild threshold is reached
      4. If yes, mark as building, release lock, run synchronous build
      5. Re-acquire lock, update state, return

    - Concurrency model:
      - File locking (flock) ensures only one process modifies state at a time
      - Queries are lock-free (use LanceDB manifest for consistency)
      - Crash detection via builder PID tracking
      - Only one rebuild per table at a time across all processes

EXAMPLES:

    # Create a bucket
    ./s3vector_concurrent_service CreateVectorBucket '{"vectorBucketName": "my-bucket"}'

    # Create index with IVF_HNSW_PQ configuration
    ./s3vector_concurrent_service CreateIndex '{
        "vectorBucketName": "my-bucket",
        "indexName": "my-index",
        "dimension": 768,
        "indexType": "IVF_HNSW_PQ",
        "distanceMetric": "cosine",
        "numPartitions": 256,
        "unindexedThreshold": 500,
        "deletionRatioThreshold": 0.15
    }'

    # Put vectors (may trigger rebuild if threshold reached)
    ./s3vector_concurrent_service PutVectors '{
        "vectorBucketName": "my-bucket",
        "indexName": "my-index",
        "vectors": [
            {"key": "v1", "data": [0.1, 0.2, ...]}
        ]
    }'

    # Delete vectors (may trigger rebuild if deletion ratio reached)
    ./s3vector_concurrent_service DeleteVectors '{
        "vectorBucketName": "my-bucket",
        "indexName": "my-index",
        "keys": ["v1", "v2"]
    }'

    # Query with custom nprobes
    ./s3vector_concurrent_service QueryVectors '{
        "vectorBucketName": "my-bucket",
        "indexName": "my-index",
        "queryVector": [0.1, 0.2, ...],
        "topK": 10,
        "nprobes": 20,
        "returnDistance": true
    }'

    # Trigger manual rebuild
    ./s3vector_concurrent_service TriggerRebuild '{
        "vectorBucketName": "my-bucket",
        "indexName": "my-index"
    }'

    # Get index state
    ./s3vector_concurrent_service GetIndexState '{
        "vectorBucketName": "my-bucket",
        "indexName": "my-index"
    }'

    # Update thresholds
    ./s3vector_concurrent_service UpdateIndexConfig '{
        "vectorBucketName": "my-bucket",
        "indexName": "my-index",
        "unindexedThreshold": 2000,
        "deletionRatioThreshold": 0.25
    }'

INDEX TYPES:
    IVF_FLAT      - Inverted file index (no compression)
    IVF_PQ        - IVF with product quantization (default)
    IVF_HNSW_PQ   - IVF with HNSW graph and PQ
    IVF_HNSW_SQ   - IVF with HNSW graph and scalar quantization

QUERY PARAMETERS:
    nprobes       - Number of IVF partitions to search (higher = more accurate)
    refineFactor  - Number of candidates to refine (higher = more accurate)
    ef            - HNSW ef parameter (for HNSW indices only)

REBUILD THRESHOLDS:
    unindexedThreshold     - Trigger rebuild when this many new vectors (default: 256, min for IVF_PQ)
    deletionRatioThreshold - Trigger rebuild when deleted/total ratio exceeds (default: 0.20)

LOGGING:
    All operations are logged with timestamps to:
    - Console (stdout)
    - /tmp/s3vectors/operations.log

STORAGE (local backend - default):
    All data is stored under /tmp/s3vectors/
    - Buckets: /tmp/s3vectors/<bucket-name>/
    - Indices: /tmp/s3vectors/<bucket-name>/<index-name>_lancedb/
    - State:   Stored in LanceDB table metadata (key: s3v_index_state)
    - Locks:   /tmp/s3vectors/<bucket-name>/.s3v_metadata/<index-name>_index.lock

STORAGE (S3 backend):
    vectorBucketName maps 1:1 to an S3 bucket:
    - Buckets: s3://<vectorBucketName>/  (created by CreateVectorBucket)
    - Indices: s3://<vectorBucketName>/<index-name>_lancedb/
    - State:   Stored in LanceDB table metadata (key: s3v_index_state)
    - Locks:   S3 objects at s3://<vectorBucketName>/.locks/<index-name>_index.lock
    - Logs:    Local /tmp/s3vectors/operations.log

S3 BACKEND CONFIGURATION (via environment variables):
    S3V_BACKEND=s3              Enable S3 backend (default: local)
    S3V_ENDPOINT                S3 endpoint URL (e.g. http://localhost:8000)
    S3V_REGION                  AWS region (e.g. us-east-1)
    S3V_ACCESS_KEY_ID           AWS access key ID
    S3V_SECRET_ACCESS_KEY       AWS secret access key
    S3V_ALLOW_HTTP=true         Allow HTTP (non-HTTPS) endpoints
    S3V_ADDRESSING_STYLE=path   Use path-style addressing (for Ceph/RGW)

S3 BACKEND EXAMPLE (Ceph/RGW):
    export S3V_BACKEND=s3
    export S3V_ENDPOINT=http://localhost:8000
    export S3V_REGION=us-east-1
    export S3V_ACCESS_KEY_ID=<your-access-key>
    export S3V_SECRET_ACCESS_KEY=<your-secret-key>
    export S3V_ALLOW_HTTP=true
    export S3V_ADDRESSING_STYLE=path
    ./s3vector_concurrent_service CreateVectorBucket '{"vectorBucketName": "my-vectors"}'

)";
}

int main(int argc, char* argv[]) {
    // Initialize backend configuration from environment variables
    BackendConfig::instance().init_from_env();

    // Ensure local directories exist (for log file and lock files)
    if (!utils::directory_exists(LOCAL_STATE_ROOT)) {
        utils::create_directories(LOCAL_STATE_ROOT);
    }
    if (BackendConfig::instance().is_local() && !utils::directory_exists(S3_VECTORS_ROOT)) {
        utils::create_directories(S3_VECTORS_ROOT);
    }

    // Initialize logger (always local)
    std::string log_path = LOCAL_STATE_ROOT + "/" + LOG_FILE_NAME;
    Logger::instance().init(log_path, Logger::INFO, true);

    LOG_INFO("INIT", "", "Backend: " + BackendConfig::instance().describe());

    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string command = argv[1];

    if (command == "--help" || command == "-h") {
        print_help();
        return 0;
    }

    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <command> <json>\n";
        std::cerr << "Use --help for more information.\n";
        return 1;
    }

    std::string json_input = argv[2];

    json request;
    try {
        request = json::parse(json_input);
    } catch (const std::exception& e) {
        std::cerr << "Error parsing JSON input: " << e.what() << std::endl;
        return 1;
    }

    ApiResponse response;

    if (command == "CreateVectorBucket") {
        response = CreateVectorBucket(request);
    } else if (command == "CreateIndex") {
        response = CreateIndex(request);
    } else if (command == "PutVectors") {
        response = PutVectors(request);
    } else if (command == "DeleteVectors") {
        response = DeleteVectors(request);
    } else if (command == "QueryVectors") {
        response = QueryVectors(request);
    } else if (command == "TriggerRebuild") {
        response = TriggerRebuild(request);
    } else if (command == "GetIndexState") {
        response = GetIndexState(request);
    } else if (command == "UpdateIndexConfig") {
        response = UpdateIndexConfig(request);
    } else {
        std::cerr << "Unknown command: " << command << std::endl;
        print_help();
        return 1;
    }

    std::cout << response.to_string() << std::endl;

    return response.is_success() ? 0 : 1;
}
