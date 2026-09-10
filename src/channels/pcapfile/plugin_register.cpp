// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_manager.h>

#include "pcap_file_channel.h"

BEGIN_PLUGIN_REGIST(flowsql::channels::pcapfile::PcapFilePlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_BLOCK_STREAM_FACTORY, flowsql::IBlockStreamFactory)
    ____INTERFACE(flowsql::IID_BLOCK_STREAM_MANAGER, flowsql::IBlockStreamManager)
    ____INTERFACE(flowsql::IID_FILTER_DOMAIN_RESOLVER_V1, flowsql::IFilterDomainResolverV1)
END_PLUGIN_REGIST()
