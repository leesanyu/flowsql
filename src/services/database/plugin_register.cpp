#include <common/iplugin.h>
#include <common/typedef.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/irouter_handle.h>

#include "database_plugin.h"

// DatabasePlugin — 注册为 IPlugin、IDatabaseFactory、IRouterHandle
BEGIN_PLUGIN_REGIST(flowsql::database::DatabasePlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_DATABASE_FACTORY, flowsql::IDatabaseFactory)
    ____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
END_PLUGIN_REGIST()
