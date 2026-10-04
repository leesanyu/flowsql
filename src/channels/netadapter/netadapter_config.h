// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_CHANNELS_NETADAPTER_CONFIG_H_
#define FLOWSQL_CHANNELS_NETADAPTER_CONFIG_H_
#include <framework/interfaces/icapture_backend.h>
namespace flowsql::channels::netadapter {
constexpr uint64_t kMiB = 1024 * 1024;
constexpr uint64_t kSharedEnvelopeBytes = 2 * kMiB;
constexpr uint64_t kMinimumInputBytes = kMiB;
constexpr uint32_t kInputPacketQuantum = 16;
constexpr uint64_t kInputByteQuantum = 64 * 1024;
int ParseNetAdapterConfig(const std::string& json, CaptureBackendConfigV1* config, std::string* normalized,
                          std::string* error);
CaptureSourceSetV2 DefaultSourceSet();
}  // namespace flowsql::channels::netadapter
#endif
