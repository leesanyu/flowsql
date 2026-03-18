#include <common/iplugin.h>
#include <framework/interfaces/idataframe_channel.h>

#include "testdata_plugin.h"

EXPORT_API void pluginunregist() {}

EXPORT_API flowsql::IPlugin* pluginregist(flowsql::IRegister* registry, const char* opt) {
    static flowsql::TestDataPlugin _plugin;

    // 提前创建通道，以便注册到 IRegister
    _plugin.InitChannel();

    registry->Regist(flowsql::IID_PLUGIN,             static_cast<flowsql::IPlugin*>(&_plugin));
    registry->Regist(flowsql::IID_CHANNEL,            static_cast<flowsql::IChannel*>(_plugin.GetChannel()));
    registry->Regist(flowsql::IID_DATAFRAME_CHANNEL,  static_cast<flowsql::IDataFrameChannel*>(_plugin.GetChannel()));

    _plugin.Option(opt);
    return &_plugin;
}
