// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_BASELINE_TASK_BASELINE_TASK_BASE_H_
#define _FLOWSQL_PLUGINS_BASELINE_TASK_BASELINE_TASK_BASE_H_

#include <framework/interfaces/ibaseline_model_parameters.h>
#include <framework/interfaces/ibaseline_service.h>
#include <framework/interfaces/ibaseline_state_control.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace flowsql {
namespace baseline {

class TaskRegistry;

class BaselineTaskBase : public std::enable_shared_from_this<BaselineTaskBase>, public IBaselineModelParametersV1 {
    friend class CheckpointAccess;

 public:
    BaselineTaskBase(TaskRegistry* registry, std::string task_id, BaselineTaskKind kind, std::string task_name,
                     std::string config_json);
    virtual ~BaselineTaskBase() = default;

    const char* Id() const;
    const char* Name() const;
    BaselineTaskKind Kind() const;
    const std::string& TaskId() const;

    BaselineSerializationResult ExportConfig(BaselineSerializationFormat format) const;
    BaselineSerializationResult QueryTaskSnapshot(BaselineSerializationFormat format) const;
    BaselineSerializationResult QuerySeriesSnapshot(std::string_view series_key,
                                                    BaselineSerializationFormat format) const;
    BaselineStatus Close();
    BaselineStatus ExportModelParameters(std::string_view series_key, uint64_t max_bytes,
                                         BaselineModelParametersV1* output) const override;
    BaselineStatus BindStateLimits(const BaselineStateLimitsV1& limits);
    BaselineStatus ReleaseIdentity(std::string_view key, BaselineStateReleaseScopeV1 scope);
    std::pair<BaselineStatus, BaselineStateUsageV1> QueryStateUsage() const;

 protected:
    BaselineStatus EnsureOpen() const;
    static BaselineSerializationResult UnsupportedFormatResult(BaselineSerializationFormat format);
    virtual void OnClosing();
    virtual BaselineStatus DoReleaseIdentity(std::string_view key, BaselineStateReleaseScopeV1 scope) = 0;
    virtual BaselineStateUsageV1 DoQueryStateUsage() const = 0;
    const BaselineStateLimitsV1* StateLimits() const { return state_limits_ ? &*state_limits_ : nullptr; }
    void MarkStateOperation() { state_operations_started_ = true; }

 private:
    static const char* KindName(BaselineTaskKind kind);

    TaskRegistry* registry_ = nullptr;
    std::string task_id_;
    BaselineTaskKind kind_ = BaselineTaskKind::kValue;
    std::string task_name_;
    std::string config_json_;
    bool closed_ = false;
    bool state_operations_started_ = false;
    bool checkpoint_bound_ = false;
    bool checkpoint_restored_ = false;
    std::optional<BaselineStateLimitsV1> state_limits_;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_BASELINE_TASK_BASE_H_
