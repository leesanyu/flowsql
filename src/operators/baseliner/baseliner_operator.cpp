// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "baseliner_operator.h"
#include <arrow/util/byte_size.h>
#include <framework/core/dataframe.h>
#include <framework/interfaces/iblock_transform_model_output.h>
#include <framework/interfaces/iblock_transform_result_output.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <set>
#include "durable_store.h"
#include "model_output.h"
#include "poll_input.h"
#include "result_codec.h"

namespace flowsql::baseliner {
namespace {
ConfigStatus ResolveCreation(const char* with, IQuerier* querier, ConfigSnapshot* result) {
    if (!with || std::strlen(with) > kMaxConfigBytes + 65536) return {ConfigError::kLimit, {}, "invalid config size"};
    rapidjson::Document doc;
    doc.Parse(with);
    if (!doc.IsObject()) return {ConfigError::kJson, {}, "invalid WITH object"};
    std::set<std::string> fields;
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        const std::string name(it->name.GetString(), it->name.GetStringLength());
        if (name != "config" && name != "parameters" && name != "model_output")
            return {ConfigError::kUnknown, {}, "unknown WITH field"};
        if (!fields.insert(name).second) return {ConfigError::kDuplicate, {}, "duplicate WITH field"};
    }
    if (doc.HasMember("parameters") == doc.HasMember("config"))
        return {ConfigError::kValue, {}, "exactly one config or parameters required"};
    auto* registry =
        querier ? static_cast<IConfigChannelRegistryV1*>(querier->First(IID_CONFIG_CHANNEL_REGISTRY_V1)) : nullptr;
    ConfigSnapshot parsed;
    struct SnapshotRegistry final : IConfigChannelRegistryV1 {
        ConfigChannelSnapshot snapshot;
        int Resolve(const char*, ConfigChannelSnapshot* out, std::string*) override {
            *out = snapshot;
            return 0;
        }
    } cached;
    if (doc.HasMember("parameters") && doc["parameters"].IsString()) {
        auto status = ParseConfig({doc["parameters"].GetString(), doc["parameters"].GetStringLength()}, &parsed);
        if (!status.ok()) return status;
    } else if (doc.HasMember("config") && doc["config"].IsString() && registry) {
        const std::string reference(doc["config"].GetString(), doc["config"].GetStringLength());
        const auto at = reference.rfind('@');
        uint64_t revision = 0;
        if (reference.compare(0, 7, "config.") != 0 || at == std::string::npos || at <= 7)
            return {ConfigError::kReference, {}, "exact config revision required"};
        const auto revision_text = reference.substr(at + 1);
        const auto converted =
            std::from_chars(revision_text.data(), revision_text.data() + revision_text.size(), revision);
        if (converted.ec != std::errc{} || converted.ptr != revision_text.data() + revision_text.size() ||
            revision == 0 || revision_text != std::to_string(revision))
            return {ConfigError::kReference, {}, "exact config revision required"};
        std::string error;
        if (registry->Resolve(reference.c_str(), &cached.snapshot, &error) != 0 || !cached.snapshot.content)
            return {ConfigError::kReference, {}, error};
        auto status = ParseConfig(*cached.snapshot.content, &parsed);
        if (!status.ok()) return status;
    } else
        return {ConfigError::kReference, {}, "config/parameters required"};
    return ResolveConfig(with, parsed.config.source, doc.HasMember("config") ? &cached : nullptr, result);
}
class BaselinerTask final : public IBlockTransformTaskV2,
                            public IBlockTransformInputSourceTaskV1,
                            public IBlockTransformDatabaseInputTaskV1,
                            public IBlockTransformDataFrameInputTaskV1,
                            public IBlockTransformInputProgressTaskV1,
                            public IBlockTransformManagedSinkTaskV1,
                            public IBlockTransformModelOutputTaskV1,
                            public IBlockTransformResultOutputTaskV1 {
 public:
    BaselinerTask(ConfigSnapshot config, IQuerier* querier, std::string run)
        : config_(std::move(config)), querier_(querier), run_(std::move(run)) {}
    ~BaselinerTask() override {
        auto input = std::atomic_load(&input_);
        if (input) input->Close();
        engine_.Close();
        store_.Close();
    }
    int BindResultOutput(const BlockResultOutputBindingV1& binding) override {
        if (binding.struct_size != sizeof(binding) || binding.contract_version != 1 || !binding.target || opened_ ||
            failed_ || cancelled_ || result_bound_ || managed_)
            return Fail("invalid result output binding");
        result_target_ = binding.target;
        if (result_target_ == config_.config.source ||
            (!config_.model_output.empty() && result_target_ == config_.model_output))
            return Fail("results overlap source/model target");
        if (binding.database) {
            const auto a = result_target_.find('.'), b = result_target_.find('.', a + 1);
            if (a == std::string::npos || b == std::string::npos ||
                result_target_.substr(0, a) != binding.database->Category() ||
                result_target_.substr(a + 1, b - a - 1) != binding.database->Name() ||
                config_.config.mode != Mode::kSnapshot)
                return Fail("specified result table requires snapshot and matching database lease");
            if (results_.Open(config_, binding.database, result_target_.substr(b + 1), SchemaKind::kResults) != 0)
                return Fail(results_.LastError());
            result_database_ = true;
            if (model_bound_ && !model_dataframe_ && results_.Join(models_) != 0) return Fail(results_.LastError());
        } else if (!result_target_.empty() && result_target_.compare(0, 10, "dataframe.") != 0)
            return Fail("result DataFrame target mismatch");
        result_bound_ = true;
        return 0;
    }
    int CompleteResultOutputPublication(bool published) override {
        if (!result_bound_ || result_database_ || result_state_ != "staged") return Fail("results were not staged");
        if (!published) {
            result_state_ = cancelled_ ? "cancelled" : "failed";
            return Fail("result DataFrame publication failed");
        }
        result_state_ = "committed";
        return cancelled_ ? Fail("results cancelled after publication") : 0;
    }
    std::string ModelOutputTarget() const override { return config_.model_output; }
    int BindModelOutput(const BlockModelOutputBindingV1& binding) override {
        if (binding.struct_size != sizeof(binding) || binding.contract_version != 1 || !binding.primary_target ||
            opened_ || failed_ || cancelled_ || model_bound_ || config_.model_output.empty() ||
            bool(binding.database) == bool(binding.dataframe))
            return Fail("invalid model output binding");
        const auto& target = config_.model_output;
        const std::string primary = binding.primary_target;
        if (target == primary || target == config_.config.source)
            return Fail("model output overlaps source/result target");
        const auto a = target.find('.'), b = target.find('.', a + 1);
        const auto category = target.substr(0, a), name = target.substr(a + 1, b - a - 1);
        const auto source_dot = config_.config.source.find('.', config_.config.source.find('.') + 1);
        const auto source_channel = config_.config.source.substr(0, source_dot);
        const auto target_channel = target.substr(0, b);
        const auto table = b == std::string::npos ? std::string{} : target.substr(b + 1);
        const std::set<std::string> reserved = {
            "baseline_tasks",      "baseline_model_versions",     "baseline_source_positions",
            "baseline_results_v1", "baseline_relation_fusion_v1", "baseline_maintenance_v1"};
        if (primary == target_channel && reserved.count(table)) return Fail("model output overlaps managed relations");
        if (source_channel == target_channel)
            for (const auto& dataset : config_.config.datasets)
                if (dataset.table == table &&
                    (dataset.schema.empty() || (category == "sqlite" && dataset.schema == "main")))
                    return Fail("model output overlaps source relation");
        if (category == "dataframe") {
            if (!binding.dataframe || std::string(binding.dataframe->Category()) != category ||
                std::string(binding.dataframe->Name()) != name || !binding.dataframe->IsOpened())
                return Fail("model DataFrame binding mismatch");
            model_dataframe_ = binding.dataframe;
        } else {
            if (!binding.database || std::string(binding.database->Category()) != category ||
                std::string(binding.database->Name()) != name)
                return Fail("model database binding mismatch");
            if (models_.Open(config_, binding.database, table) != 0) return Fail(models_.LastError());
            if (source_channel == target_channel)
                for (const auto& dataset : config_.config.datasets)
                    if (dataset.table == table && (dataset.schema.empty() || dataset.schema == models_.DefaultSchema()))
                        return Fail("model output overlaps source relation");
        }
        model_bound_ = true;
        return 0;
    }
    int CompleteModelOutputPublication(bool published) override {
        if (!model_dataframe_ || model_state_ != "staged") return Fail("model DataFrame was not staged");
        if (!published) {
            model_state_ = cancelled_ ? "cancelled" : "failed";
            return Fail("model DataFrame publication failed");
        }
        model_state_ = "committed";
        return cancelled_ ? Fail("model cancelled after publication") : 0;
    }
    int BindInputSource(const char* source) override {
        if (opened_ || bound_ || !source || config_.config.source != source) return Fail("source binding mismatch");
        bound_ = true;
        return 0;
    }
    int BindManagedSink(const BlockTransformManagedSinkBindingV1& binding) override {
        if (opened_ || managed_ || result_bound_ || failed_ || cancelled_ || binding.struct_size != sizeof(binding) ||
            binding.contract_version != 1 || !binding.sink_channel || !binding.target || !binding.category ||
            !binding.name || (binding.relation && binding.relation[0]))
            return Fail("invalid baseliner managed target");
        auto* channel = dynamic_cast<IDatabaseChannel*>(binding.sink_channel);
        if (!channel || std::string(binding.target) != std::string(binding.category) + "." + binding.name ||
            std::string(channel->Category()) != binding.category || std::string(channel->Name()) != binding.name)
            return Fail("managed target binding mismatch");
        const std::set<std::string> reserved = {
            "baseline_tasks",      "baseline_model_versions",     "baseline_source_positions",
            "baseline_results_v1", "baseline_relation_fusion_v1", "baseline_maintenance_v1"};
        for (const auto& dataset : config_.config.datasets)
            if (reserved.count(dataset.table)) return Fail("source overlaps managed baseline relations");
        if (model_bound_ && !model_dataframe_) {
            std::vector<Dataset> targets;
            for (const auto& table : reserved) {
                Dataset dataset;
                dataset.table = table;
                targets.push_back(std::move(dataset));
            }
            if (models_.ValidateRelations(channel, targets) != 0) return Fail(models_.LastError());
        }
        if (store_.Open(config_, channel, run_) != 0) return Fail(store_.LastError());
        if (store_.Load(&saved_) != 0) return Fail(store_.LastError());
        progress_ = saved_.progress;
        if (saved_.generation > 0) {
            rapidjson::Document doc;
            doc.Parse<rapidjson::kParseFullPrecisionFlag>(saved_.checkpoint.data(), saved_.checkpoint.size());
            if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("version") || !doc["version"].IsUint() ||
                doc["version"].GetUint() != 1 || !doc.HasMember("engine") || !doc["engine"].IsString() ||
                !doc.HasMember("pending_starts") || !doc["pending_starts"].IsArray() || doc.MemberCount() != 3)
                return Fail("invalid task checkpoint envelope");
            std::set<std::string> seen;
            for (const auto& item : doc["pending_starts"].GetArray()) {
                if (!item.IsObject() || item.MemberCount() != 3 || !item.HasMember("dataset") ||
                    !item["dataset"].IsString() || !item.HasMember("bucket") || !item["bucket"].IsInt64() ||
                    !item.HasMember("epoch") || !item["epoch"].IsString() || item["epoch"].GetStringLength() == 0)
                    return Fail("invalid pending restart floor");
                std::string dataset(item["dataset"].GetString(), item["dataset"].GetStringLength());
                if (!seen.insert(dataset).second ||
                    std::none_of(config_.config.datasets.begin(), config_.config.datasets.end(),
                                 [&](auto& d) { return d.id == dataset; }))
                    return Fail("invalid pending dataset");
                pending_starts_.emplace(dataset, item["bucket"].GetInt64());
                source_epochs_.emplace(dataset,
                                       std::string(item["epoch"].GetString(), item["epoch"].GetStringLength()));
            }
            saved_.checkpoint.assign(doc["engine"].GetString(), doc["engine"].GetStringLength());
        }
        managed_ = true;
        return 0;
    }
    std::string ManagedSinkResultJson() const override {
        rapidjson::StringBuffer b;
        rapidjson::Writer<rapidjson::StringBuffer> w(b);
        w.StartObject();
        auto text = [&](const char* key, const std::string& value) {
            w.Key(key);
            w.String(value.data(), value.size());
        };
        text("task_key", config_.config.task_key);
        text("run_id", run_);
        text("config_hash", config_.sha256_hex);
        w.Key("rows_written");
        w.Int64(result_bound_
                    ? (result_database_ ? results_.RowsWritten() : (result_state_ == "committed" ? result_rows_ : 0))
                    : rows_written_);
        w.Key("generation");
        w.Int64(store_.Generation());
        text("results_relation", result_bound_ ? result_target_ : "baseline_results_v1");
        text("models_relation", result_bound_ ? config_.model_output : "baseline_model_versions");
        w.Key("source_positions");
        w.StartArray();
        for (const auto& p : durable_progress_.datasets) {
            w.StartObject();
            text("dataset_id", p.dataset_id);
            text("epoch", p.epoch);
            text("committed_position", p.committed_position);
            w.Key("closed_before_bucket");
            w.Int64(p.closed_before_bucket);
            w.EndObject();
        }
        w.EndArray();
        if (result_bound_ || !result_target_.empty()) {
            w.Key("results_output");
            w.StartObject();
            text("target", result_target_);
            auto state = result_database_ ? results_.State() : result_state_;
            if ((failed_ || cancelled_) && state != "committed" && state != "unknown")
                state = cancelled_ && result_state_ != "failed" ? "cancelled" : "failed";
            text("status", state);
            w.Key("rows_written");
            w.Int64(result_database_ ? results_.RowsWritten() : (result_state_ == "committed" ? result_rows_ : 0));
            w.EndObject();
        }
        if (!config_.model_output.empty()) {
            w.Key("model_output");
            w.StartObject();
            text("target", config_.model_output);
            auto state = model_dataframe_ ? model_state_ : models_.State();
            if ((failed_ || cancelled_) && state != "committed" && state != "unknown")
                state = cancelled_ && model_state_ != "failed" ? "cancelled" : "failed";
            text("status", state);
            w.Key("rows_written");
            w.Int64(model_dataframe_ ? (model_state_ == "committed" ? model_rows_ : 0) : models_.RowsWritten());
            w.EndObject();
        }
        if (!error_.empty()) text("error", error_);
        w.EndObject();
        return {b.GetString(), b.GetSize()};
    }
    int Open(std::shared_ptr<arrow::Schema> schema, std::shared_ptr<arrow::Schema>* output) override {
        if (output) output->reset();
        if (!output || opened_ || failed_ || cancelled_ || !bound_ ||
            (!config_.model_output.empty() && !model_bound_) || !schema ||
            !schema->Equals(*MakeSchema(SchemaKind::kObservation), true))
            return Fail("invalid baseliner Open/schema");
        auto* service = querier_ ? static_cast<IBaselineService*>(querier_->First(IID_BASELINE_SERVICE)) : nullptr;
        auto* control =
            querier_
                ? static_cast<IBaselineStateControlServiceV1*>(querier_->First(IID_BASELINE_STATE_CONTROL_SERVICE_V1))
                : nullptr;
        auto* checkpoints =
            querier_ ? static_cast<IBaselineCheckpointServiceV1*>(querier_->First(IID_BASELINE_CHECKPOINT_SERVICE_V1))
                     : nullptr;
        if ((managed_ || config_.config.state.idle_timeout_ms) && !checkpoints)
            return Fail("checkpoint service unavailable");
        if (engine_.Open(config_, service, control,
                         (managed_ || config_.config.state.idle_timeout_ms) ? checkpoints : nullptr) != 0)
            return Fail(engine_.LastError());
        engine_.SetTime(Now(), WallNow());
        if (engine_.SetSourceProgress(progress_) != 0) return Fail(engine_.LastError());
        if (managed_ && saved_.generation > 0 && engine_.RestoreCheckpoint(saved_.checkpoint) != 0)
            return Fail(engine_.LastError());
        saved_.checkpoint.clear();
        durable_progress_ = progress_;
        if (managed_) SetDeadlines(Now());
        opened_ = true;
        *output = MakeSchema(SchemaKind::kResults);
        return 0;
    }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t timestamp,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        if (!outputs || !outputs->empty() || !opened_ || failed_ || finished_ || cancelled_)
            return Fail("baseliner task unavailable");
        std::vector<Observation> observations;
        if (DecodeObservations(input, config_.config.read.max_pending_bytes, &observations, &error_) != 0)
            return Fail(error_);
        if (managed_ && saved_.generation > 0) {
            std::vector<Observation> fresh;
            for (auto& observation : observations) {
                int replay = engine_.ClassifyReplay(observation);
                if (replay < 0) return Fail(engine_.LastError());
                if (!replay) fresh.push_back(std::move(observation));
            }
            observations = std::move(fresh);
        }
        engine_.SetTime(Now(), WallNow());
        std::vector<BlockTransformOutputV1> staged;
        if (Maintain(timestamp, &staged) != 0) return -EINVAL;
        if (engine_.ValidateBatch(observations) != 0) return Fail(engine_.LastError());
        std::vector<BlockTransformOutputV1> next;
        uint64_t bytes = 0;
        for (const auto& observation : observations) {
            if (cancelled_) return -ECANCELED;
            EvaluationOutput evaluated;
            if (engine_.Submit(observation, &evaluated) != 0) return Fail(engine_.LastError());
            evaluated.results.insert(evaluated.results.end(), std::make_move_iterator(evaluated.forecasts.begin()),
                                     std::make_move_iterator(evaluated.forecasts.end()));
            std::shared_ptr<arrow::RecordBatch> batch;
            if (!evaluated.results.empty()) {
                if (EncodeResults(config_, evaluated.results, config_.config.read.max_pending_bytes, &batch, &error_) !=
                    0)
                    return Fail(error_);
                bytes += arrow::util::TotalBufferSize(*batch);
                next.push_back({std::move(batch), timestamp});
            }
            if (evaluated.relation && !result_bound_) {
                if (EncodeFusion(config_, observation, *evaluated.relation, config_.config.read.max_pending_bytes,
                                 &batch, &error_) != 0)
                    return Fail(error_);
                bytes += arrow::util::TotalBufferSize(*batch);
                next.push_back({std::move(batch), timestamp});
            }
            if (bytes > config_.config.read.max_pending_bytes) return Fail("baseliner output budget exceeded");
        }
        if (cancelled_) return -ECANCELED;
        if (managed_) {
            if (bytes > config_.config.read.max_pending_bytes - pending_bytes_)
                return Fail("unpublished results budget exceeded");
            for (auto& output : next) pending_.push_back(std::move(output.batch));
            pending_bytes_ += bytes;
            std::set<int64_t> buckets;
            for (const auto& observation : observations) buckets.insert(observation.bucket);
            pending_buckets_ += buckets.size();
            dirty_ = dirty_ || !observations.empty();
            if (Maintain(timestamp, &staged) != 0) return -EINVAL;
            if (pending_buckets_ >= config_.config.persistence.checkpoint_every_buckets && Publish() != 0)
                return -EINVAL;
        } else if (result_bound_) {
            if (bytes > config_.config.read.max_pending_bytes - result_bytes_)
                return Fail("specified result buffer budget exceeded");
            result_bytes_ += bytes;
            for (auto& output : next) {
                result_rows_ += output.batch->num_rows();
                if (result_database_)
                    result_batches_.push_back(std::move(output.batch));
                else
                    staged.push_back(std::move(output));
            }
        } else
            staged.insert(staged.end(), std::make_move_iterator(next.begin()), std::make_move_iterator(next.end()));
        *outputs = std::move(staged);
        return static_cast<int>(BlockTransformStatusV1::kContinue);
    }
    int Flush(std::vector<BlockTransformOutputV1>* outputs) override {
        if (!outputs || !outputs->empty() || !opened_ || failed_ || finished_ || cancelled_)
            return Fail("invalid baseliner Flush");
        if (engine_.Finish() != 0) return Fail(engine_.LastError());
        if (managed_) {
            dirty_ = true;
            if (Publish() != 0) return -EINVAL;
        }
        std::shared_ptr<arrow::RecordBatch> model_batch;
        if (!config_.model_output.empty()) {
            if (cancelled_) return -ECANCELED;
            std::vector<ModelParametersRow> rows;
            if (engine_.ExportModelParameters(&rows) != 0) return Fail(engine_.LastError());
            uint64_t retained = rows.capacity() * sizeof(ModelParametersRow);
            for (const auto& row : rows)
                retained += row.parameters.parameters_json.capacity() + row.parameters.maturity.capacity() +
                            row.identity.dataset_id.capacity() + row.identity.metric_id.capacity() +
                            row.identity.identity.capacity() + row.identity.source_epoch.capacity() +
                            row.model_basis_id.capacity();
            const auto budget = config_.config.read.max_pending_bytes - result_bytes_;
            if (retained >= budget) return Fail("final model buffer budget exceeded");
            if (EncodeModelParameters(config_, rows, budget - retained, &model_batch, &error_) != 0)
                return Fail(error_);
            rows.clear();
            rows.shrink_to_fit();
            if (cancelled_) return -ECANCELED;
            if (model_dataframe_) {
                DataFrame frame;
                frame.FromArrow(model_batch);
                if (model_dataframe_->Write(&frame) != 0) return Fail("model DataFrame staging failed");
                if (cancelled_) return -ECANCELED;
                model_rows_ = model_batch->num_rows();
                model_state_ = "staged";
            }
        }
        const bool joint = result_database_ && model_bound_ && !model_dataframe_ && results_.SharesSession(models_);
        if (result_database_) {
            if (results_.Write(result_batches_, joint ? &models_ : nullptr, joint ? model_batch : nullptr) != 0)
                return Fail(results_.LastError());
            result_batches_.clear();
        } else if (result_bound_) {
            if (!result_rows_) {
                std::shared_ptr<arrow::RecordBatch> empty;
                if (EncodeResults(config_, {}, config_.config.read.max_pending_bytes, &empty, &error_) != 0)
                    return Fail(error_);
                outputs->push_back({std::move(empty), 0});
            }
            result_state_ = "staged";
        }
        if (model_batch && !model_dataframe_ && !joint && models_.Write(model_batch) != 0)
            return Fail(models_.LastError());
        finished_ = true;
        return 0;
    }
    void Cancel() override {
        cancelled_ = true;
        auto input = std::atomic_load(&input_);
        if (input) input->Cancel();
        store_.Cancel();
        models_.Cancel();
        results_.Cancel();
    }
    std::string LastError() const override { return error_; }
    int GetTimeDriveState(BlockTransformTimeDriveStateV1* state) override {
        if (!state) return EINVAL;
        *state = {kBlockTransformTimeDriveStateV1Size, kBlockTransformTimeDriveVersionV1, 0, {}, 0};
        if (opened_ && !failed_ && !finished_ && !cancelled_) {
            auto deadline = engine_.NextMaintenanceDeadline();
            if (managed_) {
                const auto persistence = dirty_ ? std::min(checkpoint_deadline_, renew_deadline_) : renew_deadline_;
                deadline = deadline ? std::min(*deadline, persistence) : persistence;
            }
            if (deadline) {
                state->armed = 1;
                state->deadline_ns = *deadline;
            }
        }
        return 0;
    }
    int OnTime(const BlockTransformTimeEventV1& event, std::vector<BlockTransformOutputV1>* outputs) override {
        if (!outputs || !outputs->empty() || !opened_ || failed_ || finished_ || cancelled_ ||
            event.struct_size != sizeof(event) || event.contract_version != 1)
            return Fail("invalid checkpoint time event");
        engine_.SetTime(event.monotonic_now_ns, event.wall_now_ns);
        if (Maintain(event.wall_now_ns, outputs) != 0) return -EINVAL;
        if (managed_ && dirty_ && event.monotonic_now_ns >= checkpoint_deadline_) return Publish();
        if (managed_ && event.monotonic_now_ns >= renew_deadline_) {
            if (store_.Renew() != 0) return Fail(store_.LastError());
            renew_deadline_ = Now() + int64_t(config_.config.persistence.renew_ms) * 1000000;
        }
        return 0;
    }
    int CreateDatabaseInput(const BlockDatabaseInputBindingV1& binding, BlockDatabaseInputV1* output) override {
        if (!output) return EINVAL;
        *output = {};
        if (!ValidBlockDatabaseInputBindingV1(binding) || opened_ || failed_ || cancelled_ || !bound_ || input_ ||
            !config_.config.database_source || config_.config.source != binding.exact_source)
            return Fail("invalid database input binding");
        if (model_bound_ && !model_dataframe_ &&
            models_.ValidateRelations(binding.source.get(), config_.config.datasets) != 0)
            return Fail(models_.LastError());
        if (result_database_ && results_.ValidateRelations(binding.source.get(), config_.config.datasets) != 0)
            return Fail(results_.LastError());
        std::shared_ptr<IBlockStreamChannel> created;
        if (config_.config.mode == Mode::kSnapshot) {
            auto reader = std::make_shared<SnapshotInput>();
            std::atomic_store(&input_, std::static_pointer_cast<IBlockStreamChannel>(reader));
            if (cancelled_) reader->Cancel();
            auto snapshot = config_;
            if (managed_) snapshot.sha256_hex = RestoreCompatibilityHash(config_);
            if (reader->Initialize(std::move(snapshot), binding.source, binding.exact_source) != 0) {
                std::atomic_store(&input_, std::shared_ptr<IBlockStreamChannel>{});
                return Fail(reader->LastError());
            }
            created = std::move(reader);
        } else {
            auto reader = std::make_shared<PollInput>();
            std::atomic_store(&input_, std::static_pointer_cast<IBlockStreamChannel>(reader));
            if (cancelled_) reader->Cancel();
            if (reader->Initialize(config_, binding.source, binding.exact_source, saved_.progress, pending_starts_,
                                   source_epochs_) != 0) {
                std::atomic_store(&input_, std::shared_ptr<IBlockStreamChannel>{});
                return Fail(reader->LastError());
            }
            created = std::move(reader);
        }
        std::atomic_store(&input_, created);
        if (cancelled_) {
            created->Cancel();
            return -ECANCELED;
        }
        output->input = created.get();
        output->schema = MakeSchema(SchemaKind::kObservation);
        return 0;
    }
    int CreateDataFrameInput(const BlockDataFrameInputBindingV1& binding, BlockDataFrameInputV1* output) override {
        if (!output) return EINVAL;
        const bool valid_output =
            output->struct_size == sizeof(*output) && output->contract_version == kBlockDataFrameInputVersionV1;
        *output = {};
        if (!valid_output || !ValidBlockDataFrameInputBindingV1(binding) || opened_ || failed_ || cancelled_ ||
            !bound_ || input_ || !config_.config.dataframe_source || config_.config.source != binding.exact_source)
            return Fail("invalid DataFrame input binding");
        auto reader = std::make_shared<DataFrameSnapshotInput>();
        std::atomic_store(&input_, std::static_pointer_cast<IBlockStreamChannel>(reader));
        if (cancelled_) reader->Cancel();
        if (reader->InitializeDataFrame(config_, binding) != 0) {
            std::atomic_store(&input_, std::shared_ptr<IBlockStreamChannel>{});
            return Fail(reader->LastError());
        }
        const auto& fingerprint = reader->SourceFingerprint();
        const auto& dataset = config_.config.datasets.front();
        if (saved_.generation > 0 &&
            (progress_.datasets.size() != 1 || progress_.datasets.front().dataset_id != dataset.id ||
             progress_.datasets.front().epoch != fingerprint)) {
            std::atomic_store(&input_, std::shared_ptr<IBlockStreamChannel>{});
            return Fail("restored DataFrame snapshot fingerprint mismatch");
        }
        if (progress_.datasets.empty())
            progress_.datasets.push_back(
                {dataset.id, fingerprint, "snapshot:" + fingerprint, std::numeric_limits<int64_t>::min()});
        if (cancelled_) {
            reader->Cancel();
            return -ECANCELED;
        }
        output->input = reader.get();
        output->schema = reader->InputSchema();
        output->source_fingerprint = fingerprint;
        return 0;
    }
    void ReleaseDataFrameInput(IBlockStreamChannel* input) override { ReleaseDatabaseInput(input); }
    void ReleaseDatabaseInput(IBlockStreamChannel* input) override {
        auto owned = std::atomic_load(&input_);
        if (!owned || owned.get() != input) return;
        owned->Close();
        std::atomic_store(&input_, std::shared_ptr<IBlockStreamChannel>{});
    }
    int AcceptInputProgress(const BlockInputProgressV1& progress) override {
        if (!opened_ || failed_ || finished_ || cancelled_) return Fail("invalid progress lifecycle");
        std::vector<std::string> ids;
        for (const auto& d : config_.config.datasets) ids.push_back(d.id);
        if (!ValidateProgress(progress, ids).ok()) return Fail("invalid progress envelope");
        for (const auto& p : progress.datasets) {
            auto old = std::find_if(progress_.datasets.begin(), progress_.datasets.end(),
                                    [&](const auto& o) { return o.dataset_id == p.dataset_id; });
            if (old != progress_.datasets.end() &&
                (old->epoch != p.epoch || p.closed_before_bucket < old->closed_before_bucket))
                return Fail("regressing source progress");
        }
        bool changed = progress.datasets.size() != progress_.datasets.size();
        for (size_t i = 0; !changed && i < progress.datasets.size(); ++i) {
            auto& a = progress.datasets[i];
            auto& b = progress_.datasets[i];
            changed = a.dataset_id != b.dataset_id || a.epoch != b.epoch ||
                      a.committed_position != b.committed_position || a.closed_before_bucket != b.closed_before_bucket;
        }
        if (engine_.SetSourceProgress(progress) != 0) return Fail(engine_.LastError());
        progress_ = progress;
        if (managed_ && changed) {
            dirty_ = true;
            if (Publish() != 0) return -EINVAL;
        }
        return 0;
    }

 private:
    static int64_t Now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    static int64_t WallNow() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
    int Maintain(int64_t timestamp, std::vector<BlockTransformOutputV1>* outputs) {
        if (!config_.config.state.idle_timeout_ms) return 0;
        std::vector<MaintenanceRow> rows;
        if (engine_.Maintain(&rows) != 0) return Fail(engine_.LastError());
        if (rows.empty()) return 0;
        std::shared_ptr<arrow::RecordBatch> batch;
        if (EncodeMaintenance(
                config_, rows,
                std::min(config_.config.read.max_pending_bytes, config_.config.state.maintenance_max_bytes), &batch,
                &error_) != 0)
            return Fail(error_);
        if (managed_) {
            auto bytes = uint64_t(arrow::util::TotalBufferSize(*batch));
            if (bytes > config_.config.read.max_pending_bytes - pending_bytes_)
                return Fail("maintenance unpublished byte budget exceeded");
            pending_bytes_ += bytes;
            pending_.push_back(std::move(batch));
            dirty_ = true;
            return Publish();
        }
        if (!result_bound_) outputs->push_back({std::move(batch), timestamp});
        return 0;
    }
    void SetDeadlines(int64_t now) {
        checkpoint_deadline_ = now + int64_t(config_.config.persistence.checkpoint_interval_ms) * 1000000;
        renew_deadline_ = now + int64_t(config_.config.persistence.renew_ms) * 1000000;
    }
    int Publish() {
        if (!dirty_) return 0;
        engine_.SetTime(Now(), WallNow());
        std::string checkpoint;
        if (engine_.ExportCheckpoint(&checkpoint) != 0) return Fail(engine_.LastError());
        if (cancelled_) return -ECANCELED;
        auto input = std::atomic_load(&input_);
        if (auto* poll = dynamic_cast<PollInput*>(input.get())) {
            pending_starts_ = poll->PendingStarts();
            source_epochs_ = poll->SourceEpochs();
        }
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
        w.StartObject();
        w.Key("version");
        w.Uint(1);
        w.Key("engine");
        w.String(checkpoint.data(), checkpoint.size());
        w.Key("pending_starts");
        w.StartArray();
        for (const auto& start : pending_starts_) {
            w.StartObject();
            w.Key("dataset");
            w.String(start.first.data(), start.first.size());
            w.Key("bucket");
            w.Int64(start.second);
            w.Key("epoch");
            auto& epoch = source_epochs_.at(start.first);
            w.String(epoch.data(), epoch.size());
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        checkpoint.assign(buffer.GetString(), buffer.GetSize());
        if (checkpoint.size() > config_.config.persistence.max_checkpoint_bytes ||
            (config_.config.state.idle_timeout_ms && checkpoint.size() > config_.config.state.maintenance_max_bytes))
            return Fail("combined task checkpoint byte limit");
        if (store_.Publish(checkpoint, progress_, pending_) != 0) return Fail(store_.LastError());
        durable_progress_ = progress_;
        for (const auto& batch : pending_) rows_written_ += batch->num_rows();
        pending_.clear();
        pending_bytes_ = pending_buckets_ = 0;
        dirty_ = false;
        SetDeadlines(Now());
        return 0;
    }
    int Fail(std::string error) {
        error_ = std::move(error);
        failed_ = true;
        if (result_bound_ && result_state_ != "committed") result_state_ = cancelled_ ? "cancelled" : "failed";
        if (!config_.model_output.empty() && model_state_ != "committed")
            model_state_ = cancelled_ ? "cancelled" : "failed";
        return -EINVAL;
    }
    ConfigSnapshot config_;
    IQuerier* querier_;
    EvaluationEngine engine_;
    DurableStore store_;
    ModelOutputStore models_, results_;
    std::string result_target_, result_state_ = "pending";
    std::vector<std::shared_ptr<arrow::RecordBatch>> result_batches_;
    uint64_t result_bytes_ = 0;
    int64_t result_rows_ = 0;
    bool result_bound_ = false, result_database_ = false;
    std::shared_ptr<IDataFrameChannel> model_dataframe_;
    std::string model_state_ = "pending";
    int64_t model_rows_ = 0;
    bool model_bound_ = false;
    StoredGeneration saved_;
    BlockInputProgressV1 durable_progress_;
    std::vector<std::shared_ptr<arrow::RecordBatch>> pending_;
    std::map<std::string, int64_t> pending_starts_;
    std::map<std::string, std::string> source_epochs_;
    std::string run_;
    uint64_t pending_bytes_ = 0, pending_buckets_ = 0;
    int64_t checkpoint_deadline_ = 0, renew_deadline_ = 0;
    int64_t rows_written_ = 0;
    bool managed_ = false, dirty_ = false;
    std::shared_ptr<IBlockStreamChannel> input_;
    BlockInputProgressV1 progress_;
    std::atomic<bool> cancelled_{false};
    bool bound_ = false, opened_ = false, failed_ = false, finished_ = false;
    std::string error_;
};
}  // namespace
int BaselinerOperator::Load(IQuerier* querier) {
    if (!querier) return EINVAL;
    querier_ = querier;
    return 0;
}
int BaselinerOperator::Start() {
    if (!querier_) return EINVAL;
    started_ = true;
    return 0;
}
int BaselinerOperator::Stop() {
    started_ = false;
    return 0;
}
int BaselinerOperator::Unload() {
    querier_ = nullptr;
    return 0;
}
int BaselinerOperator::CreateTask(const BlockTransformTaskConfigV2& config, IBlockTransformTaskV2** task) {
    if (!task) return EINVAL;
    *task = nullptr;
    if (!started_ || config.struct_size != kBlockTransformTaskConfigV2Size ||
        config.contract_version != kBlockTransformContractVersionV2 || !config.task_id || !config.task_id[0])
        return EINVAL;
    if (config.pushed_filter_plan_json && config.pushed_filter_plan_json[0] &&
        std::strcmp(config.pushed_filter_plan_json, "{\"version\":1,\"root\":null}") != 0)
        return EINVAL;
    ConfigSnapshot parsed;
    if (!ResolveCreation(config.with_params_json, querier_, &parsed).ok()) return EINVAL;
    *task = new BaselinerTask(std::move(parsed), querier_,
                              std::string(config.task_id) + ":" +
                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    return 0;
}
void BaselinerOperator::ReleaseTask(IBlockTransformTaskV2* task) { delete task; }
int BaselinerOperator::NormalizeSourceConfig(const char* with, const char* source, std::string* output,
                                             std::string* error) const {
    if (!with || !source || !output) return EINVAL;
    ConfigSnapshot config;
    auto* registry =
        querier_ ? static_cast<IConfigChannelRegistryV1*>(querier_->First(IID_CONFIG_CHANNEL_REGISTRY_V1)) : nullptr;
    auto status = ResolveConfig(with, source, registry, &config);
    if (!status.ok()) {
        if (error) *error = status.path + ": " + status.message;
        return EINVAL;
    }
    if (!config.exact_reference.empty()) {
        *output = with;
    } else {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        writer.StartObject();
        writer.Key("parameters");
        writer.String(config.original_json.data(), config.original_json.size());
        if (!config.model_output.empty()) {
            writer.Key("model_output");
            writer.String(config.model_output.data(), config.model_output.size());
        }
        writer.EndObject();
        output->assign(buffer.GetString(), buffer.GetSize());
    }
    if (error) error->clear();
    return 0;
}
int BaselinerOperator::RequiresAsyncExecution(const char* with, const char* source, bool* output) const {
    if (!output || !source) return EINVAL;
    *output = false;
    ConfigSnapshot config;
    auto* registry =
        querier_ ? static_cast<IConfigChannelRegistryV1*>(querier_->First(IID_CONFIG_CHANNEL_REGISTRY_V1)) : nullptr;
    if (!ResolveConfig(with ? with : "", source, registry, &config).ok()) return EINVAL;
    *output = config.config.mode == Mode::kPoll;
    return 0;
}
}  // namespace flowsql::baseliner
