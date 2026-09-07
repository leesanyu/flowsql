// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>

#include "gateway_plugin.h"

// 注册 GatewayPlugin 为动态库插件
BEGIN_PLUGIN_REGIST(flowsql::gateway::GatewayPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN_EXTERNAL_ENTRY, flowsql::IPlugin)
END_PLUGIN_REGIST()
