// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "packet_codec.h"

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

namespace {

PacketBatchError SetBatchError(PacketBatchError code, const char* message, std::string* error) {
    if (error) *error = message;
    return code;
}

PacketBatchError ValidatePacketRecord(const PacketRecord& record, std::string* error) {
    if (record.raw_data.size != record.meta.captured_len) {
        return SetBatchError(PacketBatchError::kInvalidRecord, "raw_data size does not match captured_len", error);
    }
    if (record.raw_data.size != 0 && (record.raw_data.data == nullptr || record.raw_data.owner == nullptr)) {
        return SetBatchError(PacketBatchError::kInvalidRecord, "non-empty raw_data requires data and owner", error);
    }
    if (record.raw_data.size > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return SetBatchError(PacketBatchError::kInvalidRecord, "raw_data exceeds Arrow binary length", error);
    }
    if (record.meta.wire_len != 0 && record.meta.wire_len < record.meta.captured_len) {
        return SetBatchError(PacketBatchError::kInvalidRecord, "wire_len is smaller than captured_len", error);
    }
    if (record.layer.layer_count > kMaxLayerDepth) {
        return SetBatchError(PacketBatchError::kInvalidRecord, "layer_count exceeds kMaxLayerDepth", error);
    }
    if (record.layer.endpoint_scope != EndpointScope::kInnermost &&
        record.layer.endpoint_scope != EndpointScope::kOutermost) {
        return SetBatchError(PacketBatchError::kInvalidRecord, "endpoint scope is invalid", error);
    }
    if (record.layer.network_layer_index != kNoLayerIndex &&
        record.layer.network_layer_index >= record.layer.layer_count) {
        return SetBatchError(PacketBatchError::kInvalidRecord, "network layer index exceeds layer_count", error);
    }
    if (record.layer.transport_layer_index != kNoLayerIndex &&
        record.layer.transport_layer_index >= record.layer.layer_count) {
        return SetBatchError(PacketBatchError::kInvalidRecord, "transport layer index exceeds layer_count", error);
    }
    for (uint8_t index = 0; index < record.layer.layer_count; ++index) {
        const auto& layer = record.layer.layers[index];
        if (layer.offset > record.meta.captured_len) {
            return SetBatchError(PacketBatchError::kInvalidRecord, "layer offset exceeds captured_len", error);
        }
    }
    if (record.layer.payload_offset > record.meta.captured_len) {
        return SetBatchError(PacketBatchError::kInvalidRecord, "payload_offset exceeds captured_len", error);
    }
    return PacketBatchError::kNone;
}

}  // namespace

PacketBatchError EncodePacketBatch(const std::vector<PacketRecord>& records,
                                   std::shared_ptr<arrow::RecordBatch>* output, std::string* error) {
    if (output == nullptr) return PacketBatchError::kNullOutput;
    *output = nullptr;
    if (error) error->clear();

    const auto expected_scope = records.empty() ? EndpointScope::kInnermost : records.front().layer.endpoint_scope;
    for (const auto& record : records) {
        const auto validation = ValidatePacketRecord(record, error);
        if (validation != PacketBatchError::kNone) return validation;
        if (record.layer.endpoint_scope != expected_scope) {
            return SetBatchError(PacketBatchError::kInvalidRecord, "endpoint scope is mixed within a batch", error);
        }
    }

    size_t index = 0;
    return EncodePacketStream(
        [&](PacketRecord* record) {
            if (index == records.size()) return ENOENT;
            *record = records[index++];
            return 0;
        },
        static_cast<uint32_t>(records.size()), arrow::default_memory_pool(), output, error);
}

PacketBatchError EncodePacketStream(const std::function<int(PacketRecord*)>& next, uint32_t max_records,
                                    arrow::MemoryPool* pool, std::shared_ptr<arrow::RecordBatch>* output,
                                    std::string* error) {
    if (!output || !pool) return PacketBatchError::kNullOutput;
    *output = nullptr;
    if (error) error->clear();
    uint32_t rows = 0;
    try {
        arrow::Int64Builder timestamp(pool);
        arrow::UInt32Builder captured_len(pool);
        arrow::UInt32Builder wire_len(pool);
        arrow::UInt32Builder link_type(pool);
        arrow::UInt32Builder source_id(pool);
        arrow::UInt64Builder sequence(pool);
        arrow::BinaryBuilder raw_data(pool);
        arrow::UInt8Builder layer_status(pool);
        arrow::UInt8Builder layer_count(pool);
        auto layer_ids_values = std::make_shared<arrow::UInt16Builder>(pool);
        arrow::FixedSizeListBuilder layer_ids(pool, layer_ids_values, kMaxLayerDepth);
        auto layer_offsets_values = std::make_shared<arrow::UInt32Builder>(pool);
        arrow::FixedSizeListBuilder layer_offsets(pool, layer_offsets_values, kMaxLayerDepth);
        arrow::UInt8Builder endpoint_scope(pool);
        arrow::UInt8Builder network_layer_index(pool);
        arrow::UInt8Builder transport_layer_index(pool);
        arrow::UInt32Builder payload_offset(pool);
        arrow::FixedSizeBinaryBuilder src_mac(arrow::fixed_size_binary(6), pool);
        arrow::FixedSizeBinaryBuilder dst_mac(arrow::fixed_size_binary(6), pool);
        arrow::UInt32Builder src_ip_v4(pool);
        arrow::UInt32Builder dst_ip_v4(pool);
        arrow::BinaryBuilder src_ip_v6(pool);
        arrow::BinaryBuilder dst_ip_v6(pool);
        arrow::UInt8Builder src_ip_family(pool);
        arrow::UInt8Builder dst_ip_family(pool);
        arrow::UInt8Builder transport_protocol(pool);
        arrow::UInt16Builder src_port(pool);
        arrow::UInt16Builder dst_port(pool);
        arrow::BooleanBuilder ports_valid(pool);
        arrow::UInt8Builder protocol_status(pool);
        arrow::UInt16Builder protocol_id(pool);
        arrow::UInt16Builder protocol_sub_id(pool);

        auto check = [&](const arrow::Status& status, const char* field) {
            if (status.ok()) return true;
            if (error) *error = std::string(field) + ": " + status.ToString();
            return false;
        };
        auto append_mac = [&](const MacAddress& mac, arrow::FixedSizeBinaryBuilder& builder, const char* field) {
            return mac.valid ? check(builder.Append(mac.value.bytes), field) : check(builder.AppendNull(), field);
        };
        auto append_ip = [&](const IpAddress& address, arrow::UInt32Builder& v4, arrow::BinaryBuilder& v6,
                             arrow::UInt8Builder& family, const char* v4_field, const char* v6_field,
                             const char* family_field) {
            if (const auto* value = std::get_if<IPv4Address>(&address)) {
                return check(v4.Append(value->addr), v4_field) && check(v6.AppendNull(), v6_field) &&
                       check(family.Append(static_cast<uint8_t>(AddressFamily::kIPv4)), family_field);
            }
            if (const auto* value = std::get_if<IPv6Address>(&address)) {
                return check(v4.AppendNull(), v4_field) &&
                       check(v6.Append(value->bytes, sizeof(value->bytes)), v6_field) &&
                       check(family.Append(static_cast<uint8_t>(AddressFamily::kIPv6)), family_field);
            }
            return check(v4.AppendNull(), v4_field) && check(v6.AppendNull(), v6_field) &&
                   check(family.Append(static_cast<uint8_t>(AddressFamily::kNone)), family_field);
        };

        PacketRecord record;
        for (; rows < max_records; ++rows) {
            const int next_rc = next(&record);
            if (next_rc == ENOENT) break;
            if (next_rc != 0)
                return SetBatchError(PacketBatchError::kInvalidRecord, "packet stream read failed", error);
            const auto validation = ValidatePacketRecord(record, error);
            if (validation != PacketBatchError::kNone) return validation;
            const auto& meta = record.meta;
            const auto& layer = record.layer;
            const auto& protocol = record.protocol;
            if (!check(timestamp.Append(meta.timestamp_ns), "timestamp_ns") ||
                !check(captured_len.Append(meta.captured_len), "captured_len") ||
                !check(wire_len.Append(meta.wire_len), "wire_len") ||
                !check(link_type.Append(meta.link_type), "link_type") ||
                !check(source_id.Append(meta.source_id), "source_id") ||
                !check(sequence.Append(meta.sequence), "sequence")) {
                return PacketBatchError::kArrowError;
            }

            if (meta.captured_len == 0) {
                if (!check(raw_data.Append(std::string_view()), "raw_data")) return PacketBatchError::kArrowError;
            } else if (!check(raw_data.Append(record.raw_data.data, static_cast<int32_t>(record.raw_data.size)),
                              "raw_data")) {
                return PacketBatchError::kArrowError;
            }

            if (!check(layer_status.Append(static_cast<uint8_t>(layer.status)), "layer_status") ||
                !check(layer_count.Append(layer.layer_count), "layer_count") ||
                !check(layer_ids.Append(), "layer_ids") || !check(layer_offsets.Append(), "layer_offsets")) {
                return PacketBatchError::kArrowError;
            }
            for (const auto& item : layer.layers) {
                if (!check(layer_ids_values->Append(item.kind), "layer_ids") ||
                    !check(layer_offsets_values->Append(item.offset), "layer_offsets")) {
                    return PacketBatchError::kArrowError;
                }
            }
            if (!check(endpoint_scope.Append(static_cast<uint8_t>(layer.endpoint_scope)), "endpoint_scope") ||
                !check(network_layer_index.Append(layer.network_layer_index), "network_layer_index") ||
                !check(transport_layer_index.Append(layer.transport_layer_index), "transport_layer_index") ||
                !check(payload_offset.Append(layer.payload_offset), "payload_offset") ||
                !append_mac(layer.src_mac, src_mac, "src_mac") || !append_mac(layer.dst_mac, dst_mac, "dst_mac") ||
                !append_ip(layer.src_ip, src_ip_v4, src_ip_v6, src_ip_family, "src_ip_v4", "src_ip_v6",
                           "src_ip_family") ||
                !append_ip(layer.dst_ip, dst_ip_v4, dst_ip_v6, dst_ip_family, "dst_ip_v4", "dst_ip_v6",
                           "dst_ip_family")) {
                return PacketBatchError::kArrowError;
            }

            if (layer.transport_protocol == 0) {
                if (!check(transport_protocol.AppendNull(), "transport_protocol")) return PacketBatchError::kArrowError;
            } else if (!check(transport_protocol.Append(layer.transport_protocol), "transport_protocol")) {
                return PacketBatchError::kArrowError;
            }
            if (layer.ports_valid) {
                if (!check(src_port.Append(layer.src_port), "src_port") ||
                    !check(dst_port.Append(layer.dst_port), "dst_port")) {
                    return PacketBatchError::kArrowError;
                }
            } else if (!check(src_port.AppendNull(), "src_port") || !check(dst_port.AppendNull(), "dst_port")) {
                return PacketBatchError::kArrowError;
            }
            if (!check(ports_valid.Append(layer.ports_valid != 0), "ports_valid") ||
                !check(protocol_status.Append(static_cast<uint8_t>(protocol.status)), "protocol_status")) {
                return PacketBatchError::kArrowError;
            }
            if (protocol.status == ProtocolStatus::kIdentified) {
                if (!check(protocol_id.Append(protocol.id), "protocol_id") ||
                    !check(protocol_sub_id.Append(protocol.sub_id), "protocol_sub_id")) {
                    return PacketBatchError::kArrowError;
                }
            } else if (!check(protocol_id.AppendNull(), "protocol_id") ||
                       !check(protocol_sub_id.AppendNull(), "protocol_sub_id")) {
                return PacketBatchError::kArrowError;
            }
        }

        std::vector<std::shared_ptr<arrow::Array>> arrays;
        arrays.reserve(30);
        auto finish = [&](auto& builder, const char* field) {
            std::shared_ptr<arrow::Array> array;
            if (!check(builder.Finish(&array), field)) return false;
            arrays.push_back(std::move(array));
            return true;
        };
        if (!finish(timestamp, "timestamp_ns") || !finish(captured_len, "captured_len") ||
            !finish(wire_len, "wire_len") || !finish(link_type, "link_type") || !finish(source_id, "source_id") ||
            !finish(sequence, "sequence") || !finish(raw_data, "raw_data") || !finish(layer_status, "layer_status") ||
            !finish(layer_count, "layer_count") || !finish(layer_ids, "layer_ids") ||
            !finish(layer_offsets, "layer_offsets") || !finish(endpoint_scope, "endpoint_scope") ||
            !finish(network_layer_index, "network_layer_index") ||
            !finish(transport_layer_index, "transport_layer_index") || !finish(payload_offset, "payload_offset") ||
            !finish(src_mac, "src_mac") || !finish(dst_mac, "dst_mac") || !finish(src_ip_v4, "src_ip_v4") ||
            !finish(dst_ip_v4, "dst_ip_v4") || !finish(src_ip_v6, "src_ip_v6") || !finish(dst_ip_v6, "dst_ip_v6") ||
            !finish(src_ip_family, "src_ip_family") || !finish(dst_ip_family, "dst_ip_family") ||
            !finish(transport_protocol, "transport_protocol") || !finish(src_port, "src_port") ||
            !finish(dst_port, "dst_port") || !finish(ports_valid, "ports_valid") ||
            !finish(protocol_status, "protocol_status") || !finish(protocol_id, "protocol_id") ||
            !finish(protocol_sub_id, "protocol_sub_id")) {
            return PacketBatchError::kArrowError;
        }
        *output = arrow::RecordBatch::Make(PacketSchema(), static_cast<int64_t>(rows), std::move(arrays));
        return PacketBatchError::kNone;
    } catch (const std::bad_alloc&) {
        return SetBatchError(PacketBatchError::kAllocationFailed, "packet batch allocation failed", error);
    }
}

std::shared_ptr<arrow::Schema> PacketSchema() {
    static const std::shared_ptr<arrow::Schema> schema = [] {
        auto fields = std::vector<std::shared_ptr<arrow::Field>>{
            arrow::field("timestamp_ns", arrow::int64(), false),
            arrow::field("captured_len", arrow::uint32(), false),
            arrow::field("wire_len", arrow::uint32(), false),
            arrow::field("link_type", arrow::uint32(), false),
            arrow::field("source_id", arrow::uint32(), false),
            arrow::field("sequence", arrow::uint64(), false),
            arrow::field("raw_data", arrow::binary(), false),
            arrow::field("layer_status", arrow::uint8(), false),
            arrow::field("layer_count", arrow::uint8(), false),
            arrow::field("layer_ids", arrow::fixed_size_list(arrow::uint16(), kMaxLayerDepth), false),
            arrow::field("layer_offsets", arrow::fixed_size_list(arrow::uint32(), kMaxLayerDepth), false),
            arrow::field("endpoint_scope", arrow::uint8(), false),
            arrow::field("network_layer_index", arrow::uint8(), false),
            arrow::field("transport_layer_index", arrow::uint8(), false),
            arrow::field("payload_offset", arrow::uint32(), false),
            arrow::field("src_mac", arrow::fixed_size_binary(6), true),
            arrow::field("dst_mac", arrow::fixed_size_binary(6), true),
            arrow::field("src_ip_v4", arrow::uint32(), true),
            arrow::field("dst_ip_v4", arrow::uint32(), true),
            arrow::field("src_ip_v6", arrow::binary(), true),
            arrow::field("dst_ip_v6", arrow::binary(), true),
            arrow::field("src_ip_family", arrow::uint8(), false),
            arrow::field("dst_ip_family", arrow::uint8(), false),
            arrow::field("transport_protocol", arrow::uint8(), true),
            arrow::field("src_port", arrow::uint16(), true),
            arrow::field("dst_port", arrow::uint16(), true),
            arrow::field("ports_valid", arrow::boolean(), false),
            arrow::field("protocol_status", arrow::uint8(), false),
            arrow::field("protocol_id", arrow::uint16(), true),
            arrow::field("protocol_sub_id", arrow::uint16(), true),
        };
        auto metadata = arrow::key_value_metadata(
            {"flowsql.entity", "flowsql.schema_version", "flowsql.timestamp_unit"}, {"packet", "1", "ns"});
        return arrow::schema(std::move(fields), std::move(metadata));
    }();
    return schema;
}

}  // namespace flowsql::packet
