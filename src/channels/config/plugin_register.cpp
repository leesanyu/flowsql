// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <framework/interfaces/iconfig_channel_registry.h>

#include "config_channel_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::channels::config::ConfigChannelPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_CONFIG_CHANNEL_REGISTRY, flowsql::IConfigChannelRegistry)
____INTERFACE(flowsql::IID_DATA_SEMANTICS_REGISTRY, flowsql::IDataSemanticsRegistry)
____INTERFACE(flowsql::IID_OPERATOR_DATA_CONTRACT, flowsql::IOperatorDataContract)
____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
____INTERFACE(flowsql::IID_METADATA_DESCRIPTION, flowsql::IMetadataDescription)
END_PLUGIN_REGIST()
