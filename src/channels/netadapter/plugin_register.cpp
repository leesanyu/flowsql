// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "netadapter_plugin.h"
BEGIN_PLUGIN_REGIST(flowsql::channels::netadapter::NetAdapterPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_BLOCK_STREAM_FACTORY, flowsql::IBlockStreamFactory)
____INTERFACE(flowsql::IID_BLOCK_STREAM_MANAGER, flowsql::IBlockStreamManager)
____INTERFACE(flowsql::IID_BLOCK_STREAM_READER_FACTORY_V1, flowsql::IBlockStreamReaderFactoryV1)
____INTERFACE(flowsql::IID_FILTER_DOMAIN_RESOLVER_V1, flowsql::IFilterDomainResolverV1)
____INTERFACE(flowsql::IID_FILTER_PUSHDOWN_V1, flowsql::IFilterPushdownV1)
END_PLUGIN_REGIST()
