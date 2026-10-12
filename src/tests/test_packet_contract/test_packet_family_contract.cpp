// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/ipacket_family.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace {
namespace packet = flowsql::packet;

static_assert(sizeof(packet::PacketIpEndpoints4) == 8 && alignof(packet::PacketIpEndpoints4) == 4);
static_assert(sizeof(packet::PacketIpEndpoints6) == 32 && alignof(packet::PacketIpEndpoints6) == 16);
static_assert(offsetof(packet::PacketIpEndpoints6, dst) == 16);
static_assert(std::is_abstract_v<packet::IPacketAddressSink>);
static_assert(std::is_abstract_v<packet::IPacketFamilyLayerDecoder>);
static_assert(std::is_abstract_v<packet::IPacketFamilyProtocolIdentifier>);

void Check(const arrow::Status& status) {
    if (!status.ok()) std::fprintf(stderr, "%s\n", status.ToString().c_str());
    assert(status.ok());
}
template <typename T>
T Get(arrow::Result<T> result) {
    Check(result.status());
    return std::move(result).ValueOrDie();
}

std::shared_ptr<arrow::RecordBatch> Fixture(bool only4 = false, bool null_address = false) {
    const auto schema = packet::PacketFamilySchema();
    auto type = schema->GetFieldByName("ip_endpoints")->type();
    const auto& union_type = static_cast<const arrow::DenseUnionType&>(*type);
    arrow::UInt32Builder src4, dst4;
    if (null_address)
        Check(src4.AppendNull());
    else
        Check(src4.Append(0x01020304u));
    Check(src4.Append(0xffffffffu));
    Check(dst4.AppendValues({0xc0000201u, 0u}));
    auto pair4 = std::make_shared<arrow::StructArray>(union_type.field(1)->type(), 2,
                                                      arrow::ArrayVector{Get(src4.Finish()), Get(dst4.Finish())});
    arrow::FixedSizeBinaryBuilder src6(arrow::fixed_size_binary(16)), dst6(arrow::fixed_size_binary(16));
    const uint8_t a[16] = {0x20, 1, 0x0d, 0xb8, 0, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const uint8_t b[16] = {0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    if (!only4) {
        Check(src6.Append(a));
        Check(src6.Append(b));
        Check(dst6.Append(b));
        Check(dst6.Append(a));
    }
    auto pair6 = std::make_shared<arrow::StructArray>(union_type.field(2)->type(), only4 ? 0 : 2,
                                                      arrow::ArrayVector{Get(src6.Finish()), Get(dst6.Finish())});
    arrow::Int8Builder codes;
    arrow::Int32Builder offsets;
    arrow::UInt8Builder states;
    if (only4) {
        Check(codes.AppendValues({4, 4}));
        Check(offsets.AppendValues({0, 1}));
        Check(states.AppendValues({2, 2}));
    } else {
        Check(codes.AppendValues({4, 6, 0, 4, 0, 6, 0}));
        Check(offsets.AppendValues({0, 0, 0, 1, 1, 1, 2}));
        Check(states.AppendValues({2, 2, 1, 2, 3, 2, 0}));
    }
    const auto type_codes = Get(codes.Finish());
    const auto value_offsets = Get(offsets.Finish());
    auto endpoints = std::make_shared<arrow::DenseUnionArray>(
        type, type_codes->length(), arrow::ArrayVector{std::make_shared<arrow::NullArray>(only4 ? 0 : 3), pair4, pair6},
        type_codes->data()->buffers[1], value_offsets->data()->buffers[1]);
    arrow::ArrayVector columns;
    for (const auto& field : schema->fields()) {
        auto builder = Get(arrow::MakeBuilder(field->type()));
        if (field->name() == "ip_endpoints") {
            columns.push_back(endpoints);
        } else if (field->name() == "ip_address_state") {
            columns.push_back(Get(states.Finish()));
        } else {
            Check(builder->AppendEmptyValues(type_codes->length()));
            columns.push_back(Get(builder->Finish()));
        }
    }
    return arrow::RecordBatch::Make(schema, type_codes->length(), columns);
}

void TestMixedOrderAndStorage() {
    auto batch = Fixture();
    Check(packet::ValidatePacketFamilyBatch(*batch));
    const auto& endpoints = static_cast<const arrow::DenseUnionArray&>(*batch->GetColumnByName("ip_endpoints"));
    const int8_t expected_codes[] = {4, 6, 0, 4, 0, 6, 0};
    const int32_t expected_offsets[] = {0, 0, 0, 1, 1, 1, 2};
    for (int64_t row = 0; row < batch->num_rows(); ++row) {
        assert(endpoints.type_code(row) == expected_codes[row]);
        assert(endpoints.value_offset(row) == expected_offsets[row]);
    }
    assert(endpoints.field(1)->length() == 2 && endpoints.field(2)->length() == 2);
    const auto& v4 = static_cast<const arrow::StructArray&>(*endpoints.field(1));
    const auto& v6 = static_cast<const arrow::StructArray&>(*endpoints.field(2));
    assert(static_cast<const arrow::UInt32Array&>(*v4.field(0)).Value(0) == 0x01020304u);
    assert(static_cast<const arrow::UInt32Array&>(*v4.field(0)).Value(1) == 0xffffffffu);
    assert(v4.field(0)->data()->buffers[1]->size() == 2 * 4);
    assert(v6.field(0)->data()->buffers[1]->size() == 2 * 16);
    const auto* bytes = static_cast<const arrow::FixedSizeBinaryArray&>(*v6.field(0)).GetValue(0);
    assert(bytes[4] == 0 && bytes[5] == 0xff && bytes[6] == 0x80 && bytes[15] == 1);

    auto slice = batch->Slice(1, 4);
    Check(packet::ValidatePacketFamilyBatch(*slice));
    const auto& sliced = static_cast<const arrow::DenseUnionArray&>(*slice->GetColumnByName("ip_endpoints"));
    assert(sliced.type_code(0) == 6 && sliced.value_offset(0) == 0);
    assert(sliced.type_code(2) == 4 && sliced.value_offset(2) == 1);

    auto sink = Get(arrow::io::BufferOutputStream::Create());
    auto writer = Get(arrow::ipc::MakeStreamWriter(sink, batch->schema()));
    Check(writer->WriteRecordBatch(*batch));
    Check(writer->Close());
    auto serialized = Get(sink->Finish());
    auto reader = Get(arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(serialized)));
    auto restored = Get(reader->Next());
    Check(packet::ValidatePacketFamilyBatch(*restored));
    assert(batch->Equals(*restored, true));
    assert(!Get(reader->Next()));  // One mixed batch, not separate streams per family.

    auto only4 = Fixture(true);
    Check(packet::ValidatePacketFamilyBatch(*only4));
    const auto& compact = static_cast<const arrow::DenseUnionArray&>(*only4->GetColumnByName("ip_endpoints"));
    assert(compact.field(2)->length() == 0);
    const auto& empty6 = static_cast<const arrow::StructArray&>(*compact.field(2));
    assert(empty6.field(0)->data()->buffers[1]->size() == 0);
}

void TestRejectionsAndFilterTypes() {
    arrow::FixedSizeBinaryArray short_value(arrow::fixed_size_binary(16), 1,
                                            arrow::Buffer::FromString(std::string(15, '\0')));
    assert(!short_value.ValidateFull().ok());
    assert(!packet::ValidatePacketFamilyBatch(*Fixture(false, true)).ok());
    auto batch = Fixture();
    auto columns = batch->columns();
    arrow::UInt8Builder states;
    Check(states.AppendValues({0, 2, 1, 2, 3, 2, 0}));  // IPv4 endpoints cannot mean not requested.
    columns[batch->schema()->GetFieldIndex("ip_address_state")] = Get(states.Finish());
    auto invalid = arrow::RecordBatch::Make(batch->schema(), batch->num_rows(), columns);
    assert(!packet::ValidatePacketFamilyBatch(*invalid).ok());
    auto fields = batch->schema()->fields();
    fields[0] = arrow::field("timestamp_ns", arrow::uint64(), false);
    invalid = arrow::RecordBatch::Make(arrow::schema(fields), batch->num_rows(), batch->columns());
    assert(!packet::ValidatePacketFamilyBatch(*invalid).ok());
    auto filter = packet::PacketFamilyFilterSchema();
    assert(filter->GetFieldByName("src_ip_v4")->type()->Equals(arrow::uint32()));
    assert(filter->GetFieldByName("dst_ip_v6")->type()->Equals(arrow::fixed_size_binary(16)));
    assert(filter->GetFieldByName("src_ip_v6")->nullable());
    assert(!filter->GetFieldByName("ip_endpoints"));
}

void TestOwnedBatchReferences() {
    packet::PacketFamilyBatch batch;
    batch.ipv4.push_back({flowsql::network::IPv4Address::FromUInt32(0x01020304u), {}});
    batch.ipv6.push_back({});
    auto raw = std::make_shared<std::vector<uint8_t>>(4, 0x80);
    packet::PacketFamilyRecord record;
    record.raw_data = {raw, raw->data(), 4};
    record.layer.address_state = packet::PacketAddressState::kAvailable;
    record.address = {packet::AddressFamily::kIPv6, 0};
    batch.rows.push_back(record);
    record.address = {packet::AddressFamily::kIPv4, 0};
    batch.rows.push_back(record);
    batch.rows.push_back({});
    raw.reset();
    record = {};
    assert(batch.rows[0].raw_data.data[0] == 0x80);
    assert(batch.rows[0].address.family == packet::AddressFamily::kIPv6);
    assert(batch.ipv4[batch.rows[1].address.index].src.ToUInt32() == 0x01020304u);
    assert(batch.rows[2].address.index == packet::kNoAddressIndex);
}
}  // namespace

int main() {
    TestMixedOrderAndStorage();
    TestRejectionsAndFilterTypes();
    TestOwnedBatchReferences();
    std::puts("Packet family contract: mixed order, compact storage, slice/IPC, nulls and ownership passed");
}
