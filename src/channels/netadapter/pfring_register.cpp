// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "backend_plugin.h"
#include "pfring_backend.h"
namespace flowsql::channels::netadapter {
class PfringPlugin final : public CaptureBackendPlugin {
 public:
    PfringPlugin() : CaptureBackendPlugin("pfring_classic", OpenPfringClassic) {}
};
}  // namespace flowsql::channels::netadapter
BEGIN_PLUGIN_REGIST(flowsql::channels::netadapter::PfringPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_CAPTURE_BACKEND_PROVIDER_V1, flowsql::ICaptureBackendProviderV1)
END_PLUGIN_REGIST()
