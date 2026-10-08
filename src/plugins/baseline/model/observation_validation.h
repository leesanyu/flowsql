// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_OBSERVATION_VALIDATION_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_OBSERVATION_VALIDATION_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cmath>
#include <cstddef>
#include <string>

namespace flowsql {
namespace baseline {

inline bool IsValidValueObservation(double value) { return std::isfinite(value) && value >= 0.0; }

inline bool IsValidRatioObservation(double numerator, double denominator) {
    return std::isfinite(numerator) && std::isfinite(denominator) && denominator > 0.0 && numerator >= 0.0 &&
           numerator <= denominator;
}

inline bool IsValidRelationMetricHeader(const RelationBootstrapMetric& metric, std::size_t group_count,
                                        const std::string& requested_name = {}) {
    return std::isfinite(metric.total) && metric.total > 0.0 && metric.values_by_group.size() == group_count &&
           (requested_name.empty() || metric.metric.empty() || metric.metric == requested_name);
}

inline bool IsValidRelationMass(double mass) { return IsValidValueObservation(mass); }

inline bool IsValidRelationMetric(const RelationBootstrapMetric& metric, std::size_t group_count,
                                  const std::string& requested_name = {}) {
    if (!IsValidRelationMetricHeader(metric, group_count, requested_name)) return false;
    for (double mass : metric.values_by_group) {
        if (!IsValidRelationMass(mass)) return false;
    }
    return true;
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_OBSERVATION_VALIDATION_H_
