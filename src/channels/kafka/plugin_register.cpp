// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_plugin.h"
BEGIN_PLUGIN_REGIST(flowsql::channels::kafka::Plugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_KAFKA_CONNECTION_SERVICE_V1, flowsql::IKafkaConnectionServiceV1)
____INTERFACE(flowsql::IID_KAFKA_CHANNEL_CATALOG_V1, flowsql::IKafkaChannelCatalogV1)
____INTERFACE(flowsql::IID_BLOCK_STREAM_FACTORY, flowsql::IBlockStreamFactory)
____INTERFACE(flowsql::IID_BLOCK_STREAM_READER_FACTORY_V1, flowsql::IBlockStreamReaderFactoryV1)
____INTERFACE(flowsql::IID_BLOCK_STREAM_CHANNEL_DESCRIPTOR_V1, flowsql::IBlockStreamChannelDescriptorV1)
____INTERFACE(flowsql::IID_RESULT_SINK_FACTORY_V1, flowsql::IResultSinkFactoryV1)
____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
END_PLUGIN_REGIST()
