// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <common/typedef.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/irouter_handle.h>

#include "database_plugin.h"

// DatabasePlugin — 注册为 IPlugin、IDatabaseFactory、IRouterHandle
BEGIN_PLUGIN_REGIST(flowsql::database::DatabasePlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_DATABASE_FACTORY, flowsql::IDatabaseFactory)
    ____INTERFACE(flowsql::IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1, flowsql::IDatabaseChannelLeaseProviderV1)
    ____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
END_PLUGIN_REGIST()
