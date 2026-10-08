// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_TASK_RUNNER_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_TASK_RUNNER_H_

#include <framework/interfaces/ibaseline_types.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "plugins/baseline/model/event_calendar_matcher.h"
#include "plugins/baseline/rolling/rolling_state.h"
#include "plugins/baseline/task/bootstrap_task_store.h"

namespace flowsql {
namespace baseline {

using RollingStateMap = std::unordered_map<std::string, RollingState>;

// One bucket per Submit; Relation shares this lazy lookup across its routed children.
struct RollingEventContext {
    explicit RollingEventContext(const CompiledEventCalendar* event_calendar = nullptr) : calendar(event_calendar) {}
    const CompiledEventCalendar* calendar = nullptr;
    bool resolved = false;
    std::unordered_set<std::string> hit_codes;
};

RollingBaselineResult RunValueRollingSubmit(const BaselineTaskSpec& spec, const BootstrapSeedStore& seeds,
                                            RollingStateMap* states, const ValueRollingObservation& obs,
                                            const RollingSubmitOptions& options, RollingEventContext* events = nullptr);

RollingBaselineResult RunRatioRollingSubmit(const BaselineTaskSpec& spec, const BootstrapSeedStore& seeds,
                                            RollingStateMap* states, const RatioRollingObservation& obs,
                                            const RollingSubmitOptions& options, RollingEventContext* events = nullptr);

RollingPrediction PredictRollingForSeries(const BaselineTaskSpec& spec, const BootstrapSeedStore& seeds,
                                          const RollingStateMap& states, std::string_view series_key, int64_t bucket_id,
                                          const CompiledEventCalendar* calendar = nullptr);

RollingPredictionSequence PredictRollingSequenceForSeries(const BaselineTaskSpec& spec, const BootstrapSeedStore& seeds,
                                                          const RollingStateMap& states, std::string_view series_key,
                                                          int64_t start_bucket_id, uint32_t point_count,
                                                          const CompiledEventCalendar* calendar = nullptr);

struct RollingWarmupStats {
    uint64_t success_count = 0;
    uint64_t failure_count = 0;
    uint64_t skipped_existing_count = 0;
};

RollingWarmupStats WarmupRollingStatesFromBootstrapSeeds(const BaselineTaskSpec& spec, const BootstrapSeedStore& seeds,
                                                         RollingStateMap* states, std::string_view target_series = {});

BaselineSerializationResult QueryRollingTaskSnapshot(const BaselineTaskSpec& spec,
                                                     const RollingStateMap& states,
                                                     BaselineSerializationFormat format);

BaselineSerializationResult QueryRollingSeriesSnapshot(const BaselineTaskSpec& spec,
                                                       const RollingStateMap& states,
                                                       std::string_view series_key,
                                                       BaselineSerializationFormat format);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_TASK_RUNNER_H_
