// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <framework/core/packet_codec.h>
#include <framework/interfaces/ipacket_family.h>

namespace flowsql::packet {

class PacketFamilyBatchAddressSink final : public IPacketAddressSink {
 public:
    explicit PacketFamilyBatchAddressSink(PacketFamilyBatch& batch) : batch_(batch) {}
    int Set(const PacketIpEndpoints4& endpoints) override {
        batch_.ipv4.push_back(endpoints);
        address = {AddressFamily::kIPv4, static_cast<uint32_t>(batch_.ipv4.size() - 1)};
        return 0;
    }
    int Set(const PacketIpEndpoints6& endpoints) override {
        batch_.ipv6.push_back(endpoints);
        address = {AddressFamily::kIPv6, static_cast<uint32_t>(batch_.ipv6.size() - 1)};
        return 0;
    }
    PacketAddressRef address;

 private:
    PacketFamilyBatch& batch_;
};

inline PacketFamilyLayerView PacketFamilyBorrowLayer(const PacketLayerFacts& facts, PacketAddressRef address,
                                                     const PacketFamilyBatch& batch) {
    PacketFamilyLayerView view;
    static_cast<PacketLayerFacts&>(view) = facts;
    if (address.family == AddressFamily::kIPv4)
        view.endpoints = &batch.ipv4.at(address.index);
    else if (address.family == AddressFamily::kIPv6)
        view.endpoints = &batch.ipv6.at(address.index);
    return view;
}

PacketBatchError EncodePacketFamilyBatch(const PacketFamilyBatch& records, std::shared_ptr<arrow::RecordBatch>* output,
                                         std::string* error = nullptr);
// next supplies exactly one row with family-local endpoint storage. Raw bytes are copied before the next call.
// The scratch batch reuses its family buffers; no IPv6 endpoint capacity is needed by a pure IPv4 stream.
PacketBatchError EncodePacketFamilyStream(const std::function<int(PacketFamilyBatch*)>& next, uint32_t max_records,
                                          arrow::MemoryPool* pool, std::shared_ptr<arrow::RecordBatch>* output,
                                          std::string* error = nullptr);

}  // namespace flowsql::packet
