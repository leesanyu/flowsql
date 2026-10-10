// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_BASELINE_TASK_RATIO_TASK_H_
#define _FLOWSQL_PLUGINS_BASELINE_TASK_RATIO_TASK_H_

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "baseline_task_base.h"
#include "bootstrap_task_store.h"
#include "plugins/baseline/model/event_calendar_matcher.h"
#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/rolling/rolling_state.h"

namespace flowsql {
namespace baseline {

class TaskRegistry;

class BaselineRatioTask final : public IBaselineRatioTask, public BaselineTaskBase {
    friend class CheckpointAccess;

 public:
    BaselineRatioTask(TaskRegistry* registry, std::string task_id, std::string task_name, std::string config_content,
                      BaselineTaskSpec spec, std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar);

    const char* Id() const override;
    const char* Name() const override;
    BaselineTaskKind Kind() const override;

    BaselineSerializationResult ExportConfig(BaselineSerializationFormat format) const override;
    BaselineSerializationResult QueryTaskSnapshot(BaselineSerializationFormat format) const override;
    BaselineSerializationResult QuerySeriesSnapshot(std::string_view series_key,
                                                    BaselineSerializationFormat format) const override;
    BaselineStatus Close() override;

    RollingBaselineResult SubmitObservation(const RatioRollingObservation& obs,
                                            const RollingSubmitOptions& options) override;
    RollingPrediction PredictRolling(std::string_view series_key, int64_t bucket_id) const override;
    RollingPredictionSequence PredictRolling(std::string_view series_key, int64_t start_bucket_id,
                                             uint32_t point_count) const override;

    BootstrapTrainResult Bootstrap(const RatioBootstrapInput& input) override;
    BootstrapPrediction PredictBootstrap(std::string_view series_key, int64_t bucket_id,
                                         const BootstrapPredictionOptions& options) const override;
    BootstrapPredictionSequence PredictBootstrap(std::string_view series_key, int64_t start_bucket_id,
                                                 uint32_t point_count,
                                                 const BootstrapPredictionOptions& options) const override;
    BaselineSerializationResult ExportBootstrapArtifact(BaselineSerializationFormat format) const override;
    BaselineStatus LoadBootstrapArtifact(std::string_view content, BaselineSerializationFormat format) override;
    BaselineSerializationResult ExportBootstrapSeed(BaselineSerializationFormat format) const override;

 private:
    void OnClosing() override;
    BaselineStatus DoReleaseIdentity(std::string_view key, BaselineStateReleaseScopeV1 scope) override;
    BaselineStateUsageV1 DoQueryStateUsage() const override;
    bool HasIdentityCapacity(const std::string& key, bool include_model) const;

    BaselineTaskSpec spec_;
    std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar_;
    BootstrapArtifactStore artifacts_by_series_;
    BootstrapSeedStore seeds_by_series_;
    std::unordered_map<std::string, RollingState> rolling_states_;
    BootstrapEngine bootstrap_engine_;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_RATIO_TASK_H_
