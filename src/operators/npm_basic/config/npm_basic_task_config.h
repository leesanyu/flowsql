// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_CONFIG_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_CONFIG_H_

#include "npm_parameters.h"

#include <operators/npm_basic/core/npm_module_catalog.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace flowsql::npm {

constexpr uint32_t kNpmMinSessionTcpRangesPerDirection = 8;
constexpr uint32_t kNpmDefaultSessionTcpRangesPerDirection = 1024;
constexpr uint32_t kNpmMaxSessionTcpRangesPerDirection = 65536;

enum class NpmResultEntity : uint8_t {
    kBasic = 0,
    kSession = 1,
    kProtocol = 2,
};

struct NpmBasicFeatureConfig {
    bool basic_enabled = true;
    bool session_enabled = false;
    bool labeling_enabled = false;
    NpmResultEntity observing = NpmResultEntity::kBasic;
    std::vector<std::string> module_ids;
    std::string observing_entity;
    std::shared_ptr<arrow::Schema> protocol_schema;
    uint32_t session_max_tcp_ranges_per_direction = kNpmDefaultSessionTcpRangesPerDirection;
};

struct NpmBasicTaskConfig {
    NpmAnalysisConfig analysis;
    NpmObservationDomainMap domains;
    NpmBasicFeatureConfig features;
    std::string parameters_json;
    std::string labeling_reference;
    uint32_t labeling_memory_mib = kNpmDefaultLabelingMemoryMiB;
    std::optional<uint32_t> result_retention_days;
    NpmTcpStreamConfigV1 tcp_stream;
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

/** Owns WITH parameters. Omitted namespace uses the caller's source; omitted domains accept all sources. */
NpmBasicTaskConfigStatus ParseNpmBasicTaskConfig(const char* with_params_json, NpmBasicTaskConfig* output,
                                                 bool labeling_available = false,
                                                 const NpmModuleCatalogV1& catalog = ProductionNpmModuleCatalogV1(),
                                                 std::string_view default_input_namespace = "default");

/** Side-effect-free probe used to gate optional provider lookup before consumed-field validation. */
bool NpmBasicTaskRequestsLabeling(const char* with_params_json) noexcept;

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_CONFIG_H_
