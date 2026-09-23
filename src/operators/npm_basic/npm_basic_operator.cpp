// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_operator.h"

#include <operators/npm_basic/config/npm_basic_task_config.h>
#include <operators/npm_basic/core/npm_basic_task_runtime.h>

#include <arrow/api.h>
#include <framework/interfaces/iconfig_channel_registry.h>
#include <framework/interfaces/iflow_labeling.h>
#include <plugins/npi/iprotocol.h>

#include <cerrno>
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

NpmBasicTask::NpmBasicTask(const BlockTransformTaskConfigV1& config, IQuerier* querier, const NpmBasicOperator* owner)
    : task_id_(config.task_id),
      with_params_json_(config.with_params_json),
      pushed_filter_plan_json_(config.pushed_filter_plan_json),
      querier_(querier),
      owner_(owner) {}

NpmBasicTask::~NpmBasicTask() = default;

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
    const auto parse_status = ParseNpmBasicTaskConfig(with_params_json_.c_str(), &parsed, labeling_requested);
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
    std::shared_ptr<arrow::Schema> next_schema;
    std::unique_ptr<NpmBasicTaskRuntime> next_runtime;
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

    const auto runtime_status =
        NpmBasicTaskRuntime::Create(parsed, querier_, input_schema, &next_schema, &next_runtime,
                                    std::move(labeling_budget), matcher, ProductionNpmModuleCatalogV1(), {}, task_id_);
    if (runtime_status.error != NpmBasicTaskRuntimeError::kNone) {
        expected = State::kOpening;
        expected = Fail(expected, kRuntimeOpenError);
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

    std::vector<BlockTransformOutputV1> next_outputs;
    try {
        next_outputs.reserve(1);
    } catch (const std::bad_alloc&) {
        State expected = State::kOpened;
        Fail(expected, kAllocationError);
        return -ENOMEM;
    }

    const auto runtime = Runtime();
    if (!runtime) {
        State expected = State::kOpened;
        Fail(expected, kRuntimeFlushError);
        return -EIO;
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
    if (error != nullptr) return error;
    const auto runtime = Runtime();
    return runtime ? runtime->LastError() : std::string();
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
        auto* next = new NpmBasicTask(config, querier_, this);
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

}  // namespace flowsql::npm
