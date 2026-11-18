// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_CHANNELS_NETADAPTER_AF_PACKET_BACKEND_H_
#define FLOWSQL_CHANNELS_NETADAPTER_AF_PACKET_BACKEND_H_
#include <framework/interfaces/icapture_backend.h>
namespace flowsql::channels::netadapter {
int OpenAfPacket(const CaptureBackendConfig& config, std::shared_ptr<ICaptureBackendSession>* output,
                 std::string* error);
}  // namespace flowsql::channels::netadapter
#endif
