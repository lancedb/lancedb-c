/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include <filesystem>
#include "test_common.h"

// Helper function to check if directory exists
int directory_exists(const char* path) {
  return std::filesystem::is_directory(path);
}

// Helper function to remove directory recursively
int remove_directory(const char* path) {
  return std::filesystem::remove_all(path) ? 0 : 1;
}

void LanceDBFixture::create_empty_table(const std::string& table_name) {
  // Create schema
  auto schema = create_test_schema();

  // Convert to C ABI
  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  // Create empty table
  LanceDBTable* table = nullptr;
  char* error_message = nullptr;

  LanceDBError result = lancedb_table_create(
      db,
      table_name.c_str(),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      nullptr,
      &table,
      &error_message
      );

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);

  lancedb_table_free(table);

  // Clean up schema
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }
}

LanceDBTable* LanceDBFixture::create_table_with_data(const std::string& table_name, int num_rows, int start_index) {
  // Create schema
  auto schema = create_test_schema();

  // Create data
  auto batch = create_test_record_batch(num_rows, start_index);
  auto reader = create_reader_from_batch(batch);
  REQUIRE(reader != nullptr);

  // Convert to C ABI
  struct ArrowSchema c_schema;
  REQUIRE(arrow::ExportSchema(*schema, &c_schema).ok());

  // Create table with data
  LanceDBTable* table = nullptr;
  char* error_message = nullptr;

  LanceDBError result = lancedb_table_create(
      db,
      table_name.c_str(),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      reader,
      &table,
      &error_message
  );

  REQUIRE(result == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);

  // Clean up schema
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  return table;
}

LanceDBTable* LanceDBFixture::open_table(const std::string& table_name) {
  LanceDBTable* table = nullptr;
  char* error_message = nullptr;

  REQUIRE(lancedb_connection_open_table(db, table_name.c_str(), &table, &error_message) == LANCEDB_SUCCESS);
  REQUIRE(error_message == nullptr);
  REQUIRE(table != nullptr);

  return table;
}

void LanceDBFixture::require_table_not_found(const std::string& table_name) {
  LanceDBTable* table = nullptr;
  char* error_message = nullptr;

  REQUIRE(lancedb_connection_open_table(db, table_name.c_str(), &table, &error_message) == LANCEDB_TABLE_NOT_FOUND);
  REQUIRE(error_message != nullptr);
  REQUIRE(table == nullptr);

  lancedb_free_string(error_message);
}

std::shared_ptr<arrow::Schema> create_test_schema() {
  auto key_field = arrow::field("key", arrow::utf8());
  auto data_field = arrow::field("data", arrow::fixed_size_list(arrow::float32(), TEST_SCHEMA_DIMENSIONS));
  return arrow::schema({key_field, data_field});
}

std::shared_ptr<arrow::RecordBatch> create_test_record_batch(int num_rows, int start_index) {
  auto schema = create_test_schema();

  // Create builders
  arrow::StringBuilder key_builder;
  arrow::FixedSizeListBuilder data_builder(arrow::default_memory_pool(),
      std::make_unique<arrow::FloatBuilder>(), TEST_SCHEMA_DIMENSIONS);

  // Add rows
  for (int i = 0; i < num_rows; i++) {
    int idx = start_index + i;
    REQUIRE(key_builder.Append("key_" + std::to_string(idx)).ok());

    auto list_builder = static_cast<arrow::FloatBuilder*>(data_builder.value_builder());
    for (size_t j = 0; j < TEST_SCHEMA_DIMENSIONS; j++) {
      REQUIRE(list_builder->Append(static_cast<float>(idx * 10 + j)).ok());
    }
    REQUIRE(data_builder.Append().ok());
  }

  // Build arrays
  std::shared_ptr<arrow::Array> key_array, data_array;
  REQUIRE(key_builder.Finish(&key_array).ok());
  REQUIRE(data_builder.Finish(&data_array).ok());

  return arrow::RecordBatch::Make(schema, num_rows, {key_array, data_array});
}

LanceDBRecordBatchReader* create_reader_from_batch(const std::shared_ptr<arrow::RecordBatch>& batch) {
  struct ArrowArray c_array;
  struct ArrowSchema c_schema;

  REQUIRE(arrow::ExportRecordBatch(*batch, &c_array, &c_schema).ok());

  LanceDBRecordBatchReader* reader;
  REQUIRE(lancedb_record_batch_reader_from_arrow(
      reinterpret_cast<FFI_ArrowArray*>(&c_array),
      reinterpret_cast<FFI_ArrowSchema*>(&c_schema),
      &reader, nullptr) == LANCEDB_SUCCESS);

  // Schema is only read by the function, so we must release it
  if (c_schema.release) {
    c_schema.release(&c_schema);
  }

  return reader;
}


namespace {

int32_t read_le_int32(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<int32_t>(bytes[offset]) |
         (static_cast<int32_t>(bytes[offset + 1]) << 8) |
         (static_cast<int32_t>(bytes[offset + 2]) << 16) |
         (static_cast<int32_t>(bytes[offset + 3]) << 24);
}

}  // namespace

std::vector<uint8_t> write_ipc_file(
    const std::shared_ptr<arrow::Schema>& schema,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
  auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
  auto writer = arrow::ipc::MakeFileWriter(sink.get(), schema).ValueOrDie();
  for (const auto& batch : batches) {
    REQUIRE(writer->WriteRecordBatch(*batch).ok());
  }
  REQUIRE(writer->Close().ok());
  auto buffer = sink->Finish().ValueOrDie();
  const uint8_t* data = buffer->data();
  return std::vector<uint8_t>(data, data + buffer->size());
}

std::vector<uint8_t> write_ipc_stream(
    const std::shared_ptr<arrow::Schema>& schema,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
  auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
  auto writer = arrow::ipc::MakeStreamWriter(sink.get(), schema).ValueOrDie();
  for (const auto& batch : batches) {
    REQUIRE(writer->WriteRecordBatch(*batch).ok());
  }
  REQUIRE(writer->Close().ok());
  auto buffer = sink->Finish().ValueOrDie();
  const uint8_t* data = buffer->data();
  return std::vector<uint8_t>(data, data + buffer->size());
}

std::vector<uint8_t> write_ipc_file_with_corrupt_second_batch(
    const std::shared_ptr<arrow::Schema>& schema,
    const std::shared_ptr<arrow::RecordBatch>& first,
    const std::shared_ptr<arrow::RecordBatch>& second) {
  const auto first_only = write_ipc_file(schema, {first});
  auto both = write_ipc_file(schema, {first, second});

  // The footer length sits in the last ten bytes. The writer ends a file with an
  // eight byte end-of-stream marker followed by the footer, so in the file that
  // holds only the first batch that marker is where the second batch message
  // starts in a file that holds both batches.
  const int32_t footer_len = read_le_int32(first_only, first_only.size() - 10);
  const size_t second_message =
      first_only.size() - 10 - static_cast<size_t>(footer_len) - 8;

  // Zero the message metadata flatbuffer of the second batch, so the footer and
  // the first batch stay readable while the second batch cannot be decoded
  REQUIRE(read_le_int32(both, second_message) == -1);
  const int32_t metadata_len = read_le_int32(both, second_message + 4);
  REQUIRE(metadata_len > 0);
  REQUIRE(second_message + 8 + static_cast<size_t>(metadata_len) <= both.size());
  std::memset(both.data() + second_message + 8, 0, static_cast<size_t>(metadata_len));
  return both;
}

IpcFileContents read_ipc_file(const uint8_t* data, size_t len) {
  auto buffer = arrow::Buffer::FromString(
      std::string(reinterpret_cast<const char*>(data), len));
  auto input = std::make_shared<arrow::io::BufferReader>(buffer);
  auto reader = arrow::ipc::RecordBatchFileReader::Open(input).ValueOrDie();

  IpcFileContents contents;
  contents.schema = reader->schema();
  for (int i = 0; i < reader->num_record_batches(); i++) {
    auto batch = reader->ReadRecordBatch(i);
    REQUIRE(batch.ok());
    contents.batches.push_back(*batch);
  }
  return contents;
}

std::vector<std::string> read_ipc_file_strings(const IpcFileContents& file, int column_index) {
  std::vector<std::string> values;
  for (const auto& batch : file.batches) {
    auto strings = std::dynamic_pointer_cast<arrow::StringArray>(batch->column(column_index));
    REQUIRE(strings != nullptr);
    for (int64_t i = 0; i < strings->length(); i++) {
      values.push_back(strings->GetString(i));
    }
  }
  std::sort(values.begin(), values.end());
  return values;
}

std::string join_strings(const std::vector<std::string>& values) {
  std::string joined;
  for (const auto& value : values) {
    if (!joined.empty()) {
      joined += ",";
    }
    joined += value;
  }
  return joined;
}
