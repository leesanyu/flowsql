// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ioperator_registry.h>
#include <framework/interfaces/irouter_handle.h>

#include "catalog_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::catalog::CatalogPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_CHANNEL_REGISTRY, flowsql::IChannelRegistry)
____INTERFACE(flowsql::IID_DATAFRAME_SNAPSHOT_PROVIDER_V1, flowsql::IDataFrameSnapshotProviderV1)
____INTERFACE(flowsql::IID_OPERATOR_REGISTRY, flowsql::IOperatorRegistry)
____INTERFACE(flowsql::IID_OPERATOR_CATALOG, flowsql::IOperatorCatalog)
____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
END_PLUGIN_REGIST()
