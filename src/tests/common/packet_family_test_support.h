// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include <arpa/inet.h>
#include <framework/core/packet_family_codec.h>
#include <cassert>
#include <cstring>
#include <string>

namespace flowsql::testing {
// Test fixtures own family buffers; production callback types continue to borrow these exact buffers.
class OwnedPacketLayer : public packet::PacketFamilyLayerView {
 public:
    OwnedPacketLayer() = default;
    OwnedPacketLayer(const packet::PacketFamilyLayerView& other) { Copy(other); }
    OwnedPacketLayer(const OwnedPacketLayer& other) { Copy(other); }
    OwnedPacketLayer& operator=(const packet::PacketFamilyLayerView& other) {
        Copy(other);
        return *this;
    }
    OwnedPacketLayer& operator=(const OwnedPacketLayer& other) {
        if (this != &other) Copy(other);
        return *this;
    }
    OwnedPacketLayer(OwnedPacketLayer&&) noexcept = default;
    OwnedPacketLayer& operator=(OwnedPacketLayer&&) noexcept = default;
    template <typename Address>
    void SetEndpoints(const Address& src, const Address& dst) {
        if constexpr (sizeof(src.bytes) == 4) {
            packet::PacketIpEndpoints4 pair;
            std::memcpy(pair.src.bytes, src.bytes, 4);
            std::memcpy(pair.dst.bytes, dst.bytes, 4);
            addresses_ = {};
            addresses_.ipv4.push_back(pair);
            endpoints = &addresses_.ipv4[0];
        } else {
            static_assert(sizeof(src.bytes) == 16);
            packet::PacketIpEndpoints6 pair;
            std::memcpy(pair.src.bytes, src.bytes, 16);
            std::memcpy(pair.dst.bytes, dst.bytes, 16);
            addresses_ = {};
            addresses_.ipv6.push_back(pair);
            endpoints = &addresses_.ipv6[0];
        }
        address_state = packet::PacketAddressState::kAvailable;
    }
    void SetEndpointsText(const std::string& src, const std::string& dst) {
        network::IPv4Address s4, d4;
        if (inet_pton(AF_INET, src.c_str(), s4.bytes) == 1) {
            assert(inet_pton(AF_INET, dst.c_str(), d4.bytes) == 1);
            SetEndpoints(s4, d4);
        } else {
            network::IPv6Address s6, d6;
            assert(inet_pton(AF_INET6, src.c_str(), s6.bytes) == 1 && inet_pton(AF_INET6, dst.c_str(), d6.bytes) == 1);
            SetEndpoints(s6, d6);
        }
    }
    void ClearAddresses(packet::PacketAddressState state = packet::PacketAddressState::kIncomplete) {
        endpoints = std::monostate{};
        addresses_ = {};
        address_state = state;
    }
    void SwapEndpoints() {
        if (const auto* pair = Get4())
            SetEndpoints(pair->dst, pair->src);
        else if (const auto* pair = Get6())
            SetEndpoints(pair->dst, pair->src);
    }

 private:
    void Copy(const packet::PacketFamilyLayerView& other) {
        const auto common = static_cast<const packet::PacketLayerFacts&>(other);
        if (const auto* pair = other.Get4())
            SetEndpoints(pair->src, pair->dst);
        else if (const auto* pair = other.Get6())
            SetEndpoints(pair->src, pair->dst);
        else
            ClearAddresses(other.address_state);
        static_cast<packet::PacketLayerFacts&>(*this) = common;
    }
    packet::PacketFamilyBatch addresses_;
};
inline OwnedPacketLayer DecodeLayer(packet::IPacketFamilyLayerDecoder& decoder, const packet::PacketView& packet,
                                    const packet::LayerDecodeOptions& options) {
    packet::PacketFamilyBatch addresses;
    packet::PacketLayerFacts facts;
    packet::PacketFamilyBatchAddressSink sink(addresses);
    assert(decoder.Decode(packet, options, &facts, sink) == 0);
    return OwnedPacketLayer(packet::PacketFamilyBorrowLayer(facts, sink.address, addresses));
}
struct PacketRecord {
    packet::PacketMeta meta;
    packet::PacketBytes raw_data;
    OwnedPacketLayer layer;
    packet::PacketProtocolInfo protocol;
};
inline packet::PacketBatchError EncodePacketBatch(const std::vector<PacketRecord>& records,
                                                  std::shared_ptr<arrow::RecordBatch>* output,
                                                  std::string* error = nullptr) {
    packet::PacketFamilyBatch batch;
    for (const auto& record : records) {
        packet::PacketFamilyRecord row;
        row.meta = record.meta;
        row.raw_data = record.raw_data;
        row.layer = record.layer;
        row.protocol = record.protocol;
        if (const auto* pair = record.layer.Get4()) {
            row.address = {packet::AddressFamily::kIPv4, static_cast<uint32_t>(batch.ipv4.size())};
            batch.ipv4.push_back(*pair);
        } else if (const auto* pair = record.layer.Get6()) {
            row.address = {packet::AddressFamily::kIPv6, static_cast<uint32_t>(batch.ipv6.size())};
            batch.ipv6.push_back(*pair);
        }
        batch.rows.push_back(row);
    }
    return packet::EncodePacketFamilyBatch(batch, output, error);
}
}  // namespace flowsql::testing
