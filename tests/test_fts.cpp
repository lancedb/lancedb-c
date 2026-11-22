/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr int kRowCount = 24;
constexpr size_t kDimensions = TEST_SCHEMA_DIMENSIONS;

std::shared_ptr<arrow::Schema> create_fts_test_schema() {
  return arrow::schema({
      arrow::field("id", arrow::int32()),
      arrow::field("text", arrow::utf8()),
      arrow::field("data", arrow::fixed_size_list(arrow::float32(), kDimensions)),
  });
}

std::shared_ptr<arrow::RecordBatch> create_fts_test_record_batch() {
  auto schema = create_fts_test_schema();

  arrow::Int32Builder id_builder;
  arrow::StringBuilder text_builder;
  arrow::FixedSizeListBuilder data_builder(
      arrow::default_memory_pool(), std::make_unique<arrow::FloatBuilder>(), kDimensions);

  for (int i = 0; i < kRowCount; i++) {
    REQUIRE(id_builder.Append(i).ok());
    REQUIRE(text_builder.Append("common doc" + std::to_string(i)).ok());

    auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    for (size_t j = 0; j < kDimensions; j++) {
      REQUIRE(list_builder->Append(static_cast<float>(i * 10 + static_cast<int>(j))).ok());
    }
    REQUIRE(data_builder.Append().ok());
  }

  std::shared_ptr<arrow::Array> id_array, text_array, data_array;
  REQUIRE(id_builder.Finish(&id_array).ok());
  REQUIRE(text_builder.Finish(&text_array).ok());
  REQUIRE(data_builder.Finish(&data_array).ok());

  return arrow::RecordBatch::Make(
      schema, kRowCount, {id_array, text_array, data_array});
}

std::vector<float> vector_for_row(int row) {
  std::vector<float> vector(kDimensions);
  for (size_t j = 0; j < kDimensions; j++) {
    vector[j] = static_cast<float>(row * 10 + static_cast<int>(j));
  }
  return vector;
}

struct QueryResultBatches {
  std::shared_ptr<arrow::Schema> schema;
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;

  size_t num_rows() const {
    size_t rows = 0;
    for (const auto& batch : batches) {
      rows += static_cast<size_t>(batch->num_rows());
    }
    return rows;
  }
};

void free_arrow_result(FFI_ArrowArray** arrays, FFI_ArrowSchema* schema, size_t count) {
  lancedb_free_arrow_arrays(arrays, count);
  lancedb_free_arrow_schema(schema);
}

// Consume a query result and import every batch it returned.
QueryResultBatches collect_query_result(LanceDBQueryResult* result) {
  REQUIRE(result != nullptr);

  FFI_ArrowArray** result_arrays = nullptr;
  FFI_ArrowSchema* result_schema = nullptr;
  size_t count = 0;
  char* error_message = nullptr;

  const auto status = lancedb_query_result_to_arrow(
      result, &result_arrays, &result_schema, &count, &error_message);
  if (status != LANCEDB_SUCCESS || error_message != nullptr) {
    const bool had_error_message = error_message != nullptr;
    if (had_error_message) {
      lancedb_free_string(error_message);
    }
    free_arrow_result(result_arrays, result_schema, count);
    REQUIRE(status == LANCEDB_SUCCESS);
    REQUIRE(!had_error_message);
  }

  QueryResultBatches collected;
  if (count == 0) {
    REQUIRE(result_arrays == nullptr);
    REQUIRE(result_schema == nullptr);
    return collected;
  }

  auto imported_schema = arrow::ImportSchema(reinterpret_cast<ArrowSchema*>(result_schema));
  if (!imported_schema.ok()) {
    free_arrow_result(result_arrays, result_schema, count);
    REQUIRE(imported_schema.ok());
  }
  collected.schema = imported_schema.ValueOrDie();

  for (size_t i = 0; i < count; i++) {
    auto imported_batch = arrow::ImportRecordBatch(
        reinterpret_cast<ArrowArray*>(result_arrays[i]), collected.schema);
    if (!imported_batch.ok()) {
      free_arrow_result(result_arrays, result_schema, count);
      REQUIRE(imported_batch.ok());
    }
    collected.batches.push_back(imported_batch.ValueOrDie());
  }

  free_arrow_result(result_arrays, result_schema, count);

  return collected;
}

std::vector<int32_t> ordered_ids_of(const QueryResultBatches& result) {
  std::vector<int32_t> ids;
  for (const auto& batch : result.batches) {
    auto id_column = batch->GetColumnByName("id");
    REQUIRE(id_column != nullptr);
    auto id_array = std::static_pointer_cast<arrow::Int32Array>(id_column);
    for (int64_t i = 0; i < id_array->length(); i++) {
      ids.push_back(id_array->Value(i));
    }
  }
  return ids;
}

std::set<int32_t> ids_of(const QueryResultBatches& result) {
  const auto ordered = ordered_ids_of(result);
  return std::set<int32_t>(ordered.begin(), ordered.end());
}

using LanceDBTablePtr = std::unique_ptr<LanceDBTable, void (*)(LanceDBTable*)>;

class FtsFixture : public LanceDBFixture {
public:
  FtsFixture() : table_(create_fts_table()) {
    const char* columns[] = {"text"};
    char* error_message = nullptr;
    const auto status = lancedb_table_create_fts_index(
        table_.get(), columns, 1, nullptr, &error_message);
    if (status != LANCEDB_SUCCESS || error_message != nullptr) {
      const bool had_error_message = error_message != nullptr;
      if (had_error_message) {
        lancedb_free_string(error_message);
      }
      REQUIRE(status == LANCEDB_SUCCESS);
      REQUIRE(!had_error_message);
    }
  }

  LanceDBTable* table() const {
    return table_.get();
  }

private:
  LanceDBTablePtr create_fts_table() {
    auto schema = create_fts_test_schema();
    auto batch = create_fts_test_record_batch();
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    struct ArrowSchema c_schema;
    REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

    LanceDBTable* raw_table = nullptr;
    char* error_message = nullptr;
    const auto status = lancedb_table_create(
        db,
        "fts_query_test",
        reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
        reader,
        &raw_table,
        &error_message);

    if (c_schema.release) {
      c_schema.release(&c_schema);
    }

    if (status != LANCEDB_SUCCESS || error_message != nullptr || raw_table == nullptr) {
      const bool had_error_message = error_message != nullptr;
      if (had_error_message) {
        lancedb_free_string(error_message);
      }
      if (raw_table != nullptr) {
        lancedb_table_free(raw_table);
      }
      REQUIRE(status == LANCEDB_SUCCESS);
      REQUIRE(!had_error_message);
      REQUIRE(raw_table != nullptr);
    }

    return LanceDBTablePtr(raw_table, lancedb_table_free);
  }

  LanceDBTablePtr table_;
};

}  // namespace

TEST_CASE_METHOD(FtsFixture, "LanceDB Query - full text search", "[fts]") {
  SECTION("returns the matching row and the SDK score column") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, "doc7", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_query_execute(query));

    REQUIRE(result.num_rows() == 1);
    REQUIRE(ids_of(result) == std::set<int32_t>{7});
    REQUIRE(result.schema->GetFieldByName("_score") != nullptr);
  }

  SECTION("uses the indexed text column when no column is given") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, "doc7", nullptr, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_query_execute(query));

    REQUIRE(result.num_rows() == 1);
    REQUIRE(ids_of(result) == std::set<int32_t>{7});
  }

  SECTION("keeps the SDK default limit when no limit is set") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, "common", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_query_execute(query));

    // All 24 rows match, but a full-text search without a limit uses LanceDB's
    // default top-K rather than returning every match.
    REQUIRE(result.num_rows() == 10);
  }

  SECTION("applies an explicit limit") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, "common", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_limit(query, 3, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_query_execute(query));

    REQUIRE(result.num_rows() == 3);
  }

  SECTION("applies the filter and projection to the full-text search") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    const char* columns[] = {"id"};
    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, "doc7", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_where_filter(query, "id = 7", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_query_execute(query));

    REQUIRE(result.num_rows() == 1);
    REQUIRE(ids_of(result) == std::set<int32_t>{7});
    REQUIRE(result.schema->GetFieldByName("id") != nullptr);
    REQUIRE(result.schema->GetFieldByName("text") == nullptr);
    REQUIRE(result.schema->GetFieldByName("_score") != nullptr);
  }

  SECTION("applies a filter that excludes the only match") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, "doc7", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_where_filter(query, "id = 8", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_query_execute(query));

    REQUIRE(result.num_rows() == 0);
  }
}

TEST_CASE_METHOD(FtsFixture, "LanceDB Query - full text search invalid input", "[fts]") {
  SECTION("rejects NULL query and text arguments") {
    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        nullptr, "doc7", "text", &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);
    error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, nullptr, "text", &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_query_free(query);
  }

  SECTION("rejects text and column strings that are not valid UTF-8") {
    const char invalid_utf8[] = {static_cast<char>(0xff), '\0'};
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, invalid_utf8, "text", &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, "doc7", invalid_utf8, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_query_free(query);
  }

  SECTION("fails at execution when the text column does not exist") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, "doc7", "missing", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // lancedb_query_execute() consumes the query on failure as well.
    REQUIRE(lancedb_query_execute(query) == nullptr);
  }
}

TEST_CASE_METHOD(FtsFixture, "LanceDB Vector Query - hybrid search", "[fts][vector_query]") {
  const auto query_vector = vector_for_row(0);

  SECTION("fuses the nearest and full-text result sets") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_full_text_search(
        query, "doc7", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_vector_query_execute(query));

    REQUIRE(result.num_rows() == 2);
    // Row 0 is the nearest vector and row 7 is the only full-text match. A
    // vector-only query would return rows 0 and 1.
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 7});
    REQUIRE(result.schema->GetFieldByName("_relevance_score") != nullptr);
  }

  SECTION("uses the indexed text column when an empty column is given") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_full_text_search(
        query, "doc7", "", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_vector_query_execute(query));

    REQUIRE(result.num_rows() == 2);
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 7});
  }

  SECTION("applies the filter before fusing both search legs") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_where_filter(query, "id IN (6, 7)", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_full_text_search(
        query, "doc9", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_vector_query_execute(query));

    // Row 9 is the only text match and the filter excludes it. If the FTS leg
    // missed the filter, row 9 could enter the fused result. If filtering ran
    // after fusion, the limit could be consumed before row 9 was removed.
    REQUIRE(result.num_rows() == 2);
    REQUIRE(ids_of(result) == std::set<int32_t>{6, 7});
  }

  SECTION("deduplicates a row matched by both search legs") {
    const auto near_row_one = vector_for_row(1);
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), near_row_one.data(), near_row_one.size());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 3, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_full_text_search(
        query, "doc1", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_vector_query_execute(query));

    const auto ordered_ids = ordered_ids_of(result);
    const std::set<int32_t> unique_ids(ordered_ids.begin(), ordered_ids.end());
    // Row 1 is the nearest vector and the only text match, so it receives a
    // contribution from both legs and must appear first and only once.
    REQUIRE(ordered_ids.size() == 3);
    REQUIRE(unique_ids.size() == ordered_ids.size());
    REQUIRE(ordered_ids.front() == 1);
  }

  SECTION("applies projection to the fused result") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);

    const char* columns[] = {"id"};
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_select(query, columns, 1, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_full_text_search(
        query, "doc7", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_vector_query_execute(query));

    REQUIRE(result.num_rows() == 2);
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 7});
    REQUIRE(result.schema->GetFieldByName("id") != nullptr);
    REQUIRE(result.schema->GetFieldByName("text") == nullptr);
    REQUIRE(result.schema->GetFieldByName("_relevance_score") != nullptr);
  }
}

TEST_CASE_METHOD(FtsFixture, "LanceDB Vector Query - hybrid search invalid input",
                 "[fts][vector_query]") {
  const auto query_vector = vector_for_row(0);

  SECTION("rejects NULL query and text arguments") {
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_full_text_search(
        nullptr, "doc7", "text", &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);
    error_message = nullptr;
    REQUIRE(lancedb_vector_query_full_text_search(
        query, nullptr, "text", &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_vector_query_free(query);
  }

  SECTION("fails at execution when the text column does not exist") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_full_text_search(
        query, "doc7", "missing", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    // lancedb_vector_query_execute() consumes the query on failure as well.
    REQUIRE(lancedb_vector_query_execute(query) == nullptr);
  }
}

// The status executors and the IPC file share the same plain and vector
// executors as the legacy entry points, so full-text and hybrid search have to
// survive that path unchanged.
TEST_CASE_METHOD(FtsFixture, "LanceDB Query - full text search through status and IPC",
                 "[fts][ipc]") {
  SECTION("the status executor keeps the plain full-text result in the file") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(query, "doc7", "text", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(result != nullptr);

    LanceDBIpcBuffer buffer = {};
    REQUIRE(lancedb_query_result_to_ipc_file(result, &buffer, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto contents = read_ipc_file(buffer.data, buffer.len);
    REQUIRE(contents.schema->GetFieldByName("_score") != nullptr);
    size_t rows = 0;
    for (const auto& batch : contents.batches) {
      rows += static_cast<size_t>(batch->num_rows());
    }
    REQUIRE(rows == 1);

    lancedb_ipc_buffer_free(&buffer);
  }

  SECTION("the vector status executor keeps the fused hybrid result in the file") {
    const auto query_vector = vector_for_row(0);
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 2, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_full_text_search(query, "doc7", "text", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_vector_query_execute_with_status(query, &result, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(result != nullptr);

    LanceDBIpcBuffer buffer = {};
    REQUIRE(lancedb_query_result_to_ipc_file(result, &buffer, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto contents = read_ipc_file(buffer.data, buffer.len);
    REQUIRE(contents.schema->GetFieldByName("_relevance_score") != nullptr);
    size_t rows = 0;
    for (const auto& batch : contents.batches) {
      rows += static_cast<size_t>(batch->num_rows());
    }
    REQUIRE(rows == 2);

    lancedb_ipc_buffer_free(&buffer);
  }
}
