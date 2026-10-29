// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/icpp_operator_plugin_registry.h>

#include "binaddon_host_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::binaddon::BinAddonHostPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_BINADDON_HOST, flowsql::IBinAddonHost)
____INTERFACE(flowsql::IID_CPP_OPERATOR_PLUGIN_REGISTRY_V1, flowsql::ICppOperatorPluginRegistryV1)
____INTERFACE(flowsql::IID_CPP_OPERATOR_CONTRACT_PROVIDER_V1, flowsql::ICppOperatorContractProviderV1)
END_PLUGIN_REGIST()
