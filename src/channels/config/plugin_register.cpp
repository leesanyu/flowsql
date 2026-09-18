// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <framework/interfaces/iconfig_channel_registry.h>

#include "config_channel_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::channels::config::ConfigChannelPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_CONFIG_CHANNEL_REGISTRY_V1, flowsql::IConfigChannelRegistryV1)
    ____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
END_PLUGIN_REGIST()
