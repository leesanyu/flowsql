// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_CHANNELS_NETADAPTER_PFRING_BACKEND_H_
#define FLOWSQL_CHANNELS_NETADAPTER_PFRING_BACKEND_H_
#include <framework/interfaces/icapture_backend.h>
namespace flowsql::channels::netadapter {
int OpenPfringClassic(const CaptureBackendConfig&, std::shared_ptr<ICaptureBackendSession>*, std::string*);
}
#endif
