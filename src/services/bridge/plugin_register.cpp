#include <common/typedef.h>
#include <common/loader.hpp>

#include "bridge_plugin.h"

EXPORT_API void pluginunregist() {}

EXPORT_API flowsql::IPlugin* pluginregist(flowsql::IRegister* registry, const char* opt) {
    static flowsql::bridge::BridgePlugin _plugin;

    // 注册 IPlugin（生命周期管理 + 启停控制）
    {
        flowsql::IPlugin* iface = dynamic_cast<flowsql::IPlugin*>(&_plugin);
        registry->Regist(flowsql::IID_PLUGIN, iface);
    }

    _plugin.Option(opt);
    return &_plugin;
}
