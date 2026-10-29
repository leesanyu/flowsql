// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <common/iplugin.h>
#include <common/typedef.h>

#include "bridge_plugin.h"
#include "framework/interfaces/ibridge.h"

// 注册 BridgePlugin 为动态库插件，同时暴露 IPlugin 和 IBridge 接口
BEGIN_PLUGIN_REGIST(flowsql::bridge::BridgePlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_BRIDGE, flowsql::IBridge)
____INTERFACE(flowsql::IID_PYTHON_OPERATOR_PUBLISHER_V1, flowsql::IPythonOperatorPublisherV1)
END_PLUGIN_REGIST()
