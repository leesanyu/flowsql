// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>

#include "router_agency_plugin.h"

// 注册 RouterAgencyPlugin 为动态库插件
BEGIN_PLUGIN_REGIST(flowsql::router::RouterAgencyPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN_EXTERNAL_ENTRY, flowsql::IPlugin)
END_PLUGIN_REGIST()
