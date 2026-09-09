// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_PACKET_FILTER_PLAN_H_
#define _FLOWSQL_FRAMEWORK_CORE_PACKET_FILTER_PLAN_H_

#include <framework/interfaces/ipacket.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace flowsql::packet {

constexpr uint32_t kPcapFilterPlanVersion = 1;

struct PacketIpKey {
    AddressFamily family = AddressFamily::kNone;
    uint32_t ipv4_network_order = 0;
    std::array<uint8_t, 16> ipv6{};
};

struct PacketMacKey {
    std::array<uint8_t, 6> bytes{};
};

struct PacketEndpointKey {
    PacketIpKey address;
    uint16_t port = 0;
};

/** Direction-neutral transport key. first must be the canonical lower endpoint. */
struct TransportPairKey {
    uint8_t transport_protocol = 0;
    PacketEndpointKey first;
    PacketEndpointKey second;
};

struct TimeRangeNs {
    std::optional<int64_t> lower_ns;
    std::optional<int64_t> upper_ns;
    bool lower_inclusive = false;
    bool upper_inclusive = false;
};

enum class PacketUnsignedField : uint8_t {
    kCapturedLen = 0,
    kWireLen,
    kSourceId,
    kSequence,
};

struct PacketUnsignedRange {
    PacketUnsignedField field = PacketUnsignedField::kCapturedLen;
    std::optional<uint64_t> lower;
    std::optional<uint64_t> upper;
    bool lower_inclusive = false;
    bool upper_inclusive = false;
};

enum class PacketFilterRuleKind : uint8_t {
    kMatchAll = 0,
    kMatchNone,
    kAnd,
    kOr,
    kNot,
    kTimeRange,
    kUnsignedRange,
    kMacAnyOf,
    kIpAnyOf,
    kPortAnyOf,
    kTransportPairAnyOf,
};

/**
 * Immutable-after-build packet predicate.
 *
 * Logical kinds use operands. Range kinds use their corresponding range. AnyOf kinds use only the
 * matching key vector. Packet evaluation never parses or compares textual protocol/address values.
 */
struct PacketFilterRule {
    PacketFilterRuleKind kind = PacketFilterRuleKind::kMatchAll;
    std::vector<PacketFilterRule> operands;
    TimeRangeNs time_range;
    PacketUnsignedRange unsigned_range;
    std::vector<PacketMacKey> mac_keys;
    std::vector<PacketIpKey> ip_keys;
    std::vector<uint16_t> ports;
    std::vector<TransportPairKey> transport_pairs;
};

/** Task-owned plan compiled once before the first packet is read. */
struct PcapFilterPlan {
    uint32_t version = kPcapFilterPlanVersion;
    EndpointScope endpoint_scope = EndpointScope::kInnermost;
    PacketFilterRule root;
};

}  // namespace flowsql::packet

#endif  // _FLOWSQL_FRAMEWORK_CORE_PACKET_FILTER_PLAN_H_
