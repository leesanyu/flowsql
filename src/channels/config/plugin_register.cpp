// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <framework/interfaces/iconfig_channel_registry.h>

#include "config_channel_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::channels::config::ConfigChannelPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_CONFIG_CHANNEL_REGISTRY_V1, flowsql::IConfigChannelRegistryV1)
____INTERFACE(flowsql::IID_DATA_SEMANTICS_REGISTRY_V1, flowsql::IDataSemanticsRegistryV1)
____INTERFACE(flowsql::IID_OPERATOR_DATA_CONTRACT_V1, flowsql::IOperatorDataContractV1)
____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
____INTERFACE(flowsql::IID_METADATA_DESCRIPTION_V1, flowsql::IMetadataDescriptionV1)
END_PLUGIN_REGIST()
