// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_plugin.h"
BEGIN_PLUGIN_REGIST(flowsql::channels::kafka::Plugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_KAFKA_CONNECTION_SERVICE, flowsql::IKafkaConnectionService)
____INTERFACE(flowsql::IID_KAFKA_CHANNEL_CATALOG, flowsql::IKafkaChannelCatalog)
____INTERFACE(flowsql::IID_BLOCK_STREAM_FACTORY, flowsql::IBlockStreamFactory)
____INTERFACE(flowsql::IID_BLOCK_STREAM_READER_FACTORY, flowsql::IBlockStreamReaderFactory)
____INTERFACE(flowsql::IID_BLOCK_STREAM_CHANNEL_DESCRIPTOR, flowsql::IBlockStreamChannelDescriptor)
____INTERFACE(flowsql::IID_RESULT_SINK_FACTORY, flowsql::IResultSinkFactory)
____INTERFACE(flowsql::IID_PACKET_SINK_FACTORY, flowsql::IPacketSinkFactory)
____INTERFACE(flowsql::IID_FILTER_DOMAIN_RESOLVER, flowsql::IFilterDomainResolver)
____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
____INTERFACE(flowsql::IID_CHANNEL_DATASET_DESCRIPTOR, flowsql::IChannelDatasetDescriptor)
END_PLUGIN_REGIST()
