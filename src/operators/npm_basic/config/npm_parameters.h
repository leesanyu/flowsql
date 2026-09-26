// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_PARAMETERS_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_PARAMETERS_H_

#include <operators/npm_basic/npm_analysis_contract.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace flowsql::npm {

constexpr std::size_t kNpmParametersMaxJsonBytesV1 = 64 * 1024;
constexpr std::size_t kNpmParametersMaxDepthV1 = 64;
constexpr uint32_t kNpmMinLabelingMemoryMiB = 8;
constexpr uint32_t kNpmDefaultLabelingMemoryMiB = 64;
constexpr uint32_t kNpmMaxLabelingMemoryMiB = 256;

enum class NpmParameterSourceV1 : uint8_t {
    kLegacyWith = 0,
    kParametersV1 = 1,
};

struct NpmCoreParametersV1 {
    NpmAnalysisConfig analysis;
    std::optional<std::string> labeling_reference;
    std::optional<uint32_t> labeling_memory_mib;
    std::optional<uint32_t> result_retention_days;
};

struct NpmBasicModuleParametersV1 {};

struct NpmSessionModuleParametersV1 {
    uint32_t max_tcp_ranges_per_direction = 1024;
};

struct NpmTaskParametersV1 {
    uint32_t schema_version = 1;
    NpmParameterSourceV1 source = NpmParameterSourceV1::kLegacyWith;
    NpmCoreParametersV1 core;
    std::optional<NpmBasicModuleParametersV1> basic;
    std::optional<NpmSessionModuleParametersV1> session;
};

struct NpmParameterConsumersV1 {
    bool basic_enabled = true;
    bool basic_available = true;
    bool session_enabled = false;
    bool session_available = true;
    bool labeling_enabled = false;
    bool labeling_available = false;
};

enum class NpmParameterErrorV1 : uint8_t {
    kNone = 0,
    kNullInput,
    kEmptyInput,
    kNullOutput,
    kJsonTooLarge,
    kInvalidJson,
    kInvalidEnvelope,
    kNestingTooDeep,
    kMissingSchemaVersion,
    kUnsupportedVersion,
    kDuplicateField,
    kUnknownConsumedField,
    kInvalidType,
    kInvalidValue,
    kInvalidRange,
    kUnavailableFeature,
    kLegacyConflict,
    kInvalidExactReference,
    kAllocationFailed,
};

struct NpmParameterStatusV1 {
    NpmParameterErrorV1 error = NpmParameterErrorV1::kNone;
    std::string path;
};

/** Parses an owned V1 parameters envelope. Output is replaced only after complete validation. */
NpmParameterStatusV1 ParseNpmParametersV1(const char* parameters_json, const NpmParameterConsumersV1& consumers,
                                          NpmTaskParametersV1* output);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_PARAMETERS_H_
