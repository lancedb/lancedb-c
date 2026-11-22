/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

#include <cmath>
#include <map>
#include <memory>
#include <optional>
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

std::shared_ptr<arrow::RecordBatch> create_fts_extra_record_batch(
    int id, const std::string& text) {
  auto schema = create_fts_test_schema();

  arrow::Int32Builder id_builder;
  arrow::StringBuilder text_builder;
  arrow::FixedSizeListBuilder data_builder(
      arrow::default_memory_pool(), std::make_unique<arrow::FloatBuilder>(), kDimensions);

  REQUIRE(id_builder.Append(id).ok());
  REQUIRE(text_builder.Append(text).ok());
  auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
  const auto vector = vector_for_row(id);
  for (const float value : vector) {
    REQUIRE(list_builder->Append(value).ok());
  }
  REQUIRE(data_builder.Append().ok());

  std::shared_ptr<arrow::Array> id_array, text_array, data_array;
  REQUIRE(id_builder.Finish(&id_array).ok());
  REQUIRE(text_builder.Finish(&text_array).ok());
  REQUIRE(data_builder.Finish(&data_array).ok());

  return arrow::RecordBatch::Make(schema, 1, {id_array, text_array, data_array});
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

float float_column_of(const QueryResultBatches& result,
                      const std::string& column,
                      int32_t id) {
  float value = 0.0F;
  bool found = false;
  for (const auto& batch : result.batches) {
    auto id_column = batch->GetColumnByName("id");
    auto value_column = batch->GetColumnByName(column);
    REQUIRE(id_column != nullptr);
    REQUIRE(value_column != nullptr);
    auto id_array = std::static_pointer_cast<arrow::Int32Array>(id_column);
    auto value_array = std::static_pointer_cast<arrow::FloatArray>(value_column);
    for (int64_t i = 0; i < id_array->length(); i++) {
      if (id_array->Value(i) == id) {
        REQUIRE(!found);
        found = true;
        value = value_array->Value(i);
      }
    }
  }
  REQUIRE(found);
  return value;
}

float relevance_score_of(const QueryResultBatches& result, int32_t id) {
  return float_column_of(result, "_relevance_score", id);
}

std::map<int32_t, float> expected_rrf_scores(
    const std::vector<int32_t>& vector_ids,
    const std::vector<int32_t>& fts_ids,
    float k) {
  std::map<int32_t, float> scores;
  for (size_t i = 0; i < vector_ids.size(); i++) {
    scores[vector_ids[i]] += 1.0F / (k + static_cast<float>(i + 1));
  }
  for (size_t i = 0; i < fts_ids.size(); i++) {
    scores[fts_ids[i]] += 1.0F / (k + static_cast<float>(i + 1));
  }
  return scores;
}

std::vector<int32_t> ids_ordered_by_score(const std::map<int32_t, float>& scores) {
  std::vector<int32_t> ids;
  ids.reserve(scores.size());
  for (const auto& [id, _] : scores) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end(), [&](int32_t left, int32_t right) {
    return scores.at(left) > scores.at(right);
  });
  return ids;
}

std::map<int32_t, float> expected_normalized_distances(
    const std::vector<int32_t>& ids,
    const std::map<int32_t, float>& distances,
    bool rank) {
  REQUIRE(!ids.empty());
  std::map<int32_t, float> normalized;
  if (rank) {
    auto ordered = ids;
    std::sort(ordered.begin(), ordered.end(), [&](int32_t left, int32_t right) {
      return distances.at(left) < distances.at(right);
    });
    const float denominator = static_cast<float>(ordered.size() - 1);
    for (size_t i = 0; i < ordered.size(); i++) {
      normalized[ordered[i]] = static_cast<float>(i) / denominator;
    }
    return normalized;
  }

  float min_distance = distances.at(ids.front());
  float max_distance = min_distance;
  for (const int32_t id : ids) {
    min_distance = std::min(min_distance, distances.at(id));
    max_distance = std::max(max_distance, distances.at(id));
  }
  const float range = (max_distance - min_distance < 10e-5F)
                          ? max_distance
                          : max_distance - min_distance;
  for (const int32_t id : ids) {
    normalized[id] = range != 0.0F ? (distances.at(id) - min_distance) / range
                                   : distances.at(id);
  }
  return normalized;
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

TEST_CASE_METHOD(FtsFixture, "LanceDB Query - full text row id and fast search", "[fts]") {
  SECTION("with_row_id adds the actual metadata column") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(query, "doc7", "text", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_with_row_id(query, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_query_execute(query));
    REQUIRE(result.num_rows() == 1);
    REQUIRE(ids_of(result) == std::set<int32_t>{7});
    REQUIRE(result.schema->GetFieldByName("_rowid") != nullptr);

    auto row_id_column = result.batches.at(0)->GetColumnByName("_rowid");
    REQUIRE(row_id_column != nullptr);
    auto row_id_array = std::static_pointer_cast<arrow::UInt64Array>(row_id_column);
    REQUIRE(row_id_array->length() == 1);
    REQUIRE(!row_id_array->IsNull(0));
  }

  SECTION("fast_search excludes rows appended after the text index") {
    auto extra = create_fts_extra_record_batch(99, "doc99");
    REQUIRE(lancedb_table_add(table(), create_reader_from_batch(extra), nullptr)
        == LANCEDB_SUCCESS);

    auto run_query = [&](bool fast_search) {
      LanceDBQuery* query = lancedb_query_new(table());
      REQUIRE(query != nullptr);

      char* error_message = nullptr;
      REQUIRE(lancedb_query_full_text_search(query, "doc99", "text", &error_message)
          == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
      REQUIRE(lancedb_query_limit(query, 1, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
      if (fast_search) {
        REQUIRE(lancedb_query_fast_search(query, &error_message) == LANCEDB_SUCCESS);
        REQUIRE(error_message == nullptr);
      }

      return collect_query_result(lancedb_query_execute(query));
    };

    auto unfiltered = run_query(false);
    REQUIRE(unfiltered.num_rows() == 1);
    REQUIRE(ids_of(unfiltered) == std::set<int32_t>{99});

    auto fast = run_query(true);
    REQUIRE(fast.num_rows() == 0);
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

TEST_CASE_METHOD(FtsFixture, "LanceDB Vector Query - RRF k and normalization", "[fts][vector_query]") {
  const auto query_vector = vector_for_row(0);
  constexpr size_t limit = 10;
  constexpr float custom_k = 12.0F;
  constexpr float default_k = 60.0F;

  auto run_vector_leg = [&]() {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, limit, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    return collect_query_result(lancedb_vector_query_execute(query));
  };

  auto run_fts_ids = [&]() {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(query, "doc7", "text", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_limit(query, limit, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    return ordered_ids_of(collect_query_result(lancedb_query_execute(query)));
  };

  auto run_hybrid = [&](std::optional<float> k,
                        std::optional<LanceDBNormalizeMethod> normalize_method) {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, limit, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_full_text_search(query, "doc7", "text", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    if (k.has_value()) {
      REQUIRE(lancedb_vector_query_rerank_rrf(query, *k, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }
    if (normalize_method.has_value()) {
      REQUIRE(lancedb_vector_query_norm(query, *normalize_method, &error_message)
          == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }
    return collect_query_result(lancedb_vector_query_execute(query));
  };

  const auto vector_leg = run_vector_leg();
  const auto vector_ids = ordered_ids_of(vector_leg);
  const auto fts_ids = run_fts_ids();
  // The fixture's row i vector is i * 10 + j, so the ten nearest vectors are
  // rows 0..9. Only row 7 contains "doc7".
  REQUIRE(vector_ids == std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  REQUIRE(fts_ids == std::vector<int32_t>{7});

  std::map<int32_t, float> vector_distances;
  for (const int32_t id : vector_ids) {
    vector_distances[id] = float_column_of(vector_leg, "_distance", id);
  }

  auto require_matches = [&](const QueryResultBatches& result, float k) {
    REQUIRE(result.num_rows() == limit);
    REQUIRE(result.schema->GetFieldByName("_relevance_score") != nullptr);

    const auto expected = expected_rrf_scores(vector_ids, fts_ids, k);
    const auto expected_order = ids_ordered_by_score(expected);
    REQUIRE(ordered_ids_of(result) == expected_order);
    for (const auto& [id, score] : expected) {
      REQUIRE(std::abs(relevance_score_of(result, id) - score) < 1e-6F);
    }
  };

  SECTION("an unset k uses the SDK default of 60") {
    require_matches(run_hybrid(std::nullopt, std::nullopt), default_k);
  }

  SECTION("an explicit k changes every fusion score") {
    require_matches(run_hybrid(custom_k, std::nullopt), custom_k);
  }

  SECTION("normalization changes the preserved hybrid _score without changing RRF") {
    auto score_norm = run_hybrid(custom_k, LANCEDB_NORMALIZE_SCORE);
    auto rank_norm = run_hybrid(custom_k, LANCEDB_NORMALIZE_RANK);
    const auto expected_order =
        ids_ordered_by_score(expected_rrf_scores(vector_ids, fts_ids, custom_k));

    REQUIRE(score_norm.schema->GetFieldByName("_score") != nullptr);
    REQUIRE(rank_norm.schema->GetFieldByName("_score") != nullptr);
    REQUIRE(ordered_ids_of(score_norm) == expected_order);
    REQUIRE(ordered_ids_of(rank_norm) == expected_order);

    // The SDK reranker merges with the FTS leg's schema. Both legs place their
    // single metric column at the same position, so the vector leg's normalized
    // distance survives as `_score`. Row 7 appears in both legs and the vector
    // occurrence is merged first, so every returned row here carries the
    // vector-side normalization.
    const auto expected_score =
        expected_normalized_distances(vector_ids, vector_distances, false);
    const auto expected_rank =
        expected_normalized_distances(vector_ids, vector_distances, true);
    bool saw_different_score = false;
    for (const int32_t id : vector_ids) {
      const auto score = float_column_of(score_norm, "_score", id);
      const auto rank = float_column_of(rank_norm, "_score", id);
      REQUIRE(std::abs(score - expected_score.at(id)) < 1e-6F);
      REQUIRE(std::abs(rank - expected_rank.at(id)) < 1e-6F);
      REQUIRE(std::abs(relevance_score_of(score_norm, id) - relevance_score_of(rank_norm, id))
          < 1e-6F);
      saw_different_score = saw_different_score || std::abs(score - rank) >= 1e-4F;
    }
    REQUIRE(saw_different_score);
  }

  SECTION("a reranker on a vector-only query does not change the rows") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 3, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_rerank_rrf(query, custom_k, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_norm(query, LANCEDB_NORMALIZE_RANK, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_vector_query_execute(query));
    REQUIRE(ordered_ids_of(result) == std::vector<int32_t>{0, 1, 2});
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
