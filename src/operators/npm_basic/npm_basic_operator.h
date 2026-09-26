// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_OPERATOR_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_OPERATOR_H_

#include <common/iplugin.h>
#include <framework/interfaces/iblock_transform_operator.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace flowsql::npm {

class NpmBasicOperator;
class NpmBasicTaskRuntime;

class NpmBasicTask final : public IBlockTransformTaskV1, public IBlockTransformManagedSinkTaskV1 {
 public:
    NpmBasicTask(const NpmBasicTask&) = delete;
    NpmBasicTask& operator=(const NpmBasicTask&) = delete;
    NpmBasicTask(NpmBasicTask&&) = delete;
    NpmBasicTask& operator=(NpmBasicTask&&) = delete;

    int Open(std::shared_ptr<arrow::Schema> input_schema,
             std::shared_ptr<arrow::Schema>* output_schema) override;
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input,
                     int64_t ts_ms,
                     std::vector<BlockTransformOutputV1>* outputs) override;
    int Flush(std::vector<BlockTransformOutputV1>* outputs) override;
    void Cancel() override;
    std::string LastError() const override;
    int BindManagedSink(const BlockTransformManagedSinkBindingV1& binding) override;
    std::string ManagedSinkResultJson() const override;

    const std::string& TaskId() const noexcept;
    const std::string& WithParamsJson() const noexcept;
    const std::string& PushedFilterPlanJson() const noexcept;

 private:
    enum class State : uint8_t {
        kCreated = 0,
        kOpening,
        kOpened,
        kFlushed,
        kFailed,
        kCancelled,
    };

    friend class NpmBasicOperator;

    NpmBasicTask(const BlockTransformTaskConfigV1& config,
                 IQuerier* querier,
                 const NpmBasicOperator* owner);
    ~NpmBasicTask() override;

    static int ErrorCodeForState(State state) noexcept;
    State Fail(State expected, const char* error) noexcept;
    void SetLastErrorOnce(const char* error) noexcept;
    std::shared_ptr<NpmBasicTaskRuntime> Runtime() const noexcept;

    std::string task_id_;
    std::string with_params_json_;
    std::string pushed_filter_plan_json_;
    std::string config_error_;
    std::string managed_target_;
    std::string managed_category_;
    std::string managed_name_;
    IChannel* managed_channel_ = nullptr;
    IQuerier* querier_ = nullptr;
    const NpmBasicOperator* owner_ = nullptr;
    std::shared_ptr<NpmBasicTaskRuntime> runtime_;
    std::atomic<State> state_{State::kCreated};
    std::atomic<const char*> last_error_{nullptr};
    int64_t last_output_ts_ms_ = 0;
    int64_t last_observed_at_ns_ = 0;
    bool has_observed_at_ = false;
};

class NpmBasicOperator final : public IPlugin, public IBlockTransformOperatorV1 {
 public:
    NpmBasicOperator() noexcept = default;
    /** Creates an already-started in-process provider for focused unit tests. */
    explicit NpmBasicOperator(IQuerier* querier) noexcept;

    int Option(const char* option) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    std::string Category() const override;
    std::string Name() const override;
    std::string Description() const override;
    int CreateTask(const BlockTransformTaskConfigV1& config,
                   IBlockTransformTaskV1** task) override;
    void ReleaseTask(IBlockTransformTaskV1* task) override;

 private:
    IQuerier* querier_ = nullptr;
    bool loaded_ = false;
    bool started_ = false;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_OPERATOR_H_
