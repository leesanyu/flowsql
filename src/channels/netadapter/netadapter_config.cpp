// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "netadapter_config.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cerrno>
#include <set>
namespace flowsql::channels::netadapter {
int ParseNetAdapterConfig(const std::string& json, CaptureBackendConfigV1* output, std::string* normalized,
                          std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return EINVAL;
    };
    if (!output || !normalized) return fail("null NetAdapter configuration output");
    rapidjson::Document doc;
    doc.Parse(json.data(), json.size());
    if (doc.HasParseError() || !doc.IsObject()) return fail("options must be a JSON object");
    std::set<std::string> fields;
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        const std::string name(it->name.GetString(), it->name.GetStringLength());
        if (!fields.insert(name).second) return fail("duplicate option");
        if (name != "backend" && name != "interfaces" && name != "snaplen" && name != "promiscuous" &&
            name != "buffer_mib")
            return fail("unknown NetAdapter option");
    }
    CaptureBackendConfigV1 config;
    if (!doc.HasMember("backend") || !doc["backend"].IsString()) return fail("backend is required");
    config.backend.assign(doc["backend"].GetString(), doc["backend"].GetStringLength());
    if (config.backend != "af_packet" && config.backend != "pfring_classic" && config.backend != "af_xdp_copy_skb")
        return fail("unsupported backend");
    if (!doc.HasMember("interfaces") || !doc["interfaces"].IsArray() || doc["interfaces"].Empty())
        return fail("interfaces must be a nonempty array");
    std::set<std::string> interfaces;
    for (const auto& item : doc["interfaces"].GetArray()) {
        if (!item.IsString()) return fail("interface name must be a string");
        std::string name(item.GetString(), item.GetStringLength());
        if (name.empty() || name.size() > 15 || name.find_first_of("/ :\t\r\n") != std::string::npos ||
            name.find('\0') != std::string::npos || !interfaces.insert(name).second)
            return fail("invalid or duplicate interface name");
        config.interfaces.push_back(std::move(name));
    }
    if (doc.HasMember("promiscuous")) {
        if (!doc["promiscuous"].IsBool()) return fail("promiscuous must be boolean");
        config.promiscuous = doc["promiscuous"].GetBool();
    }
    if (doc.HasMember("snaplen")) {
        if (!doc["snaplen"].IsUint() || !doc["snaplen"].GetUint() || doc["snaplen"].GetUint() > 65535)
            return fail("snaplen must be 1..65535");
        config.snaplen = doc["snaplen"].GetUint();
    }
    if (doc.HasMember("buffer_mib")) {
        if (!doc["buffer_mib"].IsUint() || !doc["buffer_mib"].GetUint()) return fail("buffer_mib must be positive");
        config.buffer_bytes = uint64_t(doc["buffer_mib"].GetUint()) * kMiB;
    }
    if (config.buffer_bytes < kSharedEnvelopeBytes + config.interfaces.size() * kMinimumInputBytes)
        return fail("buffer budget cannot cover every interface and shared Arrow envelope");
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("backend");
    writer.String(config.backend.c_str());
    writer.Key("interfaces");
    writer.StartArray();
    for (const auto& name : config.interfaces) writer.String(name.c_str());
    writer.EndArray();
    writer.Key("promiscuous");
    writer.Bool(config.promiscuous);
    writer.Key("snaplen");
    writer.Uint(config.snaplen);
    writer.Key("buffer_mib");
    writer.Uint64(config.buffer_bytes / kMiB);
    writer.EndObject();
    *normalized = buf.GetString();
    *output = std::move(config);
    if (error) error->clear();
    return 0;
}
CaptureSourceSetV2 DefaultSourceSet() {
    CaptureSourceSetV2 sources;
    sources.limits.max_packets_per_batch = 256;
    sources.limits.max_bytes_per_batch = kMiB;
    sources.limits.max_wait_ms = 10;
    sources.limits.max_outstanding_batches = 1;
    return sources;
}
}  // namespace flowsql::channels::netadapter
