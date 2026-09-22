// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_packet_batch_view.h"

#include <framework/core/packet_codec.h>

#include <arrow/api.h>

#include <cstring>
#include <new>
#include <utility>

namespace flowsql::npm {

namespace {

enum PacketColumn : int {
    kTimestampNs = 0,
    kCapturedLen,
    kWireLen,
    kLinkType,
    kSourceId,
    kSequence,
    kRawData,
    kLayerStatus,
    kLayerCount,
    kLayerIds,
    kLayerOffsets,
    kEndpointScope,
    kNetworkLayerIndex,
    kTransportLayerIndex,
    kPayloadOffset,
    kSrcMac,
    kDstMac,
    kSrcIpV4,
    kDstIpV4,
    kSrcIpV6,
    kDstIpV6,
    kSrcIpFamily,
    kDstIpFamily,
    kTransportProtocol,
    kSrcPort,
    kDstPort,
    kPortsValid,
    kProtocolStatus,
    kProtocolId,
    kProtocolSubId,
    kPacketColumnCount,
};

NpmPacketBatchError Fail(NpmPacketBatchError code, const std::string& message, std::string* error) {
    if (error != nullptr) *error = message;
    return code;
}

bool ValidLayerStatus(uint8_t value) {
    switch (static_cast<packet::LayerStatus>(value)) {
        case packet::LayerStatus::kNotDecoded:
        case packet::LayerStatus::kDecoded:
        case packet::LayerStatus::kTruncated:
        case packet::LayerStatus::kMalformed:
        case packet::LayerStatus::kUnsupportedLinkType:
            return true;
    }
    return false;
}

bool ValidEndpointScope(uint8_t value) {
    return value == static_cast<uint8_t>(packet::EndpointScope::kInnermost) ||
           value == static_cast<uint8_t>(packet::EndpointScope::kOutermost);
}

bool ValidProtocolStatus(uint8_t value) {
    switch (static_cast<packet::ProtocolStatus>(value)) {
        case packet::ProtocolStatus::kNotAttempted:
        case packet::ProtocolStatus::kIdentified:
        case packet::ProtocolStatus::kUnknown:
            return true;
    }
    return false;
}

}  // namespace

struct NpmPacketBatchView::Columns {
    std::shared_ptr<arrow::Int64Array> timestamp_ns;
    std::shared_ptr<arrow::UInt32Array> captured_len;
    std::shared_ptr<arrow::UInt32Array> wire_len;
    std::shared_ptr<arrow::UInt32Array> link_type;
    std::shared_ptr<arrow::UInt32Array> source_id;
    std::shared_ptr<arrow::UInt64Array> sequence;
    std::shared_ptr<arrow::BinaryArray> raw_data;
    std::shared_ptr<arrow::UInt8Array> layer_status;
    std::shared_ptr<arrow::UInt8Array> layer_count;
    std::shared_ptr<arrow::FixedSizeListArray> layer_ids;
    std::shared_ptr<arrow::UInt16Array> layer_id_values;
    std::shared_ptr<arrow::FixedSizeListArray> layer_offsets;
    std::shared_ptr<arrow::UInt32Array> layer_offset_values;
    std::shared_ptr<arrow::UInt8Array> endpoint_scope;
    std::shared_ptr<arrow::UInt8Array> network_layer_index;
    std::shared_ptr<arrow::UInt8Array> transport_layer_index;
    std::shared_ptr<arrow::UInt32Array> payload_offset;
    std::shared_ptr<arrow::FixedSizeBinaryArray> src_mac;
    std::shared_ptr<arrow::FixedSizeBinaryArray> dst_mac;
    std::shared_ptr<arrow::UInt32Array> src_ip_v4;
    std::shared_ptr<arrow::UInt32Array> dst_ip_v4;
    std::shared_ptr<arrow::BinaryArray> src_ip_v6;
    std::shared_ptr<arrow::BinaryArray> dst_ip_v6;
    std::shared_ptr<arrow::UInt8Array> src_ip_family;
    std::shared_ptr<arrow::UInt8Array> dst_ip_family;
    std::shared_ptr<arrow::UInt8Array> transport_protocol;
    std::shared_ptr<arrow::UInt16Array> src_port;
    std::shared_ptr<arrow::UInt16Array> dst_port;
    std::shared_ptr<arrow::BooleanArray> ports_valid;
    std::shared_ptr<arrow::UInt8Array> protocol_status;
    std::shared_ptr<arrow::UInt16Array> protocol_id;
    std::shared_ptr<arrow::UInt16Array> protocol_sub_id;
};

namespace {

NpmPacketBatchError ValidateIp(int64_t row, const char* prefix, const arrow::UInt8Array& family,
                               const arrow::UInt32Array& v4, const arrow::BinaryArray& v6, std::string* error) {
    const uint8_t value = family.Value(row);
    const bool has_v4 = !v4.IsNull(row);
    const bool has_v6 = !v6.IsNull(row);
    switch (static_cast<packet::AddressFamily>(value)) {
        case packet::AddressFamily::kNone:
            if (!has_v4 && !has_v6) return NpmPacketBatchError::kNone;
            break;
        case packet::AddressFamily::kIPv4:
            if (has_v4 && !has_v6) return NpmPacketBatchError::kNone;
            break;
        case packet::AddressFamily::kIPv6:
            if (!has_v4 && has_v6 && v6.GetView(row).size() == sizeof(packet::IPv6Address)) {
                return NpmPacketBatchError::kNone;
            }
            if (has_v6 && v6.GetView(row).size() != sizeof(packet::IPv6Address)) {
                return Fail(NpmPacketBatchError::kInvalidRow, std::string(prefix) + "_ip_v6 must contain 16 bytes",
                            error);
            }
            break;
    }
    return Fail(NpmPacketBatchError::kInvalidRow,
                std::string(prefix) + "_ip family does not match nullable address columns", error);
}

}  // namespace

NpmPacketBatchError NpmPacketBatchView::ValidateRow(int64_t row, const NpmPacketBatchView::Columns& columns,
                                                    std::string* error) {
    int32_t raw_size = 0;
    columns.raw_data->GetValue(row, &raw_size);
    const uint32_t captured_len = columns.captured_len->Value(row);
    if (captured_len != static_cast<uint32_t>(raw_size)) {
        return Fail(NpmPacketBatchError::kInvalidRow, "captured_len does not match raw_data length", error);
    }
    const uint32_t wire_len = columns.wire_len->Value(row);
    if (wire_len != 0 && wire_len < captured_len) {
        return Fail(NpmPacketBatchError::kInvalidRow, "wire_len is smaller than captured_len", error);
    }

    if (!ValidLayerStatus(columns.layer_status->Value(row))) {
        return Fail(NpmPacketBatchError::kInvalidRow, "layer_status is invalid", error);
    }
    const uint8_t layer_count = columns.layer_count->Value(row);
    if (layer_count > packet::kMaxLayerDepth) {
        return Fail(NpmPacketBatchError::kInvalidRow, "layer_count exceeds kMaxLayerDepth", error);
    }
    const int64_t offset_base = columns.layer_offsets->value_offset(row);
    for (uint8_t index = 0; index < layer_count; ++index) {
        if (columns.layer_offset_values->Value(offset_base + index) > captured_len) {
            return Fail(NpmPacketBatchError::kInvalidRow, "layer_offsets contains an out-of-bounds offset", error);
        }
    }
    if (!ValidEndpointScope(columns.endpoint_scope->Value(row))) {
        return Fail(NpmPacketBatchError::kInvalidRow, "endpoint_scope is invalid", error);
    }
    const uint8_t network_index = columns.network_layer_index->Value(row);
    if (network_index != packet::kNoLayerIndex && network_index >= layer_count) {
        return Fail(NpmPacketBatchError::kInvalidRow, "network_layer_index exceeds layer_count", error);
    }
    const uint8_t transport_index = columns.transport_layer_index->Value(row);
    if (transport_index != packet::kNoLayerIndex && transport_index >= layer_count) {
        return Fail(NpmPacketBatchError::kInvalidRow, "transport_layer_index exceeds layer_count", error);
    }
    if (columns.payload_offset->Value(row) > captured_len) {
        return Fail(NpmPacketBatchError::kInvalidRow, "payload_offset exceeds captured_len", error);
    }

    auto result = ValidateIp(row, "src", *columns.src_ip_family, *columns.src_ip_v4, *columns.src_ip_v6, error);
    if (result != NpmPacketBatchError::kNone) return result;
    result = ValidateIp(row, "dst", *columns.dst_ip_family, *columns.dst_ip_v4, *columns.dst_ip_v6, error);
    if (result != NpmPacketBatchError::kNone) return result;

    const bool ports_valid = columns.ports_valid->Value(row);
    const bool has_src_port = !columns.src_port->IsNull(row);
    const bool has_dst_port = !columns.dst_port->IsNull(row);
    if ((ports_valid && (!has_src_port || !has_dst_port)) || (!ports_valid && (has_src_port || has_dst_port))) {
        return Fail(NpmPacketBatchError::kInvalidRow, "ports_valid does not match nullable port columns", error);
    }

    const uint8_t protocol_status = columns.protocol_status->Value(row);
    if (!ValidProtocolStatus(protocol_status)) {
        return Fail(NpmPacketBatchError::kInvalidRow, "protocol_status is invalid", error);
    }
    const bool has_protocol_id = !columns.protocol_id->IsNull(row);
    const bool has_protocol_sub_id = !columns.protocol_sub_id->IsNull(row);
    const bool identified = protocol_status == static_cast<uint8_t>(packet::ProtocolStatus::kIdentified);
    if ((identified && (!has_protocol_id || !has_protocol_sub_id)) ||
        (!identified && (has_protocol_id || has_protocol_sub_id))) {
        return Fail(NpmPacketBatchError::kInvalidRow, "protocol_status does not match nullable protocol ID columns",
                    error);
    }
    return NpmPacketBatchError::kNone;
}

namespace {

template <typename ArrayType>
std::shared_ptr<ArrayType> PacketColumnAs(const std::shared_ptr<arrow::RecordBatch>& batch, int index) {
    return std::static_pointer_cast<ArrayType>(batch->column(index));
}

}  // namespace

NpmPacketBatchView::NpmPacketBatchView(std::shared_ptr<arrow::RecordBatch> batch, std::unique_ptr<Columns> columns)
    : batch_(std::move(batch)), columns_(std::move(columns)) {}

NpmPacketBatchView::~NpmPacketBatchView() = default;

NpmPacketBatchError NpmPacketBatchView::Create(const std::shared_ptr<arrow::RecordBatch>& batch,
                                               std::unique_ptr<NpmPacketBatchView>* output, std::string* error) {
    if (output == nullptr) return Fail(NpmPacketBatchError::kNullOutput, "output is null", error);
    if (batch == nullptr) return Fail(NpmPacketBatchError::kNullInput, "packet batch is null", error);
    if (error != nullptr) error->clear();

    const auto expected_schema = packet::PacketSchema();
    if (batch->schema() == nullptr || !batch->schema()->Equals(*expected_schema, true)) {
        return Fail(NpmPacketBatchError::kSchemaMismatch, "packet batch schema does not match PacketSchema", error);
    }
    if (batch->num_columns() != kPacketColumnCount) {
        return Fail(NpmPacketBatchError::kInvalidColumn, "packet batch column count is invalid", error);
    }
    for (int index = 0; index < kPacketColumnCount; ++index) {
        const auto& column = batch->column(index);
        const auto& field = expected_schema->field(index);
        if (column == nullptr || !column->type()->Equals(field->type())) {
            return Fail(NpmPacketBatchError::kInvalidColumn, field->name() + " column type is invalid", error);
        }
        if (column->length() != batch->num_rows()) {
            return Fail(NpmPacketBatchError::kInvalidColumn, field->name() + " column length is invalid", error);
        }
        if (!field->nullable() && column->null_count() != 0) {
            return Fail(NpmPacketBatchError::kInvalidColumn, field->name() + " contains null", error);
        }
    }
    const auto validation = batch->ValidateFull();
    if (!validation.ok()) {
        return Fail(NpmPacketBatchError::kInvalidColumn,
                    std::string("packet batch Arrow validation failed: ") + validation.ToString(), error);
    }

    try {
        auto columns = std::make_unique<Columns>();
        columns->timestamp_ns = PacketColumnAs<arrow::Int64Array>(batch, kTimestampNs);
        columns->captured_len = PacketColumnAs<arrow::UInt32Array>(batch, kCapturedLen);
        columns->wire_len = PacketColumnAs<arrow::UInt32Array>(batch, kWireLen);
        columns->link_type = PacketColumnAs<arrow::UInt32Array>(batch, kLinkType);
        columns->source_id = PacketColumnAs<arrow::UInt32Array>(batch, kSourceId);
        columns->sequence = PacketColumnAs<arrow::UInt64Array>(batch, kSequence);
        columns->raw_data = PacketColumnAs<arrow::BinaryArray>(batch, kRawData);
        columns->layer_status = PacketColumnAs<arrow::UInt8Array>(batch, kLayerStatus);
        columns->layer_count = PacketColumnAs<arrow::UInt8Array>(batch, kLayerCount);
        columns->layer_ids = PacketColumnAs<arrow::FixedSizeListArray>(batch, kLayerIds);
        columns->layer_id_values = std::static_pointer_cast<arrow::UInt16Array>(columns->layer_ids->values());
        columns->layer_offsets = PacketColumnAs<arrow::FixedSizeListArray>(batch, kLayerOffsets);
        columns->layer_offset_values = std::static_pointer_cast<arrow::UInt32Array>(columns->layer_offsets->values());
        columns->endpoint_scope = PacketColumnAs<arrow::UInt8Array>(batch, kEndpointScope);
        columns->network_layer_index = PacketColumnAs<arrow::UInt8Array>(batch, kNetworkLayerIndex);
        columns->transport_layer_index = PacketColumnAs<arrow::UInt8Array>(batch, kTransportLayerIndex);
        columns->payload_offset = PacketColumnAs<arrow::UInt32Array>(batch, kPayloadOffset);
        columns->src_mac = PacketColumnAs<arrow::FixedSizeBinaryArray>(batch, kSrcMac);
        columns->dst_mac = PacketColumnAs<arrow::FixedSizeBinaryArray>(batch, kDstMac);
        columns->src_ip_v4 = PacketColumnAs<arrow::UInt32Array>(batch, kSrcIpV4);
        columns->dst_ip_v4 = PacketColumnAs<arrow::UInt32Array>(batch, kDstIpV4);
        columns->src_ip_v6 = PacketColumnAs<arrow::BinaryArray>(batch, kSrcIpV6);
        columns->dst_ip_v6 = PacketColumnAs<arrow::BinaryArray>(batch, kDstIpV6);
        columns->src_ip_family = PacketColumnAs<arrow::UInt8Array>(batch, kSrcIpFamily);
        columns->dst_ip_family = PacketColumnAs<arrow::UInt8Array>(batch, kDstIpFamily);
        columns->transport_protocol = PacketColumnAs<arrow::UInt8Array>(batch, kTransportProtocol);
        columns->src_port = PacketColumnAs<arrow::UInt16Array>(batch, kSrcPort);
        columns->dst_port = PacketColumnAs<arrow::UInt16Array>(batch, kDstPort);
        columns->ports_valid = PacketColumnAs<arrow::BooleanArray>(batch, kPortsValid);
        columns->protocol_status = PacketColumnAs<arrow::UInt8Array>(batch, kProtocolStatus);
        columns->protocol_id = PacketColumnAs<arrow::UInt16Array>(batch, kProtocolId);
        columns->protocol_sub_id = PacketColumnAs<arrow::UInt16Array>(batch, kProtocolSubId);

        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            const int64_t id_base = columns->layer_ids->value_offset(row);
            const int64_t offset_base = columns->layer_offsets->value_offset(row);
            for (size_t index = 0; index < packet::kMaxLayerDepth; ++index) {
                if (columns->layer_id_values->IsNull(id_base + index)) {
                    return Fail(NpmPacketBatchError::kInvalidColumn, "layer_ids values contain null", error);
                }
                if (columns->layer_offset_values->IsNull(offset_base + index)) {
                    return Fail(NpmPacketBatchError::kInvalidColumn, "layer_offsets values contain null", error);
                }
            }
            const auto row_result = ValidateRow(row, *columns, error);
            if (row_result != NpmPacketBatchError::kNone) return row_result;
        }

        auto view = std::unique_ptr<NpmPacketBatchView>(new NpmPacketBatchView(batch, std::move(columns)));
        *output = std::move(view);
        return NpmPacketBatchError::kNone;
    } catch (const std::bad_alloc&) {
        return Fail(NpmPacketBatchError::kAllocationFailed, "packet batch view allocation failed", error);
    }
}

int64_t NpmPacketBatchView::num_rows() const { return batch_->num_rows(); }

NpmPacketBatchError NpmPacketBatchView::Get(int64_t row, packet::PacketView* packet_view,
                                            packet::PacketLayerInfo* layer_info) const {
    if (packet_view == nullptr || layer_info == nullptr) return NpmPacketBatchError::kNullOutput;
    if (row < 0 || row >= batch_->num_rows()) return NpmPacketBatchError::kInvalidRow;

    packet::PacketView next_packet;
    next_packet.meta.timestamp_ns = columns_->timestamp_ns->Value(row);
    next_packet.meta.captured_len = columns_->captured_len->Value(row);
    next_packet.meta.wire_len = columns_->wire_len->Value(row);
    next_packet.meta.link_type = columns_->link_type->Value(row);
    next_packet.meta.source_id = columns_->source_id->Value(row);
    next_packet.meta.sequence = columns_->sequence->Value(row);
    int32_t raw_size = 0;
    const uint8_t* raw_data = columns_->raw_data->GetValue(row, &raw_size);
    next_packet.bytes = Span<const uint8_t>(raw_data, static_cast<size_t>(raw_size));

    packet::PacketLayerInfo next_layer;
    next_layer.status = static_cast<packet::LayerStatus>(columns_->layer_status->Value(row));
    next_layer.layer_count = columns_->layer_count->Value(row);
    const int64_t id_base = columns_->layer_ids->value_offset(row);
    const int64_t offset_base = columns_->layer_offsets->value_offset(row);
    for (size_t index = 0; index < packet::kMaxLayerDepth; ++index) {
        next_layer.layers[index].kind = columns_->layer_id_values->Value(id_base + index);
        next_layer.layers[index].offset = columns_->layer_offset_values->Value(offset_base + index);
    }
    next_layer.endpoint_scope = static_cast<packet::EndpointScope>(columns_->endpoint_scope->Value(row));
    next_layer.network_layer_index = columns_->network_layer_index->Value(row);
    next_layer.transport_layer_index = columns_->transport_layer_index->Value(row);
    next_layer.payload_offset = columns_->payload_offset->Value(row);

    auto decode_mac = [row](const arrow::FixedSizeBinaryArray& source, packet::MacAddress* destination) {
        if (source.IsNull(row)) return;
        std::memcpy(destination->value.bytes, source.GetValue(row), sizeof(destination->value.bytes));
        destination->valid = 1;
    };
    decode_mac(*columns_->src_mac, &next_layer.src_mac);
    decode_mac(*columns_->dst_mac, &next_layer.dst_mac);

    auto decode_ip = [row](const arrow::UInt8Array& family, const arrow::UInt32Array& v4, const arrow::BinaryArray& v6,
                           packet::IpAddress* destination) {
        if (family.Value(row) == static_cast<uint8_t>(packet::AddressFamily::kIPv4)) {
            packet::IPv4Address address;
            address.addr = v4.Value(row);
            *destination = address;
        } else if (family.Value(row) == static_cast<uint8_t>(packet::AddressFamily::kIPv6)) {
            packet::IPv6Address address;
            const auto value = v6.GetView(row);
            std::memcpy(address.bytes, value.data(), sizeof(address.bytes));
            *destination = address;
        }
    };
    decode_ip(*columns_->src_ip_family, *columns_->src_ip_v4, *columns_->src_ip_v6, &next_layer.src_ip);
    decode_ip(*columns_->dst_ip_family, *columns_->dst_ip_v4, *columns_->dst_ip_v6, &next_layer.dst_ip);

    if (!columns_->transport_protocol->IsNull(row)) {
        next_layer.transport_protocol = columns_->transport_protocol->Value(row);
    }
    next_layer.ports_valid = columns_->ports_valid->Value(row) ? 1 : 0;
    if (next_layer.ports_valid != 0) {
        next_layer.src_port = columns_->src_port->Value(row);
        next_layer.dst_port = columns_->dst_port->Value(row);
    }

    *packet_view = next_packet;
    *layer_info = next_layer;
    return NpmPacketBatchError::kNone;
}

}  // namespace flowsql::npm
