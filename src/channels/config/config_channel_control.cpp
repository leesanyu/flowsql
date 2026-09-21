// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "config_channel_plugin.h"

#include <common/error_code.h>
#include <common/json_depth.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flowsql::channels::config {
namespace {

constexpr size_t kMaxContentBytes = 8 * 1024 * 1024;
constexpr size_t kMaxEncodedContentBytes = ((kMaxContentBytes + 2) / 3) * 4;
constexpr size_t kMaxControlRequestBytes = 12 * 1024 * 1024;
constexpr size_t kMaxJsonDepth = 64;

std::string ErrorJson(const std::string& message) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("error");
    writer.String(message.data(), static_cast<rapidjson::SizeType>(message.size()));
    writer.EndObject();
    return buffer.GetString();
}

int32_t ProviderError(int rc) {
    if (rc == 0) return error::OK;
    if (rc == EINVAL) return error::BAD_REQUEST;
    if (rc == ENOENT) return error::NOT_FOUND;
    if (rc == EAGAIN) return error::CONFLICT;
    if (rc == EFBIG) return error::PAYLOAD_TOO_LARGE;
    if (rc == ENODEV) return error::UNAVAILABLE;
    return error::INTERNAL_ERROR;
}

bool ParseObject(const std::string& request, const std::unordered_set<std::string>& allowed,
                 rapidjson::Document* document, std::string* detail) {
    if (!document || !detail) return false;
    if (request.size() > kMaxControlRequestBytes) {
        *detail = "control request exceeds 12 MiB";
        return false;
    }
    if (!JsonNestingWithin(request, kMaxJsonDepth)) {
        *detail = "request JSON nesting depth exceeds 64";
        return false;
    }
    document->Parse<rapidjson::kParseValidateEncodingFlag>(request.data(), request.size());
    if (document->HasParseError() || !document->IsObject()) {
        *detail = "request must be a JSON object";
        return false;
    }
    std::unordered_set<std::string> seen;
    for (auto member = document->MemberBegin(); member != document->MemberEnd(); ++member) {
        const std::string name(member->name.GetString(), member->name.GetStringLength());
        if (!seen.insert(name).second) {
            *detail = "duplicate request field: " + name;
            return false;
        }
        if (allowed.find(name) == allowed.end()) {
            *detail = "unknown request field: " + name;
            return false;
        }
    }
    return true;
}

bool ReadString(const rapidjson::Document& document, const char* name, bool required,
                std::string* value, std::string* detail) {
    const auto member = document.FindMember(name);
    if (member == document.MemberEnd()) {
        if (!required) return true;
        *detail = std::string("missing request field: ") + name;
        return false;
    }
    if (!member->value.IsString()) {
        *detail = std::string("request field must be string: ") + name;
        return false;
    }
    value->assign(member->value.GetString(), member->value.GetStringLength());
    return true;
}

bool ReadUint64(const rapidjson::Document& document, const char* name, bool required,
                uint64_t* value, std::string* detail) {
    const auto member = document.FindMember(name);
    if (member == document.MemberEnd()) {
        if (!required) return true;
        *detail = std::string("missing request field: ") + name;
        return false;
    }
    if (!member->value.IsUint64()) {
        *detail = std::string("request field must be unsigned integer: ") + name;
        return false;
    }
    *value = member->value.GetUint64();
    return true;
}

int Base64Value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

int DecodeBase64(const std::string& encoded, std::string* decoded, std::string* detail) {
    decoded->clear();
    if (encoded.size() > kMaxEncodedContentBytes) {
        *detail = "Base64 content exceeds 8 MiB decoded limit";
        return EFBIG;
    }
    if (encoded.size() % 4 != 0) {
        *detail = "invalid Base64 content length";
        return EINVAL;
    }
    decoded->reserve((encoded.size() / 4) * 3);
    for (size_t offset = 0; offset < encoded.size(); offset += 4) {
        const bool last = offset + 4 == encoded.size();
        const int first = Base64Value(encoded[offset]);
        const int second = Base64Value(encoded[offset + 1]);
        const bool third_padding = encoded[offset + 2] == '=';
        const bool fourth_padding = encoded[offset + 3] == '=';
        const int third = third_padding ? 0 : Base64Value(encoded[offset + 2]);
        const int fourth = fourth_padding ? 0 : Base64Value(encoded[offset + 3]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
            (third_padding && !fourth_padding) || ((third_padding || fourth_padding) && !last) ||
            (third_padding && (second & 0x0f) != 0) ||
            (fourth_padding && !third_padding && (third & 0x03) != 0)) {
            decoded->clear();
            *detail = "invalid Base64 content";
            return EINVAL;
        }
        decoded->push_back(static_cast<char>((first << 2) | (second >> 4)));
        if (!third_padding) decoded->push_back(static_cast<char>((second << 4) | (third >> 2)));
        if (!fourth_padding) decoded->push_back(static_cast<char>((third << 6) | fourth));
    }
    if (decoded->size() > kMaxContentBytes) {
        decoded->clear();
        *detail = "decoded content exceeds 8 MiB";
        return EFBIG;
    }
    return 0;
}

std::string EncodeBase64(const std::string& content) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((content.size() + 2) / 3) * 4);
    for (size_t offset = 0; offset < content.size(); offset += 3) {
        const uint32_t first = static_cast<unsigned char>(content[offset]);
        const bool have_second = offset + 1 < content.size();
        const bool have_third = offset + 2 < content.size();
        const uint32_t second = have_second ? static_cast<unsigned char>(content[offset + 1]) : 0;
        const uint32_t third = have_third ? static_cast<unsigned char>(content[offset + 2]) : 0;
        const uint32_t bits = (first << 16) | (second << 8) | third;
        encoded.push_back(alphabet[(bits >> 18) & 63]);
        encoded.push_back(alphabet[(bits >> 12) & 63]);
        encoded.push_back(have_second ? alphabet[(bits >> 6) & 63] : '=');
        encoded.push_back(have_third ? alphabet[bits & 63] : '=');
    }
    return encoded;
}

bool ParseCursor(const std::string& cursor, uint64_t* revision) {
    if (cursor.empty()) {
        *revision = 0;
        return true;
    }
    if (cursor[0] == '0') return false;
    const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), *revision);
    return parsed.ec == std::errc{} && parsed.ptr == cursor.data() + cursor.size();
}

template <typename Writer>
void WriteSnapshotMeta(Writer* writer, const ConfigChannelSnapshot& snapshot) {
    writer->Key("name");
    writer->String(snapshot.channel_name.c_str());
    writer->Key("revision");
    writer->Uint64(snapshot.revision);
    writer->Key("format");
    writer->String(snapshot.format.c_str());
    writer->Key("schema_id");
    writer->String(snapshot.schema_id.c_str());
    writer->Key("sha256_hex");
    writer->String(snapshot.sha256_hex.c_str());
    writer->Key("content_bytes");
    writer->Uint64(snapshot.content_bytes);
    writer->Key("created_at_unix_ms");
    writer->Int64(snapshot.created_at_unix_ms);
}

}  // namespace

void ConfigChannelPlugin::EnumRoutes(std::function<void(const RouteItem&)> callback) {
    callback({"POST", "/channels/config/list",
              [this](const auto& uri, const auto& request, auto& response) {
                  return HandleList(uri, request, response);
              }});
    callback({"POST", "/channels/config/publish",
              [this](const auto& uri, const auto& request, auto& response) {
                  return HandlePublish(uri, request, response);
              }});
    callback({"POST", "/channels/config/history",
              [this](const auto& uri, const auto& request, auto& response) {
                  return HandleHistory(uri, request, response);
              }});
    callback({"POST", "/channels/config/resolve",
              [this](const auto& uri, const auto& request, auto& response) {
                  return HandleResolve(uri, request, response);
              }});
}

int32_t ConfigChannelPlugin::HandlePublish(const std::string&, const std::string& request,
                                           std::string& response) {
    rapidjson::Document document;
    std::string detail;
    const std::unordered_set<std::string> allowed = {
        "name", "expected_current_revision", "format", "schema_id", "content_base64",
        "base_revision", "original_filename", "change_note"};
    if (!ParseObject(request, allowed, &document, &detail)) {
        response = ErrorJson(detail);
        return request.size() > kMaxControlRequestBytes ? error::PAYLOAD_TOO_LARGE : error::BAD_REQUEST;
    }
    ConfigPublishRequest publish;
    std::string encoded;
    if (!ReadString(document, "name", true, &publish.name, &detail) ||
        !ReadUint64(document, "expected_current_revision", true,
                    &publish.expected_current_revision, &detail) ||
        !ReadString(document, "format", true, &publish.format, &detail) ||
        !ReadString(document, "schema_id", true, &publish.schema_id, &detail) ||
        !ReadString(document, "content_base64", true, &encoded, &detail) ||
        !ReadUint64(document, "base_revision", false, &publish.base_revision, &detail) ||
        !ReadString(document, "original_filename", false, &publish.original_filename, &detail) ||
        !ReadString(document, "change_note", false, &publish.change_note, &detail)) {
        response = ErrorJson(detail);
        return error::BAD_REQUEST;
    }
    const int decode_rc = DecodeBase64(encoded, &publish.content, &detail);
    if (decode_rc != 0) {
        response = ErrorJson(detail);
        return ProviderError(decode_rc);
    }
    ConfigPublishResult result;
    const int rc = Publish(publish, &result, &detail);
    if (rc != 0) {
        response = ErrorJson(detail);
        return ProviderError(rc);
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("revision");
    writer.Uint64(result.revision);
    writer.Key("created_revision");
    writer.Bool(result.created_revision);
    writer.Key("sha256_hex");
    writer.String(result.sha256_hex.c_str());
    writer.Key("content_bytes");
    writer.Uint64(publish.content.size());
    writer.Key("exact_reference");
    const std::string reference = "config." + publish.name + "@" + std::to_string(result.revision);
    writer.String(reference.c_str());
    writer.EndObject();
    response = buffer.GetString();
    return error::OK;
}

int32_t ConfigChannelPlugin::HandleList(const std::string&, const std::string& request,
                                        std::string& response) {
    rapidjson::Document document;
    std::string detail;
    if (!ParseObject(request, {"cursor", "limit"}, &document, &detail)) {
        response = ErrorJson(detail);
        return request.size() > kMaxControlRequestBytes ? error::PAYLOAD_TOO_LARGE : error::BAD_REQUEST;
    }
    std::string cursor;
    uint64_t limit = 100;
    if (!ReadString(document, "cursor", false, &cursor, &detail) ||
        !ReadUint64(document, "limit", false, &limit, &detail) || limit > UINT32_MAX) {
        response = ErrorJson(detail.empty() ? "invalid list limit" : detail);
        return error::BAD_REQUEST;
    }
    std::vector<ConfigChannelListItem> items;
    std::string next_cursor;
    const int rc = ListChannels(cursor, static_cast<uint32_t>(limit), &items, &next_cursor, &detail);
    if (rc != 0) {
        response = ErrorJson(detail);
        return ProviderError(rc);
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("items");
    writer.StartArray();
    for (const auto& item : items) {
        writer.StartObject();
        writer.Key("name"); writer.String(item.name.c_str());
        writer.Key("current_revision"); writer.Uint64(item.current_revision);
        writer.Key("format"); writer.String(item.format.c_str());
        writer.Key("schema_id"); writer.String(item.schema_id.c_str());
        writer.Key("sha256_hex"); writer.String(item.sha256_hex.c_str());
        writer.Key("content_bytes"); writer.Uint64(item.content_bytes);
        writer.Key("updated_at_unix_ms"); writer.Int64(item.updated_at_unix_ms);
        writer.EndObject();
    }
    writer.EndArray();
    writer.Key("next_cursor"); writer.String(next_cursor.c_str());
    writer.EndObject();
    response = buffer.GetString();
    return error::OK;
}

int32_t ConfigChannelPlugin::HandleHistory(const std::string&, const std::string& request,
                                           std::string& response) {
    rapidjson::Document document;
    std::string detail;
    if (!ParseObject(request, {"name", "cursor", "limit"}, &document, &detail)) {
        response = ErrorJson(detail);
        return request.size() > kMaxControlRequestBytes ? error::PAYLOAD_TOO_LARGE : error::BAD_REQUEST;
    }
    std::string name;
    std::string cursor;
    uint64_t limit = 100;
    uint64_t before_revision = 0;
    if (!ReadString(document, "name", true, &name, &detail) ||
        !ReadString(document, "cursor", false, &cursor, &detail) ||
        !ReadUint64(document, "limit", false, &limit, &detail) || limit > UINT32_MAX ||
        !ParseCursor(cursor, &before_revision)) {
        response = ErrorJson(detail.empty() ? "invalid history cursor or limit" : detail);
        return error::BAD_REQUEST;
    }
    std::vector<ConfigRevisionListItem> items;
    uint64_t next_cursor = 0;
    const int rc = ListHistory(name, before_revision, static_cast<uint32_t>(limit),
                               &items, &next_cursor, &detail);
    if (rc != 0) {
        response = ErrorJson(detail);
        return ProviderError(rc);
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("items");
    writer.StartArray();
    for (const auto& item : items) {
        writer.StartObject();
        writer.Key("revision"); writer.Uint64(item.revision);
        writer.Key("format"); writer.String(item.format.c_str());
        writer.Key("schema_id"); writer.String(item.schema_id.c_str());
        writer.Key("sha256_hex"); writer.String(item.sha256_hex.c_str());
        writer.Key("content_bytes"); writer.Uint64(item.content_bytes);
        writer.Key("created_at_unix_ms"); writer.Int64(item.created_at_unix_ms);
        writer.Key("base_revision");
        if (item.base_revision) writer.Uint64(item.base_revision); else writer.Null();
        writer.Key("original_filename"); writer.String(item.original_filename.c_str());
        writer.Key("change_note"); writer.String(item.change_note.c_str());
        writer.EndObject();
    }
    writer.EndArray();
    writer.Key("next_cursor");
    const std::string next = next_cursor ? std::to_string(next_cursor) : "";
    writer.String(next.c_str());
    writer.EndObject();
    response = buffer.GetString();
    return error::OK;
}

int32_t ConfigChannelPlugin::HandleResolve(const std::string&, const std::string& request,
                                           std::string& response) {
    rapidjson::Document document;
    std::string detail;
    if (!ParseObject(request, {"exact_reference"}, &document, &detail)) {
        response = ErrorJson(detail);
        return request.size() > kMaxControlRequestBytes ? error::PAYLOAD_TOO_LARGE : error::BAD_REQUEST;
    }
    std::string reference;
    if (!ReadString(document, "exact_reference", true, &reference, &detail)) {
        response = ErrorJson(detail);
        return error::BAD_REQUEST;
    }
    ConfigChannelSnapshot snapshot;
    const int rc = Resolve(reference.c_str(), &snapshot, &detail);
    if (rc != 0) {
        response = ErrorJson(detail);
        return ProviderError(rc);
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("exact_reference"); writer.String(reference.c_str());
    WriteSnapshotMeta(&writer, snapshot);
    writer.Key("content_base64");
    const std::string encoded = EncodeBase64(*snapshot.content);
    writer.String(encoded.c_str(), static_cast<rapidjson::SizeType>(encoded.size()));
    writer.EndObject();
    response = buffer.GetString();
    return error::OK;
}

}  // namespace flowsql::channels::config
