// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_NPM_BASIC_NPM_BASIC_TASK_CONFIG_H_
#define _FLOWSQL_PLUGINS_NPM_BASIC_NPM_BASIC_TASK_CONFIG_H_

#include "npm_analysis_contract.h"

#include <cstdint>
#include <string>

namespace flowsql::npm {

struct NpmBasicTaskConfig {
    NpmAnalysisConfig analysis;
    NpmObservationDomainMap domains;
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
    kInvalidSourceDomains,
    kDomainValidationError,
    kAnalysisValidationError,
    kAllocationFailed,
};

struct NpmBasicTaskConfigStatus {
    NpmBasicTaskConfigError error = NpmBasicTaskConfigError::kNone;
    std::string field;
    NpmObservationDomainError domain_error = NpmObservationDomainError::kNone;
    NpmAnalysisConfigError analysis_error = NpmAnalysisConfigError::kNone;
};

/** Parses and owns Scheduler WITH parameters. Output is replaced only after complete validation. */
NpmBasicTaskConfigStatus ParseNpmBasicTaskConfig(const char* with_params_json, NpmBasicTaskConfig* output);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_PLUGINS_NPM_BASIC_NPM_BASIC_TASK_CONFIG_H_
