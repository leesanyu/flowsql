// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "af_xdp_backend.h"
#include "backend_plugin.h"
namespace flowsql::channels::netadapter {
class AfXdpPlugin final : public CaptureBackendPlugin {
 public:
    AfXdpPlugin() : CaptureBackendPlugin("af_xdp_copy_skb", OpenAfXdpCopySkb) {}
};
}  // namespace flowsql::channels::netadapter
BEGIN_PLUGIN_REGIST(flowsql::channels::netadapter::AfXdpPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_CAPTURE_BACKEND_PROVIDER, flowsql::ICaptureBackendProvider)
END_PLUGIN_REGIST()
