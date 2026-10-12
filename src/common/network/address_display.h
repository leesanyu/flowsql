// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <arpa/inet.h>
#include <arrow/api.h>

#include <optional>
#include <string>

namespace flowsql::network {
// Called only at presentation boundaries; owning Arrow addresses retain their compact binary types.
inline std::optional<std::string> DisplayAddress(const arrow::Field& field, const arrow::Array& array, int64_t row) {
    if (array.IsNull(row) || !field.metadata()) return std::nullopt;
    const auto semantic = field.metadata()->Get("flowsql.semantic");
    if (!semantic.ok()) return std::nullopt;
    char output[INET6_ADDRSTRLEN];
    if (*semantic == "ipv4_address" && array.type_id() == arrow::Type::UINT32) {
        const auto value = static_cast<const arrow::UInt32Array&>(array).Value(row);
        const uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                                  static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        if (inet_ntop(AF_INET, bytes, output, sizeof(output))) return std::string(output);
    } else if (*semantic == "ipv6_address" && array.type_id() == arrow::Type::FIXED_SIZE_BINARY) {
        const auto& values = static_cast<const arrow::FixedSizeBinaryArray&>(array);
        if (values.byte_width() == 16 && inet_ntop(AF_INET6, values.GetValue(row), output, sizeof(output)))
            return std::string(output);
    }
    return std::nullopt;
}
}  // namespace flowsql::network
