// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <common/network/wire.h>

#include <cstdint>
#include <cstring>

namespace flowsql::network {

// Owned analysis values. bytes always contain network-order octets, independently of host endianness.
struct alignas(4) IPv4Address {
    uint8_t bytes[4]{};

    // The Arrow/SQL unsigned value: 1.2.3.4 is 0x01020304, not the native integer representation of bytes.
    constexpr uint32_t ToUInt32() const {
        return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) | (uint32_t{bytes[2]} << 8) | bytes[3];
    }

    static constexpr IPv4Address FromUInt32(uint32_t value) {
        return {{static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8),
                 static_cast<uint8_t>(value)}};
    }

    friend bool operator==(const IPv4Address& left, const IPv4Address& right) {
        return std::memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
    }
};

struct alignas(16) IPv6Address {
    uint8_t bytes[16]{};

    friend bool operator==(const IPv6Address& left, const IPv6Address& right) {
        return std::memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
    }
};

struct alignas(2) EthernetAddress {
    uint8_t bytes[6]{};

    friend bool operator==(const EthernetAddress& left, const EthernetAddress& right) {
        return std::memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
    }
};

// Copy each validated wire address into a naturally aligned owned value; retain no packet reference.
inline IPv4Address FromWire(const wire::IPv4Address& value) {
    IPv4Address result;
    std::memcpy(result.bytes, value.bytes, sizeof(result.bytes));
    return result;
}

inline IPv6Address FromWire(const wire::IPv6Address& value) {
    IPv6Address result;
    std::memcpy(result.bytes, value.bytes, sizeof(result.bytes));
    return result;
}

inline EthernetAddress FromWire(const wire::EthernetAddress& value) {
    EthernetAddress result;
    std::memcpy(result.bytes, value.bytes, sizeof(result.bytes));
    return result;
}

}  // namespace flowsql::network
