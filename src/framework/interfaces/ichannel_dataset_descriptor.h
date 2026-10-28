// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/interfaces/idata_semantics_registry.h>
#include <cstdint>

namespace flowsql {
const Guid IID_CHANNEL_DATASET_DESCRIPTOR_V1 = {
    0x2d334dd8, 0x40ad, 0x4550, {0x9a, 0xb1, 0xd4, 0xd2, 0xa3, 0xb6, 0x50, 0x19}};
struct ChannelDatasetRequestV1 {
    std::string locator_json;
    uint32_t timeout_ms = 5000;
};
struct ChannelDatasetSnapshotV1 {
    std::shared_ptr<const std::string> document_json;
};
// Providers own metadata discovery; no data reads, credentials or implicit lazy loading.
interface IChannelDatasetDescriptorV1 {
    virtual ~IChannelDatasetDescriptorV1() = default;
    virtual const char* ProviderId() const = 0;
    // Thread safe. Failure clears output. The total call budget is timeout_ms (1..30000).
    virtual int Describe(const ChannelDatasetRequestV1&, ChannelDatasetSnapshotV1*, DataSemanticsErrorV1*) = 0;
};
}  // namespace flowsql
