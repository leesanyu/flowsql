// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/ipacket_family.h>

#include <arrow/api.h>

namespace flowsql::packet {

std::shared_ptr<arrow::Schema> PacketFamilySchema() {
    auto endpoints = arrow::dense_union(
        {arrow::field("none", arrow::null()),
         arrow::field(
             "ipv4",
             arrow::struct_({arrow::field("src", arrow::uint32(), false), arrow::field("dst", arrow::uint32(), false)}),
             false),
         arrow::field("ipv6",
                      arrow::struct_({arrow::field("src", arrow::fixed_size_binary(16), false),
                                      arrow::field("dst", arrow::fixed_size_binary(16), false)}),
                      false)},
        {0, 4, 6});
    return arrow::schema({arrow::field("timestamp_ns", arrow::int64(), false),
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
                          arrow::field("ip_address_state", arrow::uint8(), false),
                          arrow::field("ip_endpoints", endpoints, false),
                          arrow::field("transport_protocol", arrow::uint8(), true),
                          arrow::field("src_port", arrow::uint16(), true),
                          arrow::field("dst_port", arrow::uint16(), true),
                          arrow::field("ports_valid", arrow::boolean(), false),
                          arrow::field("protocol_status", arrow::uint8(), false),
                          arrow::field("protocol_id", arrow::uint16(), true),
                          arrow::field("protocol_sub_id", arrow::uint16(), true)},
                         arrow::key_value_metadata({"flowsql.entity", "flowsql.schema_version",
                                                    "flowsql.timestamp_unit", "flowsql.ip_storage"},
                                                   {"packet", "2", "ns", "dense_family_endpoints"}));
}

std::shared_ptr<arrow::Schema> PacketFamilyFilterSchema() {
    auto fields = PacketFamilySchema()->fields();
    fields.erase(fields.begin() + 18);
    fields.insert(
        fields.end(),
        {arrow::field("src_ip_v4", arrow::uint32(), true), arrow::field("dst_ip_v4", arrow::uint32(), true),
         arrow::field("src_ip_v6", arrow::fixed_size_binary(16), true),
         arrow::field("dst_ip_v6", arrow::fixed_size_binary(16), true),
         arrow::field("src_ip_family", arrow::uint8(), false), arrow::field("dst_ip_family", arrow::uint8(), false)});
    return arrow::schema(std::move(fields));
}

arrow::Status ValidatePacketFamilyBatch(const arrow::RecordBatch& batch) {
    if (!batch.schema()->Equals(*PacketFamilySchema(), true))
        return arrow::Status::Invalid("packet family Schema mismatch");
    ARROW_RETURN_NOT_OK(batch.ValidateFull());
    for (int index = 0; index < batch.num_columns(); ++index) {
        if (!batch.schema()->field(index)->nullable() && batch.column(index)->null_count() != 0)
            return arrow::Status::Invalid("NULL common packet field: ", batch.schema()->field(index)->name());
    }
    const auto& states = static_cast<const arrow::UInt8Array&>(*batch.GetColumnByName("ip_address_state"));
    const auto& endpoints = static_cast<const arrow::DenseUnionArray&>(*batch.GetColumnByName("ip_endpoints"));
    for (int64_t row = 0; row < batch.num_rows(); ++row) {
        const auto state = states.Value(row);
        const auto family = endpoints.type_code(row);
        if (state > static_cast<uint8_t>(PacketAddressState::kIncomplete) ||
            ((state == static_cast<uint8_t>(PacketAddressState::kAvailable)) != (family == 4 || family == 6)))
            return arrow::Status::Invalid("packet address state/family mismatch at row ", row);
        if (family == 0) continue;
        const auto& pair = static_cast<const arrow::StructArray&>(*endpoints.field(endpoints.child_id(row)));
        const auto offset = endpoints.value_offset(row);
        if (pair.IsNull(offset) || pair.field(0)->IsNull(offset) || pair.field(1)->IsNull(offset))
            return arrow::Status::Invalid("NULL available packet endpoints at row ", row);
    }
    return arrow::Status::OK();
}

}  // namespace flowsql::packet
