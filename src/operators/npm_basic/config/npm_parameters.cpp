// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_parameters.h"

#include "npm_basic_task_config.h"

#include <rapidjson/document.h>

#include <charconv>
#include <iterator>
#include <limits>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>

namespace flowsql::npm {
namespace {

NpmParameterStatusV1 Fail(NpmParameterErrorV1 error, std::string path = {}) { return {error, std::move(path)}; }

std::string EscapeJsonPointerToken(std::string_view token) {
    std::string escaped;
    escaped.reserve(token.size());
    for (const char character : token) {
        if (character == '~') {
            escaped += "~0";
        } else if (character == '/') {
            escaped += "~1";
        } else {
            escaped.push_back(character);
        }
    }
    return escaped;
}

std::string ChildPath(const std::string& path, std::string_view token) {
    return path + "/" + EscapeJsonPointerToken(token);
}

void KeepLexicographicallyFirst(NpmParameterStatusV1 candidate, NpmParameterStatusV1* best) {
    if (!best || candidate.error == NpmParameterErrorV1::kNone) return;
    if (best->error == NpmParameterErrorV1::kNone || candidate.path < best->path) {
        *best = std::move(candidate);
    }
}

void ValidateJsonTree(const rapidjson::Value& value, const std::string& path, std::size_t depth,
                      NpmParameterStatusV1* best) {
    if (depth > kNpmParametersMaxDepthV1) {
        KeepLexicographicallyFirst(Fail(NpmParameterErrorV1::kNestingTooDeep, path), best);
        return;
    }
    if (value.IsObject()) {
        for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member) {
            const std::string_view name(member->name.GetString(), member->name.GetStringLength());
            for (auto previous = value.MemberBegin(); previous != member; ++previous) {
                if (name == std::string_view(previous->name.GetString(), previous->name.GetStringLength())) {
                    KeepLexicographicallyFirst(Fail(NpmParameterErrorV1::kDuplicateField, ChildPath(path, name)), best);
                    break;
                }
            }
            ValidateJsonTree(member->value, ChildPath(path, name), depth + 1, best);
        }
        return;
    }
    if (value.IsArray()) {
        for (rapidjson::SizeType index = 0; index < value.Size(); ++index) {
            ValidateJsonTree(value[index], path + "/" + std::to_string(index), depth + 1, best);
        }
    }
}

bool IsAllowedField(std::string_view name, const char* const* fields, std::size_t field_count) {
    for (std::size_t index = 0; index < field_count; ++index) {
        if (name == fields[index]) return true;
    }
    return false;
}

NpmParameterStatusV1 RejectUnknownFields(const rapidjson::Value& object, const std::string& path,
                                         const char* const* fields, std::size_t field_count) {
    bool has_unknown = false;
    std::string first_unknown;
    for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member) {
        const std::string_view name(member->name.GetString(), member->name.GetStringLength());
        if (!IsAllowedField(name, fields, field_count) && (!has_unknown || name < first_unknown)) {
            has_unknown = true;
            first_unknown.assign(name.data(), name.size());
        }
    }
    if (has_unknown) {
        return Fail(NpmParameterErrorV1::kUnknownConsumedField, ChildPath(path, first_unknown));
    }
    return {};
}

const rapidjson::Value* Find(const rapidjson::Value& object, const char* name) {
    const auto member = object.FindMember(name);
    return member == object.MemberEnd() ? nullptr : &member->value;
}

NpmParameterStatusV1 ReadString(const rapidjson::Value& object, const char* name, const std::string& path,
                                const rapidjson::Value** output) {
    const rapidjson::Value* value = Find(object, name);
    if (output) *output = value;
    if (!value) return {};
    if (!value->IsString()) {
        return Fail(NpmParameterErrorV1::kInvalidType, ChildPath(path, name));
    }
    return {};
}

NpmParameterStatusV1 ReadInt64(const rapidjson::Value& object, const char* name, const std::string& path,
                               int64_t* output) {
    const rapidjson::Value* value = Find(object, name);
    if (!value) return {};
    if (!value->IsInt64()) {
        return Fail(NpmParameterErrorV1::kInvalidType, ChildPath(path, name));
    }
    *output = value->GetInt64();
    return {};
}

NpmParameterStatusV1 ReadUint64(const rapidjson::Value& object, const char* name, const std::string& path,
                                uint64_t* output) {
    const rapidjson::Value* value = Find(object, name);
    if (!value) return {};
    if (!value->IsUint64()) {
        return Fail(NpmParameterErrorV1::kInvalidType, ChildPath(path, name));
    }
    *output = value->GetUint64();
    return {};
}

NpmParameterStatusV1 ReadUint32(const rapidjson::Value& object, const char* name, const std::string& path,
                                uint32_t* output) {
    const rapidjson::Value* value = Find(object, name);
    if (!value) return {};
    if (!value->IsUint64()) {
        return Fail(NpmParameterErrorV1::kInvalidType, ChildPath(path, name));
    }
    if (value->GetUint64() > std::numeric_limits<uint32_t>::max()) {
        return Fail(NpmParameterErrorV1::kInvalidRange, ChildPath(path, name));
    }
    *output = static_cast<uint32_t>(value->GetUint64());
    return {};
}

bool IsValidConfigName(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    for (const char character : name) {
        if ((character < 'a' || character > 'z') && (character < '0' || character > '9') && character != '_' &&
            character != '-') {
            return false;
        }
    }
    return true;
}

bool IsValidExactConfigReference(std::string_view reference) {
    constexpr std::string_view prefix = "config.";
    if (reference.substr(0, prefix.size()) != prefix) return false;
    const std::size_t at = reference.find('@', prefix.size());
    if (at == std::string_view::npos || reference.find('@', at + 1) != std::string_view::npos) {
        return false;
    }
    const std::string_view name = reference.substr(prefix.size(), at - prefix.size());
    const std::string_view revision_text = reference.substr(at + 1);
    if (!IsValidConfigName(name) || revision_text.empty() || revision_text.front() == '0') {
        return false;
    }
    uint64_t revision = 0;
    const auto parsed = std::from_chars(revision_text.data(), revision_text.data() + revision_text.size(), revision);
    return parsed.ec == std::errc{} && parsed.ptr == revision_text.data() + revision_text.size() &&
           revision <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
}

bool IsValidLabelingMemoryMiB(uint32_t value) {
    return value >= kNpmMinLabelingMemoryMiB && value <= kNpmMaxLabelingMemoryMiB;
}

NpmParameterStatusV1 ValidateAnalysisRange(const NpmAnalysisConfig& config) {
    switch (ValidateNpmAnalysisConfig(config)) {
        case NpmAnalysisConfigError::kNone:
            return {};
        case NpmAnalysisConfigError::kInvalidRunMode:
            return Fail(NpmParameterErrorV1::kInvalidValue, "/core/run_mode");
        case NpmAnalysisConfigError::kInvalidResultMode:
            return Fail(NpmParameterErrorV1::kInvalidValue, "/core/result_mode");
        case NpmAnalysisConfigError::kOutputIntervalOutOfRange:
            return Fail(NpmParameterErrorV1::kInvalidRange, "/core/output_interval_ns");
        case NpmAnalysisConfigError::kPayloadSamplePacketsOutOfRange:
            return Fail(NpmParameterErrorV1::kInvalidRange, "/core/payload_sample_packets");
        case NpmAnalysisConfigError::kTcpIdleTimeoutOutOfRange:
            return Fail(NpmParameterErrorV1::kInvalidRange, "/core/tcp_idle_timeout_ns");
        case NpmAnalysisConfigError::kUdpIdleTimeoutOutOfRange:
            return Fail(NpmParameterErrorV1::kInvalidRange, "/core/udp_idle_timeout_ns");
        case NpmAnalysisConfigError::kOutOfOrderToleranceOutOfRange:
            return Fail(NpmParameterErrorV1::kInvalidRange, "/core/out_of_order_tolerance_ns");
        case NpmAnalysisConfigError::kActiveSessionsOutOfRange:
            return Fail(NpmParameterErrorV1::kInvalidRange, "/core/max_active_sessions");
        case NpmAnalysisConfigError::kTrackedBytesOutOfRange:
            return Fail(NpmParameterErrorV1::kInvalidRange, "/core/max_tracked_bytes");
        case NpmAnalysisConfigError::kPendingOutputBytesOutOfRange:
            return Fail(NpmParameterErrorV1::kInvalidRange, "/core/max_pending_output_bytes");
        case NpmAnalysisConfigError::kUnsupportedOverloadPolicy:
            return Fail(NpmParameterErrorV1::kInvalidValue, "/core/overload_policy");
    }
    return Fail(NpmParameterErrorV1::kInvalidValue, "/core");
}

NpmParameterStatusV1 ParseCore(const rapidjson::Value* value, const NpmParameterConsumersV1& consumers,
                               NpmCoreParametersV1* output) {
    output->analysis = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    output->labeling_reference.reset();
    output->labeling_memory_mib.reset();
    if (consumers.labeling_enabled && consumers.labeling_available) {
        output->labeling_memory_mib = kNpmDefaultLabelingMemoryMiB;
    }
    if (!value) return {};
    if (!value->IsObject()) {
        return Fail(NpmParameterErrorV1::kInvalidType, "/core");
    }

    static constexpr const char* kFields[] = {
        "run_mode",
        "result_mode",
        "overload_policy",
        "output_interval_ns",
        "payload_sample_packets",
        "tcp_idle_timeout_ns",
        "udp_idle_timeout_ns",
        "out_of_order_tolerance_ns",
        "max_active_sessions",
        "max_tracked_bytes",
        "max_pending_output_bytes",
        "labeling",
        "labeling_memory_mib",
    };
    auto status = RejectUnknownFields(*value, "/core", kFields, std::size(kFields));
    if (status.error != NpmParameterErrorV1::kNone) return status;

    const rapidjson::Value* text = nullptr;
    status = ReadString(*value, "run_mode", "/core", &text);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    if (text) {
        const std::string_view mode(text->GetString(), text->GetStringLength());
        if (mode == "offline") {
            output->analysis = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
        } else if (mode == "realtime") {
            output->analysis = DefaultNpmAnalysisConfig(NpmRunMode::kRealtime);
        } else {
            return Fail(NpmParameterErrorV1::kInvalidValue, "/core/run_mode");
        }
    }

    status = ReadString(*value, "result_mode", "/core", &text);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    if (text) {
        const std::string_view mode(text->GetString(), text->GetStringLength());
        if (mode == "final") {
            output->analysis.result_mode = NpmResultMode::kFinal;
        } else if (mode == "periodic_snapshot") {
            output->analysis.result_mode = NpmResultMode::kPeriodicSnapshot;
        } else {
            return Fail(NpmParameterErrorV1::kInvalidValue, "/core/result_mode");
        }
    }

    status = ReadString(*value, "overload_policy", "/core", &text);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    if (text) {
        const std::string_view policy(text->GetString(), text->GetStringLength());
        if (policy != "fail") {
            return Fail(NpmParameterErrorV1::kInvalidValue, "/core/overload_policy");
        }
        output->analysis.overload_policy = NpmOverloadPolicy::kFail;
    }

    status = ReadInt64(*value, "output_interval_ns", "/core", &output->analysis.output_interval_ns);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    status = ReadUint32(*value, "payload_sample_packets", "/core", &output->analysis.payload_sample_packets);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    status = ReadInt64(*value, "tcp_idle_timeout_ns", "/core", &output->analysis.tcp_idle_timeout_ns);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    status = ReadInt64(*value, "udp_idle_timeout_ns", "/core", &output->analysis.udp_idle_timeout_ns);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    status = ReadInt64(*value, "out_of_order_tolerance_ns", "/core", &output->analysis.out_of_order_tolerance_ns);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    status = ReadUint64(*value, "max_active_sessions", "/core", &output->analysis.max_active_sessions);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    status = ReadUint64(*value, "max_tracked_bytes", "/core", &output->analysis.max_tracked_bytes);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    status = ReadUint64(*value, "max_pending_output_bytes", "/core", &output->analysis.max_pending_output_bytes);
    if (status.error != NpmParameterErrorV1::kNone) return status;

    status = ValidateAnalysisRange(output->analysis);
    if (status.error != NpmParameterErrorV1::kNone) return status;

    if (consumers.labeling_enabled && consumers.labeling_available) {
        status = ReadString(*value, "labeling", "/core", &text);
        if (status.error != NpmParameterErrorV1::kNone) return status;
        if (text) {
            const std::string_view reference(text->GetString(), text->GetStringLength());
            if (!IsValidExactConfigReference(reference)) {
                return Fail(NpmParameterErrorV1::kInvalidExactReference, "/core/labeling");
            }
            output->labeling_reference.emplace(reference);
        }
        uint32_t labeling_memory_mib = *output->labeling_memory_mib;
        status = ReadUint32(*value, "labeling_memory_mib", "/core", &labeling_memory_mib);
        if (status.error != NpmParameterErrorV1::kNone) return status;
        if (!IsValidLabelingMemoryMiB(labeling_memory_mib) ||
            output->analysis.max_tracked_bytes < static_cast<uint64_t>(labeling_memory_mib) * 2 * kNpmMebibyte) {
            return Fail(NpmParameterErrorV1::kInvalidRange, "/core/labeling_memory_mib");
        }
        output->labeling_memory_mib = labeling_memory_mib;
    }
    return {};
}

NpmParameterStatusV1 ValidateModuleNodes(const rapidjson::Value& root) {
    bool has_invalid = false;
    std::string first_invalid;
    for (auto member = root.MemberBegin(); member != root.MemberEnd(); ++member) {
        const std::string_view name(member->name.GetString(), member->name.GetStringLength());
        if (name == "schema_version" || name == "core") continue;
        if (name == "framework") {
            return Fail(NpmParameterErrorV1::kUnknownConsumedField, "/framework");
        }
        if (!member->value.IsObject() && (!has_invalid || name < first_invalid)) {
            has_invalid = true;
            first_invalid.assign(name.data(), name.size());
        }
    }
    return has_invalid ? Fail(NpmParameterErrorV1::kInvalidType, ChildPath("", first_invalid)) : NpmParameterStatusV1{};
}

NpmParameterStatusV1 ParseBasic(const rapidjson::Value* value, NpmBasicModuleParametersV1* output) {
    if (!value) {
        *output = {};
        return {};
    }
    const auto status = RejectUnknownFields(*value, "/basic", nullptr, 0);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    *output = {};
    return {};
}

NpmParameterStatusV1 ParseSession(const rapidjson::Value* value, NpmSessionModuleParametersV1* output) {
    *output = {};
    if (!value) return {};
    static constexpr const char* kFields[] = {"max_tcp_ranges_per_direction"};
    auto status = RejectUnknownFields(*value, "/session", kFields, std::size(kFields));
    if (status.error != NpmParameterErrorV1::kNone) return status;
    status = ReadUint32(*value, "max_tcp_ranges_per_direction", "/session", &output->max_tcp_ranges_per_direction);
    if (status.error != NpmParameterErrorV1::kNone) return status;
    if (output->max_tcp_ranges_per_direction < kNpmMinSessionTcpRangesPerDirection ||
        output->max_tcp_ranges_per_direction > kNpmMaxSessionTcpRangesPerDirection) {
        return Fail(NpmParameterErrorV1::kInvalidRange, "/session/max_tcp_ranges_per_direction");
    }
    return {};
}

}  // namespace

NpmParameterStatusV1 ParseNpmParametersV1(const char* parameters_json, const NpmParameterConsumersV1& consumers,
                                          NpmTaskParametersV1* output) {
    if (!parameters_json) return Fail(NpmParameterErrorV1::kNullInput);
    if (parameters_json[0] == '\0') return Fail(NpmParameterErrorV1::kEmptyInput);
    if (!output) return Fail(NpmParameterErrorV1::kNullOutput);
    std::size_t json_size = 0;
    while (json_size <= kNpmParametersMaxJsonBytesV1 && parameters_json[json_size] != '\0') {
        ++json_size;
    }
    if (json_size > kNpmParametersMaxJsonBytesV1) {
        return Fail(NpmParameterErrorV1::kJsonTooLarge);
    }

    try {
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(parameters_json);
        if (document.HasParseError()) return Fail(NpmParameterErrorV1::kInvalidJson);
        if (!document.IsObject()) return Fail(NpmParameterErrorV1::kInvalidEnvelope);

        NpmParameterStatusV1 structural_status;
        ValidateJsonTree(document, "", 0, &structural_status);
        if (structural_status.error != NpmParameterErrorV1::kNone) return structural_status;

        const rapidjson::Value* schema_version = Find(document, "schema_version");
        if (!schema_version) {
            return Fail(NpmParameterErrorV1::kMissingSchemaVersion, "/schema_version");
        }
        if (!schema_version->IsUint()) {
            return Fail(NpmParameterErrorV1::kInvalidType, "/schema_version");
        }
        if (schema_version->GetUint() != 1) {
            return Fail(NpmParameterErrorV1::kUnsupportedVersion, "/schema_version");
        }

        auto status = ValidateModuleNodes(document);
        if (status.error != NpmParameterErrorV1::kNone) return status;
        if (consumers.basic_enabled && !consumers.basic_available) {
            return Fail(NpmParameterErrorV1::kUnavailableFeature, "/basic");
        }
        if (consumers.labeling_enabled && !consumers.labeling_available) {
            return Fail(NpmParameterErrorV1::kUnavailableFeature, "/labeling");
        }
        if (consumers.session_enabled && !consumers.session_available) {
            return Fail(NpmParameterErrorV1::kUnavailableFeature, "/session");
        }

        NpmTaskParametersV1 next;
        next.schema_version = 1;
        next.source = NpmParameterSourceV1::kParametersV1;
        status = ParseCore(Find(document, "core"), consumers, &next.core);
        if (status.error != NpmParameterErrorV1::kNone) return status;

        if (consumers.basic_enabled) {
            NpmBasicModuleParametersV1 basic;
            status = ParseBasic(Find(document, "basic"), &basic);
            if (status.error != NpmParameterErrorV1::kNone) return status;
            next.basic = std::move(basic);
        }
        if (consumers.session_enabled) {
            NpmSessionModuleParametersV1 session;
            status = ParseSession(Find(document, "session"), &session);
            if (status.error != NpmParameterErrorV1::kNone) return status;
            next.session = std::move(session);
        }

        *output = std::move(next);
        return {};
    } catch (const std::bad_alloc&) {
        return Fail(NpmParameterErrorV1::kAllocationFailed);
    }
}

}  // namespace flowsql::npm
