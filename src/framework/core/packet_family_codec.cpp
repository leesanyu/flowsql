// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "packet_family_codec.h"
#include <arrow/api.h>
#include <cerrno>
#include <limits>
#include <new>

namespace flowsql::packet {
namespace {
PacketBatchError SetBatchError(PacketBatchError code, const char* message, std::string* error) {
    if (error) *error = message;
    return code;
}

PacketBatchError ValidatePacketRecord(const PacketFamilyRecord& record, std::string* error) {
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

PacketBatchError EncodePacketFamilyBatch(const PacketFamilyBatch& records, std::shared_ptr<arrow::RecordBatch>* output,
                                         std::string* error) {
    if (!output) return PacketBatchError::kNullOutput;
    if (records.rows.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return PacketBatchError::kInvalidRecord;
    size_t index = 0;
    return EncodePacketFamilyStream(
        [&](PacketFamilyBatch* scratch) {
            if (index == records.rows.size()) return ENOENT;
            auto row = records.rows[index++];
            if (row.address.family == AddressFamily::kIPv4) {
                if (row.address.index >= records.ipv4.size()) return EINVAL;
                scratch->ipv4.push_back(records.ipv4[row.address.index]);
                row.address.index = 0;
            } else if (row.address.family == AddressFamily::kIPv6) {
                if (row.address.index >= records.ipv6.size()) return EINVAL;
                scratch->ipv6.push_back(records.ipv6[row.address.index]);
                row.address.index = 0;
            }
            scratch->rows.push_back(std::move(row));
            return 0;
        },
        static_cast<uint32_t>(records.rows.size()), arrow::default_memory_pool(), output, error);
}

PacketBatchError EncodePacketFamilyStream(const std::function<int(PacketFamilyBatch*)>& next, uint32_t max_records,
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
        arrow::UInt32Builder src_ip_v4(pool), dst_ip_v4(pool);
        arrow::FixedSizeBinaryBuilder src_ip_v6(arrow::fixed_size_binary(16), pool);
        arrow::FixedSizeBinaryBuilder dst_ip_v6(arrow::fixed_size_binary(16), pool);
        arrow::Int8Builder codes(pool);
        arrow::Int32Builder offsets(pool);
        arrow::UInt8Builder address_states(pool);
        int32_t none_count = 0;
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
        PacketFamilyBatch scratch;
        EndpointScope expected_scope = EndpointScope::kInnermost;
        for (; rows < max_records; ++rows) {
            scratch.rows.clear();
            scratch.ipv4.clear();
            scratch.ipv6.clear();
            const int next_rc = next(&scratch);
            if (next_rc == ENOENT) break;
            if (next_rc != 0)
                return SetBatchError(PacketBatchError::kInvalidRecord, "packet stream read failed", error);
            if (scratch.rows.size() != 1)
                return SetBatchError(PacketBatchError::kInvalidRecord, "packet stream must supply exactly one row",
                                     error);
            const auto& record = scratch.rows.front();
            const auto validation = ValidatePacketRecord(record, error);
            if (validation != PacketBatchError::kNone) return validation;
            const auto& meta = record.meta;
            const auto& layer = record.layer;
            const auto& protocol = record.protocol;
            if (rows == 0) expected_scope = layer.endpoint_scope;
            if (layer.endpoint_scope != expected_scope)
                return SetBatchError(PacketBatchError::kInvalidRecord, "endpoint scope is mixed within a batch", error);
            const auto family = record.address.family;
            if (layer.address_state == PacketAddressState::kAvailable) {
                if (family == AddressFamily::kIPv4 && record.address.index < scratch.ipv4.size()) {
                    const auto& pair = scratch.ipv4[record.address.index];
                    if (!check(codes.Append(4), "ip_endpoints") ||
                        !check(offsets.Append(static_cast<int32_t>(src_ip_v4.length())), "ip_endpoints") ||
                        !check(src_ip_v4.Append(pair.src.ToUInt32()), "ipv4.src") ||
                        !check(dst_ip_v4.Append(pair.dst.ToUInt32()), "ipv4.dst"))
                        return PacketBatchError::kArrowError;
                } else if (family == AddressFamily::kIPv6 && record.address.index < scratch.ipv6.size()) {
                    const auto& pair = scratch.ipv6[record.address.index];
                    if (!check(codes.Append(6), "ip_endpoints") ||
                        !check(offsets.Append(static_cast<int32_t>(src_ip_v6.length())), "ip_endpoints") ||
                        !check(src_ip_v6.Append(pair.src.bytes), "ipv6.src") ||
                        !check(dst_ip_v6.Append(pair.dst.bytes), "ipv6.dst"))
                        return PacketBatchError::kArrowError;
                } else
                    return SetBatchError(PacketBatchError::kInvalidRecord,
                                         "available packet endpoint index/family is invalid", error);
            } else {
                if (family != AddressFamily::kNone || record.address.index != kNoAddressIndex ||
                    static_cast<uint8_t>(layer.address_state) > static_cast<uint8_t>(PacketAddressState::kIncomplete))
                    return SetBatchError(PacketBatchError::kInvalidRecord, "absent packet must not own an endpoint",
                                         error);
                if (!check(codes.Append(0), "ip_endpoints") || !check(offsets.Append(none_count++), "ip_endpoints"))
                    return PacketBatchError::kArrowError;
            }
            if (!check(address_states.Append(static_cast<uint8_t>(layer.address_state)), "ip_address_state"))
                return PacketBatchError::kArrowError;
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
                !(layer.src_mac_valid ? check(src_mac.Append(layer.src_mac.bytes), "src_mac")
                                      : check(src_mac.AppendNull(), "src_mac")) ||
                !(layer.dst_mac_valid ? check(dst_mac.Append(layer.dst_mac.bytes), "dst_mac")
                                      : check(dst_mac.AppendNull(), "dst_mac"))) {
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

        const auto schema = PacketFamilySchema();
        const auto union_type =
            std::static_pointer_cast<arrow::DenseUnionType>(schema->GetFieldByName("ip_endpoints")->type());
        std::shared_ptr<arrow::Array> src4, dst4, src6, dst6, type_codes, value_offsets;
        if (!check(src_ip_v4.Finish(&src4), "ipv4.src") || !check(dst_ip_v4.Finish(&dst4), "ipv4.dst") ||
            !check(src_ip_v6.Finish(&src6), "ipv6.src") || !check(dst_ip_v6.Finish(&dst6), "ipv6.dst") ||
            !check(codes.Finish(&type_codes), "ip_endpoints") || !check(offsets.Finish(&value_offsets), "ip_endpoints"))
            return PacketBatchError::kArrowError;
        auto pair4 = std::make_shared<arrow::StructArray>(union_type->field(1)->type(), src4->length(),
                                                          arrow::ArrayVector{src4, dst4});
        auto pair6 = std::make_shared<arrow::StructArray>(union_type->field(2)->type(), src6->length(),
                                                          arrow::ArrayVector{src6, dst6});
        auto endpoints = std::make_shared<arrow::DenseUnionArray>(
            union_type, rows, arrow::ArrayVector{std::make_shared<arrow::NullArray>(none_count), pair4, pair6},
            type_codes->data()->buffers[1], value_offsets->data()->buffers[1]);
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
            !finish(src_mac, "src_mac") || !finish(dst_mac, "dst_mac") || !finish(address_states, "ip_address_state") ||
            !finish(transport_protocol, "transport_protocol") || !finish(src_port, "src_port") ||
            !finish(dst_port, "dst_port") || !finish(ports_valid, "ports_valid") ||
            !finish(protocol_status, "protocol_status") || !finish(protocol_id, "protocol_id") ||
            !finish(protocol_sub_id, "protocol_sub_id")) {
            return PacketBatchError::kArrowError;
        }
        arrays.insert(arrays.begin() + 18, endpoints);
        *output = arrow::RecordBatch::Make(PacketFamilySchema(), static_cast<int64_t>(rows), std::move(arrays));
        const auto validation = ValidatePacketFamilyBatch(**output);
        if (!validation.ok()) {
            *output = nullptr;
            if (error) *error = validation.ToString();
            return PacketBatchError::kInvalidRecord;
        }
        return PacketBatchError::kNone;
    } catch (const std::bad_alloc&) {
        return SetBatchError(PacketBatchError::kAllocationFailed, "packet batch allocation failed", error);
    }
}

}  // namespace flowsql::packet
