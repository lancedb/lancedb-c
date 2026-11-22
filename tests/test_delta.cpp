/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" {
LanceDBError lancedb_test_table_delta_with_kind(
    const LanceDBTable* table,
    uint64_t version,
    int kind,
    LanceDBTableDelta** delta_out,
    char** error_message);
}

namespace {

struct ConnectionDeleter {
  void operator()(LanceDBConnection* connection) const {
    lancedb_connection_free(connection);
  }
};

struct TableDeleter {
  void operator()(LanceDBTable* table) const { lancedb_table_free(table); }
};

struct DeltaDeleter {
  void operator()(LanceDBTableDelta* delta) const {
    lancedb_table_delta_free(delta);
  }
};

using ConnectionPtr =
    std::unique_ptr<LanceDBConnection, ConnectionDeleter>;
using TablePtr = std::unique_ptr<LanceDBTable, TableDeleter>;
using DeltaPtr = std::unique_ptr<LanceDBTableDelta, DeltaDeleter>;

enum class DeltaRows {
  Inserted,
  Updated,
  Upserted,
  Deleted,
};

std::shared_ptr<arrow::Schema> delta_schema() {
  return arrow::schema({arrow::field("id", arrow::int64()),
                        arrow::field("value", arrow::int64())});
}

std::shared_ptr<arrow::RecordBatch> delta_batch(
    const std::vector<int64_t>& ids, const std::vector<int64_t>& values) {
  REQUIRE(ids.size() == values.size());

  arrow::Int64Builder id_builder;
  arrow::Int64Builder value_builder;
  for (size_t i = 0; i < ids.size(); ++i) {
    REQUIRE(id_builder.Append(ids[i]).ok());
    REQUIRE(value_builder.Append(values[i]).ok());
  }

  std::shared_ptr<arrow::Array> id_array;
  std::shared_ptr<arrow::Array> value_array;
  REQUIRE(id_builder.Finish(&id_array).ok());
  REQUIRE(value_builder.Finish(&value_array).ok());
  return arrow::RecordBatch::Make(delta_schema(), ids.size(),
                                  {id_array, value_array});
}

ConnectionPtr open_stable_connection(const std::string& uri) {
  LanceDBConnectBuilder* builder = lancedb_connect(uri.c_str());
  REQUIRE(builder != nullptr);
  builder = lancedb_connect_builder_storage_option(
      builder, "new_table_enable_stable_row_ids", "true");
  REQUIRE(builder != nullptr);

  LanceDBConnection* connection = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_connect_builder_execute(builder, &connection, &error_message) ==
          LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(connection != nullptr);
  return ConnectionPtr(connection);
}

TablePtr create_delta_table(LanceDBConnection* connection,
                            const std::string& table_name,
                            const std::vector<int64_t>& ids,
                            const std::vector<int64_t>& values) {
  auto schema = delta_schema();
  auto batch = delta_batch(ids, values);
  LanceDBRecordBatchReader* reader = create_reader_from_batch(batch);
  REQUIRE(reader != nullptr);

  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  LanceDBTable* table = nullptr;
  char* error_message = nullptr;
  LanceDBError result = lancedb_table_create(
      connection, table_name.c_str(),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema), reader, &table,
      &error_message);
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }
  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);
  return TablePtr(table);
}

void append_rows(LanceDBTable* table, const std::vector<int64_t>& ids,
                 const std::vector<int64_t>& values) {
  auto batch = delta_batch(ids, values);
  LanceDBRecordBatchReader* reader = create_reader_from_batch(batch);
  REQUIRE(reader != nullptr);

  char* error_message = nullptr;
  REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
}

void set_value(LanceDBTable* table, int64_t id, int64_t value) {
  const char* columns[] = {"value"};
  std::string expression = std::to_string(value);
  const char* expressions[] = {expression.c_str()};
  std::string predicate = "id = " + std::to_string(id);

  LanceDBUpdateResult update_result{};
  char* error_message = nullptr;
  REQUIRE(lancedb_table_update(table, columns, expressions, 1,
                               predicate.c_str(), &update_result,
                               &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
}

void delete_rows(LanceDBTable* table, const std::string& predicate) {
  char* error_message = nullptr;
  REQUIRE(lancedb_table_delete(table, predicate.c_str(), &error_message) ==
          LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
}

DeltaPtr create_delta(const LanceDBTable* table, const LanceDBDeltaRef* begin,
                      const LanceDBDeltaRef* end) {
  LanceDBTableDelta* delta = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_table_delta(table, begin, end, &delta, &error_message) ==
          LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(delta != nullptr);
  return DeltaPtr(delta);
}

LanceDBError read_delta_rows(const LanceDBTableDelta* delta, DeltaRows rows,
                             LanceDBQueryResult** result_out,
                             char** error_message) {
  switch (rows) {
    case DeltaRows::Inserted:
      return lancedb_table_delta_inserted_rows(delta, result_out,
                                               error_message);
    case DeltaRows::Updated:
      return lancedb_table_delta_updated_rows(delta, result_out,
                                              error_message);
    case DeltaRows::Upserted:
      return lancedb_table_delta_upserted_rows(delta, result_out,
                                               error_message);
    case DeltaRows::Deleted:
      return lancedb_table_delta_deleted_row_ids(delta, result_out,
                                                 error_message);
  }
  FAIL("unknown delta reader");
  return LANCEDB_UNKNOWN;
}

std::vector<int64_t> int64_values(const IpcFileContents& file,
                                  const std::string& column) {
  int index = file.schema->GetFieldIndex(column);
  REQUIRE(index >= 0);
  std::vector<int64_t> values;
  for (const auto& batch : file.batches) {
    auto array =
        std::dynamic_pointer_cast<arrow::Int64Array>(batch->column(index));
    REQUIRE(array != nullptr);
    for (int64_t i = 0; i < array->length(); ++i) {
      values.push_back(array->Value(i));
    }
  }
  return values;
}

std::vector<uint64_t> uint64_values(const IpcFileContents& file,
                                    const std::string& column) {
  int index = file.schema->GetFieldIndex(column);
  REQUIRE(index >= 0);
  std::vector<uint64_t> values;
  for (const auto& batch : file.batches) {
    auto array =
        std::dynamic_pointer_cast<arrow::UInt64Array>(batch->column(index));
    REQUIRE(array != nullptr);
    for (int64_t i = 0; i < array->length(); ++i) {
      values.push_back(array->Value(i));
    }
  }
  return values;
}

std::map<int64_t, uint64_t> id_row_ids(const IpcFileContents& file) {
  int id_index = file.schema->GetFieldIndex("id");
  int row_id_index = file.schema->GetFieldIndex("_rowid");
  REQUIRE(id_index >= 0);
  REQUIRE(row_id_index >= 0);

  std::map<int64_t, uint64_t> result;
  for (const auto& batch : file.batches) {
    auto ids = std::dynamic_pointer_cast<arrow::Int64Array>(
        batch->column(id_index));
    auto row_ids = std::dynamic_pointer_cast<arrow::UInt64Array>(
        batch->column(row_id_index));
    REQUIRE(ids != nullptr);
    REQUIRE(row_ids != nullptr);
    REQUIRE(ids->length() == row_ids->length());
    for (int64_t i = 0; i < ids->length(); ++i) {
      result[ids->Value(i)] = row_ids->Value(i);
    }
  }
  return result;
}

std::map<int64_t, int64_t> id_int64_values(const IpcFileContents& file,
                                           const std::string& column) {
  int id_index = file.schema->GetFieldIndex("id");
  int value_index = file.schema->GetFieldIndex(column);
  REQUIRE(id_index >= 0);
  REQUIRE(value_index >= 0);

  std::map<int64_t, int64_t> result;
  for (const auto& batch : file.batches) {
    auto ids =
        std::dynamic_pointer_cast<arrow::Int64Array>(batch->column(id_index));
    auto values = std::dynamic_pointer_cast<arrow::Int64Array>(
        batch->column(value_index));
    REQUIRE(ids != nullptr);
    REQUIRE(values != nullptr);
    REQUIRE(ids->length() == values->length());
    for (int64_t i = 0; i < ids->length(); ++i) {
      result[ids->Value(i)] = values->Value(i);
    }
  }
  return result;
}

void require_int64_set(const IpcFileContents& file, const std::string& column,
                       std::vector<int64_t> expected) {
  auto actual = int64_values(file, column);
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  REQUIRE(actual == expected);
}

void require_row_change_schema(const IpcFileContents& file) {
  REQUIRE(file.schema->num_fields() == 5);
  REQUIRE(file.schema->field(0)->name() == "id");
  REQUIRE(file.schema->field(1)->name() == "value");
  REQUIRE(file.schema->field(2)->name() == "_rowid");
  REQUIRE(file.schema->field(3)->name() == "_row_created_at_version");
  REQUIRE(file.schema->field(4)->name() == "_row_last_updated_at_version");
  REQUIRE(file.schema->field(2)->type()->id() == arrow::Type::UINT64);
}

void require_deleted_row_schema(const IpcFileContents& file) {
  REQUIRE(file.schema->num_fields() == 1);
  REQUIRE(file.schema->field(0)->name() == "_rowid");
  REQUIRE(file.schema->field(0)->type()->id() == arrow::Type::UINT64);
}

IpcFileContents into_ipc(LanceDBQueryResult* result) {
  LanceDBIpcBuffer buffer{};
  char* error_message = nullptr;
  LanceDBError status =
      lancedb_query_result_to_ipc_file(result, &buffer, &error_message);
  INFO((error_message != nullptr ? error_message : ""));
  REQUIRE(status == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  IpcFileContents contents = read_ipc_file(buffer.data, buffer.len);
  lancedb_ipc_buffer_free(&buffer);
  return contents;
}

}  // namespace

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Delta streams row changes",
                 "[table][delta]") {
  auto stable_db = open_stable_connection(uri);
  auto table =
      create_delta_table(stable_db.get(), "delta", {0, 1}, {10, 20});
  REQUIRE(lancedb_table_version(table.get()) == 1);

  append_rows(table.get(), {2}, {30});
  REQUIRE(lancedb_table_version(table.get()) == 2);

  set_value(table.get(), 0, 100);
  REQUIRE(lancedb_table_version(table.get()) == 3);

  set_value(table.get(), 1, 200);
  REQUIRE(lancedb_table_version(table.get()) == 4);

  char* error_message = nullptr;
  REQUIRE(lancedb_table_create_tag(table.get(), "baseline", 2,
                                   &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_create_tag(table.get(), "before_delete", 4,
                                   &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  LanceDBDeltaRef from_v1{LANCEDB_DELTA_REF_VERSION, 1, nullptr};
  uint64_t row_id_0 = 0;
  uint64_t row_id_1 = 0;
  uint64_t row_id_2 = 0;

  {
    auto delta = create_delta(table.get(), &from_v1, nullptr);
    REQUIRE(lancedb_table_delta_begin_version(delta.get()) == 1);
    REQUIRE(lancedb_table_delta_end_version(delta.get()) == 4);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_table_delta_inserted_rows(delta.get(), &result,
                                              &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    IpcFileContents inserted = into_ipc(result);
    require_row_change_schema(inserted);
    require_int64_set(inserted, "id", {2});
    require_int64_set(inserted, "value", {30});
    auto inserted_ids = id_row_ids(inserted);
    REQUIRE(inserted_ids.size() == 1);
    row_id_2 = inserted_ids.at(2);

    REQUIRE(lancedb_table_delta_updated_rows(delta.get(), &result,
                                             &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    IpcFileContents updated = into_ipc(result);
    require_row_change_schema(updated);
    require_int64_set(updated, "id", {0, 1});
    require_int64_set(updated, "value", {100, 200});
    auto updated_values = id_int64_values(updated, "value");
    REQUIRE(updated_values.size() == 2);
    REQUIRE(updated_values.at(0) == 100);
    REQUIRE(updated_values.at(1) == 200);
    auto updated_ids = id_row_ids(updated);
    REQUIRE(updated_ids.size() == 2);
    row_id_0 = updated_ids.at(0);
    row_id_1 = updated_ids.at(1);
    REQUIRE(row_id_0 != row_id_1);
    REQUIRE(row_id_0 != row_id_2);
    REQUIRE(row_id_1 != row_id_2);

    REQUIRE(lancedb_table_delta_upserted_rows(delta.get(), &result,
                                              &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    IpcFileContents upserted = into_ipc(result);
    require_row_change_schema(upserted);
    require_int64_set(upserted, "id", {0, 1, 2});
    require_int64_set(upserted, "value", {100, 200, 30});
    auto upserted_values = id_int64_values(upserted, "value");
    REQUIRE(upserted_values.size() == 3);
    REQUIRE(upserted_values.at(0) == 100);
    REQUIRE(upserted_values.at(1) == 200);
    REQUIRE(upserted_values.at(2) == 30);
    auto upserted_ids = id_row_ids(upserted);
    REQUIRE(upserted_ids.size() == 3);
    REQUIRE(upserted_ids.at(0) == row_id_0);
    REQUIRE(upserted_ids.at(1) == row_id_1);
    REQUIRE(upserted_ids.at(2) == row_id_2);
  }

  delete_rows(table.get(), "id = 0");
  REQUIRE(lancedb_table_version(table.get()) == 5);

  {
    LanceDBDeltaRef from_v4{LANCEDB_DELTA_REF_VERSION, 4, nullptr};
    auto delta = create_delta(table.get(), &from_v4, nullptr);
    REQUIRE(lancedb_table_delta_end_version(delta.get()) == 5);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_table_delta_deleted_row_ids(delta.get(), &result,
                                                &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    IpcFileContents deleted = into_ipc(result);
    require_deleted_row_schema(deleted);
    REQUIRE(uint64_values(deleted, "_rowid") ==
            std::vector<uint64_t>{row_id_0});
  }

  {
    LanceDBDeltaRef from_baseline{LANCEDB_DELTA_REF_TAG, 0, "baseline"};
    LanceDBDeltaRef to_before_delete{LANCEDB_DELTA_REF_TAG, 0,
                                     "before_delete"};
    auto delta =
        create_delta(table.get(), &from_baseline, &to_before_delete);
    REQUIRE(lancedb_table_delta_begin_version(delta.get()) == 2);
    REQUIRE(lancedb_table_delta_end_version(delta.get()) == 4);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_table_delta_updated_rows(delta.get(), &result,
                                             &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    IpcFileContents updated = into_ipc(result);
    require_int64_set(updated, "id", {0, 1});
    require_int64_set(updated, "value", {100, 200});
    auto updated_values = id_int64_values(updated, "value");
    REQUIRE(updated_values.size() == 2);
    REQUIRE(updated_values.at(0) == 100);
    REQUIRE(updated_values.at(1) == 200);
  }

  {
    LanceDBDeltaRef to_baseline{LANCEDB_DELTA_REF_TAG, 0, "baseline"};
    auto delta = create_delta(table.get(), &from_v1, &to_baseline);
    REQUIRE(lancedb_table_delta_begin_version(delta.get()) == 1);
    REQUIRE(lancedb_table_delta_end_version(delta.get()) == 2);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_table_delta_inserted_rows(delta.get(), &result,
                                              &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    IpcFileContents inserted = into_ipc(result);
    require_int64_set(inserted, "id", {2});
    require_int64_set(inserted, "value", {30});
  }

  // A result keeps its batches and schema after both handles are freed, and a
  // delta keeps its pinned endpoints after the source table is freed.
  {
    auto delta = create_delta(table.get(), &from_v1, nullptr);
    table.reset();
    stable_db.reset();

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_table_delta_inserted_rows(delta.get(), &result,
                                              &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    delta.reset();

    IpcFileContents inserted = into_ipc(result);
    require_row_change_schema(inserted);
    require_int64_set(inserted, "id", {2});
    require_int64_set(inserted, "value", {30});
  }
}

TEST_CASE_METHOD(LanceDBFixture,
                 "LanceDB Table Delta validation and stable row ids",
                 "[table][delta]") {
  auto table = create_delta_table(db, "plain_delta", {0}, {10});
  append_rows(table.get(), {1}, {20});
  REQUIRE(lancedb_table_version(table.get()) == 2);

  LanceDBDeltaRef from_v1{LANCEDB_DELTA_REF_VERSION, 1, nullptr};

  SECTION("row readers require stable row ids at their own stage") {
    LanceDBTableDelta* delta = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_delta(table.get(), &from_v1, nullptr, &delta,
                                &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(delta != nullptr);
    REQUIRE(lancedb_table_delta_begin_version(delta) == 1);
    REQUIRE(lancedb_table_delta_end_version(delta) == 2);

    for (auto reader : {DeltaRows::Inserted, DeltaRows::Updated,
                        DeltaRows::Upserted, DeltaRows::Deleted}) {
      LanceDBQueryResult* result =
          reinterpret_cast<LanceDBQueryResult*>(static_cast<uintptr_t>(1));
      REQUIRE(read_delta_rows(delta, reader, &result, &error_message) ==
              LANCEDB_INVALID_INPUT);
      REQUIRE(result == nullptr);
      REQUIRE(error_message != nullptr);
      std::string message(error_message);
      REQUIRE(message.find("new_table_enable_stable_row_ids") !=
              std::string::npos);
      REQUIRE(message.find("list_transactions") != std::string::npos);
      lancedb_free_string(error_message);
      error_message = nullptr;
    }

    lancedb_table_delta_free(delta);
  }

  SECTION("equal endpoints keep their schema without stable row ids") {
    uint64_t version = lancedb_table_version(table.get());
    LanceDBDeltaRef equal{LANCEDB_DELTA_REF_VERSION, version, nullptr};

    LanceDBTableDelta* delta = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_delta(table.get(), &equal, &equal, &delta,
                                &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_delta_begin_version(delta) == version);
    REQUIRE(lancedb_table_delta_end_version(delta) == version);

    LanceDBQueryResult* result = nullptr;
    REQUIRE(lancedb_table_delta_inserted_rows(delta, &result,
                                              &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    IpcFileContents inserted = into_ipc(result);
    require_row_change_schema(inserted);
    REQUIRE(inserted.batches.empty());

    REQUIRE(lancedb_table_delta_deleted_row_ids(delta, &result,
                                                &error_message) ==
            LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    IpcFileContents deleted = into_ipc(result);
    require_deleted_row_schema(deleted);
    REQUIRE(deleted.batches.empty());

    lancedb_table_delta_free(delta);
  }

  SECTION("constructor validates endpoints and output pointers") {
    LanceDBTableDelta* delta = nullptr;
    char* error_message = nullptr;

    REQUIRE(lancedb_table_delta(nullptr, &from_v1, nullptr, &delta,
                                &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(delta == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_delta(table.get(), nullptr, nullptr, &delta,
                                &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(delta == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_delta(table.get(), &from_v1, nullptr, nullptr,
                                &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_test_table_delta_with_kind(table.get(), 1, 99, &delta,
                                               &error_message) ==
            LANCEDB_INVALID_ARGUMENT);
    REQUIRE(delta == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    LanceDBDeltaRef null_tag{LANCEDB_DELTA_REF_TAG, 0, nullptr};
    REQUIRE(lancedb_table_delta(table.get(), &null_tag, nullptr, &delta,
                                &error_message) == LANCEDB_INVALID_ARGUMENT);
    REQUIRE(delta == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    LanceDBDeltaRef from_v2{LANCEDB_DELTA_REF_VERSION, 2, nullptr};
    REQUIRE(lancedb_table_delta(table.get(), &from_v2, &from_v1, &delta,
                                &error_message) == LANCEDB_INVALID_INPUT);
    REQUIRE(delta == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    LanceDBDeltaRef missing_version{LANCEDB_DELTA_REF_VERSION, 99, nullptr};
    REQUIRE(lancedb_table_delta(table.get(), &missing_version, nullptr,
                                &delta, &error_message) ==
            LANCEDB_INVALID_INPUT);
    REQUIRE(delta == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    LanceDBDeltaRef missing_tag{LANCEDB_DELTA_REF_TAG, 0, "missing"};
    REQUIRE(lancedb_table_delta(table.get(), &missing_tag, nullptr, &delta,
                                &error_message) == LANCEDB_INVALID_INPUT);
    REQUIRE(delta == nullptr);
    REQUIRE(error_message != nullptr);
    lancedb_free_string(error_message);
    error_message = nullptr;

    REQUIRE(lancedb_table_delta_begin_version(nullptr) == 0);
    REQUIRE(lancedb_table_delta_end_version(nullptr) == 0);
    lancedb_table_delta_free(nullptr);
  }
}
