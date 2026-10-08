// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_TASK_SPEC_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_TASK_SPEC_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flowsql {
namespace baseline {

struct BaselineClockSpec {
    int64_t bucket_seconds = 0;
    std::string timezone;
};

struct BaselineCalendarRef {
    std::string calendar_id;
    std::string calendar_version;
};

struct BaselineTaskSpec {
    std::string task_id;
    std::string name;
    std::string task_kind;
    std::string feature_id;
    std::string profile;
    BaselineClockSpec clock_spec;
    BaselineCalendarRef calendar_ref;

    std::string key;
    std::string feature;
    std::string feature_type;
    int64_t delta = 0;
    std::string tz;
    std::string config_json;
    // Resolved by the existing sampled profile lookup when parsing task configuration.
    bool value_identity_transform = false;
};

struct RelationSupportPolicySpec {
    int32_t k_support = 0;
    double min_hist_share = 0.0;
    double min_active_ratio = 0.0;
};

struct RelationSummaryPolicySpec {
    int32_t k_head = 0;
    int32_t k_stable = 0;
};

struct RelationTaskSpec {
    std::string task_id;
    std::string name;
    std::string task_kind;
    std::string feature_id;
    std::string profile;
    BaselineCalendarRef calendar_ref;
    std::string feature_base;
    std::string group_space_id;
    std::optional<std::string> group_space_version;
    std::string metric_set_id;
    std::vector<std::string> metrics;
    std::string encode_type;
    std::vector<uint32_t> other_group_idxs;
    RelationSupportPolicySpec support_policy;
    RelationSummaryPolicySpec summary_policy;
    std::string config_json;
};

struct RelationTaskClockSpec {
    int64_t delta = 0;
    std::string tz;
};

struct RelationTaskCreateSpec {
    RelationTaskSpec task_spec;
    RelationTaskClockSpec clock_spec;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_TASK_SPEC_H_
