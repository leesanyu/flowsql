#include <common/loader.hpp>

#include "web_plugin.h"

// 注册 WebPlugin 为动态库插件
BEGIN_PLUGIN_REGIST(flowsql::web::WebPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
END_PLUGIN_REGIST()
