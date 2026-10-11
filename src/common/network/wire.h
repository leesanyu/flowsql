// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "netaddress.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace flowsql::wire {

struct IPv4Address {
    uint8_t bytes[4];
};
struct IPv6Address {
    uint8_t bytes[16];
};
struct EthernetAddress {
    uint8_t bytes[6];
};

inline ::IPv4Address ToAnalysis(const IPv4Address& value) {
    ::IPv4Address result;
    std::memcpy(result.bytes, value.bytes, sizeof(value.bytes));
    return result;
}
inline ::IPv6Address ToAnalysis(const IPv6Address& value) {
    ::IPv6Address result;
    std::memcpy(result.bytes, value.bytes, sizeof(value.bytes));
    return result;
}
inline ::EtherAdderss ToAnalysis(const EthernetAddress& value) {
    ::EtherAdderss result;
    std::memcpy(result.bytes, value.bytes, sizeof(value.bytes));
    return result;
}

inline bool Contains(size_t size, size_t offset, size_t length) { return offset <= size && length <= size - offset; }

// A view borrows the packet storage. Callers keep that storage alive while reading fields.
template <typename Header>
const Header* View(const uint8_t* bytes, size_t size, size_t offset = 0) {
    static_assert(alignof(Header) == 1, "Wire headers must be byte-aligned");
    static_assert(std::is_trivially_copyable_v<Header>);
    if (!bytes || !Contains(size, offset, sizeof(Header))) return nullptr;
    return reinterpret_cast<const Header*>(bytes + offset);
}

}  // namespace flowsql::wire
