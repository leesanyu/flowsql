// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "af_packet_backend.h"
#include "backend_plugin.h"
namespace flowsql::channels::netadapter {
class AfPacketPlugin final : public CaptureBackendPlugin {
 public:
    AfPacketPlugin() : CaptureBackendPlugin("af_packet", OpenAfPacket) {}
};
}  // namespace flowsql::channels::netadapter
BEGIN_PLUGIN_REGIST(flowsql::channels::netadapter::AfPacketPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_CAPTURE_BACKEND_PROVIDER, flowsql::ICaptureBackendProvider)
END_PLUGIN_REGIST()
