// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_CONFIG_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_CONFIG_H_

#include "npm_analysis_contract.h"
#include "npm_parameters.h"

#include <cstdint>
#include <string>

namespace flowsql::npm {

constexpr uint32_t kNpmMinSessionTcpRangesPerDirection = 8;
constexpr uint32_t kNpmDefaultSessionTcpRangesPerDirection = 1024;
constexpr uint32_t kNpmMaxSessionTcpRangesPerDirection = 65536;

enum class NpmResultEntity : uint8_t {
    kBasic = 0,
    kSession = 1,
};

struct NpmBasicFeatureConfig {
    bool basic_enabled = true;
    bool session_enabled = false;
    NpmResultEntity observing = NpmResultEntity::kBasic;
    uint32_t session_max_tcp_ranges_per_direction = kNpmDefaultSessionTcpRangesPerDirection;
};

struct NpmBasicTaskConfig {
    NpmAnalysisConfig analysis;
    NpmObservationDomainMap domains;
    NpmBasicFeatureConfig features;
};

enum class NpmBasicTaskConfigError : uint8_t {
    kNone = 0,
    kNullInput,
    kEmptyInput,
    kNullOutput,
    kInvalidJson,
    kDuplicateField,
    kUnknownField,
    kNonStringValue,
    kMissingRequiredField,
    kInvalidEnum,
    kInvalidInteger,
    kInvalidFeatures,
    kInvalidObserving,
    kObservingFeatureDisabled,
    kSessionConfigWithoutFeature,
    kSessionTcpRangesOutOfRange,
    kInvalidSourceDomains,
    kDomainValidationError,
    kAnalysisValidationError,
    kAllocationFailed,
    kInvalidParameters,
    kParameterSourceConflict,
};

struct NpmBasicTaskConfigStatus {
    NpmBasicTaskConfigError error = NpmBasicTaskConfigError::kNone;
    std::string field;
    NpmObservationDomainError domain_error = NpmObservationDomainError::kNone;
    NpmAnalysisConfigError analysis_error = NpmAnalysisConfigError::kNone;
    NpmParameterStatusV1 parameter_status;
};

/** Parses and owns Scheduler WITH parameters. Output is replaced only after complete validation. */
NpmBasicTaskConfigStatus ParseNpmBasicTaskConfig(const char* with_params_json, NpmBasicTaskConfig* output);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_CONFIG_H_
