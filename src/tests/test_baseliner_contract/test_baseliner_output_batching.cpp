// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/ibaseline_evaluation.h>
#include <cmath>
#include <iostream>
#include <limits>
#include "baseliner_batch_fixture.h"

using namespace flowsql;
using namespace flowsql::baseliner;
using namespace flowsql::baseliner::batch_test;
namespace {
void Case(size_t count, uint32_t horizon, size_t page_rows, bool baseline) {
    auto config = Config(horizon);
    auto library = std::make_shared<Library>();
    auto task = Task(library, config);
    auto model_sink = std::make_shared<DataFrameChannel>("dataframe", "models");
    Check(model_sink->Open() == 0, "model sink");
    auto* model_task = dynamic_cast<IBlockTransformModelOutputTaskV1*>(task.get());
    BlockModelOutputBindingV1 binding;
    binding.primary_target = "dataframe.results";
    binding.dataframe = model_sink;
    Check(model_task && model_task->BindModelOutput(binding) == 0, task->LastError());
    Check(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("dataframe.samples") == 0,
          task->LastError());
    std::shared_ptr<arrow::Schema> schema;
    Check(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0, task->LastError());
    Querier reference_owner;
    EvaluationEngine reference;
    Check(reference.Open(config, &reference_owner.plugin, &reference_owner.plugin) == 0, reference.LastError());
    std::vector<EvaluationRow> expected_rows;
    std::vector<Batch> actual;
    size_t callbacks = 0, batches = 0;
    for (size_t offset = 0; offset < count; offset += page_rows) {
        std::vector<Observation> page;
        for (size_t i = offset; i < std::min(count, offset + page_rows); ++i) page.push_back(Input(config, i));
        Check(reference.ValidateBatch(page) == 0, reference.LastError());
        for (const auto& observation : page) {
            EvaluationOutput evaluated;
            Check(reference.Submit(observation, &evaluated) == 0, reference.LastError());
            for (auto& row : evaluated.results) expected_rows.push_back(std::move(row));
            for (auto& row : evaluated.forecasts) expected_rows.push_back(std::move(row));
        }
        std::vector<BlockTransformOutputV1> outputs;
        const int64_t timestamp = 123 + callbacks++;
        Check(task->ProcessBlock(Wire(page), timestamp, &outputs) == 0, task->LastError());
        size_t rows = 0, nonempty = 0;
        for (const auto& output : outputs) {
            Check(output.batch->schema()->Equals(*MakeSchema(SchemaKind::kResults), true), "results Schema");
            Check(output.ts_ms == timestamp, "callback timestamp preserved");
            if (!output.batch->num_rows()) continue;
            ++nonempty;
            rows += output.batch->num_rows();
            if (!baseline) Check(output.batch->num_rows() <= 4096, "batch row limit");
            actual.push_back(output.batch);
        }
        if (baseline)
            Check(nonempty == page.size(), "reproduce original observation-sized batches");
        else
            Check(nonempty == (rows + 4095) / 4096, "expected bounded page-local batch count");
        batches += nonempty;
    }
    std::vector<BlockTransformOutputV1> tail;
    Check(task->Flush(&tail) == 0, task->LastError());
    Check(tail.empty(), "no delayed rows across callbacks or EOF");
    Check(reference.Finish() == 0, reference.LastError());
    Batch expected;
    std::string error;
    Check(EncodeResults(config, expected_rows, 268435456, &expected, &error) == 0, error);
    Equal(Merge(actual), expected);
    std::vector<ModelParametersRow> model_rows;
    Check(reference.ExportModelParameters(&model_rows) == 0, reference.LastError());
    Batch expected_model;
    Check(EncodeModelParameters(config, model_rows, 268435456, &expected_model, &error) == 0, error);
    DataFrame final_model;
    Check(model_sink->Read(&final_model) == 0, "read model");
    Equal(final_model.ToArrow(), expected_model);
    tail.clear();
    Check(task->Flush(&tail) != 0 && tail.empty(), "duplicate Flush rejected");
    std::cout << "PASS rows=" << count << " horizon=" << horizon << " page=" << page_rows
              << " result_rows=" << expected->num_rows() << " batches=" << batches << std::endl;
}
void BudgetAndOwnership() {
    auto config = Config();
    Querier owner;
    EvaluationEngine engine;
    Check(engine.Open(config, &owner.plugin, &owner.plugin) == 0, engine.LastError());
    EvaluationOutput evaluated;
    Check(engine.Submit(Input(config, 0), &evaluated) == 0 && evaluated.results.size() == 1, engine.LastError());
    auto row = evaluated.results.front();
    row.input.source_epoch.assign(1000, 'x');
    Batch expected;
    std::string error;
    Check(EncodeResults(config, {row}, 268435456, &expected, &error) == 0, error);
    uint64_t peak = 0;
    std::shared_ptr<arrow::Array> extracted;
    {
        ResultBatchEncoder encoder(config, 268435456);
        std::vector<Batch> batches;
        Check(encoder.Append(row, &batches, &error) == 0 && batches.empty(), error);
        auto changed = row;
        changed.input.source_epoch.assign(1000, 'y');
        row = changed;
        Check(encoder.Flush(&batches, &error) == 0 && batches.size() == 1, error);
        Equal(batches.front(), expected);
        extracted = batches.front()->GetColumnByName("source_epoch");
        peak = encoder.Stats().peak_staging_bytes;
        Check(encoder.Stats().result_rows == 1 && encoder.Stats().result_batches == 1, "batch stats");
    }
    Check(std::static_pointer_cast<arrow::StringArray>(extracted)->GetString(0) == std::string(1000, 'x'),
          "extracted array owns pool after encoder/batch destruction");
    for (uint64_t budget : {peak, peak - 1}) {
        ResultBatchEncoder encoder(config, budget);
        std::vector<Batch> batches;
        int rc = encoder.Append(row, &batches, &error);
        if (rc == 0) rc = encoder.Flush(&batches, &error);
        Check((rc == 0) == (budget == peak), "exact allocation peak budget boundary");
        Check(encoder.Stats().peak_staging_bytes <= budget, "never exceeds allocation budget");
    }
    uint64_t full_peak = 0;
    std::vector<Batch> reference;
    {
        ResultBatchEncoder encoder(config, 268435456);
        for (size_t i = 0; i < 128; ++i) Check(encoder.Append(row, &reference, &error) == 0, error);
        Check(encoder.Flush(&reference, &error) == 0, error);
        full_peak = encoder.Stats().peak_staging_bytes;
    }
    {
        ResultBatchEncoder encoder(config, full_peak - 64);
        std::vector<Batch> batches;
        for (size_t i = 0; i < 128; ++i) Check(encoder.Append(row, &batches, &error) == 0, error);
        Check(encoder.Flush(&batches, &error) == 0, error);
        Check(batches.size() > 1, "byte budget splits before row limit");
        Equal(Merge(batches), Merge(reference));
        Check(encoder.Stats().peak_staging_bytes <= full_peak - 64, "split peak bounded");
    }
    // A callback that has already encoded full batches must still reject cumulative excess atomically.
    auto limited = Config(0, 2097152);
    auto library = std::make_shared<Library>();
    auto task = Task(library, limited, false);
    Check(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("dataframe.samples") == 0,
          task->LastError());
    std::shared_ptr<arrow::Schema> schema;
    Check(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0, task->LastError());
    std::vector<Observation> page;
    for (size_t i = 0; i < 8193; ++i) page.push_back(Input(limited, i));
    std::vector<BlockTransformOutputV1> outputs;
    auto wire = Wire(page);
    std::vector<Observation> decoded;
    Check(DecodeObservations(wire, limited.config.read.max_pending_bytes, &decoded, &error) == 0, error);
    Check(task->ProcessBlock(wire, 1, &outputs) != 0 && outputs.empty(), "no partial callback on budget failure");
    Check(task->LastError().find("baseliner output budget") != std::string::npos,
          "preserve output budget failure cause");
    std::cout << "PASS byte boundaries, split, cumulative failure, owned strings/allocator" << std::endl;
}
void AnalysisKinds() {
    for (const std::string kind : {"value", "ratio", "relation"})
        for (uint32_t horizon : {0, 2}) {
            auto config = Config(horizon, 268435456, kind);
            auto library = std::make_shared<Library>();
            auto task = Task(library, config, false);
            Check(
                dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("dataframe.samples") == 0,
                task->LastError());
            std::shared_ptr<arrow::Schema> schema;
            Check(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0, task->LastError());
            Querier owner;
            EvaluationEngine reference;
            Check(reference.Open(config, &owner.plugin, &owner.plugin) == 0, reference.LastError());
            std::vector<Batch> expected, actual;
            std::vector<Observation> observations;
            for (size_t bucket = 0; bucket < 96; ++bucket)
                for (size_t series = 1; series <= 3; ++series) observations.push_back(Input(config, bucket, series));
            for (size_t offset = 0; offset < observations.size(); offset += 97) {
                std::vector<Observation> page(observations.begin() + offset,
                                              observations.begin() + std::min(observations.size(), offset + 97));
                std::vector<EvaluationRow> pending;
                Check(reference.ValidateBatch(page) == 0, reference.LastError());
                for (const auto& observation : page) {
                    EvaluationOutput out;
                    Check(reference.Submit(observation, &out) == 0, reference.LastError());
                    for (auto& row : out.results) pending.push_back(std::move(row));
                    for (auto& row : out.forecasts) pending.push_back(std::move(row));
                    if (out.relation) {
                        Batch results, fusion;
                        std::string error;
                        Check(EncodeResults(config, pending, 268435456, &results, &error) == 0, error);
                        expected.push_back(results);
                        pending.clear();
                        Check(EncodeFusion(config, observation, *out.relation, 268435456, &fusion, &error) == 0, error);
                        expected.push_back(fusion);
                    }
                }
                if (!pending.empty()) {
                    Batch results;
                    std::string error;
                    Check(EncodeResults(config, pending, 268435456, &results, &error) == 0, error);
                    expected.push_back(results);
                }
                std::vector<BlockTransformOutputV1> outputs;
                Check(task->ProcessBlock(Wire(page), offset, &outputs) == 0, task->LastError());
                for (const auto& out : outputs) {
                    Check(out.ts_ms == static_cast<int64_t>(offset), "analysis callback timestamp");
                    actual.push_back(out.batch);
                }
            }
            Check(actual.size() == expected.size(), "Schema boundaries and fusion order");
            for (size_t i = 0; i < actual.size(); ++i) Equal(actual[i], expected[i]);
            std::vector<BlockTransformOutputV1> tail;
            Check(task->Flush(&tail) == 0 && tail.empty(), task->LastError());
            std::cout << "PASS analysis=" << kind << " horizon=" << horizon << " multi-series/fusion order"
                      << std::endl;
        }
}
// A controlled service exercises cancellation/codec failure after a full output batch was staged.
struct HookValue final : IBaselineValueTask, IBaselineValueEvaluationV1 {
    size_t count = 0;
    std::function<void(size_t)> on_submit;
    bool invalid = false;
    const char* Id() const override { return "hook"; }
    const char* Name() const override { return "hook"; }
    BaselineTaskKind Kind() const override { return BaselineTaskKind::kValue; }
    BaselineSerializationResult ExportConfig(BaselineSerializationFormat) const override { return {}; }
    BaselineSerializationResult QueryTaskSnapshot(BaselineSerializationFormat) const override { return {}; }
    BaselineSerializationResult QuerySeriesSnapshot(std::string_view, BaselineSerializationFormat) const override {
        return {};
    }
    BaselineStatus Close() override { return BaselineStatus::kOk; }
    RollingBaselineResult SubmitObservation(const ValueRollingObservation&, const RollingSubmitOptions&) override {
        return {};
    }
    RollingPrediction PredictRolling(std::string_view, int64_t) const override { return {}; }
    RollingPredictionSequence PredictRolling(std::string_view, int64_t, uint32_t) const override { return {}; }
    BootstrapTrainResult Bootstrap(const ValueBootstrapInput&) override { return {}; }
    BootstrapPrediction PredictBootstrap(std::string_view, int64_t, const BootstrapPredictionOptions&) const override {
        return {};
    }
    BootstrapPredictionSequence PredictBootstrap(std::string_view, int64_t, uint32_t,
                                                 const BootstrapPredictionOptions&) const override {
        return {};
    }
    BaselineSerializationResult ExportBootstrapArtifact(BaselineSerializationFormat) const override { return {}; }
    BaselineStatus LoadBootstrapArtifact(std::string_view, BaselineSerializationFormat) override {
        return BaselineStatus::kInvalidArgument;
    }
    BaselineSerializationResult ExportBootstrapSeed(BaselineSerializationFormat) const override { return {}; }
    BaselineEvaluationResultV1 SubmitObservationWithReadiness(const ValueRollingObservation& input,
                                                              const RollingSubmitOptions&) override {
        ++count;
        if (on_submit) on_submit(count);
        BaselineEvaluationResultV1 result;
        result.readiness = BaselineEvaluationReadinessV1::kColdStart;
        result.result.observed = invalid && count == 4097 ? std::numeric_limits<double>::quiet_NaN() : input.value;
        return result;
    }
};
struct HookService final : IBaselineService,
                           IBaselineStateControlServiceV1,
                           IBaselineTaskStateControlV1,
                           std::enable_shared_from_this<HookService> {
    std::shared_ptr<HookValue> value = std::make_shared<HookValue>();
    std::pair<BaselineStatus, std::shared_ptr<IBaselineValueTask>> CreateValueTask(
        std::string_view, BaselineSerializationFormat) override {
        return {BaselineStatus::kOk, value};
    }
    std::pair<BaselineStatus, std::shared_ptr<IBaselineRatioTask>> CreateRatioTask(
        std::string_view, BaselineSerializationFormat) override {
        return {BaselineStatus::kInvalidArgument, nullptr};
    }
    std::pair<BaselineStatus, std::shared_ptr<IBaselineRelationTask>> CreateRelationTask(
        std::string_view, BaselineSerializationFormat) override {
        return {BaselineStatus::kInvalidArgument, nullptr};
    }
    BaselineSerializationResult QueryServiceSnapshot(BaselineSerializationFormat) const override { return {}; }
    std::pair<BaselineStatus, std::shared_ptr<IBaselineTaskStateControlV1>> Bind(
        std::shared_ptr<IBaselineTask>, const BaselineStateLimitsV1&) override {
        return {BaselineStatus::kOk, shared_from_this()};
    }
    BaselineStatus ReleaseIdentity(std::string_view, BaselineStateReleaseScopeV1) override {
        return BaselineStatus::kOk;
    }
    std::pair<BaselineStatus, BaselineStateUsageV1> QueryUsage() const override { return {BaselineStatus::kOk, {}}; }
};
void StagedFailure() {
    for (bool cancel : {false, true}) {
        auto config = Config();
        auto service = std::make_shared<HookService>();
        auto library = std::make_shared<Library>();
        library->querier->service = service.get();
        library->querier->control = service.get();
        auto task = Task(library, config, false);
        Check(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("dataframe.samples") == 0,
              task->LastError());
        std::shared_ptr<arrow::Schema> schema;
        Check(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0, task->LastError());
        service->value->invalid = !cancel;
        service->value->on_submit = [&](size_t count) {
            if (cancel && count == 4097) task->Cancel();
        };
        std::vector<Observation> page;
        for (size_t i = 0; i < 8193; ++i) page.push_back(Input(config, i));
        std::vector<BlockTransformOutputV1> outputs;
        Check(task->ProcessBlock(Wire(page), 0, &outputs) != 0 && outputs.empty(),
              "no partial staged output on error/cancel");
        Check(service->value->count == 4097, "one Submit per accepted row; no retry after failure/cancel");
        Check(task->Flush(&outputs) != 0 && outputs.empty(), "failed/cancelled task rejects EOF");
        std::cout << "PASS staged " << (cancel ? "cancellation" : "codec failure") << " without partial delivery"
                  << std::endl;
    }
}
}  // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--full") {
            Case(57761, 0, 4096, false);
            return 0;
        }
        const std::string mode = argc == 2 ? argv[1] : "--boundaries";
        const bool baseline = mode == "--baseline";
        Check(ResultBatchLimits{}.max_rows == 4096 && ResultBatchStats{}.peak_staging_bytes == 0,
              "frozen internal batch contract");
        if (baseline || mode == "--boundaries")
            for (size_t count : {0, 1, 4095, 4096, 4097, 8193}) Case(count, 0, 8193, baseline);
        if (baseline || mode == "--paging") {
            for (size_t page : {1, 256, 4096}) Case(4097, 0, page, baseline);
            Case(4097, 2, 4096, baseline);
        }
        if (mode == "--budget") BudgetAndOwnership();
        if (mode == "--analysis") AnalysisKinds();
        if (mode == "--failure") StagedFailure();
        Check(baseline || mode == "--boundaries" || mode == "--paging" || mode == "--budget" || mode == "--analysis" ||
                  mode == "--failure",
              "unknown batching test mode");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
