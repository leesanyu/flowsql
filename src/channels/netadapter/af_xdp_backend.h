// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_CHANNELS_NETADAPTER_AF_XDP_BACKEND_H_
#define FLOWSQL_CHANNELS_NETADAPTER_AF_XDP_BACKEND_H_
#include <framework/interfaces/icapture_backend.h>
namespace flowsql::channels::netadapter {
int OpenAfXdpCopySkb(const CaptureBackendConfigV1&, std::shared_ptr<ICaptureBackendSessionV1>*, std::string*);
}
#endif
