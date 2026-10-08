// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "value_task.h"

#include <limits>
#include <utility>

#include "plugins/baseline/rolling/rolling_task_runner.h"

namespace flowsql {
namespace baseline {

BaselineValueTask::BaselineValueTask(TaskRegistry* registry,
                                     std::string task_id,
                                     std::string task_name,
                                     std::string config_content,
                                     BaselineTaskSpec spec,
                                     std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar)
    : BaselineTaskBase(registry,
                       std::move(task_id),
                       BaselineTaskKind::kValue,
                       std::move(task_name),
                       std::move(config_content)),
      spec_(std::move(spec)),
      compiled_event_calendar_(std::move(compiled_event_calendar)) {}

const char* BaselineValueTask::Id() const { return BaselineTaskBase::Id(); }
const char* BaselineValueTask::Name() const { return BaselineTaskBase::Name(); }
BaselineTaskKind BaselineValueTask::Kind() const { return BaselineTaskBase::Kind(); }

BaselineSerializationResult BaselineValueTask::ExportConfig(
    BaselineSerializationFormat format) const {
    return BaselineTaskBase::ExportConfig(format);
}

BaselineSerializationResult BaselineValueTask::QueryTaskSnapshot(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return QueryRollingTaskSnapshot(spec_, rolling_states_, format);
}

BaselineSerializationResult BaselineValueTask::QuerySeriesSnapshot(
    std::string_view series_key,
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return QueryRollingSeriesSnapshot(spec_, rolling_states_, series_key, format);
}

BaselineStatus BaselineValueTask::Close() { return BaselineTaskBase::Close(); }

BaselineStatus BaselineValueTask::DoReleaseIdentity(std::string_view key, BaselineStateReleaseScopeV1 scope) {
    const std::string identity(key);
    rolling_states_.erase(identity);
    if (scope == BaselineStateReleaseScopeV1::kAllState) {
        seeds_by_series_.erase(identity);
        artifacts_by_series_.erase(identity);
    }
    return BaselineStatus::kOk;
}

BaselineStateUsageV1 BaselineValueTask::DoQueryStateUsage() const {
    return {static_cast<uint64_t>(rolling_states_.size()), static_cast<uint64_t>(artifacts_by_series_.size()), 0, 0};
}

bool BaselineValueTask::HasIdentityCapacity(const std::string& key, bool include_model) const {
    const auto* limits = StateLimits();
    if (!limits) return true;
    return (rolling_states_.find(key) != rolling_states_.end() ||
            rolling_states_.size() < limits->max_runtime_identities) &&
           (!include_model || artifacts_by_series_.find(key) != artifacts_by_series_.end() ||
            artifacts_by_series_.size() < limits->max_model_identities);
}

void BaselineValueTask::OnClosing() {
    decltype(rolling_states_){}.swap(rolling_states_);
    BootstrapSeedStore{}.swap(seeds_by_series_);
    BootstrapArtifactStore{}.swap(artifacts_by_series_);
    compiled_event_calendar_.reset();
}

RollingBaselineResult BaselineValueTask::SubmitObservation(
    const ValueRollingObservation& obs,
    const RollingSubmitOptions& options) {
    RollingBaselineResult result;
    result.series_key = obs.series_key;
    result.bucket_id = obs.bucket_id;
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }
    RollingEventContext events{compiled_event_calendar_.get()};
    MarkStateOperation();
    const bool may_initialize =
        options.allow_auto_init_from_empty ||
        (options.allow_auto_init_from_bootstrap && seeds_by_series_.find(obs.series_key) != seeds_by_series_.end());
    if (may_initialize && !HasIdentityCapacity(obs.series_key, false)) {
        result.status = BaselineStatus::kInvalidArgument;
        result.diagnostics = "baseline_state_capacity_reached";
        return result;
    }
    return RunValueRollingSubmit(spec_, seeds_by_series_, &rolling_states_, obs, options, &events);
}

RollingPrediction BaselineValueTask::PredictRolling(std::string_view series_key,
                                                    int64_t bucket_id) const {
    RollingPrediction result;
    result.series_key = std::string(series_key);
    result.bucket_id = bucket_id;
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }
    return PredictRollingForSeries(spec_, seeds_by_series_, rolling_states_, series_key, bucket_id,
                                   compiled_event_calendar_.get());
}

RollingPredictionSequence BaselineValueTask::PredictRolling(
    std::string_view series_key,
    int64_t start_bucket_id,
    uint32_t point_count) const {
    RollingPredictionSequence sequence;
    sequence.series_key = std::string(series_key);
    sequence.start_bucket_id = start_bucket_id;
    sequence.point_count = point_count;
    if (point_count == 0) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }
    if (start_bucket_id > std::numeric_limits<int64_t>::max() -
                              static_cast<int64_t>(point_count - 1)) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }

    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) {
        sequence.status = status;
        sequence.predictions.reserve(point_count);
        for (uint32_t i = 0; i < point_count; ++i) {
            RollingPrediction prediction;
            prediction.series_key = sequence.series_key;
            prediction.bucket_id = start_bucket_id + static_cast<int64_t>(i);
            prediction.status = status;
            sequence.predictions.push_back(std::move(prediction));
        }
        return sequence;
    }
    return PredictRollingSequenceForSeries(spec_, seeds_by_series_, rolling_states_, series_key, start_bucket_id,
                                           point_count, compiled_event_calendar_.get());
}

BootstrapTrainResult BaselineValueTask::Bootstrap(const ValueBootstrapInput& input) {
    BootstrapTrainResult result;
    result.status = EnsureOpen();
    if (result.status != BaselineStatus::kOk) return result;
    MarkStateOperation();
    if (!input.options.force_replace_existing_artifact &&
        FindBootstrapArtifact(artifacts_by_series_, input.series_key)) {
        result.status = BaselineStatus::kInvalidArgument;
        if (input.options.include_diagnostics) {
            result.diagnostics = "bootstrap artifact already exists for series_key";
        }
        return result;
    }
    if (!HasIdentityCapacity(input.series_key, true)) {
        result.status = BaselineStatus::kInvalidArgument;
        if (input.options.include_diagnostics) result.diagnostics = "baseline_state_capacity_reached";
        return result;
    }
    BootstrapArtifact artifact;
    result = bootstrap_engine_.TrainValue(
        spec_, input, &artifact, compiled_event_calendar_.get());
    if (result.status == BaselineStatus::kOk) {
        const BaselineStatus seed_status =
            StoreBootstrapArtifact(input.series_key,
                                   std::move(artifact),
                                   bootstrap_engine_,
                                   &artifacts_by_series_,
                                   &seeds_by_series_);
        if (seed_status != BaselineStatus::kOk) {
            result.status = seed_status;
            return result;
        }
        rolling_states_.erase(input.series_key);
        (void)WarmupRollingStatesFromBootstrapSeeds(
            spec_, seeds_by_series_, &rolling_states_,
            StateLimits() ? std::string_view(input.series_key) : std::string_view{});
    }
    return result;
}

BootstrapPrediction BaselineValueTask::PredictBootstrap(
    std::string_view series_key,
    int64_t bucket_id,
    const BootstrapPredictionOptions& options) const {
    const std::string key(series_key);
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) {
        BootstrapPrediction prediction;
        prediction.status = status;
        prediction.series_key = key;
        prediction.bucket_id = bucket_id;
        return prediction;
    }
    const BootstrapArtifact* artifact = FindBootstrapArtifact(artifacts_by_series_, key);
    if (!artifact) {
        BootstrapPrediction prediction;
        prediction.status = key.empty() ? BaselineStatus::kInvalidArgument
                                        : BaselineStatus::kNotTrained;
        prediction.series_key = key;
        prediction.bucket_id = bucket_id;
        return prediction;
    }
    return bootstrap_engine_.PredictValue(
        *artifact, bucket_id, options, &spec_, compiled_event_calendar_.get());
}

BootstrapPredictionSequence BaselineValueTask::PredictBootstrap(
    std::string_view series_key,
    int64_t start_bucket_id,
    uint32_t point_count,
    const BootstrapPredictionOptions& options) const {
    BootstrapPredictionSequence sequence;
    sequence.series_key = std::string(series_key);
    sequence.start_bucket_id = start_bucket_id;
    sequence.point_count = point_count;
    if (point_count == 0) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }
    if (start_bucket_id > std::numeric_limits<int64_t>::max() -
                              static_cast<int64_t>(point_count - 1)) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }

    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) {
        sequence.status = status;
        sequence.predictions.reserve(point_count);
        for (uint32_t i = 0; i < point_count; ++i) {
            BootstrapPrediction prediction;
            prediction.status = status;
            prediction.series_key = sequence.series_key;
            prediction.bucket_id = start_bucket_id + static_cast<int64_t>(i);
            sequence.predictions.push_back(std::move(prediction));
        }
        return sequence;
    }

    const BootstrapArtifact* artifact =
        FindBootstrapArtifact(artifacts_by_series_, sequence.series_key);
    if (!artifact) {
        sequence.status = sequence.series_key.empty() ? BaselineStatus::kInvalidArgument
                                                      : BaselineStatus::kNotTrained;
        sequence.predictions.reserve(point_count);
        for (uint32_t i = 0; i < point_count; ++i) {
            BootstrapPrediction prediction;
            prediction.status = sequence.status;
            prediction.series_key = sequence.series_key;
            prediction.bucket_id = start_bucket_id + static_cast<int64_t>(i);
            sequence.predictions.push_back(std::move(prediction));
        }
        return sequence;
    }

    return bootstrap_engine_.PredictValueSequence(
        *artifact, start_bucket_id, point_count, options, &spec_, compiled_event_calendar_.get());
}

BaselineSerializationResult BaselineValueTask::ExportBootstrapArtifact(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapArtifactStore(artifacts_by_series_, bootstrap_engine_, format);
}

BaselineStatus BaselineValueTask::LoadBootstrapArtifact(
    std::string_view content,
    BaselineSerializationFormat format) {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return status;
    MarkStateOperation();
    BootstrapArtifactStore loaded_artifacts;
    BootstrapSeedStore loaded_seeds;
    const BaselineStatus load_status = LoadBootstrapArtifactStore(
        content, format, bootstrap_engine_, spec_, BootstrapArtifactKind::kValue, &loaded_artifacts, &loaded_seeds);
    if (load_status == BaselineStatus::kOk) {
        const auto* limits = StateLimits();
        if (limits && (loaded_artifacts.size() > limits->max_model_identities ||
                       loaded_seeds.size() > limits->max_runtime_identities)) {
            return BaselineStatus::kInvalidArgument;
        }
        artifacts_by_series_.swap(loaded_artifacts);
        seeds_by_series_.swap(loaded_seeds);
        rolling_states_.clear();
        (void)WarmupRollingStatesFromBootstrapSeeds(spec_, seeds_by_series_, &rolling_states_);
    }
    return load_status;
}

BaselineSerializationResult BaselineValueTask::ExportBootstrapSeed(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapSeedStore(seeds_by_series_, bootstrap_engine_, format);
}

}  // namespace baseline
}  // namespace flowsql
