// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "packet_codec.h"
#include <framework/interfaces/ipacket_family.h>

#include <arrow/api.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace flowsql::packet {

PacketEnvelopeError ValidatePacketView(const PacketView& packet) {
    if (packet.bytes.size != packet.meta.captured_len) return PacketEnvelopeError::kLengthMismatch;
    if (packet.bytes.size != 0 && packet.bytes.data == nullptr) return PacketEnvelopeError::kNullData;
    if (packet.meta.wire_len != 0 && packet.meta.wire_len < packet.meta.captured_len) {
        return PacketEnvelopeError::kWireLengthTooSmall;
    }
    return PacketEnvelopeError::kNone;
}

PacketEnvelopeError CopyPacketBytes(const PacketView& packet, PacketBytes* output) {
    if (output == nullptr) return PacketEnvelopeError::kNullOutput;

    *output = PacketBytes{};
    const auto validation = ValidatePacketView(packet);
    if (validation != PacketEnvelopeError::kNone) return validation;
    if (packet.meta.captured_len == 0) return PacketEnvelopeError::kNone;

    try {
        auto storage = std::make_shared<std::vector<uint8_t>>(packet.meta.captured_len);
        std::memcpy(storage->data(), packet.bytes.data, packet.meta.captured_len);
        const uint8_t* data = storage->data();
        output->owner = std::move(storage);
        output->data = data;
        output->size = packet.meta.captured_len;
    } catch (const std::bad_alloc&) {
        return PacketEnvelopeError::kAllocationFailed;
    }
    return PacketEnvelopeError::kNone;
}

std::shared_ptr<arrow::Schema> PacketSchema() { return PacketFamilySchema(); }

}  // namespace flowsql::packet
