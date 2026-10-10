// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/ibaseline_types.h>
#include <string>
#include <string_view>

namespace flowsql {
struct BaselineModelParametersV1 {
    uint32_t parameters_version = 1;
    BaselineStatus status = BaselineStatus::kNotTrained;
    std::string maturity;
    std::string parameters_json;  // Empty means NULL (no trained model).
};
// Independent optional task capability; existing task/service virtual tables stay unchanged.
// Serialize with Submit/Bootstrap/Release/Close. No learning, cursor or diagnostic mutation.
// Owned output; keep the plugin alive through the task and every exported product.
interface IBaselineModelParametersV1 {
    virtual ~IBaselineModelParametersV1() = default;
    // max_bytes bounds serialization; failure leaves output unchanged. Missing series is a
    // successful untrained result. Closed task, empty key or invalid budget returns failure.
    virtual BaselineStatus ExportModelParameters(std::string_view series_key, uint64_t max_bytes,
                                                 BaselineModelParametersV1 * output) const = 0;
};
}  // namespace flowsql
