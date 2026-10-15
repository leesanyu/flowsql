// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/api.h>
#include <framework/interfaces/ikafka_consumer_session.h>
#include <rapidjson/document.h>
#include <string>
#include <vector>
namespace flowsql::channels::kafka {
struct StructuredField {
    std::string name, path, type, unit;
    bool nullable = false;
};
struct StructuredConfig {
    std::string entity, offset_reset = "earliest", commit_policy = "after_success";
    std::vector<StructuredField> fields;
    uint32_t max_batch_rows = 256, max_message_bytes = 1048576;
};
// Throws invalid_argument with a fixed public code, never input contents.
StructuredConfig ParseStructuredConfig(const rapidjson::Value& value);
std::shared_ptr<arrow::Schema> StructuredSchema(const StructuredConfig& config);
arrow::Result<std::shared_ptr<arrow::RecordBatch>> DecodeStructured(const StructuredConfig& config,
                                                                    const std::vector<KafkaMessageV1>& messages);
}  // namespace flowsql::channels::kafka
