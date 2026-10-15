// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/api.h>
#include <framework/interfaces/ikafka_producer_session.h>
#include <rapidjson/document.h>
namespace flowsql::channels::kafka {
struct SinkConfig {
    std::string entity, record_kind, revision_field;
    uint32_t schema_version = 0, max_message_bytes = 1048576, queue_max_messages = 1024, queue_max_bytes = 8388608,
             delivery_timeout_ms = 10000;
    std::vector<std::string> key_fields;
};
// Exceptions contain fixed public codes, never configuration or row values.
SinkConfig ParseSinkConfig(const rapidjson::Value& value);
class ResultCodec {
 public:
    ResultCodec(SinkConfig config, std::string publication_id, std::string task_id);
    void Open(const std::shared_ptr<arrow::Schema>& schema);
    void Validate(const arrow::RecordBatch& batch) const;
    KafkaProduceMessageV1 Row(const arrow::RecordBatch& batch, int64_t row, uint64_t ordinal) const;
    const std::string& Fingerprint() const { return fingerprint_; }

 private:
    SinkConfig config_;
    std::string publication_, task_, schema_json_, fingerprint_;
    std::shared_ptr<arrow::Schema> schema_;
    std::vector<int> keys_;
    int revision_ = -1;
};
}  // namespace flowsql::channels::kafka
