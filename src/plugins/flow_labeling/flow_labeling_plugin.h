// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_FLOW_LABELING_FLOW_LABELING_PLUGIN_H_
#define _FLOWSQL_PLUGINS_FLOW_LABELING_FLOW_LABELING_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/iflow_labeling.h>

#include <cstdint>
#include <mutex>

namespace flowsql {

class FlowLabelingPlugin final : public IPlugin, public IFlowLabelingProviderV1 {
 public:
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    FlowLabelingErrorV1 RuntimeStatus(FlowLabelingDiagnosticV1* diagnostic) const override;
    FlowLabelingErrorV1 CreateMatcher(const FlowLabelingCompileRequestV1& request, IFlowLabelMatcherV1** output,
                                      FlowLabelingDiagnosticV1* diagnostic) override;

 private:
    class Matcher;

    void ReleaseMatcher() noexcept;

    mutable std::mutex mutex_;
    bool loaded_ = false;
    bool ready_ = false;
    bool eal_initialized_ = false;
    uint64_t active_matchers_ = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_FLOW_LABELING_FLOW_LABELING_PLUGIN_H_
