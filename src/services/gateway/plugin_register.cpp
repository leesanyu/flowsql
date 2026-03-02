#include <common/loader.hpp>

#include "gateway_plugin.h"

// 注册 GatewayPlugin 为动态库插件
BEGIN_PLUGIN_REGIST(flowsql::gateway::GatewayPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
END_PLUGIN_REGIST()
