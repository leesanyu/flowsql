// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IPACKET_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IPACKET_H_

#include <common/network/netaddress.h>
#include <common/span.h>
#include <common/typedef.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

namespace flowsql::packet {

constexpr size_t kMaxLayerDepth = 15;

enum class LayerStatus : uint8_t {
    kNotDecoded = 0,
    kDecoded = 1,
    kTruncated = 2,
    kMalformed = 3,
    kUnsupportedLinkType = 4,
};

enum class ProtocolStatus : uint8_t {
    kNotAttempted = 0,
    kIdentified = 1,
    kUnknown = 2,
};

enum class AddressFamily : uint8_t {
    kNone = 0,
    kIPv4 = 4,
    kIPv6 = 6,
};

enum LayerFieldMask : uint32_t {
    kExtractNone = 0,
    kExtractMac = 1u << 0,
    kExtractIp = 1u << 1,
    kExtractPort = 1u << 2,
};

enum class EndpointScope : uint8_t {
    kInnermost = 0,
    kOutermost = 1,
};

constexpr uint8_t kNoLayerIndex = 0xff;

struct LayerDecodeOptions {
    uint32_t field_mask = kExtractNone;
    EndpointScope endpoint_scope = EndpointScope::kInnermost;
};

// Address layouts are shared with the network header definitions.
using IPv4Address = ::IPv4Address;
using IPv6Address = ::IPv6Address;

struct MacAddress {
    ::EtherAdderss value{};
    uint8_t valid = 0;
};

// An empty variant means that the address was not requested or was not present.
using IpAddress = std::variant<std::monostate, IPv4Address, IPv6Address>;

struct LayerRef {
    uint16_t kind = 0;
    uint32_t offset = 0;
};

struct PacketMeta {
    int64_t timestamp_ns = 0;
    uint32_t captured_len = 0;
    uint32_t wire_len = 0;
    uint32_t link_type = 0;
    uint32_t source_id = 0;
    uint64_t sequence = 0;
};

// A borrowed view valid only for the current capture callback or Poll call.
struct PacketView {
    PacketMeta meta;
    Span<const uint8_t> bytes;
};

// An owned byte range that may cross thread or batch boundaries.
struct PacketBytes {
    std::shared_ptr<const void> owner;
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

struct PacketProtocolInfo {
    ProtocolStatus status = ProtocolStatus::kNotAttempted;
    uint16_t id = 0;
    uint16_t sub_id = 0;
};

}  // namespace flowsql::packet

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IPACKET_H_
