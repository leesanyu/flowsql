// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_result_codec.h"
#include <arrow/util/utf8.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
namespace flowsql::channels::kafka {
namespace {
using V = rapidjson::Value;
using A = rapidjson::Document::AllocatorType;
void Check(bool ok, const char* code = "INVALID_RESULT_SCHEMA") {
    if (!ok) throw std::invalid_argument(code);
}
bool Utf8(const std::string& s) { return arrow::util::ValidateUTF8(s); }
V Str(const std::string& s, A& a) {
    Check(Utf8(s), "INVALID_RESULT_UTF8");
    return V(s.data(), s.size(), a);
}
void Put(V& v, const char* key, const std::string& s, A& a) { v.AddMember(V(key, a), Str(s, a), a); }
std::string Json(const V& v) {
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    Check(v.Accept(w));
    return {b.GetString(), b.GetSize()};
}
std::string Text(const V& v, const char* k) {
    Check(v.HasMember(k) && v[k].IsString(), "INVALID_STRUCTURED_SINK");
    std::string s(v[k].GetString(), v[k].GetStringLength());
    Check(!s.empty() && s.find('\0') == std::string::npos && Utf8(s), "INVALID_STRUCTURED_SINK");
    return s;
}
bool Integer(arrow::Type::type t) { return t >= arrow::Type::UINT8 && t <= arrow::Type::INT64; }
bool KeyType(arrow::Type::type t) {
    return Integer(t) || t == arrow::Type::STRING || t == arrow::Type::BINARY || t == arrow::Type::FIXED_SIZE_BINARY;
}
std::string Type(const arrow::DataType& t) {
    switch (t.id()) {
        case arrow::Type::BOOL:
            return "bool";
        case arrow::Type::INT8:
            return "int8";
        case arrow::Type::INT16:
            return "int16";
        case arrow::Type::INT32:
            return "int32";
        case arrow::Type::INT64:
            return "int64";
        case arrow::Type::UINT8:
            return "uint8";
        case arrow::Type::UINT16:
            return "uint16";
        case arrow::Type::UINT32:
            return "uint32";
        case arrow::Type::UINT64:
            return "uint64";
        case arrow::Type::FLOAT:
            return "float32";
        case arrow::Type::DOUBLE:
            return "float64";
        case arrow::Type::STRING:
            return "utf8";
        case arrow::Type::BINARY:
            return "binary";
        case arrow::Type::FIXED_SIZE_BINARY:
            return "fixed_size_binary";
        case arrow::Type::TIMESTAMP:
            return "timestamp";
        default:
            throw std::invalid_argument("UNSUPPORTED_RESULT_TYPE");
    }
}
V Metadata(const std::shared_ptr<const arrow::KeyValueMetadata>& m, A& a) {
    V v(rapidjson::kObjectType);
    std::map<std::string, std::string> sorted;
    if (m)
        for (int64_t i = 0; i < m->size(); ++i) {
            Check(Utf8(m->key(i)) && Utf8(m->value(i)) && sorted.emplace(m->key(i), m->value(i)).second);
        }
    for (auto& [k, val] : sorted) v.AddMember(Str(k, a), Str(val, a), a);
    return v;
}
std::string Schema(const arrow::Schema& s) {
    rapidjson::Document d(rapidjson::kObjectType);
    auto& a = d.GetAllocator();
    V fields(rapidjson::kArrayType);
    std::set<std::string> names;
    for (auto& f : s.fields()) {
        Check(!f->name().empty() && Utf8(f->name()) && f->name().find('\0') == std::string::npos &&
              names.insert(f->name()).second);
        V field(rapidjson::kObjectType);
        auto& t = *f->type();
        // Lexicographic UTF-8 order of every object key is part of the fingerprint contract.
        if (t.id() == arrow::Type::FIXED_SIZE_BINARY)
            field.AddMember("byte_width", static_cast<const arrow::FixedSizeBinaryType&>(t).byte_width(), a);
        field.AddMember("metadata", Metadata(f->metadata(), a), a);
        Put(field, "name", f->name(), a);
        field.AddMember("nullable", f->nullable(), a);
        if (t.id() == arrow::Type::TIMESTAMP)
            Put(field, "timezone", static_cast<const arrow::TimestampType&>(t).timezone(), a);
        Put(field, "type", Type(t), a);
        if (t.id() == arrow::Type::TIMESTAMP) {
            const char* units[] = {"s", "ms", "us", "ns"};
            Put(field, "unit", units[static_cast<const arrow::TimestampType&>(t).unit()], a);
        }
        fields.PushBack(field, a);
    }
    d.AddMember("fields", fields, a);
    d.AddMember("metadata", Metadata(s.metadata(), a), a);
    return Json(d);
}
V Value(const arrow::Array& ar, int64_t r, A& a) {
    if (ar.IsNull(r)) return V();
    switch (ar.type_id()) {
        case arrow::Type::BOOL:
            return V(static_cast<const arrow::BooleanArray&>(ar).Value(r));
        case arrow::Type::INT8:
            return V(static_cast<const arrow::Int8Array&>(ar).Value(r));
        case arrow::Type::INT16:
            return V(static_cast<const arrow::Int16Array&>(ar).Value(r));
        case arrow::Type::INT32:
            return V(static_cast<const arrow::Int32Array&>(ar).Value(r));
        case arrow::Type::UINT8:
            return V(static_cast<const arrow::UInt8Array&>(ar).Value(r));
        case arrow::Type::UINT16:
            return V(static_cast<const arrow::UInt16Array&>(ar).Value(r));
        case arrow::Type::UINT32:
            return V(static_cast<const arrow::UInt32Array&>(ar).Value(r));
        case arrow::Type::INT64:
            return Str(std::to_string(static_cast<const arrow::Int64Array&>(ar).Value(r)), a);
        case arrow::Type::UINT64:
            return Str(std::to_string(static_cast<const arrow::UInt64Array&>(ar).Value(r)), a);
        case arrow::Type::TIMESTAMP:
            return Str(std::to_string(static_cast<const arrow::TimestampArray&>(ar).Value(r)), a);
        case arrow::Type::FLOAT: {
            auto x = static_cast<const arrow::FloatArray&>(ar).Value(r);
            Check(std::isfinite(x), "NONFINITE_RESULT");
            return V(double(x));
        }
        case arrow::Type::DOUBLE: {
            auto x = static_cast<const arrow::DoubleArray&>(ar).Value(r);
            Check(std::isfinite(x), "NONFINITE_RESULT");
            return V(double(x));
        }
        case arrow::Type::STRING:
            return Str(static_cast<const arrow::StringArray&>(ar).GetString(r), a);
        case arrow::Type::BINARY:
        case arrow::Type::FIXED_SIZE_BINARY: {
            auto view = ar.type_id() == arrow::Type::BINARY
                            ? static_cast<const arrow::BinaryArray&>(ar).GetView(r)
                            : static_cast<const arrow::FixedSizeBinaryArray&>(ar).GetView(r);
            std::string out(4 * ((view.size() + 2) / 3), '\0');
            if (!view.empty())
                EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                reinterpret_cast<const unsigned char*>(view.data()), view.size());
            return Str(out, a);
        }
        default:
            throw std::invalid_argument("UNSUPPORTED_RESULT_TYPE");
    }
}
}  // namespace
SinkConfig ParseSinkConfig(const V& v) {
    Check(v.IsObject(), "INVALID_STRUCTURED_SINK");
    const std::set<std::string> allowed = {"encoding",          "entity",
                                           "schema_version",    "record_kind",
                                           "key_fields",        "revision_field",
                                           "max_message_bytes", "queue_max_messages",
                                           "queue_max_bytes",   "delivery_timeout_ms"};
    std::set<std::string> seen;
    for (auto& m : v.GetObject())
        Check(allowed.count({m.name.GetString(), m.name.GetStringLength()}) &&
                  seen.insert({m.name.GetString(), m.name.GetStringLength()}).second,
              "INVALID_STRUCTURED_SINK");
    Check(Text(v, "encoding") == "json", "INVALID_STRUCTURED_SINK");
    SinkConfig c;
    c.entity = Text(v, "entity");
    c.record_kind = Text(v, "record_kind");
    Check(std::regex_match(c.entity, std::regex("[a-z][a-z0-9_]{0,63}")) &&
              (c.record_kind == "event" || c.record_kind == "upsert"),
          "INVALID_STRUCTURED_SINK");
    auto number = [&](const char* k, uint32_t fallback, uint32_t lo, uint32_t hi) {
        if (!v.HasMember(k)) return fallback;
        Check(v[k].IsUint() && v[k].GetUint() >= lo && v[k].GetUint() <= hi, "INVALID_STRUCTURED_SINK");
        return v[k].GetUint();
    };
    Check(v.HasMember("schema_version"), "INVALID_STRUCTURED_SINK");
    c.schema_version = number("schema_version", 0, 1, INT32_MAX);
    c.max_message_bytes = number("max_message_bytes", 1048576, 1024, 16777216);
    c.queue_max_messages = number("queue_max_messages", 1024, 1, 65536);
    c.queue_max_bytes = number("queue_max_bytes", 8388608, 1048576, 268435456);
    c.delivery_timeout_ms = number("delivery_timeout_ms", 10000, 100, 120000);
    Check(c.queue_max_bytes >= c.max_message_bytes, "INVALID_STRUCTURED_SINK");
    if (v.HasMember("key_fields")) {
        Check(v["key_fields"].IsArray() && v["key_fields"].Size() <= 8, "INVALID_STRUCTURED_SINK");
        std::set<std::string> keys;
        for (auto& key : v["key_fields"].GetArray()) {
            Check(key.IsString(), "INVALID_STRUCTURED_SINK");
            std::string s(key.GetString(), key.GetStringLength());
            Check(!s.empty() && Utf8(s) && s.find('\0') == std::string::npos && keys.insert(s).second,
                  "INVALID_STRUCTURED_SINK");
            c.key_fields.push_back(s);
        }
    }
    if (c.record_kind == "upsert") {
        c.revision_field = Text(v, "revision_field");
        Check(!c.key_fields.empty() &&
                  std::find(c.key_fields.begin(), c.key_fields.end(), c.revision_field) == c.key_fields.end(),
              "INVALID_STRUCTURED_SINK");
    } else
        Check(!v.HasMember("revision_field"), "INVALID_STRUCTURED_SINK");
    return c;
}
ResultCodec::ResultCodec(SinkConfig c, std::string p, std::string t)
    : config_(std::move(c)), publication_(std::move(p)), task_(std::move(t)) {}
void ResultCodec::Open(const std::shared_ptr<arrow::Schema>& s) {
    Check(bool(s));
    schema_json_ = Schema(*s);
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(schema_json_.data()), schema_json_.size(), hash);
    fingerprint_.clear();
    const char hex[] = "0123456789abcdef";
    for (auto b : hash) {
        fingerprint_ += hex[b >> 4];
        fingerprint_ += hex[b & 15];
    }
    keys_.clear();
    for (auto& k : config_.key_fields) {
        int i = s->GetFieldIndex(k);
        Check(i >= 0 && KeyType(s->field(i)->type()->id()), "INVALID_RESULT_KEY");
        keys_.push_back(i);
    }
    revision_ = -1;
    if (config_.record_kind == "upsert") {
        revision_ = s->GetFieldIndex(config_.revision_field);
        Check(revision_ >= 0 && Integer(s->field(revision_)->type()->id()), "INVALID_RESULT_REVISION");
    }
    schema_ = s;
}
void ResultCodec::Validate(const arrow::RecordBatch& b) const {
    Check(schema_ && Schema(*b.schema()) == schema_json_, "RESULT_SCHEMA_DRIFT");
    Check(b.ValidateFull().ok(), "INVALID_RESULT_BATCH");
    for (int i = 0; i < b.num_columns(); ++i)
        Check(schema_->field(i)->nullable() || b.column(i)->null_count() == 0, "RESULT_NULLABILITY");
}
KafkaProduceMessageV1 ResultCodec::Row(const arrow::RecordBatch& b, int64_t r, uint64_t ordinal) const {
    rapidjson::Document d(rapidjson::kObjectType);
    auto& a = d.GetAllocator();
    V data(rapidjson::kObjectType), id(rapidjson::kArrayType);
    for (int i = 0; i < b.num_columns(); ++i)
        data.AddMember(Str(schema_->field(i)->name(), a), Value(*b.column(i), r, a), a);
    for (auto i : keys_) {
        Check(!b.column(i)->IsNull(r), "INVALID_RESULT_KEY");
        if (b.column(i)->type_id() == arrow::Type::STRING)
            Check(!static_cast<const arrow::StringArray&>(*b.column(i)).GetView(r).empty(), "INVALID_RESULT_KEY");
        V part(rapidjson::kObjectType);
        Put(part, "type", Type(*b.column(i)->type()), a);
        part.AddMember("value", Value(*b.column(i), r, a), a);
        id.PushBack(part, a);
    }
    if (keys_.empty()) {
        V p(rapidjson::kObjectType);
        Put(p, "type", "publication", a);
        Put(p, "value", publication_, a);
        id.PushBack(p, a);
        V n(rapidjson::kObjectType);
        Put(n, "type", "uint64", a);
        Put(n, "value", std::to_string(ordinal), a);
        id.PushBack(n, a);
    }
    V revision;
    if (revision_ >= 0) {
        auto& ar = *b.column(revision_);
        Check(!ar.IsNull(r), "INVALID_RESULT_REVISION");
        auto v = Value(ar, r, a);
        std::string s = v.IsString() ? std::string(v.GetString()) : Json(v);
        Check(!s.empty() && s[0] != '-' && s != "0", "INVALID_RESULT_REVISION");
        revision = Str(s, a);
    }
    V key(rapidjson::kArrayType);
    key.PushBack(Str(config_.entity, a), a);
    key.PushBack(config_.schema_version, a);
    key.PushBack(V(id, a), a);
    Put(d, "entity", config_.entity, a);
    d.AddMember("envelope_version", 1, a);
    d.AddMember("data", data, a);
    Put(d, "publication_id", publication_, a);
    Put(d, "record_kind", config_.record_kind, a);
    d.AddMember("result_id", id, a);
    d.AddMember("revision", revision, a);
    rapidjson::Document schema;
    schema.Parse(schema_json_.c_str());
    d.AddMember("schema", V(schema, a), a);
    Put(d, "schema_fingerprint", fingerprint_, a);
    d.AddMember("schema_version", config_.schema_version, a);
    Put(d, "task_id", task_, a);
    KafkaProduceMessageV1 out{Json(key), Json(d)};
    Check(out.key.size() <= 4096 && out.key.size() + out.value.size() <= config_.max_message_bytes,
          "RESULT_MESSAGE_TOO_LARGE");
    return out;
}
}  // namespace flowsql::channels::kafka
