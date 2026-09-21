// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "flow_labeling_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::FlowLabelingPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_FLOW_LABELING_PROVIDER_V1, flowsql::IFlowLabelingProviderV1)
END_PLUGIN_REGIST()
