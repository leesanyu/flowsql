// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_task_config.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace flowsql::npm {
namespace {

enum class TaskConfigField : size_t {
    kRunMode = 0,
    kResultMode,
    kOverloadPolicy,
    kOutputIntervalNs,
    kPayloadSamplePackets,
    kTcpIdleTimeoutNs,
    kUdpIdleTimeoutNs,
    kOutOfOrderToleranceNs,
    kMaxActiveSessions,
    kMaxTrackedBytes,
    kMaxPendingOutputBytes,
    kFeatures,
    kObserving,
    kSessionMaxTcpRangesPerDirection,
    kInputNamespace,
    kSourceDomains,
    kParameters,
    kCount,
};

constexpr std::array<const char*, static_cast<size_t>(TaskConfigField::kCount)> kTaskConfigFieldNames = {
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
    "features",
    "observing",
    "session_max_tcp_ranges_per_direction",
    "input_namespace",
    "source_domains",
    "parameters",
};

constexpr std::array<TaskConfigField, 12> kLegacyTuningFields = {
    TaskConfigField::kRunMode,
    TaskConfigField::kResultMode,
    TaskConfigField::kOverloadPolicy,
    TaskConfigField::kOutputIntervalNs,
    TaskConfigField::kPayloadSamplePackets,
    TaskConfigField::kTcpIdleTimeoutNs,
    TaskConfigField::kUdpIdleTimeoutNs,
    TaskConfigField::kOutOfOrderToleranceNs,
    TaskConfigField::kMaxActiveSessions,
    TaskConfigField::kMaxTrackedBytes,
    TaskConfigField::kMaxPendingOutputBytes,
    TaskConfigField::kSessionMaxTcpRangesPerDirection,
};

size_t FieldIndex(TaskConfigField field) { return static_cast<size_t>(field); }

const char* FieldName(TaskConfigField field) { return kTaskConfigFieldNames[FieldIndex(field)]; }

int FindField(std::string_view name) {
    for (size_t index = 0; index < kTaskConfigFieldNames.size(); ++index) {
        if (name == kTaskConfigFieldNames[index]) return static_cast<int>(index);
    }
    return -1;
}

NpmBasicTaskConfigStatus Fail(NpmBasicTaskConfigError error, std::string field = {}) {
    NpmBasicTaskConfigStatus status;
    status.error = error;
    status.field = std::move(field);
    return status;
}

bool ParseUnsignedDecimal(std::string_view text, uint64_t* output) {
    if (text.empty() || output == nullptr) return false;
    uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(character - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *output = value;
    return true;
}

bool ParseInt64(std::string_view text, int64_t* output) {
    uint64_t value = 0;
    if (!ParseUnsignedDecimal(text, &value) || value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    *output = static_cast<int64_t>(value);
    return true;
}

bool ParseUint32(std::string_view text, uint32_t* output) {
    uint64_t value = 0;
    if (!ParseUnsignedDecimal(text, &value) || value > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    *output = static_cast<uint32_t>(value);
    return true;
}

bool IsAsciiWhitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' ||
           character == '\v';
}

std::string_view TrimAsciiWhitespace(std::string_view text) {
    while (!text.empty() && IsAsciiWhitespace(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsAsciiWhitespace(text.back())) text.remove_suffix(1);
    return text;
}

bool ParseFeatures(std::string_view text, NpmBasicFeatureConfig* config, const NpmModuleCatalogV1& catalog) {
    if (config == nullptr) return false;
    NpmBasicFeatureConfig next;
    next.basic_enabled = false;
    next.session_enabled = false;
    next.labeling_enabled = false;

    size_t token_begin = 0;
    while (token_begin <= text.size()) {
        const size_t separator = text.find(',', token_begin);
        const size_t token_end = separator == std::string_view::npos ? text.size() : separator;
        const std::string_view token = TrimAsciiWhitespace(text.substr(token_begin, token_end - token_begin));
        if (token.empty()) return false;
        if (token == "labeling") {
            if (next.labeling_enabled) return false;
            next.labeling_enabled = true;
        } else {
            const auto entry =
                std::find_if(catalog.begin(), catalog.end(), [&](const auto& item) { return item.module_id == token; });
            if (entry == catalog.end() || !entry->available ||
                std::find(next.module_ids.begin(), next.module_ids.end(), token) != next.module_ids.end())
                return false;
            next.module_ids.emplace_back(token);
            if (token == "basic") next.basic_enabled = true;
            if (token == "session") next.session_enabled = true;
        }
        if (separator == std::string_view::npos) break;
        token_begin = separator + 1;
    }

    config->basic_enabled = next.basic_enabled;
    config->session_enabled = next.session_enabled;
    config->labeling_enabled = next.labeling_enabled;
    config->module_ids = std::move(next.module_ids);
    return !config->module_ids.empty();
}

NpmBasicTaskConfigStatus ParseFeatureConfig(
    const std::array<const rapidjson::Value*, static_cast<size_t>(TaskConfigField::kCount)>& values,
    NpmBasicFeatureConfig* config, const NpmModuleCatalogV1& catalog) {
    if (ValidateNpmModuleCatalogV1(catalog).error != NpmProtocolContractErrorV1::kNone)
        return Fail(NpmBasicTaskConfigError::kInvalidFeatures, "catalog");
    const rapidjson::Value* features = values[FieldIndex(TaskConfigField::kFeatures)];
    const std::string_view text =
        features ? std::string_view(features->GetString(), features->GetStringLength()) : "basic";
    if (!ParseFeatures(text, config, catalog)) return Fail(NpmBasicTaskConfigError::kInvalidFeatures, "features");
    const rapidjson::Value* observing = values[FieldIndex(TaskConfigField::kObserving)];
    config->observing_entity =
        observing
            ? std::string(TrimAsciiWhitespace(std::string_view(observing->GetString(), observing->GetStringLength())))
            : "basic";
    const auto owner = std::find_if(catalog.begin(), catalog.end(), [&](const auto& entry) {
        return std::find(entry.entity_ids.begin(), entry.entity_ids.end(), config->observing_entity) !=
               entry.entity_ids.end();
    });
    if (owner == catalog.end()) return Fail(NpmBasicTaskConfigError::kInvalidObserving, "observing");
    if (std::find(config->module_ids.begin(), config->module_ids.end(), owner->module_id) == config->module_ids.end())
        return Fail(NpmBasicTaskConfigError::kObservingFeatureDisabled, "observing");
    config->observing = config->observing_entity == "basic"     ? NpmResultEntity::kBasic
                        : config->observing_entity == "session" ? NpmResultEntity::kSession
                                                                : NpmResultEntity::kProtocol;

    const rapidjson::Value* range_limit = values[FieldIndex(TaskConfigField::kSessionMaxTcpRangesPerDirection)];
    if (range_limit == nullptr) return {};
    if (!ParseUint32(std::string_view(range_limit->GetString(), range_limit->GetStringLength()),
                     &config->session_max_tcp_ranges_per_direction)) {
        return Fail(NpmBasicTaskConfigError::kInvalidInteger,
                    FieldName(TaskConfigField::kSessionMaxTcpRangesPerDirection));
    }
    if (!config->session_enabled) {
        return Fail(NpmBasicTaskConfigError::kSessionConfigWithoutFeature,
                    FieldName(TaskConfigField::kSessionMaxTcpRangesPerDirection));
    }
    if (config->session_max_tcp_ranges_per_direction < kNpmMinSessionTcpRangesPerDirection ||
        config->session_max_tcp_ranges_per_direction > kNpmMaxSessionTcpRangesPerDirection) {
        return Fail(NpmBasicTaskConfigError::kSessionTcpRangesOutOfRange,
                    FieldName(TaskConfigField::kSessionMaxTcpRangesPerDirection));
    }
    return {};
}

bool ParseSourceDomains(std::string_view text, std::vector<NpmSourceDomainBinding>* bindings) {
    if (text.empty() || bindings == nullptr) return false;
    size_t item_begin = 0;
    while (item_begin < text.size()) {
        const size_t separator = text.find(';', item_begin);
        const size_t item_end = separator == std::string_view::npos ? text.size() : separator;
        if (item_begin == item_end) return false;

        const size_t colon = text.find(':', item_begin);
        if (colon == std::string_view::npos || colon == item_begin || colon >= item_end ||
            text.find(':', colon + 1) < item_end) {
            return false;
        }

        uint32_t source_id = 0;
        uint64_t observation_domain_id = 0;
        if (!ParseUint32(text.substr(item_begin, colon - item_begin), &source_id) ||
            !ParseUnsignedDecimal(text.substr(colon + 1, item_end - colon - 1), &observation_domain_id)) {
            return false;
        }
        bindings->push_back({source_id, observation_domain_id});

        if (separator == std::string_view::npos) break;
        item_begin = separator + 1;
        if (item_begin == text.size()) return false;
    }
    return !bindings->empty();
}

NpmBasicTaskConfigStatus ParseDomainConfig(
    const std::array<const rapidjson::Value*, static_cast<size_t>(TaskConfigField::kCount)>& values,
    NpmObservationDomainMap* domains) {
    const rapidjson::Value* input_namespace = values[FieldIndex(TaskConfigField::kInputNamespace)];
    domains->input_namespace.assign(input_namespace->GetString(), input_namespace->GetStringLength());
    const rapidjson::Value* source_domains = values[FieldIndex(TaskConfigField::kSourceDomains)];
    if (!ParseSourceDomains(std::string_view(source_domains->GetString(), source_domains->GetStringLength()),
                            &domains->bindings)) {
        return Fail(NpmBasicTaskConfigError::kInvalidSourceDomains, FieldName(TaskConfigField::kSourceDomains));
    }

    const auto domain_error = ValidateNpmObservationDomainMap(*domains);
    if (domain_error == NpmObservationDomainError::kNone) return {};
    auto status = Fail(NpmBasicTaskConfigError::kDomainValidationError,
                       domain_error == NpmObservationDomainError::kEmptyInputNamespace
                           ? FieldName(TaskConfigField::kInputNamespace)
                           : FieldName(TaskConfigField::kSourceDomains));
    status.domain_error = domain_error;
    return status;
}

const char* AnalysisErrorField(NpmAnalysisConfigError error) {
    switch (error) {
        case NpmAnalysisConfigError::kInvalidRunMode:
            return FieldName(TaskConfigField::kRunMode);
        case NpmAnalysisConfigError::kInvalidResultMode:
            return FieldName(TaskConfigField::kResultMode);
        case NpmAnalysisConfigError::kOutputIntervalOutOfRange:
            return FieldName(TaskConfigField::kOutputIntervalNs);
        case NpmAnalysisConfigError::kPayloadSamplePacketsOutOfRange:
            return FieldName(TaskConfigField::kPayloadSamplePackets);
        case NpmAnalysisConfigError::kTcpIdleTimeoutOutOfRange:
            return FieldName(TaskConfigField::kTcpIdleTimeoutNs);
        case NpmAnalysisConfigError::kUdpIdleTimeoutOutOfRange:
            return FieldName(TaskConfigField::kUdpIdleTimeoutNs);
        case NpmAnalysisConfigError::kOutOfOrderToleranceOutOfRange:
            return FieldName(TaskConfigField::kOutOfOrderToleranceNs);
        case NpmAnalysisConfigError::kActiveSessionsOutOfRange:
            return FieldName(TaskConfigField::kMaxActiveSessions);
        case NpmAnalysisConfigError::kTrackedBytesOutOfRange:
            return FieldName(TaskConfigField::kMaxTrackedBytes);
        case NpmAnalysisConfigError::kPendingOutputBytesOutOfRange:
            return FieldName(TaskConfigField::kMaxPendingOutputBytes);
        case NpmAnalysisConfigError::kUnsupportedOverloadPolicy:
            return FieldName(TaskConfigField::kOverloadPolicy);
        case NpmAnalysisConfigError::kNone:
            return "";
    }
    return "";
}

NpmBasicTaskConfigStatus ParseIntegerFields(
    const std::array<const rapidjson::Value*, static_cast<size_t>(TaskConfigField::kCount)>& values,
    NpmAnalysisConfig* config) {
    const auto parse_i64 = [&](TaskConfigField field, int64_t* output) {
        const rapidjson::Value* value = values[FieldIndex(field)];
        if (value == nullptr) return true;
        return ParseInt64(std::string_view(value->GetString(), value->GetStringLength()), output);
    };
    const auto parse_u64 = [&](TaskConfigField field, uint64_t* output) {
        const rapidjson::Value* value = values[FieldIndex(field)];
        if (value == nullptr) return true;
        return ParseUnsignedDecimal(std::string_view(value->GetString(), value->GetStringLength()), output);
    };
    const auto parse_u32 = [&](TaskConfigField field, uint32_t* output) {
        const rapidjson::Value* value = values[FieldIndex(field)];
        if (value == nullptr) return true;
        return ParseUint32(std::string_view(value->GetString(), value->GetStringLength()), output);
    };

    if (!parse_i64(TaskConfigField::kOutputIntervalNs, &config->output_interval_ns)) {
        return Fail(NpmBasicTaskConfigError::kInvalidInteger, FieldName(TaskConfigField::kOutputIntervalNs));
    }
    if (!parse_u32(TaskConfigField::kPayloadSamplePackets, &config->payload_sample_packets)) {
        return Fail(NpmBasicTaskConfigError::kInvalidInteger, FieldName(TaskConfigField::kPayloadSamplePackets));
    }
    if (!parse_i64(TaskConfigField::kTcpIdleTimeoutNs, &config->tcp_idle_timeout_ns)) {
        return Fail(NpmBasicTaskConfigError::kInvalidInteger, FieldName(TaskConfigField::kTcpIdleTimeoutNs));
    }
    if (!parse_i64(TaskConfigField::kUdpIdleTimeoutNs, &config->udp_idle_timeout_ns)) {
        return Fail(NpmBasicTaskConfigError::kInvalidInteger, FieldName(TaskConfigField::kUdpIdleTimeoutNs));
    }
    if (!parse_i64(TaskConfigField::kOutOfOrderToleranceNs, &config->out_of_order_tolerance_ns)) {
        return Fail(NpmBasicTaskConfigError::kInvalidInteger, FieldName(TaskConfigField::kOutOfOrderToleranceNs));
    }
    if (!parse_u64(TaskConfigField::kMaxActiveSessions, &config->max_active_sessions)) {
        return Fail(NpmBasicTaskConfigError::kInvalidInteger, FieldName(TaskConfigField::kMaxActiveSessions));
    }
    if (!parse_u64(TaskConfigField::kMaxTrackedBytes, &config->max_tracked_bytes)) {
        return Fail(NpmBasicTaskConfigError::kInvalidInteger, FieldName(TaskConfigField::kMaxTrackedBytes));
    }
    if (!parse_u64(TaskConfigField::kMaxPendingOutputBytes, &config->max_pending_output_bytes)) {
        return Fail(NpmBasicTaskConfigError::kInvalidInteger, FieldName(TaskConfigField::kMaxPendingOutputBytes));
    }
    return {};
}

}  // namespace

NpmBasicTaskConfigStatus ParseNpmBasicTaskConfig(const char* with_params_json, NpmBasicTaskConfig* output,
                                                 bool labeling_available, const NpmModuleCatalogV1& catalog) {
    if (with_params_json == nullptr) return Fail(NpmBasicTaskConfigError::kNullInput);
    if (with_params_json[0] == '\0') return Fail(NpmBasicTaskConfigError::kEmptyInput);
    if (output == nullptr) return Fail(NpmBasicTaskConfigError::kNullOutput);

    try {
        rapidjson::Document document;
        document.Parse(with_params_json);
        if (document.HasParseError() || !document.IsObject()) {
            return Fail(NpmBasicTaskConfigError::kInvalidJson);
        }

        std::array<const rapidjson::Value*, static_cast<size_t>(TaskConfigField::kCount)> values{};
        for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
            const std::string_view name(member->name.GetString(), member->name.GetStringLength());
            const int field_index = FindField(name);
            if (field_index < 0) {
                return Fail(NpmBasicTaskConfigError::kUnknownField, std::string(name));
            }
            if (values[static_cast<size_t>(field_index)] != nullptr) {
                return Fail(NpmBasicTaskConfigError::kDuplicateField, std::string(name));
            }
            if (!member->value.IsString()) {
                return Fail(NpmBasicTaskConfigError::kNonStringValue, std::string(name));
            }
            values[static_cast<size_t>(field_index)] = &member->value;
        }

        const auto input_namespace_index = FieldIndex(TaskConfigField::kInputNamespace);
        const auto source_domains_index = FieldIndex(TaskConfigField::kSourceDomains);
        if (values[input_namespace_index] == nullptr) {
            return Fail(NpmBasicTaskConfigError::kMissingRequiredField, FieldName(TaskConfigField::kInputNamespace));
        }
        if (values[source_domains_index] == nullptr) {
            return Fail(NpmBasicTaskConfigError::kMissingRequiredField, FieldName(TaskConfigField::kSourceDomains));
        }

        const rapidjson::Value* parameters = values[FieldIndex(TaskConfigField::kParameters)];
        if (parameters != nullptr) {
            for (const TaskConfigField legacy_field : kLegacyTuningFields) {
                if (values[FieldIndex(legacy_field)] == nullptr) continue;
                auto status = Fail(NpmBasicTaskConfigError::kParameterSourceConflict, FieldName(legacy_field));
                status.parameter_status.error = NpmParameterErrorV1::kLegacyConflict;
                status.parameter_status.path = std::string("/") + FieldName(legacy_field);
                return status;
            }

            NpmBasicTaskConfig next;
            auto status = ParseFeatureConfig(values, &next.features, catalog);
            if (status.error != NpmBasicTaskConfigError::kNone) return status;
            status = ParseDomainConfig(values, &next.domains);
            if (status.error != NpmBasicTaskConfigError::kNone) return status;

            NpmParameterConsumersV1 consumers;
            consumers.basic_enabled = next.features.basic_enabled;
            consumers.session_enabled = next.features.session_enabled;
            consumers.labeling_enabled = next.features.labeling_enabled;
            consumers.labeling_available = labeling_available;
            const std::string_view parameters_text(parameters->GetString(), parameters->GetStringLength());
            if (parameters_text.find('\0') != std::string_view::npos) {
                status = Fail(NpmBasicTaskConfigError::kInvalidParameters, FieldName(TaskConfigField::kParameters));
                status.parameter_status.error = NpmParameterErrorV1::kInvalidJson;
                return status;
            }
            NpmTaskParametersV1 parsed_parameters;
            const auto parameter_status = ParseNpmParametersV1(parameters->GetString(), consumers, &parsed_parameters);
            if (parameter_status.error != NpmParameterErrorV1::kNone) {
                status = Fail(NpmBasicTaskConfigError::kInvalidParameters, FieldName(TaskConfigField::kParameters));
                status.parameter_status = parameter_status;
                return status;
            }

            next.parameters_json.assign(parameters_text);
            next.analysis = std::move(parsed_parameters.core.analysis);
            if (parsed_parameters.core.labeling_reference.has_value()) {
                next.labeling_reference = std::move(*parsed_parameters.core.labeling_reference);
            }
            if (parsed_parameters.core.labeling_memory_mib.has_value()) {
                next.labeling_memory_mib = *parsed_parameters.core.labeling_memory_mib;
            }
            if (parsed_parameters.session.has_value()) {
                next.features.session_max_tcp_ranges_per_direction =
                    parsed_parameters.session->max_tcp_ranges_per_direction;
            }
            *output = std::move(next);
            return {};
        }

        NpmRunMode run_mode = NpmRunMode::kOffline;
        if (const rapidjson::Value* value = values[FieldIndex(TaskConfigField::kRunMode)]) {
            const std::string_view text(value->GetString(), value->GetStringLength());
            if (text == "offline") {
                run_mode = NpmRunMode::kOffline;
            } else if (text == "realtime") {
                run_mode = NpmRunMode::kRealtime;
            } else {
                return Fail(NpmBasicTaskConfigError::kInvalidEnum, FieldName(TaskConfigField::kRunMode));
            }
        }

        NpmBasicTaskConfig next;
        next.analysis = DefaultNpmAnalysisConfig(run_mode);
        if (const rapidjson::Value* value = values[FieldIndex(TaskConfigField::kResultMode)]) {
            const std::string_view text(value->GetString(), value->GetStringLength());
            if (text == "final") {
                next.analysis.result_mode = NpmResultMode::kFinal;
            } else if (text == "periodic_snapshot") {
                next.analysis.result_mode = NpmResultMode::kPeriodicSnapshot;
            } else {
                return Fail(NpmBasicTaskConfigError::kInvalidEnum, FieldName(TaskConfigField::kResultMode));
            }
        }
        if (const rapidjson::Value* value = values[FieldIndex(TaskConfigField::kOverloadPolicy)]) {
            const std::string_view text(value->GetString(), value->GetStringLength());
            if (text != "fail") {
                return Fail(NpmBasicTaskConfigError::kInvalidEnum, FieldName(TaskConfigField::kOverloadPolicy));
            }
            next.analysis.overload_policy = NpmOverloadPolicy::kFail;
        }

        auto status = ParseIntegerFields(values, &next.analysis);
        if (status.error != NpmBasicTaskConfigError::kNone) return status;
        status = ParseFeatureConfig(values, &next.features, catalog);
        if (status.error != NpmBasicTaskConfigError::kNone) return status;

        status = ParseDomainConfig(values, &next.domains);
        if (status.error != NpmBasicTaskConfigError::kNone) return status;

        const auto analysis_error = ValidateNpmAnalysisConfig(next.analysis);
        if (analysis_error != NpmAnalysisConfigError::kNone) {
            status = Fail(NpmBasicTaskConfigError::kAnalysisValidationError, AnalysisErrorField(analysis_error));
            status.analysis_error = analysis_error;
            return status;
        }

        *output = std::move(next);
        return {};
    } catch (const std::bad_alloc&) {
        return Fail(NpmBasicTaskConfigError::kAllocationFailed);
    }
}

bool NpmBasicTaskRequestsLabeling(const char* with_params_json) noexcept {
    if (with_params_json == nullptr || with_params_json[0] == '\0') return false;
    try {
        rapidjson::Document document;
        document.Parse(with_params_json);
        if (document.HasParseError() || !document.IsObject()) return false;
        const auto member = document.FindMember("features");
        if (member == document.MemberEnd() || !member->value.IsString()) return false;
        NpmBasicFeatureConfig features;
        return ParseFeatures(std::string_view(member->value.GetString(), member->value.GetStringLength()), &features,
                             ProductionNpmModuleCatalogV1()) &&
               features.labeling_enabled;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

}  // namespace flowsql::npm
