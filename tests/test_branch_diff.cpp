/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

struct ConnectionDeleter {
  void operator()(LanceDBConnection* connection) const {
    lancedb_connection_free(connection);
  }
};

struct TableDeleter {
  void operator()(LanceDBTable* table) const { lancedb_table_free(table); }
};

struct BranchDiffDeleter {
  void operator()(LanceDBBranchDiff* diff) const {
    lancedb_free_branch_diff(diff);
  }
};

struct CherryPickResultDeleter {
  void operator()(LanceDBCherryPickResult* result) const {
    lancedb_free_cherry_pick_result(result);
  }
};

using ConnectionPtr =
    std::unique_ptr<LanceDBConnection, ConnectionDeleter>;
using TablePtr = std::unique_ptr<LanceDBTable, TableDeleter>;
using BranchDiffPtr =
    std::unique_ptr<LanceDBBranchDiff, BranchDiffDeleter>;
using CherryPickResultPtr =
    std::unique_ptr<LanceDBCherryPickResult, CherryPickResultDeleter>;

std::string require_string(const char* value) {
  REQUIRE(value != nullptr);
  return std::string(value);
}

const char* kBranchDiffJson = R"json({
  "fromBranch": "exp",
  "parentVersion": 3,
  "mainVersion": 7,
  "branchVersion": 9,
  "baseMoved": true,
  "rowCountMain": 11,
  "rowCountBranch": 12,
  "rowSummary": {
    "unchanged": 1,
    "newOnBase": 2,
    "newOnBranch": 3,
    "staleRecompute": 4,
    "inputsChanged": 5,
    "deltaAvailable": true
  },
  "addedColumns": [
    {"name": "added_b", "dataType": "int64", "nullable": false},
    {"name": "added_a", "dataType": "utf8", "nullable": true}
  ],
  "removedColumns": [
    {"name": "removed", "dataType": "float64", "nullable": true}
  ],
  "changedColumns": [
    {
      "name": "changed",
      "main": {"name": "changed", "dataType": "int32", "nullable": true},
      "branch": {"name": "changed", "dataType": "int64", "nullable": false}
    }
  ],
  "addedIndexes": [
    {"indexName": "idx_a", "columns": ["col2", "col1"], "indexType": "BTREE", "status": "ready"}
  ],
  "removedIndexes": [
    {"indexName": "idx_old", "columns": ["old"], "status": "legacy"}
  ],
  "errors": [
    {"code": "baseMoved", "message": "main advanced"}
  ]
})json";

#ifndef _WIN32

std::string ready_result_json() {
  return std::string(R"json({"status":"ready","diff":)json") +
         kBranchDiffJson +
         R"json(,"preview":{"promotedColumns":["tag"]}})json";
}

std::string cherry_picked_result_json() {
  return std::string(R"json({"status":"cherryPicked","diff":)json") +
         kBranchDiffJson +
         R"json(,"preview":{"promotedColumns":[]},"mainVersionAfter":42})json";
}

std::string failed_result_json() {
  std::string diff = kBranchDiffJson;
  const std::string errors =
      "\"errors\": [\n"
      "    {\"code\": \"baseMoved\", \"message\": \"main advanced\"}\n"
      "  ]";
  const std::string replacement =
      "\"errors\": [\n"
      "    {\"code\": \"baseMoved\", \"message\": \"main advanced\"},\n"
      "    {\"code\": \"multipleCommits\", \"message\": \"branch has more than one data commit\"}\n"
      "  ]";
  const size_t position = diff.find(errors);
  REQUIRE(position != std::string::npos);
  diff.replace(position, errors.size(), replacement);
  return std::string(R"json({"status":"failed","diff":)json") + diff +
         R"json(,"preview":{"promotedColumns":[]}})json";
}

std::string unknown_status_result_json() {
  return std::string(R"json({"status":"futureStatus","diff":)json") +
         kBranchDiffJson +
         R"json(,"preview":{"promotedColumns":[]}})json";
}

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string body;
};

std::string read_headers(int socket) {
  std::string request;
  char buffer[4096];
  while (request.find("\r\n\r\n") == std::string::npos) {
    const ssize_t count = recv(socket, buffer, sizeof(buffer), 0);
    if (count <= 0) {
      return {};
    }
    request.append(buffer, static_cast<size_t>(count));
    if (request.size() > 1024 * 1024) {
      return {};
    }
  }
  return request;
}

size_t content_length(const std::string& headers) {
  size_t position = 0;
  while ((position = headers.find("\r\n", position)) != std::string::npos) {
    position += 2;
    const std::string line = headers.substr(position, headers.find("\r\n", position) - position);
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const std::string prefix = "content-length:";
    if (lower.rfind(prefix, 0) == 0) {
      return static_cast<size_t>(std::stoul(line.substr(prefix.size())));
    }
  }
  return 0;
}

void send_all(int socket, const std::string& bytes) {
  size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t count =
        send(socket, bytes.data() + sent, bytes.size() - sent, 0);
    if (count <= 0) {
      return;
    }
    sent += static_cast<size_t>(count);
  }
}

const char* reason_phrase(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 409:
      return "Conflict";
    case 500:
      return "Internal Server Error";
    default:
      return "Response";
  }
}

class LoopbackHttpServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  explicit LoopbackHttpServer(Handler handler) : handler_(std::move(handler)) {
    std::signal(SIGPIPE, SIG_IGN);
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd_ >= 0);

    int reuse = 1;
    REQUIRE(setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(listen(listen_fd_, 8) == 0);

    socklen_t length = sizeof(address);
    REQUIRE(getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    thread_ = std::thread([this] { serve(); });
  }

  ~LoopbackHttpServer() {
    stop_ = true;
    if (thread_.joinable()) {
      thread_.join();
    }
    if (listen_fd_ >= 0) {
      close(listen_fd_);
      listen_fd_ = -1;
    }
  }

  LoopbackHttpServer(const LoopbackHttpServer&) = delete;
  LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

  const std::string& url() const { return url_; }

  std::vector<HttpRequest> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

 private:
  void serve() {
    while (!stop_) {
      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(listen_fd_, &read_fds);
      timeval timeout{};
      timeout.tv_sec = 0;
      timeout.tv_usec = 100000;
      const int ready =
          select(listen_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
      if (ready <= 0) {
        continue;
      }

      const int client = accept(listen_fd_, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      handle_connection(client);
      close(client);
    }
  }

  void handle_connection(int client) {
    const std::string headers = read_headers(client);
    if (headers.empty()) {
      return;
    }

    HttpRequest request;
    const size_t line_end = headers.find("\r\n");
    const std::string request_line = headers.substr(0, line_end);
    const size_t first_space = request_line.find(' ');
    const size_t second_space = request_line.find(' ', first_space + 1);
    request.method = request_line.substr(0, first_space);
    request.path =
        request_line.substr(first_space + 1, second_space - first_space - 1);

    const size_t header_end = headers.find("\r\n\r\n") + 4;
    const size_t length = content_length(headers.substr(0, header_end));
    request.body = headers.substr(header_end);
    char buffer[4096];
    while (request.body.size() < length) {
      const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
      if (count <= 0) {
        return;
      }
      request.body.append(buffer, static_cast<size_t>(count));
    }
    request.body.resize(length);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(request);
    }

    const HttpResponse response = handler_(request);
    std::string wire = "HTTP/1.1 " + std::to_string(response.status) + " " +
                       reason_phrase(response.status) + "\r\n";
    wire += "Content-Type: application/json\r\n";
    wire += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    wire += "Connection: close\r\n\r\n";
    wire += response.body;
    send_all(client, wire);
  }

  int listen_fd_ = -1;
  std::string url_;
  Handler handler_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mutex_;
  std::vector<HttpRequest> requests_;
};

HttpResponse handle_request(const HttpRequest& request) {
  if (request.path == "/v1/table/my_table/describe/") {
    return {200, "{}"};
  }
  if (request.path == "/v1/table/my_table/branches/diff/") {
    return {200, kBranchDiffJson};
  }
  if (request.path == "/v1/table/my_table/branches/cherry_pick/") {
    if (request.body.find("\"dry_run\":true") != std::string::npos) {
      return {200, ready_result_json()};
    }
    if (request.body.find("\"from_branch\":\"apply\"") != std::string::npos) {
      return {200, cherry_picked_result_json()};
    }
    if (request.body.find("\"from_branch\":\"future\"") != std::string::npos) {
      return {200, unknown_status_result_json()};
    }
    if (request.body.find("\"from_branch\":\"conflict\"") != std::string::npos) {
      return {409, failed_result_json()};
    }
    return {500, "{\"error\":\"synthetic failure\"}"};
  }
  return {404, "{}"};
}

ConnectionPtr open_remote_connection(const std::string& url) {
  LanceDBConnectBuilder* builder = lancedb_connect("db://dummy");
  REQUIRE(builder != nullptr);
  builder = lancedb_connect_builder_storage_option(
      builder, "remote_database_api_key", "test-api-key");
  REQUIRE(builder != nullptr);
  builder = lancedb_connect_builder_storage_option(
      builder, "remote_database_region", "us-east-1");
  REQUIRE(builder != nullptr);
  builder = lancedb_connect_builder_storage_option(
      builder, "remote_database_host_override", url.c_str());
  REQUIRE(builder != nullptr);

  LanceDBConnection* connection = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_connect_builder_execute(builder, &connection, &error_message) ==
          LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(connection != nullptr);
  return ConnectionPtr(connection);
}

TablePtr open_remote_table(LanceDBConnection* connection) {
  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_connection_open_table(connection, "my_table", &table,
                                        &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  return TablePtr(table);
}

#endif

}  // namespace

TEST_CASE_METHOD(LanceDBFixture,
                 "Branch diff and cherry-pick report native unsupported",
                 "[table]") {
  TablePtr table(create_table_with_data("native_branch_remote_ops", 1, 0));

  SECTION("diff_branch preserves the SDK unsupported status and message") {
    LanceDBBranchDiff storage{};
    LanceDBBranchDiff* diff = &storage;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_diff_branch(table.get(), "exp", &diff, &error_message) ==
            LANCEDB_NOT_SUPPORTED);
    REQUIRE(diff == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(require_string(error_message).find("Enterprise tables") !=
            std::string::npos);
    lancedb_free_string(error_message);
  }

  SECTION("cherry_pick preserves the SDK unsupported status and message") {
    for (int dry_run : {0, 1}) {
      LanceDBCherryPickResult storage{};
      LanceDBCherryPickResult* result = &storage;
      char* error_message = nullptr;
      REQUIRE(lancedb_table_cherry_pick(table.get(), "exp", dry_run, &result,
                                        &error_message) == LANCEDB_NOT_SUPPORTED);
      REQUIRE(result == nullptr);
      REQUIRE(error_message != nullptr);
      REQUIRE(require_string(error_message).find("Enterprise tables") !=
              std::string::npos);
      lancedb_free_string(error_message);
    }
  }

  SECTION("invalid arguments clear the output") {
    LanceDBBranchDiff storage{};
    LanceDBBranchDiff* diff = &storage;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_diff_branch(table.get(), nullptr, &diff,
                                      &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(diff == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    LanceDBCherryPickResult result_storage{};
    LanceDBCherryPickResult* result = &result_storage;
    error_message = nullptr;
    REQUIRE(lancedb_table_cherry_pick(table.get(), "exp", 0, nullptr,
                                      &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    REQUIRE(lancedb_table_cherry_pick(nullptr, "exp", 0, &result,
                                      &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(result == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }
}

#ifndef _WIN32

TEST_CASE("Remote branch diff and cherry-pick forward SDK results", "[table]") {
  LoopbackHttpServer server(handle_request);
  ConnectionPtr connection = open_remote_connection(server.url());
  TablePtr table = open_remote_table(connection.get());

  SECTION("diff_branch preserves every nested field and list order") {
    BranchDiffPtr diff;
    LanceDBBranchDiff* raw = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_diff_branch(table.get(), "exp", &raw,
                                      &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(raw != nullptr);
    diff.reset(raw);

    REQUIRE(require_string(diff->from_branch) == "exp");
    REQUIRE(diff->parent_version == 3);
    REQUIRE(diff->main_version == 7);
    REQUIRE(diff->branch_version == 9);
    REQUIRE(diff->base_moved != 0);
    REQUIRE(diff->row_count_main == 11);
    REQUIRE(diff->row_count_branch == 12);
    REQUIRE(diff->row_summary.unchanged == 1);
    REQUIRE(diff->row_summary.new_on_base == 2);
    REQUIRE(diff->row_summary.new_on_branch == 3);
    REQUIRE(diff->row_summary.stale_recompute == 4);
    REQUIRE(diff->row_summary.inputs_changed == 5);
    REQUIRE(diff->row_summary.delta_available != 0);

    REQUIRE(diff->added_column_count == 2);
    REQUIRE(require_string(diff->added_columns[0].name) == "added_b");
    REQUIRE(require_string(diff->added_columns[0].data_type) == "int64");
    REQUIRE(diff->added_columns[0].nullable == 0);
    REQUIRE(require_string(diff->added_columns[1].name) == "added_a");
    REQUIRE(require_string(diff->added_columns[1].data_type) == "utf8");
    REQUIRE(diff->added_columns[1].nullable != 0);

    REQUIRE(diff->removed_column_count == 1);
    REQUIRE(require_string(diff->removed_columns[0].name) == "removed");
    REQUIRE(diff->removed_columns[0].nullable != 0);

    REQUIRE(diff->changed_column_count == 1);
    REQUIRE(require_string(diff->changed_columns[0].name) == "changed");
    REQUIRE(require_string(diff->changed_columns[0].main.data_type) == "int32");
    REQUIRE(diff->changed_columns[0].main.nullable != 0);
    REQUIRE(require_string(diff->changed_columns[0].branch.data_type) == "int64");
    REQUIRE(diff->changed_columns[0].branch.nullable == 0);

    REQUIRE(diff->added_index_count == 1);
    REQUIRE(require_string(diff->added_indexes[0].index_name) == "idx_a");
    REQUIRE(require_string(diff->added_indexes[0].index_type) == "BTREE");
    REQUIRE(require_string(diff->added_indexes[0].status) == "ready");
    REQUIRE(diff->added_indexes[0].column_count == 2);
    REQUIRE(require_string(diff->added_indexes[0].columns[0]) == "col2");
    REQUIRE(require_string(diff->added_indexes[0].columns[1]) == "col1");

    REQUIRE(diff->removed_index_count == 1);
    REQUIRE(require_string(diff->removed_indexes[0].index_name) == "idx_old");
    REQUIRE(diff->removed_indexes[0].index_type == nullptr);
    REQUIRE(diff->removed_indexes[0].column_count == 1);
    REQUIRE(require_string(diff->removed_indexes[0].columns[0]) == "old");

    REQUIRE(diff->error_count == 1);
    REQUIRE(diff->errors[0].code == LANCEDB_CHERRY_PICK_ERROR_BASE_MOVED);
    REQUIRE(require_string(diff->errors[0].message) == "main advanced");

    const auto requests = server.requests();
    const auto request = std::find_if(
        requests.begin(), requests.end(), [](const HttpRequest& request) {
          return request.path == "/v1/table/my_table/branches/diff/";
        });
    REQUIRE(request != requests.end());
    REQUIRE(request->method == "POST");
    REQUIRE(request->body.find("\"from_branch\":\"exp\"") != std::string::npos);
  }

  SECTION("dry run forwards the flag and leaves the optional version absent") {
    CherryPickResultPtr result;
    LanceDBCherryPickResult* raw = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_cherry_pick(table.get(), "exp", 1, &raw,
                                      &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(raw != nullptr);
    result.reset(raw);

    REQUIRE(result->status == LANCEDB_CHERRY_PICK_STATUS_READY);
    REQUIRE(result->has_main_version_after == 0);
    REQUIRE(result->promoted_column_count == 1);
    REQUIRE(require_string(result->promoted_columns[0]) == "tag");
    REQUIRE(require_string(result->diff.from_branch) == "exp");
    REQUIRE(result->diff.added_column_count == 2);

    const auto requests = server.requests();
    const auto request = std::find_if(
        requests.begin(), requests.end(), [](const HttpRequest& request) {
          return request.path == "/v1/table/my_table/branches/cherry_pick/" &&
                 request.body.find("\"dry_run\":true") != std::string::npos;
        });
    REQUIRE(request != requests.end());
  }

  SECTION("successful cherry-pick preserves the optional main version") {
    CherryPickResultPtr result;
    LanceDBCherryPickResult* raw = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_cherry_pick(table.get(), "apply", 0, &raw,
                                      &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(raw != nullptr);
    result.reset(raw);

    REQUIRE(result->status == LANCEDB_CHERRY_PICK_STATUS_CHERRY_PICKED);
    REQUIRE(result->has_main_version_after != 0);
    REQUIRE(result->main_version_after == 42);
    REQUIRE(result->promoted_column_count == 0);
    REQUIRE(result->promoted_columns == nullptr);
  }

  SECTION("HTTP 409 is an SDK result with Failed, not a C error") {
    CherryPickResultPtr result;
    LanceDBCherryPickResult* raw = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_cherry_pick(table.get(), "conflict", 0, &raw,
                                      &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(raw != nullptr);
    result.reset(raw);

    REQUIRE(result->status == LANCEDB_CHERRY_PICK_STATUS_FAILED);
    REQUIRE(result->has_main_version_after == 0);
    REQUIRE(result->diff.error_count == 2);
    REQUIRE(result->diff.errors[0].code ==
            LANCEDB_CHERRY_PICK_ERROR_BASE_MOVED);
    REQUIRE(require_string(result->diff.errors[0].message) == "main advanced");
    REQUIRE(result->diff.errors[1].code ==
            LANCEDB_CHERRY_PICK_ERROR_UNKNOWN);
    REQUIRE(require_string(result->diff.errors[1].message) ==
            "branch has more than one data commit");
  }

  SECTION("unknown status maps to the SDK Unknown value") {
    CherryPickResultPtr result;
    LanceDBCherryPickResult* raw = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_cherry_pick(table.get(), "future", 0, &raw,
                                      &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(raw != nullptr);
    result.reset(raw);

    REQUIRE(result->status == LANCEDB_CHERRY_PICK_STATUS_UNKNOWN);
  }

  SECTION("transport failures remain C errors with no result") {
    LanceDBCherryPickResult* result = nullptr;
    char* error_message = nullptr;
    const LanceDBError status =
        lancedb_table_cherry_pick(table.get(), "transport", 0, &result,
                                  &error_message);
    REQUIRE(status != LANCEDB_SUCCESS);
    REQUIRE(status == LANCEDB_HTTP);
    REQUIRE(result == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(require_string(error_message).find("500") != std::string::npos);
    lancedb_free_string(error_message);
  }
}

#endif
