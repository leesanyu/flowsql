// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_BASELINE_BASELINE_PLUGIN_H_
#define _FLOWSQL_PLUGINS_BASELINE_BASELINE_PLUGIN_H_

#include <common/error_code.h>
#include <common/iplugin.h>
#include <framework/interfaces/ibaseline_checkpoint.h>
#include <framework/interfaces/ibaseline_service.h>
#include <framework/interfaces/ibaseline_state_control.h>

#include <memory>
#include <string>
#include <string_view>

namespace flowsql {
namespace baseline {

class TaskRegistry;

class __attribute__((visibility("default"))) BaselinePlugin : public IPlugin,
                                                              public IBaselineService,
                                                              public IBaselineStateControlServiceV1,
                                                              public IBaselineCheckpointServiceV1 {
 public:
    BaselinePlugin();
    ~BaselinePlugin() override;

    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    std::pair<BaselineStatus, std::shared_ptr<IBaselineValueTask>> CreateValueTask(
        std::string_view config_content, BaselineSerializationFormat format) override;

    std::pair<BaselineStatus, std::shared_ptr<IBaselineRatioTask>> CreateRatioTask(
        std::string_view config_content, BaselineSerializationFormat format) override;

    std::pair<BaselineStatus, std::shared_ptr<IBaselineRelationTask>> CreateRelationTask(
        std::string_view config_content, BaselineSerializationFormat format) override;

    BaselineSerializationResult QueryServiceSnapshot(BaselineSerializationFormat format) const override;

    std::pair<BaselineStatus, std::shared_ptr<IBaselineTaskStateControlV1>> Bind(
        std::shared_ptr<IBaselineTask> task, const BaselineStateLimitsV1& limits) override;
    std::pair<BaselineStatus, std::shared_ptr<IBaselineCheckpointV1>> Bind(
        std::shared_ptr<IBaselineTask> task, std::shared_ptr<IBaselineTaskStateControlV1> state_control,
        const BaselineCheckpointBindingV1& binding) override;

 private:
    IQuerier* querier_ = nullptr;
    std::string option_;
    std::string config_file_;
    bool config_strict_ = true;
    std::unique_ptr<TaskRegistry> task_registry_;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_BASELINE_PLUGIN_H_
