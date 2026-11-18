// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/ifilter_pushdown.h>

#include "pcap_file_channel.h"

BEGIN_PLUGIN_REGIST(flowsql::channels::pcapfile::PcapFilePlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_BLOCK_STREAM_FACTORY, flowsql::IBlockStreamFactory)
    ____INTERFACE(flowsql::IID_BLOCK_STREAM_MANAGER, flowsql::IBlockStreamManager)
    ____INTERFACE(flowsql::IID_BLOCK_STREAM_READER_FACTORY, flowsql::IBlockStreamReaderFactory)
    ____INTERFACE(flowsql::IID_FILTER_DOMAIN_RESOLVER, flowsql::IFilterDomainResolver)
    ____INTERFACE(flowsql::IID_FILTER_PUSHDOWN, flowsql::IFilterPushdown)
    END_PLUGIN_REGIST()
