// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/ibaseline_checkpoint.h>
#include <framework/interfaces/ibaseline_model_parameters.h>
#include <framework/interfaces/ibaseline_service.h>
#include "bucket_aggregator.h"

namespace flowsql::baseliner {
// Internal owned output, converted to the frozen Arrow schemas at the operator/storage boundary.
struct EvaluationRow {
    Observation input;
    ScalarResult values;
    RollingBaselineResult rolling;
    std::string summary_id;
    std::string basis_id;
    std::string model_basis_id;
    std::string band_kind = "detection";
    std::string unit;
};
struct EvaluationOutput {
    std::vector<EvaluationRow> results;
    std::optional<RelationRollingResult> relation;
    std::vector<EvaluationRow> forecasts;
};
struct ModelParametersRow {
    Observation identity;
    std::string model_basis_id;
    std::optional<int64_t> as_of_bucket;
    BaselineModelParametersV1 parameters;
};
struct MaintenanceRow {
    Observation identity;
    std::string event_kind;
    std::string reason;
    BaselineStateUsageV1 usage;
};
// All calls serialized. The caller retains the service/plugin owner until after Close/destruction.
class EvaluationEngine {
 public:
    EvaluationEngine();
    ~EvaluationEngine();
    int Open(ConfigSnapshot config, IBaselineService* service, IBaselineStateControlServiceV1* control,
             IBaselineCheckpointServiceV1* checkpoints = nullptr);
    int ExportCheckpoint(std::string* output) const;
    int ExportModelParameters(std::vector<ModelParametersRow>* output) const;
    int RestoreCheckpoint(std::string_view checkpoint);
    void SetTime(int64_t monotonic_ns, int64_t wall_ns);
    int SetSourceProgress(const BlockInputProgressV1& progress);
    std::optional<int64_t> NextMaintenanceDeadline() const;
    int Maintain(std::vector<MaintenanceRow>* output);
    std::pair<BaselineStatus, BaselineStateUsageV1> QueryUsage() const;
    // 1: durable replay, 0: new observation, -1: incompatible source epoch.
    int ClassifyReplay(const Observation& input) const;
    int Submit(const Observation& input, EvaluationOutput* output);
    int ValidateBatch(
        const std::vector<Observation>& rows);  // No algorithm mutation, including duplicate/capacity gates.
    int Finish();  // Validate history-only identities too; never manufacture evaluation rows.
    BaselineSerializationResult QuerySeriesSnapshot(std::string_view dataset, std::string_view metric,
                                                    std::string_view identity) const;
    void Close();
    const std::string& LastError() const;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace flowsql::baseliner
