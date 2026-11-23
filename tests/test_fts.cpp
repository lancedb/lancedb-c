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
constexpr float kDefaultRrfK = 60.0F;

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

std::shared_ptr<arrow::RecordBatch> create_phrase_test_record_batch() {
  auto schema = create_fts_test_schema();
  const std::vector<std::string> texts = {
      "alpha beta",
      "alpha gap beta",
      "beta alpha",
  };

  arrow::Int32Builder id_builder;
  arrow::StringBuilder text_builder;
  arrow::FixedSizeListBuilder data_builder(
      arrow::default_memory_pool(), std::make_unique<arrow::FloatBuilder>(), kDimensions);

  for (size_t i = 0; i < texts.size(); i++) {
    REQUIRE(id_builder.Append(static_cast<int32_t>(i)).ok());
    REQUIRE(text_builder.Append(texts[i]).ok());

    auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    const auto vector = vector_for_row(static_cast<int>(i));
    for (const float value : vector) {
      REQUIRE(list_builder->Append(value).ok());
    }
    REQUIRE(data_builder.Append().ok());
  }

  std::shared_ptr<arrow::Array> id_array, text_array, data_array;
  REQUIRE(id_builder.Finish(&id_array).ok());
  REQUIRE(text_builder.Finish(&text_array).ok());
  REQUIRE(data_builder.Finish(&data_array).ok());

  return arrow::RecordBatch::Make(
      schema, static_cast<int64_t>(texts.size()), {id_array, text_array, data_array});
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
  FtsFixture(const std::string& table_name = "fts_query_test",
             std::shared_ptr<arrow::RecordBatch> batch = nullptr,
             bool with_position = false)
      : table_(create_fts_table(table_name, batch)) {
    const char* columns[] = {"text"};
    char* error_message = nullptr;
    LanceDBError status = LANCEDB_SUCCESS;
    if (with_position) {
      auto options = lancedb_index_options_default();
      options.fts_with_position = 1;
      status = lancedb_table_create_index(
          table_.get(), columns, 1, LANCEDB_INDEX_FTS, &options, &error_message);
    } else {
      status = lancedb_table_create_fts_index(
          table_.get(), columns, 1, nullptr, &error_message);
    }
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
  LanceDBTablePtr create_fts_table(
      const std::string& table_name,
      std::shared_ptr<arrow::RecordBatch> batch) {
    if (batch == nullptr) {
      batch = create_fts_test_record_batch();
    }
    auto schema = create_fts_test_schema();
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    struct ArrowSchema c_schema;
    REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

    LanceDBTable* raw_table = nullptr;
    char* error_message = nullptr;
    const auto status = lancedb_table_create(
        db,
        table_name.c_str(),
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

class PhraseFtsFixture : public FtsFixture {
public:
  PhraseFtsFixture()
      : FtsFixture("fts_phrase_test", create_phrase_test_record_batch(), true) {}
};

LanceDBFtsQuery* make_phrase_query(const char* text, const char* column, uint32_t slop) {
  LanceDBFtsQuery* fts_query = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_fts_query_new_phrase(
      text, column, slop, &fts_query, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(fts_query != nullptr);
  return fts_query;
}

LanceDBFtsQuery* make_match_query(
    const char* text,
    const char* column,
    const LanceDBFtsMatchOptions* options = nullptr) {
  LanceDBFtsQuery* fts_query = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_fts_query_new_match(
      text, column, options, &fts_query, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(fts_query != nullptr);
  return fts_query;
}

QueryResultBatches run_phrase_query(
    LanceDBTable* table,
    const char* text,
    const char* column,
    uint32_t slop,
    size_t limit = 10) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);
  auto* fts_query = make_phrase_query(text, column, slop);

  char* error_message = nullptr;
  REQUIRE(lancedb_query_full_text_search_query(query, fts_query, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  lancedb_fts_query_free(fts_query);
  REQUIRE(lancedb_query_limit(query, limit, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  return collect_query_result(lancedb_query_execute(query));
}

QueryResultBatches run_match_query(
    LanceDBTable* table,
    const char* text,
    const char* column,
    const LanceDBFtsMatchOptions* options = nullptr,
    size_t limit = 10) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);
  auto* fts_query = make_match_query(text, column, options);

  char* error_message = nullptr;
  REQUIRE(lancedb_query_full_text_search_query(query, fts_query, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  lancedb_fts_query_free(fts_query);
  REQUIRE(lancedb_query_limit(query, limit, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  return collect_query_result(lancedb_query_execute(query));
}

QueryResultBatches run_vector_leg_query(
    LanceDBTable* table,
    const std::vector<float>& query_vector,
    size_t limit) {
  LanceDBVectorQuery* query = lancedb_vector_query_new(
      table, query_vector.data(), query_vector.size());
  REQUIRE(query != nullptr);
  char* error_message = nullptr;
  REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_vector_query_limit(query, limit, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  return collect_query_result(lancedb_vector_query_execute(query));
}

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

TEST_CASE_METHOD(PhraseFtsFixture, "LanceDB Query - structured phrase full text", "[fts]") {
  SECTION("slop 0 requires adjacent tokens in order") {
    auto result = run_phrase_query(table(), "alpha beta", "text", 0);
    REQUIRE(ids_of(result) == std::set<int32_t>{0});
  }

  SECTION("slop 1 allows one intervening token") {
    auto result = run_phrase_query(table(), "alpha beta", "text", 1);
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1});
  }

  SECTION("an empty column uses the indexed text column") {
    auto result = run_phrase_query(table(), "alpha beta", "", 0);
    REQUIRE(ids_of(result) == std::set<int32_t>{0});
  }

  SECTION("one handle can be attached to several builders and freed immediately") {
    auto* fts_query = make_phrase_query("alpha beta", "text", 0);
    LanceDBQuery* first = lancedb_query_new(table());
    LanceDBQuery* second = lancedb_query_new(table());
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search_query(first, fts_query, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_full_text_search_query(second, fts_query, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_fts_query_free(fts_query);

    REQUIRE(ids_of(collect_query_result(lancedb_query_execute(first)))
        == std::set<int32_t>{0});
    REQUIRE(ids_of(collect_query_result(lancedb_query_execute(second)))
        == std::set<int32_t>{0});
  }

  SECTION("the legacy text setter overrides the phrase and keeps OR matching") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    auto* fts_query = make_phrase_query("alpha beta", "text", 0);
    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search_query(query, fts_query, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_fts_query_free(fts_query);

    // The default Match operator is OR. An AND query would match only the
    // middle row, which contains all three terms.
    REQUIRE(lancedb_query_full_text_search(
        query, "alpha beta gap", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_query_execute(query));
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 2});
  }

  SECTION("the structured setter overrides legacy text matching") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search(
        query, "alpha gap", "text", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto* fts_query = make_phrase_query("alpha beta", "text", 0);
    REQUIRE(lancedb_query_full_text_search_query(query, fts_query, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_fts_query_free(fts_query);

    auto result = collect_query_result(lancedb_query_execute(query));
    REQUIRE(ids_of(result) == std::set<int32_t>{0});
  }

  SECTION("EXPLAIN consumes the phrase setting without consuming the query") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    auto* fts_query = make_phrase_query("alpha beta", "text", 0);
    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search_query(query, fts_query, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_fts_query_free(fts_query);

    char* plan = nullptr;
    REQUIRE(lancedb_query_explain_plan(query, false, &plan, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(plan != nullptr);
    REQUIRE(plan[0] != '\0');
    lancedb_free_string(plan);

    REQUIRE(ids_of(collect_query_result(lancedb_query_execute(query)))
        == std::set<int32_t>{0});
  }
}

TEST_CASE_METHOD(PhraseFtsFixture, "LanceDB Query - structured phrase invalid input", "[fts]") {
  SECTION("rejects NULL and invalid UTF-8 text and clears the output") {
    auto* valid = make_phrase_query("alpha beta", "text", 0);
    LanceDBFtsQuery* fts_query = valid;
    char* error_message = nullptr;
    REQUIRE(lancedb_fts_query_new_phrase(nullptr, "text", 0, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_fts_query_free(valid);

    const char invalid_utf8[] = {static_cast<char>(0xff), '\0'};
    fts_query = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_fts_query_new_phrase(
        invalid_utf8, "text", 0, &fts_query, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    fts_query = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_fts_query_new_phrase(
        "alpha beta", invalid_utf8, 0, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("a failed setter keeps the previous phrase") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    auto* fts_query = make_phrase_query("gamma", "text", 0);
    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search_query(query, fts_query, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_fts_query_free(fts_query);

    error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search_query(query, nullptr, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    // If the failed setter dropped the phrase, this would be a plain scan and
    // return all three fixture rows.
    auto result = collect_query_result(lancedb_query_execute(query));
    REQUIRE(result.num_rows() == 0);
  }

  SECTION("rejects a NULL builder and the free function accepts NULL") {
    auto* fts_query = make_phrase_query("alpha beta", "text", 0);
    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search_query(nullptr, fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_fts_query_free(fts_query);
    lancedb_fts_query_free(nullptr);
  }
}

TEST_CASE_METHOD(PhraseFtsFixture, "LanceDB Query - structured match full text", "[fts]") {
  SECTION("the default operator matches any term") {
    auto result = run_match_query(table(), "alpha gap", "text");
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 2});
  }

  SECTION("AND requires every term") {
    auto options = lancedb_fts_match_options_default();
    options.term_operator = LANCEDB_FTS_OPERATOR_AND;

    auto result = run_match_query(table(), "alpha gap", "text", &options);
    REQUIRE(ids_of(result) == std::set<int32_t>{1});
  }

  SECTION("boost scales the full-text score") {
    auto baseline = run_match_query(table(), "alpha", "text");

    auto options = lancedb_fts_match_options_default();
    options.boost = 2.0F;
    auto boosted = run_match_query(table(), "alpha", "text", &options);

    for (const int32_t id : {0, 1, 2}) {
      REQUIRE(std::abs(float_column_of(boosted, "_score", id) -
                       2.0F * float_column_of(baseline, "_score", id)) < 1e-5F);
    }
  }

  SECTION("prefix_length keeps the leading characters exact") {
    auto options = lancedb_fts_match_options_default();
    options.fuzziness = 1;

    auto fuzzy = run_match_query(table(), "xlpha", "text", &options);
    REQUIRE(ids_of(fuzzy) == std::set<int32_t>{0, 1, 2});

    options.prefix_length = 1;
    auto prefixed = run_match_query(table(), "xlpha", "text", &options);
    REQUIRE(prefixed.num_rows() == 0);
  }

  SECTION("explicit distance 1 matches a typo that the exact default rejects") {
    auto exact = run_match_query(table(), "alpa", "text");
    REQUIRE(exact.num_rows() == 0);

    auto options = lancedb_fts_match_options_default();
    REQUIRE(options.fuzziness == 0);
    options.fuzziness = 1;

    auto fuzzy = run_match_query(table(), "alpa", "text", &options);
    REQUIRE(ids_of(fuzzy) == std::set<int32_t>{0, 1, 2});

    options.max_expansions = 0;
    auto no_expansions = run_match_query(table(), "alpa", "text", &options);
    REQUIRE(no_expansions.num_rows() == 0);

    options.max_expansions = 1;
    auto one_expansion = run_match_query(table(), "alpa", "text", &options);
    REQUIRE(ids_of(one_expansion) == std::set<int32_t>{0, 1, 2});
  }

  SECTION("NULL options and the default helper agree") {
    auto defaults = lancedb_fts_match_options_default();
    REQUIRE(defaults.fuzziness == 0);
    REQUIRE(defaults.boost == 1.0F);
    REQUIRE(defaults.max_expansions == 50);
    REQUIRE(defaults.term_operator == LANCEDB_FTS_OPERATOR_OR);
    REQUIRE(defaults.prefix_length == 0);

    auto null_options = run_match_query(table(), "alpa", "text");
    auto explicit_defaults = run_match_query(table(), "alpa", "text", &defaults);
    REQUIRE(null_options.num_rows() == 0);
    REQUIRE(explicit_defaults.num_rows() == 0);
  }

  SECTION("an empty column uses the indexed text column") {
    auto result = run_match_query(table(), "alpha gap", "");
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 2});
  }

  SECTION("the constructor copies its inputs and the setters clone the handle") {
    char text[] = "alpha gap";
    char column[] = "text";
    auto options = lancedb_fts_match_options_default();
    options.term_operator = LANCEDB_FTS_OPERATOR_AND;

    auto* fts_query = make_match_query(text, column, &options);
    text[0] = 'z';
    column[0] = 'x';

    LanceDBQuery* first = lancedb_query_new(table());
    LanceDBQuery* second = lancedb_query_new(table());
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search_query(first, fts_query, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_full_text_search_query(second, fts_query, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_fts_query_free(fts_query);

    REQUIRE(ids_of(collect_query_result(lancedb_query_execute(first)))
        == std::set<int32_t>{1});
    REQUIRE(ids_of(collect_query_result(lancedb_query_execute(second)))
        == std::set<int32_t>{1});
  }
}

TEST_CASE_METHOD(PhraseFtsFixture, "LanceDB Query - structured match invalid input", "[fts]") {
  SECTION("rejects an out-of-range operator and clears the output") {
    auto* valid = make_match_query("alpha", "text");
    LanceDBFtsQuery* fts_query = valid;
    char* error_message = nullptr;
    REQUIRE(lancedb_test_fts_query_new_match_with_operator(
        "alpha", "text", 2, &fts_query, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    lancedb_fts_query_free(valid);
  }

  SECTION("rejects fuzziness outside -1 through UINT32_MAX") {
    auto options = lancedb_fts_match_options_default();
    char* error_message = nullptr;

    options.fuzziness = -2;
    LanceDBFtsQuery* fts_query = nullptr;
    REQUIRE(lancedb_fts_query_new_match(
        "alpha", "text", &options, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    options.fuzziness = 4294967296LL;
    error_message = nullptr;
    REQUIRE(lancedb_fts_query_new_match(
        "alpha", "text", &options, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    options.fuzziness = -1;
    error_message = nullptr;
    REQUIRE(lancedb_fts_query_new_match(
        "alpha", "text", &options, &fts_query, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(fts_query != nullptr);
    REQUIRE(error_message == nullptr);
    lancedb_fts_query_free(fts_query);

    options.fuzziness = UINT32_MAX;
    fts_query = nullptr;
    error_message = nullptr;
    REQUIRE(lancedb_fts_query_new_match(
        "alpha", "text", &options, &fts_query, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(fts_query != nullptr);
    REQUIRE(error_message == nullptr);
    lancedb_fts_query_free(fts_query);
  }

  SECTION("rejects NULL and invalid UTF-8 text and clears the output") {
    char* error_message = nullptr;
    LanceDBFtsQuery* fts_query = nullptr;
    REQUIRE(lancedb_fts_query_new_match(
        nullptr, "text", nullptr, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    const char invalid_utf8[] = {static_cast<char>(0xff), '\0'};
    error_message = nullptr;
    REQUIRE(lancedb_fts_query_new_match(
        invalid_utf8, "text", nullptr, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }
}

TEST_CASE_METHOD(FtsFixture, "LanceDB Query - phrase EXPLAIN requires indexed positions",
                 "[fts]") {
  LanceDBQuery* query = lancedb_query_new(table());
  REQUIRE(query != nullptr);

  auto* fts_query = make_phrase_query("common doc7", "text", 0);
  char* error_message = nullptr;
  REQUIRE(lancedb_query_full_text_search_query(query, fts_query, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  lancedb_fts_query_free(fts_query);

  char* plan = nullptr;
  REQUIRE(lancedb_query_explain_plan(query, false, &plan, &error_message)
      == LANCEDB_INVALID_INPUT);
  REQUIRE(plan == nullptr);
  REQUIRE(error_message != nullptr);
  REQUIRE(std::strstr(error_message, "position is not found") != nullptr);
  lancedb_free_string(error_message);

  // EXPLAIN does not consume the query.
  lancedb_query_free(query);
}

TEST_CASE_METHOD(PhraseFtsFixture, "LanceDB Vector Query - structured phrase hybrid",
                 "[fts][vector_query]") {
  const auto query_vector = vector_for_row(2);

  auto run_hybrid = [&]() {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 3, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto* fts_query = make_phrase_query("alpha beta", "text", 1);
    REQUIRE(lancedb_vector_query_full_text_search_query(query, fts_query, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_fts_query_free(fts_query);

    return collect_query_result(lancedb_vector_query_execute(query));
  };

  const auto vector_ids = ordered_ids_of(run_vector_leg_query(table(), query_vector, 3));
  const auto fts_result = run_phrase_query(table(), "alpha beta", "text", 1, 3);
  const auto fts_ids = ordered_ids_of(fts_result);
  REQUIRE(vector_ids == std::vector<int32_t>{2, 1, 0});
  REQUIRE(std::set<int32_t>(fts_ids.begin(), fts_ids.end()) == std::set<int32_t>{0, 1});

  auto result = run_hybrid();
  REQUIRE(result.num_rows() == 3);
  REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 2});

  // No explicit reranker is set, so the SDK's default RRF k = 60 applies.
  const auto expected = expected_rrf_scores(vector_ids, fts_ids, kDefaultRrfK);
  for (const auto& [id, score] : expected) {
    REQUIRE(std::abs(relevance_score_of(result, id) - score) < 1e-6F);
  }

  // Rows 0 and 1 appear in both legs; row 2 only appears in the vector leg.
  REQUIRE(relevance_score_of(result, 0) > 1.0F / (kDefaultRrfK + 1.0F));
  REQUIRE(relevance_score_of(result, 1) > 1.0F / (kDefaultRrfK + 1.0F));
}

TEST_CASE_METHOD(FtsFixture, "LanceDB Vector Query - structured match hybrid", "[fts][vector_query]") {
  auto options = lancedb_fts_match_options_default();
  options.term_operator = LANCEDB_FTS_OPERATOR_AND;

  const auto fts_result = run_match_query(table(), "common doc7", "text", &options);
  REQUIRE(ids_of(fts_result) == std::set<int32_t>{7});

  const auto query_vector = vector_for_row(0);
  LanceDBVectorQuery* query = lancedb_vector_query_new(
      table(), query_vector.data(), query_vector.size());
  REQUIRE(query != nullptr);

  char* error_message = nullptr;
  REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_vector_query_limit(query, 2, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  auto* fts_query = make_match_query("common doc7", "text", &options);
  REQUIRE(lancedb_vector_query_full_text_search_query(query, fts_query, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  lancedb_fts_query_free(fts_query);

  auto result = collect_query_result(lancedb_vector_query_execute(query));
  REQUIRE(result.num_rows() == 2);
  REQUIRE(ids_of(result) == std::set<int32_t>{0, 7});
  REQUIRE(result.schema->GetFieldByName("_relevance_score") != nullptr);
}

TEST_CASE_METHOD(FtsFixture, "LanceDB Query - full text ordering", "[fts][order_by]") {
  const auto query_vector = vector_for_row(0);

  SECTION("orders all matches before selecting the search page") {
    LanceDBQuery* query = lancedb_query_new(table());
    REQUIRE(query != nullptr);

    LanceDBColumnOrdering orderings[] = {{"id", /*ascending=*/0, /*nulls_first=*/0}};
    char* error_message = nullptr;
    REQUIRE(lancedb_query_limit(query, 3, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_offset(query, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_full_text_search(query, "common", "text", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_order_by(query, orderings, 1, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_query_execute(query));
    // All 24 rows match. The ordering sorts every match before limit and offset
    // select the page, so the result is 22, 21, 20 rather than a sorted top-K.
    REQUIRE(ordered_ids_of(result) == std::vector<int32_t>{22, 21, 20});
  }

  SECTION("applies the ordering to both hybrid search legs") {
    LanceDBVectorQuery* query = lancedb_vector_query_new(
        table(), query_vector.data(), query_vector.size());
    REQUIRE(query != nullptr);

    LanceDBColumnOrdering orderings[] = {{"id", /*ascending=*/0, /*nulls_first=*/0}};
    char* error_message = nullptr;
    REQUIRE(lancedb_vector_query_column(query, "data", &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_limit(query, 48, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_full_text_search(query, "common", "text", &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_vector_query_order_by(query, orderings, 1, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = collect_query_result(lancedb_vector_query_execute(query));
    // The limit covers every row, so both legs contribute all 24 rows. Applying
    // the same descending ordering to both legs makes row 23 the strongest
    // result and row 0 the weakest; ordering only one leg would interleave them.
    std::vector<int32_t> expected_ids;
    for (int32_t id = kRowCount - 1; id >= 0; id--) {
      expected_ids.push_back(id);
    }
    REQUIRE(ordered_ids_of(result) == expected_ids);
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

  auto run_vector_leg = [&]() { return run_vector_leg_query(table(), query_vector, limit); };

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
    require_matches(run_hybrid(std::nullopt, std::nullopt), kDefaultRrfK);
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
