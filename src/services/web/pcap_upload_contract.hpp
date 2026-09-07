// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_SERVICES_WEB_PCAP_UPLOAD_CONTRACT_HPP_
#define FLOWSQL_SERVICES_WEB_PCAP_UPLOAD_CONTRACT_HPP_

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <system_error>
#include <utility>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace flowsql::web {

constexpr uint64_t kDefaultPcapUploadMaxBytes = 1ULL << 30;

struct PcapUploadRequest {
    std::string channel_name;
    std::string original_filename;
    std::string format = "auto";
    uint32_t batch_packets = 256;
    std::string replay_mode = "fast";
    uint32_t replay_speed_milli = 1000;
};

struct ManagedCaptureRef {
    std::string channel_name;
    std::string original_filename;
    std::filesystem::path canonical_path;
    uint64_t size_bytes = 0;
};

enum class PcapUploadError {
    kOk = 0,
    kInvalidRequest,
    kConflict,
    kTooLarge,
    kUnavailable,
    kStorageFailure,
    kInternal,
};

struct PcapUploadErrorInfo {
    int http_status;
    const char* error;
};

using PcapUploadFields = std::map<std::string, std::string>;

PcapUploadError ParsePcapUploadFields(const PcapUploadFields& fields,
                                      const std::string& original_filename,
                                      PcapUploadRequest* output,
                                      std::string* message);

PcapUploadError CheckPcapUploadSize(uint64_t current_size,
                                    size_t chunk_size,
                                    uint64_t max_bytes,
                                    uint64_t* next_size);

PcapUploadErrorInfo MapPcapUploadError(PcapUploadError error);

std::string BuildSchedulerPcapAddJson(const PcapUploadRequest& request,
                                      const ManagedCaptureRef& capture);

std::string BuildPublicPcapUploadJson(const ManagedCaptureRef& capture,
                                      const std::string& status);

namespace detail {

inline PcapUploadError InvalidField(const std::string& text, std::string* message) {
    if (message) *message = text;
    return PcapUploadError::kInvalidRequest;
}

inline bool IsSafeLogicalName(const std::string& value) {
    if (value.empty() || value == "." || value == "..") return false;
    for (unsigned char ch : value) {
        if (ch == '/' || ch == '\\' || ch < 0x20 || ch == 0x7f) return false;
    }
    return true;
}

inline bool IsSupportedCaptureFilename(const std::string& value) {
    if (value.empty() || value == "." || value == ".." || value.find('/') != std::string::npos ||
        value.find('\\') != std::string::npos) {
        return false;
    }
    const size_t dot = value.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == value.size()) return false;
    std::string extension = value.substr(dot);
    for (char& ch : extension) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return extension == ".pcap" || extension == ".pcapng";
}

inline bool ParsePositiveUint32(const std::string& text, uint32_t* output) {
    if (!output || text.empty()) return false;
    uint64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc() || result.ptr != end || parsed == 0 ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    *output = static_cast<uint32_t>(parsed);
    return true;
}

inline bool IsAllowedField(const std::string& key) {
    return key == "name" || key == "format" || key == "batch_packets" || key == "replay_mode" ||
           key == "replay_speed_milli";
}

}  // namespace detail

inline PcapUploadError ParsePcapUploadFields(const PcapUploadFields& fields,
                                             const std::string& original_filename,
                                             PcapUploadRequest* output,
                                             std::string* message) {
    if (message) message->clear();
    if (!output) {
        if (message) *message = "output is null";
        return PcapUploadError::kInternal;
    }
    for (const auto& [key, value] : fields) {
        (void)value;
        if (!detail::IsAllowedField(key)) return detail::InvalidField("unknown field: " + key, message);
    }

    const auto name = fields.find("name");
    if (name == fields.end() || !detail::IsSafeLogicalName(name->second)) {
        return detail::InvalidField("invalid channel name", message);
    }
    if (!detail::IsSupportedCaptureFilename(original_filename)) {
        return detail::InvalidField("invalid capture filename", message);
    }

    PcapUploadRequest parsed;
    parsed.channel_name = name->second;
    parsed.original_filename = original_filename;

    const auto format = fields.find("format");
    if (format != fields.end()) {
        if (format->second != "auto" && format->second != "pcap" && format->second != "pcapng") {
            return detail::InvalidField("invalid capture format", message);
        }
        parsed.format = format->second;
    }

    const auto batch_packets = fields.find("batch_packets");
    if (batch_packets != fields.end() &&
        !detail::ParsePositiveUint32(batch_packets->second, &parsed.batch_packets)) {
        return detail::InvalidField("invalid batch_packets", message);
    }

    const auto replay_mode = fields.find("replay_mode");
    if (replay_mode != fields.end()) {
        if (replay_mode->second != "fast" && replay_mode->second != "timestamp") {
            return detail::InvalidField("invalid replay_mode", message);
        }
        parsed.replay_mode = replay_mode->second;
    }

    const auto replay_speed = fields.find("replay_speed_milli");
    if (replay_speed != fields.end() &&
        !detail::ParsePositiveUint32(replay_speed->second, &parsed.replay_speed_milli)) {
        return detail::InvalidField("invalid replay_speed_milli", message);
    }

    *output = std::move(parsed);
    return PcapUploadError::kOk;
}

inline PcapUploadError CheckPcapUploadSize(uint64_t current_size,
                                           size_t chunk_size,
                                           uint64_t max_bytes,
                                           uint64_t* next_size) {
    if (!next_size) return PcapUploadError::kInternal;
    if constexpr (sizeof(size_t) > sizeof(uint64_t)) {
        if (chunk_size > std::numeric_limits<uint64_t>::max()) return PcapUploadError::kTooLarge;
    }
    const uint64_t chunk = static_cast<uint64_t>(chunk_size);
    if (current_size > max_bytes || chunk > max_bytes - current_size) return PcapUploadError::kTooLarge;
    *next_size = current_size + chunk;
    return PcapUploadError::kOk;
}

inline PcapUploadErrorInfo MapPcapUploadError(PcapUploadError error) {
    switch (error) {
        case PcapUploadError::kOk:
            return {200, ""};
        case PcapUploadError::kInvalidRequest:
            return {400, "invalid_request"};
        case PcapUploadError::kConflict:
            return {409, "channel_conflict"};
        case PcapUploadError::kTooLarge:
            return {413, "file_too_large"};
        case PcapUploadError::kUnavailable:
            return {503, "provider_unavailable"};
        case PcapUploadError::kStorageFailure:
            return {500, "storage_failure"};
        case PcapUploadError::kInternal:
            return {500, "internal_error"};
    }
    return {500, "internal_error"};
}

inline std::string BuildSchedulerPcapAddJson(const PcapUploadRequest& request,
                                             const ManagedCaptureRef& capture) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    const std::string path = capture.canonical_path.string();
    writer.StartObject();
    writer.Key("type");
    writer.String("pcapfile");
    writer.Key("name");
    writer.String(request.channel_name.c_str());
    writer.Key("role");
    writer.String("source");
    writer.Key("options");
    writer.StartObject();
    writer.Key("path");
    writer.String(path.c_str());
    writer.Key("format");
    writer.String(request.format.c_str());
    writer.Key("batch_packets");
    writer.Uint(request.batch_packets);
    writer.Key("replay_mode");
    writer.String(request.replay_mode.c_str());
    writer.Key("replay_speed_milli");
    writer.Uint(request.replay_speed_milli);
    writer.EndObject();
    writer.EndObject();
    return buffer.GetString();
}

inline std::string BuildPublicPcapUploadJson(const ManagedCaptureRef& capture,
                                             const std::string& status) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("type");
    writer.String("pcapfile");
    writer.Key("name");
    writer.String(capture.channel_name.c_str());
    writer.Key("role");
    writer.String("source");
    writer.Key("status");
    writer.String(status.c_str());
    writer.Key("filename");
    writer.String(capture.original_filename.c_str());
    writer.Key("size_bytes");
    writer.Uint64(capture.size_bytes);
    writer.EndObject();
    return buffer.GetString();
}

}  // namespace flowsql::web

#endif  // FLOWSQL_SERVICES_WEB_PCAP_UPLOAD_CONTRACT_HPP_
