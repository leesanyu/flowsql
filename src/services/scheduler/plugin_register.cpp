#include <common/iplugin.h>
#include <framework/interfaces/irouter_handle.h>

#include "scheduler_plugin.h"

// 注册 SchedulerPlugin 为动态库插件，同时注册 IRouterHandle
BEGIN_PLUGIN_REGIST(flowsql::scheduler::SchedulerPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
END_PLUGIN_REGIST()
