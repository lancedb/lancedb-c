/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

// Include the public header first so this translation unit checks that the
// standard Arrow C declarations accept Arrow C++ being included afterwards.
// The other test translation units include Arrow before lancedb.h.
#include "lancedb.h"

#include "test_common.h"

#include <atomic>
#include <cerrno>
#include <limits>

namespace {

// A producer whose schema, batches, errors and callback counts are controlled
// by the test. The callbacks intentionally run on whatever thread the SDK uses.
struct ControlledStream {
  std::shared_ptr<arrow::Schema> schema;
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  int schema_error_code = 0;
  std::string schema_error;
  size_t fail_at_batch = std::numeric_limits<size_t>::max();
  int batch_error_code = EIO;
  std::string batch_error;
  bool null_last_error = false;
  std::atomic<int> get_schema_calls{0};
  std::atomic<int> get_next_calls{0};
  std::atomic<int> release_calls{0};
  std::string last_error;
};

extern "C" int controlled_get_schema(struct ArrowArrayStream* stream,
                                     struct ArrowSchema* out) {
  auto* state = static_cast<ControlledStream*>(stream->private_data);
  state->get_schema_calls.fetch_add(1);

  if (state->schema_error_code != 0) {
    state->last_error = state->schema_error;
    return state->schema_error_code;
  }

  auto status = arrow::ExportSchema(*state->schema, out);
  if (!status.ok()) {
    state->last_error = status.ToString();
    return EINVAL;
  }
  return 0;
}

extern "C" int controlled_get_next(struct ArrowArrayStream* stream,
                                   struct ArrowArray* out) {
  auto* state = static_cast<ControlledStream*>(stream->private_data);
  const auto index = static_cast<size_t>(state->get_next_calls.fetch_add(1));

  if (index == state->fail_at_batch) {
    state->last_error = state->batch_error;
    return state->batch_error_code;
  }

  // Array entries let a test hand over a valid struct array that cannot be
  // represented as a RecordBatch, without changing the stream callbacks.
  if (!state->arrays.empty()) {
    if (index >= state->arrays.size()) {
      *out = ArrowArray{};
      return 0;
    }
    auto status = arrow::ExportArray(*state->arrays[index], out);
    if (!status.ok()) {
      state->last_error = status.ToString();
      return EINVAL;
    }
    return 0;
  }

  if (index >= state->batches.size()) {
    *out = ArrowArray{};
    return 0;
  }

  auto status = arrow::ExportRecordBatch(*state->batches[index], out);
  if (!status.ok()) {
    state->last_error = status.ToString();
    return EINVAL;
  }
  return 0;
}

extern "C" const char* controlled_get_last_error(struct ArrowArrayStream* stream) {
  auto* state = static_cast<ControlledStream*>(stream->private_data);
  if (state->null_last_error || state->last_error.empty()) {
    return nullptr;
  }
  return state->last_error.c_str();
}

extern "C" void controlled_release(struct ArrowArrayStream* stream) {
  auto* state = static_cast<ControlledStream*>(stream->private_data);
  state->release_calls.fetch_add(1);
  stream->release = nullptr;
}

void initialize_stream(ControlledStream& state, struct ArrowArrayStream* stream) {
  *stream = ArrowArrayStream{};
  stream->get_schema = controlled_get_schema;
  stream->get_next = controlled_get_next;
  stream->get_last_error = controlled_get_last_error;
  stream->release = controlled_release;
  stream->private_data = &state;
}

// Export a real Arrow C++ reader so the import path is exercised through the
// same C stream implementation consumers use.
void export_batches(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                    struct ArrowArrayStream* stream) {
  auto schema = batches.empty() ? create_test_schema() : batches.front()->schema();
  auto reader = arrow::RecordBatchReader::Make(batches, schema).ValueOrDie();
  REQUIRE(arrow::ExportRecordBatchReader(reader, stream).ok());
}

LanceDBRecordBatchReader* import_stream(struct ArrowArrayStream* stream) {
  LanceDBRecordBatchReader* reader = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_record_batch_reader_from_arrow_stream(
      stream, &reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(reader != nullptr);
  REQUIRE(stream->release == nullptr);
  return reader;
}

LanceDBRecordBatchReader* import_controlled_stream(ControlledStream& state,
                                                   struct ArrowArrayStream* stream) {
  initialize_stream(state, stream);
  return import_stream(stream);
}

std::shared_ptr<arrow::RecordBatch> make_batch_with_value(int num_rows,
                                                          int start_index,
                                                          float value) {
  auto schema = create_test_schema();
  arrow::StringBuilder key_builder;
  arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
      std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

  for (int i = 0; i < num_rows; i++) {
    REQUIRE(key_builder.Append("key_" + std::to_string(start_index + i)).ok());
    auto* values = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
      REQUIRE(values->Append(value).ok());
    }
    REQUIRE(data_builder.Append().ok());
  }

  std::shared_ptr<arrow::Array> key_array, data_array;
  REQUIRE(key_builder.Finish(&key_array).ok());
  REQUIRE(data_builder.Finish(&data_array).ok());
  return arrow::RecordBatch::Make(schema, num_rows, {key_array, data_array});
}

// Build a valid struct array whose root has one null row. Its child columns
// are ordinary valid Arrow arrays; only the top-level struct is invalid for a
// RecordBatch.
std::shared_ptr<arrow::StructArray> make_root_null_struct(
    const std::shared_ptr<arrow::RecordBatch>& batch) {
  auto null_bitmap = arrow::AllocateEmptyBitmap(batch->num_rows()).ValueOrDie();
  null_bitmap->mutable_data()[0] &= static_cast<uint8_t>(~1U);
  return arrow::StructArray::Make(batch->columns(), batch->schema()->fields(),
                                  std::move(null_bitmap), 1).ValueOrDie();
}

// Read every key through the existing IPC result path, so the check covers
// actual table contents rather than the reader's callback counters.
std::string read_table_keys(LanceDBTable* table) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);

  const char* columns[] = {"key"};
  char* error_message = nullptr;
  REQUIRE(lancedb_query_select(query, columns, 1, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  LanceDBQueryResult* result = nullptr;
  REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(result != nullptr);

  LanceDBIpcBuffer output = {};
  REQUIRE(lancedb_query_result_to_ipc_file(result, &output, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  auto contents = read_ipc_file(output.data, output.len);
  auto keys = join_strings(read_ipc_file_strings(contents, 0));

  lancedb_ipc_buffer_free(&output);
  // lancedb_query_result_to_ipc_file() consumes the result.
  return keys;
}

float read_table_value(LanceDBTable* table, const std::string& key) {
  LanceDBQuery* query = lancedb_query_new(table);
  REQUIRE(query != nullptr);

  const std::string filter = "key = '" + key + "'";
  char* error_message = nullptr;
  REQUIRE(lancedb_query_where_filter(query, filter.c_str(), &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  const char* columns[] = {"key", "data"};
  REQUIRE(lancedb_query_select(query, columns, 2, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  LanceDBQueryResult* result = nullptr;
  REQUIRE(lancedb_query_execute_with_status(query, &result, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(result != nullptr);

  LanceDBIpcBuffer output = {};
  REQUIRE(lancedb_query_result_to_ipc_file(result, &output, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);

  auto contents = read_ipc_file(output.data, output.len);
  bool found = false;
  float value = 0.0F;
  for (const auto& batch : contents.batches) {
    auto keys = std::static_pointer_cast<arrow::StringArray>(batch->column(0));
    auto data = std::static_pointer_cast<arrow::FixedSizeListArray>(batch->column(1));
    auto values = std::static_pointer_cast<arrow::FloatArray>(data->values());
    for (int64_t i = 0; i < batch->num_rows(); i++) {
      if (keys->GetString(i) == key) {
        value = values->Value(data->value_offset(i));
        found = true;
      }
    }
  }

  lancedb_ipc_buffer_free(&output);
  // lancedb_query_result_to_ipc_file() consumes the result.

  REQUIRE(found);
  return value;
}

}  // namespace

TEST_CASE_METHOD(LanceDBFixture, "Arrow C Stream import creates and adds multi-batch data",
                 "[arrow_stream]") {
  const auto schema = create_test_schema();
  const std::vector<std::shared_ptr<arrow::RecordBatch>> batches = {
      create_test_record_batch(2, 0), create_test_record_batch(3, 2)};

  SECTION("create table") {
    struct ArrowSchema c_schema;
    REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

    struct ArrowArrayStream stream;
    export_batches(batches, &stream);
    REQUIRE(stream.release != nullptr);
    auto reader = import_stream(&stream);

    LanceDBTable* table = nullptr;
    char* error_message = nullptr;
    REQUIRE(lancedb_table_create(db, "stream_create",
        reinterpret_cast<FFI_ArrowSchema*>(&c_schema), reader, &table,
        &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(lancedb_table_count_rows(table) == 5);
    REQUIRE(read_table_keys(table) == "key_0,key_1,key_2,key_3,key_4");

    lancedb_table_free(table);
    if (c_schema.release) {
      c_schema.release(&c_schema);
    }
  }

  SECTION("add to table") {
    create_empty_table("stream_add");
    LanceDBTable* table = open_table("stream_add");

    struct ArrowArrayStream stream;
    export_batches(batches, &stream);
    auto reader = import_stream(&stream);

    char* error_message = nullptr;
    REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
    REQUIRE(error_message == nullptr);
    REQUIRE(lancedb_table_count_rows(table) == 5);
    REQUIRE(read_table_keys(table) == "key_0,key_1,key_2,key_3,key_4");

    lancedb_table_free(table);
  }
}

TEST_CASE_METHOD(LanceDBFixture, "Arrow C Stream import merges multi-batch data",
                 "[arrow_stream]") {
  create_empty_table("stream_merge");
  LanceDBTable* table = open_table("stream_merge");

  auto initial = create_reader_from_batch(make_batch_with_value(3, 0, 0.0F));
  REQUIRE(initial != nullptr);
  char* error_message = nullptr;
  REQUIRE(lancedb_table_add(table, initial, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == 3);

  const std::vector<std::shared_ptr<arrow::RecordBatch>> batches = {
      make_batch_with_value(1, 1, 7.0F), make_batch_with_value(2, 2, 7.0F)};
  struct ArrowArrayStream stream;
  export_batches(batches, &stream);
  auto reader = import_stream(&stream);

  const char* on_columns[] = {"key"};
  LanceDBMergeInsertConfig config = {
      .when_matched_update_all = 1,
      .when_not_matched_insert_all = 1,
      .when_matched_update_all_condition = nullptr,
      .when_matched_update_all_expr = nullptr};
  REQUIRE(lancedb_table_merge_insert(
      table, reader, on_columns, 1, &config, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == 4);
  REQUIRE(read_table_keys(table) == "key_0,key_1,key_2,key_3");
  REQUIRE(read_table_value(table, "key_1") == 7.0F);
  REQUIRE(read_table_value(table, "key_2") == 7.0F);

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "Arrow C Stream import acquires schema before returning",
                 "[arrow_stream]") {
  create_empty_table("stream_lazy");
  LanceDBTable* table = open_table("stream_lazy");

  ControlledStream state;
  state.schema = create_test_schema();
  state.batches = {create_test_record_batch(2, 0), create_test_record_batch(3, 2)};

  struct ArrowArrayStream stream;
  auto reader = import_controlled_stream(state, &stream);

  REQUIRE(state.get_schema_calls.load() == 1);
  REQUIRE(state.get_next_calls.load() == 0);
  REQUIRE(state.release_calls.load() == 0);

  char* error_message = nullptr;
  REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == 5);
  REQUIRE(state.get_next_calls.load() >= 2);
  REQUIRE(state.release_calls.load() == 1);

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "Arrow C Stream import frees an unused reader once",
                 "[arrow_stream]") {
  ControlledStream state;
  state.schema = create_test_schema();
  state.batches = {create_test_record_batch(1, 0)};

  struct ArrowArrayStream stream;
  auto reader = import_controlled_stream(state, &stream);

  REQUIRE(state.get_schema_calls.load() == 1);
  REQUIRE(state.get_next_calls.load() == 0);
  REQUIRE(stream.release == nullptr);

  lancedb_record_batch_reader_free(reader);
  REQUIRE(state.release_calls.load() == 1);
  REQUIRE(state.get_next_calls.load() == 0);
}

TEST_CASE_METHOD(LanceDBFixture, "Arrow C Stream import validates callbacks before moving",
                 "[arrow_stream]") {
  ControlledStream state;
  state.schema = create_test_schema();
  state.batches = {create_test_record_batch(1, 0)};

  struct ArrowArrayStream stream;
  initialize_stream(state, &stream);
  stream.get_next = nullptr;

  LanceDBRecordBatchReader* reader = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_record_batch_reader_from_arrow_stream(
      &stream, &reader, &error_message) == LANCEDB_INVALID_ARGUMENT);
  REQUIRE(reader == nullptr);
  REQUIRE(error_message != nullptr);
  REQUIRE(std::string(error_message).find("get_next") != std::string::npos);
  REQUIRE(stream.release != nullptr);
  REQUIRE(state.release_calls.load() == 0);

  stream.release(&stream);
  REQUIRE(state.release_calls.load() == 1);
  lancedb_free_string(error_message);
}

TEST_CASE_METHOD(LanceDBFixture, "Arrow C Stream import releases after schema failure",
                 "[arrow_stream]") {
  ControlledStream state;
  state.schema = create_test_schema();
  state.schema_error_code = EINVAL;
  state.schema_error = "deliberate schema failure";

  struct ArrowArrayStream stream;
  initialize_stream(state, &stream);
  LanceDBRecordBatchReader* reader = nullptr;
  char* error_message = nullptr;
  REQUIRE(lancedb_record_batch_reader_from_arrow_stream(
      &stream, &reader, &error_message) == LANCEDB_INVALID_INPUT);
  REQUIRE(reader == nullptr);
  REQUIRE(error_message != nullptr);
  REQUIRE(std::string(error_message).find("deliberate schema failure") != std::string::npos);
  REQUIRE(stream.release == nullptr);
  REQUIRE(state.release_calls.load() == 1);
  REQUIRE(state.get_next_calls.load() == 0);

  lancedb_free_string(error_message);
}

TEST_CASE_METHOD(LanceDBFixture, "Arrow C Stream import preserves read error detail",
                 "[arrow_stream]") {
  create_empty_table("stream_read_error");
  LanceDBTable* table = open_table("stream_read_error");

  ControlledStream state;
  state.schema = create_test_schema();
  state.batches = {create_test_record_batch(1, 0)};
  state.fail_at_batch = 1;
  state.batch_error = "deliberate read failure";

  struct ArrowArrayStream stream;
  auto reader = import_controlled_stream(state, &stream);

  char* error_message = nullptr;
  REQUIRE(lancedb_table_add(table, reader, &error_message) != LANCEDB_SUCCESS);
  REQUIRE(error_message != nullptr);
  REQUIRE(std::string(error_message).find("deliberate read failure") != std::string::npos);
  REQUIRE(state.release_calls.load() == 1);

  lancedb_free_string(error_message);
  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "Arrow C Stream import handles NULL get_last_error",
                 "[arrow_stream]") {
  create_empty_table("stream_null_error");
  LanceDBTable* table = open_table("stream_null_error");

  ControlledStream state;
  state.schema = create_test_schema();
  state.fail_at_batch = 0;
  state.batch_error_code = EIO;
  state.null_last_error = true;

  struct ArrowArrayStream stream;
  auto reader = import_controlled_stream(state, &stream);

  char* error_message = nullptr;
  REQUIRE(lancedb_table_add(table, reader, &error_message) != LANCEDB_SUCCESS);
  REQUIRE(error_message != nullptr);
  const std::string message(error_message);
  REQUIRE(message.find("code 5") != std::string::npos);
  REQUIRE(message.find("without an error message") != std::string::npos);
  REQUIRE(state.release_calls.load() == 1);

  lancedb_free_string(error_message);
  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "Arrow C Stream import does not treat a zero-row batch as EOF",
                 "[arrow_stream]") {
  create_empty_table("stream_zero_row");
  LanceDBTable* table = open_table("stream_zero_row");

  ControlledStream state;
  state.schema = create_test_schema();
  state.batches = {create_test_record_batch(0, 0), create_test_record_batch(2, 0)};

  struct ArrowArrayStream stream;
  auto reader = import_controlled_stream(state, &stream);

  char* error_message = nullptr;
  REQUIRE(lancedb_table_add(table, reader, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(lancedb_table_count_rows(table) == 2);
  REQUIRE(read_table_keys(table) == "key_0,key_1");
  REQUIRE(state.release_calls.load() == 1);

  lancedb_table_free(table);
}

TEST_CASE_METHOD(LanceDBFixture, "Arrow C Stream import rejects root-null struct rows",
                 "[arrow_stream]") {
  constexpr int initial_rows = 1;
  LanceDBTable* table = create_table_with_data("stream_root_nulls", initial_rows, 0);
  const auto version = lancedb_table_version(table);
  REQUIRE(lancedb_table_count_rows(table) == initial_rows);

  auto batch = create_test_record_batch(2, 10);
  auto root_null = make_root_null_struct(batch);
  REQUIRE(root_null->null_count() == 1);
  REQUIRE(root_null->IsNull(0));
  REQUIRE(root_null->IsValid(1));

  ControlledStream state;
  state.schema = batch->schema();
  state.arrays = {root_null};

  struct ArrowArrayStream stream;
  auto reader = import_controlled_stream(state, &stream);

  char* error_message = nullptr;
  REQUIRE(lancedb_table_add(table, reader, &error_message) != LANCEDB_SUCCESS);
  REQUIRE(error_message != nullptr);
  REQUIRE(std::string(error_message).find("null rows") != std::string::npos);
  REQUIRE(lancedb_table_count_rows(table) == initial_rows);
  REQUIRE(lancedb_table_version(table) == version);
  REQUIRE(state.get_next_calls.load() >= 1);
  REQUIRE(state.release_calls.load() == 1);

  lancedb_free_string(error_message);
  lancedb_table_free(table);
}
