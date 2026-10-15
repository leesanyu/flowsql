// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_structured.h"
#include <rapidjson/pointer.h>
#include <rapidjson/reader.h>
#include <cmath>
#include <regex>
#include <set>
#include <stdexcept>

namespace flowsql::channels::kafka {
namespace {
void Require(bool condition) {
    if (!condition) throw std::invalid_argument("INVALID_STRUCTURED_SOURCE");
}
void Keys(const rapidjson::Value& value, const std::set<std::string>& allowed) {
    Require(value.IsObject());
    std::set<std::string> seen;
    for (const auto& item : value.GetObject()) {
        const std::string key(item.name.GetString(), item.name.GetStringLength());
        Require(allowed.count(key) && seen.insert(key).second);
    }
}
std::string String(const rapidjson::Value& value, const char* name, bool required = true) {
    if (!value.HasMember(name)) {
        Require(!required);
        return {};
    }
    const auto& field = value[name];
    Require(field.IsString() && field.GetStringLength() &&
            std::string(field.GetString(), field.GetStringLength()).find('\0') == std::string::npos);
    return {field.GetString(), field.GetStringLength()};
}
std::shared_ptr<arrow::DataType> Type(const std::string& type) {
    if (type == "bool") return arrow::boolean();
    if (type == "int64") return arrow::int64();
    if (type == "uint64") return arrow::uint64();
    if (type == "float64") return arrow::float64();
    if (type == "utf8") return arrow::utf8();
    throw std::invalid_argument("INVALID_STRUCTURED_SOURCE");
}
struct JsonGuard : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, JsonGuard> {
    std::vector<std::set<std::string>> levels;
    bool StartObject() {
        levels.emplace_back();
        return levels.size() <= 64;
    }
    bool StartArray() { return StartObject(); }
    bool EndObject(rapidjson::SizeType) {
        levels.pop_back();
        return true;
    }
    bool EndArray(rapidjson::SizeType n) { return EndObject(n); }
    bool Key(const char* key, rapidjson::SizeType size, bool) {
        return levels.back().insert(std::string(key, size)).second;
    }
};
arrow::Status Append(arrow::ArrayBuilder* builder, const StructuredField& field, const rapidjson::Value* value) {
    if (!value || value->IsNull()) {
        return field.nullable ? builder->AppendNull() : arrow::Status::Invalid("REQUIRED_FIELD_MISSING");
    }
    if (field.type == "bool" && value->IsBool())
        return static_cast<arrow::BooleanBuilder*>(builder)->Append(value->GetBool());
    if (field.type == "int64" && value->IsInt64())
        return static_cast<arrow::Int64Builder*>(builder)->Append(value->GetInt64());
    if (field.type == "uint64" && value->IsUint64())
        return static_cast<arrow::UInt64Builder*>(builder)->Append(value->GetUint64());
    if (field.type == "float64" && value->IsNumber() && std::isfinite(value->GetDouble()))
        return static_cast<arrow::DoubleBuilder*>(builder)->Append(value->GetDouble());
    if (field.type == "utf8" && value->IsString())
        return static_cast<arrow::StringBuilder*>(builder)->Append(value->GetString(), value->GetStringLength());
    return arrow::Status::Invalid("FIELD_TYPE_MISMATCH");
}
}  // namespace

StructuredConfig ParseStructuredConfig(const rapidjson::Value& value) {
    Keys(value,
         {"encoding", "entity", "fields", "offset_reset", "commit_policy", "max_batch_rows", "max_message_bytes"});
    Require(String(value, "encoding") == "json");
    StructuredConfig config;
    config.entity = String(value, "entity");
    Require(std::regex_match(config.entity, std::regex("[a-zA-Z_][a-zA-Z0-9_]{0,63}")));
    if (value.HasMember("offset_reset")) config.offset_reset = String(value, "offset_reset");
    if (value.HasMember("commit_policy")) config.commit_policy = String(value, "commit_policy");
    Require(config.offset_reset == "earliest" || config.offset_reset == "latest" || config.offset_reset == "error");
    Require(config.commit_policy == "after_success" || config.commit_policy == "none");
    auto bound = [&](const char* name, uint32_t limit, uint32_t* target) {
        if (value.HasMember(name)) {
            Require(value[name].IsUint() && value[name].GetUint() > 0 && value[name].GetUint() <= limit);
            *target = value[name].GetUint();
        }
    };
    bound("max_batch_rows", 4096, &config.max_batch_rows);
    bound("max_message_bytes", 16777216, &config.max_message_bytes);
    Require(value.HasMember("fields") && value["fields"].IsArray() && value["fields"].Size() > 0 &&
            value["fields"].Size() <= 128);
    std::set<std::string> names;
    for (const auto& field : value["fields"].GetArray()) {
        Keys(field, {"name", "path", "type", "nullable", "unit"});
        StructuredField f;
        f.name = String(field, "name");
        f.path = String(field, "path");
        f.type = String(field, "type");
        f.unit = String(field, "unit", false);
        Require(std::regex_match(f.name, std::regex("[a-zA-Z_][a-zA-Z0-9_]{0,63}")) &&
                f.name.rfind("__kafka_", 0) != 0 && names.insert(f.name).second);
        Require(f.path.size() <= 1024 && f.path.front() == '/' && rapidjson::Pointer(f.path.c_str()).IsValid());
        Require(field.HasMember("nullable") && field["nullable"].IsBool() && f.unit.size() <= 64);
        f.nullable = field["nullable"].GetBool();
        Type(f.type);
        if (f.unit.rfind("unix_", 0) == 0)
            Require(f.type == "int64" &&
                    (f.unit == "unix_s" || f.unit == "unix_ms" || f.unit == "unix_us" || f.unit == "unix_ns"));
        config.fields.push_back(std::move(f));
    }
    return config;
}

std::shared_ptr<arrow::Schema> StructuredSchema(const StructuredConfig& config) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (const auto& f : config.fields) {
        auto metadata = arrow::key_value_metadata({"json_pointer"}, {f.path});
        if (!f.unit.empty()) metadata->Append("unit", f.unit);
        fields.push_back(arrow::field(f.name, Type(f.type), f.nullable, metadata));
    }
    fields.push_back(arrow::field("__kafka_topic", arrow::utf8(), false));
    fields.push_back(arrow::field("__kafka_partition", arrow::int32(), false));
    fields.push_back(arrow::field("__kafka_offset", arrow::int64(), false));
    fields.push_back(arrow::field("__kafka_timestamp_ms", arrow::int64(), true));
    fields.push_back(arrow::field("__kafka_key", arrow::binary(), true));
    return arrow::schema(fields, arrow::key_value_metadata({"entity", "encoding"}, {config.entity, "json"}));
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> DecodeStructured(const StructuredConfig& config,
                                                                    const std::vector<KafkaMessageV1>& messages) {
    if (messages.size() > config.max_batch_rows) return arrow::Status::Invalid("BATCH_LIMIT_EXCEEDED");
    auto schema = StructuredSchema(config);
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    for (const auto& field : schema->fields()) {
        ARROW_ASSIGN_OR_RAISE(auto builder, arrow::MakeBuilder(field->type()));
        builders.push_back(std::move(builder));
    }
    for (const auto& message : messages) {
        if (message.value_is_null || message.value.size() > config.max_message_bytes ||
            message.value.find('\0') != std::string::npos)
            return arrow::Status::Invalid("INVALID_MESSAGE_SIZE_OR_TOMBSTONE");
        rapidjson::Reader reader;
        rapidjson::StringStream stream(message.value.c_str());
        JsonGuard guard;
        if (!reader.Parse<rapidjson::kParseValidateEncodingFlag>(stream, guard))
            return arrow::Status::Invalid("INVALID_JSON_OBJECT");
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag>(message.value.data(), message.value.size());
        if (document.HasParseError() || !document.IsObject()) return arrow::Status::Invalid("INVALID_JSON_OBJECT");
        size_t column = 0;
        for (const auto& field : config.fields) {
            ARROW_RETURN_NOT_OK(
                Append(builders[column++].get(), field, rapidjson::Pointer(field.path.c_str()).Get(document)));
        }
        ARROW_RETURN_NOT_OK(static_cast<arrow::StringBuilder*>(builders[column++].get())->Append(message.topic));
        ARROW_RETURN_NOT_OK(static_cast<arrow::Int32Builder*>(builders[column++].get())->Append(message.partition));
        ARROW_RETURN_NOT_OK(static_cast<arrow::Int64Builder*>(builders[column++].get())->Append(message.offset));
        auto* timestamp = static_cast<arrow::Int64Builder*>(builders[column++].get());
        ARROW_RETURN_NOT_OK(message.timestamp_ms < 0 ? timestamp->AppendNull()
                                                     : timestamp->Append(message.timestamp_ms));
        auto* key = static_cast<arrow::BinaryBuilder*>(builders[column].get());
        ARROW_RETURN_NOT_OK(message.key_is_null ? key->AppendNull() : key->Append(message.key));
    }
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (auto& builder : builders) {
        ARROW_ASSIGN_OR_RAISE(auto array, builder->Finish());
        arrays.push_back(std::move(array));
    }
    return arrow::RecordBatch::Make(schema, messages.size(), arrays);
}
}  // namespace flowsql::channels::kafka
