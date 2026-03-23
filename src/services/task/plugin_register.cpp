#include "task_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::task::TaskPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
____INTERFACE(flowsql::IID_TASK_STORE, flowsql::ITaskStore)
END_PLUGIN_REGIST()
