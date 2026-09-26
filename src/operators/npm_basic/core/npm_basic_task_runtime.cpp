// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_task_runtime.h"

#include <framework/core/packet_codec.h>

#include <arrow/api.h>
#include <arrow/util/byte_size.h>

#include <limits>
#include <new>
#include <utility>

namespace flowsql::npm {

namespace {

constexpr const char* kCancelledError = "npm.basic task was cancelled";
constexpr const char* kRealtimeModeError = "npm.basic realtime maintenance requires realtime mode";
constexpr const char* kRealtimeOutputError = "npm.basic realtime maintenance output is null";
constexpr const char* kRealtimeClockError = "npm.basic realtime monotonic clock regressed";
constexpr const char* kRealtimeSessionError = "npm.basic realtime active session snapshot failed";
constexpr const char* kRealtimeModuleError = "npm.basic realtime session end callback failed";
constexpr const char* kRealtimeProjectionError = "npm.basic realtime active result projection failed";
constexpr const char* kRealtimeWriterError = "npm.basic realtime active result collection failed";
constexpr const char* kRealtimeDrainError = "npm.basic realtime result drain failed";
constexpr const char* kRealtimeAllocationError = "npm.basic realtime maintenance allocation failed";

NpmEofFlushStatus TerminalFlushStatus(NpmEofFlushState state) {
    NpmEofFlushStatus status;
    if (state == NpmEofFlushState::kFlushed) {
        status.error = NpmEofFlushError::kAlreadyFlushed;
    } else if (state == NpmEofFlushState::kCancelled) {
        status.error = NpmEofFlushError::kCancelled;
    } else if (state == NpmEofFlushState::kFailed) {
        status.error = NpmEofFlushError::kFailedState;
    }
    return status;
}

const char* EofErrorMessage(NpmEofFlushError error) {
    switch (error) {
        case NpmEofFlushError::kNullOutput:
            return "npm.basic EOF output is null";
        case NpmEofFlushError::kNullBudget:
            return "npm.basic EOF budget is null";
        case NpmEofFlushError::kNullModule:
            return "npm.basic EOF module is null";
        case NpmEofFlushError::kSessionError:
            return "npm.basic EOF session termination failed";
        case NpmEofFlushError::kModuleError:
            return "npm.basic EOF module callback failed";
        case NpmEofFlushError::kDrainError:
            return "npm.basic EOF result drain failed";
        case NpmEofFlushError::kAllocationFailed:
            return "npm.basic EOF allocation failed";
        default:
            return "npm.basic EOF failed";
    }
}

class InputBatchLease final {
 public:
    InputBatchLease(std::shared_ptr<INpmTaskBudget> budget, uint64_t bytes)
        : budget_(std::move(budget)), bytes_(bytes) {}

    ~InputBatchLease() {
        if (reserved_) budget_->Release(NpmBudgetCategory::kInputBatch, bytes_);
    }

    NpmBudgetError Reserve() {
        const auto result = budget_->Reserve(NpmBudgetCategory::kInputBatch, bytes_);
        reserved_ = result == NpmBudgetError::kNone;
        return result;
    }

 private:
    std::shared_ptr<INpmTaskBudget> budget_;
    uint64_t bytes_ = 0;
    bool reserved_ = false;
};

}  // namespace

NpmBasicTaskRuntimeStatus NpmBasicTaskRuntime::Create(
    const NpmBasicTaskConfig& config, IQuerier* querier, const std::shared_ptr<arrow::Schema>& input_schema,
    std::shared_ptr<arrow::Schema>* output_schema, std::unique_ptr<NpmBasicTaskRuntime>* output,
    std::shared_ptr<NpmTaskBudget> budget, IFlowLabelMatcherV1* matcher, const NpmModuleCatalogV1& catalog,
    std::unique_ptr<INpmResultConsumerV1> consumer, std::string task_id,
    INpmResultConsumerFactoryV1* consumer_factory) {
    return CreateWithTimeCapabilities(config, querier, input_schema, {}, output_schema, output, std::move(budget),
                                      matcher, catalog, std::move(consumer), std::move(task_id), consumer_factory);
}

NpmBasicTaskRuntimeStatus NpmBasicTaskRuntime::CreateWithTimeCapabilities(
    const NpmBasicTaskConfig& config, IQuerier* querier, const std::shared_ptr<arrow::Schema>& input_schema,
    const NpmTimeCapabilities& time_capabilities, std::shared_ptr<arrow::Schema>* output_schema,
    std::unique_ptr<NpmBasicTaskRuntime>* output, std::shared_ptr<NpmTaskBudget> budget, IFlowLabelMatcherV1* matcher,
    const NpmModuleCatalogV1& catalog, std::unique_ptr<INpmResultConsumerV1> consumer, std::string task_id,
    INpmResultConsumerFactoryV1* consumer_factory) {
    struct ConsumerGuard {
        std::unique_ptr<INpmResultConsumerV1>& consumer;
        ~ConsumerGuard() {
            if (consumer) consumer->Cancel();
        }
    } consumer_guard{consumer};
    MatcherLease matcher_lease(matcher);
    NpmBasicTaskRuntimeStatus status;
    if (!input_schema) {
        status.error = NpmBasicTaskRuntimeError::kNullInputSchema;
        return status;
    }
    if (!output_schema) {
        status.error = NpmBasicTaskRuntimeError::kNullOutputSchema;
        return status;
    }
    if (!output) {
        status.error = NpmBasicTaskRuntimeError::kNullRuntimeOutput;
        return status;
    }
    if (consumer && consumer_factory) {
        status.error = NpmBasicTaskRuntimeError::kConsumerCreateError;
        status.consumer_error = "consumer and consumer factory are mutually exclusive";
        return status;
    }
    if (config.features.labeling_enabled != (matcher_lease != nullptr)) {
        status.error = NpmBasicTaskRuntimeError::kLabelingMatcherMissing;
        return status;
    }
    const auto packet_schema = packet::PacketSchema();
    if (!packet_schema || !input_schema->Equals(*packet_schema, true)) {
        status.error = NpmBasicTaskRuntimeError::kSchemaMismatch;
        return status;
    }

    status.time_error = ValidateNpmTimeCapabilities(config.analysis, time_capabilities);
    if (status.time_error != NpmTimeCapabilityError::kNone) {
        status.error = NpmBasicTaskRuntimeError::kTimeCapabilityError;
        return status;
    }

    try {
        std::vector<NpmPreparedModuleV1> prepared;
        status.module_status = PrepareNpmModulesV1(config, catalog, &prepared);
        if (status.module_status.error != NpmProtocolContractErrorV1::kNone) {
            status.error = NpmBasicTaskRuntimeError::kModulePlanError;
            return status;
        }
        NpmBasicTaskConfig frozen = config;
        const std::string observing =
            config.features.observing_entity.empty()
                ? (config.features.observing == NpmResultEntity::kSession ? "session" : "basic")
                : config.features.observing_entity;
        std::shared_ptr<arrow::Schema> result_schema;
        NpmModuleCapabilitiesV1 capabilities;
        capabilities.labeling_enabled = config.features.labeling_enabled;
        capabilities.labeling_available = matcher != nullptr;
        for (const auto& entry : prepared) {
            if (matcher && entry.plan.primary_label_ids) {
                for (uint32_t id : *entry.plan.primary_label_ids) {
                    FlowPrimaryLabelViewV1 label;
                    if (matcher->FindLabel(id, &label)) capabilities.available_label_ids.push_back(id);
                }
            }
            status.module_status = ValidateNpmModulePlanV1(entry.plan, capabilities);
            if (status.module_status.error != NpmProtocolContractErrorV1::kNone) {
                status.module_status.field = entry.plan.module_id + "/" + status.module_status.field;
                status.error = NpmBasicTaskRuntimeError::kModulePlanError;
                return status;
            }
            for (const auto& entity : entry.plan.entities) {
                if (entity.entity_id == observing) result_schema = entity.schema;
            }
        }
        if (!result_schema) {
            status.error = NpmBasicTaskRuntimeError::kModulePlanError;
            status.module_status = {NpmProtocolContractErrorV1::kInvalidEntity, observing};
            return status;
        }
        if (frozen.features.observing == NpmResultEntity::kProtocol) frozen.features.protocol_schema = result_schema;
        std::unique_ptr<NpmProtocolContext> protocol_context;
        status.protocol_error = NpmProtocolContext::Create(querier, &protocol_context);
        if (status.protocol_error != NpmProtocolContextError::kNone) {
            status.error = NpmBasicTaskRuntimeError::kProtocolContextError;
            return status;
        }

        if (!budget) budget = std::make_shared<NpmTaskBudget>(config.analysis);
        std::unique_ptr<NpmBasicTaskRuntime> runtime(new NpmBasicTaskRuntime(
            std::move(frozen), std::move(protocol_context), std::move(budget), std::move(matcher_lease)));
        std::vector<NpmEntityDescriptorV1> entities;
        for (const auto& entry : prepared)
            entities.insert(entities.end(), entry.plan.entities.begin(), entry.plan.entities.end());
        std::vector<NpmModuleInstanceV1> instances;
        instances.reserve(prepared.size());
        struct ProtocolInstanceGuard {
            std::vector<NpmModuleInstanceV1>& instances;
            ~ProtocolInstanceGuard() {
                for (auto& instance : instances) {
                    if (instance.protocol) instance.protocol->Abort();
                }
            }
        } protocol_instance_guard{instances};
        for (const auto& entry : prepared) {
            auto instance = entry.create(*runtime->protocol_context_, runtime->budget_);
            if ((instance.protocol && instance.analysis) ||
                (!instance.protocol && !instance.analysis && entry.plan.module_id != "basic")) {
                if (instance.protocol) instance.protocol->Abort();
                status.error = NpmBasicTaskRuntimeError::kModuleCreateError;
                status.module_status = {NpmProtocolContractErrorV1::kInvalidPlan, entry.plan.module_id};
                return status;
            }
            instances.push_back(std::move(instance));
        }
        NpmResultContextV1 context = MakeNpmResultContext(std::move(task_id));
        if (consumer_factory) {
            std::unique_ptr<INpmManagedResultConsumerV1> managed_consumer;
            const int consumer_error = consumer_factory->Create(context, entities, runtime->budget_, &managed_consumer);
            if (consumer_error != 0 || !managed_consumer) {
                status.error = NpmBasicTaskRuntimeError::kConsumerCreateError;
                status.consumer_error = consumer_factory->LastError();
                return status;
            }
            consumer = std::move(managed_consumer);
        }
        runtime->router_ = std::make_shared<NpmResultRouter>(
            std::move(entities), observing, std::move(context), runtime->budget_, std::move(consumer),
            [self = runtime.get()](const std::function<int()>& callback) {
                // The active-operation gate serializes entry while external code runs without the lifecycle lock.
                self->operation_mutex_.unlock();
                try {
                    const int error = callback();
                    self->operation_mutex_.lock();
                    return error;
                } catch (...) {
                    self->operation_mutex_.lock();
                    throw;
                }
            });
        runtime->collector_->BindRouter(runtime->router_);
        runtime->prepared_modules_ = std::move(prepared);
        for (size_t index = 0; index < runtime->prepared_modules_.size(); ++index) {
            const auto& entry = runtime->prepared_modules_[index];
            auto instance = std::move(instances[index]);
            if (instance.protocol && !instance.analysis) {
                auto adapter = std::make_unique<NpmProtocolModuleAdapter>(entry.plan, std::move(instance.protocol),
                                                                          &runtime->cancellation_requested_,
                                                                          runtime->router_.get());
                runtime->protocol_modules_.push_back(adapter.get());
                instance.analysis = std::move(adapter);
            }
            if (instance.analysis) {
                runtime->modules_.push_back(instance.analysis.get());
                runtime->owned_modules_.push_back(std::move(instance.analysis));
            }
        }
        *output_schema = std::move(result_schema);
        *output = std::move(runtime);
        return status;
    } catch (const std::bad_alloc&) {
        status.error = NpmBasicTaskRuntimeError::kAllocationFailed;
        return status;
    }
}

NpmBasicTaskRuntime::NpmBasicTaskRuntime(NpmBasicTaskConfig config,
                                         std::unique_ptr<NpmProtocolContext> protocol_context,
                                         std::shared_ptr<NpmTaskBudget> budget, MatcherLease matcher)
    : config_(std::move(config)),
      protocol_context_(std::move(protocol_context)),
      budget_(std::move(budget)),
      matcher_(std::move(matcher)),
      sessions_(std::make_unique<NpmSessionTable>(config_.analysis, budget_)),
      collector_(std::make_unique<NpmBasicResultCollector>(config_.features)),
      projector_(std::make_unique<NpmBasicResultProjector>(*protocol_context_)) {}

NpmBasicTaskRuntime::~NpmBasicTaskRuntime() {
    Cancel();
    std::unique_lock<std::mutex> lock(operation_mutex_);
    operation_done_.wait(lock, [this] { return !operation_active_.load(); });
    ReleaseResources();
}

const NpmBasicTaskConfig& NpmBasicTaskRuntime::Config() const noexcept { return config_; }

NpmBasicOfflineBatchStatus NpmBasicTaskRuntime::ProcessOfflineBatch(const std::shared_ptr<arrow::RecordBatch>& input,
                                                                    std::shared_ptr<arrow::RecordBatch>* output) {
    NpmBasicOfflineBatchStatus status;
    std::unique_lock<std::mutex> lock(operation_mutex_);
    operation_done_.wait(lock, [this] { return !operation_active_.load(); });
    status.runtime_state = state_.load(std::memory_order_acquire);
    if (status.runtime_state != NpmEofFlushState::kOpen) {
        status.error = NpmBasicOfflineBatchError::kTerminalState;
        return status;
    }
    operation_active_.store(true, std::memory_order_release);

    const auto finish = [this, &lock]() {
        operation_active_.store(false, std::memory_order_release);
        operation_done_.notify_all();
        lock.unlock();
    };
    const auto cancel = [this, &status, &finish]() {
        status.error = NpmBasicOfflineBatchError::kCancelled;
        status.runtime_state = NpmEofFlushState::kCancelled;
        eof_flusher_.Cancel();
        ReleaseResources();
        finish();
        return status;
    };
    const auto fail = [this, &status, &finish, &cancel](NpmBasicOfflineBatchError error, const char* message) {
        NpmEofFlushState expected = NpmEofFlushState::kOpen;
        if (!state_.compare_exchange_strong(expected, NpmEofFlushState::kFailed, std::memory_order_acq_rel)) {
            return cancel();
        }
        status.error = error;
        status.runtime_state = NpmEofFlushState::kFailed;
        SetLastErrorOnce(message);
        eof_flusher_.MarkFailed();
        ReleaseResources();
        finish();
        return status;
    };
    if (state_.load(std::memory_order_acquire) == NpmEofFlushState::kCancelled) return cancel();
    if (!input) {
        return fail(NpmBasicOfflineBatchError::kNullInput, "npm.basic input batch is null");
    }
    if (output == nullptr) {
        return fail(NpmBasicOfflineBatchError::kNullOutput, "npm.basic output is null");
    }

    try {
        const int64_t signed_bytes = arrow::util::TotalBufferSize(*input);
        if (signed_bytes < 0) {
            return fail(NpmBasicOfflineBatchError::kInvalidBufferSize,
                        "npm.basic input batch has a negative Arrow buffer size");
        }
        status.input_bytes = static_cast<uint64_t>(signed_bytes);

        InputBatchLease input_lease(budget_, status.input_bytes);
        status.budget_error = input_lease.Reserve();
        if (status.budget_error != NpmBudgetError::kNone) {
            return fail(NpmBasicOfflineBatchError::kInputBudgetError,
                        "npm.basic input batch budget reservation failed");
        }

        std::unique_ptr<NpmPacketBatchView> batch;
        status.batch_view_error = NpmPacketBatchView::Create(input, &batch);
        if (status.batch_view_error != NpmPacketBatchError::kNone) {
            return fail(NpmBasicOfflineBatchError::kBatchViewError, "npm.basic input batch validation failed");
        }
        if (state_.load(std::memory_order_acquire) == NpmEofFlushState::kCancelled) return cancel();

        std::vector<NpmSessionEndEvent> ended_events;
        status.process_status =
            ProcessNpmOfflinePacketBatch(config_.domains, *batch, *sessions_, *protocol_context_->Identifier(),
                                         modules_, *collector_, &ended_events, matcher_.get(), protocol_modules_);
        if (status.process_status.error != NpmPacketBatchProcessError::kNone) {
            return fail(NpmBasicOfflineBatchError::kBatchProcessError, "npm.basic offline batch processing failed");
        }
        if (state_.load(std::memory_order_acquire) == NpmEofFlushState::kCancelled) return cancel();

        std::shared_ptr<arrow::RecordBatch> next_output;
        status.drain_status = collector_->Drain(ended_events, *projector_, budget_, &next_output);
        if (status.drain_status.error != NpmBasicDrainError::kNone) {
            return fail(NpmBasicOfflineBatchError::kDrainError, "npm.basic offline batch result drain failed");
        }
        status.runtime_state = state_.load(std::memory_order_acquire);
        if (status.runtime_state == NpmEofFlushState::kCancelled) return cancel();
        *output = std::move(next_output);
        finish();
        return status;
    } catch (const std::bad_alloc&) {
        return fail(NpmBasicOfflineBatchError::kAllocationFailed, "npm.basic offline batch allocation failed");
    }
}

NpmBasicRealtimeMaintenanceStatus NpmBasicTaskRuntime::DriveRealtimeMaintenance(
    const NpmBasicRealtimeMaintenanceInput& input, std::shared_ptr<arrow::RecordBatch>* output) {
    NpmBasicRealtimeMaintenanceStatus status;
    std::unique_lock<std::mutex> lock(operation_mutex_);
    operation_done_.wait(lock, [this] { return !operation_active_.load(); });
    status.runtime_state = state_.load(std::memory_order_acquire);
    if (status.runtime_state != NpmEofFlushState::kOpen) {
        status.error = NpmBasicRealtimeMaintenanceError::kTerminalState;
        return status;
    }
    operation_active_.store(true, std::memory_order_release);

    const auto finish = [this, &lock]() {
        operation_active_.store(false, std::memory_order_release);
        operation_done_.notify_all();
        lock.unlock();
    };
    const auto cancel = [this, &status, &finish]() {
        status.error = NpmBasicRealtimeMaintenanceError::kCancelled;
        status.runtime_state = NpmEofFlushState::kCancelled;
        eof_flusher_.Cancel();
        ReleaseResources();
        finish();
        return status;
    };
    const auto fail = [this, &status, &finish, &cancel](NpmBasicRealtimeMaintenanceError error, const char* message) {
        NpmEofFlushState expected = NpmEofFlushState::kOpen;
        if (!state_.compare_exchange_strong(expected, NpmEofFlushState::kFailed, std::memory_order_acq_rel)) {
            return cancel();
        }
        status.error = error;
        status.runtime_state = NpmEofFlushState::kFailed;
        SetLastErrorOnce(message);
        eof_flusher_.MarkFailed();
        ReleaseResources();
        finish();
        return status;
    };
    const auto finish_success = [this, &status, &finish]() {
        status.runtime_state = state_.load(std::memory_order_acquire);
        const bool cancelled = status.runtime_state == NpmEofFlushState::kCancelled;
        if (cancelled) {
            eof_flusher_.Cancel();
            ReleaseResources();
            status.error = NpmBasicRealtimeMaintenanceError::kCancelled;
        }
        finish();
        return cancelled;
    };

    if (state_.load(std::memory_order_acquire) == NpmEofFlushState::kCancelled) return cancel();
    if (config_.analysis.run_mode != NpmRunMode::kRealtime) {
        return fail(NpmBasicRealtimeMaintenanceError::kInvalidRunMode, kRealtimeModeError);
    }
    if (output == nullptr) {
        return fail(NpmBasicRealtimeMaintenanceError::kNullOutput, kRealtimeOutputError);
    }
    if (realtime_clock_initialized_ && input.monotonic_now_ns < last_realtime_drive_ns_) {
        return fail(NpmBasicRealtimeMaintenanceError::kMonotonicTimeRegression, kRealtimeClockError);
    }

    try {
        if (!realtime_clock_initialized_) {
            realtime_clock_initialized_ = true;
            last_realtime_drive_ns_ = input.monotonic_now_ns;
            last_realtime_snapshot_ns_ = input.monotonic_now_ns;
        } else {
            last_realtime_drive_ns_ = input.monotonic_now_ns;
            const uint64_t elapsed =
                static_cast<uint64_t>(input.monotonic_now_ns) - static_cast<uint64_t>(last_realtime_snapshot_ns_);
            status.snapshot_due = config_.analysis.result_mode == NpmResultMode::kPeriodicSnapshot &&
                                  elapsed >= static_cast<uint64_t>(config_.analysis.output_interval_ns);
            if (status.snapshot_due) last_realtime_snapshot_ns_ = input.monotonic_now_ns;
        }

        auto progress = sessions_->AdvanceCaptureProgress(input.capture_progress);
        status.progress_disposition = progress.disposition;
        status.ended_sessions = progress.ended_sessions.size();
        status.module_error = NotifyNpmSessionEnd(progress.ended_sessions, modules_, input.observed_at_ns, *collector_);
        if (status.module_error != 0) {
            return fail(NpmBasicRealtimeMaintenanceError::kModuleError, kRealtimeModuleError);
        }
        if (progress.disposition == NpmCaptureProgressDisposition::kAdvanced) {
            const NpmModuleTimeV1 time{{progress.watermark_ns}, input.observed_at_ns};
            for (auto* module : protocol_modules_) {
                status.module_error = module->OnTime(time);
                if (status.module_error != 0) {
                    return fail(NpmBasicRealtimeMaintenanceError::kModuleError, kRealtimeModuleError);
                }
            }
        }

        if (status.snapshot_due) {
            std::vector<NpmSessionView> active;
            status.session_error = sessions_->SnapshotActive(&active);
            if (status.session_error != NpmSessionTableError::kNone) {
                return fail(NpmBasicRealtimeMaintenanceError::kSessionError, kRealtimeSessionError);
            }
            status.active_sessions = active.size();
            for (size_t index = 0; index < active.size(); ++index) {
                if (config_.features.basic_enabled) {
                    NpmBasicResult result;
                    status.projection_error = projector_->ProjectActive(active[index], input.observed_at_ns, &result);
                    if (status.projection_error != NpmBasicProjectionError::kNone) {
                        status.active_session_index = static_cast<int64_t>(index);
                        return fail(NpmBasicRealtimeMaintenanceError::kProjectionError, kRealtimeProjectionError);
                    }
                    status.writer_error = collector_->WriteBasic(result);
                    if (status.writer_error != 0) {
                        status.active_session_index = static_cast<int64_t>(index);
                        return fail(NpmBasicRealtimeMaintenanceError::kWriterError, kRealtimeWriterError);
                    }
                }
                for (auto* module : modules_) {
                    status.module_error = module->OnSessionSnapshot(active[index], input.observed_at_ns, *collector_);
                    if (status.module_error != 0) {
                        status.active_session_index = static_cast<int64_t>(index);
                        return fail(NpmBasicRealtimeMaintenanceError::kModuleError, kRealtimeModuleError);
                    }
                }
            }
        }

        const bool should_emit = !progress.ended_sessions.empty() || collector_->pending_results() != 0;
        if (!should_emit) {
            finish_success();
            return status;
        }

        std::vector<NpmSessionEndEvent> events;
        events.reserve(progress.ended_sessions.size());
        for (auto& ended : progress.ended_sessions) {
            events.push_back({std::move(ended), input.observed_at_ns});
        }
        std::shared_ptr<arrow::RecordBatch> next_output;
        status.drain_status = collector_->Drain(events, *projector_, budget_, &next_output);
        if (status.drain_status.error != NpmBasicDrainError::kNone) {
            return fail(NpmBasicRealtimeMaintenanceError::kDrainError, kRealtimeDrainError);
        }
        if (!next_output) {
            return fail(NpmBasicRealtimeMaintenanceError::kDrainError, kRealtimeDrainError);
        }

        if (finish_success()) return status;
        *output = std::move(next_output);
        status.emitted = true;
        return status;
    } catch (const std::bad_alloc&) {
        return fail(NpmBasicRealtimeMaintenanceError::kAllocationFailed, kRealtimeAllocationError);
    }
}

NpmEofFlushStatus NpmBasicTaskRuntime::FlushOffline(int64_t observed_at, std::shared_ptr<arrow::RecordBatch>* output) {
    std::unique_lock<std::mutex> lock(operation_mutex_);
    operation_done_.wait(lock, [this] { return !operation_active_.load(); });
    NpmEofFlushState current = state_.load(std::memory_order_acquire);
    if (current != NpmEofFlushState::kOpen) return TerminalFlushStatus(current);
    operation_active_.store(true, std::memory_order_release);
    if (state_.load(std::memory_order_acquire) == NpmEofFlushState::kCancelled) {
        eof_flusher_.Cancel();
        ReleaseResources();
        operation_active_.store(false, std::memory_order_release);
        operation_done_.notify_all();
        lock.unlock();
        return TerminalFlushStatus(NpmEofFlushState::kCancelled);
    }

    std::shared_ptr<arrow::RecordBatch> next_output;
    auto status = eof_flusher_.Flush(observed_at, *sessions_, modules_, protocol_modules_, *collector_, *projector_,
                                     budget_, output == nullptr ? nullptr : &next_output);
    NpmEofFlushState expected = NpmEofFlushState::kOpen;
    const NpmEofFlushState terminal =
        status.error == NpmEofFlushError::kNone ? NpmEofFlushState::kFlushed : NpmEofFlushState::kFailed;
    if (!state_.compare_exchange_strong(expected, terminal, std::memory_order_acq_rel)) {
        status = TerminalFlushStatus(expected);
        SetLastErrorOnce(kCancelledError);
    } else if (terminal == NpmEofFlushState::kFailed) {
        SetLastErrorOnce(EofErrorMessage(status.error));
    }
    ReleaseResources();
    if (output != nullptr && state_.load(std::memory_order_acquire) == NpmEofFlushState::kFlushed) {
        *output = std::move(next_output);
    }
    operation_active_.store(false, std::memory_order_release);
    operation_done_.notify_all();
    lock.unlock();
    return status;
}

void NpmBasicTaskRuntime::Cancel() noexcept {
    NpmEofFlushState expected = NpmEofFlushState::kOpen;
    if (!state_.compare_exchange_strong(expected, NpmEofFlushState::kCancelled, std::memory_order_acq_rel)) {
        return;
    }
    cancellation_requested_.store(true, std::memory_order_release);
    if (router_) router_->Cancel();
    SetLastErrorOnce(kCancelledError);
    if (operation_active_.load(std::memory_order_acquire)) return;
    if (!operation_mutex_.try_lock()) return;
    if (!operation_active_.load(std::memory_order_acquire)) {
        eof_flusher_.Cancel();
        ReleaseResources();
    }
    operation_mutex_.unlock();
}

NpmMaintenancePlanV1 NpmBasicTaskRuntime::MaintenancePlan() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    NpmMaintenancePlanV1 plan;
    if (state_.load(std::memory_order_acquire) != NpmEofFlushState::kOpen) return plan;
    plan.event_deadline_ns = sessions_->NextEventDeadlineNs();
    for (const auto* module : protocol_modules_) {
        const auto deadline = module->NextEventDeadlineNs();
        if (deadline && (!plan.event_deadline_ns || *deadline < *plan.event_deadline_ns)) {
            plan.event_deadline_ns = deadline;
        }
    }
    if (config_.analysis.run_mode == NpmRunMode::kRealtime && realtime_clock_initialized_ &&
        config_.analysis.result_mode == NpmResultMode::kPeriodicSnapshot) {
        if (last_realtime_snapshot_ns_ > std::numeric_limits<int64_t>::max() - config_.analysis.output_interval_ns) {
            plan.snapshot_deadline_monotonic_ns = std::numeric_limits<int64_t>::max();
        } else {
            plan.snapshot_deadline_monotonic_ns = last_realtime_snapshot_ns_ + config_.analysis.output_interval_ns;
        }
    }
    return plan;
}

std::string NpmBasicTaskRuntime::LastError() const {
    const char* error = last_error_.load(std::memory_order_acquire);
    return error == nullptr ? std::string() : std::string(error);
}

std::string NpmBasicTaskRuntime::ManagedResultJson() const { return router_ ? router_->ResultJson() : std::string(); }

NpmEofFlushState NpmBasicTaskRuntime::State() const noexcept { return state_.load(std::memory_order_acquire); }

void NpmBasicTaskRuntime::SetLastErrorOnce(const char* error) noexcept {
    const char* expected = nullptr;
    if (error != kCancelledError && router_ && !router_->LastError().empty()) {
        last_error_.compare_exchange_strong(expected, router_->LastError().c_str(), std::memory_order_acq_rel);
    } else {
        last_error_.compare_exchange_strong(expected, error, std::memory_order_acq_rel);
    }
}

void NpmBasicTaskRuntime::ReleaseResources() noexcept {
    if (router_) {
        const auto runtime_state = state_.load(std::memory_order_acquire);
        const std::string message = router_->LastError().empty() ? LastError() : router_->LastError();
        operation_mutex_.unlock();
        router_->Cancel();
        operation_mutex_.lock();
        if (runtime_state != NpmEofFlushState::kFlushed) {
            (void)router_->FailRun(runtime_state == NpmEofFlushState::kCancelled ? ECANCELED : EIO,
                                   runtime_state == NpmEofFlushState::kCancelled ? "cancel" : "runtime",
                                   message.empty() ? "npm.basic managed result run failed" : message);
        }
        router_->Discard();
    }
    modules_.clear();
    protocol_modules_.clear();
    owned_modules_.clear();
    prepared_modules_.clear();
    sessions_.reset();
    collector_.reset();
    projector_.reset();
    protocol_context_.reset();
    matcher_.reset();
}

NpmProtocolContext& NpmBasicTaskRuntime::ProtocolContext() noexcept { return *protocol_context_; }

std::shared_ptr<INpmTaskBudget> NpmBasicTaskRuntime::Budget() const { return budget_; }

NpmSessionTable& NpmBasicTaskRuntime::Sessions() noexcept { return *sessions_; }

NpmBasicResultCollector& NpmBasicTaskRuntime::Collector() noexcept { return *collector_; }

NpmBasicResultProjector& NpmBasicTaskRuntime::Projector() noexcept { return *projector_; }

NpmEofFlusher& NpmBasicTaskRuntime::EofFlusher() noexcept { return eof_flusher_; }

const std::vector<INpmAnalysisModule*>& NpmBasicTaskRuntime::Modules() const noexcept { return modules_; }

}  // namespace flowsql::npm
