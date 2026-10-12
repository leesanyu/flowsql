// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <common/network/analysis_address.h>
#include <framework/interfaces/ipacket.h>

#include <limits>
#include <vector>

namespace arrow {
class Schema;
class RecordBatch;
class Status;
}  // namespace arrow

namespace flowsql::packet {

enum class PacketAddressState : uint8_t { kNotRequested = 0, kNotPresent = 1, kAvailable = 2, kIncomplete = 3 };
constexpr uint32_t kNoAddressIndex = std::numeric_limits<uint32_t>::max();

template <typename Address>
struct PacketIpEndpoints {
    Address src;
    Address dst;
};
using PacketIpEndpoints4 = PacketIpEndpoints<network::IPv4Address>;
using PacketIpEndpoints6 = PacketIpEndpoints<network::IPv6Address>;

// Common facts contain no IP storage. Layer status and address availability are independent.
struct PacketLayerFacts {
    LayerStatus status = LayerStatus::kNotDecoded;
    uint8_t layer_count = 0;
    std::array<LayerRef, kMaxLayerDepth> layers{};
    EndpointScope endpoint_scope = EndpointScope::kInnermost;
    uint8_t network_layer_index = kNoLayerIndex;
    uint8_t transport_layer_index = kNoLayerIndex;
    uint32_t payload_offset = 0;
    network::EthernetAddress src_mac;
    network::EthernetAddress dst_mac;
    bool src_mac_valid = false;
    bool dst_mac_valid = false;
    PacketAddressState address_state = PacketAddressState::kNotRequested;
    uint8_t transport_protocol = 0;
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    bool ports_valid = false;
};

struct PacketAddressRef {
    AddressFamily family = AddressFamily::kNone;
    uint32_t index = kNoAddressIndex;
};

// Callback/batch-borrowed endpoints; its concrete pair belongs to a batch or family table.
struct PacketFamilyLayerView : PacketLayerFacts {
    std::variant<std::monostate, const PacketIpEndpoints4*, const PacketIpEndpoints6*> endpoints;
    const PacketIpEndpoints4* Get4() const {
        const auto* value = std::get_if<const PacketIpEndpoints4*>(&endpoints);
        return value ? *value : nullptr;
    }
    const PacketIpEndpoints6* Get6() const {
        const auto* value = std::get_if<const PacketIpEndpoints6*>(&endpoints);
        return value ? *value : nullptr;
    }
};

struct PacketFamilyRecord {
    PacketMeta meta;
    PacketBytes raw_data;
    PacketLayerFacts layer;
    PacketProtocolInfo protocol;
    PacketAddressRef address;
};

// rows is capture order, including non-IP rows. Each available row indexes exactly one family array.
// No endpoint is created for not-requested, absent or incomplete addresses. PacketBytes retains its owner.
struct PacketFamilyBatch {
    std::vector<PacketFamilyRecord> rows;
    std::vector<PacketIpEndpoints4> ipv4;
    std::vector<PacketIpEndpoints6> ipv6;
};

// Decoder emits one concrete pair iff facts.address_state is available; it retains neither sink nor bytes.
interface IPacketAddressSink {
    virtual ~IPacketAddressSink() = default;
    virtual int Set(const PacketIpEndpoints4& endpoints) = 0;
    virtual int Set(const PacketIpEndpoints6& endpoints) = 0;
};
interface IPacketFamilyLayerDecoder {
    virtual ~IPacketFamilyLayerDecoder() = default;
    virtual int Decode(const PacketView& packet, const LayerDecodeOptions& options, PacketLayerFacts* facts,
                       IPacketAddressSink& addresses) = 0;
};
interface IPacketFamilyProtocolIdentifier {
    virtual ~IPacketFamilyProtocolIdentifier() = default;
    // Same sampled-session path; Identify consumes the validated layer path without decoding it again.
    virtual PacketProtocolInfo Identify(const PacketView& packet, const PacketLayerFacts& facts) = 0;
};

// One RecordBatch with dense union codes 0/4/6 and family-local int32 offsets. Child arrays own their buffers.
std::shared_ptr<arrow::Schema> PacketFamilySchema();
// Logical predicate types only; T2 binds these names to union access without materializing full-width columns.
std::shared_ptr<arrow::Schema> PacketFamilyFilterSchema();
arrow::Status ValidatePacketFamilyBatch(const arrow::RecordBatch& batch);

}  // namespace flowsql::packet
