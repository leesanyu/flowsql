// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <common/error_code.h>
#include <framework/interfaces/ichannel_dataset_descriptor.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <chrono>
#include <common/json_depth.hpp>
#include <stdexcept>

namespace flowsql::dataset {
using Json = rapidjson::Document;
using Value = rapidjson::Value;
using Alloc = Json::AllocatorType;
inline std::string Text(const Value& v) { return {v.GetString(), v.GetStringLength()}; }
inline std::string Get(const Value& v, const char* key, const std::string& fallback = "") {
    return v.IsObject() && v.HasMember(key) && v[key].IsString() ? Text(v[key]) : fallback;
}
inline Value Copy(const Value& v, Alloc& a) { return Value(v, a); }
inline void Set(Value& v, const char* key, Value value, Alloc& a) {
    if (v.HasMember(key))
        v[key] = std::move(value);
    else
        v.AddMember(Value(key, a), value, a);
}
inline void Set(Value& v, const char* key, const std::string& text, Alloc& a) {
    Set(v, key, Value(text.c_str(), text.size(), a), a);
}
inline std::string Encode(const Value& v) {
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    v.Accept(w);
    return {b.GetString(), b.GetSize()};
}
inline void Sort(Value& v) {
    if (v.IsObject()) {
        std::sort(v.MemberBegin(), v.MemberEnd(),
                  [](const auto& a, const auto& b) { return Text(a.name) < Text(b.name); });
        for (auto& m : v.GetObject()) Sort(m.value);
    } else if (v.IsArray())
        for (auto& x : v.GetArray()) Sort(x);
}
inline std::string Canonical(const Value& v) {
    Json d;
    d.CopyFrom(v, d.GetAllocator());
    Sort(d);
    return Encode(d);
}
inline std::string Hex(const unsigned char* p, size_t n) {
    std::string text;
    for (size_t i = 0; i < n; ++i) {
        text += "0123456789abcdef"[p[i] >> 4];
        text += "0123456789abcdef"[p[i] & 15];
    }
    return text;
}
inline std::string Hash(const std::string& s) {
    unsigned char bytes[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), bytes);
    return Hex(bytes, sizeof(bytes));
}
inline std::string Token() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("identity entropy unavailable");
    return Hex(bytes, sizeof(bytes));
}
inline int Fail(DataSemanticsErrorV1* e, const std::string& code, const std::string& message,
                const std::string& path = "") {
    if (e) *e = {code, message, path};
    return -1;
}
inline Value Type(const std::string& system, const std::string& name, Alloc& a) {
    Value t(rapidjson::kObjectType);
    Set(t, "system", system, a);
    Set(t, "name", name, a);
    t.AddMember("parameters", Value(rapidjson::kObjectType), a);
    return t;
}
inline Value Field(const std::string& name, Value type, bool nullable, Alloc& a) {
    Value f(rapidjson::kObjectType);
    Set(f, "name", name, a);
    f.AddMember("type", type, a).AddMember("nullable", nullable, a);
    return f;
}
inline Json Snapshot(const std::string& provider, const std::string& id, const Value& locator, const Value& fields,
                     const Value& metadata, const std::string& generation, const std::string& identity = "known",
                     const std::string& state = "known") {
    Json d(rapidjson::kObjectType);
    auto& a = d.GetAllocator();
    Set(d, "schema_id", "flowsql.channel-dataset.v1", a);
    Value key(rapidjson::kObjectType);
    Set(key, "provider_id", provider, a);
    Set(key, "object_id", id, a);
    d.AddMember("object_key", key, a).AddMember("locator", Copy(locator, a), a);
    Set(d, "identity_state", identity, a);
    Set(d, "schema_state", state, a);
    d.AddMember("fields", Copy(fields, a), a).AddMember("producer_metadata", Copy(metadata, a), a);
    d.AddMember("structure_gaps", Value(rapidjson::kArrayType), a);
    if (state == "known")
        Set(d, "schema_fingerprint", Hash(Canonical(fields)), a);
    else
        d.AddMember("schema_fingerprint", Value(), a);
    Set(d, "source_revision", Hash(id + "\n" + generation + "\n" + Canonical(fields) + "\n" + Canonical(metadata)), a);
    return d;
}
inline int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
inline int32_t DescriptorControl(IChannelDatasetDescriptorV1& provider, const std::string& request,
                                 std::string& response) {
    Json req;
    if (JsonNestingWithin(request, 64)) req.Parse(request.c_str());
    ChannelDatasetSnapshotV1 snapshot;
    DataSemanticsErrorV1 e;
    int rc = -1;
    if (!req.IsObject() || !req.HasMember("locator") || !req["locator"].IsObject() ||
        (req.HasMember("timeout_ms") &&
         (!req["timeout_ms"].IsUint() || !req["timeout_ms"].GetUint() || req["timeout_ms"].GetUint() > 30000)))
        e = {"INVALID_REQUEST", "locator and timeout_ms are invalid", ""};
    else
        rc = provider.Describe(
            {Encode(req["locator"]), req.HasMember("timeout_ms") ? req["timeout_ms"].GetUint() : 5000}, &snapshot, &e);
    Json result(rapidjson::kObjectType);
    auto& a = result.GetAllocator();
    if (rc == 0 && snapshot.document_json) {
        Json s;
        s.Parse(snapshot.document_json->c_str());
        Value data(rapidjson::kObjectType);
        data.AddMember("snapshot", Copy(s, a), a);
        result.AddMember("data", data, a);
        response = Encode(result);
        return 0;
    }
    Value err(rapidjson::kObjectType);
    Set(err, "code", e.code, a);
    Set(err, "message", e.message, a);
    Set(err, "field_path", e.field_path, a);
    err.AddMember("details", Value(rapidjson::kObjectType), a);
    result.AddMember("error", err, a);
    response = Encode(result);
    if (e.code == "NOT_FOUND") return error::NOT_FOUND;
    if (e.code == "SOURCE_CHANGED") return error::CONFLICT;
    if (e.code == "SOURCE_TIMEOUT") return error::TIMEOUT;
    if (e.code == "SOURCE_UNAVAILABLE" || e.code == "STORAGE_ERROR") return error::UNAVAILABLE;
    return error::BAD_REQUEST;
}
}  // namespace flowsql::dataset
