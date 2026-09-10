// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <common/error_code.h>
#include <common/log.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
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

#include "framework/core/channel_adapter.h"
#include "framework/core/packet_codec.h"
#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/fan_in_stream_channel.h"
#include "framework/core/fan_out_stream_channel.h"
#include "framework/core/filter_binding.h"
#include "framework/core/filter_expression.h"
#include "framework/core/filter_planner.h"
#include "framework/core/json_error_builder.h"
#include "framework/core/pipeline.h"
#include "framework/core/ring_stream_channel.h"
#include "framework/core/sql_parser.h"
#include "framework/core/sql_text_splitter.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ichannel_registry.h"
#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idatabase_factory.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/ibuiltin_registry.h"
#include "framework/interfaces/ibridge.h"
#include "framework/interfaces/ioperator.h"
#include "framework/interfaces/ioperator_catalog.h"
#include "framework/interfaces/ioperator_registry.h"
#include "framework/interfaces/istream_channel.h"
#include "framework/interfaces/istream_factory.h"
#include "framework/interfaces/istream_manager.h"
#include "framework/interfaces/iblock_stream_channel.h"
#include "framework/interfaces/iblock_stream_factory.h"
#include "framework/interfaces/ifilter_domain_resolver.h"
#include "scheduler_json_codec.h"
#include "scheduler_internal_utils.h"

namespace flowsql {
namespace scheduler {

static std::string EnsureExecutionErrorJson(const std::string& rsp,
                                            const std::string& fallback_error,
                                            ErrorCodeId fallback_code,
                                            ErrorStageId fallback_stage) {
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
    explicit SharedSourceState(std::shared_ptr<IStreamChannel> source)
        : source_(std::move(source)) {}

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

    bool IsFinished() const {
        return source_ ? source_->IsFinished() : true;
    }

    bool IsFull() const {
        return source_ && source_->IsFull();
    }

    bool IsEmpty() const {
        return !source_ || source_->IsEmpty();
    }

    size_t Capacity() const {
        return source_ ? source_->Capacity() : 0;
    }

    size_t Size() const {
        return source_ ? source_->Size() : 0;
    }

    std::shared_ptr<arrow::Schema> GetOutputSchema() {
        return source_ ? source_->GetOutputSchema() : nullptr;
    }

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

    const char* Category() override {
        return state_ ? state_->Category() : "stateless";
    }

    const char* Name() override {
        return view_name_.c_str();
    }

    const char* Type() override {
        return ChannelType::kStream;
    }

    const char* Schema() override {
        return state_ ? state_->Schema() : "[]";
    }

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

    int Put(std::shared_ptr<arrow::RecordBatch>, int64_t) override {
        return ENOTSUP;
    }

    PollEvent PollNext(int timeout_ms = 100) override {
        if (!state_) return PollEvent::Error(-EINVAL, "invalid shared spmc state");
        return state_->PollNext(timeout_ms);
    }

    std::shared_ptr<arrow::Schema> GetOutputSchema() override {
        return state_ ? state_->GetOutputSchema() : nullptr;
    }

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
    bool IsFinished() const override {
        return state_ && state_->IsFinished();
    }

 private:
    std::shared_ptr<SharedSourceState> state_;
    std::string view_name_;
    std::atomic<bool> closed_{false};
};

class FanOutPartitionView final : public IStreamChannel {
 public:
    FanOutPartitionView(std::shared_ptr<FanOutStreamChannel> parent,
                        std::shared_ptr<IStreamChannel> partition)
        : parent_(std::move(parent)),
          partition_(std::move(partition)) {}

    const char* Category() override {
        return partition_ ? partition_->Category() : "fanout";
    }

    const char* Name() override {
        return partition_ ? partition_->Name() : "fanout.partition";
    }

    const char* Type() override {
        return ChannelType::kStream;
    }

    const char* Schema() override {
        return partition_ ? partition_->Schema() : "[]";
    }

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

    int SetFilter(const char* condition_json,
                  std::vector<std::string>* unsupported_out) override {
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

std::shared_ptr<IOperator> SchedulerPlugin::CreateOperator(const std::string& category,
                                                           const std::string& name) {
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

IBlockStreamOperator* SchedulerPlugin::FindBlockOperator(const std::string& category,
                                                          const std::string& name) {
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

IBlockTransformOperatorV1* SchedulerPlugin::FindBlockTransformOperator(
    const std::string& category,
    const std::string& name,
    bool* ambiguous,
    int* traverse_error) {
    if (ambiguous) *ambiguous = false;
    if (traverse_error) *traverse_error = 0;
    if (!querier_) return nullptr;

    IBlockTransformOperatorV1* found = nullptr;
    size_t matches = 0;
    const int traversal_rc = querier_->Traverse(
        IID_BLOCK_TRANSFORM_OPERATOR_V1,
        [&](void* value) -> int {
            auto* candidate = static_cast<IBlockTransformOperatorV1*>(value);
            if (!candidate || !IEquals(candidate->Category(), category) ||
                candidate->Name() != name) {
                return 0;
            }
            found = candidate;
            ++matches;
            return 0;
        });
    if (traverse_error) *traverse_error = traversal_rc;
    if (traversal_rc != 0) return nullptr;
    if (ambiguous) *ambiguous = matches > 1;
    return matches == 1 ? found : nullptr;
}

int SchedulerPlugin::ExecuteBlockOperator(IBlockStreamChannel* source,
                                           IBlockStreamOperator* op,
                                           const std::shared_ptr<const BoundFilterExpr>& source_residual,
                                           BlockExecutionTerminal* terminal,
                                           int64_t* rows_affected,
                                           std::string* error) {
    if (terminal) *terminal = BlockExecutionTerminal::kFailed;
    if (!source || !op) return EINVAL;
    BlockFilterStage source_filter(source_residual);
    if (source_residual) {
        std::shared_ptr<arrow::Schema> filtered_schema;
        std::string filter_error;
        const auto open_rc = source_filter.Open(
            packet::PacketSchema(), &filtered_schema, &filter_error);
        if (open_rc != FilterEvalError::kNone) {
            if (error) *error = "source-stage residual Open failed: " + filter_error;
            return EINVAL;
        }
    }
    std::shared_ptr<arrow::Schema> schema;
    int64_t rows = 0;
    bool schema_ready = false;
    bool stopped = false;
    while (true) {
        const BlockPollEvent event = source->PollBlock(100);
        if (event.kind == BlockPollEvent::kData) {
            if (!event.batch) {
                if (error) *error = "block operator received empty data batch";
                return EINVAL;
            }
            int64_t batch_rows = event.batch->num_rows();
            int rc = 0;
            bool schema_failed = false;
            try {
                if (!schema_ready) {
                    schema = event.batch->schema();
                    if (!schema || !schema->Equals(flowsql::packet::PacketSchema())) {
                        rc = EINVAL;
                        schema_failed = true;
                        if (error) *error = "block source schema does not match packet schema";
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
                    const auto eval_rc = source_filter.ProcessBlock(
                        event.batch, 0, &filtered, &filter_error);
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
            if (schema_failed) {
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
        *terminal = stopped ? BlockExecutionTerminal::kStopped
                            : BlockExecutionTerminal::kCompleted;
    }
    return 0;
}

namespace {

class SchemaCheckingBlockTransformTask final : public IBlockTransformTaskV1 {
 public:
    SchemaCheckingBlockTransformTask(
        IBlockTransformTaskV1* task,
        std::shared_ptr<arrow::Schema> expected_output_schema)
        : task_(task), expected_output_schema_(std::move(expected_output_schema)) {}

    int Open(std::shared_ptr<arrow::Schema> input_schema,
             std::shared_ptr<arrow::Schema>* output_schema) override {
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

    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input,
                     int64_t ts_ms,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        return task_->ProcessBlock(input, ts_ms, outputs);
    }

    int Flush(std::vector<BlockTransformOutputV1>* outputs) override {
        return task_->Flush(outputs);
    }

    void Cancel() override { task_->Cancel(); }

    std::string LastError() const override {
        return last_error_.empty() ? task_->LastError() : last_error_;
    }

 private:
    IBlockTransformTaskV1* task_ = nullptr;
    std::shared_ptr<arrow::Schema> expected_output_schema_;
    std::string last_error_;
};

class SynchronousBlockTransformChainTask final : public IBlockTransformTaskV1 {
 public:
    SynchronousBlockTransformChainTask(
        std::vector<IBlockTransformTaskV1*> tasks,
        std::vector<std::shared_ptr<arrow::Schema>> expected_output_schemas,
        const std::vector<std::shared_ptr<const BoundFilterExpr>>& residuals)
        : tasks_(std::move(tasks)),
          expected_output_schemas_(std::move(expected_output_schemas)) {
        filters_.reserve(residuals.size());
        for (const auto& residual : residuals) filters_.emplace_back(residual);
    }

    int Open(std::shared_ptr<arrow::Schema> input_schema,
             std::shared_ptr<arrow::Schema>* output_schema) override {
        if (open_attempted_ || !input_schema || !output_schema || tasks_.empty() ||
            tasks_.size() != expected_output_schemas_.size() ||
            tasks_.size() != filters_.size()) {
            last_error_ = "multi transform chain received an invalid Open request";
            return EINVAL;
        }
        open_attempted_ = true;
        output_schema->reset();

        auto current_schema = std::move(input_schema);
        for (size_t i = 0; i < tasks_.size(); ++i) {
            std::shared_ptr<arrow::Schema> actual_output_schema;
            int open_rc = 0;
            try {
                open_rc = tasks_[i]->Open(current_schema, &actual_output_schema);
            } catch (const std::exception& ex) {
                return SetStageError(i, "Open threw: " + std::string(ex.what()), EFAULT);
            } catch (...) {
                return SetStageError(i, "Open threw an unknown exception", EFAULT);
            }
            if (open_rc != 0) {
                return SetStageError(i, "Open failed: " + TaskError(i), open_rc);
            }
            if (!actual_output_schema || !expected_output_schemas_[i] ||
                !actual_output_schema->Equals(*expected_output_schemas_[i], true)) {
                return SetStageError(
                    i, "execution Schema differs from its planning Schema", EPROTO);
            }

            std::string filter_error;
            std::shared_ptr<arrow::Schema> filtered_schema;
            const auto filter_rc = filters_[i].Open(
                actual_output_schema, &filtered_schema, &filter_error);
            if (filter_rc != FilterEvalError::kNone) {
                if (filter_error.empty()) filter_error = "residual filter Open failed";
                return SetStageError(i, std::move(filter_error), EINVAL);
            }
            current_schema = std::move(filtered_schema);
        }

        opened_ = true;
        *output_schema = std::move(current_schema);
        return 0;
    }

    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input,
                     int64_t ts_ms,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        if (!opened_ || !input || !outputs || !outputs->empty() || flush_started_) {
            last_error_ = "multi transform chain received an invalid ProcessBlock request";
            return -EINVAL;
        }
        std::vector<BlockTransformOutputV1> initial = {{input, ts_ms}};
        std::vector<BlockTransformOutputV1> final_outputs;
        bool stopped = false;
        const int rc = PropagateFrom(
            0, std::move(initial), &final_outputs, &stopped);
        if (rc != 0) return rc;
        *outputs = std::move(final_outputs);
        return stopped ? static_cast<int>(BlockTransformStatusV1::kStop)
                       : static_cast<int>(BlockTransformStatusV1::kContinue);
    }

    int Flush(std::vector<BlockTransformOutputV1>* outputs) override {
        if (!opened_ || !outputs || !outputs->empty() || flush_started_) {
            last_error_ = "multi transform chain received an invalid Flush request";
            return EINVAL;
        }
        flush_started_ = true;
        std::vector<BlockTransformOutputV1> final_outputs;

        for (size_t i = 0; i < tasks_.size(); ++i) {
            std::vector<BlockTransformOutputV1> stage_outputs;
            int flush_rc = 0;
            try {
                flush_rc = tasks_[i]->Flush(&stage_outputs);
            } catch (const std::exception& ex) {
                return SetStageError(i, "Flush threw: " + std::string(ex.what()), EFAULT);
            } catch (...) {
                return SetStageError(i, "Flush threw an unknown exception", EFAULT);
            }
            if (flush_rc != 0) {
                if (!stage_outputs.empty()) {
                    return SetStageError(
                        i, "Flush failed while returning output batches", EPROTO);
                }
                return SetStageError(i, "Flush failed: " + TaskError(i), flush_rc);
            }

            std::vector<BlockTransformOutputV1> filtered_outputs;
            const int filter_rc = FilterStageOutputs(
                i, stage_outputs, &filtered_outputs);
            if (filter_rc != 0) return filter_rc;

            std::vector<BlockTransformOutputV1> propagated_outputs;
            bool ignored_stop = false;
            const int propagate_rc = PropagateFrom(
                i + 1,
                std::move(filtered_outputs),
                &propagated_outputs,
                &ignored_stop);
            if (propagate_rc != 0) return propagate_rc;
            final_outputs.insert(final_outputs.end(),
                                 std::make_move_iterator(propagated_outputs.begin()),
                                 std::make_move_iterator(propagated_outputs.end()));
        }

        *outputs = std::move(final_outputs);
        return 0;
    }

    void Cancel() override {
        bool expected = false;
        if (!cancel_started_.compare_exchange_strong(expected, true)) return;
        for (auto* task : tasks_) {
            if (!task) continue;
            try {
                task->Cancel();
            } catch (...) {
            }
        }
    }

    std::string LastError() const override { return last_error_; }

 private:
    int PropagateFrom(size_t first_stage,
                      std::vector<BlockTransformOutputV1> inputs,
                      std::vector<BlockTransformOutputV1>* outputs,
                      bool* stopped) {
        if (!outputs || !stopped || first_stage > tasks_.size()) return -EINVAL;
        outputs->clear();
        for (size_t i = first_stage; i < tasks_.size(); ++i) {
            std::vector<BlockTransformOutputV1> next_inputs;
            for (const auto& input : inputs) {
                if (!input.batch) {
                    return SetStageError(i, "received a null input batch", EPROTO);
                }
                std::vector<BlockTransformOutputV1> stage_outputs;
                int process_rc = 0;
                try {
                    process_rc = tasks_[i]->ProcessBlock(
                        input.batch, input.ts_ms, &stage_outputs);
                } catch (const std::exception& ex) {
                    return SetStageError(
                        i, "ProcessBlock threw: " + std::string(ex.what()), EFAULT);
                } catch (...) {
                    return SetStageError(
                        i, "ProcessBlock threw an unknown exception", EFAULT);
                }
                const bool valid_status =
                    process_rc == static_cast<int>(BlockTransformStatusV1::kContinue) ||
                    process_rc == static_cast<int>(BlockTransformStatusV1::kStop);
                if (!valid_status) {
                    if (!stage_outputs.empty()) {
                        return SetStageError(
                            i,
                            "ProcessBlock failed while returning output batches",
                            EPROTO);
                    }
                    return SetStageError(
                        i, "ProcessBlock failed: " + TaskError(i), process_rc);
                }
                if (process_rc == static_cast<int>(BlockTransformStatusV1::kStop)) {
                    *stopped = true;
                }

                std::vector<BlockTransformOutputV1> filtered_outputs;
                const int filter_rc = FilterStageOutputs(
                    i, stage_outputs, &filtered_outputs);
                if (filter_rc != 0) return filter_rc;
                next_inputs.insert(next_inputs.end(),
                                   std::make_move_iterator(filtered_outputs.begin()),
                                   std::make_move_iterator(filtered_outputs.end()));
            }
            inputs = std::move(next_inputs);
        }
        *outputs = std::move(inputs);
        return 0;
    }

    int FilterStageOutputs(
        size_t stage,
        const std::vector<BlockTransformOutputV1>& inputs,
        std::vector<BlockTransformOutputV1>* outputs) {
        if (!outputs || stage >= filters_.size()) return -EINVAL;
        outputs->clear();
        for (const auto& input : inputs) {
            if (!input.batch) {
                return SetStageError(stage, "returned a null output batch", EPROTO);
            }
            BlockTransformOutputV1 filtered;
            std::string filter_error;
            const auto filter_rc = filters_[stage].ProcessBlock(
                input.batch, input.ts_ms, &filtered, &filter_error);
            if (filter_rc != FilterEvalError::kNone) {
                if (filter_error.empty()) filter_error = "residual filter failed";
                return SetStageError(stage, std::move(filter_error), EIO);
            }
            outputs->push_back(std::move(filtered));
        }
        return 0;
    }

    int SetStageError(size_t stage, std::string detail, int rc) {
        last_error_ = "transform stage " + std::to_string(stage + 1) + ": " + detail;
        if (rc == 0) return -EIO;
        return rc > 0 ? -rc : rc;
    }

    std::string TaskError(size_t stage) const {
        if (stage >= tasks_.size() || !tasks_[stage]) return "task unavailable";
        try {
            const std::string detail = tasks_[stage]->LastError();
            return detail.empty() ? "task did not provide an error message" : detail;
        } catch (...) {
            return "task LastError threw";
        }
    }

    std::vector<IBlockTransformTaskV1*> tasks_;
    std::vector<std::shared_ptr<arrow::Schema>> expected_output_schemas_;
    std::vector<BlockFilterStage> filters_;
    bool open_attempted_ = false;
    bool opened_ = false;
    bool flush_started_ = false;
    std::atomic<bool> cancel_started_{false};
    std::string last_error_;
};

const std::string* FindStageFilterText(
    const SqlStatement& stmt,
    uint32_t after_stage,
    bool* duplicate) {
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

bool ParseStageFilter(const std::string* filter_text,
                      uint32_t after_stage,
                      std::shared_ptr<FilterExpr>* expression,
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

int ResolveFilterDomain(IQuerier* querier,
                        FilterDomainTargetKindV1 target_kind,
                        const std::string& target_category,
                        const std::string& target_name,
                        const std::shared_ptr<arrow::Schema>& output_schema,
                        const std::shared_ptr<FilterExpr>& expression,
                        std::shared_ptr<FilterExpr>* resolved_expression,
                        std::string* error) {
    if (resolved_expression) resolved_expression->reset();
    if (error) error->clear();
    if (!resolved_expression || !output_schema || !expression || target_category.empty() ||
        target_name.empty()) {
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
    const int traversal_rc = querier->Traverse(
        IID_FILTER_DOMAIN_RESOLVER_V1,
        [&](void* value) -> int {
            auto* resolver = static_cast<IFilterDomainResolverV1*>(value);
            if (!resolver) {
                resolution_error = EIO;
                resolution_diagnostic =
                    "filter domain resolver discovery returned a null provider";
                return -1;
            }

            FilterDomainResolveResultV1 result;
            int rc = 0;
            try {
                rc = resolver->Resolve(request, &result);
            } catch (const std::exception& ex) {
                resolution_error = EIO;
                resolution_diagnostic =
                    "filter domain resolver threw: " + std::string(ex.what());
                return -1;
            } catch (...) {
                resolution_error = EIO;
                resolution_diagnostic = "filter domain resolver threw an unknown exception";
                return -1;
            }
            if (rc == ENOTSUP) {
                if (result.lowered_expression) {
                    resolution_error = EIO;
                    resolution_diagnostic =
                        "non-owner filter domain resolver returned an expression";
                    return -1;
                }
                return 0;
            }
            if (rc != 0) {
                resolution_error = rc == ENOMEM || rc == EFAULT ? EIO : EINVAL;
                resolution_diagnostic =
                    result.diagnostic.empty()
                        ? "filter domain resolver rejected the expression with code " +
                              std::to_string(rc)
                        : std::move(result.diagnostic);
                return -1;
            }
            if (!result.lowered_expression) {
                resolution_error = EIO;
                resolution_diagnostic =
                    "owning filter domain resolver returned no expression";
                return -1;
            }
            if (owner_count != 0) {
                resolution_error = EIO;
                resolution_diagnostic =
                    "multiple filter domain resolvers accepted the current stage";
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
            *error = "filter domain resolver traversal failed with code " +
                     std::to_string(traversal_rc);
        }
        return EIO;
    }
    if (owner_count == 0) *resolved_expression = expression;
    return 0;
}

int ResolveAndBindStageFilter(IQuerier* querier,
                              FilterDomainTargetKindV1 target_kind,
                              const std::string& target_category,
                              const std::string& target_name,
                              const std::shared_ptr<arrow::Schema>& output_schema,
                              const std::shared_ptr<FilterExpr>& expression,
                              const std::string& stage_label,
                              std::shared_ptr<const BoundFilterExpr>* bound_expression,
                              std::string* error) {
    if (bound_expression) bound_expression->reset();
    if (!expression) return 0;

    std::shared_ptr<FilterExpr> resolved_expression;
    std::string detail;
    const int resolve_rc = ResolveFilterDomain(
        querier, target_kind, target_category, target_name, output_schema, expression,
        &resolved_expression, &detail);
    if (resolve_rc != 0) {
        if (error) *error = stage_label + " filter domain resolution failed: " + detail;
        return resolve_rc;
    }

    const auto bind_rc = BindFilterExpression(
        output_schema, resolved_expression, bound_expression, &detail);
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

int SchedulerPlugin::BuildBlockSourceFilterPlan(
    IBlockStreamChannel* source,
    const SqlStatement& stmt,
    BlockSourceFilterPlan* plan,
    std::string* error) {
    if (plan) *plan = BlockSourceFilterPlan{};
    if (error) error->clear();
    if (!source || !plan || !error) return EINVAL;
    plan->pushed_filter_plan_json = kEmptyCanonicalFilterPlanV1;

    bool duplicate_source_filter = false;
    const auto* source_filter_text = FindStageFilterText(
        stmt, 0, &duplicate_source_filter);
    if (duplicate_source_filter) {
        *error = "block source contains duplicate filters";
        return EINVAL;
    }
    std::shared_ptr<FilterExpr> source_expression;
    if (!ParseStageFilter(source_filter_text, 0, &source_expression, error)) {
        return EINVAL;
    }
    if (!source_expression) return 0;

    const auto source_schema = packet::PacketSchema();
    std::shared_ptr<const BoundFilterExpr> bound_source_filter;
    const int filter_rc = ResolveAndBindStageFilter(
        querier_, FilterDomainTargetKindV1::kSource,
        source->Category() ? source->Category() : "",
        source->Name() ? source->Name() : "", source_schema, source_expression,
        "source-stage", &bound_source_filter, error);
    if (filter_rc != 0) return filter_rc;

    FilterPushdownTarget target;
    target.kind = FilterPushdownTargetKindV1::kSource;
    target.category = source->Category() ? source->Category() : "";
    target.name = source->Name() ? source->Name() : "";
    target.output_schema = source_schema;
    FilterPushdownNegotiation negotiation;
    std::string plan_error;
    const auto negotiate_rc = NegotiateFilterPushdown(
        querier_, target, bound_source_filter, &negotiation, &plan_error);
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
        source_schema, bound_source_filter, negotiation,
        FilterTaskIsolation::kExclusive, &exclusive_plan, &plan_error);
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
        source_schema, bound_source_filter, negotiation,
        FilterTaskIsolation::kSharedSource, &shared_plan, &plan_error);
    if (shared_rc != FilterPlanError::kNone) {
        *error = "source-stage shared filter task planning failed: " + plan_error;
        return shared_rc == FilterPlanError::kInvalidArgument ||
                       shared_rc == FilterPlanError::kInvalidBoundExpression ||
                       shared_rc == FilterPlanError::kCanonicalPlanError
                   ? EINVAL
                   : EIO;
    }

    plan->pushed_filter_plan_json =
        std::move(exclusive_plan.pushed_filter_plan_json);
    plan->exclusive_residual = std::move(exclusive_plan.residual_expression);
    plan->shared_residual = std::move(shared_plan.residual_expression);
    return 0;
}

int SchedulerPlugin::ExecuteSingleBlockTransformPipeline(
    IBlockStreamChannel* source,
    IBlockTransformOperatorV1* provider,
    IDataFrameChannel* sink,
    const SqlStatement& stmt,
    const std::shared_ptr<const BoundFilterExpr>& source_residual,
    BlockExecutionTerminal* terminal,
    int64_t* rows_affected,
    std::string* error) {
    if (terminal) *terminal = BlockExecutionTerminal::kFailed;
    if (rows_affected) *rows_affected = 0;
    if (error) error->clear();
    if (!source || !provider || !sink || stmt.operators.size() != 1) {
        if (error) *error = "block transform pipeline requires one source, provider, operator, and sink";
        return EINVAL;
    }
    auto* appendable_sink = dynamic_cast<IAppendableDataFrameChannel*>(sink);
    if (!appendable_sink) {
        if (error) *error = "block transform pipeline requires an appendable DataFrame sink";
        return EINVAL;
    }

    bool duplicate_transform_filter = false;
    const auto* transform_filter_text = FindStageFilterText(
        stmt, 1, &duplicate_transform_filter);
    if (duplicate_transform_filter) {
        if (error) *error = "block transform stage contains duplicate filters";
        return EINVAL;
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

    const auto source_schema = packet::PacketSchema();

    const auto& params = !stmt.operator_with_params.empty()
                             ? stmt.operator_with_params.front()
                             : stmt.with_params;
    const std::string with_params_json = MakeWithParamsJson(params);
    const std::string task_id = NextStreamTaskId();
    FilterTaskSessionPlan transform_filter_plan;
    transform_filter_plan.pushed_filter_plan_json = kEmptyCanonicalFilterPlanV1;
    std::shared_ptr<arrow::Schema> planned_output_schema;

    auto make_task_holder = [provider](IBlockTransformTaskV1* task) {
        return std::unique_ptr<IBlockTransformTaskV1,
                               std::function<void(IBlockTransformTaskV1*)>>(
            task,
            [provider](IBlockTransformTaskV1* owned) {
                if (!owned) return;
                try {
                    provider->ReleaseTask(owned);
                } catch (...) {
                }
            });
    };

    if (transform_expression) {
        IBlockTransformTaskV1* probe_raw = nullptr;
        std::string plan_error;
        const auto create_probe = CreateBlockTransformTaskSession(
            provider,
            task_id + ".schema",
            with_params_json,
            transform_filter_plan,
            &probe_raw,
            &plan_error);
        if (create_probe != FilterPlanError::kNone) {
            if (error) *error = "transform Schema probe creation failed: " + plan_error;
            return EIO;
        }
        auto probe = make_task_holder(probe_raw);

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
        const int filter_rc = ResolveAndBindStageFilter(
            querier_, FilterDomainTargetKindV1::kTransform,
            stmt.operators.front().category, stmt.operators.front().name,
            planned_output_schema, transform_expression, "operator-stage",
            &bound_transform_filter, &plan_error);
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
        const auto negotiate_rc = NegotiateFilterPushdown(
            querier_, target, bound_transform_filter, &negotiation, &plan_error);
        if (negotiate_rc != FilterPlanError::kNone) {
            if (error) *error = "operator-stage filter pushdown negotiation failed: " + plan_error;
            return negotiate_rc == FilterPlanError::kInvalidArgument ||
                           negotiate_rc == FilterPlanError::kInvalidBoundExpression ||
                           negotiate_rc == FilterPlanError::kCanonicalPlanError
                       ? EINVAL
                       : EIO;
        }
        const auto materialize_rc = MaterializeFilterTaskSessionPlan(
            planned_output_schema,
            bound_transform_filter,
            negotiation,
            FilterTaskIsolation::kExclusive,
            &transform_filter_plan,
            &plan_error);
        if (materialize_rc != FilterPlanError::kNone) {
            if (error) *error = "operator-stage filter task planning failed: " + plan_error;
            return EIO;
        }
    }

    IBlockTransformTaskV1* execution_raw = nullptr;
    std::string plan_error;
    const auto create_execution = CreateBlockTransformTaskSession(
        provider,
        task_id + ".run",
        with_params_json,
        transform_filter_plan,
        &execution_raw,
        &plan_error);
    if (create_execution != FilterPlanError::kNone) {
        if (error) *error = "transform execution task creation failed: " + plan_error;
        return EIO;
    }
    auto execution = make_task_holder(execution_raw);
    SchemaCheckingBlockTransformTask checked_execution(
        execution.get(), planned_output_schema);
    IBlockTransformTaskV1* runner_task = planned_output_schema
                                             ? static_cast<IBlockTransformTaskV1*>(&checked_execution)
                                             : execution.get();

    BlockTransformPipelineConfig config;
    config.source = source;
    config.source_schema = source_schema;
    config.transform = runner_task;
    config.source_residual = source_residual;
    config.transform_residual = transform_filter_plan.residual_expression;
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
        *error = runner_error.empty() ? "block transform pipeline execution failed"
                                     : std::move(runner_error);
    }
    return runner_rc == BlockTransformPipelineError::kCancelled ? ECANCELED : EIO;
}

int SchedulerPlugin::ExecuteBlockTransformPipeline(
    IBlockStreamChannel* source,
    const std::vector<IBlockTransformOperatorV1*>& providers,
    IDataFrameChannel* sink,
    const SqlStatement& stmt,
    const std::shared_ptr<const BoundFilterExpr>& source_residual,
    BlockExecutionTerminal* terminal,
    int64_t* rows_affected,
    std::string* error) {
    if (providers.size() == 1) {
        return ExecuteSingleBlockTransformPipeline(
            source,
            providers.front(),
            sink,
            stmt,
            source_residual,
            terminal,
            rows_affected,
            error);
    }

    if (terminal) *terminal = BlockExecutionTerminal::kFailed;
    if (rows_affected) *rows_affected = 0;
    if (error) error->clear();
    if (!source || !sink || providers.size() < 2 ||
        providers.size() != stmt.operators.size()) {
        if (error) {
            *error = "multi block transform pipeline requires one source, aligned providers, "
                     "operators, and a sink";
        }
        return EINVAL;
    }
    for (auto* provider : providers) {
        if (!provider) {
            if (error) *error = "multi block transform pipeline received a null provider";
            return EINVAL;
        }
    }
    auto* appendable_sink = dynamic_cast<IAppendableDataFrameChannel*>(sink);
    if (!appendable_sink) {
        if (error) *error = "multi block transform pipeline requires an appendable DataFrame sink";
        return EINVAL;
    }

    std::vector<const std::string*> stage_filter_texts(providers.size() + 1);
    for (size_t stage = 1; stage < stage_filter_texts.size(); ++stage) {
        bool duplicate = false;
        stage_filter_texts[stage] = FindStageFilterText(
            stmt, static_cast<uint32_t>(stage), &duplicate);
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
        if (!ParseStageFilter(
                stage_filter_texts[stage], static_cast<uint32_t>(stage), &stage_expressions[stage], error)) {
            return EINVAL;
        }
    }

    const auto source_schema = packet::PacketSchema();
    struct StagePlan {
        IBlockTransformOperatorV1* provider = nullptr;
        std::string with_params_json;
        FilterTaskSessionPlan filter_plan;
        std::shared_ptr<arrow::Schema> output_schema;
    };
    std::vector<StagePlan> stage_plans(providers.size());
    const std::unordered_map<std::string, std::string> empty_params;
    const std::string task_id = NextStreamTaskId();
    auto input_schema = source_schema;

    using TaskHolder = std::unique_ptr<
        IBlockTransformTaskV1,
        std::function<void(IBlockTransformTaskV1*)>>;
    auto make_task_holder = [](IBlockTransformOperatorV1* provider,
                               IBlockTransformTaskV1* task) {
        return TaskHolder(
            task,
            [provider](IBlockTransformTaskV1* owned) {
                if (!owned) return;
                try {
                    provider->ReleaseTask(owned);
                } catch (...) {
                }
            });
    };

    for (size_t i = 0; i < providers.size(); ++i) {
        auto& plan = stage_plans[i];
        plan.provider = providers[i];
        plan.filter_plan.pushed_filter_plan_json = kEmptyCanonicalFilterPlanV1;
        const auto& params = i < stmt.operator_with_params.size()
                                 ? stmt.operator_with_params[i]
                                 : empty_params;
        plan.with_params_json = MakeWithParamsJson(params);

        IBlockTransformTaskV1* probe_raw = nullptr;
        std::string plan_error;
        const auto create_probe = CreateBlockTransformTaskSession(
            plan.provider,
            task_id + ".stage" + std::to_string(i + 1) + ".schema",
            plan.with_params_json,
            plan.filter_plan,
            &probe_raw,
            &plan_error);
        if (create_probe != FilterPlanError::kNone) {
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) +
                         " Schema probe creation failed: " + plan_error;
            }
            return EIO;
        }
        auto probe = make_task_holder(plan.provider, probe_raw);

        int probe_open_rc = 0;
        try {
            probe_open_rc = probe->Open(input_schema, &plan.output_schema);
        } catch (const std::exception& ex) {
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) +
                         " Schema probe Open threw: " + ex.what();
            }
            try {
                probe->Cancel();
            } catch (...) {
            }
            return EFAULT;
        } catch (...) {
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) +
                         " Schema probe Open threw an unknown exception";
            }
            try {
                probe->Cancel();
            } catch (...) {
            }
            return EFAULT;
        }
        if (probe_open_rc != 0 || !plan.output_schema) {
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) +
                         " Schema probe Open failed";
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
                *error = "transform stage " + std::to_string(i + 1) +
                         " Schema probe Cancel failed";
            }
            return EFAULT;
        }
        probe.reset();

        if (stage_expressions[i + 1]) {
            std::shared_ptr<const BoundFilterExpr> bound_filter;
            const std::string stage_label =
                "transform stage " + std::to_string(i + 1);
            const int filter_rc = ResolveAndBindStageFilter(
                querier_, FilterDomainTargetKindV1::kTransform,
                stmt.operators[i].category, stmt.operators[i].name,
                plan.output_schema, stage_expressions[i + 1], stage_label,
                &bound_filter, &plan_error);
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
            const auto negotiate_rc = NegotiateFilterPushdown(
                querier_, target, bound_filter, &negotiation, &plan_error);
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
            const auto materialize_rc = MaterializeFilterTaskSessionPlan(
                plan.output_schema,
                bound_filter,
                negotiation,
                FilterTaskIsolation::kExclusive,
                &plan.filter_plan,
                &plan_error);
            if (materialize_rc != FilterPlanError::kNone) {
                if (error) {
                    *error = "transform stage " + std::to_string(i + 1) +
                             " filter task planning failed: " + plan_error;
                }
                return EIO;
            }
        }
        input_schema = plan.output_schema;
    }

    std::vector<TaskHolder> execution_holders;
    std::vector<IBlockTransformTaskV1*> execution_tasks;
    execution_holders.reserve(stage_plans.size());
    execution_tasks.reserve(stage_plans.size());
    for (size_t i = 0; i < stage_plans.size(); ++i) {
        auto& plan = stage_plans[i];
        IBlockTransformTaskV1* execution_raw = nullptr;
        std::string plan_error;
        const auto create_execution = CreateBlockTransformTaskSession(
            plan.provider,
            task_id + ".stage" + std::to_string(i + 1) + ".run",
            plan.with_params_json,
            plan.filter_plan,
            &execution_raw,
            &plan_error);
        if (create_execution != FilterPlanError::kNone) {
            for (auto* task : execution_tasks) {
                try {
                    task->Cancel();
                } catch (...) {
                }
            }
            if (error) {
                *error = "transform stage " + std::to_string(i + 1) +
                         " execution task creation failed: " + plan_error;
            }
            return EIO;
        }
        execution_tasks.push_back(execution_raw);
        execution_holders.push_back(
            make_task_holder(plan.provider, execution_raw));
    }

    std::vector<std::shared_ptr<arrow::Schema>> expected_output_schemas;
    std::vector<std::shared_ptr<const BoundFilterExpr>> residuals;
    expected_output_schemas.reserve(stage_plans.size());
    residuals.reserve(stage_plans.size());
    for (const auto& plan : stage_plans) {
        expected_output_schemas.push_back(plan.output_schema);
        residuals.push_back(plan.filter_plan.residual_expression);
    }
    SynchronousBlockTransformChainTask chain_task(
        execution_tasks, std::move(expected_output_schemas), residuals);

    BlockTransformPipelineConfig config;
    config.source = source;
    config.source_schema = source_schema;
    config.transform = &chain_task;
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
        *error = runner_error.empty() ? "multi block transform pipeline execution failed"
                                     : std::move(runner_error);
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
        sql = sql.substr(0, m.position()) +
              m[1].str() + table +
              sql.substr(m.position() + m.length());
    }

    return sql;
}

// --- 辅助：对 DataFrame 通道应用 WHERE 过滤 ---
static std::shared_ptr<DataFrameChannel> ApplyDataFrameFilter(
    IDataFrameChannel* src, const std::string& where_clause, uint64_t seq) {
    DataFrame data;
    if (src->Read(&data) != 0 || data.RowCount() == 0) return nullptr;

    if (data.Filter(where_clause.c_str()) != 0) return nullptr;

    auto filtered = std::make_shared<DataFrameChannel>("_filter", std::to_string(seq));
    filtered->Open();
    filtered->Write(&data);
    return filtered;
}

// --- 无算子：纯数据搬运 ---
int SchedulerPlugin::ExecuteTransfer(IChannel* source, IChannel* sink,
                                      const std::string& source_type,
                                      const std::string& sink_type,
                                      const SqlStatement& stmt,
                                      const std::shared_ptr<const BoundFilterExpr>& source_residual,
                                      int64_t* rows_affected,
                                      std::string* error) {
    if (source_type == ChannelType::kBlockStream && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IBlockStreamChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;

        const auto source_schema = packet::PacketSchema();
        BlockFilterStage source_filter(source_residual);
        if (source_residual) {
            std::shared_ptr<arrow::Schema> filtered_schema;
            std::string filter_error;
            const auto open_rc = source_filter.Open(
                source_schema, &filtered_schema, &filter_error);
            if (open_rc != FilterEvalError::kNone) {
                if (error) *error = "source-stage residual Open failed: " + filter_error;
                return EINVAL;
            }
        }
        int64_t rows = 0;
        bool wrote = false;
        while (true) {
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
                        const auto eval_rc = source_filter.ProcessBlock(
                            event.batch, 0, &filtered, &filter_error);
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
int SchedulerPlugin::ExecuteWithOperator(IChannel* source, IChannel* sink,
                                          IOperator* op,
                                          const std::string& sink_type,
                                          const SqlStatement& stmt, int64_t* rows_affected,
                                          std::string* error) {
    std::vector<IOperator*> ops;
    std::vector<IChannel*> inputs;
    inputs.push_back(source);
    ops.push_back(op);
    return ExecuteWithOperatorChain(Span<IChannel*>(inputs), sink, ops, sink_type, stmt, rows_affected, error);
}

int SchedulerPlugin::ExecuteWithOperatorChain(Span<IChannel*> inputs, IChannel* sink,
                                              const std::vector<IOperator*>& ops,
                                              const std::string& sink_type,
                                              const SqlStatement& stmt, int64_t* rows_affected,
                                              std::string* error) {
    if (ops.empty() || inputs.empty()) return -1;

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

        if (ops[i]->Work(stage_inputs, actual_sink) != 0) {
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
        int64_t written_rows = ChannelAdapter::WriteFromDataFrame(stage_buffers.back().get(), db_sink, table.c_str(), error);
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
    stream_manager->QueryChannels([&](const std::string& ch_type,
                                      const std::string& ch_name,
                                      const std::string& option,
                                      const std::string&) {
        if (ToLowerAscii(ch_type) != expect_type || ch_name != name) return;
        role = ReadRoleFromOption(option);
    });
    return role;
}

int32_t SchedulerPlugin::ResolveSourceBindings(const SqlStatement& stmt,
                                               SourceResolveResult* out,
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

    auto fail = [&](int32_t status,
                    const std::string& message,
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

    for (const auto& source_ref : stmt.sources) {
        ParsedChannelRef ref;
        std::string parse_err;
        if (!ParseChannelRef(source_ref, &ref, &parse_err)) {
            return fail(error::BAD_REQUEST, parse_err, ErrorCodeId::kStreamHubSelectorInvalid);
        }

        std::shared_ptr<IChannel> source_owner;
        bool source_ambiguous = false;
        IChannel* source_ch = FindChannel(ref.base, &source_owner, &source_ambiguous);
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
            if (!block_ch) return fail(error::BAD_REQUEST, "source channel cast to IBlockStreamChannel failed: " + ref.base);
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
            return fail(error::BAD_REQUEST,
                        "source channel cast to IStreamChannel failed: " + ref.base);
        }
        const std::string source_role = QueryStreamChannelRole(stream_ch->Category(), stream_ch->Name());
        if (!IsSourceRoleAllowed(source_role)) {
            return fail(error::BAD_REQUEST,
                        "stream channel role does not allow source: " +
                            std::string(stream_ch->Category()) + "." + stream_ch->Name(),
                        ErrorCodeId::kStreamChannelRoleMismatch);
        }

        if (stream_ch->IsHubChannel()) {
            if (IEquals(stream_ch->HubModeHint() ? stream_ch->HubModeHint() : "", "merge")) {
                if (ref.has_selector) {
                    return fail(error::BAD_REQUEST,
                                "stream_hub(merge) does not allow selector: " + source_ref,
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
                return fail(error::BAD_REQUEST,
                            "stream_hub(split) has no derived partitions: " + ref.base,
                            ErrorCodeId::kStreamHubSelectorInvalid);
            }
            if (partition_count > max_resolved_sources_) {
                return fail(error::BAD_REQUEST,
                            "resolved sources exceed max_resolved_sources: " +
                                std::to_string(partition_count) + " > " + std::to_string(max_resolved_sources_),
                            ErrorCodeId::kStreamHubSelectorOutOfRange);
            }

            if (!ref.has_selector || ref.wildcard_selector) {
                if (!ref.has_selector) {
                    out->source_expand_rule = "auto_wildcard";
                }
                for (size_t i = 0; i < partition_count; ++i) {
                    auto partition = stream_ch->HubPartition(i);
                    if (!partition) {
                        return fail(error::BAD_REQUEST,
                                    "stream_hub partition resolve failed: " + ref.base,
                                    ErrorCodeId::kStreamHubSelectorInvalid);
                    }
                    out->channels.push_back(partition.get());
                    out->stream_channels.push_back(partition);
                    out->source_keys.push_back(
                        MakeStreamChannelKey(partition->Category(), partition->Name()));
                    out->resolved_sources.push_back(
                        ref.base + "[" + std::to_string(i) + "]");
                }
                continue;
            }

            const int idx = ref.selector_index;
            if (idx < 0 || static_cast<size_t>(idx) >= partition_count) {
                return fail(error::BAD_REQUEST,
                            "stream_hub selector out of range: " + source_ref,
                            ErrorCodeId::kStreamHubSelectorOutOfRange);
            }
            auto partition = stream_ch->HubPartition(static_cast<size_t>(idx));
            if (!partition) {
                return fail(error::BAD_REQUEST,
                            "stream_hub partition resolve failed: " + source_ref,
                            ErrorCodeId::kStreamHubSelectorInvalid);
            }
            out->channels.push_back(partition.get());
            out->stream_channels.push_back(partition);
            out->source_keys.push_back(
                MakeStreamChannelKey(partition->Category(), partition->Name()));
            out->resolved_sources.push_back(ref.base + "[" + std::to_string(idx) + "]");
            continue;
        }

        if (ref.has_selector) {
            return fail(error::BAD_REQUEST,
                        "channel selector is only supported on stream_hub source: " + source_ref,
                        ErrorCodeId::kStreamHubSelectorInvalid);
        }
        out->channels.push_back(stream_ch);
        out->stream_channels.push_back(MakeStreamOwner(stream_ch, source_owner));
        out->source_keys.push_back(MakeStreamChannelKey(stream_ch->Category(), stream_ch->Name()));
        out->resolved_sources.push_back(ref.base);
    }

    return error::OK;
}

int32_t SchedulerPlugin::ResolveStreamSink(
    const SqlStatement& stmt,
    SinkBinding* binding,
    std::string* err_out) {
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
            *err_out = BuildExecutionErrorJson(
                "INTO stream selector is not allowed: " + stmt.dest,
                ErrorCodeId::kStreamHubSelectorNotAllowedInto,
                ErrorStageId::kSinkResolve);
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
                *err_out = BuildExecutionErrorJson(
                    "stream channel role does not allow sink: " +
                        std::string(matched->Category()) + "." + matched->Name(),
                    ErrorCodeId::kStreamChannelRoleMismatch,
                    ErrorStageId::kSinkResolve);
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
        auto* ch_registry = querier_
            ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY))
            : nullptr;
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
        if (err_out) *err_out = "invalid INTO destination: " + dest_ref.base +
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

    auto* db_factory = querier_
        ? static_cast<IDatabaseFactory*>(querier_->First(IID_DATABASE_FACTORY))
        : nullptr;
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

int32_t SchedulerPlugin::BuildStreamExecutionPlan(const SqlStatement& stmt,
                                                  const std::string& lease_owner_id,
                                                  bool skip_lease_acquire,
                                                  StreamExecutionPlan* plan,
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
        *err_rsp = BuildErrorJson(
            "sink_table is not supported for stream tasks; use INTO <db_type>.<db_name>.<table>");
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
                *err_rsp = BuildExecutionErrorJson(
                    "stream fanin capability mismatch: source=" + src_name +
                        ", reason=source must support timeout poll and lock-free poll",
                    ErrorCodeId::kStreamFaninCapabilityMismatch,
                    ErrorStageId::kFanin);
                return error::BAD_REQUEST;
            }
        }
        source = std::make_shared<FanInStreamChannel>(
            "fanin", plan->runtime_task_id + ".fanin", plan->source_channels);
    }
    plan->source = source;

    const OperatorRef& op_ref = plan->parsed_ops[0];
    plan->first_operator_holder = CreateOperator(op_ref.category, op_ref.name);
    if (!plan->first_operator_holder) {
        *err_rsp = BuildExecutionErrorJson(
            "operator create failed: " + op_ref.category + "." + op_ref.name,
            ErrorCodeId::kOpExecFail,
            ErrorStageId::kExecute);
        return error::NOT_FOUND;
    }
    plan->first_stream_operator = std::dynamic_pointer_cast<IStreamOperator>(plan->first_operator_holder);
    if (!plan->first_stream_operator) {
        *err_rsp = BuildExecutionErrorJson(
            "operator is not stream operator: " + op_ref.category + "." + op_ref.name,
            ErrorCodeId::kOpExecFail,
            ErrorStageId::kExecute);
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
            *err_rsp = BuildExecutionErrorJson(
                "stream sink cast to IStreamChannel failed",
                ErrorCodeId::kStreamSinkCapabilityMismatch,
                ErrorStageId::kCapabilityCheck);
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
        const bool consumers_ok = plan->source_caps.concurrency.max_consumers == 0 ||
                                  plan->source_caps.concurrency.max_consumers >=
                                      static_cast<uint32_t>(plan->parallelism);
        if (!poll_mode_ok || !consumers_ok) {
            *err_rsp = BuildCapabilityMismatchJson(
                "stream source capability mismatch: strategy=STATELESS, parallelism=" +
                    std::to_string(plan->parallelism) +
                    ", required.poll_mode=MULTI, actual.poll_mode=" +
                    std::string(StreamConsumerModeName(plan->source_caps.concurrency.poll_mode)) +
                    ", actual.max_consumers=" +
                    std::to_string(plan->source_caps.concurrency.max_consumers),
                ErrorCodeId::kStreamSourceCapabilityMismatch,
                &plan->source_caps,
                &plan->sink_caps);
            return error::BAD_REQUEST;
        }
    }

    if (plan->parallelism > 1) {
        const bool put_mode_ok = plan->sink_caps.concurrency.put_mode == ProducerMode::MULTI;
        const bool producers_ok = plan->sink_caps.concurrency.max_producers == 0 ||
                                  plan->sink_caps.concurrency.max_producers >=
                                      static_cast<uint32_t>(plan->parallelism);
        if (!put_mode_ok || !producers_ok) {
            *err_rsp = BuildCapabilityMismatchJson(
                "stream sink capability mismatch: strategy=" +
                    std::to_string(static_cast<int>(plan->strategy)) +
                    ", parallelism=" + std::to_string(plan->parallelism) +
                    ", required.put_mode=MULTI, actual.put_mode=" +
                    std::string(StreamProducerModeName(plan->sink_caps.concurrency.put_mode)) +
                    ", actual.max_producers=" +
                    std::to_string(plan->sink_caps.concurrency.max_producers),
                ErrorCodeId::kStreamSinkCapabilityMismatch,
                &plan->source_caps,
                &plan->sink_caps);
            return error::BAD_REQUEST;
        }
    }
    return error::OK;
}

int32_t SchedulerPlugin::AcquireStreamExecutionLease(StreamExecutionPlan* plan,
                                                     LeaseToken* lease_token,
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
    const int lease_rc = TryAcquireStreamTaskLeases(plan->runtime_task_id,
                                                    plan->source_keys,
                                                    plan->sink_keys,
                                                    &conflict_key,
                                                    &blocked_by_mutation,
                                                    plan->lease_owner_id,
                                                    &plan->version_snapshot,
                                                    &version_conflict_key);
    if (lease_rc != 0) {
        if (lease_rc == EBUSY) {
            if (blocked_by_mutation) {
                *err_rsp = BuildExecutionErrorJson(
                    "stream channel is being modified: " + conflict_key,
                    ErrorCodeId::kStreamChannelMutating,
                    ErrorStageId::kLease);
                return error::CONFLICT;
            }
            *err_rsp = BuildExecutionErrorJson(
                "stream source is in use: " + conflict_key,
                ErrorCodeId::kStreamSourceInUse,
                ErrorStageId::kLease);
            return error::CONFLICT;
        }
        if (lease_rc == EAGAIN) {
            *err_rsp = BuildExecutionErrorJson(
                "stream channel changed during execute prepare: " + version_conflict_key,
                ErrorCodeId::kStreamChannelVersionChanged,
                ErrorStageId::kLease);
            return error::CONFLICT;
        }
        *err_rsp = BuildExecutionErrorJson(
            "stream channel lease acquire failed",
            ErrorCodeId::kStreamLeaseFailed,
            ErrorStageId::kLease);
        return MapStreamManagerErrorToStatus(lease_rc);
    }

    *lease_token = LeaseToken([this, runtime_task_id = plan->runtime_task_id]() {
        ReleaseStreamTaskLeases(runtime_task_id);
    });
    return error::OK;
}

int32_t SchedulerPlugin::ExecuteStreamTask(const SqlStatement& stmt,
                                           std::string& rsp,
                                           const std::string& lease_owner_id,
                                           bool skip_lease_acquire) {
    StreamExecutionPlan plan;
    std::string err_rsp;
    int32_t rc = BuildStreamExecutionPlan(stmt,
                                          lease_owner_id,
                                          skip_lease_acquire,
                                          &plan,
                                          &err_rsp);
    if (rc != error::OK) {
        rsp = EnsureExecutionErrorJson(err_rsp,
                                       "build stream execution plan failed",
                                       ErrorCodeId::kSqlTextInvalid,
                                       ErrorStageId::kParse);
        return rc;
    }

    rc = ValidateStreamExecutionPlan(&plan, &err_rsp);
    if (rc != error::OK) {
        rsp = EnsureExecutionErrorJson(err_rsp,
                                       "validate stream execution plan failed",
                                       ErrorCodeId::kOpExecFail,
                                       ErrorStageId::kCapabilityCheck);
        return rc;
    }

    LeaseToken lease_token;
    rc = AcquireStreamExecutionLease(&plan, &lease_token, &err_rsp);
    if (rc != error::OK) {
        rsp = EnsureExecutionErrorJson(err_rsp,
                                       "acquire stream lease failed",
                                       ErrorCodeId::kStreamLeaseFailed,
                                       ErrorStageId::kLease);
        return rc;
    }

    std::shared_ptr<IStreamChannel> source = plan.source;
    std::shared_ptr<IChannel> output = plan.sink_channel;
    if (!source || !output) {
        rsp = BuildExecutionErrorJson("stream execution plan source/sink is null",
                                      ErrorCodeId::kOpExecFail,
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
        rsp = EnsureExecutionErrorJson(err_rsp,
                                       "acquire shared source subscription failed",
                                       ErrorCodeId::kSharedSourceSubscribeFailed,
                                       ErrorStageId::kSourceResolve);
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
            rsp = BuildExecutionErrorJson("stream source SetFilter failed",
                                          ErrorCodeId::kOpExecFail,
                                          ErrorStageId::kExecute);
            return error::BAD_REQUEST;
        }
        if (!unsupported.empty()) {
            std::ostringstream oss;
            oss << "WHERE pushdown not fully supported:";
            for (size_t i = 0; i < unsupported.size(); ++i) {
                oss << (i == 0 ? " " : ", ") << unsupported[i];
            }
            rsp = BuildExecutionErrorJson(oss.str(),
                                          ErrorCodeId::kOpExecFail,
                                          ErrorStageId::kExecute);
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
            input_ports.push_back(
                std::make_shared<StatelessSourceView>(shared_source_state, static_cast<uint32_t>(i)));
        }
    } else if (plan.strategy == ParallelStrategy::KEYED) {
        RingStreamChannelOptions partition_opts;
        const size_t source_cap = source->Capacity();
        if (source_cap > 0) {
            partition_opts.ring_size = NextPowerOfTwo(std::max<size_t>(64, source_cap));
        }
        fanout = std::make_shared<FanOutStreamChannel>(
            "fanout",
            plan.runtime_task_id + ".fanout",
            source,
            static_cast<size_t>(plan.parallelism),
            FanOutMode::ROUTE_BY_PARTITION_ID,
            plan.first_stream_operator->GetPartitionSpec(),
            partition_opts);
        open_target = fanout;
        for (int i = 0; i < plan.parallelism; ++i) {
            auto part = fanout->GetPartition(static_cast<size_t>(i));
            if (!part) {
                rsp = BuildExecutionErrorJson("fanout partition create failed",
                                              ErrorCodeId::kOpExecFail,
                                              ErrorStageId::kExecute);
                return error::INTERNAL_ERROR;
            }
            input_ports.push_back(std::make_shared<FanOutPartitionView>(fanout, part));
        }
    } else {
        rsp = BuildExecutionErrorJson("unsupported stream parallel strategy",
                                      ErrorCodeId::kOpExecFail,
                                      ErrorStageId::kExecute);
        return error::BAD_REQUEST;
    }

    if (input_ports.empty()) {
        rsp = BuildExecutionErrorJson("stream input ports build failed",
                                      ErrorCodeId::kOpExecFail,
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
            std::shared_ptr<IOperator> op_holder =
                CreateOperator(plan.parsed_ops[0].category, plan.parsed_ops[0].name);
            if (!op_holder) {
                rsp = BuildExecutionErrorJson("operator create failed for shard",
                                              ErrorCodeId::kOpExecFail,
                                              ErrorStageId::kExecute);
                return error::INTERNAL_ERROR;
            }
            stream_op = std::dynamic_pointer_cast<IStreamOperator>(op_holder);
        }
        if (!stream_op) {
            rsp = BuildExecutionErrorJson("stream operator cast failed for shard",
                                          ErrorCodeId::kOpExecFail,
                                          ErrorStageId::kExecute);
            return error::INTERNAL_ERROR;
        }

        const int init_rc = stream_op->Init(with_params_json.c_str(), plan.sink_ctx);
        if (init_rc != 0) {
            const std::string err = stream_op->LastError().empty()
                ? "stream operator Init failed"
                : stream_op->LastError();
            rsp = BuildExecutionErrorJson(err, ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
            return error::BAD_REQUEST;
        }

        if (static_schema) {
            const int schema_rc = stream_op->OnSchemaReady(static_schema);
            if (schema_rc != 0) {
                const std::string err = stream_op->LastError().empty()
                    ? "stream operator OnSchemaReady failed"
                    : stream_op->LastError();
                rsp = BuildExecutionErrorJson(err, ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
                return error::BAD_REQUEST;
            }
        }

        task->AddShard(std::make_shared<ShardRunner>(
            static_cast<uint32_t>(i),
            input_ports[i],
            stream_op,
            output,
            task.get(),
            static_schema != nullptr));
    }
    task->PrepareForRun(static_cast<uint32_t>(input_ports.size()), CurrentTimeMs());
    task->SetSourceResolveMeta(plan.resolved_sources, plan.source_expand_rule);

    const int open_rc = open_target->Open();
    if (open_rc != 0) {
        rsp = BuildExecutionErrorJson("open stream source failed",
                                      ErrorCodeId::kOpExecFail,
                                      ErrorStageId::kExecute);
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

int32_t SchedulerPlugin::ClassifySqlTaskKind(const std::string& sql_text,
                                             std::string* task_kind,
                                             std::string* err_rsp) {
    if (!task_kind || !err_rsp) return error::INTERNAL_ERROR;
    task_kind->clear();
    err_rsp->clear();

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

    *task_kind = source_resolved.has_stream_source ? "stream" : "batch";
    return error::OK;
}

}  // namespace scheduler
}  // namespace flowsql
