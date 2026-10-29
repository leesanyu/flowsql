// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "scheduler_plugin.h"

#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <common/error_code.h>
#include <common/log.h>
#include <framework/interfaces/icpp_operator_contract_provider.h>
#include <framework/interfaces/idataframe_metadata.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <common/operator_block_contract.hpp>
#include <common/operator_contract_execution.hpp>
#include <common/operator_contract_observation.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <regex>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_set>

#include <framework/interfaces/iblock_stream_acknowledgement.h>
#include <framework/interfaces/iblock_stream_schema.h>
#include <framework/interfaces/iblock_transform_dataframe_input.h>
#include <framework/interfaces/iblock_transform_execution_policy.h>
#include <framework/interfaces/iblock_transform_model_output.h>
#include <framework/interfaces/iblock_transform_result_output.h>
#include <framework/interfaces/iblock_transform_source_config.h>
#include <framework/interfaces/ikafka_connection_service.h>
#include "framework/core/channel_adapter.h"
#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/fan_in_stream_channel.h"
#include "framework/core/fan_out_stream_channel.h"
#include "framework/core/filter_binding.h"
#include "framework/core/filter_expression.h"
#include "framework/core/filter_planner.h"
#include "framework/core/json_error_builder.h"
#include "framework/core/packet_codec.h"
#include "framework/core/pipeline.h"
#include "framework/core/ring_stream_channel.h"
#include "framework/core/sql_parser.h"
#include "framework/core/sql_text_splitter.h"
#include "framework/interfaces/iblock_stream_channel.h"
#include "framework/interfaces/iblock_stream_channel_descriptor.h"
#include "framework/interfaces/iblock_stream_factory.h"
#include "framework/interfaces/ibridge.h"
#include "framework/interfaces/ibuiltin_registry.h"
#include "framework/interfaces/icapture_block_stream_reader.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ichannel_registry.h"
#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idatabase_factory.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/ifilter_domain_resolver.h"
#include "framework/interfaces/ioperator.h"
#include "framework/interfaces/ioperator_catalog.h"
#include "framework/interfaces/ioperator_registry.h"
#include "framework/interfaces/istream_channel.h"
#include "framework/interfaces/istream_factory.h"
#include "framework/interfaces/istream_manager.h"
#include "scheduler_internal_utils.h"
#include "scheduler_json_codec.h"

namespace flowsql {
namespace scheduler {

static std::shared_ptr<arrow::Schema> BlockSourceArrowSchema(IChannel* source) {
    auto* capability = dynamic_cast<IBlockStreamSchemaV1*>(source);
    return capability ? capability->ArrowSchema() : packet::PacketSchema();
}

static int AcknowledgeProcessedBlock(IBlockStreamChannel* source, const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto* capability = dynamic_cast<IBlockStreamAcknowledgementV1*>(source);
    if (!capability) return 0;
    try {
        return capability->AcknowledgeBlock(batch);
    } catch (...) {
        return EFAULT;
    }
}

static std::string EnsureExecutionErrorJson(const std::string& rsp, const std::string& fallback_error,
                                            ErrorCodeId fallback_code, ErrorStageId fallback_stage) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject()) {
        const std::string err = rsp.empty() ? fallback_error : rsp;
        return BuildExecutionErrorJson(err, fallback_code, fallback_stage);
    }

    std::string error_text = fallback_error;
    if (d.HasMember("error") && d["error"].IsString()) {
        error_text = d["error"].GetString();
    } else if (!rsp.empty()) {
        error_text = rsp;
    }
    if (error_text.empty()) error_text = fallback_error;

    bool changed = false;
    auto& alloc = d.GetAllocator();
    if (!(d.HasMember("error") && d["error"].IsString())) {
        d.RemoveMember("error");
        d.AddMember("error", rapidjson::Value(error_text.c_str(), alloc), alloc);
        changed = true;
    }
    if (!(d.HasMember("error_code") && d["error_code"].IsString())) {
        d.RemoveMember("error_code");
        d.AddMember("error_code", rapidjson::Value(ToErrorCode(fallback_code), alloc), alloc);
        changed = true;
    }
    if (!(d.HasMember("error_stage") && d["error_stage"].IsString())) {
        d.RemoveMember("error_stage");
        d.AddMember("error_stage", rapidjson::Value(ToErrorStage(fallback_stage), alloc), alloc);
        changed = true;
    }
    if (!changed) return rsp;

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    return buf.GetString();
}

static std::shared_ptr<IChannel> MakeNonOwningChannelHolder(IChannel* ch) {
    if (!ch) return nullptr;
    return std::shared_ptr<IChannel>(ch, [](IChannel*) {});
}

static std::shared_ptr<IStreamChannel> MakeStreamOwner(IStreamChannel* stream_ch,
                                                       const std::shared_ptr<IChannel>& owner) {
    if (!stream_ch) return nullptr;
    if (owner) {
        auto stream_owner = std::dynamic_pointer_cast<IStreamChannel>(owner);
        if (stream_owner) return stream_owner;
    }
    return std::shared_ptr<IStreamChannel>(stream_ch, [](IStreamChannel*) {});
}

static std::string MakeWithParamsJson(const std::unordered_map<std::string, std::string>& params) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    for (const auto& kv : params) {
        w.Key(kv.first.c_str());
        w.String(kv.second.c_str());
    }
    w.EndObject();
    return buf.GetString();
}

static std::string CanonicalSharedHubKey(const std::vector<std::string>& source_keys) {
    std::vector<std::string> keys;
    keys.reserve(source_keys.size());
    for (const auto& key : source_keys) {
        if (!key.empty()) keys.push_back(key);
    }
    if (keys.empty()) return "";
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::string out;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i != 0) out.push_back('\x1f');
        out += keys[i];
    }
    return out;
}

class SharedSourceState final : public std::enable_shared_from_this<SharedSourceState> {
 public:
    explicit SharedSourceState(std::shared_ptr<IStreamChannel> source) : source_(std::move(source)) {}

    PollEvent PollNext(int timeout_ms) {
        if (!source_) return PollEvent::Error(-EINVAL, "shared source unavailable");
        return source_->PollNext(timeout_ms);
    }

    void Cancel() {
        std::call_once(cancel_once_, [this]() {
            if (source_) source_->Cancel();
        });
    }

    int CloseView() {
        const int remain = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remain == 0 && source_) {
            return source_->Close();
        }
        return 0;
    }

    bool IsFinished() const { return source_ ? source_->IsFinished() : true; }

    bool IsFull() const { return source_ && source_->IsFull(); }

    bool IsEmpty() const { return !source_ || source_->IsEmpty(); }

    size_t Capacity() const { return source_ ? source_->Capacity() : 0; }

    size_t Size() const { return source_ ? source_->Size() : 0; }

    std::shared_ptr<arrow::Schema> GetOutputSchema() { return source_ ? source_->GetOutputSchema() : nullptr; }

    StreamChannelCapabilities Capabilities() const {
        return source_ ? source_->Capabilities() : StreamChannelCapabilities{};
    }

    const char* Category() const { return source_ ? source_->Category() : "stateless"; }
    const char* Name() const { return source_ ? source_->Name() : "stateless"; }
    const char* Schema() const { return source_ ? source_->Schema() : "[]"; }
    bool IsFinite() const { return source_ && source_->IsFinite(); }

 private:
    std::shared_ptr<IStreamChannel> source_;
    std::atomic<int> refs_{0};
    std::once_flag cancel_once_;

    friend class StatelessSourceView;
};

class StatelessSourceView final : public IStreamChannel {
 public:
    StatelessSourceView(std::shared_ptr<SharedSourceState> state, uint32_t view_id)
        : state_(std::move(state)),
          view_name_(state_ ? std::string(state_->Name()) + ".sv" + std::to_string(view_id)
                            : ("stateless.sv" + std::to_string(view_id))) {
        if (state_) {
            state_->refs_.fetch_add(1, std::memory_order_release);
        }
    }
    ~StatelessSourceView() override { (void)Close(); }

    const char* Category() override { return state_ ? state_->Category() : "stateless"; }

    const char* Name() override { return view_name_.c_str(); }

    const char* Type() override { return ChannelType::kStream; }

    const char* Schema() override { return state_ ? state_->Schema() : "[]"; }

    int Open() override { return 0; }
    int Close() override {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return 0;
        }
        return state_ ? state_->CloseView() : 0;
    }
    bool IsOpened() const override { return true; }
    int Flush() override { return 0; }

    int Put(std::shared_ptr<arrow::RecordBatch>, int64_t) override { return ENOTSUP; }

    PollEvent PollNext(int timeout_ms = 100) override {
        if (!state_) return PollEvent::Error(-EINVAL, "invalid shared spmc state");
        return state_->PollNext(timeout_ms);
    }

    std::shared_ptr<arrow::Schema> GetOutputSchema() override { return state_ ? state_->GetOutputSchema() : nullptr; }

    int SetFilter(const char*, std::vector<std::string>* unsupported_out) override {
        if (unsupported_out) unsupported_out->clear();
        return 0;
    }

    StreamChannelCapabilities Capabilities() const override {
        return state_ ? state_->Capabilities() : StreamChannelCapabilities{};
    }

    bool IsFull() const override { return state_ && state_->IsFull(); }
    bool IsEmpty() const override { return !state_ || state_->IsEmpty(); }
    size_t Capacity() const override { return state_ ? state_->Capacity() : 0; }
    size_t Size() const override { return state_ ? state_->Size() : 0; }
    bool IsFinite() const override { return state_ && state_->IsFinite(); }
    void CloseStream() override {}
    void Cancel() override {
        if (state_) state_->Cancel();
    }
    bool IsFinished() const override { return state_ && state_->IsFinished(); }

 private:
    std::shared_ptr<SharedSourceState> state_;
    std::string view_name_;
    std::atomic<bool> closed_{false};
};

class FanOutPartitionView final : public IStreamChannel {
 public:
    FanOutPartitionView(std::shared_ptr<FanOutStreamChannel> parent, std::shared_ptr<IStreamChannel> partition)
        : parent_(std::move(parent)), partition_(std::move(partition)) {}

    const char* Category() override { return partition_ ? partition_->Category() : "fanout"; }

    const char* Name() override { return partition_ ? partition_->Name() : "fanout.partition"; }

    const char* Type() override { return ChannelType::kStream; }

    const char* Schema() override { return partition_ ? partition_->Schema() : "[]"; }

    int Open() override { return 0; }
    int Close() override { return partition_ ? partition_->Close() : 0; }
    bool IsOpened() const override { return partition_ && partition_->IsOpened(); }
    int Flush() override { return partition_ ? partition_->Flush() : 0; }

    int Put(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) override {
        return partition_ ? partition_->Put(std::move(batch), ts_ms) : EINVAL;
    }

    PollEvent PollNext(int timeout_ms = 100) override {
        if (!partition_) return PollEvent::Error(-EINVAL, "fanout partition unavailable");
        return partition_->PollNext(timeout_ms);
    }

    std::shared_ptr<arrow::Schema> GetOutputSchema() override {
        return partition_ ? partition_->GetOutputSchema() : nullptr;
    }

    int SetFilter(const char* condition_json, std::vector<std::string>* unsupported_out) override {
        return partition_ ? partition_->SetFilter(condition_json, unsupported_out) : EINVAL;
    }

    bool IsFull() const override { return partition_ && partition_->IsFull(); }
    bool IsEmpty() const override { return !partition_ || partition_->IsEmpty(); }
    size_t Capacity() const override { return partition_ ? partition_->Capacity() : 0; }
    size_t Size() const override { return partition_ ? partition_->Size() : 0; }
    bool IsFinite() const override { return partition_ && partition_->IsFinite(); }
    void CloseStream() override {
        if (partition_) partition_->CloseStream();
    }
    void Cancel() override {
        if (partition_) partition_->Cancel();
        if (parent_) parent_->Cancel();
    }
    bool IsFinished() const override { return partition_ && partition_->IsFinished(); }

 private:
    std::shared_ptr<FanOutStreamChannel> parent_;
    std::shared_ptr<IStreamChannel> partition_;
};

// --- IPlugin ---

std::shared_ptr<IOperator> SchedulerPlugin::FindOperator(const std::string& category, const std::string& name) {
    if (!querier_) return nullptr;
    auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));

    // Factories and Bridge return invocation-owned configuration and component leases.
    if (auto owned = CreateOperator(category, name)) return owned;

    // 1. 先查 C++ 静态算子
    IOperator* found = nullptr;
    querier_->Traverse(IID_OPERATOR, [&](void* p) -> int {
        auto* op = static_cast<IOperator*>(p);
        if (IEquals(op->Category(), category) && op->Name() == name) {
            found = op;
            return -1;
        }
        return 0;
    });
    // C++ 算子由 PluginLoader 管理生命周期，用空 deleter 包装
    if (found) return std::shared_ptr<IOperator>(found, [](IOperator*) {});

    // 2. 再查 Python 算子（通过 IBridge）
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        auto py_op = bridge->FindOperator(category, name);
        if (py_op) return py_op;
    }

    // 3. 查注册表（兼容 "category.name" 与 legacy builtin name）
    if (op_registry) {
        const std::string key = category + "." + name;
        IOperator* op = op_registry->Create(key.c_str());
        if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    }
    if (IEquals(category, "builtin") && op_registry) {
        IOperator* op = op_registry->Create(name.c_str());
        if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    }
    return nullptr;
}

std::shared_ptr<IOperator> SchedulerPlugin::CreateOperator(const std::string& category, const std::string& name) {
    if (!querier_) return nullptr;
    auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));
    if (op_registry) {
        const std::string key = category + "." + name;
        if (IOperator* op = op_registry->Create(key.c_str())) {
            return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
        }
        if (IEquals(category, "builtin")) {
            if (IOperator* op = op_registry->Create(name.c_str())) {
                return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
            }
        }
    }

    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        return bridge->FindOperator(category, name);
    }
    return nullptr;
}

IBlockStreamOperator* SchedulerPlugin::FindBlockOperator(const std::string& category, const std::string& name) {
    if (!querier_) return nullptr;
    IBlockStreamOperator* found = nullptr;
    size_t matches = 0;
    querier_->Traverse(IID_BLOCK_STREAM_OPERATOR, [&](void* value) -> int {
        auto* candidate = static_cast<IBlockStreamOperator*>(value);
        if (!candidate || !IEquals(candidate->Category(), category) || candidate->Name() != name) return 0;
        found = candidate;
        ++matches;
        return 0;
    });
    return matches == 1 ? found : nullptr;
}

SchedulerPlugin::BlockTransformProviderRef SchedulerPlugin::FindBlockTransformOperator(
    const std::string& category, const std::string& name, CppOperatorCapabilityLeaseV1* dynamic_lease, bool* ambiguous,
    int* traverse_error) {
    if (dynamic_lease) *dynamic_lease = {};
    if (ambiguous) *ambiguous = false;
    if (traverse_error) *traverse_error = 0;
    if (!querier_ || !dynamic_lease) return {};
    OperatorComponentContractV1 selected_component;

    auto resolve_version = [&](const Guid& iid, bool is_v2, void** selected,
                               CppOperatorCapabilityLeaseV1* selected_lease, bool* version_ambiguous) {
        *selected = nullptr;
        *selected_lease = {};
        *version_ambiguous = false;
        selected_component = {};
        size_t matches = 0;
        int traversal_rc = querier_->Traverse(iid, [&](void* value) -> int {
            if (!value) return 0;
            const std::string candidate_category = is_v2 ? static_cast<IBlockTransformOperatorV2*>(value)->Category()
                                                         : static_cast<IBlockTransformOperatorV1*>(value)->Category();
            const std::string candidate_name = is_v2 ? static_cast<IBlockTransformOperatorV2*>(value)->Name()
                                                     : static_cast<IBlockTransformOperatorV1*>(value)->Name();
            if (!IEquals(candidate_category, category) || candidate_name != name) return 0;
            *selected = value;
            DataSemanticsErrorV1 ignored;
            auto* identity =
                is_v2 ? dynamic_cast<IOperatorContractIdentityV1*>(static_cast<IBlockTransformOperatorV2*>(value))
                      : dynamic_cast<IOperatorContractIdentityV1*>(static_cast<IBlockTransformOperatorV1*>(value));
            if (contracts::Component(querier_, identity, category, name, &selected_component, &ignored)) return EPROTO;
            ++matches;
            return 0;
        });
        if (traversal_rc != 0) return traversal_rc;

        CppOperatorCapabilityLeaseV1 acquired_dynamic_lease;
        bool invalid_dynamic_lease = false;
        traversal_rc = querier_->Traverse(IID_CPP_OPERATOR_PLUGIN_REGISTRY_V1, [&](void* value) -> int {
            auto* registry = static_cast<ICppOperatorPluginRegistryV1*>(value);
            if (!registry) return 0;

            CppOperatorCapabilityLeaseV1 candidate_lease;
            OperatorComponentContractV1 candidate_component;
            auto* contract_provider = dynamic_cast<ICppOperatorContractProviderV1*>(registry);
            const int acquire_rc = contract_provider
                                       ? contract_provider->AcquireContract(category.c_str(), name.c_str(), iid,
                                                                            &candidate_lease, &candidate_component)
                                       : registry->Acquire(category.c_str(), name.c_str(), iid, &candidate_lease);
            if (acquire_rc != 0) {
                return 0;
            }
            if (!candidate_lease.capability || !candidate_lease.lifetime) {
                invalid_dynamic_lease = true;
                return 0;
            }

            *selected = candidate_lease.capability;
            if (!contract_provider) {
                DataSemanticsErrorV1 ignored;
                if (contracts::Component(querier_, nullptr, category, name, &candidate_component, &ignored))
                    return EPROTO;
            }
            selected_component = std::move(candidate_component);
            ++matches;
            if (matches == 1) {
                acquired_dynamic_lease = std::move(candidate_lease);
            }
            return 0;
        });
        if (traversal_rc != 0) return traversal_rc;
        if (invalid_dynamic_lease) return EPROTO;
        *version_ambiguous = matches > 1;
        if (matches != 1) {
            *selected = nullptr;
            return 0;
        }
        *selected_lease = std::move(acquired_dynamic_lease);
        return 0;
    };

    void* selected = nullptr;
    CppOperatorCapabilityLeaseV1 selected_lease;
    bool version_ambiguous = false;
    int traversal_rc =
        resolve_version(IID_BLOCK_TRANSFORM_OPERATOR_V2, true, &selected, &selected_lease, &version_ambiguous);
    if (traverse_error) *traverse_error = traversal_rc;
    if (traversal_rc != 0) return {};
    if (version_ambiguous) {
        if (ambiguous) *ambiguous = true;
        return {};
    }
    if (selected) {
        *dynamic_lease = std::move(selected_lease);
        return {nullptr, static_cast<IBlockTransformOperatorV2*>(selected), selected_component};
    }

    traversal_rc =
        resolve_version(IID_BLOCK_TRANSFORM_OPERATOR_V1, false, &selected, &selected_lease, &version_ambiguous);
    if (traverse_error) *traverse_error = traversal_rc;
    if (traversal_rc != 0) return {};
    if (version_ambiguous) {
        if (ambiguous) *ambiguous = true;
        return {};
    }
    if (!selected) return {};
    *dynamic_lease = std::move(selected_lease);
    return {static_cast<IBlockTransformOperatorV1*>(selected), nullptr, selected_component};
}

int SchedulerPlugin::ExecuteBlockOperator(IBlockStreamChannel* source, IBlockStreamOperator* op,
                                          const std::shared_ptr<const BoundFilterExpr>& source_residual,
                                          BlockExecutionTerminal* terminal, int64_t* rows_affected,
                                          std::string* error) {
    if (terminal) *terminal = BlockExecutionTerminal::kFailed;
    if (!source || !op) return EINVAL;
    BlockFilterStage source_filter(source_residual);
    if (source_residual) {
        std::shared_ptr<arrow::Schema> filtered_schema;
        std::string filter_error;
        const auto open_rc = source_filter.Open(BlockSourceArrowSchema(source), &filtered_schema, &filter_error);
        if (open_rc != FilterEvalError::kNone) {
            if (error) *error = "source-stage residual Open failed: " + filter_error;
            return EINVAL;
        }
    }
    std::shared_ptr<arrow::Schema> schema;
    int64_t rows = 0;
    bool schema_ready = false;
    bool stopped = false;
    const auto control = SchedulerBatchRuntime::CurrentControl();
    while (true) {
        if (control && control->cancel_requested.load()) {
            source->Cancel();
            if (rows_affected) *rows_affected = rows;
            if (terminal) *terminal = BlockExecutionTerminal::kCancelled;
            return ECANCELED;
        }
        if (control && control->stop_requested.load()) {
            stopped = true;
            break;
        }
        const BlockPollEvent event = source->PollBlock(100);
        if (event.kind == BlockPollEvent::kData) {
            if (!event.batch) {
                if (error) *error = "block operator received empty data batch";
                return EINVAL;
            }
            int64_t batch_rows = event.batch->num_rows();
            int rc = 0;
            bool schema_failed = false;
            bool acknowledgement_failed = false;
            try {
                if (!schema_ready) {
                    schema = event.batch->schema();
                    if (!schema || !schema->Equals(BlockSourceArrowSchema(source))) {
                        rc = EINVAL;
                        schema_failed = true;
                        if (error) *error = "block source schema does not match declared schema";
                    } else {
                        rc = op->OnSchemaReady(schema);
                        schema_ready = rc == 0;
                        schema_failed = !schema_ready;
                    }
                }
                std::shared_ptr<arrow::RecordBatch> output_batch = event.batch;
                if (rc == 0 && source_residual) {
                    BlockTransformOutputV1 filtered;
                    std::string filter_error;
                    const auto eval_rc = source_filter.ProcessBlock(event.batch, 0, &filtered, &filter_error);
                    if (eval_rc != FilterEvalError::kNone || !filtered.batch) {
                        if (error) *error = "source-stage residual failed: " + filter_error;
                        rc = EINVAL;
                    } else {
                        output_batch = std::move(filtered.batch);
                        batch_rows = output_batch->num_rows();
                    }
                }
                if (rc == 0 && (!source_residual || batch_rows > 0)) {
                    rc = op->ProcessBlock(output_batch, 0);
                }
            } catch (const std::exception& ex) {
                rc = EFAULT;
                schema_failed = !schema_ready;
                if (error) *error = ex.what();
            } catch (...) {
                rc = EFAULT;
                schema_failed = !schema_ready;
                if (error) *error = "block operator threw an unknown exception";
            }
            if (!schema_failed && (rc == 0 || rc == 1)) {
                const int ack_rc = AcknowledgeProcessedBlock(source, event.batch);
                if (ack_rc) {
                    rc = ack_rc;
                    acknowledgement_failed = true;
                    if (error) *error = "block source acknowledgement failed";
                }
            }
            int release_rc = 0;
            try {
                release_rc = source->ReleaseBlock(event.batch);
            } catch (...) {
                release_rc = EFAULT;
            }
            if (release_rc != 0) {
                if (error) *error = "block source ReleaseBlock failed";
                return release_rc;
            }
            if (schema_failed || acknowledgement_failed) {
                if (error && error->empty()) *error = op->LastError();
                return rc;
            }
            if (rc == 1) {
                rows += batch_rows;
                stopped = true;
                break;
            }
            if (rc != 0) {
                if (error && error->empty()) *error = op->LastError();
                return rc;
            }
            rows += batch_rows;
            continue;
        }
        if (event.kind == BlockPollEvent::kTimeout) continue;
        if (event.kind == BlockPollEvent::kEof) break;
        if (event.kind == BlockPollEvent::kCancelled) {
            if (error) *error = "block source cancelled";
            if (rows_affected) *rows_affected = rows;
            if (terminal) *terminal = BlockExecutionTerminal::kCancelled;
            return event.err != 0 ? event.err : ECANCELED;
        }
        if (error) *error = "block source poll failed";
        return event.err != 0 ? event.err : EIO;
    }
    int flush_rc = 0;
    try {
        flush_rc = op->Flush();
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return EFAULT;
    } catch (...) {
        if (error) *error = "block operator flush threw an unknown exception";
        return EFAULT;
    }
    if (flush_rc != 0) {
        if (error && error->empty()) *error = op->LastError();
        return flush_rc;
    }
    if (rows_affected) *rows_affected = rows;
    if (terminal) {
        *terminal = stopped ? BlockExecutionTerminal::kStopped : BlockExecutionTerminal::kCompleted;
    }
    return 0;
}

namespace {

class SchemaCheckingBlockTransformTask final : public IBlockTransformTaskV1 {
 public:
    SchemaCheckingBlockTransformTask(IBlockTransformTaskV1* task, std::shared_ptr<arrow::Schema> expected_output_schema)
        : task_(task), expected_output_schema_(std::move(expected_output_schema)) {}

    int Open(std::shared_ptr<arrow::Schema> input_schema, std::shared_ptr<arrow::Schema>* output_schema) override {
        if (!task_) return EINVAL;
        const int rc = task_->Open(std::move(input_schema), output_schema);
        if (rc != 0) return rc;
        if (!output_schema || !*output_schema || !expected_output_schema_ ||
            !(*output_schema)->Equals(*expected_output_schema_, true)) {
            last_error_ = "transform execution Schema differs from its planning Schema";
            return EPROTO;
        }
        return 0;
    }

    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t ts_ms,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        return task_->ProcessBlock(input, ts_ms, outputs);
    }

    int Flush(std::vector<BlockTransformOutputV1>* outputs) override { return task_->Flush(outputs); }

    void Cancel() override { task_->Cancel(); }

    std::string LastError() const override { return last_error_.empty() ? task_->LastError() : last_error_; }

 private:
    IBlockTransformTaskV1* task_ = nullptr;
    std::shared_ptr<arrow::Schema> expected_output_schema_;
    std::string last_error_;
};

const std::string* FindStageFilterText(const SqlStatement& stmt, uint32_t after_stage, bool* duplicate) {
    if (duplicate) *duplicate = false;
    const std::string* found = nullptr;
    for (const auto& filter : stmt.stage_filters) {
        if (filter.after_stage != after_stage) continue;
        if (found) {
            if (duplicate) *duplicate = true;
            return nullptr;
        }
        found = &filter.filter_text;
    }
    return found;
}

bool ParseStageFilter(const std::string* filter_text, uint32_t after_stage, std::shared_ptr<FilterExpr>* expression,
                      std::string* error) {
    if (!expression) return false;
    expression->reset();
    if (!filter_text) return true;

    std::string parse_error;
    if (ParseFilterExpression(*filter_text, expression, &parse_error)) return true;
    if (error) {
        const char* stage_name = after_stage == 0 ? "source-stage" : "operator-stage";
        *error = std::string(stage_name) + " filter syntax failed: " + parse_error;
    }
    return false;
}

int ResolveFilterDomain(IQuerier* querier, FilterDomainTargetKindV1 target_kind, const std::string& target_category,
                        const std::string& target_name, const std::shared_ptr<arrow::Schema>& output_schema,
                        const std::shared_ptr<FilterExpr>& expression, std::shared_ptr<FilterExpr>* resolved_expression,
                        std::string* error) {
    if (resolved_expression) resolved_expression->reset();
    if (error) error->clear();
    if (!resolved_expression || !output_schema || !expression || target_category.empty() || target_name.empty()) {
        if (error) *error = "filter domain resolver request is incomplete";
        return EIO;
    }
    if (!querier) {
        *resolved_expression = expression;
        return 0;
    }

    FilterDomainResolveRequestV1 request;
    request.target_kind = target_kind;
    request.target_category = target_category.c_str();
    request.target_name = target_name.c_str();
    request.output_schema = output_schema;
    request.expression = expression;

    size_t owner_count = 0;
    int resolution_error = 0;
    std::string resolution_diagnostic;
    const int traversal_rc = querier->Traverse(IID_FILTER_DOMAIN_RESOLVER_V1, [&](void* value) -> int {
        auto* resolver = static_cast<IFilterDomainResolverV1*>(value);
        if (!resolver) {
            resolution_error = EIO;
            resolution_diagnostic = "filter domain resolver discovery returned a null provider";
            return -1;
        }

        FilterDomainResolveResultV1 result;
        int rc = 0;
        try {
            rc = resolver->Resolve(request, &result);
        } catch (const std::exception& ex) {
            resolution_error = EIO;
            resolution_diagnostic = "filter domain resolver threw: " + std::string(ex.what());
            return -1;
        } catch (...) {
            resolution_error = EIO;
            resolution_diagnostic = "filter domain resolver threw an unknown exception";
            return -1;
        }
        if (rc == ENOTSUP) {
            if (result.lowered_expression) {
                resolution_error = EIO;
                resolution_diagnostic = "non-owner filter domain resolver returned an expression";
                return -1;
            }
            return 0;
        }
        if (rc != 0) {
            resolution_error = rc == ENOMEM || rc == EFAULT ? EIO : EINVAL;
            resolution_diagnostic =
                result.diagnostic.empty()
                    ? "filter domain resolver rejected the expression with code " + std::to_string(rc)
                    : std::move(result.diagnostic);
            return -1;
        }
        if (!result.lowered_expression) {
            resolution_error = EIO;
            resolution_diagnostic = "owning filter domain resolver returned no expression";
            return -1;
        }
        if (owner_count != 0) {
            resolution_error = EIO;
            resolution_diagnostic = "multiple filter domain resolvers accepted the current stage";
            return -1;
        }
        ++owner_count;
        *resolved_expression = std::move(result.lowered_expression);
        return 0;
    });
    if (resolution_error != 0) {
        resolved_expression->reset();
        if (error) *error = std::move(resolution_diagnostic);
        return resolution_error;
    }
    if (traversal_rc != 0) {
        resolved_expression->reset();
        if (error) {
            *error = "filter domain resolver traversal failed with code " + std::to_string(traversal_rc);
        }
        return EIO;
    }
    if (owner_count == 0) *resolved_expression = expression;
    return 0;
}

int ResolveAndBindStageFilter(IQuerier* querier, FilterDomainTargetKindV1 target_kind,
                              const std::string& target_category, const std::string& target_name,
                              const std::shared_ptr<arrow::Schema>& output_schema,
                              const std::shared_ptr<FilterExpr>& expression, const std::string& stage_label,
                              std::shared_ptr<const BoundFilterExpr>* bound_expression, std::string* error) {
    if (bound_expression) bound_expression->reset();
    if (!expression) return 0;

    std::shared_ptr<FilterExpr> resolved_expression;
    std::string detail;
    const int resolve_rc = ResolveFilterDomain(querier, target_kind, target_category, target_name, output_schema,
                                               expression, &resolved_expression, &detail);
    if (resolve_rc != 0) {
        if (error) *error = stage_label + " filter domain resolution failed: " + detail;
        return resolve_rc;
    }

    const auto bind_rc = BindFilterExpression(output_schema, resolved_expression, bound_expression, &detail);
    if (bind_rc != FilterBindError::kNone) {
        if (error) *error = stage_label + " filter binding failed: " + detail;
        return EINVAL;
    }
    return 0;
}

std::string SafeBlockTransformLastError(IBlockTransformTaskV1* task) {
    if (!task) return "";
    try {
        return task->LastError();
    } catch (...) {
        return "";
    }
}

}  // namespace

int SchedulerPlugin::BuildBlockSourceFilterPlan(IBlockStreamChannel* source, const SqlStatement& stmt,
                                                BlockSourceFilterPlan* plan, std::string* error) {
    if (plan) *plan = BlockSourceFilterPlan{};
    if (error) error->clear();
    if (!source || !plan || !error) return EINVAL;
    plan->pushed_filter_plan_json = kEmptyCanonicalFilterPlanV1;

    bool duplicate_source_filter = false;
    const auto* source_filter_text = FindStageFilterText(stmt, 0, &duplicate_source_filter);
    if (duplicate_source_filter) {
        *error = "block source contains duplicate filters";
        return EINVAL;
    }
    std::shared_ptr<FilterExpr> source_expression;
    if (!ParseStageFilter(source_filter_text, 0, &source_expression, error)) {
        return EINVAL;
    }
    if (!source_expression) return 0;

    const auto source_schema = BlockSourceArrowSchema(source);
    std::shared_ptr<const BoundFilterExpr> bound_source_filter;
    const int filter_rc =
        ResolveAndBindStageFilter(querier_, FilterDomainTargetKindV1::kSource,
                                  source->Category() ? source->Category() : "", source->Name() ? source->Name() : "",
                                  source_schema, source_expression, "source-stage", &bound_source_filter, error);
    if (filter_rc != 0) return filter_rc;

    FilterPushdownTarget target;
    target.kind = FilterPushdownTargetKindV1::kSource;
    target.category = source->Category() ? source->Category() : "";
    target.name = source->Name() ? source->Name() : "";
    target.output_schema = source_schema;
    FilterPushdownNegotiation negotiation;
    std::string plan_error;
    const auto negotiate_rc = NegotiateFilterPushdown(querier_, target, bound_source_filter, &negotiation, &plan_error);
    if (negotiate_rc != FilterPlanError::kNone) {
        *error = "source-stage filter pushdown negotiation failed: " + plan_error;
        return negotiate_rc == FilterPlanError::kInvalidArgument ||
                       negotiate_rc == FilterPlanError::kInvalidBoundExpression ||
                       negotiate_rc == FilterPlanError::kCanonicalPlanError
                   ? EINVAL
                   : EIO;
    }

    FilterTaskSessionPlan exclusive_plan;
    const auto exclusive_rc = MaterializeFilterTaskSessionPlan(
        source_schema, bound_source_filter, negotiation, FilterTaskIsolation::kExclusive, &exclusive_plan, &plan_error);
    if (exclusive_rc != FilterPlanError::kNone) {
        *error = "source-stage exclusive filter task planning failed: " + plan_error;
        return exclusive_rc == FilterPlanError::kInvalidArgument ||
                       exclusive_rc == FilterPlanError::kInvalidBoundExpression ||
                       exclusive_rc == FilterPlanError::kCanonicalPlanError
                   ? EINVAL
                   : EIO;
    }
    FilterTaskSessionPlan shared_plan;
    const auto shared_rc = MaterializeFilterTaskSessionPlan(
        source_schema, bound_source_filter, negotiation, FilterTaskIsolation::kSharedSource, &shared_plan, &plan_error);
    if (shared_rc != FilterPlanError::kNone) {
        *error = "source-stage shared filter task planning failed: " + plan_error;
        return shared_rc == FilterPlanError::kInvalidArgument ||
                       shared_rc == FilterPlanError::kInvalidBoundExpression ||
                       shared_rc == FilterPlanError::kCanonicalPlanError
                   ? EINVAL
                   : EIO;
    }

    plan->pushed_filter_plan_json = std::move(exclusive_plan.pushed_filter_plan_json);
    plan->exclusive_residual = std::move(exclusive_plan.residual_expression);
    plan->shared_residual = std::move(shared_plan.residual_expression);
    return 0;
}

static int BindBlockTransformInputSource(IBlockTransformTaskV1* task, const std::string& source, std::string* error) {
    auto* input_task = dynamic_cast<IBlockTransformInputSourceTaskV1*>(task);
    if (!input_task) return 0;
    try {
        const int rc = input_task->BindInputSource(source.c_str());
        if (rc != 0 && error) *error = "block transform input source binding failed";
        return rc;
    } catch (const std::exception& ex) {
        if (error) *error = std::string("block transform input source binding threw: ") + ex.what();
        return EFAULT;
    } catch (...) {
        if (error) *error = "block transform input source binding threw an unknown exception";
        return EFAULT;
    }
}

int SchedulerPlugin::ExecuteSingleBlockTransformPipeline(
    IBlockStreamChannel* source, BlockTransformProviderRef provider, IDataFrameChannel* sink,
    const BlockTransformManagedSinkBindingV1* managed_sink, const SqlStatement& stmt,
    const std::shared_ptr<const BoundFilterExpr>& source_residual, BlockExecutionTerminal* terminal,
    int64_t* rows_affected, std::string* managed_result_json, std::string* error,
    std::shared_ptr<IDatabaseChannel> database_source, std::shared_ptr<IDataFrameChannel> dataframe_source,
    IResultSinkWriterV1* result_sink, std::string* contract_checks) {
    if (terminal) *terminal = BlockExecutionTerminal::kFailed;
    if (rows_affected) *rows_affected = 0;
    if (managed_result_json) managed_result_json->clear();
    if (error) error->clear();
    if (contract_checks) *contract_checks = "[]";
    if ((!source && !database_source && !dataframe_source) || !provider || (provider.v1 && provider.v2) ||
        (int(sink != nullptr) + int(managed_sink != nullptr) + int(result_sink != nullptr) != 1) ||
        stmt.operators.size() != 1) {
        if (error) *error = "block transform pipeline requires one source, provider, operator, and sink";
        return EINVAL;
    }
    auto* appendable_sink = dynamic_cast<IAppendableDataFrameChannel*>(sink);
    if (sink && !appendable_sink) {
        if (error) *error = "block transform pipeline requires an appendable DataFrame sink";
        return EINVAL;
    }

    bool duplicate_transform_filter = false;
    const auto* transform_filter_text = FindStageFilterText(stmt, 1, &duplicate_transform_filter);
    if (duplicate_transform_filter) {
        if (error) *error = "block transform stage contains duplicate filters";
        return EINVAL;
    }
    if (managed_sink && (!stmt.columns.empty() || transform_filter_text != nullptr)) {
        const auto& parameters = stmt.operator_with_params.empty() ? stmt.with_params : stmt.operator_with_params[0];
        const auto observing = parameters.find("observing");
        if (stmt.operators.front().category != "npm" || stmt.operators.front().name != "basic" ||
            observing == parameters.end() || observing->second.empty()) {
            if (error) *error = "managed sink without observing requires SELECT * without operator-stage WHERE";
            return EINVAL;
        }
    }
    for (const auto& filter : stmt.stage_filters) {
        if (filter.after_stage > 1) {
            if (error) *error = "block transform pipeline received an unsupported filter stage";
            return EINVAL;
        }
    }

    std::shared_ptr<FilterExpr> transform_expression;
    if (!ParseStageFilter(transform_filter_text, 1, &transform_expression, error)) {
        return EINVAL;
    }

    if ((database_source || dataframe_source) &&
        (!provider.v2 || !stmt.columns.empty() || !stmt.stage_filters.empty())) {
        if (error) *error = "database block input requires one V2 operator, SELECT *, and no WHERE";
        return EINVAL;
    }
    auto source_schema =
        (database_source || dataframe_source) ? std::shared_ptr<arrow::Schema>{} : BlockSourceArrowSchema(source);

    const auto& params = !stmt.operator_with_params.empty() ? stmt.operator_with_params.front() : stmt.with_params;
    auto contract_state = std::make_shared<contracts::BlockInvocation>();
    contract_state->querier = querier_;
    contract_state->invocation.component = provider.component;
    auto& component = contract_state->invocation.component;
    if (component.category.empty()) component.category = stmt.operators.front().category;
    if (component.name.empty()) component.name = stmt.operators.front().name;
    contract_state->parameters.Parse(MakeWithParamsJson(params).c_str());
    dataset::Set(contract_state->context, "source", stmt.source, contract_state->context.GetAllocator());
    dataset::Set(contract_state->context, "managed_output",
                 dataset::Value(bool(managed_sink && (!managed_sink->relation || !managed_sink->relation[0]))),
                 contract_state->context.GetAllocator());
    auto* provider_adapter = provider.v2 ? dynamic_cast<IOperatorConfigAdapterV1*>(provider.v2)
                                         : dynamic_cast<IOperatorConfigAdapterV1*>(provider.v1);
    auto publish_check = [&] {
        if (!contract_checks || !contract_state->invocation.check.IsObject()) return;
        dataset::Json checks(rapidjson::kArrayType);
        checks.PushBack(dataset::Copy(contract_state->invocation.check, checks.GetAllocator()), checks.GetAllocator());
        *contract_checks = dataset::Encode(checks);
    };
    if (dataframe_source && component.contract_state == "declared") {
        using namespace dataset;
        Json locator(rapidjson::kObjectType), before, after;
        auto& a = locator.GetAllocator();
        Set(locator, "kind", "dataframe", a);
        Set(locator, "name", dataframe_source->Name(), a);
        std::string binding;
        if (contracts::DescribeInput(querier_, locator, {}, &before, &binding, &contract_state->error)) {
            if (error) *error = contract_state->ErrorText();
            return EINVAL;
        }
        DataFrame frame;
        if (dataframe_source->Read(&frame) != 0 || !frame.ToArrow()) {
            if (error) *error = "SOURCE_UNAVAILABLE: cannot capture block DataFrame input";
            return EIO;
        }
        if (contracts::DescribeInput(querier_, locator, frame.ToArrow()->schema(), &after, &binding,
                                     &contract_state->error) ||
            (Get(before, "schema_state") == "known" &&
             (before["object_key"] != after["object_key"] || before["source_revision"] != after["source_revision"])) ||
            FindChannel(stmt.source) != dataframe_source.get()) {
            if (error) *error = "SOURCE_CHANGED: block DataFrame input changed during capture";
            return EINVAL;
        }
        auto frozen = std::make_shared<DataFrameChannel>(dataframe_source->Category(), dataframe_source->Name());
        if (frozen->Open() || frozen->Write(&frame)) {
            if (error) *error = "cannot freeze block input";
            return EIO;
        }
        dataframe_source = std::move(frozen);
        auto& ia = contract_state->inputs.GetAllocator();
        Value input(rapidjson::kObjectType);
        Set(input, "port_id", "input", ia);
        input.AddMember("input_index", 0, ia).AddMember("snapshot", Copy(after, ia), ia);
        if (!binding.empty()) Set(input, "binding_ref", binding, ia);
        contract_state->inputs.PushBack(input, ia);
        contract_state->context.AddMember("source_row_count", static_cast<uint64_t>(frame.RowCount()),
                                          contract_state->context.GetAllocator());
    }
    if (component.contract_state == "declared" && provider_adapter &&
        contract_state->Check(nullptr, provider_adapter, source_schema)) {
        publish_check();
        if (error) *error = contract_state->ErrorText();
        return EINVAL;
    }
    std::string with_params_json = MakeWithParamsJson(params);
    auto* source_config = provider.v2 ? dynamic_cast<IBlockTransformSourceConfigProviderV1*>(provider.v2) : nullptr;
    if (source_config) {
        std::string normalized;
        const int rc =
            source_config->NormalizeSourceConfig(with_params_json.c_str(), stmt.source.c_str(), &normalized, error);
        if (rc != 0) return rc;
        with_params_json = std::move(normalized);
    }
    auto* capture_reader = dynamic_cast<ICaptureBlockStreamReaderV2*>(source);
    if (capture_reader && !stmt.operators.empty() && stmt.operators.front().category == "npm" &&
        stmt.operators.front().name == "basic" && !managed_sink) {
        if (error)
            *error =
                "continuous npm.basic requires INTO <database>.<name> managed target; DataFrame/unbounded foreground "
                "output is unsupported";
        return EINVAL;
    }
    CaptureSourceSetV2 capture_sources;
    if (capture_reader) {
        int describe_rc = 0;
        try {
            describe_rc = capture_reader->DescribeSources(&capture_sources);
        } catch (...) {
            if (error) *error = "capture reader Describe threw";
            return EFAULT;
        }
        if (describe_rc != 0 || ValidateCaptureSourceSetV2(capture_sources) != CaptureDescriptionErrorV1::kNone) {
            if (error) *error = "capture reader returned an invalid identity or limits";
            return describe_rc != 0 ? describe_rc : EINVAL;
        }
    }
    const std::string task_id = NextStreamTaskId();
    FilterTaskSessionPlan transform_filter_plan;
    transform_filter_plan.pushed_filter_plan_json = kEmptyCanonicalFilterPlanV1;
    std::shared_ptr<arrow::Schema> planned_output_schema;

    auto make_task_holder = [provider](IBlockTransformTaskV1* task) {
        return std::unique_ptr<IBlockTransformTaskV1, std::function<void(IBlockTransformTaskV1*)>>(
            task, [provider](IBlockTransformTaskV1* owned) {
                if (!owned) return;
                try {
                    if (provider.v2) {
                        provider.v2->ReleaseTask(static_cast<IBlockTransformTaskV2*>(owned));
                    } else {
                        provider.v1->ReleaseTask(owned);
                    }
                } catch (...) {
                }
            });
    };
    auto create_task = [&](const std::string& session_task_id, const FilterTaskSessionPlan& filter_plan,
                           IBlockTransformTaskV1** data_task, IBlockTransformTimeDrivenTaskV1** time_task,
                           std::string* plan_error) {
        if (data_task) *data_task = nullptr;
        if (time_task) *time_task = nullptr;
        if (provider.v1) {
            return CreateBlockTransformTaskSession(provider.v1, session_task_id, with_params_json, filter_plan,
                                                   data_task, plan_error);
        }
        if (!provider.v2 || !data_task || !time_task || session_task_id.empty() || with_params_json.empty() ||
            filter_plan.pushed_filter_plan_json.empty()) {
            if (plan_error) *plan_error = "invalid V2 block transform task request";
            return FilterPlanError::kInvalidArgument;
        }

        BlockTransformTaskConfigV2 config{};
        config.struct_size = kBlockTransformTaskConfigV2Size;
        config.contract_version = kBlockTransformContractVersionV2;
        config.task_id = session_task_id.c_str();
        config.with_params_json = with_params_json.c_str();
        config.pushed_filter_plan_json = filter_plan.pushed_filter_plan_json.c_str();
        IBlockTransformTaskV2* created_task = nullptr;
        const int create_rc = provider.v2->CreateTask(config, &created_task);
        if (create_rc != 0 || !created_task) {
            if (created_task) provider.v2->ReleaseTask(created_task);
            if (plan_error) {
                *plan_error = "V2 block transform task creation failed";
                if (create_rc != 0) {
                    *plan_error += " with code " + std::to_string(create_rc);
                } else {
                    *plan_error += ": provider returned a null task";
                }
            }
            return FilterPlanError::kTaskSessionCreationFailed;
        }
        *data_task = created_task;
        *time_task = created_task;
        return FilterPlanError::kNone;
    };

    if (transform_expression) {
        IBlockTransformTaskV1* probe_raw = nullptr;
        IBlockTransformTimeDrivenTaskV1* probe_time = nullptr;
        std::string plan_error;
        const auto create_probe =
            create_task(task_id + ".schema", transform_filter_plan, &probe_raw, &probe_time, &plan_error);
        if (create_probe != FilterPlanError::kNone) {
            if (error) *error = "transform Schema probe creation failed: " + plan_error;
            return EIO;
        }
        auto probe = make_task_holder(probe_raw);
        if (contract_state->Check(probe.get(), provider_adapter, source_schema)) {
            publish_check();
            if (error) *error = contract_state->ErrorText();
            return EINVAL;
        }
        const int input_bind_rc = BindBlockTransformInputSource(probe.get(), stmt.source, error);
        if (input_bind_rc != 0) return input_bind_rc;

        if (capture_reader) {
            auto* capture_task = dynamic_cast<IBlockTransformCaptureFactTaskV2*>(probe.get());
            if (capture_task && capture_task->BindCaptureSources(capture_sources) != 0) {
                if (error) *error = "capture Schema probe source binding failed";
                return EINVAL;
            }
        }
        int probe_open_rc = 0;
        try {
            probe_open_rc = probe->Open(source_schema, &planned_output_schema);
        } catch (const std::exception& ex) {
            if (error) *error = std::string("transform Schema probe Open threw: ") + ex.what();
            try {
                probe->Cancel();
            } catch (...) {
            }
            return EFAULT;
        } catch (...) {
            if (error) *error = "transform Schema probe Open threw an unknown exception";
            try {
                probe->Cancel();
            } catch (...) {
            }
            return EFAULT;
        }
        if (probe_open_rc != 0 || !planned_output_schema) {
            if (error) {
                *error = "transform Schema probe Open failed";
                const std::string detail = SafeBlockTransformLastError(probe.get());
                if (!detail.empty()) *error += ": " + detail;
            }
            try {
                probe->Cancel();
            } catch (...) {
            }
            return probe_open_rc != 0 ? probe_open_rc : EINVAL;
        }
        try {
            probe->Cancel();
        } catch (...) {
            if (error) *error = "transform Schema probe Cancel failed";
            return EFAULT;
        }
        probe.reset();

        std::shared_ptr<const BoundFilterExpr> bound_transform_filter;
        const int filter_rc =
            ResolveAndBindStageFilter(querier_, FilterDomainTargetKindV1::kTransform, stmt.operators.front().category,
                                      stmt.operators.front().name, planned_output_schema, transform_expression,
                                      "operator-stage", &bound_transform_filter, &plan_error);
        if (filter_rc != 0) {
            if (error) *error = plan_error;
            return filter_rc;
        }

        FilterPushdownTarget target;
        target.kind = FilterPushdownTargetKindV1::kTransform;
        target.category = stmt.operators.front().category;
        target.name = stmt.operators.front().name;
        target.output_schema = planned_output_schema;
        FilterPushdownNegotiation negotiation;
        const auto negotiate_rc =
            NegotiateFilterPushdown(querier_, target, bound_transform_filter, &negotiation, &plan_error);
        if (negotiate_rc != FilterPlanError::kNone) {
            if (error) *error = "operator-stage filter pushdown negotiation failed: " + plan_error;
            return negotiate_rc == FilterPlanError::kInvalidArgument ||
                           negotiate_rc == FilterPlanError::kInvalidBoundExpression ||
                           negotiate_rc == FilterPlanError::kCanonicalPlanError
                       ? EINVAL
                       : EIO;
        }
        const auto materialize_rc =
            MaterializeFilterTaskSessionPlan(planned_output_schema, bound_transform_filter, negotiation,
                                             FilterTaskIsolation::kExclusive, &transform_filter_plan, &plan_error);
        if (materialize_rc != FilterPlanError::kNone) {
            if (error) *error = "operator-stage filter task planning failed: " + plan_error;
            return EIO;
        }
    }

    IBlockTransformTaskV1* execution_raw = nullptr;
    IBlockTransformTimeDrivenTaskV1* execution_time = nullptr;
    std::string plan_error;
    const auto create_execution =
        create_task(task_id + ".run", transform_filter_plan, &execution_raw, &execution_time, &plan_error);
    if (create_execution != FilterPlanError::kNone) {
        if (error) *error = "transform execution task creation failed: " + plan_error;
        return EIO;
    }
    auto execution = make_task_holder(execution_raw);
    if (contract_state->Check(execution.get(), provider_adapter, source_schema)) {
        publish_check();
        if (error) *error = contract_state->ErrorText();
        return EINVAL;
    }
    publish_check();
    BlockDatabaseInputV1 database_input;
    BlockDataFrameInputV1 dataframe_input;
    std::unique_ptr<void, std::function<void(void*)>> input_guard(nullptr, [](void*) {});
    // Cover target binding and input initialization, before the runner owns the wake lease.
    const auto creation_control = SchedulerBatchRuntime::CurrentControl();
    struct CreationWakeLease {
        std::shared_ptr<BlockTransformRunControl> control;
        ~CreationWakeLease() {
            if (control) control->BindWake({});
        }
    } creation_wake{creation_control};
    if (creation_control)
        creation_control->BindWake([creation_control, task = execution.get()] {
            if (creation_control->cancel_requested.load()) task->Cancel();
        });
    const int input_bind_rc = BindBlockTransformInputSource(execution.get(), stmt.source, error);
    if (input_bind_rc != 0) return input_bind_rc;
    if (capture_reader) {
        auto* capture_task = dynamic_cast<IBlockTransformCaptureFactTaskV2*>(execution.get());
        if (capture_task && capture_task->BindCaptureSources(capture_sources) != 0) {
            if (error) *error = "capture source binding failed";
            return EINVAL;
        }
    }
    if (result_sink && (dynamic_cast<IBlockTransformManagedSinkTaskV1*>(execution.get()) ||
                        dynamic_cast<IBlockTransformModelOutputTaskV1*>(execution.get()) ||
                        dynamic_cast<IBlockTransformResultOutputTaskV1*>(execution.get()))) {
        if (error) *error = "SINK_OPERATOR_OUTPUT_UNSUPPORTED";
        return ENOTSUP;
    }
    auto* managed_task = dynamic_cast<IBlockTransformManagedSinkTaskV1*>(execution.get());
    auto* model_task = dynamic_cast<IBlockTransformModelOutputTaskV1*>(execution.get());
    const auto model_target = model_task ? model_task->ModelOutputTarget() : std::string{};
    std::shared_ptr<DataFrameChannel> model_dataframe;
    IChannelRegistry* model_registry = nullptr;
    std::string model_name;
    if (!model_target.empty()) {
        BlockModelOutputBindingV1 binding;
        binding.primary_target = stmt.dest.c_str();
        if (model_target.compare(0, 10, "dataframe.") == 0) {
            model_name = model_target.substr(10);
            model_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
            if (!model_registry) {
                if (error) *error = "model channel registry unavailable";
                return EINVAL;
            }
            model_dataframe = std::make_shared<DataFrameChannel>("dataframe", model_name);
            if (model_dataframe->Open() != 0) {
                if (error) *error = "model staging channel open failed";
                return EIO;
            }
            binding.dataframe = model_dataframe;
        } else {
            std::string category, name, relation;
            if (!ParseDatabaseDestination(model_target, &category, &name, &relation) || relation.empty()) {
                if (error) *error = "invalid model table target";
                return EINVAL;
            }
            auto* leases = querier_ ? static_cast<IDatabaseChannelLeaseProviderV1*>(
                                          querier_->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1))
                                    : nullptr;
            if (!leases) {
                if (error) *error = "model database lease provider unavailable";
                return EINVAL;
            }
            binding.database = leases->AcquireChannel(category.c_str(), name.c_str());
        }
        const int rc = model_task->BindModelOutput(binding);
        if (rc != 0) {
            if (managed_task && managed_result_json) *managed_result_json = managed_task->ManagedSinkResultJson();
            if (error) *error = "model target binding failed: " + SafeBlockTransformLastError(execution.get());
            return rc;
        }
    }
    auto* result_task = dynamic_cast<IBlockTransformResultOutputTaskV1*>(execution.get());
    const bool specified_table = managed_sink && managed_sink->relation && managed_sink->relation[0];
    const bool specified_result = result_task && (!managed_sink || specified_table);
    if (specified_table && !result_task) {
        if (error) *error = "operator lacks specified result output capability";
        return EINVAL;
    }
    if (specified_result) {
        BlockResultOutputBindingV1 binding;
        binding.target = stmt.dest.c_str();
        if (specified_table) {
            auto* leases = querier_ ? static_cast<IDatabaseChannelLeaseProviderV1*>(
                                          querier_->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1))
                                    : nullptr;
            binding.database = leases ? leases->AcquireChannel(managed_sink->category, managed_sink->name) : nullptr;
        }
        const int rc = result_task->BindResultOutput(binding);
        if (rc != 0) {
            if (managed_task && managed_result_json) *managed_result_json = managed_task->ManagedSinkResultJson();
            if (error) *error = "result target binding failed: " + SafeBlockTransformLastError(execution.get());
            return rc;
        }
    }
    if (managed_sink && !specified_table) {
        if (!managed_task) {
            if (error) *error = "block transform task does not support managed sink";
            return EINVAL;
        }
        const int bind_rc = managed_task->BindManagedSink(*managed_sink);
        if (bind_rc != 0) {
            if (error) *error = "managed sink binding failed: " + SafeBlockTransformLastError(execution.get());
            return bind_rc;
        }
    }
    if (database_source) {
        auto* input_task = dynamic_cast<IBlockTransformDatabaseInputTaskV1*>(execution.get());
        if (!input_task) {
            if (error) *error = "operator lacks database input capability";
            return EINVAL;
        }
        BlockDatabaseInputBindingV1 binding;
        binding.source = std::move(database_source);
        binding.exact_source = stmt.source.c_str();
        const int rc = input_task->CreateDatabaseInput(binding, &database_input);
        if (rc != 0) {
            if (managed_task && managed_result_json) *managed_result_json = managed_task->ManagedSinkResultJson();
            if (error) *error = "database input creation failed: " + SafeBlockTransformLastError(execution.get());
            return rc;
        }
        input_guard = std::unique_ptr<void, std::function<void(void*)>>(
            reinterpret_cast<void*>(1), [input_task, &database_input](void*) {
                try {
                    input_task->ReleaseDatabaseInput(database_input.input);
                } catch (...) {
                }
                database_input.schema.reset();
            });
        if (database_input.struct_size != sizeof(database_input) ||
            database_input.contract_version != kBlockDatabaseInputVersionV1 || !database_input.input ||
            !database_input.schema) {
            if (error) *error = "invalid database input contract";
            return EINVAL;
        }
        source = database_input.input;
        source_schema = database_input.schema;
    }
    if (dataframe_source) {
        auto* input_task = dynamic_cast<IBlockTransformDataFrameInputTaskV1*>(execution.get());
        if (!input_task) {
            if (error) *error = "operator lacks DataFrame input capability";
            return EINVAL;
        }
        BlockDataFrameInputBindingV1 binding;
        binding.source = std::move(dataframe_source);
        binding.exact_source = stmt.source.c_str();
        const int rc = input_task->CreateDataFrameInput(binding, &dataframe_input);
        if (rc != 0) {
            if (managed_task && managed_result_json) *managed_result_json = managed_task->ManagedSinkResultJson();
            if (error) *error = "DataFrame input creation failed: " + SafeBlockTransformLastError(execution.get());
            return rc;
        }
        input_guard = std::unique_ptr<void, std::function<void(void*)>>(
            reinterpret_cast<void*>(1), [input_task, &dataframe_input](void*) {
                try {
                    input_task->ReleaseDataFrameInput(dataframe_input.input);
                } catch (...) {
                }
                dataframe_input.schema.reset();
            });
        if (dataframe_input.struct_size != sizeof(dataframe_input) ||
            dataframe_input.contract_version != kBlockDataFrameInputVersionV1 || !dataframe_input.input ||
            !dataframe_input.schema || dataframe_input.source_fingerprint.empty()) {
            if (error) *error = "invalid DataFrame input contract";
            return EINVAL;
        }
        source = dataframe_input.input;
        source_schema = dataframe_input.schema;
    }
    contracts::CheckedBlockTask input_checked(execution.get(), provider_adapter, contract_state);
    SchemaCheckingBlockTransformTask checked_execution(&input_checked, planned_output_schema);
    IBlockTransformTaskV1* runner_task =
        planned_output_schema ? static_cast<IBlockTransformTaskV1*>(&checked_execution) : &input_checked;

    BlockTransformPipelineConfig config;
    config.source = source;
    config.source_schema = source_schema;
    config.transform = runner_task;
    config.input_progress_task = dynamic_cast<IBlockTransformInputProgressTaskV1*>(execution.get());
    config.time_transform = execution_time ? &input_checked : nullptr;
    config.capture_fact_task =
        capture_reader ? dynamic_cast<IBlockTransformCaptureFactTaskV2*>(execution.get()) : nullptr;
    config.source_residual = source_residual;
    config.transform_residual = transform_filter_plan.residual_expression;
    if (result_sink) {
        config.output_open = [result_sink, columns = stmt.columns](const std::shared_ptr<arrow::Schema>& schema) {
            if (columns.empty()) return result_sink->Open(schema);
            std::vector<std::shared_ptr<arrow::Field>> fields;
            for (const auto& name : columns) {
                int i = schema->GetFieldIndex(name);
                if (i < 0) return EINVAL;
                fields.push_back(schema->field(i));
            }
            return result_sink->Open(arrow::schema(fields, schema->metadata()));
        };
        config.output_cancel = [result_sink] { result_sink->Cancel(); };
    }
    config.output_consumer = [appendable_sink, managed_sink, result_sink,
                              columns = stmt.columns](const BlockTransformOutputV1& output) {
        if (!output.batch) return EINVAL;
        if (result_sink) {
            if (columns.empty()) return result_sink->Write(output.batch);
            std::vector<int> indices;
            for (const auto& name : columns) {
                int i = output.batch->schema()->GetFieldIndex(name);
                if (i < 0) return EINVAL;
                indices.push_back(i);
            }
            auto selected = output.batch->SelectColumns(indices);
            return selected.ok() ? result_sink->Write(*selected) : EINVAL;
        }
        if (managed_sink) {
            if (!columns.empty()) {
                std::vector<int> indices;
                for (const auto& name : columns) {
                    const int index = output.batch->schema()->GetFieldIndex(name);
                    if (index < 0) return EINVAL;
                    indices.push_back(index);
                }
                if (!output.batch->SelectColumns(indices).ok()) return EINVAL;
            }
            return 0;
        }
        DataFrame frame;
        frame.FromArrow(output.batch);
        return appendable_sink->Append(&frame);
    };

    config.control = SchedulerBatchRuntime::CurrentControl();
    config.opened_callback = [managed_task, control = config.control]() {
        if (managed_task && control) control->Publish(managed_task->ManagedSinkResultJson());
    };
    config.progress_callback = config.opened_callback;
    const auto run_control = config.control;
    BlockTransformPipelineRunner runner(std::move(config));
    BlockTransformPipelineResult result;
    std::string runner_error;
    const auto runner_rc = runner.Run(&result, &runner_error);
    publish_check();
    if (!contract_state->error.code.empty()) {
        if (error) *error = contract_state->ErrorText();
        return EINVAL;
    }
    if (runner_rc == BlockTransformPipelineError::kNone &&
        result.terminal == BlockTransformPipelineTerminal::kCompleted && specified_result && !specified_table) {
        bool published = stmt.dest.empty();
        if (!stmt.dest.empty()) {
            auto* registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
            const auto name = DataframeNamePart(stmt.dest);
            auto staged = std::make_shared<DataFrameChannel>("dataframe", name);
            DataFrame frame;
            if (registry && staged->Open() == 0 && sink->Read(&frame) == 0 && staged->Write(&frame) == 0) {
                if (registry->Get(name.c_str())) (void)registry->Unregister(name.c_str());
                published = registry->Register(name.c_str(), staged) == 0;
            }
        }
        if (result_task->CompleteResultOutputPublication(published) != 0) {
            if (model_dataframe) (void)model_task->CompleteModelOutputPublication(false);
            if (managed_task && managed_result_json) *managed_result_json = managed_task->ManagedSinkResultJson();
            if (error) *error = "result publication failed: " + SafeBlockTransformLastError(execution.get());
            return EIO;
        }
    }
    if (runner_rc == BlockTransformPipelineError::kNone &&
        result.terminal == BlockTransformPipelineTerminal::kCompleted && model_dataframe) {
        if (model_registry->Get(model_name.c_str())) (void)model_registry->Unregister(model_name.c_str());
        const bool published =
            model_registry->Register(model_name.c_str(), std::static_pointer_cast<IChannel>(model_dataframe)) == 0;
        if (model_task->CompleteModelOutputPublication(published) != 0) {
            if (managed_task && managed_result_json) *managed_result_json = managed_task->ManagedSinkResultJson();
            if (error) *error = "model publication failed: " + SafeBlockTransformLastError(execution.get());
            return EIO;
        }
        if (contract_checks && contracts::BindNamedOutput(querier_, model_name, contract_checks, &contract_state->error,
                                                          "model_parameters")) {
            if (error) *error = contract_state->ErrorText();
            return EIO;
        }
    }
    if (managed_task) {
        const auto result_json = managed_task->ManagedSinkResultJson();
        if (managed_result_json) *managed_result_json = result_json;
        if (run_control) run_control->Publish(result_json);
    }
    if (rows_affected) *rows_affected = result.output_rows;
    if (result.terminal == BlockTransformPipelineTerminal::kCompleted) {
        if (terminal) *terminal = BlockExecutionTerminal::kCompleted;
    } else if (result.terminal == BlockTransformPipelineTerminal::kStopped) {
        if (terminal) *terminal = BlockExecutionTerminal::kStopped;
    } else if (result.terminal == BlockTransformPipelineTerminal::kCancelled) {
        if (terminal) *terminal = BlockExecutionTerminal::kCancelled;
    }
    if (runner_rc == BlockTransformPipelineError::kNone) return 0;
    if (error) {
        *error = runner_error.empty() ? "block transform pipeline execution failed" : std::move(runner_error);
    }
    return runner_rc == BlockTransformPipelineError::kCancelled ? ECANCELED : EIO;
}

int SchedulerPlugin::ExecuteBlockTransformPipeline(
    IBlockStreamChannel* source, const std::vector<BlockTransformProviderRef>& providers, IDataFrameChannel* sink,
    const BlockTransformManagedSinkBindingV1* managed_sink, const SqlStatement& stmt,
    const std::shared_ptr<const BoundFilterExpr>& source_residual, BlockExecutionTerminal* terminal,
    int64_t* rows_affected, std::string* managed_result_json, std::string* error,
    std::shared_ptr<IDatabaseChannel> database_source, std::shared_ptr<IDataFrameChannel> dataframe_source,
    std::string* contract_checks) {
    if (providers.size() == 1) {
        return ExecuteSingleBlockTransformPipeline(source, providers.front(), sink, managed_sink, stmt, source_residual,
                                                   terminal, rows_affected, managed_result_json, error,
                                                   std::move(database_source), std::move(dataframe_source), nullptr,
                                                   contract_checks);
    }
    if (database_source || dataframe_source) {
        if (error) *error = "snapshot block input requires one operator";
        return EINVAL;
    }
    if (managed_sink) {
        if (error) *error = "managed sink requires exactly one block transform operator";
        return EINVAL;
    }

    if (terminal) *terminal = BlockExecutionTerminal::kFailed;
    if (rows_affected) *rows_affected = 0;
    if (error) error->clear();
    if (!source || !sink || providers.size() < 2 || providers.size() != stmt.operators.size()) {
        if (error) {
            *error =
                "multi block transform pipeline requires one source, aligned providers, "
                "operators, and a sink";
        }
        return EINVAL;
    }
    for (const auto& provider : providers) {
        if (!provider || (provider.v1 && provider.v2)) {
            if (error) *error = "multi block transform pipeline received an invalid provider";
            return EINVAL;
        }
    }
    auto* appendable_sink = dynamic_cast<IAppendableDataFrameChannel*>(sink);
    if (!appendable_sink) {
        if (error) *error = "multi block transform pipeline requires an appendable DataFrame sink";
        return EINVAL;
    }
    auto* capture_reader = dynamic_cast<ICaptureBlockStreamReaderV2*>(source);
    if (capture_reader && stmt.operators.front().category == "npm" && stmt.operators.front().name == "basic") {
        if (error)
            *error =
                "continuous npm.basic requires INTO <database>.<name> managed target; "
                "DataFrame/unbounded foreground output is unsupported";
        return EINVAL;
    }
    CaptureSourceSetV2 capture_sources;
    if (capture_reader) {
        int describe_rc = 0;
        try {
            describe_rc = capture_reader->DescribeSources(&capture_sources);
        } catch (...) {
            if (error) *error = "capture reader Describe threw";
            return EFAULT;
        }
        if (describe_rc != 0 || ValidateCaptureSourceSetV2(capture_sources) != CaptureDescriptionErrorV1::kNone) {
            if (error) *error = "capture reader returned an invalid identity or limits";
            return describe_rc != 0 ? describe_rc : EINVAL;
        }
    }

    std::vector<const std::string*> stage_filter_texts(providers.size() + 1);
    for (size_t stage = 1; stage < stage_filter_texts.size(); ++stage) {
        bool duplicate = false;
        stage_filter_texts[stage] = FindStageFilterText(stmt, static_cast<uint32_t>(stage), &duplicate);
        if (duplicate) {
            if (error) *error = "block transform stage contains duplicate filters";
            return EINVAL;
        }
    }
    for (const auto& filter : stmt.stage_filters) {
        if (filter.after_stage >= stage_filter_texts.size()) {
            if (error) *error = "block transform pipeline received an unsupported filter stage";
            return EINVAL;
        }
    }

    std::vector<std::shared_ptr<FilterExpr>> stage_expressions(stage_filter_texts.size());
    for (size_t stage = 1; stage < stage_filter_texts.size(); ++stage) {
        if (!ParseStageFilter(stage_filter_texts[stage], static_cast<uint32_t>(stage), &stage_expressions[stage],
                              error)) {
            return EINVAL;
        }
    }

    const auto source_schema = BlockSourceArrowSchema(source);
    struct StagePlan {
        BlockTransformProviderRef provider;
        std::string with_params_json;
        FilterTaskSessionPlan filter_plan;
        std::shared_ptr<arrow::Schema> output_schema;
    };
    std::vector<StagePlan> stage_plans(providers.size());
    const std::unordered_map<std::string, std::string> empty_params;
    const std::string task_id = NextStreamTaskId();
    auto input_schema = source_schema;

    using TaskHolder = std::unique_ptr<IBlockTransformTaskV1, std::function<void(IBlockTransformTaskV1*)>>;
    auto make_task_holder = [](BlockTransformProviderRef provider, IBlockTransformTaskV1* task) {
        return TaskHolder(task, [provider](IBlockTransformTaskV1* owned) {
            if (!owned) return;
            try {
                if (provider.v2) {
                    provider.v2->ReleaseTask(static_cast<IBlockTransformTaskV2*>(owned));
                } else {
                    provider.v1->ReleaseTask(owned);
                }
            } catch (...) {
            }
        });
    };
    auto create_task = [](BlockTransformProviderRef provider, const std::string& session_task_id,
                          const std::string& with_params_json, const FilterTaskSessionPlan& filter_plan,
                          IBlockTransformTaskV1** data_task, IBlockTransformTimeDrivenTaskV1** time_task,
                          std::string* plan_error) {
        if (data_task) *data_task = nullptr;
        if (time_task) *time_task = nullptr;
        if (provider.v1) {
            return CreateBlockTransformTaskSession(provider.v1, session_task_id, with_params_json, filter_plan,
                                                   data_task, plan_error);
        }
        if (!provider.v2 || !data_task || !time_task || session_task_id.empty() || with_params_json.empty() ||
            filter_plan.pushed_filter_plan_json.empty()) {
            if (plan_error) *plan_error = "invalid V2 block transform task request";
            return FilterPlanError::kInvalidArgument;
        }

        BlockTransformTaskConfigV2 config{};
        config.struct_size = kBlockTransformTaskConfigV2Size;
        config.contract_version = kBlockTransformContractVersionV2;
        config.task_id = session_task_id.c_str();
        config.with_params_json = with_params_json.c_str();
        config.pushed_filter_plan_json = filter_plan.pushed_filter_plan_json.c_str();
        IBlockTransformTaskV2* created_task = nullptr;
        const int create_rc = provider.v2->CreateTask(config, &created_task);
        if (create_rc != 0 || !created_task) {
            if (created_task) provider.v2->ReleaseTask(created_task);
            if (plan_error) {
                *plan_error = "V2 block transform task creation failed";
                if (create_rc != 0) {
                    *plan_error += " with code " + std::to_string(create_rc);
                } else {
                    *plan_error += ": provider returned a null task";
                }
            }
            return FilterPlanError::kTaskSessionCreationFailed;
        }
        *data_task = created_task;
        *time_task = created_task;
        return FilterPlanError::kNone;
    };

    std::vector<std::shared_ptr<contracts::BlockInvocation>> contract_states;
    const auto contract_run = dataset::Token();
    auto publish_checks = [&] {
        if (!contract_checks) return;
        dataset::Json checks(rapidjson::kArrayType);
        for (const auto& state : contract_states)
            if (state->invocation.check.IsObject())
                checks.PushBack(dataset::Copy(state->invocation.check, checks.GetAllocator()), checks.GetAllocator());
        *contract_checks = dataset::Encode(checks);
    };
    for (size_t i = 0; i < providers.size(); ++i) {
        auto& plan = stage_plans[i];
        plan.provider = providers[i];
        plan.filter_plan.pushed_filter_plan_json = kEmptyCanonicalFilterPlanV1;
        const auto& params = i < stmt.operator_with_params.size() ? stmt.operator_with_params[i] : empty_params;
        plan.with_params_json = MakeWithParamsJson(params);
        auto state = std::make_shared<contracts::BlockInvocation>();
        state->querier = querier_;
        state->invocation.component = plan.provider.component;
        if (state->invocation.component.category.empty())
            state->invocation.component.category = stmt.operators[i].category;
        if (state->invocation.component.name.empty()) state->invocation.component.name = stmt.operators[i].name;
        state->parameters.Parse(plan.with_params_json.c_str());
        dataset::Set(state->context, "source", i == 0 ? stmt.source : "temporary", state->context.GetAllocator());
        dataset::Set(state->context, "contract_run_id", contract_run, state->context.GetAllocator());
        state->context.AddMember("contract_stage_index", static_cast<unsigned>(i), state->context.GetAllocator());
        if (i) state->previous = contract_states.back();
        contract_states.push_back(state);

        IBlockTransformTaskV1* probe_raw = nullptr;
        IBlockTransformTimeDrivenTaskV1* probe_time = nullptr;
        std::string plan_error;
        const auto create_probe =
            create_task(plan.provider, task_id + ".stage" + std::to_string(i + 1) + ".schema", plan.with_params_json,
                        plan.filter_plan, &probe_raw, &probe_time, &plan_error);
        if (create_probe != FilterPlanError::kNone) {
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) + " Schema probe creation failed: " + plan_error;
            }
            return EIO;
        }
        auto probe = make_task_holder(plan.provider, probe_raw);
        auto* adapter = plan.provider.v2 ? dynamic_cast<IOperatorConfigAdapterV1*>(plan.provider.v2)
                                         : dynamic_cast<IOperatorConfigAdapterV1*>(plan.provider.v1);
        if (state->Check(probe.get(), adapter, input_schema)) {
            publish_checks();
            if (error) *error = state->ErrorText();
            return EINVAL;
        }
        const int input_bind_rc = BindBlockTransformInputSource(probe.get(), stmt.source, error);
        if (input_bind_rc != 0) return input_bind_rc;

        if (i == 0 && capture_reader) {
            auto* capture_task = dynamic_cast<IBlockTransformCaptureFactTaskV2*>(probe.get());
            if (capture_task && capture_task->BindCaptureSources(capture_sources) != 0) return EINVAL;
        }
        int probe_open_rc = 0;
        try {
            probe_open_rc = probe->Open(input_schema, &plan.output_schema);
        } catch (const std::exception& ex) {
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) + " Schema probe Open threw: " + ex.what();
            }
            try {
                probe->Cancel();
            } catch (...) {
            }
            return EFAULT;
        } catch (...) {
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) + " Schema probe Open threw an unknown exception";
            }
            try {
                probe->Cancel();
            } catch (...) {
            }
            return EFAULT;
        }
        if (probe_open_rc != 0 || !plan.output_schema) {
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) + " Schema probe Open failed";
                const std::string detail = SafeBlockTransformLastError(probe.get());
                if (!detail.empty()) *error += ": " + detail;
            }
            try {
                probe->Cancel();
            } catch (...) {
            }
            return probe_open_rc != 0 ? probe_open_rc : EINVAL;
        }
        try {
            probe->Cancel();
        } catch (...) {
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) + " Schema probe Cancel failed";
            }
            return EFAULT;
        }
        probe.reset();

        if (stage_expressions[i + 1]) {
            std::shared_ptr<const BoundFilterExpr> bound_filter;
            const std::string stage_label = "transform stage " + std::to_string(i + 1);
            const int filter_rc = ResolveAndBindStageFilter(
                querier_, FilterDomainTargetKindV1::kTransform, stmt.operators[i].category, stmt.operators[i].name,
                plan.output_schema, stage_expressions[i + 1], stage_label, &bound_filter, &plan_error);
            if (filter_rc != 0) {
                if (error) *error = plan_error;
                return filter_rc;
            }

            FilterPushdownTarget target;
            target.kind = FilterPushdownTargetKindV1::kTransform;
            target.category = stmt.operators[i].category;
            target.name = stmt.operators[i].name;
            target.output_schema = plan.output_schema;
            FilterPushdownNegotiation negotiation;
            const auto negotiate_rc =
                NegotiateFilterPushdown(querier_, target, bound_filter, &negotiation, &plan_error);
            if (negotiate_rc != FilterPlanError::kNone) {
                if (error) {
                    *error = "transform stage " + std::to_string(i + 1) +
                             " filter pushdown negotiation failed: " + plan_error;
                }
                return negotiate_rc == FilterPlanError::kInvalidArgument ||
                               negotiate_rc == FilterPlanError::kInvalidBoundExpression ||
                               negotiate_rc == FilterPlanError::kCanonicalPlanError
                           ? EINVAL
                           : EIO;
            }
            const auto materialize_rc =
                MaterializeFilterTaskSessionPlan(plan.output_schema, bound_filter, negotiation,
                                                 FilterTaskIsolation::kExclusive, &plan.filter_plan, &plan_error);
            if (materialize_rc != FilterPlanError::kNone) {
                if (error) {
                    *error = "transform stage " + std::to_string(i + 1) + " filter task planning failed: " + plan_error;
                }
                return EIO;
            }
        }
        input_schema = plan.output_schema;
    }

    std::vector<TaskHolder> execution_holders;
    std::vector<IBlockTransformTaskV1*> execution_tasks;
    std::vector<IBlockTransformTimeDrivenTaskV1*> time_tasks;
    std::vector<std::unique_ptr<contracts::CheckedBlockTask>> checked_tasks;
    execution_holders.reserve(stage_plans.size());
    execution_tasks.reserve(stage_plans.size());
    time_tasks.reserve(stage_plans.size());
    for (size_t i = 0; i < stage_plans.size(); ++i) {
        auto& plan = stage_plans[i];
        IBlockTransformTaskV1* execution_raw = nullptr;
        IBlockTransformTimeDrivenTaskV1* execution_time = nullptr;
        std::string plan_error;
        const auto create_execution =
            create_task(plan.provider, task_id + ".stage" + std::to_string(i + 1) + ".run", plan.with_params_json,
                        plan.filter_plan, &execution_raw, &execution_time, &plan_error);
        if (create_execution != FilterPlanError::kNone) {
            for (auto* task : execution_tasks) {
                try {
                    task->Cancel();
                } catch (...) {
                }
            }
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) + " execution task creation failed: " + plan_error;
            }
            return EIO;
        }
        auto execution = make_task_holder(plan.provider, execution_raw);
        const int input_bind_rc = BindBlockTransformInputSource(execution.get(), stmt.source, error);
        if (input_bind_rc != 0) return input_bind_rc;
        if (i == 0 && capture_reader) {
            auto* capture_task = dynamic_cast<IBlockTransformCaptureFactTaskV2*>(execution.get());
            if (capture_task && capture_task->BindCaptureSources(capture_sources) != 0) {
                if (error) *error = "capture source binding failed";
                return EINVAL;
            }
        }
        auto* adapter = plan.provider.v2 ? dynamic_cast<IOperatorConfigAdapterV1*>(plan.provider.v2)
                                         : dynamic_cast<IOperatorConfigAdapterV1*>(plan.provider.v1);
        checked_tasks.push_back(
            std::make_unique<contracts::CheckedBlockTask>(execution_raw, adapter, contract_states[i]));
        execution_tasks.push_back(checked_tasks.back().get());
        time_tasks.push_back(execution_time ? checked_tasks.back().get() : nullptr);
        execution_holders.push_back(std::move(execution));
    }

    std::vector<std::shared_ptr<arrow::Schema>> expected_output_schemas;
    std::vector<std::shared_ptr<const BoundFilterExpr>> residuals;
    expected_output_schemas.reserve(stage_plans.size());
    residuals.reserve(stage_plans.size());
    for (const auto& plan : stage_plans) {
        expected_output_schemas.push_back(plan.output_schema);
        residuals.push_back(plan.filter_plan.residual_expression);
    }
    const bool has_time_tasks =
        std::any_of(time_tasks.begin(), time_tasks.end(), [](const auto* task) { return task != nullptr; });
    SynchronousBlockTransformChainTask chain_task(execution_tasks, std::move(time_tasks),
                                                  std::move(expected_output_schemas), residuals);

    BlockTransformPipelineConfig config;
    config.source = source;
    config.source_schema = source_schema;
    config.transform = &chain_task;
    config.time_transform = has_time_tasks ? &chain_task : nullptr;
    config.capture_fact_task = capture_reader && !execution_tasks.empty()
                                   ? dynamic_cast<IBlockTransformCaptureFactTaskV2*>(execution_tasks.front())
                                   : nullptr;
    config.source_residual = source_residual;
    config.output_consumer = [appendable_sink](const BlockTransformOutputV1& output) {
        if (!output.batch) return EINVAL;
        DataFrame frame;
        frame.FromArrow(output.batch);
        return appendable_sink->Append(&frame);
    };

    BlockTransformPipelineRunner runner(std::move(config));
    BlockTransformPipelineResult result;
    std::string runner_error;
    const auto runner_rc = runner.Run(&result, &runner_error);
    publish_checks();
    for (const auto& state : contract_states)
        if (!state->error.code.empty()) {
            if (error) *error = state->ErrorText();
            return EINVAL;
        }
    if (rows_affected) *rows_affected = result.output_rows;
    if (result.terminal == BlockTransformPipelineTerminal::kCompleted) {
        if (terminal) *terminal = BlockExecutionTerminal::kCompleted;
    } else if (result.terminal == BlockTransformPipelineTerminal::kStopped) {
        if (terminal) *terminal = BlockExecutionTerminal::kStopped;
    } else if (result.terminal == BlockTransformPipelineTerminal::kCancelled) {
        if (terminal) *terminal = BlockExecutionTerminal::kCancelled;
    }
    if (runner_rc == BlockTransformPipelineError::kNone) return 0;
    if (error) {
        *error = runner_error.empty() ? "multi block transform pipeline execution failed" : std::move(runner_error);
    }
    return runner_rc == BlockTransformPipelineError::kCancelled ? ECANCELED : EIO;
}

// --- Build Database Query ---

// 从目标名称中提取表名（支持三段式 type.name.table）
static std::string ExtractTableName(const std::string& dest_name) {
    auto pos1 = dest_name.find('.');
    if (pos1 != std::string::npos) {
        auto pos2 = dest_name.find('.', pos1 + 1);
        if (pos2 != std::string::npos) {
            return dest_name.substr(pos2 + 1);  // 三段式
        }
        return dest_name.substr(pos1 + 1);  // 两段式
    }
    return dest_name;
}

// BuildQuery: 构建数据库查询语句
// 将 sql_part 中的第一个 FROM 子句替换为实际表名（支持多段式表名规范化）
// 子查询中的 FROM 保持原样，不做替换
static std::string BuildQuery(const std::string& source_name, const SqlStatement& stmt) {
    std::string sql = stmt.sql_part;
    std::string table = ExtractTableName(source_name);

    // 匹配第一个 FROM 子句（支持多段式表名如 catalog.db.table）
    // 使用不区分大小写，兼容 "from" / "From" 等写法。
    std::regex FROM_PATTERN(R"((\bFROM\s+)((?:[\w-]+\.)*[\w-]+))", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(sql, m, FROM_PATTERN)) {
        // 只替换第一个匹配（主查询的 FROM），子查询不受影响
        sql = sql.substr(0, m.position()) + m[1].str() + table + sql.substr(m.position() + m.length());
    }

    return sql;
}

// --- 辅助：对 DataFrame 通道应用 WHERE 过滤 ---
static std::shared_ptr<DataFrameChannel> ApplyDataFrameFilter(IDataFrameChannel* src, const std::string& where_clause,
                                                              uint64_t seq) {
    DataFrame data;
    if (src->Read(&data) != 0 || data.RowCount() == 0) return nullptr;

    if (data.Filter(where_clause.c_str()) != 0) return nullptr;

    auto filtered = std::make_shared<DataFrameChannel>("_filter", std::to_string(seq));
    filtered->Open();
    filtered->Write(&data);
    return filtered;
}

namespace {
std::string PublicationJson(const ResultSinkReportV1& r, const std::string& target) {
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject();
    auto text = [&](const char* key, const std::string& s) {
        w.Key(key);
        w.String(s.data(), s.size());
    };
    text("target", target);
    text("publication_id", r.publication_id);
    text("state", r.state);
    text("code", r.code);
    text("stage", r.stage);
    w.Key("input_rows");
    w.Uint64(r.input_rows);
    w.Key("confirmed_rows");
    w.Uint64(r.confirmed_rows);
    w.Key("not_sent_rows");
    w.Uint64(r.not_sent_rows);
    w.Key("unknown_rows");
    w.Uint64(r.unknown_rows);
    w.Key("binding");
    if (r.binding_json.empty()) {
        w.StartObject();
        w.EndObject();
    } else
        w.RawValue(r.binding_json.data(), r.binding_json.size(), rapidjson::kObjectType);
    w.EndObject();
    return b.GetString();
}
std::shared_ptr<arrow::RecordBatch> ProjectResult(const std::shared_ptr<arrow::RecordBatch>& b,
                                                  const std::vector<std::string>& columns) {
    if (!b || columns.empty()) return b;
    std::vector<int> indices;
    for (const auto& column : columns) {
        int i = b->schema()->GetFieldIndex(column);
        if (i < 0) return {};
        indices.push_back(i);
    }
    auto result = b->SelectColumns(indices);
    return result.ok() ? *result : nullptr;
}
}  // namespace
int32_t SchedulerPlugin::ExecuteResultSink(IChannel* source, const SqlStatement& stmt,
                                           const std::shared_ptr<const BoundFilterExpr>& residual, std::string& rsp) {
    const auto control = SchedulerBatchRuntime::CurrentControl();
    std::unique_ptr<IResultSinkWriterV1> writer;
    std::string contract_checks = "[]";
    ResultSinkReportV1 fallback;
    fallback.state = "failed";
    fallback.code = "SINK_RESOLVE_FAILED";
    fallback.stage = "sink_resolve";
    auto finish_response = [&](int rc, const char* status, const char* fallback_code = nullptr) {
        auto report = writer ? writer->Report() : fallback;
        if (rc && (report.code.empty() || report.code == "OK" || report.code == "CANCELLED") && fallback_code) {
            report.code = fallback_code;
            report.stage = "sink_deliver";
        }
        const auto publication = PublicationJson(report, stmt.dest);
        if (control) control->Publish("{\"publication\":" + publication + "}");
        rapidjson::StringBuffer b;
        rapidjson::Writer<rapidjson::StringBuffer> w(b);
        w.StartObject();
        w.Key("status");
        w.String(status);
        w.Key("rows");
        w.Uint64(report.confirmed_rows);
        w.Key("rows_affected");
        w.Uint64(report.confirmed_rows);
        w.Key("result_row_count");
        w.Uint64(report.confirmed_rows);
        w.Key("contract_checks");
        w.RawValue(contract_checks.c_str(), contract_checks.size(), rapidjson::kArrayType);
        w.Key("result_target");
        w.String(stmt.dest.c_str());
        w.Key("result");
        w.StartObject();
        w.Key("publication");
        w.RawValue(publication.data(), publication.size(), rapidjson::kObjectType);
        w.EndObject();
        if (rc) {
            w.Key("error");
            w.String(report.code.c_str());
            w.Key("error_code");
            w.String(report.code.c_str());
            w.Key("error_stage");
            w.String(report.stage.c_str());
        }
        w.EndObject();
        rsp = b.GetString();
        return rc ? error::INTERNAL_ERROR : error::OK;
    };
    auto* block = dynamic_cast<IBlockStreamChannel*>(source);
    auto* database = dynamic_cast<IDatabaseChannel*>(source);
    auto* dataframe = dynamic_cast<IDataFrameChannel*>(source);
    if (!source || (!block && !database && !dataframe) || stmt.sources.size() != 1) {
        fallback.code = "SINK_SOURCE_UNSUPPORTED";
        return finish_response(ENOTSUP, "failed");
    }
    // A non-transform operator must explicitly support ordinary DataFrame output. Discovery precedes input reads.
    BlockTransformProviderRef provider;
    CppOperatorCapabilityLeaseV1 provider_lease;
    std::vector<std::shared_ptr<IOperator>> holders;
    std::vector<IOperator*> operators;
    if (!stmt.operators.empty()) {
        bool ambiguous = false;
        int traverse = 0;
        if (stmt.operators.size() == 1)
            provider = FindBlockTransformOperator(stmt.operators[0].category, stmt.operators[0].name, &provider_lease,
                                                  &ambiguous, &traverse);
        if (ambiguous || traverse) {
            fallback.code = "SINK_OPERATOR_OUTPUT_UNSUPPORTED";
            return finish_response(ENOTSUP, "failed");
        }
        if (provider && !block) {
            fallback.code = "SINK_OPERATOR_OUTPUT_UNSUPPORTED";
            return finish_response(ENOTSUP, "failed");
        }
        if (!provider) {
            if (std::any_of(stmt.stage_filters.begin(), stmt.stage_filters.end(),
                            [](const StageFilterClause& filter) { return filter.after_stage > 0; })) {
                fallback.code = "SINK_OPERATOR_FILTER_UNSUPPORTED";
                return finish_response(ENOTSUP, "failed");
            }
            if (block) {
                fallback.code = "SINK_OPERATOR_OUTPUT_UNSUPPORTED";
                return finish_response(ENOTSUP, "failed");
            }
            for (auto& op : stmt.operators) {
                auto holder = FindOperator(op.category, op.name);
                if (!holder) {
                    fallback.code = "SINK_OPERATOR_OUTPUT_UNSUPPORTED";
                    return finish_response(ENOTSUP, "failed");
                }
                operators.push_back(holder.get());
                holders.push_back(std::move(holder));
            }
            for (size_t i = 0; i < operators.size(); ++i) {
                const auto& params =
                    i < stmt.operator_with_params.size() ? stmt.operator_with_params[i] : stmt.with_params;
                for (auto& [k, v] : params)
                    if (operators[i]->Configure(k.c_str(), v.c_str())) {
                        fallback.code = "SINK_OPERATOR_CONFIGURATION";
                        return finish_response(EINVAL, "failed");
                    }
            }
        }
    }
    int create = ENOTSUP;
    if (querier_)
        querier_->Traverse(IID_RESULT_SINK_FACTORY_V1, [&](void* p) {
            create = static_cast<IResultSinkFactoryV1*>(p)->CreateWriter({1, stmt.dest, NextStreamTaskId()}, &writer);
            return create == ENOTSUP ? 0 : -1;
        });
    if (create || !writer) return finish_response(create ? create : EIO, "failed");
    struct WakeGuard {
        std::shared_ptr<BlockTransformRunControl> c;
        ~WakeGuard() {
            if (c) c->BindWake({});
        }
    } wake{control};
    auto bind_writer = [&](IBatchReader* reader = nullptr) {
        if (control)
            control->BindWake([control, p = writer.get(), reader] {
                if (control->cancel_requested.load()) {
                    p->Cancel();
                    if (reader) reader->Cancel();
                }
            });
    };
    bind_writer();
    int rc = 0;
    bool stopped = false;
    std::string upstream_error;
    try {
        if (provider) {
            BlockExecutionTerminal terminal = BlockExecutionTerminal::kFailed;
            int64_t rows = 0;
            std::string summary;
            rc =
                ExecuteSingleBlockTransformPipeline(block, provider, nullptr, nullptr, stmt, residual, &terminal, &rows,
                                                    &summary, &upstream_error, {}, {}, writer.get(), &contract_checks);
            stopped = terminal == BlockExecutionTerminal::kStopped;
            if (terminal == BlockExecutionTerminal::kCancelled) {
                writer->Cancel();
                return finish_response(0, "cancelled");
            }
            if (terminal == BlockExecutionTerminal::kFailed && !rc) rc = EIO;
            bind_writer();
        } else if (block) {
            auto schema = BlockSourceArrowSchema(source);
            BlockFilterStage filter(residual);
            std::shared_ptr<arrow::Schema> filtered;
            if (filter.Open(schema, &filtered, &upstream_error) != FilterEvalError::kNone) rc = EINVAL;
            auto empty = arrow::RecordBatch::MakeEmpty(filtered ? filtered : schema);
            auto projected = empty.ok() ? ProjectResult(*empty, stmt.columns) : nullptr;
            if (!rc) rc = projected ? writer->Open(projected->schema()) : EINVAL;
            if (control)
                control->BindWake([control, block, p = writer.get()] {
                    if (control->cancel_requested.load()) p->Cancel();
                    block->Cancel();
                });
            while (!rc) {
                if (control && control->cancel_requested.load()) {
                    writer->Cancel();
                    return finish_response(0, "cancelled");
                }
                if (control && control->stop_requested.load()) {
                    stopped = true;
                    break;
                }
                auto event = block->PollBlock(100);
                if (event.kind == BlockPollEvent::kTimeout) continue;
                if (event.kind == BlockPollEvent::kEof) break;
                if (event.kind == BlockPollEvent::kCancelled && control && control->stop_requested.load()) {
                    stopped = true;
                    break;
                }
                if (event.kind != BlockPollEvent::kData || !event.batch) {
                    rc = EIO;
                    break;
                }
                BlockTransformOutputV1 out;
                if (filter.ProcessBlock(event.batch, 0, &out, &upstream_error) != FilterEvalError::kNone) rc = EINVAL;
                auto selected = rc ? nullptr : ProjectResult(out.batch, stmt.columns);
                if (!rc) rc = selected ? writer->Write(selected) : EINVAL;
                if (!rc) rc = AcknowledgeProcessedBlock(block, event.batch);
                int released = block->ReleaseBlock(event.batch);
                if (!rc) rc = released;
            }
            bind_writer();
        } else if (!operators.empty() || dataframe) {
            DataFrame snapshot;
            if (!operators.empty()) {
                DataFrameChannel result("_temp", "publication");
                result.Open();
                std::vector<IChannel*> inputs{source};
                int64_t rows = 0;
                rc = ExecuteWithOperatorChain(Span<IChannel*>(inputs), &result, operators, ChannelType::kDataFrame,
                                              stmt, &rows, &upstream_error, &contract_checks);
                if (!rc) rc = result.Read(&snapshot);
            } else {
                rc = dataframe->Read(&snapshot);
                if (!rc && !stmt.where_clause.empty()) rc = snapshot.Filter(stmt.where_clause.c_str());
            }
            auto batch = rc ? nullptr : ProjectResult(snapshot.ToArrow(), stmt.columns);
            if (!rc) rc = batch ? writer->Open(batch->schema()) : EINVAL;
            if (!rc && !(control && control->stop_requested.load())) rc = writer->Write(batch);
            stopped = control && control->stop_requested.load();
        } else {
            std::string category, name, relation;
            if (!ParseDatabaseDestination(stmt.source, &category, &name, &relation) || relation.empty()) rc = EINVAL;
            IBatchReader* raw = nullptr;
            if (!rc) rc = database->CreateReader(BuildQuery(stmt.source, stmt).c_str(), &raw);
            std::unique_ptr<IBatchReader, std::function<void(IBatchReader*)>> reader(raw, [](auto* p) {
                if (p) {
                    p->Close();
                    p->Release();
                }
            });
            if (!rc && !reader) rc = EIO;
            if (!rc) {
                bind_writer(reader.get());
                const uint8_t* bytes = nullptr;
                size_t size = 0;
                rc = reader->GetSchema(&bytes, &size);
                auto decode = [](const uint8_t* data, size_t n) {
                    auto owned = arrow::Buffer::FromString(std::string(reinterpret_cast<const char*>(data), n));
                    return arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(owned));
                };
                if (!rc) {
                    auto schema = decode(bytes, size);
                    rc = schema.ok() ? writer->Open((*schema)->schema()) : EINVAL;
                }
                while (!rc) {
                    if (control && control->cancel_requested.load()) {
                        writer->Cancel();
                        rc = ECANCELED;
                        break;
                    }
                    if (control && control->stop_requested.load()) {
                        stopped = true;
                        break;
                    }
                    int next = reader->Next(&bytes, &size);
                    if (next == 1) break;
                    if (next) {
                        rc = EIO;
                        break;
                    }
                    auto stream = decode(bytes, size);
                    if (!stream.ok()) {
                        rc = EINVAL;
                        break;
                    }
                    while (!rc) {
                        std::shared_ptr<arrow::RecordBatch> batch;
                        if (!(*stream)->ReadNext(&batch).ok()) {
                            rc = EINVAL;
                            break;
                        }
                        if (!batch) break;
                        rc = writer->Write(batch);
                    }
                }
                bind_writer();
            }
        }
        if (control && control->cancel_requested.load()) {
            writer->Cancel();
            return finish_response(0, "cancelled");
        }
        if (rc)
            return finish_response(rc, "failed",
                                   upstream_error == "SINK_OPERATOR_OUTPUT_UNSUPPORTED"
                                       ? "SINK_OPERATOR_OUTPUT_UNSUPPORTED"
                                       : "SINK_UPSTREAM_FAILED");
        rc = writer->Finish();
        return finish_response(rc, rc ? "failed" : (stopped ? "stopped" : "completed"));
    } catch (...) {
        writer->Cancel();
        return finish_response(EIO, "failed", "SINK_UPSTREAM_FAILED");
    }
}

// --- 无算子：纯数据搬运 ---
int SchedulerPlugin::ExecuteTransfer(IChannel* source, IChannel* sink, const std::string& source_type,
                                     const std::string& sink_type, const SqlStatement& stmt,
                                     const std::shared_ptr<const BoundFilterExpr>& source_residual,
                                     int64_t* rows_affected, std::string* error) {
    if (source_type == ChannelType::kBlockStream && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IBlockStreamChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;

        const auto source_schema = BlockSourceArrowSchema(source);
        BlockFilterStage source_filter(source_residual);
        if (source_residual) {
            std::shared_ptr<arrow::Schema> filtered_schema;
            std::string filter_error;
            const auto open_rc = source_filter.Open(source_schema, &filtered_schema, &filter_error);
            if (open_rc != FilterEvalError::kNone) {
                if (error) *error = "source-stage residual Open failed: " + filter_error;
                return EINVAL;
            }
        }
        int64_t rows = 0;
        bool wrote = false;
        const auto control = SchedulerBatchRuntime::CurrentControl();
        while (true) {
            if (control && control->cancel_requested.load()) {
                src->Cancel();
                if (error) *error = "block source cancelled";
                return ECANCELED;
            }
            if (control && control->stop_requested.load()) break;
            const BlockPollEvent event = src->PollBlock(100);
            if (event.kind == BlockPollEvent::kData) {
                if (!event.batch) {
                    if (error) *error = "block source returned empty data batch";
                    return -1;
                }
                int rc = 0;
                std::shared_ptr<arrow::RecordBatch> output_batch = event.batch;
                try {
                    if (source_residual) {
                        BlockTransformOutputV1 filtered;
                        std::string filter_error;
                        const auto eval_rc = source_filter.ProcessBlock(event.batch, 0, &filtered, &filter_error);
                        if (eval_rc != FilterEvalError::kNone || !filtered.batch) {
                            if (error) {
                                *error = "source-stage residual failed: " + filter_error;
                            }
                            rc = EINVAL;
                        } else {
                            output_batch = std::move(filtered.batch);
                        }
                    }
                    if (rc == 0) {
                        DataFrame frame;
                        frame.FromArrow(output_batch);
                        if (auto* appendable = dynamic_cast<IAppendableDataFrameChannel*>(dst)) {
                            rc = appendable->Append(&frame);
                        } else if (!wrote) {
                            rc = dst->Write(&frame);
                        }
                    }
                } catch (const std::exception& ex) {
                    if (error) *error = ex.what();
                    rc = EFAULT;
                } catch (...) {
                    if (error) *error = "block source sink threw an unknown exception";
                    rc = EFAULT;
                }
                if (rc == 0) {
                    rc = AcknowledgeProcessedBlock(src, event.batch);
                    if (rc && error) *error = "block source acknowledgement failed";
                }
                int release_rc = 0;
                try {
                    release_rc = src->ReleaseBlock(event.batch);
                } catch (...) {
                    release_rc = EFAULT;
                }
                if (release_rc != 0) {
                    if (error) *error = "block source ReleaseBlock failed";
                    return -1;
                }
                if (rc != 0) {
                    if (error && error->empty()) *error = "write block source batch failed";
                    return rc;
                }
                wrote = true;
                rows += output_batch->num_rows();
                continue;
            }
            if (event.kind == BlockPollEvent::kTimeout) continue;
            if (event.kind == BlockPollEvent::kEof) break;
            if (event.kind == BlockPollEvent::kCancelled) {
                if (error) *error = "block source cancelled";
                return -1;
            }
            if (error) *error = "block source poll failed";
            return -1;
        }
        if (rows_affected) *rows_affected = rows;
        return 0;
    }

    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;

        // DataFrame + WHERE → 先过滤再复制
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            return ChannelAdapter::CopyDataFrame(filtered.get(), dst);
        }
        return ChannelAdapter::CopyDataFrame(src, dst);
    }

    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kDatabase) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;
        std::string table = ExtractTableName(stmt.dest);

        // DataFrame + WHERE → 先过滤再写入
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            int64_t rows = ChannelAdapter::WriteFromDataFrame(filtered.get(), dst, table.c_str(), error);
            if (rows_affected) *rows_affected = rows;
            return (rows < 0) ? -1 : 0;
        }
        int64_t rows = ChannelAdapter::WriteFromDataFrame(src, dst, table.c_str(), error);
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kStream) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IStreamChannel*>(sink);
        if (!src || !dst) return -1;

        IDataFrameChannel* payload_src = src;
        std::shared_ptr<DataFrameChannel> filtered;
        if (!stmt.where_clause.empty()) {
            filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            payload_src = filtered.get();
        }

        DataFrame data;
        if (payload_src->Read(&data) != 0) return -1;
        if (rows_affected) *rows_affected = data.RowCount();

        if (data.RowCount() == 0) {
            dst->CloseStream();
            return 0;
        }

        auto batch = data.ToArrow();
        if (!batch) return -1;
        const int rc = dst->Put(std::move(batch), CurrentTimeMs());
        if (rc != 0) {
            if (error) *error = "write dataframe to stream failed, rc=" + std::to_string(rc);
            return -1;
        }
        dst->CloseStream();
        return 0;
    }

    if (source_type == ChannelType::kDatabase && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;
        std::string query = BuildQuery(stmt.source, stmt);
        return ChannelAdapter::ReadToDataFrame(src, query.c_str(), dst, error);
    }

    if (source_type == ChannelType::kDatabase && sink_type == ChannelType::kDatabase) {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;

        auto tmp = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
        tmp->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(src, query.c_str(), tmp.get(), error);
        if (rc != 0) return rc;

        std::string table = ExtractTableName(stmt.dest);
        int64_t rows = ChannelAdapter::WriteFromDataFrame(tmp.get(), dst, table.c_str(), error);
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (error) *error = "unsupported transfer: " + source_type + " → " + sink_type;
    return -1;
}

// --- 有算子：自动适配通道类型 ---
int SchedulerPlugin::ExecuteWithOperator(IChannel* source, IChannel* sink, IOperator* op, const std::string& sink_type,
                                         const SqlStatement& stmt, int64_t* rows_affected, std::string* error) {
    std::vector<IOperator*> ops;
    std::vector<IChannel*> inputs;
    inputs.push_back(source);
    ops.push_back(op);
    return ExecuteWithOperatorChain(Span<IChannel*>(inputs), sink, ops, sink_type, stmt, rows_affected, error);
}

int SchedulerPlugin::ExecuteWithOperatorChain(Span<IChannel*> inputs, IChannel* sink,
                                              const std::vector<IOperator*>& ops, const std::string& sink_type,
                                              const SqlStatement& stmt, int64_t* rows_affected, std::string* error,
                                              std::string* contract_checks) {
    if (ops.empty() || inputs.empty()) return -1;
    using namespace dataset;
    Json checks(rapidjson::kArrayType);
    auto fail_contract = [&](const DataSemanticsErrorV1& issue) {
        if (error) *error = issue.code + ": " + issue.message;
        if (contract_checks) *contract_checks = Encode(checks);
        return -1;
    };
    std::vector<contracts::Invocation> invocations(ops.size());
    const auto contract_run = Token();
    DataSemanticsErrorV1 contract_error;
    for (size_t i = 0; i < ops.size(); ++i)
        if (contracts::Component(querier_, dynamic_cast<IOperatorContractIdentityV1*>(ops[i]), ops[i]->Category(),
                                 ops[i]->Name(), &invocations[i].component, &contract_error))
            return fail_contract(contract_error);
    // Source descriptions are captured before filtering/loading. They are compared again
    // after a stable Arrow snapshot has been retained for the actual Work call.
    std::vector<Json> source_snapshots(inputs.size);
    std::vector<std::string> source_bindings(inputs.size);
    std::vector<Json> source_locators(inputs.size);
    if (invocations.front().component.contract_state == "declared") {
        for (size_t n = 0; n < inputs.size; ++n) {
            const auto name = n < stmt.sources.size() ? stmt.sources[n] : stmt.source;
            if (name.compare(0, 10, "dataframe.") || std::string(inputs[n]->Type()) != ChannelType::kDataFrame)
                continue;
            source_locators[n].SetObject();
            Set(source_locators[n], "kind", "dataframe", source_locators[n].GetAllocator());
            Set(source_locators[n], "name", name.substr(10), source_locators[n].GetAllocator());
            if (contracts::DescribeInput(querier_, source_locators[n], nullptr, &source_snapshots[n],
                                         &source_bindings[n], &contract_error))
                return fail_contract(contract_error);
        }
    }

    Span<IChannel*> stage_inputs = inputs;
    std::shared_ptr<DataFrameChannel> tmp_in;
    std::vector<IChannel*> stage_input_holder;
    std::vector<std::shared_ptr<DataFrameChannel>> stage_buffers;

    // 单源算子路径仍保留 Database/DataFrame 自动适配；多源在 HandleExecute 限制为 dataframe.*
    if (stage_inputs.size == 1) {
        IChannel* single = stage_inputs[0];
        const std::string source_type(single->Type());

        if (source_type == ChannelType::kDatabase) {
            auto* db_src = dynamic_cast<IDatabaseChannel*>(single);
            if (!db_src) return -1;

            tmp_in = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
            tmp_in->Open();

            std::string query = BuildQuery(stmt.source, stmt);
            int rc = ChannelAdapter::ReadToDataFrame(db_src, query.c_str(), tmp_in.get(), error);
            if (rc != 0) return rc;

            stage_input_holder.clear();
            stage_input_holder.push_back(tmp_in.get());
            stage_inputs = Span<IChannel*>(stage_input_holder);
        } else if (source_type == ChannelType::kDataFrame && !stmt.where_clause.empty()) {
            auto* df_src = dynamic_cast<IDataFrameChannel*>(single);
            if (!df_src) return -1;

            tmp_in = ApplyDataFrameFilter(df_src, stmt.where_clause, ++tmp_channel_seq_);
            if (!tmp_in) return -1;

            stage_input_holder.clear();
            stage_input_holder.push_back(tmp_in.get());
            stage_inputs = Span<IChannel*>(stage_input_holder);
        }
    } else {
        for (size_t i = 0; i < stage_inputs.size; ++i) {
            auto* df = dynamic_cast<IDataFrameChannel*>(stage_inputs[i]);
            if (!df) {
                if (error) *error = "multi-source operator input must be dataframe channel";
                return -1;
            }
        }
    }

    for (size_t i = 0; i < ops.size(); ++i) {
        auto& invocation = invocations[i];
        std::vector<std::shared_ptr<DataFrameChannel>> frozen_channels;
        std::vector<IChannel*> frozen_inputs;
        Json input_snapshots(rapidjson::kArrayType), parameters(rapidjson::kObjectType),
            context(rapidjson::kObjectType);
        auto& ia = input_snapshots.GetAllocator();
        Value shapes(rapidjson::kArrayType);
        const auto stage_params = i < stmt.operator_with_params.size()
                                      ? stmt.operator_with_params[i]
                                      : (i == 0 ? stmt.with_params : std::unordered_map<std::string, std::string>{});
        for (const auto& [key, value] : stage_params) Set(parameters, key.c_str(), value, parameters.GetAllocator());
        if (invocation.component.contract_state == "declared") {
            auto* service = querier_
                                ? static_cast<IOperatorDataContractV1*>(querier_->First(IID_OPERATOR_DATA_CONTRACT_V1))
                                : nullptr;
            OperatorContractSnapshotV1 definition;
            if (!service || service->Resolve(invocation.component.contract_ref.c_str(), &definition, &contract_error)) {
                if (!service) contract_error = {"UNAVAILABLE", "declared operator contract service unavailable", ""};
                return fail_contract(contract_error);
            }
            Json contract;
            contract.Parse(definition.document_json->c_str());
            size_t port_index = 0, port_count = 0;
            for (size_t n = 0; n < stage_inputs.size; ++n) {
                auto* df = dynamic_cast<IDataFrameChannel*>(stage_inputs[n]);
                if (!df)
                    return fail_contract(
                        {"CONTRACT_INVALID", "declared DataFrame stage requires DataFrame inputs", ""});
                DataFrame frame;
                if (df->Read(&frame))
                    return fail_contract({"SOURCE_UNAVAILABLE", "cannot retain stage input snapshot", ""});
                auto frozen = std::make_shared<DataFrameChannel>("_contract", std::to_string(++tmp_channel_seq_));
                if (frozen->Open() || frozen->Write(&frame))
                    return fail_contract({"SOURCE_UNAVAILABLE", "cannot freeze stage input", ""});
                Json snapshot;
                std::string binding;
                if (i == 0 && n < source_locators.size() && source_locators[n].IsObject()) {
                    if (contracts::DescribeInput(querier_, source_locators[n], frame.ToArrow()->schema(), &snapshot,
                                                 &binding, &contract_error))
                        return fail_contract(contract_error);
                    if (Get(source_snapshots[n], "schema_state") == "known" &&
                        (source_snapshots[n]["object_key"] != snapshot["object_key"] ||
                         Get(source_snapshots[n], "source_revision") != Get(snapshot, "source_revision")))
                        return fail_contract({"SOURCE_CHANGED", "source changed after stage resolution", ""});
                    if (FindChannel(n < stmt.sources.size() ? stmt.sources[n] : stmt.source) != inputs[n])
                        return fail_contract({"SOURCE_CHANGED", "source instance was replaced", ""});
                } else if (i && invocations[i - 1].check.HasMember("outputs") &&
                           !invocations[i - 1].check["outputs"].Empty() &&
                           invocations[i - 1].check["outputs"][0].HasMember("binding_ref")) {
                    const auto& output = invocations[i - 1].check["outputs"][0];
                    snapshot.CopyFrom(output["snapshot"], snapshot.GetAllocator());
                    binding = Get(output, "binding_ref");
                } else if (contracts::DescribeInput(nullptr, Value(), frame.ToArrow()->schema(), &snapshot, &binding,
                                                    &contract_error))
                    return fail_contract(contract_error);
                const auto& ports = contract["inputs"];
                while (port_index + 1 < ports.Size() && port_count >= ports[port_index]["max_count"].GetUint64()) {
                    ++port_index;
                    port_count = 0;
                }
                ++port_count;
                Value input(rapidjson::kObjectType);
                Set(input, "port_id", Get(ports[port_index], "port_id"), ia);
                input.AddMember("input_index", static_cast<uint64_t>(n), ia)
                    .AddMember("snapshot", Copy(snapshot, ia), ia);
                if (!binding.empty()) Set(input, "binding_ref", binding, ia);
                input_snapshots.PushBack(input, ia);
                shapes.PushBack("dataframe", context.GetAllocator());
                frozen_inputs.push_back(frozen.get());
                frozen_channels.push_back(std::move(frozen));
            }
        }
        context.AddMember("input_shapes", shapes, context.GetAllocator());
        Set(context, "contract_run_id", contract_run, context.GetAllocator());
        context.AddMember("contract_stage_index", static_cast<unsigned>(i), context.GetAllocator());
        Set(context, "source", i == 0 ? stmt.source : "temporary", context.GetAllocator());
        if (contracts::CheckInvocation(querier_, &invocation, dynamic_cast<IOperatorConfigAdapterV1*>(ops[i]),
                                       parameters, input_snapshots, context, &contract_error)) {
            if (invocation.check.IsObject())
                checks.PushBack(Copy(invocation.check, checks.GetAllocator()), checks.GetAllocator());
            return fail_contract(contract_error);
        }
        checks.PushBack(Copy(invocation.check, checks.GetAllocator()), checks.GetAllocator());
        if (contract_checks) *contract_checks = Encode(checks);
        std::shared_ptr<DataFrameChannel> stage_out;
        IChannel* actual_sink = nullptr;
        if (i + 1 == ops.size()) {
            if (sink_type == ChannelType::kDatabase) {
                stage_out = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
                stage_out->Open();
                actual_sink = stage_out.get();
            } else {
                actual_sink = sink;
            }
        } else {
            stage_out = std::make_shared<DataFrameChannel>("_pipe", std::to_string(++tmp_channel_seq_));
            stage_out->Open();
            actual_sink = stage_out.get();
        }

        const auto execution_inputs = frozen_inputs.empty() ? stage_inputs : Span<IChannel*>(frozen_inputs);
        if (ops[i]->Work(execution_inputs, actual_sink) != 0) {
            if (error && error->empty()) {
                std::string op_error = ops[i]->LastError();
                if (!op_error.empty()) {
                    *error = op_error;
                } else {
                    *error = "operator " + ops[i]->Category() + "." + ops[i]->Name() + " execution failed";
                }
            }
            return -1;
        }

        if (invocation.component.contract_state == "declared") {
            auto* observed = dynamic_cast<IDataFrameChannel*>(actual_sink);
            DataFrame frame;
            auto schema =
                observed && observed->Read(&frame) == 0 && frame.ToArrow() ? frame.ToArrow()->schema() : nullptr;
            if (contracts::ObservePrimary(querier_, &invocation, schema, schema ? frame.RowCount() : -1,
                                          dynamic_cast<IOperatorOutputObservationV1*>(ops[i]), "complete",
                                          &contract_error)) {
                checks[static_cast<rapidjson::SizeType>(i)].CopyFrom(invocation.check, checks.GetAllocator());
                return fail_contract(contract_error);
            }
            checks[static_cast<rapidjson::SizeType>(i)].CopyFrom(invocation.check, checks.GetAllocator());
            if (contract_checks) *contract_checks = Encode(checks);
        }
        if (stage_out) {
            stage_buffers.push_back(stage_out);
            stage_input_holder.clear();
            stage_input_holder.push_back(stage_out.get());
            stage_inputs = Span<IChannel*>(stage_input_holder);
        }
    }

    if (sink_type == ChannelType::kDatabase) {
        auto* db_sink = dynamic_cast<IDatabaseChannel*>(sink);
        if (!db_sink) return -1;
        if (stage_buffers.empty()) return -1;

        std::string table = ExtractTableName(stmt.dest);
        int64_t written_rows =
            ChannelAdapter::WriteFromDataFrame(stage_buffers.back().get(), db_sink, table.c_str(), error);
        if (written_rows < 0) return -1;

        if (rows_affected) *rows_affected = written_rows;
    }

    return 0;
}

std::string SchedulerPlugin::QueryStreamChannelRole(const std::string& type, const std::string& name) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    if (!stream_manager) return "both";
    std::string role = "both";
    const std::string expect_type = ToLowerAscii(type);
    stream_manager->QueryChannels(
        [&](const std::string& ch_type, const std::string& ch_name, const std::string& option, const std::string&) {
            if (ToLowerAscii(ch_type) != expect_type || ch_name != name) return;
            role = ReadRoleFromOption(option);
        });
    return role;
}

int32_t SchedulerPlugin::ResolveSourceBindings(const SqlStatement& stmt, SourceResolveResult* out,
                                               std::string* err_rsp) {
    if (!out) {
        if (err_rsp) *err_rsp = BuildErrorJson("source resolve target is null");
        return error::INTERNAL_ERROR;
    }
    out->channels.clear();
    out->channel_holders.clear();
    out->stream_channels.clear();
    out->block_channels.clear();
    out->source_keys.clear();
    out->resolved_sources.clear();
    out->source_expand_rule = "explicit";
    out->has_stream_source = false;
    out->has_non_stream_source = false;
    out->has_block_source = false;
    if (err_rsp) err_rsp->clear();

    auto fail = [&](int32_t status, const std::string& message,
                    ErrorCodeId error_code = ErrorCodeId::kUnknown) -> int32_t {
        if (err_rsp) {
            if (error_code == ErrorCodeId::kUnknown) {
                *err_rsp = BuildErrorJson(message);
            } else {
                *err_rsp = BuildExecutionErrorJson(message, error_code, ErrorStageId::kSourceResolve);
            }
        }
        return status;
    };

    if (stmt.sources.empty()) {
        return fail(error::BAD_REQUEST, "source channel not found");
    }

    // Kafka control definitions exist before a data adapter is installed. Resolve role and
    // capability through the catalog IID, without opening credentials or a network client.
    auto kafka_binding = [&](const std::string& reference, const char* usage) -> int32_t {
        if (reference.rfind("kafka.", 0) != 0) return error::OK;
        const std::string stage = std::string(usage) == "source" ? "source_resolve" : "sink_resolve";
        auto reject = [&](int32_t status, const char* code) {
            if (err_rsp) *err_rsp = BuildExecutionErrorJson(code, code, stage);
            return status;
        };
        auto* catalog =
            querier_ ? static_cast<IKafkaChannelCatalogV1*>(querier_->First(IID_KAFKA_CHANNEL_CATALOG_V1)) : nullptr;
        if (!catalog) return reject(error::UNAVAILABLE, "KAFKA_PROVIDER_UNAVAILABLE");
        KafkaChannelDescriptionV1 description;
        int rc = catalog->DescribeChannel(reference.substr(6).c_str(), &description);
        if (rc == ENOENT) return reject(error::NOT_FOUND, "KAFKA_CHANNEL_NOT_FOUND");
        if (rc != 0) return reject(error::UNAVAILABLE, "KAFKA_PROVIDER_UNAVAILABLE");
        if (description.role != "both" && description.role != usage)
            return reject(error::BAD_REQUEST, "KAFKA_ROLE_MISMATCH");
        if (!(std::string(usage) == "source" ? description.source_adapter_available
                                             : description.sink_adapter_available))
            return reject(error::UNAVAILABLE, "KAFKA_DATA_ADAPTER_UNAVAILABLE");
        return error::OK;
    };
    for (const auto& reference : stmt.sources) {
        auto rc = kafka_binding(reference, "source");
        if (rc != error::OK) return rc;
    }
    auto kafka_sink_rc = kafka_binding(stmt.dest, "sink");
    if (kafka_sink_rc != error::OK) return kafka_sink_rc;

    for (const auto& source_ref : stmt.sources) {
        ParsedChannelRef ref;
        std::string parse_err;
        if (!ParseChannelRef(source_ref, &ref, &parse_err)) {
            return fail(error::BAD_REQUEST, parse_err, ErrorCodeId::kStreamHubSelectorInvalid);
        }

        std::shared_ptr<IChannel> source_owner;
        bool source_ambiguous = false;
        IChannel* source_ch = nullptr;
        std::string database_category, database_name, database_relation;
        const bool database_ref =
            ParseDatabaseDestination(ref.base, &database_category, &database_name, &database_relation) &&
            (database_category == "sqlite" || database_category == "mysql" || database_category == "postgres" ||
             database_category == "clickhouse");
        auto* database_leases =
            database_ref && querier_
                ? static_cast<IDatabaseChannelLeaseProviderV1*>(querier_->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1))
                : nullptr;
        if (database_leases) {
            source_owner = database_leases->AcquireChannel(database_category.c_str(), database_name.c_str());
            source_ch = source_owner.get();
        } else
            source_ch = FindChannel(ref.base, &source_owner, &source_ambiguous);
        if (source_ambiguous) {
            return fail(error::CONFLICT, "multiple block stream sources matched: " + ref.base);
        }
        if (!source_ch) {
            return fail(IsDataframeRefName(ref.base) ? error::NOT_FOUND : error::BAD_REQUEST,
                        "source channel not found: " + ref.base);
        }
        if (source_owner) {
            out->channel_holders.push_back(source_owner);
        }

        const std::string source_type = source_ch->Type() ? source_ch->Type() : "";
        if (source_type == ChannelType::kBlockStream) {
            if (ref.has_selector) {
                return fail(error::BAD_REQUEST, "block stream source does not support selector",
                            ErrorCodeId::kStreamHubSelectorInvalid);
            }
            auto* block_ch = dynamic_cast<IBlockStreamChannel*>(source_ch);
            if (!block_ch)
                return fail(error::BAD_REQUEST, "source channel cast to IBlockStreamChannel failed: " + ref.base);
            out->has_block_source = true;
            out->channels.push_back(source_ch);
            out->block_channels.emplace_back(block_ch, [](IBlockStreamChannel*) {});
            out->source_keys.push_back(std::string(block_ch->Category()) + "." + block_ch->Name());
            out->resolved_sources.push_back(ref.base);
            continue;
        }

        if (source_type != ChannelType::kStream) {
            if (ref.has_selector) {
                return fail(error::BAD_REQUEST,
                            "channel selector is only supported on stream_hub source: " + source_ref,
                            ErrorCodeId::kStreamHubSelectorInvalid);
            }
            out->has_non_stream_source = true;
            out->channels.push_back(source_ch);
            out->resolved_sources.push_back(ref.base);
            continue;
        }

        out->has_stream_source = true;
        auto* stream_ch = dynamic_cast<IStreamChannel*>(source_ch);
        if (!stream_ch) {
            return fail(error::BAD_REQUEST, "source channel cast to IStreamChannel failed: " + ref.base);
        }
        const std::string source_role = QueryStreamChannelRole(stream_ch->Category(), stream_ch->Name());
        if (!IsSourceRoleAllowed(source_role)) {
            return fail(error::BAD_REQUEST,
                        "stream channel role does not allow source: " + std::string(stream_ch->Category()) + "." +
                            stream_ch->Name(),
                        ErrorCodeId::kStreamChannelRoleMismatch);
        }

        if (stream_ch->IsHubChannel()) {
            if (IEquals(stream_ch->HubModeHint() ? stream_ch->HubModeHint() : "", "merge")) {
                if (ref.has_selector) {
                    return fail(error::BAD_REQUEST, "stream_hub(merge) does not allow selector: " + source_ref,
                                ErrorCodeId::kStreamHubSelectorNotAllowedMerge);
                }
                out->channels.push_back(stream_ch);
                out->stream_channels.push_back(MakeStreamOwner(stream_ch, source_owner));
                out->source_keys.push_back(MakeStreamChannelKey(stream_ch->Category(), stream_ch->Name()));
                out->resolved_sources.push_back(ref.base);
                continue;
            }

            const size_t partition_count = stream_ch->HubPartitionCount();
            if (partition_count == 0) {
                return fail(error::BAD_REQUEST, "stream_hub(split) has no derived partitions: " + ref.base,
                            ErrorCodeId::kStreamHubSelectorInvalid);
            }
            if (partition_count > max_resolved_sources_) {
                return fail(error::BAD_REQUEST,
                            "resolved sources exceed max_resolved_sources: " + std::to_string(partition_count) + " > " +
                                std::to_string(max_resolved_sources_),
                            ErrorCodeId::kStreamHubSelectorOutOfRange);
            }

            if (!ref.has_selector || ref.wildcard_selector) {
                if (!ref.has_selector) {
                    out->source_expand_rule = "auto_wildcard";
                }
                for (size_t i = 0; i < partition_count; ++i) {
                    auto partition = stream_ch->HubPartition(i);
                    if (!partition) {
                        return fail(error::BAD_REQUEST, "stream_hub partition resolve failed: " + ref.base,
                                    ErrorCodeId::kStreamHubSelectorInvalid);
                    }
                    out->channels.push_back(partition.get());
                    out->stream_channels.push_back(partition);
                    out->source_keys.push_back(MakeStreamChannelKey(partition->Category(), partition->Name()));
                    out->resolved_sources.push_back(ref.base + "[" + std::to_string(i) + "]");
                }
                continue;
            }

            const int idx = ref.selector_index;
            if (idx < 0 || static_cast<size_t>(idx) >= partition_count) {
                return fail(error::BAD_REQUEST, "stream_hub selector out of range: " + source_ref,
                            ErrorCodeId::kStreamHubSelectorOutOfRange);
            }
            auto partition = stream_ch->HubPartition(static_cast<size_t>(idx));
            if (!partition) {
                return fail(error::BAD_REQUEST, "stream_hub partition resolve failed: " + source_ref,
                            ErrorCodeId::kStreamHubSelectorInvalid);
            }
            out->channels.push_back(partition.get());
            out->stream_channels.push_back(partition);
            out->source_keys.push_back(MakeStreamChannelKey(partition->Category(), partition->Name()));
            out->resolved_sources.push_back(ref.base + "[" + std::to_string(idx) + "]");
            continue;
        }

        if (ref.has_selector) {
            return fail(error::BAD_REQUEST, "channel selector is only supported on stream_hub source: " + source_ref,
                        ErrorCodeId::kStreamHubSelectorInvalid);
        }
        out->channels.push_back(stream_ch);
        out->stream_channels.push_back(MakeStreamOwner(stream_ch, source_owner));
        out->source_keys.push_back(MakeStreamChannelKey(stream_ch->Category(), stream_ch->Name()));
        out->resolved_sources.push_back(ref.base);
    }

    return error::OK;
}

int32_t SchedulerPlugin::ResolveStreamSink(const SqlStatement& stmt, SinkBinding* binding, std::string* err_out) {
    if (!binding) {
        if (err_out) *err_out = "invalid sink binding target";
        return error::BAD_REQUEST;
    }
    binding->sink_channel.reset();
    binding->sink_type.clear();
    binding->db_type.clear();
    binding->db_name.clear();
    binding->table_name.clear();

    if (stmt.dest.empty()) {
        if (err_out) *err_out = "stream task requires INTO destination";
        return error::BAD_REQUEST;
    }

    ParsedChannelRef dest_ref;
    std::string dest_parse_err;
    if (!ParseChannelRef(stmt.dest, &dest_ref, &dest_parse_err)) {
        if (err_out) *err_out = dest_parse_err;
        return error::BAD_REQUEST;
    }
    if (dest_ref.has_selector && IsStreamRefName(dest_ref.base)) {
        if (err_out) {
            *err_out =
                BuildExecutionErrorJson("INTO stream selector is not allowed: " + stmt.dest,
                                        ErrorCodeId::kStreamHubSelectorNotAllowedInto, ErrorStageId::kSinkResolve);
        }
        return error::BAD_REQUEST;
    }

    if (IsStreamRefName(dest_ref.base)) {
        std::shared_ptr<IChannel> sink_owner;
        IChannel* sink_raw = FindChannel(dest_ref.base, &sink_owner);
        auto* matched = dynamic_cast<IStreamChannel*>(sink_raw);
        if (!matched) {
            if (err_out) *err_out = "stream sink not found: " + dest_ref.base;
            return error::NOT_FOUND;
        }
        const std::string sink_role = QueryStreamChannelRole(matched->Category(), matched->Name());
        if (!IsSinkRoleAllowed(sink_role)) {
            if (err_out) {
                *err_out = BuildExecutionErrorJson("stream channel role does not allow sink: " +
                                                       std::string(matched->Category()) + "." + matched->Name(),
                                                   ErrorCodeId::kStreamChannelRoleMismatch, ErrorStageId::kSinkResolve);
            }
            return error::BAD_REQUEST;
        }

        auto output = MakeStreamOwner(matched, sink_owner);
        if (output && !output->IsOpened()) {
            (void)output->Open();
        }
        binding->sink_channel = std::static_pointer_cast<IChannel>(output);
        binding->sink_type = ChannelType::kStream;
        return error::OK;
    }

    if (dest_ref.has_selector) {
        if (err_out) *err_out = "selector is only supported for stream source";
        return error::BAD_REQUEST;
    }

    if (IsDataframeRefName(dest_ref.base)) {
        auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
        if (!ch_registry) {
            if (err_out) *err_out = "channel registry unavailable";
            return error::UNAVAILABLE;
        }

        const std::string df_name = DataframeNamePart(dest_ref.base);
        std::shared_ptr<IChannel> df_holder = ch_registry->Get(df_name.c_str());
        if (!df_holder) {
            auto created = std::make_shared<DataFrameChannel>("dataframe", df_name);
            (void)created->Open();
            if (ch_registry->Register(df_name.c_str(), std::static_pointer_cast<IChannel>(created)) != 0) {
                // 处理并发注册：重查一次
                df_holder = ch_registry->Get(df_name.c_str());
                if (!df_holder) {
                    if (err_out) *err_out = "register dataframe sink failed: " + dest_ref.base;
                    return error::INTERNAL_ERROR;
                }
            } else {
                df_holder = std::static_pointer_cast<IChannel>(created);
            }
        }

        auto appendable = std::dynamic_pointer_cast<IAppendableDataFrameChannel>(df_holder);
        if (!appendable) {
            if (err_out) *err_out = "dataframe sink is not appendable: " + dest_ref.base;
            return error::BAD_REQUEST;
        }
        if (!appendable->IsOpened()) {
            (void)appendable->Open();
        }

        binding->sink_channel = std::static_pointer_cast<IChannel>(appendable);
        binding->sink_type = ChannelType::kDataFrame;
        return error::OK;
    }

    std::string db_type;
    std::string db_name;
    std::string table_from_dest;
    if (!ParseDatabaseDestination(dest_ref.base, &db_type, &db_name, &table_from_dest)) {
        if (err_out)
            *err_out = "invalid INTO destination: " + dest_ref.base +
                       ", expected stream.<name>, dataframe.<name>, or <db_type>.<db_name>[.<table>]";
        return error::BAD_REQUEST;
    }

    std::shared_ptr<IChannel> existing_owner;
    if (IChannel* existing = FindChannel(dest_ref.base, &existing_owner); existing) {
        if (!existing->IsOpened()) {
            (void)existing->Open();
        }
        binding->sink_channel = existing_owner ? existing_owner : MakeNonOwningChannelHolder(existing);
        binding->sink_type = existing->Type() ? existing->Type() : "";
        if (binding->sink_type == ChannelType::kDatabase) {
            binding->db_type = db_type;
            binding->db_name = db_name;
            binding->table_name = table_from_dest;
        }
        return error::OK;
    }

    auto* db_factory = querier_ ? static_cast<IDatabaseFactory*>(querier_->First(IID_DATABASE_FACTORY)) : nullptr;
    if (!db_factory) {
        if (err_out) *err_out = "database factory unavailable";
        return error::UNAVAILABLE;
    }

    IDatabaseChannel* db_raw = db_factory->Get(db_type.c_str(), db_name.c_str());
    if (!db_raw) {
        if (err_out) *err_out = "database channel not found: " + db_type + "." + db_name;
        return error::NOT_FOUND;
    }

    auto db_sink = std::shared_ptr<IDatabaseChannel>(db_raw, [](IDatabaseChannel*) {});
    if (!db_sink->IsOpened()) {
        (void)db_sink->Open();
    }

    binding->sink_channel = std::static_pointer_cast<IChannel>(db_sink);
    binding->sink_type = ChannelType::kDatabase;
    binding->db_type = db_type;
    binding->db_name = db_name;
    binding->table_name = table_from_dest;
    return error::OK;
}

int32_t SchedulerPlugin::BuildStreamExecutionPlan(const SqlStatement& stmt, const std::string& lease_owner_id,
                                                  bool skip_lease_acquire, StreamExecutionPlan* plan,
                                                  std::string* err_rsp) {
    if (!plan || !err_rsp) return error::INTERNAL_ERROR;
    err_rsp->clear();

    if (!querier_) {
        *err_rsp = BuildErrorJson("querier not initialized");
        return error::INTERNAL_ERROR;
    }
    if (stmt.dest.empty()) {
        *err_rsp = BuildErrorJson("stream task requires INTO destination");
        return error::BAD_REQUEST;
    }
    if (!IsQualifiedDestination(stmt.dest)) {
        *err_rsp = BuildErrorJson("invalid INTO destination: " + stmt.dest);
        return error::BAD_REQUEST;
    }

    std::vector<OperatorRef> parsed_ops = stmt.operators;
    if (parsed_ops.empty() && !stmt.op_category.empty() && !stmt.op_name.empty()) {
        parsed_ops.push_back({stmt.op_category, stmt.op_name});
    }
    // 仅 group 节点（skip_lease_acquire=true）允许省略 USING：
    // 隐式注入 builtin.passthrough_stream 作为默认算子。
    // single stream 入口保持原有约束：必须显式 USING stream operator。
    const bool implicit_passthrough = parsed_ops.empty() && skip_lease_acquire;
    if (implicit_passthrough) {
        parsed_ops.push_back({"builtin", "passthrough_stream"});
    }
    if (parsed_ops.empty()) {
        *err_rsp = BuildErrorJson("stream task requires USING stream operator");
        return error::BAD_REQUEST;
    }
    if (parsed_ops.size() != 1) {
        *err_rsp = BuildErrorJson("stream task currently supports single USING operator");
        return error::BAD_REQUEST;
    }
    const OperatorRef& op_ref = parsed_ops[0];

    if (!implicit_passthrough) {
        auto* catalog = static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG));
        if (!catalog) {
            *err_rsp = BuildErrorJson("operator catalog unavailable");
            return error::UNAVAILABLE;
        }
        const OperatorStatus status = catalog->QueryStatus(op_ref.category, op_ref.name);
        if (status == OperatorStatus::kNotFound) {
            *err_rsp = BuildErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
            return error::NOT_FOUND;
        }
        if (status == OperatorStatus::kDeactivated) {
            *err_rsp = BuildErrorJson("operator is deactivated: " + op_ref.category + "." + op_ref.name);
            return error::CONFLICT;
        }
    }

    SourceResolveResult source_resolved;
    std::string source_err_rsp;
    const int32_t source_rc = ResolveSourceBindings(stmt, &source_resolved, &source_err_rsp);
    if (source_rc != error::OK) {
        *err_rsp = source_err_rsp.empty() ? BuildErrorJson("source resolve failed") : source_err_rsp;
        return source_rc;
    }
    if (!source_resolved.has_stream_source || source_resolved.has_non_stream_source) {
        *err_rsp = BuildErrorJson("stream task requires stream source only");
        return error::BAD_REQUEST;
    }
    if (source_resolved.stream_channels.empty()) {
        *err_rsp = BuildErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }

    const std::unordered_map<std::string, std::string> with_params =
        !stmt.operator_with_params.empty() ? stmt.operator_with_params[0] : stmt.with_params;
    if (with_params.find("sink_table") != with_params.end()) {
        *err_rsp = BuildErrorJson("sink_table is not supported for stream tasks; use INTO <db_type>.<db_name>.<table>");
        return error::BAD_REQUEST;
    }

    SinkBinding sink_binding;
    std::string sink_error;
    const int32_t sink_rc = ResolveStreamSink(stmt, &sink_binding, &sink_error);
    if (sink_rc != error::OK) {
        if (!sink_error.empty() && sink_error.front() == '{') {
            *err_rsp = sink_error;
        } else {
            *err_rsp = BuildErrorJson(sink_error);
        }
        return sink_rc;
    }
    if (!sink_binding.sink_channel) {
        *err_rsp = BuildErrorJson("resolve stream sink failed: output channel is null");
        return error::INTERNAL_ERROR;
    }

    plan->stmt = stmt;
    plan->runtime_task_id = NextStreamTaskId();
    plan->lease_owner_id = lease_owner_id;
    plan->skip_lease_acquire = skip_lease_acquire;
    plan->parsed_ops = std::move(parsed_ops);
    plan->with_params = with_params;

    plan->source_channel_holders = source_resolved.channel_holders;
    plan->source_channels = source_resolved.stream_channels;
    plan->source_keys = source_resolved.source_keys;
    plan->resolved_sources = source_resolved.resolved_sources;
    plan->source_expand_rule = source_resolved.source_expand_rule;
    plan->shared_hub_key = CanonicalSharedHubKey(plan->source_keys);
    plan->where_signature = TrimAsciiSpace(stmt.where_clause);
    if (plan->lease_owner_id.empty() && !plan->shared_hub_key.empty()) {
        plan->lease_owner_id = plan->shared_hub_key;
    }

    plan->sink_channel = sink_binding.sink_channel;
    plan->sink_type = sink_binding.sink_type;
    plan->db_type = sink_binding.db_type;
    plan->db_name = sink_binding.db_name;
    plan->table_name = sink_binding.table_name;
    plan->sink_ctx.sink_channel = plan->sink_channel.get();
    plan->sink_ctx.sink_type = plan->sink_type;
    plan->sink_ctx.into_raw = stmt.dest;
    plan->sink_ctx.db_type = plan->db_type;
    plan->sink_ctx.db_name = plan->db_name;
    plan->sink_ctx.table_name = plan->table_name;

    plan->sink_keys.clear();
    if (plan->sink_type == ChannelType::kStream) {
        auto* sink_stream = dynamic_cast<IStreamChannel*>(plan->sink_channel.get());
        if (sink_stream) {
            plan->sink_keys.push_back(MakeStreamChannelKey(sink_stream->Category(), sink_stream->Name()));
        }
    }
    return error::OK;
}

int32_t SchedulerPlugin::ValidateStreamExecutionPlan(StreamExecutionPlan* plan, std::string* err_rsp) {
    // 逻辑链：
    // 1) 基于 source 列表构建单源或 fan-in 读模型；
    // 2) 创建并校验首个 stream 算子实例，读取并行策略与并行度；
    // 3) 汇总 source/sink capabilities，按策略校验并发能力匹配；
    // 4) 在校验阶段提前拒绝能力不匹配的执行计划，避免运行期失败。
    if (!plan || !err_rsp) return error::INTERNAL_ERROR;
    err_rsp->clear();

    std::shared_ptr<IStreamChannel> source = plan->source_channels[0];
    if (plan->source_channels.size() > 1) {
        for (size_t i = 0; i < plan->source_channels.size(); ++i) {
            const auto& sc = plan->source_channels[i];
            const StreamChannelCapabilities caps = sc ? sc->Capabilities() : StreamChannelCapabilities{};
            if (!sc || !caps.semantics.supports_timeout_poll || !caps.concurrency.lock_free_poll) {
                const std::string src_name =
                    sc ? (std::string(sc->Category()) + "." + sc->Name()) : plan->source_keys[i];
                *err_rsp = BuildExecutionErrorJson("stream fanin capability mismatch: source=" + src_name +
                                                       ", reason=source must support timeout poll and lock-free poll",
                                                   ErrorCodeId::kStreamFaninCapabilityMismatch, ErrorStageId::kFanin);
                return error::BAD_REQUEST;
            }
        }
        source = std::make_shared<FanInStreamChannel>("fanin", plan->runtime_task_id + ".fanin", plan->source_channels);
    }
    plan->source = source;

    const OperatorRef& op_ref = plan->parsed_ops[0];
    plan->first_operator_holder = CreateOperator(op_ref.category, op_ref.name);
    if (!plan->first_operator_holder) {
        *err_rsp = BuildExecutionErrorJson("operator create failed: " + op_ref.category + "." + op_ref.name,
                                           ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
        return error::NOT_FOUND;
    }
    plan->first_stream_operator = std::dynamic_pointer_cast<IStreamOperator>(plan->first_operator_holder);
    if (!plan->first_stream_operator) {
        *err_rsp = BuildExecutionErrorJson("operator is not stream operator: " + op_ref.category + "." + op_ref.name,
                                           ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
        return error::BAD_REQUEST;
    }

    plan->strategy = plan->first_stream_operator->GetParallelStrategy();
    plan->parallelism = std::max(1, plan->first_stream_operator->GetParallelism());
    if (plan->strategy == ParallelStrategy::NONE) {
        plan->parallelism = 1;
    }
    if (plan->parallelism < 1) plan->parallelism = 1;

    plan->source_caps = plan->source->Capabilities();
    plan->sink_caps = StreamChannelCapabilities{};
    if (plan->sink_type == ChannelType::kStream) {
        auto* sink_stream = dynamic_cast<IStreamChannel*>(plan->sink_channel.get());
        if (!sink_stream) {
            *err_rsp =
                BuildExecutionErrorJson("stream sink cast to IStreamChannel failed",
                                        ErrorCodeId::kStreamSinkCapabilityMismatch, ErrorStageId::kCapabilityCheck);
            return error::BAD_REQUEST;
        }
        plan->sink_caps = sink_stream->Capabilities();
    } else {
        plan->sink_caps.channel_type = plan->sink_type;
        plan->sink_caps.concurrency.put_mode = ProducerMode::SINGLE;
        plan->sink_caps.concurrency.poll_mode = ConsumerMode::SINGLE;
        plan->sink_caps.concurrency.max_producers = 1;
        plan->sink_caps.concurrency.max_consumers = 1;
        plan->sink_caps.concurrency.lock_free_put = false;
        plan->sink_caps.concurrency.lock_free_poll = false;
        plan->sink_caps.concurrency.cancel_wakeup_guaranteed = false;
    }

    if (plan->strategy == ParallelStrategy::STATELESS && plan->parallelism > 1) {
        const bool poll_mode_ok = plan->source_caps.concurrency.poll_mode == ConsumerMode::MULTI;
        const bool consumers_ok =
            plan->source_caps.concurrency.max_consumers == 0 ||
            plan->source_caps.concurrency.max_consumers >= static_cast<uint32_t>(plan->parallelism);
        if (!poll_mode_ok || !consumers_ok) {
            *err_rsp = BuildCapabilityMismatchJson(
                "stream source capability mismatch: strategy=STATELESS, parallelism=" +
                    std::to_string(plan->parallelism) + ", required.poll_mode=MULTI, actual.poll_mode=" +
                    std::string(StreamConsumerModeName(plan->source_caps.concurrency.poll_mode)) +
                    ", actual.max_consumers=" + std::to_string(plan->source_caps.concurrency.max_consumers),
                ErrorCodeId::kStreamSourceCapabilityMismatch, &plan->source_caps, &plan->sink_caps);
            return error::BAD_REQUEST;
        }
    }

    if (plan->parallelism > 1) {
        const bool put_mode_ok = plan->sink_caps.concurrency.put_mode == ProducerMode::MULTI;
        const bool producers_ok = plan->sink_caps.concurrency.max_producers == 0 ||
                                  plan->sink_caps.concurrency.max_producers >= static_cast<uint32_t>(plan->parallelism);
        if (!put_mode_ok || !producers_ok) {
            *err_rsp = BuildCapabilityMismatchJson(
                "stream sink capability mismatch: strategy=" + std::to_string(static_cast<int>(plan->strategy)) +
                    ", parallelism=" + std::to_string(plan->parallelism) +
                    ", required.put_mode=MULTI, actual.put_mode=" +
                    std::string(StreamProducerModeName(plan->sink_caps.concurrency.put_mode)) +
                    ", actual.max_producers=" + std::to_string(plan->sink_caps.concurrency.max_producers),
                ErrorCodeId::kStreamSinkCapabilityMismatch, &plan->source_caps, &plan->sink_caps);
            return error::BAD_REQUEST;
        }
    }
    return error::OK;
}

int32_t SchedulerPlugin::AcquireStreamExecutionLease(StreamExecutionPlan* plan, LeaseToken* lease_token,
                                                     std::string* err_rsp) {
    if (!plan || !lease_token || !err_rsp) return error::INTERNAL_ERROR;
    err_rsp->clear();
    if (plan->skip_lease_acquire) return error::OK;

    SweepFinishedTaskLeases();

    plan->lease_keys.clear();
    plan->lease_keys.reserve(plan->source_keys.size() + plan->sink_keys.size());
    plan->lease_keys.insert(plan->lease_keys.end(), plan->source_keys.begin(), plan->source_keys.end());
    plan->lease_keys.insert(plan->lease_keys.end(), plan->sink_keys.begin(), plan->sink_keys.end());

    plan->version_snapshot.clear();
    CaptureStreamChannelVersionSnapshot(plan->lease_keys, &plan->version_snapshot);

    std::string conflict_key;
    std::string version_conflict_key;
    bool blocked_by_mutation = false;
    const int lease_rc = TryAcquireStreamTaskLeases(plan->runtime_task_id, plan->source_keys, plan->sink_keys,
                                                    &conflict_key, &blocked_by_mutation, plan->lease_owner_id,
                                                    &plan->version_snapshot, &version_conflict_key);
    if (lease_rc != 0) {
        if (lease_rc == EBUSY) {
            if (blocked_by_mutation) {
                *err_rsp = BuildExecutionErrorJson("stream channel is being modified: " + conflict_key,
                                                   ErrorCodeId::kStreamChannelMutating, ErrorStageId::kLease);
                return error::CONFLICT;
            }
            *err_rsp = BuildExecutionErrorJson("stream source is in use: " + conflict_key,
                                               ErrorCodeId::kStreamSourceInUse, ErrorStageId::kLease);
            return error::CONFLICT;
        }
        if (lease_rc == EAGAIN) {
            *err_rsp = BuildExecutionErrorJson("stream channel changed during execute prepare: " + version_conflict_key,
                                               ErrorCodeId::kStreamChannelVersionChanged, ErrorStageId::kLease);
            return error::CONFLICT;
        }
        *err_rsp = BuildExecutionErrorJson("stream channel lease acquire failed", ErrorCodeId::kStreamLeaseFailed,
                                           ErrorStageId::kLease);
        return MapStreamManagerErrorToStatus(lease_rc);
    }

    *lease_token =
        LeaseToken([this, runtime_task_id = plan->runtime_task_id]() { ReleaseStreamTaskLeases(runtime_task_id); });
    return error::OK;
}

int32_t SchedulerPlugin::ExecuteStreamTask(const SqlStatement& stmt, std::string& rsp,
                                           const std::string& lease_owner_id, bool skip_lease_acquire) {
    StreamExecutionPlan plan;
    std::string err_rsp;
    int32_t rc = BuildStreamExecutionPlan(stmt, lease_owner_id, skip_lease_acquire, &plan, &err_rsp);
    if (rc != error::OK) {
        rsp = EnsureExecutionErrorJson(err_rsp, "build stream execution plan failed", ErrorCodeId::kSqlTextInvalid,
                                       ErrorStageId::kParse);
        return rc;
    }

    rc = ValidateStreamExecutionPlan(&plan, &err_rsp);
    if (rc != error::OK) {
        rsp = EnsureExecutionErrorJson(err_rsp, "validate stream execution plan failed", ErrorCodeId::kOpExecFail,
                                       ErrorStageId::kCapabilityCheck);
        return rc;
    }

    LeaseToken lease_token;
    rc = AcquireStreamExecutionLease(&plan, &lease_token, &err_rsp);
    if (rc != error::OK) {
        rsp = EnsureExecutionErrorJson(err_rsp, "acquire stream lease failed", ErrorCodeId::kStreamLeaseFailed,
                                       ErrorStageId::kLease);
        return rc;
    }

    std::shared_ptr<IStreamChannel> source = plan.source;
    std::shared_ptr<IChannel> output = plan.sink_channel;
    if (!source || !output) {
        rsp = BuildExecutionErrorJson("stream execution plan source/sink is null", ErrorCodeId::kOpExecFail,
                                      ErrorStageId::kExecute);
        return error::INTERNAL_ERROR;
    }

    bool cleanup_shared_subscription = false;
    auto shared_subscription_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [this, runtime_task_id = plan.runtime_task_id, &cleanup_shared_subscription](void*) {
            if (cleanup_shared_subscription) {
                ReleaseRuntimeSubscriptions(runtime_task_id);
            }
        });

    std::shared_ptr<IStreamChannel> source_override;
    rc = AcquireSharedSourceSubscription(&plan, &source_override, &err_rsp);
    if (rc != error::OK) {
        rsp = EnsureExecutionErrorJson(err_rsp, "acquire shared source subscription failed",
                                       ErrorCodeId::kSharedSourceSubscribeFailed, ErrorStageId::kSourceResolve);
        return rc;
    }
    const bool using_shared_source = (source_override != nullptr);
    if (using_shared_source) {
        source = source_override;
        cleanup_shared_subscription = true;
    }

    if (!plan.stmt.where_clause.empty() && !using_shared_source) {
        std::vector<std::string> unsupported;
        const int filter_rc = source->SetFilter(plan.stmt.where_clause.c_str(), &unsupported);
        if (filter_rc != 0) {
            rsp = BuildExecutionErrorJson("stream source SetFilter failed", ErrorCodeId::kOpExecFail,
                                          ErrorStageId::kExecute);
            return error::BAD_REQUEST;
        }
        if (!unsupported.empty()) {
            std::ostringstream oss;
            oss << "WHERE pushdown not fully supported:";
            for (size_t i = 0; i < unsupported.size(); ++i) {
                oss << (i == 0 ? " " : ", ") << unsupported[i];
            }
            rsp = BuildExecutionErrorJson(oss.str(), ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
            return error::BAD_REQUEST;
        }
    }

    std::shared_ptr<FanOutStreamChannel> fanout;
    std::shared_ptr<SharedSourceState> shared_source_state;
    std::vector<std::shared_ptr<IStreamChannel>> input_ports;
    input_ports.reserve(static_cast<size_t>(plan.parallelism));
    std::shared_ptr<IStreamChannel> open_target = source;

    if (plan.strategy == ParallelStrategy::NONE) {
        input_ports.push_back(source);
    } else if (plan.strategy == ParallelStrategy::STATELESS) {
        shared_source_state = std::make_shared<SharedSourceState>(source);
        for (int i = 0; i < plan.parallelism; ++i) {
            input_ports.push_back(std::make_shared<StatelessSourceView>(shared_source_state, static_cast<uint32_t>(i)));
        }
    } else if (plan.strategy == ParallelStrategy::KEYED) {
        RingStreamChannelOptions partition_opts;
        const size_t source_cap = source->Capacity();
        if (source_cap > 0) {
            partition_opts.ring_size = NextPowerOfTwo(std::max<size_t>(64, source_cap));
        }
        fanout = std::make_shared<FanOutStreamChannel>(
            "fanout", plan.runtime_task_id + ".fanout", source, static_cast<size_t>(plan.parallelism),
            FanOutMode::ROUTE_BY_PARTITION_ID, plan.first_stream_operator->GetPartitionSpec(), partition_opts);
        open_target = fanout;
        for (int i = 0; i < plan.parallelism; ++i) {
            auto part = fanout->GetPartition(static_cast<size_t>(i));
            if (!part) {
                rsp = BuildExecutionErrorJson("fanout partition create failed", ErrorCodeId::kOpExecFail,
                                              ErrorStageId::kExecute);
                return error::INTERNAL_ERROR;
            }
            input_ports.push_back(std::make_shared<FanOutPartitionView>(fanout, part));
        }
    } else {
        rsp = BuildExecutionErrorJson("unsupported stream parallel strategy", ErrorCodeId::kOpExecFail,
                                      ErrorStageId::kExecute);
        return error::BAD_REQUEST;
    }

    if (input_ports.empty()) {
        rsp = BuildExecutionErrorJson("stream input ports build failed", ErrorCodeId::kOpExecFail,
                                      ErrorStageId::kExecute);
        return error::INTERNAL_ERROR;
    }

    const std::string with_params_json = MakeWithParamsJson(plan.with_params);
    const std::shared_ptr<arrow::Schema> static_schema = source->GetOutputSchema();
    auto task = std::make_shared<StreamTask>(plan.runtime_task_id, &stream_runtime_);

    for (size_t i = 0; i < input_ports.size(); ++i) {
        std::shared_ptr<IStreamOperator> stream_op;
        if (i == 0) {
            stream_op = plan.first_stream_operator;
        } else {
            std::shared_ptr<IOperator> op_holder = CreateOperator(plan.parsed_ops[0].category, plan.parsed_ops[0].name);
            if (!op_holder) {
                rsp = BuildExecutionErrorJson("operator create failed for shard", ErrorCodeId::kOpExecFail,
                                              ErrorStageId::kExecute);
                return error::INTERNAL_ERROR;
            }
            stream_op = std::dynamic_pointer_cast<IStreamOperator>(op_holder);
        }
        if (!stream_op) {
            rsp = BuildExecutionErrorJson("stream operator cast failed for shard", ErrorCodeId::kOpExecFail,
                                          ErrorStageId::kExecute);
            return error::INTERNAL_ERROR;
        }

        const int init_rc = stream_op->Init(with_params_json.c_str(), plan.sink_ctx);
        if (init_rc != 0) {
            const std::string err =
                stream_op->LastError().empty() ? "stream operator Init failed" : stream_op->LastError();
            rsp = BuildExecutionErrorJson(err, ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
            return error::BAD_REQUEST;
        }

        if (static_schema) {
            const int schema_rc = stream_op->OnSchemaReady(static_schema);
            if (schema_rc != 0) {
                const std::string err =
                    stream_op->LastError().empty() ? "stream operator OnSchemaReady failed" : stream_op->LastError();
                rsp = BuildExecutionErrorJson(err, ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
                return error::BAD_REQUEST;
            }
        }

        task->AddShard(std::make_shared<ShardRunner>(static_cast<uint32_t>(i), input_ports[i], stream_op, output,
                                                     task.get(), static_schema != nullptr));
    }
    task->PrepareForRun(static_cast<uint32_t>(input_ports.size()), CurrentTimeMs());
    task->SetSourceResolveMeta(plan.resolved_sources, plan.source_expand_rule);

    const int open_rc = open_target->Open();
    if (open_rc != 0) {
        rsp = BuildExecutionErrorJson("open stream source failed", ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
        return error::INTERNAL_ERROR;
    }

    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        stream_tasks_[task->Id()] = task;
    }
    TouchRuntimeAccess(task->Id());
    for (const auto& shard : task->Shards()) {
        stream_runtime_.TrySchedule(shard);
    }
    if (!plan.skip_lease_acquire) {
        lease_token.Commit();
    }
    cleanup_shared_subscription = false;
    shared_subscription_guard.reset();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status");
    w.String("submitted");
    w.Key("runtime_task_id");
    w.String(task->Id().c_str());
    w.Key("task_id");
    w.String(task->Id().c_str());
    w.Key("runtime_kind");
    w.String("single");
    w.Key("resolved_sources");
    w.StartArray();
    for (const auto& source_name : plan.resolved_sources) {
        w.String(source_name.c_str());
    }
    w.EndArray();
    w.Key("source_expand_rule");
    w.String(plan.source_expand_rule.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::ClassifySqlTaskKind(const std::string& sql_text, std::string* task_kind, std::string* err_rsp,
                                             bool* requires_async) {
    if (!task_kind || !err_rsp) return error::INTERNAL_ERROR;
    task_kind->clear();
    err_rsp->clear();
    if (requires_async) *requires_async = false;

    static constexpr size_t kMaxSqlLength = 64 * 1024;
    if (sql_text.size() > kMaxSqlLength) {
        *err_rsp = BuildErrorJson("SQL too long (max 64KB)");
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    SqlStatement stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        *err_rsp = BuildErrorJson(stmt.error);
        return error::BAD_REQUEST;
    }
    if (stmt.sources.empty() && !stmt.source.empty()) {
        stmt.sources.push_back(stmt.source);
    }
    if (stmt.sources.empty()) {
        *err_rsp = BuildErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }

    SourceResolveResult source_resolved;
    const int32_t resolve_rc = ResolveSourceBindings(stmt, &source_resolved, err_rsp);
    if (resolve_rc != error::OK) {
        return resolve_rc;
    }

    if (source_resolved.has_stream_source && source_resolved.has_non_stream_source) {
        *err_rsp = BuildErrorJson("mixed stream and non-stream sources are not supported");
        return error::BAD_REQUEST;
    }

    bool operator_requires_async = false;
    if (stmt.operators.size() == 1) {
        CppOperatorCapabilityLeaseV1 lease;
        bool ambiguous = false;
        int traversal = 0;
        auto provider = FindBlockTransformOperator(stmt.operators[0].category, stmt.operators[0].name, &lease,
                                                   &ambiguous, &traversal);
        if (ambiguous || traversal != 0) {
            *err_rsp = BuildErrorJson("operator policy lookup failed");
            return error::BAD_REQUEST;
        }
        auto* policy = provider.v2 ? dynamic_cast<IBlockTransformExecutionPolicyProviderV1*>(provider.v2) : nullptr;
        if (policy) {
            const auto& params =
                !stmt.operator_with_params.empty() ? stmt.operator_with_params.front() : stmt.with_params;
            const auto with = MakeWithParamsJson(params);
            if (policy->RequiresAsyncExecution(with.c_str(), stmt.source.c_str(), &operator_requires_async) != 0) {
                *err_rsp = BuildErrorJson("operator execution policy invalid");
                return error::BAD_REQUEST;
            }
        }
    }
    *task_kind = source_resolved.has_stream_source ? "stream" : "batch";
    if (requires_async) {
        *requires_async = source_resolved.has_stream_source || operator_requires_async;
        if (querier_ && !source_resolved.block_channels.empty()) {
            querier_->Traverse(IID_BLOCK_STREAM_CHANNEL_DESCRIPTOR_V1, [&](void* value) {
                static_cast<IBlockStreamChannelDescriptorV1*>(value)->DescribeChannelTypes([&](const auto& type) {
                    if (type.is_finite) return;
                    for (const auto& channel : source_resolved.block_channels) {
                        if (IEquals(channel->Category(), type.channel_type)) {
                            *requires_async = true;
                            break;
                        }
                    }
                });
                return 0;
            });
        }
    }
    return error::OK;
}

}  // namespace scheduler
}  // namespace flowsql
