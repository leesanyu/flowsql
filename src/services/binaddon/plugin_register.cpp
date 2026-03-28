#include <common/iplugin.h>
#include <framework/interfaces/ibinaddon_host.h>

#include "binaddon_host_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::binaddon::BinAddonHostPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_BINADDON_HOST, flowsql::IBinAddonHost)
END_PLUGIN_REGIST()
