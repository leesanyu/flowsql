// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arrow/api.h>
#include <channels/pcapfile/packet_filter_domain.h>
#include <framework/core/filter_binding.h>
#include <framework/core/filter_executor.h>
#include <framework/core/filter_planner.h>
#include <framework/core/packet_family_codec.h>
#include <operators/npm_basic/core/npm_packet_batch_view.h>
#include <plugins/npi/packet_decoder.h>
#include <cassert>
#include <cerrno>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#include <common/loader.hpp>
#pragma GCC diagnostic pop
#include <cstdio>

void TestDecodeAndView() {
    namespace p = flowsql::packet;
    auto* loader = flowsql::PluginLoader::Single();
    struct Owner {
        flowsql::PluginLoader* loader;
        ~Owner() {
            loader->StopAll();
            loader->Unload();
        }
    } owner{loader};
    const char* libraries[] = {FLOWSQL_NPI_PLUGIN_PATH};
    const char* options[] = {""};
    assert(loader->Load("", libraries, options, 1) == 0 && loader->StartAll() == 0);
    auto* protocol = static_cast<flowsql::IProtocol*>(loader->First(flowsql::IID_PROTOCOL));
    assert(protocol);
    flowsql::protocol::NpiPacketLayerDecoder decoder(protocol);
    p::PacketFamilyBatch owned;
    for (bool ipv6 : {false, true, false}) {
        auto bytes = std::make_shared<std::vector<uint8_t>>(14 + (ipv6 ? 40 : 20) + 8);
        auto& b = *bytes;
        auto u16 = [&](size_t i, uint16_t value) {
            b[i] = value >> 8;
            b[i + 1] = value;
        };
        u16(12, ipv6 ? 0x86dd : 0x0800);
        const size_t transport = 14 + (ipv6 ? 40 : 20);
        if (ipv6) {
            b[14] = 0x60;
            u16(18, 8);
            b[20] = 17;
            b[22] = 0x20;
            b[23] = 1;
            b[27] = 0xff;
            b[28] = 0x80;
            b[37] = 1;
            b[38] = 0x20;
            b[53] = 2;
        } else {
            b[14] = 0x45;
            u16(16, 28);
            b[23] = 17;
            b[26] = 1;
            b[27] = 2;
            b[28] = 3;
            b[29] = 4;
            b[30] = 192;
            b[33] = 1;
        }
        u16(transport, 1234);
        u16(transport + 2, 53);
        u16(transport + 4, 8);
        p::PacketFamilyRecord row;
        row.meta = {static_cast<int64_t>(owned.rows.size()),
                    static_cast<uint32_t>(b.size()),
                    static_cast<uint32_t>(b.size()),
                    1,
                    0,
                    owned.rows.size()};
        row.raw_data = {bytes, bytes->data(), static_cast<uint32_t>(bytes->size())};
        p::PacketView packet{row.meta, {bytes->data(), bytes->size()}};
        assert(decoder.Decode(packet, {p::kExtractMac | p::kExtractIp | p::kExtractPort}, owned, &row) == 0);
        assert(row.layer.status == p::LayerStatus::kDecoded &&
               row.layer.address_state == p::PacketAddressState::kAvailable);
        assert(row.layer.src_port == 1234 && row.layer.dst_port == 53);
        assert(row.address.family == (ipv6 ? p::AddressFamily::kIPv6 : p::AddressFamily::kIPv4));
        p::PacketFamilyBatch absent;
        p::PacketFamilyRecord missing;
        assert(decoder.Decode(packet, {p::kExtractNone}, absent, &missing) == 0);
        assert(missing.layer.address_state == p::PacketAddressState::kNotRequested && absent.ipv4.empty() &&
               absent.ipv6.empty());
        packet.bytes.size = 15;
        packet.meta.captured_len = 15;
        assert(decoder.Decode(packet, {p::kExtractIp}, absent, &missing) == 0);
        assert(missing.layer.address_state == p::PacketAddressState::kIncomplete && absent.ipv4.empty() &&
               absent.ipv6.empty());
        owned.rows.push_back(row);
    }
    std::shared_ptr<arrow::RecordBatch> encoded;
    assert(p::EncodePacketFamilyBatch(owned, &encoded) == p::PacketBatchError::kNone);
    std::unique_ptr<flowsql::npm::NpmPacketBatchView> view;
    assert(flowsql::npm::NpmPacketBatchView::Create(encoded, &view) == flowsql::npm::NpmPacketBatchError::kNone);
    assert(view->owned_address_bytes() == 2 * sizeof(p::PacketIpEndpoints4) + sizeof(p::PacketIpEndpoints6));
    encoded.reset();
    owned = {};
    p::PacketView packet;
    p::PacketFamilyLayerView layer;
    assert(view->Get(0, &packet, &layer) == flowsql::npm::NpmPacketBatchError::kNone);
    assert(layer.Get4() && layer.Get4()->src.ToUInt32() == 0x01020304 && !layer.Get6());
    const auto* first = layer.Get4();
    assert(view->Get(1, &packet, &layer) == flowsql::npm::NpmPacketBatchError::kNone);
    assert(layer.Get6() && layer.Get6()->src.bytes[5] == 0xff && layer.Get6()->src.bytes[6] == 0x80);
    assert(first->src.ToUInt32() == 0x01020304);
}

void TestTypedFilter() {
    namespace p = flowsql::packet;
    namespace filter = flowsql::channels::pcapfile;
    p::PacketFamilyLayerView layer;
    p::PacketIpEndpoints4 pair4{flowsql::network::IPv4Address::FromUInt32(0x01020304), {}};
    p::PacketIpEndpoints6 pair6{{{0x20, 1, 0xd, 0xb8, 0, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 1}}, {}};
    p::PcapFilterPlan plan;
    plan.root.kind = p::PacketFilterRuleKind::kIpAnyOf;
    p::PacketIpKey key;
    std::string error;
    assert(filter::CompilePacketIpKey("1.2.3.4", &key, &error) == 0 && key.ipv4_value == 0x01020304);
    plan.root.ip_keys.push_back(key);
    layer.status = p::LayerStatus::kDecoded;
    layer.address_state = p::PacketAddressState::kAvailable;
    layer.endpoints = &pair4;
    assert(filter::EvaluatePcapDecodedFilter(plan, {}, layer));
    layer.endpoints = &pair6;
    assert(!filter::EvaluatePcapDecodedFilter(plan, {}, layer));
    assert(filter::CompilePacketIpKey("2001:db8:ff:8000::1", &key, &error) == 0);
    plan.root.ip_keys = {key};
    assert(filter::EvaluatePcapDecodedFilter(plan, {}, layer));
    layer.endpoints = std::monostate{};
    layer.address_state = p::PacketAddressState::kNotRequested;
    assert(!filter::EvaluatePcapDecodedFilter(plan, {}, layer));
}

int main() {
    TestDecodeAndView();
    TestTypedFilter();
    namespace p = flowsql::packet;
    p::PacketFamilyBatch batch;
    batch.ipv4.push_back({flowsql::network::IPv4Address::FromUInt32(0x01020304), {}});
    batch.ipv6.push_back({{{0x20, 1, 0xd, 0xb8, 0, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 1}}, {}});
    auto bytes = std::make_shared<std::vector<uint8_t>>(3, 0x80);
    for (auto family :
         {p::AddressFamily::kIPv4, p::AddressFamily::kNone, p::AddressFamily::kIPv6, p::AddressFamily::kIPv4}) {
        p::PacketFamilyRecord row;
        row.meta = {static_cast<int64_t>(batch.rows.size()), 3, 3, 1, 0, batch.rows.size()};
        row.raw_data = {bytes, bytes->data(), 3};
        row.layer.address_state =
            family == p::AddressFamily::kNone ? p::PacketAddressState::kIncomplete : p::PacketAddressState::kAvailable;
        if (family != p::AddressFamily::kNone) row.address = {family, 0};
        batch.rows.push_back(row);
    }
    std::shared_ptr<arrow::RecordBatch> encoded;
    std::string error;
    assert(p::EncodePacketFamilyBatch(batch, &encoded, &error) == p::PacketBatchError::kNone);
    assert(p::ValidatePacketFamilyBatch(*encoded).ok());
    const auto& endpoints = static_cast<const arrow::DenseUnionArray&>(*encoded->GetColumnByName("ip_endpoints"));
    assert(endpoints.type_code(0) == 4 && endpoints.type_code(1) == 0 && endpoints.type_code(2) == 6);
    assert(endpoints.value_offset(3) == 1 && endpoints.field(1)->length() == 2 && endpoints.field(2)->length() == 1);
    std::shared_ptr<flowsql::FilterExpr> ast;
    std::shared_ptr<const flowsql::BoundFilterExpr> bound;
    std::shared_ptr<arrow::BooleanArray> mask;
    for (const auto& predicate :
         {"src_ip_v4 = 16909060", "src_ip_v4 BETWEEN 16909059 AND 16909061", "src_ip_v4 IN (16909060, 0)"}) {
        assert(flowsql::ParseFilterExpression(predicate, &ast, &error));
        assert(flowsql::BindFilterExpression(encoded->schema(), ast, &bound, &error) ==
               flowsql::FilterBindError::kNone);
        assert(flowsql::EvaluateFilterMask(encoded, bound, &mask, &error) == flowsql::FilterEvalError::kNone);
        assert(mask->Value(0) && mask->IsNull(1) && mask->IsNull(2) && mask->Value(3));
    }
    assert(flowsql::ParseFilterExpression("src_ip_v4 IS NULL OR (sequence >= 3 AND src_ip_family = 4)", &ast, &error));
    assert(flowsql::BindFilterExpression(encoded->schema(), ast, &bound, &error) == flowsql::FilterBindError::kNone);
    assert(flowsql::EvaluateFilterMask(encoded, bound, &mask, &error) == flowsql::FilterEvalError::kNone);
    assert(!mask->Value(0) && mask->Value(1) && mask->Value(2) && mask->Value(3));
    std::shared_ptr<arrow::RecordBatch> selected;
    assert(flowsql::FilterRecordBatch(encoded, bound, &selected, &error) == flowsql::FilterEvalError::kNone);
    assert(selected->num_rows() == 3 && p::ValidatePacketFamilyBatch(*selected).ok());
    assert(static_cast<const arrow::UInt64Array&>(*selected->GetColumnByName("sequence")).Value(0) == 1);
    std::string canonical;
    assert(flowsql::BuildCanonicalFilterPlan(encoded->schema(), bound, &canonical, &error) ==
           flowsql::FilterPlanError::kNone);
    assert(canonical.find("src_ip_v4") != std::string::npos);

    const auto& pair4 = static_cast<const arrow::StructArray&>(*endpoints.field(1));
    assert(static_cast<const arrow::UInt32Array&>(*pair4.field(0)).Value(0) == 0x01020304);
    bytes->assign(3, 0);
    assert(static_cast<const arrow::BinaryArray&>(*encoded->GetColumnByName("raw_data")).GetView(0)[0] == '\x80');
    batch.rows.erase(batch.rows.begin() + 1, batch.rows.begin() + 3);
    assert(p::EncodePacketFamilyBatch(batch, &encoded, &error) == p::PacketBatchError::kNone);
    const auto& only4 = static_cast<const arrow::DenseUnionArray&>(*encoded->GetColumnByName("ip_endpoints"));
    assert(only4.field(2)->length() == 0);
    assert(static_cast<const arrow::StructArray&>(*only4.field(2)).field(0)->data()->buffers[1]->size() == 0);
    batch.rows[0].address.index = 99;
    assert(p::EncodePacketFamilyBatch(batch, &encoded, &error) == p::PacketBatchError::kInvalidRecord && !encoded);
    batch.rows[0].address.index = 0;
    batch.rows[0].layer.address_state = p::PacketAddressState::kNotRequested;
    assert(p::EncodePacketFamilyBatch(batch, &encoded, &error) == p::PacketBatchError::kInvalidRecord && !encoded);
    int index = 0;
    assert(p::EncodePacketFamilyStream(
               [&](p::PacketFamilyBatch* scratch) {
                   if (index == 3) return ENOENT;
                   bytes->assign(3, static_cast<uint8_t>(index));
                   p::PacketFamilyRecord row;
                   row.meta = {index, 3, 3, 1, 0, static_cast<uint64_t>(index)};
                   row.raw_data = {bytes, bytes->data(), 3};
                   row.layer.address_state = p::PacketAddressState::kNotPresent;
                   scratch->rows.push_back(row);
                   ++index;
                   return 0;
               },
               4, arrow::default_memory_pool(), &encoded, &error) == p::PacketBatchError::kNone);
    const auto& raw = static_cast<const arrow::BinaryArray&>(*encoded->GetColumnByName("raw_data"));
    assert(raw.GetView(0)[0] == 0 && raw.GetView(1)[0] == 1 && raw.GetView(2)[0] == 2);
    std::puts("production packet family codec: mixed order, compact buffers, owning bytes and validation passed");
}
