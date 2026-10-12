// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/api.h>
#include <common/network/analysis_address.h>
#include <framework/interfaces/ipacket.h>

namespace flowsql::npm {
template <typename Address>
struct NpmAddressArrowBuilder;
template <>
struct NpmAddressArrowBuilder<network::IPv4Address> : arrow::UInt32Builder {
    static constexpr packet::AddressFamily family = packet::AddressFamily::kIPv4;
    explicit NpmAddressArrowBuilder(arrow::MemoryPool* pool) : arrow::UInt32Builder(pool) {}
    arrow::Status Append(network::IPv4Address value) { return arrow::UInt32Builder::Append(value.ToUInt32()); }
};
template <>
struct NpmAddressArrowBuilder<network::IPv6Address> : arrow::FixedSizeBinaryBuilder {
    static constexpr packet::AddressFamily family = packet::AddressFamily::kIPv6;
    explicit NpmAddressArrowBuilder(arrow::MemoryPool* pool)
        : arrow::FixedSizeBinaryBuilder(arrow::fixed_size_binary(16), pool) {}
    arrow::Status Append(const network::IPv6Address& value) {
        return arrow::FixedSizeBinaryBuilder::Append(value.bytes);
    }
};
}  // namespace flowsql::npm
