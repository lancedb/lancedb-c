/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

std::shared_ptr<arrow::Schema> create_multi_match_test_schema() {
  return arrow::schema({
      arrow::field("id", arrow::int32()),
      arrow::field("title", arrow::utf8()),
      arrow::field("body", arrow::utf8()),
  });
}

// The two text columns have identical term and length distributions, so a
// shared term scores the same in either field. Rows 1 and 2 isolate the two
// fields, row 3 matches both terms in both fields, and row 4 matches neither.
std::shared_ptr<arrow::RecordBatch> create_multi_match_test_record_batch() {
  auto schema = create_multi_match_test_schema();
  const std::vector<std::pair<std::string, std::string>> texts = {
      {"alpha", "alpha"},
      {"alpha", "beta"},
      {"beta", "alpha"},
      {"alpha beta", "alpha beta"},
      {"gamma delta", "gamma delta"},
  };

  arrow::Int32Builder id_builder;
  arrow::StringBuilder title_builder;
  arrow::StringBuilder body_builder;
  for (size_t i = 0; i < texts.size(); i++) {
    REQUIRE(id_builder.Append(static_cast<int32_t>(i)).ok());
    REQUIRE(title_builder.Append(texts[i].first).ok());
    REQUIRE(body_builder.Append(texts[i].second).ok());
  }

  std::shared_ptr<arrow::Array> id_array, title_array, body_array;
  REQUIRE(id_builder.Finish(&id_array).ok());
  REQUIRE(title_builder.Finish(&title_array).ok());
  REQUIRE(body_builder.Finish(&body_array).ok());

  return arrow::RecordBatch::Make(
      schema, static_cast<int64_t>(texts.size()), {id_array, title_array, body_array});
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

class MultiMatchFtsFixture : public LanceDBFixture {
public:
  MultiMatchFtsFixture() : table_(create_table()) {
    for (const char* column : {"title", "body"}) {
      char* error_message = nullptr;
      REQUIRE(lancedb_table_create_fts_index(
          table_.get(), &column, 1, nullptr, &error_message) == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
    }
  }

  LanceDBTable* table() const {
    return table_.get();
  }

private:
  LanceDBTablePtr create_table() {
    auto schema = create_multi_match_test_schema();
    auto batch = create_multi_match_test_record_batch();
    auto reader = create_reader_from_batch(batch);
    REQUIRE(reader != nullptr);

    struct ArrowSchema c_schema;
    REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

    LanceDBTable* raw_table = nullptr;
    char* error_message = nullptr;
    const auto status = lancedb_table_create(
        db,
        "fts_multi_match_test",
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

LanceDBFtsQuery* make_boolean_query(const std::vector<LanceDBFtsClause>& clauses) {
  LanceDBFtsQuery* fts_query = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_fts_query_new_boolean(
      clauses.empty() ? nullptr : clauses.data(), clauses.size(), &fts_query,
      &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(fts_query != nullptr);
  return fts_query;
}

LanceDBFtsQuery* make_boost_query(
    const LanceDBFtsQuery* positive,
    const LanceDBFtsQuery* negative,
    const float* negative_boost = nullptr) {
  LanceDBFtsQuery* fts_query = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_fts_query_new_boost(
      positive, negative, negative_boost, &fts_query, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(fts_query != nullptr);
  return fts_query;
}

// Run one already-built structured query. The handle is borrowed, so the caller
// keeps ownership and decides when to free it.
QueryResultBatches run_structured_query(
    LanceDBTable* table,
    LanceDBFtsQuery* fts_query,
    size_t limit = 10) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);
  char* error_message = nullptr;
  REQUIRE(lancedb_query_full_text_search_query(query, fts_query, &error_message)
      == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_query_limit(query, limit, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  return collect_query_result(lancedb_query_execute(query));
}

LanceDBFtsQuery* make_multi_match_query(
    const char* text,
    const std::vector<std::string>& columns,
    const std::vector<float>& boosts = {},
    LanceDBFtsOperator term_operator = LANCEDB_FTS_OPERATOR_OR) {
  REQUIRE(!columns.empty());
  std::vector<const char*> column_names;
  column_names.reserve(columns.size());
  for (const auto& column : columns) {
    column_names.push_back(column.c_str());
  }

  auto options = lancedb_fts_multi_match_options_default();
  options.term_operator = term_operator;
  if (!boosts.empty()) {
    options.boosts = boosts.data();
    options.num_boosts = boosts.size();
  }

  LanceDBFtsQuery* fts_query = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_fts_query_new_multi_match(
      text,
      column_names.data(),
      column_names.size(),
      &options,
      &fts_query,
      &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(fts_query != nullptr);
  return fts_query;
}

QueryResultBatches run_multi_match_query(
    LanceDBTable* table,
    const char* text,
    const std::vector<std::string>& columns,
    const std::vector<float>& boosts = {},
    LanceDBFtsOperator term_operator = LANCEDB_FTS_OPERATOR_OR,
    size_t limit = 10) {
  auto* fts_query = make_multi_match_query(text, columns, boosts, term_operator);
  auto result = run_structured_query(table, fts_query, limit);
  lancedb_fts_query_free(fts_query);
  return result;
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

TEST_CASE_METHOD(MultiMatchFtsFixture, "LanceDB Query - structured multi-match full text",
                 "[fts]") {
  const std::vector<std::string> columns = {"title", "body"};

  SECTION("returns each matching row once with the best field score") {
    auto result = run_multi_match_query(table(), "alpha", columns);
    REQUIRE(result.num_rows() == 4);
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 2, 3});

    // Row 0 matches in both fields, row 1 only in title, and row 2 only in body.
    // Their fields have identical term and length distributions, so the best
    // field score equals the single matching field score; summing the fields
    // would make row 0 twice as large.
    REQUIRE(std::abs(float_column_of(result, "_score", 0) -
                     float_column_of(result, "_score", 1)) < 1e-5F);
    REQUIRE(std::abs(float_column_of(result, "_score", 0) -
                     float_column_of(result, "_score", 2)) < 1e-5F);
  }

  SECTION("uses the SDK defaults when options is NULL") {
    const char* column_names[] = {"title", "body"};
    LanceDBFtsQuery* fts_query = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_fts_query_new_multi_match(
        "alpha", column_names, 2, nullptr, &fts_query, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(fts_query != nullptr);

    auto result = run_structured_query(table(), fts_query);
    lancedb_fts_query_free(fts_query);
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 2, 3});
  }

  SECTION("applies boosts in column order") {
    auto title_boosted = run_multi_match_query(
        table(), "alpha", columns, {2.0F, 1.0F});
    auto body_boosted = run_multi_match_query(
        table(), "alpha", columns, {1.0F, 2.0F});

    REQUIRE(float_column_of(title_boosted, "_score", 1) >
            float_column_of(title_boosted, "_score", 2));
    REQUIRE(float_column_of(body_boosted, "_score", 2) >
            float_column_of(body_boosted, "_score", 1));
  }

  SECTION("applies the shared operator to every column") {
    auto any = run_multi_match_query(table(), "alpha beta", columns);
    REQUIRE(ids_of(any) == std::set<int32_t>{0, 1, 2, 3});

    auto all = run_multi_match_query(
        table(), "alpha beta", columns, {}, LANCEDB_FTS_OPERATOR_AND);
    REQUIRE(ids_of(all) == std::set<int32_t>{3});
  }
}

TEST_CASE_METHOD(MultiMatchFtsFixture, "LanceDB Query - structured multi-match invalid input",
                 "[fts]") {
  const char* columns[] = {"title", "body"};
  const char invalid_utf8[] = {static_cast<char>(0xff), '\0'};
  char* error_message = nullptr;
  LanceDBFtsQuery* fts_query = nullptr;

  SECTION("rejects missing text, columns, output and boosts") {
    REQUIRE(lancedb_fts_multi_match_options_default().term_operator ==
            LANCEDB_FTS_OPERATOR_OR);

    REQUIRE(lancedb_fts_query_new_multi_match(
        nullptr, columns, 2, nullptr, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_fts_query_new_multi_match(
        "alpha", nullptr, 2, nullptr, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const char* null_column[] = {"title", nullptr};
    REQUIRE(lancedb_fts_query_new_multi_match(
        "alpha", null_column, 2, nullptr, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_fts_query_new_multi_match(
        "alpha", columns, 2, nullptr, nullptr, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    auto options = lancedb_fts_multi_match_options_default();
    options.num_boosts = 1;
    REQUIRE(lancedb_fts_query_new_multi_match(
        "alpha", columns, 2, &options, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("rejects invalid UTF-8 and out-of-range operators") {
    REQUIRE(lancedb_fts_query_new_multi_match(
        invalid_utf8, columns, 2, nullptr, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const char* invalid_columns[] = {invalid_utf8, "body"};
    REQUIRE(lancedb_fts_query_new_multi_match(
        "alpha", invalid_columns, 2, nullptr, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_test_fts_query_new_multi_match_with_operator(
        "alpha", columns, 2, 2, &fts_query, &error_message)
        == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("maps SDK constructor errors to invalid input") {
    REQUIRE(lancedb_fts_query_new_multi_match(
        "alpha", nullptr, 0, nullptr, &fts_query, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::strstr(error_message, "no columns") != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    const float one_boost = 1.0F;
    auto options = lancedb_fts_multi_match_options_default();
    options.boosts = &one_boost;
    options.num_boosts = 1;
    REQUIRE(lancedb_fts_query_new_multi_match(
        "alpha", columns, 2, &options, &fts_query, &error_message)
        == LANCEDB_INVALID_INPUT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::strstr(error_message, "number of boosts") != nullptr);
    lancedb_free_string(error_message);
  }

  SECTION("non-finite or negative boosts fail when executed") {
    for (const float boost : {-1.0F, std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity()}) {
      const float boosts[] = {boost, 1.0F};
      auto options = lancedb_fts_multi_match_options_default();
      options.boosts = boosts;
      options.num_boosts = 2;
      REQUIRE(lancedb_fts_query_new_multi_match(
          "alpha", columns, 2, &options, &fts_query, &error_message)
          == LANCEDB_SUCCESS);
      REQUIRE(fts_query != nullptr);
      REQUIRE(error_message == nullptr);

      LanceDBQuery* query = lancedb_query_new(table());
      REQUIRE(query != nullptr);
      REQUIRE(lancedb_query_full_text_search_query(query, fts_query, &error_message)
          == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);
      lancedb_fts_query_free(fts_query);
      fts_query = nullptr;

      LanceDBQueryResult* result = nullptr;
      error_message = nullptr;
      REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message)
          == LANCEDB_INVALID_INPUT);
      REQUIRE(result == nullptr);
      REQUIRE(error_message != nullptr);
      REQUIRE(std::strstr(error_message, "MultiMatchQuery boost") != nullptr);
      lancedb_free_string(error_message);
      error_message = nullptr;
      // execute_with_status consumes the builder on every path.
    }
  }
}

TEST_CASE_METHOD(MultiMatchFtsFixture,
                 "LanceDB Query - structured FTS column selection", "[fts]") {
  SECTION("replaces a Match column with several fields") {
    auto options = lancedb_fts_match_options_default();
    options.boost = 2.0F;
    auto* fts_query = make_match_query("alpha", "missing", &options);
    const char* columns[] = {"title", "body"};
    char* error_message = nullptr;
    REQUIRE(lancedb_fts_query_with_columns(fts_query, columns, 2, &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto baseline = run_multi_match_query(
        table(), "alpha", {"title", "body"});
    auto result = run_structured_query(table(), fts_query);
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 2, 3});
    for (const int32_t id : {0, 1, 2, 3}) {
      REQUIRE(std::abs(float_column_of(result, "_score", id) -
                       2.0F * float_column_of(baseline, "_score", id)) <
              1e-5F);
    }
    lancedb_fts_query_free(fts_query);
  }

  SECTION("rewrites nested Boolean children instead of using automatic discovery") {
    auto* child = make_match_query("alpha", nullptr);
    std::vector<LanceDBFtsClause> clauses = {
        {child, LANCEDB_FTS_OCCUR_MUST},
    };
    auto* boolean = make_boolean_query(clauses);
    lancedb_fts_query_free(child);

    // Without a column, the Boolean resolves against every indexed text field.
    REQUIRE(run_structured_query(table(), boolean).num_rows() == 4);

    const char* columns[] = {"title"};
    char* error_message = nullptr;
    REQUIRE(lancedb_fts_query_with_columns(boolean, columns, 1, &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);

    auto result = run_structured_query(table(), boolean);
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 3});
    lancedb_fts_query_free(boolean);
  }
}

TEST_CASE_METHOD(PhraseFtsFixture,
                 "LanceDB Query - structured FTS column selection failure", "[fts]") {
  SECTION("a Phrase accepts exactly one selected column") {
    auto* fts_query = make_phrase_query("alpha beta", "text", 0);
    const char* columns[] = {"text", "text"};
    char* error_message = nullptr;
    REQUIRE(lancedb_fts_query_with_columns(fts_query, columns, 2, &error_message) ==
            LANCEDB_INVALID_INPUT);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::strstr(error_message, "column must be specified") != nullptr);
    lancedb_free_string(error_message);

    // The rejected rewrite leaves the previous phrase in place.
    auto result = run_structured_query(table(), fts_query);
    REQUIRE(ids_of(result) == std::set<int32_t>{0});
    lancedb_fts_query_free(fts_query);
  }

  SECTION("an empty selection fails atomically") {
    auto* fts_query = make_match_query("alpha", "text");
    char* error_message = nullptr;
    REQUIRE(lancedb_fts_query_with_columns(fts_query, nullptr, 0, &error_message) ==
            LANCEDB_INVALID_INPUT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    auto result = run_structured_query(table(), fts_query);
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 2});
    lancedb_fts_query_free(fts_query);
  }

  SECTION("invalid C inputs leave the previous query usable") {
    auto* fts_query = make_match_query("alpha", "text");
    char* error_message = nullptr;
    REQUIRE(lancedb_fts_query_with_columns(fts_query, nullptr, 1, &error_message) ==
            LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    const char invalid_utf8[] = {static_cast<char>(0xff), '\0'};
    const char* columns[] = {invalid_utf8};
    error_message = nullptr;
    REQUIRE(lancedb_fts_query_with_columns(fts_query, columns, 1, &error_message) ==
            LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    auto result = run_structured_query(table(), fts_query);
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 2});
    lancedb_fts_query_free(fts_query);
  }
}

TEST_CASE_METHOD(PhraseFtsFixture, "LanceDB Query - structured boolean full text", "[fts]") {
  SECTION("MUST requires every clause") {
    auto* alpha = make_match_query("alpha", "text");
    auto* gap = make_match_query("gap", "text");
    std::vector<LanceDBFtsClause> clauses = {
        {alpha, LANCEDB_FTS_OCCUR_MUST},
        {gap, LANCEDB_FTS_OCCUR_MUST},
    };
    auto* boolean = make_boolean_query(clauses);
    lancedb_fts_query_free(alpha);
    lancedb_fts_query_free(gap);

    auto result = run_structured_query(table(), boolean);
    lancedb_fts_query_free(boolean);
    REQUIRE(ids_of(result) == std::set<int32_t>{1});
  }

  SECTION("the constructor copies borrowed children before they are freed") {
    auto* alpha = make_match_query("alpha", "text");
    auto* beta = make_match_query("beta", "text");
    std::vector<LanceDBFtsClause> clauses = {
        {alpha, LANCEDB_FTS_OCCUR_MUST},
        {beta, LANCEDB_FTS_OCCUR_MUST},
    };
    auto* boolean = make_boolean_query(clauses);
    lancedb_fts_query_free(alpha);
    lancedb_fts_query_free(beta);

    LanceDBQuery* first = lancedb_query_new(table());
    LanceDBQuery* second = lancedb_query_new(table());
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    char* error_message = nullptr;
    REQUIRE(lancedb_query_full_text_search_query(first, boolean, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_limit(first, 10, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_full_text_search_query(second, boolean, &error_message)
        == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_query_limit(second, 10, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    lancedb_fts_query_free(boolean);

    REQUIRE(ids_of(collect_query_result(lancedb_query_execute(first)))
        == std::set<int32_t>{0, 1, 2});
    REQUIRE(ids_of(collect_query_result(lancedb_query_execute(second)))
        == std::set<int32_t>{0, 1, 2});
  }

  SECTION("an empty or MUST_NOT-only query constructs but fails when executed") {
    auto* alpha = make_match_query("alpha", "text");
    std::vector<LanceDBFtsClause> must_not_only = {
        {alpha, LANCEDB_FTS_OCCUR_MUST_NOT},
    };
    auto* empty = make_boolean_query({});
    auto* negative = make_boolean_query(must_not_only);
    lancedb_fts_query_free(alpha);

    const std::pair<LanceDBFtsQuery*, const char*> cases[] = {
        {empty, "at least one leaf query"},
        {negative, "at least one should/must query"},
    };
    for (const auto& [fts_query, expected_message] : cases) {
      LanceDBQuery* query = lancedb_query_new(table());
      REQUIRE(query != nullptr);
      char* error_message = nullptr;
      REQUIRE(lancedb_query_full_text_search_query(query, fts_query, &error_message)
          == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);

      LanceDBQueryResult* result = nullptr;
      error_message = nullptr;
      REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message)
          == LANCEDB_INVALID_INPUT);
      REQUIRE(result == nullptr);
      REQUIRE(error_message != nullptr);
      REQUIRE(std::strstr(error_message, expected_message) != nullptr);
      lancedb_free_string(error_message);
    }
    lancedb_fts_query_free(empty);
    lancedb_fts_query_free(negative);
  }

  SECTION("SHOULD sums the matching clauses") {
    auto* alpha = make_match_query("alpha", "text");
    auto* gap = make_match_query("gap", "text");
    std::vector<LanceDBFtsClause> alpha_only = {{alpha, LANCEDB_FTS_OCCUR_SHOULD}};
    std::vector<LanceDBFtsClause> gap_only = {{gap, LANCEDB_FTS_OCCUR_SHOULD}};
    std::vector<LanceDBFtsClause> both = {
        {alpha, LANCEDB_FTS_OCCUR_SHOULD},
        {gap, LANCEDB_FTS_OCCUR_SHOULD},
    };
    auto* alpha_query = make_boolean_query(alpha_only);
    auto* gap_query = make_boolean_query(gap_only);
    auto* both_query = make_boolean_query(both);
    lancedb_fts_query_free(alpha);
    lancedb_fts_query_free(gap);

    auto alpha_result = run_structured_query(table(), alpha_query);
    auto gap_result = run_structured_query(table(), gap_query);
    auto both_result = run_structured_query(table(), both_query);
    lancedb_fts_query_free(alpha_query);
    lancedb_fts_query_free(gap_query);
    lancedb_fts_query_free(both_query);

    REQUIRE(ids_of(both_result) == std::set<int32_t>{0, 1, 2});
    REQUIRE(ids_of(gap_result) == std::set<int32_t>{1});
    for (const int32_t id : {0, 2}) {
      REQUIRE(std::abs(float_column_of(both_result, "_score", id) -
                       float_column_of(alpha_result, "_score", id)) < 1e-5F);
    }
    // Row 1 matches both SHOULD clauses, so both scores contribute.
    REQUIRE(std::abs(float_column_of(both_result, "_score", 1) -
                     (float_column_of(alpha_result, "_score", 1) +
                      float_column_of(gap_result, "_score", 1))) < 1e-5F);
  }

  SECTION("MUST sums the clause scores") {
    auto* alpha = make_match_query("alpha", "text");
    auto* beta = make_match_query("beta", "text");
    std::vector<LanceDBFtsClause> alpha_only = {{alpha, LANCEDB_FTS_OCCUR_MUST}};
    std::vector<LanceDBFtsClause> beta_only = {{beta, LANCEDB_FTS_OCCUR_MUST}};
    std::vector<LanceDBFtsClause> both = {
        {alpha, LANCEDB_FTS_OCCUR_MUST},
        {beta, LANCEDB_FTS_OCCUR_MUST},
    };
    auto* alpha_query = make_boolean_query(alpha_only);
    auto* beta_query = make_boolean_query(beta_only);
    auto* both_query = make_boolean_query(both);
    lancedb_fts_query_free(alpha);
    lancedb_fts_query_free(beta);

    auto alpha_result = run_structured_query(table(), alpha_query);
    auto beta_result = run_structured_query(table(), beta_query);
    auto both_result = run_structured_query(table(), both_query);
    lancedb_fts_query_free(alpha_query);
    lancedb_fts_query_free(beta_query);
    lancedb_fts_query_free(both_query);

    REQUIRE(ids_of(both_result) == std::set<int32_t>{0, 1, 2});
    for (const int32_t id : {0, 1, 2}) {
      REQUIRE(std::abs(float_column_of(both_result, "_score", id) -
                       (float_column_of(alpha_result, "_score", id) +
                        float_column_of(beta_result, "_score", id))) < 1e-5F);
    }
  }

  SECTION("MUST_NOT excludes documents without contributing score") {
    auto* alpha = make_match_query("alpha", "text");
    auto* gap = make_match_query("gap", "text");
    std::vector<LanceDBFtsClause> alpha_only = {{alpha, LANCEDB_FTS_OCCUR_SHOULD}};
    std::vector<LanceDBFtsClause> excluded = {
        {alpha, LANCEDB_FTS_OCCUR_SHOULD},
        {gap, LANCEDB_FTS_OCCUR_MUST_NOT},
    };
    auto* alpha_query = make_boolean_query(alpha_only);
    auto* excluded_query = make_boolean_query(excluded);
    lancedb_fts_query_free(alpha);
    lancedb_fts_query_free(gap);

    auto alpha_result = run_structured_query(table(), alpha_query);
    auto excluded_result = run_structured_query(table(), excluded_query);
    lancedb_fts_query_free(alpha_query);
    lancedb_fts_query_free(excluded_query);

    REQUIRE(ids_of(excluded_result) == std::set<int32_t>{0, 2});
    for (const int32_t id : {0, 2}) {
      REQUIRE(std::abs(float_column_of(excluded_result, "_score", id) -
                       float_column_of(alpha_result, "_score", id)) < 1e-5F);
    }
  }

  SECTION("an empty child column is resolved against the indexed column") {
    auto* alpha = make_match_query("alpha", "");
    auto* gap = make_match_query("gap", "");
    std::vector<LanceDBFtsClause> clauses = {
        {alpha, LANCEDB_FTS_OCCUR_MUST},
        {gap, LANCEDB_FTS_OCCUR_MUST},
    };
    auto* boolean = make_boolean_query(clauses);
    lancedb_fts_query_free(alpha);
    lancedb_fts_query_free(gap);

    auto result = run_structured_query(table(), boolean);
    lancedb_fts_query_free(boolean);
    REQUIRE(ids_of(result) == std::set<int32_t>{1});
  }
}

TEST_CASE_METHOD(PhraseFtsFixture, "LanceDB Query - structured boolean invalid input", "[fts]") {
  SECTION("rejects an out-of-range occurrence and keeps the child valid") {
    auto* valid = make_match_query("alpha", "text");
    LanceDBFtsQuery* fts_query = valid;
    char* error_message = nullptr;
    REQUIRE(lancedb_test_fts_query_new_boolean_with_occur(
        valid, 3, &fts_query, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    REQUIRE(std::strstr(error_message, "LanceDBFtsOccur") != nullptr);
    lancedb_free_string(error_message);

    // The failed constructor borrowed the child and must leave it usable.
    std::vector<LanceDBFtsClause> clauses = {{valid, LANCEDB_FTS_OCCUR_MUST}};
    auto* boolean = make_boolean_query(clauses);
    lancedb_fts_query_free(valid);
    auto result = run_structured_query(table(), boolean);
    lancedb_fts_query_free(boolean);
    REQUIRE(ids_of(result) == std::set<int32_t>{0, 1, 2});
  }

  SECTION("rejects NULL clause arrays and NULL child handles") {
    auto* valid = make_match_query("alpha", "text");
    LanceDBFtsQuery* fts_query = nullptr;
    char* error_message = nullptr;

    REQUIRE(lancedb_fts_query_new_boolean(
        nullptr, 1, &fts_query, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    const LanceDBFtsClause null_clause = {nullptr, LANCEDB_FTS_OCCUR_MUST};
    error_message = nullptr;
    REQUIRE(lancedb_fts_query_new_boolean(
        &null_clause, 1, &fts_query, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_fts_query_free(valid);
  }
}

TEST_CASE_METHOD(PhraseFtsFixture, "LanceDB Query - structured boost full text", "[fts]") {
  SECTION("the positive leg gates membership and the multiplier demotes") {
    auto* alpha = make_match_query("alpha", "text");
    auto* gap = make_match_query("gap", "text");

    // The positive leg decides membership: only the row containing gap matches.
    auto* gated = make_boost_query(gap, alpha, nullptr);
    auto gated_result = run_structured_query(table(), gated);
    lancedb_fts_query_free(gated);
    REQUIRE(ids_of(gated_result) == std::set<int32_t>{1});

    const float zero = 0.0F;
    const float half = 0.5F;
    const float one = 1.0F;
    auto* none = make_boost_query(alpha, gap, &zero);
    auto* defaulted = make_boost_query(alpha, gap, nullptr);
    auto* explicit_half = make_boost_query(alpha, gap, &half);
    auto* full = make_boost_query(alpha, gap, &one);
    lancedb_fts_query_free(alpha);
    lancedb_fts_query_free(gap);

    auto none_result = run_structured_query(table(), none);
    auto defaulted_result = run_structured_query(table(), defaulted);
    auto half_result = run_structured_query(table(), explicit_half);
    auto full_result = run_structured_query(table(), full);
    lancedb_fts_query_free(none);
    lancedb_fts_query_free(defaulted);
    lancedb_fts_query_free(explicit_half);
    lancedb_fts_query_free(full);

    REQUIRE(ids_of(none_result) == std::set<int32_t>{0, 1, 2});
    for (const int32_t id : {0, 1, 2}) {
      REQUIRE(std::abs(float_column_of(defaulted_result, "_score", id) -
                       float_column_of(half_result, "_score", id)) < 1e-5F);
    }
    for (const int32_t id : {0, 2}) {
      REQUIRE(std::abs(float_column_of(none_result, "_score", id) -
                       float_column_of(full_result, "_score", id)) < 1e-5F);
    }
    const float none_score = float_column_of(none_result, "_score", 1);
    const float half_score = float_column_of(half_result, "_score", 1);
    const float full_score = float_column_of(full_result, "_score", 1);
    REQUIRE(half_score < none_score);
    REQUIRE(full_score < half_score);
    REQUIRE(std::abs((half_score - none_score) - 0.5F * (full_score - none_score)) < 1e-5F);
  }

  SECTION("a non-finite or negative multiplier fails when executed") {
    for (const float multiplier : {-1.0F, std::numeric_limits<float>::quiet_NaN(),
                                   std::numeric_limits<float>::infinity()}) {
      auto* alpha = make_match_query("alpha", "text");
      auto* gap = make_match_query("gap", "text");
      auto* boost = make_boost_query(alpha, gap, &multiplier);
      lancedb_fts_query_free(alpha);
      lancedb_fts_query_free(gap);

      LanceDBQuery* query = lancedb_query_new(table());
      REQUIRE(query != nullptr);
      char* error_message = nullptr;
      REQUIRE(lancedb_query_full_text_search_query(query, boost, &error_message)
          == LANCEDB_SUCCESS);
      REQUIRE(error_message == nullptr);

      LanceDBQueryResult* result = nullptr;
      error_message = nullptr;
      REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message)
          == LANCEDB_INVALID_INPUT);
      REQUIRE(result == nullptr);
      REQUIRE(error_message != nullptr);
      REQUIRE(std::strstr(error_message, "negative_boost") != nullptr);
      lancedb_free_string(error_message);
      lancedb_fts_query_free(boost);
    }
  }

  SECTION("rejects NULL legs and clears the output") {
    auto* valid = make_match_query("alpha", "text");
    LanceDBFtsQuery* fts_query = nullptr;
    char* error_message = nullptr;

    REQUIRE(lancedb_fts_query_new_boost(
        nullptr, valid, nullptr, &fts_query, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    error_message = nullptr;
    REQUIRE(lancedb_fts_query_new_boost(
        valid, nullptr, nullptr, &fts_query, &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(fts_query == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);

    lancedb_fts_query_free(valid);
  }
}

TEST_CASE_METHOD(PhraseFtsFixture, "LanceDB Query - structured boolean nesting", "[fts]") {
  auto* alpha = make_match_query("alpha", "text");
  auto* gap = make_match_query("gap", "text");
  auto* beta = make_match_query("beta", "text");

  const float zero = 0.0F;
  const float one = 1.0F;
  auto* plain = make_boost_query(alpha, gap, &zero);
  auto* demoted = make_boost_query(alpha, gap, &one);
  std::vector<LanceDBFtsClause> plain_clauses = {
      {plain, LANCEDB_FTS_OCCUR_MUST},
      {beta, LANCEDB_FTS_OCCUR_MUST},
  };
  std::vector<LanceDBFtsClause> demoted_clauses = {
      {demoted, LANCEDB_FTS_OCCUR_MUST},
      {beta, LANCEDB_FTS_OCCUR_MUST},
  };
  auto* plain_boolean = make_boolean_query(plain_clauses);
  auto* demoted_boolean = make_boolean_query(demoted_clauses);
  lancedb_fts_query_free(alpha);
  lancedb_fts_query_free(gap);
  lancedb_fts_query_free(beta);

  auto plain_result = run_structured_query(table(), plain_boolean);
  auto demoted_result = run_structured_query(table(), demoted_boolean);
  lancedb_fts_query_free(plain);
  lancedb_fts_query_free(demoted);
  lancedb_fts_query_free(plain_boolean);
  lancedb_fts_query_free(demoted_boolean);

  REQUIRE(ids_of(plain_result) == std::set<int32_t>{0, 1, 2});
  REQUIRE(ids_of(demoted_result) == std::set<int32_t>{0, 1, 2});
  for (const int32_t id : {0, 2}) {
    REQUIRE(std::abs(float_column_of(demoted_result, "_score", id) -
                     float_column_of(plain_result, "_score", id)) < 1e-5F);
  }
  REQUIRE(float_column_of(demoted_result, "_score", 1) <
          float_column_of(plain_result, "_score", 1));
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

TEST_CASE_METHOD(FtsFixture,
                 "LanceDB Vector Query - structured FTS column selection",
                 "[fts][vector_query]") {
  auto options = lancedb_fts_match_options_default();
  options.term_operator = LANCEDB_FTS_OPERATOR_AND;

  char selected_column[] = "text";
  const char* selected_columns[] = {selected_column};
  auto* fts_query = make_match_query("common doc7", "missing", &options);
  char* error_message = nullptr;
  REQUIRE(lancedb_fts_query_with_columns(fts_query, selected_columns, 1, &error_message) ==
          LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  // The setter copies the names, so mutating the caller's buffer must not
  // change either builder.
  selected_column[0] = 'x';

  LanceDBQuery* plain = lancedb_query_new(table());
  REQUIRE(plain != nullptr);
  REQUIRE(lancedb_query_full_text_search_query(plain, fts_query, &error_message) ==
          LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  const auto query_vector = vector_for_row(0);
  LanceDBVectorQuery* hybrid = lancedb_vector_query_new(
      table(), query_vector.data(), query_vector.size());
  REQUIRE(hybrid != nullptr);
  REQUIRE(lancedb_vector_query_column(hybrid, "data", &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_vector_query_limit(hybrid, 2, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_vector_query_full_text_search_query(hybrid, fts_query, &error_message) ==
          LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  lancedb_fts_query_free(fts_query);

  auto plain_result = collect_query_result(lancedb_query_execute(plain));
  REQUIRE(ids_of(plain_result) == std::set<int32_t>{7});

  auto hybrid_result = collect_query_result(lancedb_vector_query_execute(hybrid));
  REQUIRE(hybrid_result.num_rows() == 2);
  REQUIRE(ids_of(hybrid_result) == std::set<int32_t>{0, 7});
  REQUIRE(hybrid_result.schema->GetFieldByName("_relevance_score") != nullptr);
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
