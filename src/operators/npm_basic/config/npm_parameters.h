// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_PARAMETERS_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_PARAMETERS_H_

#include <operators/npm_basic/npm_analysis_contract.h>
#include <operators/npm_basic/npm_tcp_stream_contract.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace flowsql::npm {

constexpr std::size_t kNpmParametersMaxJsonBytes = 64 * 1024;
constexpr std::size_t kNpmParametersMaxDepth = 64;
constexpr uint32_t kNpmMinLabelingMemoryMiB = 8;
constexpr uint32_t kNpmDefaultLabelingMemoryMiB = 64;
constexpr uint32_t kNpmMaxLabelingMemoryMiB = 256;

struct NpmCoreParameters {
    NpmAnalysisConfig analysis;
    bool run_mode_explicit = false;
    std::optional<std::string> labeling_reference;
    std::optional<uint32_t> labeling_memory_mib;
    std::optional<uint32_t> result_retention_days;
    NpmTcpStreamConfig tcp_stream;
};

struct NpmBasicModuleParameters {};

struct NpmSessionModuleParameters {
    uint32_t max_tcp_ranges_per_direction = 1024;
};

struct NpmTaskParameters {
    uint32_t schema_version = 1;
    NpmCoreParameters core;
    std::optional<NpmBasicModuleParameters> basic;
    std::optional<NpmSessionModuleParameters> session;
};

struct NpmParameterConsumers {
    bool basic_enabled = true;
    bool basic_available = true;
    bool session_enabled = false;
    bool session_available = true;
    bool labeling_enabled = false;
    bool labeling_available = false;
};

enum class NpmParameterError : uint8_t {
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
    kInvalidExactReference,
    kAllocationFailed,
};

struct NpmParameterStatus {
    NpmParameterError error = NpmParameterError::kNone;
    std::string path;
};

/** Parses an owned parameters envelope. Output is replaced only after complete validation. */
NpmParameterStatus ParseNpmParameters(const char* parameters_json, const NpmParameterConsumers& consumers,
                                      NpmTaskParameters* output);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_PARAMETERS_H_
