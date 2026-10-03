// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_operator.h"

#include "npm_basic_result_consumer.h"

#include <operators/npm_basic/config/npm_basic_task_config.h>
#include <operators/npm_basic/core/npm_basic_task_runtime.h>

#include <arrow/api.h>
#include <framework/interfaces/iconfig_channel_registry.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/iflow_labeling.h>
#include <plugins/npi/iprotocol.h>

#include <cerrno>
#include <chrono>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

constexpr const char* kCancelledError = "npm.basic task was cancelled";
constexpr const char* kInvalidOpenError = "npm.basic task received an invalid Open request";
constexpr const char* kRepeatedOpenError = "npm.basic task Open may only be called once";
constexpr const char* kConfigError = "npm.basic task WITH configuration is invalid";
constexpr const char* kRuntimeOpenError = "npm.basic task runtime creation failed";
constexpr const char* kInvalidProcessError = "npm.basic task received an invalid ProcessBlock request";
constexpr const char* kProcessStateError = "npm.basic task is not open for ProcessBlock";
constexpr const char* kRuntimeProcessError = "npm.basic task runtime processing failed";
constexpr const char* kInvalidFlushError = "npm.basic task received an invalid Flush request";
constexpr const char* kFlushStateError = "npm.basic task is not open for Flush";
constexpr const char* kRuntimeFlushError = "npm.basic task runtime Flush failed";
constexpr const char* kAllocationError = "npm.basic task allocation failed";
constexpr const char* kLabelingProviderError = "npm.basic labeling provider is unavailable";
constexpr const char* kLabelingConfigRegistryError = "npm.basic labeling config registry is unavailable";
constexpr const char* kLabelingConfigResolveError = "npm.basic labeling config resolve failed";
constexpr const char* kLabelingConfigMissingError = "npm.basic labeling exact config reference is required";
constexpr const char* kLabelingBudgetError = "npm.basic labeling matcher budget reservation failed";
constexpr const char* kLabelingMatcherError = "npm.basic labeling matcher creation failed";
constexpr const char* kManagedConsumerUnavailableError = "npm.basic managed result consumer is unavailable";
constexpr uint32_t kLabelingMaxLabels = 10'000;
constexpr uint32_t kLabelingMaxLogicalRules = 50'000;
constexpr uint32_t kLabelingMaxCompiledRules = 100'000;

std::string BuildConfigError(const NpmBasicTaskConfigStatus& status) {
    std::string error(kConfigError);
    if (status.error == NpmBasicTaskConfigError::kParameterSourceConflict) {
        error += ": configuration source conflict";
    } else if (status.error == NpmBasicTaskConfigError::kInvalidParameters) {
        error += ": invalid parameters";
    } else {
        error += ": invalid configuration";
    }

    if (!status.parameter_status.path.empty()) {
        error += " at ";
        error += status.parameter_status.path;
    } else if (!status.field.empty()) {
        error += " at /";
        error += status.field;
    }
    return error;
}

std::string BuildLabelingProviderError(const FlowLabelingDiagnosticV1& diagnostic) {
    std::string error(kLabelingProviderError);
    if (diagnostic.path != nullptr && diagnostic.path[0] != '\0') {
        error += " at ";
        error += diagnostic.path;
    }
    if (diagnostic.detail != nullptr && diagnostic.detail[0] != '\0') {
        error += ": ";
        error += diagnostic.detail;
    }
    return error;
}

std::string BuildLabelingError(const char* prefix, const char* path, const char* detail) {
    std::string error(prefix);
    if (path != nullptr && path[0] != '\0') {
        error += " at ";
        error += path;
    }
    if (detail != nullptr && detail[0] != '\0') {
        error += ": ";
        error += detail;
    }
    return error;
}

bool MaxPacketTimestampNs(const std::shared_ptr<arrow::RecordBatch>& input, int64_t* output) {
    if (!input || input->num_rows() == 0 || input->num_columns() == 0 || output == nullptr) {
        return false;
    }
    const auto timestamps = std::dynamic_pointer_cast<arrow::Int64Array>(input->column(0));
    if (!timestamps || timestamps->length() != input->num_rows()) return false;

    int64_t maximum = timestamps->Value(0);
    for (int64_t row = 1; row < timestamps->length(); ++row) {
        if (timestamps->Value(row) > maximum) maximum = timestamps->Value(row);
    }
    *output = maximum;
    return true;
}

}  // namespace

NpmBasicTask::NpmBasicTask(const BlockTransformTaskConfigV1& config, IQuerier* querier, const NpmBasicOperator* owner,
                           bool v2)
    : task_id_(config.task_id),
      with_params_json_(config.with_params_json),
      pushed_filter_plan_json_(config.pushed_filter_plan_json),
      querier_(querier),
      owner_(owner),
      v2_(v2) {}

NpmBasicTask::~NpmBasicTask() = default;

int NpmBasicTask::BindInputSource(const char* source) {
    if (state_.load(std::memory_order_acquire) != State::kCreated || !input_source_.empty()) return EALREADY;
    if (source == nullptr || source[0] == '\0') return EINVAL;
    try {
        input_source_ = source;
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int NpmBasicTask::BindCaptureSource(const CaptureQueueIdentityV1& identity) {
    if (!v2_) return ENOTSUP;
    if (state_.load(std::memory_order_acquire) != State::kCreated || capture_bound_) return EALREADY;
    if (identity.struct_size < sizeof(CaptureQueueIdentityV1) ||
        identity.contract_version != kCaptureBlockStreamContractVersionV1 || !identity.source_name ||
        identity.source_name[0] == '\0' || identity.generation == 0) {
        return EINVAL;
    }
    try {
        capture_source_name_ = identity.source_name;
        capture_identity_ = identity;
        capture_identity_.source_name = capture_source_name_.c_str();
        capture_bound_ = true;
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int NpmBasicTask::AcceptCaptureFact(const CaptureProgressV1& fact) {
    if (state_.load(std::memory_order_acquire) != State::kOpened || !v2_ || !capture_bound_) return EPIPE;
    if (fact.struct_size < sizeof(CaptureProgressV1) || fact.contract_version != kCaptureBlockStreamContractVersionV1 ||
        fact.source_id != capture_identity_.source_id || fact.queue_id != capture_identity_.queue_id ||
        fact.generation != capture_identity_.generation || fact.fact_sequence == 0 ||
        fact.fact_sequence <= last_capture_fact_sequence_ || fact.capture_time_ns < 0 ||
        (fact.packet_observed && fact.source_idle_confirmed) ||
        (fact.backlog != CaptureBacklogV1::kUnknown && fact.backlog != CaptureBacklogV1::kEmpty &&
         fact.backlog != CaptureBacklogV1::kPresent)) {
        return EINVAL;
    }
    try {
        pending_capture_facts_.push_back(fact);
        last_capture_fact_sequence_ = fact.fact_sequence;
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int NpmBasicTask::GetTimeDriveState(BlockTransformTimeDriveStateV1* state) {
    if (!state || state->struct_size < kBlockTransformTimeDriveStateV1Size ||
        state->contract_version != kBlockTransformTimeDriveVersionV1) {
        return EINVAL;
    }
    *state = BlockTransformTimeDriveStateV1{};
    state->struct_size = kBlockTransformTimeDriveStateV1Size;
    state->contract_version = kBlockTransformTimeDriveVersionV1;
    const auto runtime = Runtime();
    if (state_.load(std::memory_order_acquire) != State::kOpened || !runtime) return EPIPE;
    if (runtime->Config().analysis.run_mode != NpmRunMode::kRealtime) return 0;
    if (!realtime_origin_initialized_ || !pending_capture_facts_.empty()) {
        state->armed = 1;
        state->deadline_ns = 0;
        return 0;
    }
    const auto plan = runtime->MaintenancePlan();
    if (plan.snapshot_deadline_monotonic_ns) {
        state->armed = 1;
        state->deadline_ns = *plan.snapshot_deadline_monotonic_ns;
    }
    return 0;
}

int NpmBasicTask::OnTime(const BlockTransformTimeEventV1& event, std::vector<BlockTransformOutputV1>* outputs) {
    if (!outputs || !outputs->empty() || event.struct_size < kBlockTransformTimeEventV1Size ||
        event.contract_version != kBlockTransformTimeDriveVersionV1 || event.monotonic_now_ns < 0) {
        return -EINVAL;
    }
    const auto runtime = Runtime();
    if (state_.load(std::memory_order_acquire) != State::kOpened || !runtime ||
        runtime->Config().analysis.run_mode != NpmRunMode::kRealtime) {
        return -EPIPE;
    }
    std::vector<BlockTransformOutputV1> next_outputs;
    const auto drive = [&](const CaptureProgressV1* fact) {
        NpmBasicRealtimeMaintenanceInput input;
        input.monotonic_now_ns = event.monotonic_now_ns;
        input.observed_at_ns = event.wall_now_ns;
        if (fact) {
            input.capture_progress.capture_time_ns = fact->capture_time_ns;
            input.capture_progress.packet_observed = fact->packet_observed;
            input.capture_progress.source_idle_confirmed = fact->source_idle_confirmed;
            input.capture_progress.source_backlog_known = fact->backlog != CaptureBacklogV1::kUnknown;
            input.capture_progress.source_has_backlog = fact->backlog == CaptureBacklogV1::kPresent;
        }
        std::shared_ptr<arrow::RecordBatch> batch;
        const auto status = runtime->DriveRealtimeMaintenance(input, &batch);
        if (status.error != NpmBasicRealtimeMaintenanceError::kNone) return false;
        if (batch) next_outputs.push_back({std::move(batch), event.wall_now_ns / 1'000'000});
        return true;
    };
    const auto fail_maintenance = [&](int code, const char* message) {
        State expected = State::kOpened;
        Fail(expected, message);
        return expected == State::kCancelled ? -ECANCELED : code;
    };
    try {
        if (pending_capture_facts_.empty() && !drive(nullptr)) {
            return fail_maintenance(-EIO, "npm.basic realtime maintenance failed");
        }
        while (!pending_capture_facts_.empty()) {
            if (!drive(&pending_capture_facts_.front())) {
                return fail_maintenance(-EIO, "npm.basic realtime maintenance failed");
            }
            pending_capture_facts_.pop_front();
        }
    } catch (const std::bad_alloc&) {
        return fail_maintenance(-ENOMEM, kAllocationError);
    }
    realtime_origin_initialized_ = true;
    last_observed_at_ns_ = event.wall_now_ns;
    has_observed_at_ = true;
    last_output_ts_ms_ = event.wall_now_ns / 1'000'000;
    *outputs = std::move(next_outputs);
    return static_cast<int>(BlockTransformStatusV1::kContinue);
}

int NpmBasicTask::BindManagedSink(const BlockTransformManagedSinkBindingV1& binding) {
    if (state_.load(std::memory_order_acquire) != State::kCreated || managed_channel_ != nullptr) return EALREADY;
    if (binding.struct_size != sizeof(BlockTransformManagedSinkBindingV1) ||
        binding.contract_version != kBlockTransformManagedSinkContractVersionV1 || !binding.sink_channel ||
        !binding.target || !binding.category || !binding.name || !binding.relation) {
        return EINVAL;
    }
    auto* db = dynamic_cast<IDatabaseChannel*>(binding.sink_channel);
    if (!db || !db->IsOpened() || !db->IsConnected() || binding.relation[0] != '\0') return EINVAL;
    const std::string category(binding.category);
    const std::string name(binding.name);
    const std::string target(binding.target);
    if (name.empty() ||
        (category != "sqlite" && category != "mysql" && category != "postgres" && category != "clickhouse") ||
        target != category + "." + name || category != db->Category() || name != db->Name()) {
        return EINVAL;
    }
    managed_target_ = target;
    managed_category_ = category;
    managed_name_ = name;
    managed_channel_ = binding.sink_channel;
    return 0;
}

std::string NpmBasicTask::ManagedSinkResultJson() const {
    const auto runtime = Runtime();
    return runtime ? runtime->ManagedResultJson() : std::string();
}

int NpmBasicTask::Open(std::shared_ptr<arrow::Schema> input_schema, std::shared_ptr<arrow::Schema>* output_schema) {
    State expected = State::kCreated;
    if (!state_.compare_exchange_strong(expected, State::kOpening, std::memory_order_acq_rel)) {
        if (expected == State::kOpened) {
            Fail(expected, kRepeatedOpenError);
        } else if (expected != State::kCancelled) {
            SetLastErrorOnce(kRepeatedOpenError);
        }
        return expected == State::kCancelled ? ECANCELED : EALREADY;
    }
    if (!input_schema || output_schema == nullptr) {
        expected = State::kOpening;
        expected = Fail(expected, kInvalidOpenError);
        return expected == State::kCancelled ? ECANCELED : EINVAL;
    }

    const bool labeling_requested = NpmBasicTaskRequestsLabeling(with_params_json_.c_str());
    IFlowLabelingProviderV1* labeling_provider = nullptr;
    if (labeling_requested) {
        labeling_provider = querier_ == nullptr
                                ? nullptr
                                : static_cast<IFlowLabelingProviderV1*>(querier_->First(IID_FLOW_LABELING_PROVIDER_V1));
        FlowLabelingDiagnosticV1 diagnostic;
        const auto provider_status = labeling_provider == nullptr ? FlowLabelingErrorV1::kUnavailable
                                                                  : labeling_provider->RuntimeStatus(&diagnostic);
        if (provider_status != FlowLabelingErrorV1::kNone) {
            const char* error = kLabelingProviderError;
            if (labeling_provider != nullptr) {
                try {
                    config_error_ = BuildLabelingProviderError(diagnostic);
                    error = config_error_.c_str();
                } catch (const std::bad_alloc&) {
                }
            }
            expected = State::kOpening;
            expected = Fail(expected, error);
            return expected == State::kCancelled ? ECANCELED : ENODEV;
        }
    }

    NpmBasicTaskConfig parsed;
    const auto parse_status =
        ParseNpmBasicTaskConfig(with_params_json_.c_str(), &parsed, labeling_requested, ProductionNpmModuleCatalogV1(),
                                input_source_.empty() ? task_id_ : input_source_);
    if (parse_status.error != NpmBasicTaskConfigError::kNone) {
        const char* error = kConfigError;
        try {
            config_error_ = BuildConfigError(parse_status);
            error = config_error_.c_str();
        } catch (const std::bad_alloc&) {
        }
        expected = State::kOpening;
        expected = Fail(expected, error);
        return expected == State::kCancelled ? ECANCELED : EINVAL;
    }
    if (!parsed.run_mode_explicit) {
        parsed.analysis.run_mode = capture_bound_ ? NpmRunMode::kRealtime : NpmRunMode::kOffline;
    } else if (v2_ && (parsed.analysis.run_mode == NpmRunMode::kRealtime) != capture_bound_) {
        expected = State::kOpening;
        expected = Fail(expected, "npm.basic run_mode conflicts with bound source capabilities");
        return expected == State::kCancelled ? ECANCELED : EINVAL;
    }
    NpmTimeCapabilities time_capabilities;
    if (v2_ && parsed.analysis.run_mode == NpmRunMode::kRealtime) {
        const bool schema_probe = task_id_.size() >= 7 && task_id_.compare(task_id_.size() - 7, 7, ".schema") == 0;
        if (!capture_bound_ && !schema_probe) {
            expected = State::kOpening;
            expected = Fail(expected, "npm.basic realtime capture source is not bound");
            return expected == State::kCancelled ? ECANCELED : EINVAL;
        }
        if (capture_bound_) {
            uint64_t observation_domain_id = 0;
            if (ResolveNpmObservationDomain(parsed.domains, capture_identity_.source_id, &observation_domain_id) !=
                    NpmObservationDomainError::kNone ||
                observation_domain_id != capture_identity_.observation_domain_id) {
                expected = State::kOpening;
                expected = Fail(expected, "npm.basic realtime source domain conflicts with capture identity");
                return expected == State::kCancelled ? ECANCELED : EINVAL;
            }
        }
        time_capabilities = {true, true, true, true};
    }
    if (parsed.result_retention_days.has_value() && managed_channel_ == nullptr) {
        expected = State::kOpening;
        expected = Fail(expected, "npm.basic result retention requires a managed database sink");
        return expected == State::kCancelled ? ECANCELED : EINVAL;
    }
    std::shared_ptr<arrow::Schema> next_schema;
    std::unique_ptr<NpmBasicTaskRuntime> next_runtime;
    std::unique_ptr<INpmResultConsumerFactoryV1> managed_consumer_factory;
    if (managed_channel_ != nullptr) {
        auto* database = dynamic_cast<IDatabaseChannel*>(managed_channel_);
        if (!database) {
            expected = State::kOpening;
            expected = Fail(expected, kManagedConsumerUnavailableError);
            return expected == State::kCancelled ? ECANCELED : ENOTSUP;
        }
        managed_consumer_factory = MakeNpmDatabaseResultConsumerFactory(database, parsed.domains.input_namespace,
                                                                        parsed.result_retention_days);
    }
    std::shared_ptr<NpmTaskBudget> labeling_budget;
    IFlowLabelMatcherV1* matcher = nullptr;
    if (parsed.features.labeling_enabled) {
        const uint64_t labeling_memory_bytes = static_cast<uint64_t>(parsed.labeling_memory_mib) * kNpmMebibyte;
        if (parsed.labeling_reference.empty()) {
            expected = State::kOpening;
            expected = Fail(expected, kLabelingConfigMissingError);
            return expected == State::kCancelled ? ECANCELED : EINVAL;
        }
        auto* registry = querier_ == nullptr
                             ? nullptr
                             : static_cast<IConfigChannelRegistryV1*>(querier_->First(IID_CONFIG_CHANNEL_REGISTRY_V1));
        if (registry == nullptr) {
            expected = State::kOpening;
            expected = Fail(expected, kLabelingConfigRegistryError);
            return expected == State::kCancelled ? ECANCELED : ENODEV;
        }

        ConfigChannelSnapshot snapshot;
        std::string resolve_error;
        const int resolve_status = registry->Resolve(parsed.labeling_reference.c_str(), &snapshot, &resolve_error);
        if (resolve_status != 0) {
            const char* error = kLabelingConfigResolveError;
            try {
                config_error_ =
                    BuildLabelingError(kLabelingConfigResolveError, "/core/labeling", resolve_error.c_str());
                error = config_error_.c_str();
            } catch (const std::bad_alloc&) {
            }
            expected = State::kOpening;
            expected = Fail(expected, error);
            return expected == State::kCancelled ? ECANCELED : EINVAL;
        }

        try {
            labeling_budget = std::make_shared<NpmTaskBudget>(parsed.analysis);
        } catch (const std::bad_alloc&) {
            expected = State::kOpening;
            expected = Fail(expected, kAllocationError);
            return expected == State::kCancelled ? ECANCELED : ENOMEM;
        }
        if (labeling_budget->Reserve(NpmBudgetCategory::kModuleState, labeling_memory_bytes) != NpmBudgetError::kNone) {
            expected = State::kOpening;
            expected = Fail(expected, kLabelingBudgetError);
            return expected == State::kCancelled ? ECANCELED : ENOSPC;
        }

        FlowLabelingCompileRequestV1 request;
        request.snapshot = &snapshot;
        request.reserved_module_state_bytes = labeling_memory_bytes;
        request.max_labels = kLabelingMaxLabels;
        request.max_logical_rules = kLabelingMaxLogicalRules;
        request.max_compiled_rules = kLabelingMaxCompiledRules;
        FlowLabelingDiagnosticV1 diagnostic;
        const auto matcher_status = labeling_provider->CreateMatcher(request, &matcher, &diagnostic);
        if (matcher_status != FlowLabelingErrorV1::kNone || matcher == nullptr) {
            const char* error = kLabelingMatcherError;
            try {
                config_error_ = BuildLabelingError(kLabelingMatcherError, diagnostic.path, diagnostic.detail);
                error = config_error_.c_str();
            } catch (const std::bad_alloc&) {
            }
            expected = State::kOpening;
            expected = Fail(expected, error);
            return expected == State::kCancelled ? ECANCELED : EINVAL;
        }
    }

    const auto runtime_status = NpmBasicTaskRuntime::CreateWithTimeCapabilities(
        parsed, querier_, input_schema, time_capabilities, &next_schema, &next_runtime, std::move(labeling_budget),
        matcher, ProductionNpmModuleCatalogV1(), {}, task_id_, managed_consumer_factory.get());
    if (runtime_status.error != NpmBasicTaskRuntimeError::kNone) {
        expected = State::kOpening;
        if (!runtime_status.consumer_error.empty()) config_error_ = runtime_status.consumer_error;
        expected = Fail(expected, config_error_.empty() ? kRuntimeOpenError : config_error_.c_str());
        return expected == State::kCancelled ? ECANCELED : EINVAL;
    }

    try {
        std::shared_ptr<NpmBasicTaskRuntime> published(std::move(next_runtime));
        std::atomic_store_explicit(&runtime_, published, std::memory_order_release);
        expected = State::kOpening;
        if (!state_.compare_exchange_strong(expected, State::kOpened, std::memory_order_acq_rel)) {
            published->Cancel();
            return expected == State::kCancelled ? ECANCELED : EINVAL;
        }
        *output_schema = std::move(next_schema);
        return 0;
    } catch (const std::bad_alloc&) {
        expected = State::kOpening;
        expected = Fail(expected, kAllocationError);
        return expected == State::kCancelled ? ECANCELED : ENOMEM;
    }
}

int NpmBasicTask::ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t ts_ms,
                               std::vector<BlockTransformOutputV1>* outputs) {
    State current = state_.load(std::memory_order_acquire);
    if (current != State::kOpened) {
        if (current == State::kCreated) {
            current = Fail(current, kProcessStateError);
        } else if (current != State::kCancelled) {
            SetLastErrorOnce(kProcessStateError);
        }
        return ErrorCodeForState(current);
    }
    if (!input || outputs == nullptr || !outputs->empty()) {
        if (outputs != nullptr) outputs->clear();
        State expected = State::kOpened;
        expected = Fail(expected, kInvalidProcessError);
        return expected == State::kCancelled ? -ECANCELED : -EINVAL;
    }

    const auto runtime = Runtime();
    if (!runtime) {
        State expected = State::kOpened;
        Fail(expected, kRuntimeProcessError);
        return -EIO;
    }

    std::shared_ptr<arrow::RecordBatch> batch;
    const auto status = runtime->ProcessOfflineBatch(input, &batch);
    if (status.error != NpmBasicOfflineBatchError::kNone) {
        if (status.error == NpmBasicOfflineBatchError::kCancelled ||
            status.runtime_state == NpmEofFlushState::kCancelled) {
            State expected = State::kOpened;
            state_.compare_exchange_strong(expected, State::kCancelled, std::memory_order_acq_rel);
            SetLastErrorOnce(kCancelledError);
            return -ECANCELED;
        }
        State expected = State::kOpened;
        if (state_.compare_exchange_strong(expected, State::kFailed, std::memory_order_acq_rel)) {
            SetLastErrorOnce(kRuntimeProcessError);
        }
        return expected == State::kCancelled ? -ECANCELED : -EIO;
    }
    if (!batch) {
        State expected = State::kOpened;
        Fail(expected, kRuntimeProcessError);
        return -EIO;
    }

    std::vector<BlockTransformOutputV1> next_outputs;
    try {
        next_outputs.push_back({std::move(batch), ts_ms});
    } catch (const std::bad_alloc&) {
        State expected = State::kOpened;
        Fail(expected, kAllocationError);
        return -ENOMEM;
    }

    current = state_.load(std::memory_order_acquire);
    if (current != State::kOpened) return ErrorCodeForState(current);

    int64_t batch_observed_at = 0;
    if (MaxPacketTimestampNs(input, &batch_observed_at) &&
        (!has_observed_at_ || batch_observed_at > last_observed_at_ns_)) {
        last_observed_at_ns_ = batch_observed_at;
        has_observed_at_ = true;
    }
    last_output_ts_ms_ = ts_ms;
    *outputs = std::move(next_outputs);
    return static_cast<int>(BlockTransformStatusV1::kContinue);
}

int NpmBasicTask::Flush(std::vector<BlockTransformOutputV1>* outputs) {
    State current = state_.load(std::memory_order_acquire);
    if (current != State::kOpened) {
        if (current == State::kCreated) {
            current = Fail(current, kFlushStateError);
        } else if (current != State::kCancelled) {
            SetLastErrorOnce(kFlushStateError);
        }
        return ErrorCodeForState(current);
    }
    if (outputs == nullptr || !outputs->empty()) {
        if (outputs != nullptr) outputs->clear();
        State expected = State::kOpened;
        expected = Fail(expected, kInvalidFlushError);
        return expected == State::kCancelled ? -ECANCELED : -EINVAL;
    }

    const auto runtime = Runtime();
    if (!runtime) {
        State expected = State::kOpened;
        Fail(expected, kRuntimeFlushError);
        return -EIO;
    }

    std::vector<BlockTransformOutputV1> next_outputs;
    if (!pending_capture_facts_.empty()) {
        // Stop can arrive after AcceptCaptureFact and before the runner's next OnTime.
        BlockTransformTimeEventV1 event{};
        event.struct_size = kBlockTransformTimeEventV1Size;
        event.contract_version = kBlockTransformTimeDriveVersionV1;
        event.monotonic_now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        event.wall_now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        const int rc = OnTime(event, &next_outputs);
        if (rc != 0) return rc;
    }
    try {
        next_outputs.reserve(next_outputs.size() + 1);
    } catch (const std::bad_alloc&) {
        State expected = State::kOpened;
        Fail(expected, kAllocationError);
        return -ENOMEM;
    }

    std::shared_ptr<arrow::RecordBatch> batch;
    const auto status = runtime->FlushOffline(has_observed_at_ ? last_observed_at_ns_ : 0, &batch);
    if (status.error != NpmEofFlushError::kNone) {
        if (status.error == NpmEofFlushError::kCancelled) {
            State expected = State::kOpened;
            state_.compare_exchange_strong(expected, State::kCancelled, std::memory_order_acq_rel);
            SetLastErrorOnce(kCancelledError);
            return -ECANCELED;
        }
        State expected = State::kOpened;
        if (state_.compare_exchange_strong(expected, State::kFailed, std::memory_order_acq_rel)) {
            SetLastErrorOnce(kRuntimeFlushError);
        }
        return expected == State::kCancelled ? -ECANCELED : -EIO;
    }
    if (!batch) {
        State expected = State::kOpened;
        Fail(expected, kRuntimeFlushError);
        return -EIO;
    }
    next_outputs.push_back({std::move(batch), last_output_ts_ms_});

    State expected = State::kOpened;
    if (!state_.compare_exchange_strong(expected, State::kFlushed, std::memory_order_acq_rel)) {
        return ErrorCodeForState(expected);
    }
    *outputs = std::move(next_outputs);
    return 0;
}

void NpmBasicTask::Cancel() {
    State current = state_.load(std::memory_order_acquire);
    while (current == State::kCreated || current == State::kOpening || current == State::kOpened) {
        if (state_.compare_exchange_weak(current, State::kCancelled, std::memory_order_acq_rel)) {
            SetLastErrorOnce(kCancelledError);
            break;
        }
    }
    if (current != State::kCancelled && state_.load(std::memory_order_acquire) != State::kCancelled) {
        return;
    }
    const auto runtime = Runtime();
    if (runtime) runtime->Cancel();
}

std::string NpmBasicTask::LastError() const {
    const char* error = last_error_.load(std::memory_order_acquire);
    if (error != nullptr && error != kRuntimeProcessError) return error;
    const auto runtime = Runtime();
    const auto detail = runtime ? runtime->LastError() : std::string();
    return detail.empty() && error != nullptr ? std::string(error) : detail;
}

const std::string& NpmBasicTask::TaskId() const noexcept { return task_id_; }

const std::string& NpmBasicTask::WithParamsJson() const noexcept { return with_params_json_; }

const std::string& NpmBasicTask::PushedFilterPlanJson() const noexcept { return pushed_filter_plan_json_; }

int NpmBasicTask::ErrorCodeForState(State state) noexcept { return state == State::kCancelled ? -ECANCELED : -EPIPE; }

NpmBasicTask::State NpmBasicTask::Fail(State expected, const char* error) noexcept {
    if (!state_.compare_exchange_strong(expected, State::kFailed, std::memory_order_acq_rel)) {
        return expected;
    }
    SetLastErrorOnce(error);
    const auto runtime = Runtime();
    if (runtime) runtime->Cancel();
    return State::kFailed;
}

void NpmBasicTask::SetLastErrorOnce(const char* error) noexcept {
    const char* expected = nullptr;
    last_error_.compare_exchange_strong(expected, error, std::memory_order_acq_rel);
}

std::shared_ptr<NpmBasicTaskRuntime> NpmBasicTask::Runtime() const noexcept {
    return std::atomic_load_explicit(&runtime_, std::memory_order_acquire);
}

NpmBasicOperator::NpmBasicOperator(IQuerier* querier) noexcept : querier_(querier), loaded_(true), started_(true) {}

int NpmBasicOperator::Option(const char* option) {
    if (loaded_ || started_) return EBUSY;
    return option == nullptr || option[0] == '\0' ? 0 : EINVAL;
}

int NpmBasicOperator::Load(IQuerier* querier) {
    if (querier == nullptr) return EINVAL;
    if (loaded_ || started_) return EALREADY;
    querier_ = querier;
    loaded_ = true;
    return 0;
}

int NpmBasicOperator::Unload() {
    started_ = false;
    loaded_ = false;
    querier_ = nullptr;
    return 0;
}

int NpmBasicOperator::Start() {
    if (!loaded_ || querier_ == nullptr) return EINVAL;
    if (started_) return 0;

    auto* pool = static_cast<IProtocolPipelinePoolV1*>(querier_->First(IID_PROTOCOL_PIPELINE_POOL_V1));
    if (pool == nullptr || pool->Capacity() < 1) return ENODEV;
    IProtocol* protocol = pool->Protocol();
    if (protocol == nullptr || protocol->Dictionary() == nullptr) return ENODEV;

    started_ = true;
    return 0;
}

int NpmBasicOperator::Stop() {
    started_ = false;
    return 0;
}

std::string NpmBasicOperator::Category() const { return "npm"; }

std::string NpmBasicOperator::Name() const { return "basic"; }

std::string NpmBasicOperator::Description() const { return "NPM basic session analysis"; }

int NpmBasicOperator::CreateTask(const BlockTransformTaskConfigV1& config, IBlockTransformTaskV1** task) {
    if (!started_) return EPIPE;
    if (task == nullptr || config.contract_version != kBlockTransformContractVersionV1 || config.task_id == nullptr ||
        config.task_id[0] == '\0' || config.with_params_json == nullptr || config.with_params_json[0] == '\0' ||
        config.pushed_filter_plan_json == nullptr || config.pushed_filter_plan_json[0] == '\0') {
        return EINVAL;
    }

    try {
        auto* next = new NpmBasicTask(config, querier_, this, false);
        *task = next;
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

void NpmBasicOperator::ReleaseTask(IBlockTransformTaskV1* task) {
    auto* owned = dynamic_cast<NpmBasicTask*>(task);
    if (owned == nullptr || owned->owner_ != this) return;
    delete owned;
}

int NpmBasicOperator::CreateTask(const BlockTransformTaskConfigV2& config, IBlockTransformTaskV2** task) {
    if (!started_) return EPIPE;
    if (!task || config.struct_size < kBlockTransformTaskConfigV2Size ||
        config.contract_version != kBlockTransformContractVersionV2 || !config.task_id || !config.task_id[0] ||
        !config.with_params_json || !config.with_params_json[0] || !config.pushed_filter_plan_json ||
        !config.pushed_filter_plan_json[0]) {
        return EINVAL;
    }
    try {
        BlockTransformTaskConfigV1 v1;
        v1.task_id = config.task_id;
        v1.with_params_json = config.with_params_json;
        v1.pushed_filter_plan_json = config.pushed_filter_plan_json;
        *task = new NpmBasicTask(v1, querier_, this, true);
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

void NpmBasicOperator::ReleaseTask(IBlockTransformTaskV2* task) {
    ReleaseTask(static_cast<IBlockTransformTaskV1*>(task));
}

}  // namespace flowsql::npm
