#include <common/loader.hpp>

#include "scheduler_plugin.h"

// 注册 SchedulerPlugin 为动态库插件
BEGIN_PLUGIN_REGIST(flowsql::scheduler::SchedulerPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
END_PLUGIN_REGIST()
