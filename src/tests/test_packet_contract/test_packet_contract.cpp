// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <tests/common/packet_family_test_support.h>

#include <framework/core/packet_codec.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <plugins/npi/packet_decoder.h>
#include <common/loader.hpp>

#include <common/network/netbase.h>

#include <arrow/api.h>

#include <arpa/inet.h>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

namespace packet = flowsql::packet;

namespace {

class ContractBlockChannel final : public flowsql::IBlockStreamChannel {
 public:
    const char* Category() override { return "pcapfile"; }
    const char* Name() override { return "contract"; }
    const char* Type() override { return flowsql::ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    bool IsOpened() const override { return true; }
    int Flush() override { return 0; }
    flowsql::BlockPollEvent PollBlock(int) override { return {}; }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) override {
        if (!block || released_) return EINVAL;
        released_ = true;
        return 0;
    }
    void Cancel() override {}
    bool IsFinished() const override { return true; }
    bool released_ = false;
};

class ContractFactory final : public flowsql::IBlockStreamFactory {
 public:
    explicit ContractFactory(flowsql::IBlockStreamChannel* channel) : channel_(channel) {}
    flowsql::IBlockStreamChannel* Get(const char* type, const char* name) override {
        return type && name && std::string(type) == "pcapfile" && std::string(name) == "contract" ? channel_ : nullptr;
    }
    void List(std::function<void(const char*, const char*, flowsql::IBlockStreamChannel*)> callback) override {
        if (callback) callback("pcapfile", "contract", channel_);
    }
    flowsql::IBlockStreamChannel* channel_;
};

class ContractManager final : public flowsql::IBlockStreamManager {
 public:
    int AddChannel(const std::string&, const std::string&, const std::string&) override { return ENOTSUP; }
    int ModifyChannel(const std::string&, const std::string&, const std::string&) override { return ENOENT; }
    int RemoveChannel(const std::string&, const std::string&) override { return ENOENT; }
    void QueryChannels(
        std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> callback)
        override {
        if (callback) callback("pcapfile", "contract", "{}", "active");
    }
};

class ContractQuerier final : public flowsql::IQuerier {
 public:
    void Add(const flowsql::Guid& iid, void* value) { entries_[iid].push_back(value); }
    int Traverse(const flowsql::Guid& iid, fntraverse callback) override {
        auto it = entries_.find(iid);
        if (it == entries_.end()) return 0;
        for (void* value : it->second) {
            if (callback && callback(value) != 0) break;
        }
        return 0;
    }
    void* First(const flowsql::Guid& iid) override {
        auto it = entries_.find(iid);
        return it == entries_.end() || it->second.empty() ? nullptr : it->second.front();
    }

 private:
    std::map<flowsql::Guid, std::vector<void*>> entries_;
};

void TestBlockStreamPublicContracts() {
    static_assert(sizeof(flowsql::IID_BLOCK_STREAM_FACTORY.d4_) == 8);
    static_assert(sizeof(flowsql::IID_BLOCK_STREAM_MANAGER.d4_) == 8);

    ContractBlockChannel channel;
    ContractFactory factory(&channel);
    ContractManager manager;
    ContractQuerier loader;
    loader.Add(flowsql::IID_BLOCK_STREAM_FACTORY, &factory);
    loader.Add(flowsql::IID_BLOCK_STREAM_FACTORY, &factory);
    loader.Add(flowsql::IID_BLOCK_STREAM_MANAGER, &manager);

    int factory_count = 0;
    assert(loader.Traverse(flowsql::IID_BLOCK_STREAM_FACTORY, [&](void* value) {
        assert(value == &factory);
        ++factory_count;
        return 0;
    }) == 0);
    assert(factory_count == 2);
    assert(factory.Get("pcapfile", "contract") == &channel);
    int listed = 0;
    factory.List([&](const char* type, const char* name, flowsql::IBlockStreamChannel* value) {
        assert(std::string(type) == "pcapfile");
        assert(std::string(name) == "contract");
        assert(value == &channel);
        ++listed;
    });
    assert(listed == 1);
    assert(manager.AddChannel("unsupported", "x", "{}") == ENOTSUP);
    assert(manager.ModifyChannel("pcapfile", "missing", "{}") == ENOENT);
    auto batch =
        arrow::RecordBatch::Make(flowsql::packet::PacketSchema(), 0, std::vector<std::shared_ptr<arrow::Array>>{});
    assert(channel.ReleaseBlock(batch) == 0);
    assert(channel.ReleaseBlock(batch) != 0);
}

class MockLayerDecoder final : public packet::IPacketFamilyLayerDecoder {
 public:
    int Decode(const packet::PacketView& view, const packet::LayerDecodeOptions& options,
               packet::PacketLayerFacts* facts, packet::IPacketAddressSink&) override {
        ++decode_count;
        last_size = view.bytes.size;
        last_options = options;
        facts->status = packet::LayerStatus::kDecoded;
        facts->layer_count = 1;
        facts->layers[0] = {1, 0};
        return 0;
    }

    int decode_count = 0;
    size_t last_size = 0;
    packet::LayerDecodeOptions last_options;
};

class MockProtocolIdentifier final : public packet::IPacketFamilyProtocolIdentifier {
 public:
    packet::PacketProtocolInfo Identify(const packet::PacketView&, const flowsql::packet::PacketLayerFacts&) override {
        ++identify_count;
        return {packet::ProtocolStatus::kUnknown, 0, 0};
    }

    int identify_count = 0;
};

class MockNpiProtocol final : public flowsql::IProtocol {
 public:
    void Concurrency(int32_t) override {}

    flowsql::protocol::Protocol Identify(int32_t, const uint8_t*, int32_t,
                                         const flowsql::protocol::Layers* input) override {
        ++identify_calls;
        identify_top = input->Top();
        identify_layercount = input->layercount;
        identify_payload = input->payload;
        return identify_result;
    }

    int32_t Layer(int32_t, const uint8_t*, int32_t, flowsql::protocol::Layers* output) override {
        ++layer_calls;
        *output = layers;
        return output->layercount;
    }

    flowsql::protocol::IDictionary* Dictionary() override { return nullptr; }

    flowsql::protocol::Layers layers{};
    int layer_calls = 0;
    int identify_calls = 0;
    flowsql::eLayer identify_top = flowsql::eLayer::NONE;
    uint16_t identify_layercount = 0;
    uint16_t identify_payload = 0;
    flowsql::protocol::Protocol identify_result{7, 8};
};

void TestAddressReuseAndVariant() {
    static_assert(std::is_same_v<packet::IPv4Address, ::IPv4Address>);
    static_assert(std::is_same_v<packet::IPv6Address, ::IPv6Address>);
    static_assert(std::is_same_v<decltype(packet::MacAddress::value), ::EtherAdderss>);

    ::EtherAdderss mac;
    mac.bytes[0] = 0x00;
    mac.bytes[1] = 0x11;
    mac.bytes[2] = 0x22;
    mac.bytes[3] = 0x33;
    mac.bytes[4] = 0x44;
    mac.bytes[5] = 0x55;

    packet::MacAddress packet_mac;
    packet_mac.value = mac;
    packet_mac.valid = 1;
    assert(packet_mac.valid == 1);
    assert(packet_mac.value == mac);

    packet::IPv4Address ipv4;
    ipv4.addr = inet_addr("192.0.2.1");
    packet::IpAddress address = ipv4;
    assert(std::holds_alternative<packet::IPv4Address>(address));
    assert(std::get<packet::IPv4Address>(address).addr == inet_addr("192.0.2.1"));

    packet::IPv6Address ipv6;
    for (size_t index = 0; index < sizeof(ipv6.bytes); ++index) {
        ipv6.bytes[index] = static_cast<uint8_t>(index);
    }
    address = ipv6;
    assert(std::holds_alternative<packet::IPv6Address>(address));
    assert(std::get<packet::IPv6Address>(address).bytes[15] == 15);

    address = std::monostate{};
    assert(std::holds_alternative<std::monostate>(address));
}

void TestContractDefaultsAndInterfaces() {
    static_assert(packet::kMaxLayerDepth == 15);
    static_assert(packet::kNoLayerIndex == 0xff);

    packet::LayerDecodeOptions options;
    assert(options.field_mask == packet::kExtractNone);
    assert(options.endpoint_scope == packet::EndpointScope::kInnermost);

    std::array<uint8_t, 4> bytes{1, 2, 3, 4};
    packet::PacketView view;
    view.meta.captured_len = bytes.size();
    view.bytes = flowsql::Span<const uint8_t>(bytes.data(), bytes.size());

    MockLayerDecoder decoder;
    auto layer = flowsql::testing::DecodeLayer(decoder, view, options);
    assert(decoder.decode_count == 1);
    assert(decoder.last_size == bytes.size());
    assert(layer.status == packet::LayerStatus::kDecoded);
    assert(layer.layer_count == 1);
    assert(layer.network_layer_index == packet::kNoLayerIndex);

    MockProtocolIdentifier identifier;
    auto protocol = identifier.Identify(view, layer);
    assert(identifier.identify_count == 1);
    assert(protocol.status == packet::ProtocolStatus::kUnknown);
    assert(protocol.id == 0);
    assert(protocol.sub_id == 0);
}

void TestPacketEnvelopeAndSchema() {
    std::array<uint8_t, 4> bytes{1, 2, 3, 4};
    packet::PacketView view;
    view.meta.captured_len = bytes.size();
    view.meta.wire_len = bytes.size() + 2;
    view.bytes = flowsql::Span<const uint8_t>(bytes.data(), bytes.size());

    assert(packet::ValidatePacketView(view) == packet::PacketEnvelopeError::kNone);
    packet::PacketBytes owned;
    assert(packet::CopyPacketBytes(view, &owned) == packet::PacketEnvelopeError::kNone);
    assert(owned.owner != nullptr);
    assert(owned.data != nullptr);
    assert(owned.size == bytes.size());
    assert(std::memcmp(owned.data, bytes.data(), bytes.size()) == 0);

    auto temporary = std::make_unique<uint8_t[]>(3);
    temporary[0] = 9;
    temporary[1] = 8;
    temporary[2] = 7;
    packet::PacketView temporary_view;
    temporary_view.meta.captured_len = 3;
    temporary_view.bytes = flowsql::Span<const uint8_t>(temporary.get(), 3);
    packet::PacketBytes temporary_owned;
    assert(packet::CopyPacketBytes(temporary_view, &temporary_owned) == packet::PacketEnvelopeError::kNone);
    temporary.reset();
    assert(temporary_owned.data[0] == 9);
    assert(temporary_owned.data[1] == 8);
    assert(temporary_owned.data[2] == 7);

    view.meta.wire_len = 1;
    assert(packet::ValidatePacketView(view) == packet::PacketEnvelopeError::kWireLengthTooSmall);
    view.meta.wire_len = bytes.size();
    view.meta.captured_len = bytes.size() + 1;
    assert(packet::ValidatePacketView(view) == packet::PacketEnvelopeError::kLengthMismatch);
    view.meta.captured_len = bytes.size();
    view.bytes.data = nullptr;
    assert(packet::ValidatePacketView(view) == packet::PacketEnvelopeError::kNullData);
    assert(packet::CopyPacketBytes(view, nullptr) == packet::PacketEnvelopeError::kNullOutput);

    packet::PacketView empty;
    packet::PacketBytes empty_owned;
    assert(packet::CopyPacketBytes(empty, &empty_owned) == packet::PacketEnvelopeError::kNone);
    assert(empty_owned.owner == nullptr);
    assert(empty_owned.data == nullptr);
    assert(empty_owned.size == 0);

    auto schema = packet::PacketSchema();
    assert(schema != nullptr);
    assert(schema->num_fields() == 26);
    assert(schema->metadata()->Get("flowsql.entity").ValueOrDie() == "packet");
    assert(schema->metadata()->Get("flowsql.schema_version").ValueOrDie() == "2");
    assert(schema->metadata()->Get("flowsql.timestamp_unit").ValueOrDie() == "ns");
    assert(schema->GetFieldByName("src_mac")->nullable());
    assert(packet::PacketFamilyFilterSchema()->GetFieldByName("src_ip_v4")->type()->id() == arrow::Type::UINT32);
    assert(packet::PacketFamilyFilterSchema()->GetFieldByName("src_ip_v6")->type()->id() ==
           arrow::Type::FIXED_SIZE_BINARY);
    assert(schema->GetFieldByName("layer_ids")->type()->id() == arrow::Type::FIXED_SIZE_LIST);
    assert(schema->GetFieldByName("layer_offsets")->type()->id() == arrow::Type::FIXED_SIZE_LIST);
    assert(schema->GetFieldByName("ports_valid")->type()->id() == arrow::Type::BOOL);
}

void TestPacketRecordBatchEncoding() {
    std::array<uint8_t, 20> bytes{0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    packet::PacketView view;
    view.meta.captured_len = bytes.size();
    view.meta.wire_len = bytes.size();
    view.bytes = flowsql::Span<const uint8_t>(bytes.data(), bytes.size());

    flowsql::testing::PacketRecord record;
    record.meta.timestamp_ns = 123456789;
    record.meta.captured_len = bytes.size();
    record.meta.wire_len = bytes.size();
    record.meta.link_type = 1;
    record.meta.source_id = 2;
    record.meta.sequence = 3;
    assert(packet::CopyPacketBytes(view, &record.raw_data) == packet::PacketEnvelopeError::kNone);

    record.layer.status = packet::LayerStatus::kDecoded;
    record.layer.layer_count = 2;
    record.layer.layers[0] = {2, 0};
    record.layer.layers[1] = {17, 14};
    record.layer.endpoint_scope = packet::EndpointScope::kInnermost;
    record.layer.network_layer_index = 1;
    record.layer.transport_layer_index = packet::kNoLayerIndex;
    record.layer.payload_offset = 0;
    record.layer.src_mac_valid = 1;
    record.layer.src_mac.bytes[0] = 0x10;
    record.layer.src_mac.bytes[5] = 0x15;
    record.layer.SetEndpoints(flowsql::network::IPv4Address::FromUInt32(0xc000020a),
                              flowsql::network::IPv4Address::FromUInt32(0xc6336402));
    record.layer.transport_protocol = 6;
    record.layer.src_port = 1234;
    record.layer.dst_port = 443;
    record.layer.ports_valid = 1;
    record.protocol.status = packet::ProtocolStatus::kIdentified;
    record.protocol.id = 7;
    record.protocol.sub_id = 8;

    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    const auto encode_result = flowsql::testing::EncodePacketBatch({record}, &batch, &error);
    if (encode_result != packet::PacketBatchError::kNone) {
        std::fprintf(stderr, "EncodePacketBatch failed: %d %s\n", static_cast<int>(encode_result), error.c_str());
    }
    assert(encode_result == packet::PacketBatchError::kNone);
    assert(error.empty());
    assert(batch != nullptr);
    assert(batch->num_rows() == 1);
    assert(batch->num_columns() == 26);

    auto raw = std::static_pointer_cast<arrow::BinaryArray>(batch->GetColumnByName("raw_data"));
    assert(raw->GetView(0) == std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    auto encoded_src_mac = std::static_pointer_cast<arrow::FixedSizeBinaryArray>(batch->GetColumnByName("src_mac"));
    assert(!encoded_src_mac->IsNull(0));
    assert(encoded_src_mac->GetValue(0)[0] == 0x10);
    assert(encoded_src_mac->GetValue(0)[5] == 0x15);

    const auto& endpoints = static_cast<const arrow::DenseUnionArray&>(*batch->GetColumnByName("ip_endpoints"));
    assert(endpoints.type_code(0) == 4 && endpoints.field(2)->length() == 0);
    const auto& pair4 = static_cast<const arrow::StructArray&>(*endpoints.field(1));
    assert(static_cast<const arrow::UInt32Array&>(*pair4.field(0)).Value(0) == 0xc000020a);
    assert(static_cast<const arrow::UInt32Array&>(*pair4.field(1)).Value(0) == 0xc6336402);

    auto encoded_layer_ids = std::static_pointer_cast<arrow::FixedSizeListArray>(batch->GetColumnByName("layer_ids"));
    auto layer_ids_values = std::static_pointer_cast<arrow::UInt16Array>(encoded_layer_ids->values());
    assert(layer_ids_values->Value(0) == 2);
    assert(layer_ids_values->Value(1) == 17);
    auto encoded_layer_offsets =
        std::static_pointer_cast<arrow::FixedSizeListArray>(batch->GetColumnByName("layer_offsets"));
    auto layer_offsets_values = std::static_pointer_cast<arrow::UInt32Array>(encoded_layer_offsets->values());
    assert(layer_offsets_values->Value(0) == 0);
    assert(layer_offsets_values->Value(1) == 14);

    auto encoded_protocol_id = std::static_pointer_cast<arrow::UInt16Array>(batch->GetColumnByName("protocol_id"));
    assert(encoded_protocol_id->Value(0) == 7);
    auto encoded_protocol_sub_id =
        std::static_pointer_cast<arrow::UInt16Array>(batch->GetColumnByName("protocol_sub_id"));
    assert(encoded_protocol_sub_id->Value(0) == 8);

    flowsql::testing::PacketRecord invalid = record;
    invalid.raw_data.size = 1;
    batch.reset();
    assert(flowsql::testing::EncodePacketBatch({invalid}, &batch, &error) == packet::PacketBatchError::kInvalidRecord);
    assert(batch == nullptr);
    assert(!error.empty());

    flowsql::testing::PacketRecord mixed_scope = record;
    mixed_scope.layer.endpoint_scope = packet::EndpointScope::kOutermost;
    assert(flowsql::testing::EncodePacketBatch({record, mixed_scope}, &batch, &error) ==
           packet::PacketBatchError::kInvalidRecord);
    assert(batch == nullptr);
    assert(!error.empty());

    flowsql::testing::PacketRecord missing_owner = record;
    missing_owner.raw_data.owner.reset();
    assert(flowsql::testing::EncodePacketBatch({missing_owner}, &batch, &error) ==
           packet::PacketBatchError::kInvalidRecord);
    assert(batch == nullptr);
    assert(!error.empty());

    flowsql::testing::PacketRecord invalid_scope = record;
    invalid_scope.layer.endpoint_scope = static_cast<packet::EndpointScope>(9);
    assert(flowsql::testing::EncodePacketBatch({invalid_scope}, &batch, &error) ==
           packet::PacketBatchError::kInvalidRecord);
    assert(batch == nullptr);
    assert(!error.empty());

    flowsql::testing::PacketRecord missing;
    assert(flowsql::testing::EncodePacketBatch({missing}, &batch, &error) == packet::PacketBatchError::kNone);
    const auto& missing_endpoints = static_cast<const arrow::DenseUnionArray&>(*batch->GetColumnByName("ip_endpoints"));
    assert(missing_endpoints.type_code(0) == 0 && missing_endpoints.field(1)->length() == 0 &&
           missing_endpoints.field(2)->length() == 0);
    auto missing_protocol_id = std::static_pointer_cast<arrow::UInt16Array>(batch->GetColumnByName("protocol_id"));
    assert(missing_protocol_id->IsNull(0));
}

template <size_t N>
void PacketU16(std::array<uint8_t, N>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = value >> 8;
    bytes[offset + 1] = value;
}
template <size_t N>
void PacketIPv4(std::array<uint8_t, N>& bytes, size_t offset, uint8_t next, const char* source,
                const char* destination) {
    bytes[offset] = 0x45;
    bytes[offset + 9] = next;
    const auto s = inet_addr(source), d = inet_addr(destination);
    std::memcpy(bytes.data() + offset + 12, &s, sizeof(s));
    std::memcpy(bytes.data() + offset + 16, &d, sizeof(d));
}
std::array<uint8_t, 104> MakeVxlanPacket() {
    std::array<uint8_t, 104> bytes{};
    bytes[6] = 0x01;
    bytes[0] = 0x02;
    PacketU16(bytes, 12, flowsql::ethernet::eNext::IPv4);
    PacketIPv4(bytes, 14, flowsql::ipv4::eNext::UDP, "198.51.100.1", "198.51.100.2");
    PacketU16(bytes, 34, 40000);
    PacketU16(bytes, 36, 4789);
    PacketU16(bytes, 38, 70);
    bytes[42] = 0x08;
    bytes[56] = 0x11;
    bytes[50] = 0x22;
    PacketU16(bytes, 62, flowsql::ethernet::eNext::IPv4);
    PacketIPv4(bytes, 64, flowsql::ipv4::eNext::TCP, "192.0.2.10", "192.0.2.20");
    PacketU16(bytes, 84, 12345);
    PacketU16(bytes, 86, 443);
    bytes[96] = 0x50;
    return bytes;
}
std::array<uint8_t, 78> MakeGrePacket() {
    std::array<uint8_t, 78> bytes{};
    bytes[6] = 0x31;
    bytes[0] = 0x32;
    PacketU16(bytes, 12, flowsql::ethernet::eNext::IPv4);
    PacketIPv4(bytes, 14, flowsql::ipv4::eNext::GRE, "198.51.100.10", "198.51.100.20");
    PacketU16(bytes, 34 + offsetof(flowsql::GreHeader, proto), flowsql::ethernet::eNext::IPv4);
    PacketIPv4(bytes, 38, flowsql::ipv4::eNext::TCP, "192.0.2.30", "192.0.2.40");
    PacketU16(bytes, 58, 2345);
    PacketU16(bytes, 60, 8443);
    bytes[70] = 0x50;
    return bytes;
}
std::array<uint8_t, 74> MakeIpInIpPacket() {
    std::array<uint8_t, 74> bytes{};
    bytes[6] = 0x41;
    bytes[0] = 0x42;
    PacketU16(bytes, 12, flowsql::ethernet::eNext::IPv4);
    PacketIPv4(bytes, 14, flowsql::ipv4::eNext::IPv4, "198.51.100.30", "198.51.100.40");
    PacketIPv4(bytes, 34, flowsql::ipv4::eNext::TCP, "192.0.2.50", "192.0.2.60");
    PacketU16(bytes, 54, 3456);
    PacketU16(bytes, 56, 9443);
    bytes[66] = 0x50;
    return bytes;
}

void SetVxlanLayers(flowsql::protocol::Layers* layers) {
    layers->layercount = 7;
    layers->payload = 104;
    layers->layers[0] = {0, flowsql::eLayer::ETHERNET};
    layers->layers[1] = {14, flowsql::eLayer::IPv4};
    layers->layers[2] = {34, flowsql::eLayer::UDP};
    layers->layers[3] = {42, flowsql::eLayer::VXLAN};
    layers->layers[4] = {50, flowsql::eLayer::ETHERNET};
    layers->layers[5] = {64, flowsql::eLayer::IPv4};
    layers->layers[6] = {84, flowsql::eLayer::TCP};
}

void SetGreLayers(flowsql::protocol::Layers* layers) {
    layers->layercount = 5;
    layers->payload = 78;
    layers->layers[0] = {0, flowsql::eLayer::ETHERNET};
    layers->layers[1] = {14, flowsql::eLayer::IPv4};
    layers->layers[2] = {34, flowsql::eLayer::GRE};
    layers->layers[3] = {38, flowsql::eLayer::IPv4};
    layers->layers[4] = {58, flowsql::eLayer::TCP};
}

void SetIpInIpLayers(flowsql::protocol::Layers* layers) {
    layers->layercount = 4;
    layers->payload = 74;
    layers->layers[0] = {0, flowsql::eLayer::ETHERNET};
    layers->layers[1] = {14, flowsql::eLayer::IPv4};
    layers->layers[2] = {34, flowsql::eLayer::IPv4};
    layers->layers[3] = {54, flowsql::eLayer::TCP};
}

void TestNpiLayerDecoder() {
    auto bytes = MakeVxlanPacket();
    packet::PacketView view;
    view.meta.captured_len = bytes.size();
    view.meta.wire_len = bytes.size();
    view.meta.link_type = 1;
    view.bytes = flowsql::Span<const uint8_t>(bytes.data(), bytes.size());

    MockNpiProtocol protocol;
    SetVxlanLayers(&protocol.layers);
    flowsql::protocol::NpiPacketLayerDecoder decoder(&protocol);
    packet::LayerDecodeOptions options;
    options.field_mask = packet::kExtractMac | packet::kExtractIp | packet::kExtractPort;

    auto inner = flowsql::testing::DecodeLayer(decoder, view, options);
    assert(protocol.layer_calls == 1);
    assert(inner.status == packet::LayerStatus::kDecoded);
    assert(inner.layer_count == 7);
    assert(inner.network_layer_index == 5);
    assert(inner.transport_layer_index == 6);
    assert(inner.Get4()->src.ToUInt32() == ntohl(inet_addr("192.0.2.10")));
    assert(inner.src_mac_valid == 1);
    assert(inner.src_mac.bytes[0] == 0x11);
    assert(inner.transport_protocol == flowsql::ipv4::eNext::TCP);
    assert(inner.src_port == 12345);
    assert(inner.dst_port == 443);
    assert(inner.ports_valid == 1);
    assert(inner.payload_offset == bytes.size());

    options.endpoint_scope = packet::EndpointScope::kOutermost;
    auto outer = flowsql::testing::DecodeLayer(decoder, view, options);
    assert(protocol.layer_calls == 2);
    assert(outer.status == packet::LayerStatus::kDecoded);
    assert(outer.network_layer_index == 1);
    assert(outer.transport_layer_index == 2);
    assert(outer.Get4()->src.ToUInt32() == ntohl(inet_addr("198.51.100.1")));
    assert(outer.src_mac.bytes[0] == 0x01);
    assert(outer.transport_protocol == flowsql::ipv4::eNext::UDP);
    assert(outer.src_port == 40000);
    assert(outer.dst_port == 4789);
    assert(outer.payload_offset == 42);
    for (uint8_t index = 0; index < inner.layer_count; ++index) {
        assert(inner.layers[index].kind == outer.layers[index].kind);
        assert(inner.layers[index].offset == outer.layers[index].offset);
    }

    auto truncated_view = view;
    truncated_view.meta.captured_len = 20;
    truncated_view.bytes = flowsql::Span<const uint8_t>(bytes.data(), 20);
    auto truncated = flowsql::testing::DecodeLayer(decoder, truncated_view, options);
    assert(truncated.status == packet::LayerStatus::kTruncated);

    auto unsupported_view = view;
    unsupported_view.meta.link_type = 113;
    auto unsupported = flowsql::testing::DecodeLayer(decoder, unsupported_view, options);
    assert(unsupported.status == packet::LayerStatus::kUnsupportedLinkType);
    assert(protocol.layer_calls == 3);

    options.endpoint_scope = static_cast<packet::EndpointScope>(9);
    auto invalid_scope = flowsql::testing::DecodeLayer(decoder, view, options);
    assert(invalid_scope.status == packet::LayerStatus::kMalformed);
    assert(protocol.layer_calls == 3);
}

void TestNpiTunnelEndpointSelection() {
    auto gre_bytes = MakeGrePacket();
    packet::PacketView gre_view;
    gre_view.meta.captured_len = gre_bytes.size();
    gre_view.meta.wire_len = gre_bytes.size();
    gre_view.meta.link_type = 1;
    gre_view.bytes = flowsql::Span<const uint8_t>(gre_bytes.data(), gre_bytes.size());

    MockNpiProtocol gre_protocol;
    SetGreLayers(&gre_protocol.layers);
    flowsql::protocol::NpiPacketLayerDecoder gre_decoder(&gre_protocol);
    packet::LayerDecodeOptions options;
    options.field_mask = packet::kExtractMac | packet::kExtractIp | packet::kExtractPort;

    auto inner = flowsql::testing::DecodeLayer(gre_decoder, gre_view, options);
    assert(inner.status == packet::LayerStatus::kDecoded);
    assert(inner.network_layer_index == 3);
    assert(inner.transport_layer_index == 4);
    assert(inner.Get4()->src.ToUInt32() == ntohl(inet_addr("192.0.2.30")));
    assert(inner.src_mac_valid == 0);
    assert(inner.ports_valid == 1);

    options.endpoint_scope = packet::EndpointScope::kOutermost;
    auto outer = flowsql::testing::DecodeLayer(gre_decoder, gre_view, options);
    assert(outer.network_layer_index == 1);
    assert(outer.transport_layer_index == 2);
    assert(outer.src_mac_valid == 1);
    assert(outer.Get4()->src.ToUInt32() == ntohl(inet_addr("198.51.100.10")));
    assert(outer.ports_valid == 0);
    assert(outer.payload_offset == 38);

    auto ipip_bytes = MakeIpInIpPacket();
    packet::PacketView ipip_view;
    ipip_view.meta.captured_len = ipip_bytes.size();
    ipip_view.meta.wire_len = ipip_bytes.size();
    ipip_view.meta.link_type = 1;
    ipip_view.bytes = flowsql::Span<const uint8_t>(ipip_bytes.data(), ipip_bytes.size());
    MockNpiProtocol ipip_protocol;
    SetIpInIpLayers(&ipip_protocol.layers);
    flowsql::protocol::NpiPacketLayerDecoder ipip_decoder(&ipip_protocol);
    options.endpoint_scope = packet::EndpointScope::kInnermost;
    auto ipip_inner = flowsql::testing::DecodeLayer(ipip_decoder, ipip_view, options);
    assert(ipip_inner.network_layer_index == 2);
    assert(ipip_inner.transport_layer_index == 3);
    assert(ipip_inner.src_mac_valid == 0);
    assert(ipip_inner.ports_valid == 1);
    auto ipip_outer = flowsql::testing::DecodeLayer(
        ipip_decoder, ipip_view,
        packet::LayerDecodeOptions{packet::kExtractMac | packet::kExtractIp | packet::kExtractPort,
                                   packet::EndpointScope::kOutermost});
    assert(ipip_outer.network_layer_index == 1);
    assert(ipip_outer.transport_layer_index == packet::kNoLayerIndex);
    assert(ipip_outer.src_mac_valid == 1);
    assert(ipip_outer.ports_valid == 0);
    assert(ipip_outer.payload_offset == 34);

    flowsql::protocol::NpiPacketProtocolIdentifier ipip_identifier(&ipip_protocol);
    auto ipip_protocol_result = ipip_identifier.Identify(ipip_view, ipip_outer);
    assert(ipip_protocol_result.status == packet::ProtocolStatus::kIdentified);
    assert(ipip_protocol.identify_top == flowsql::eLayer::IPv4);
    assert(ipip_protocol.identify_layercount == 2);
    assert(ipip_protocol.identify_payload == 34);

    auto malformed_bytes = gre_bytes;
    malformed_bytes[38] = (malformed_bytes[38] & 0xf0) | 4;
    gre_view.bytes = flowsql::Span<const uint8_t>(malformed_bytes.data(), malformed_bytes.size());
    auto malformed = flowsql::testing::DecodeLayer(
        gre_decoder, gre_view,
        packet::LayerDecodeOptions{packet::kExtractMac | packet::kExtractIp | packet::kExtractPort,
                                   packet::EndpointScope::kInnermost});
    assert(malformed.status == packet::LayerStatus::kMalformed);
    gre_view.meta.wire_len = gre_bytes.size() + 1;
    auto malformed_truncated = flowsql::testing::DecodeLayer(
        gre_decoder, gre_view,
        packet::LayerDecodeOptions{packet::kExtractMac | packet::kExtractIp | packet::kExtractPort,
                                   packet::EndpointScope::kInnermost});
    assert(malformed_truncated.status == packet::LayerStatus::kMalformed);
    gre_view.meta.wire_len = gre_bytes.size();

    std::array<uint8_t, sizeof(flowsql::EthernetHeader)> ethernet_bytes{};
    packet::PacketView ethernet_view;
    ethernet_view.meta.captured_len = ethernet_bytes.size();
    ethernet_view.meta.wire_len = ethernet_bytes.size();
    ethernet_view.meta.link_type = 1;
    ethernet_view.bytes = flowsql::Span<const uint8_t>(ethernet_bytes.data(), ethernet_bytes.size());
    MockNpiProtocol ethernet_protocol;
    ethernet_protocol.layers.layercount = 1;
    ethernet_protocol.layers.payload = sizeof(flowsql::EthernetHeader);
    ethernet_protocol.layers.layers[0] = {0, flowsql::eLayer::ETHERNET};
    flowsql::protocol::NpiPacketLayerDecoder ethernet_decoder(&ethernet_protocol);
    auto ethernet = flowsql::testing::DecodeLayer(ethernet_decoder, ethernet_view, options);
    assert(ethernet.status == packet::LayerStatus::kDecoded);
    assert(ethernet.network_layer_index == packet::kNoLayerIndex);
    assert(ethernet.src_mac_valid == 1);
}

void TestNpiProtocolIdentifier() {
    auto bytes = MakeVxlanPacket();
    packet::PacketView view;
    view.meta.captured_len = bytes.size();
    view.meta.wire_len = bytes.size();
    view.meta.link_type = 1;
    view.bytes = flowsql::Span<const uint8_t>(bytes.data(), bytes.size());

    MockNpiProtocol protocol;
    SetVxlanLayers(&protocol.layers);
    flowsql::protocol::NpiPacketLayerDecoder layer_decoder(&protocol);
    packet::LayerDecodeOptions options;
    options.field_mask = packet::kExtractMac | packet::kExtractIp | packet::kExtractPort;
    auto layer = flowsql::testing::DecodeLayer(layer_decoder, view, options);
    assert(protocol.layer_calls == 1);

    flowsql::protocol::NpiPacketProtocolIdentifier identifier(&protocol);
    auto identified = identifier.Identify(view, layer);
    assert(identified.status == packet::ProtocolStatus::kIdentified);
    assert(identified.id == 7);
    assert(identified.sub_id == 8);
    assert(protocol.identify_calls == 1);
    assert(protocol.layer_calls == 1);
    assert(protocol.identify_top == flowsql::eLayer::TCP);
    assert(protocol.identify_layercount == 7);
    assert(protocol.identify_payload == bytes.size());
    assert(layer.layer_count == 7);
    assert(layer.layers[6].kind == static_cast<uint16_t>(flowsql::eLayer::TCP));

    auto outer = layer;
    outer.transport_layer_index = 2;
    outer.payload_offset = 42;
    auto outer_identified = identifier.Identify(view, outer);
    assert(outer_identified.status == packet::ProtocolStatus::kIdentified);
    assert(protocol.identify_calls == 2);
    assert(protocol.layer_calls == 1);
    assert(protocol.identify_top == flowsql::eLayer::UDP);
    assert(protocol.identify_layercount == 3);
    assert(protocol.identify_payload == 42);

    auto no_transport = layer;
    no_transport.transport_layer_index = packet::kNoLayerIndex;
    auto no_transport_identified = identifier.Identify(view, no_transport);
    assert(no_transport_identified.status == packet::ProtocolStatus::kIdentified);
    assert(protocol.identify_calls == 3);
    assert(protocol.identify_top == flowsql::eLayer::TCP);
    assert(protocol.identify_layercount == 7);

    auto invalid_offset = layer;
    invalid_offset.layers[0].offset = static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1;
    auto unknown_offset = identifier.Identify(view, invalid_offset);
    assert(unknown_offset.status == packet::ProtocolStatus::kUnknown);
    assert(protocol.identify_calls == 3);

    auto invalid_payload = layer;
    invalid_payload.payload_offset = static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1;
    auto unknown_payload = identifier.Identify(view, invalid_payload);
    assert(unknown_payload.status == packet::ProtocolStatus::kUnknown);
    assert(protocol.identify_calls == 3);

    auto invalid_index = layer;
    invalid_index.transport_layer_index = invalid_index.layer_count;
    auto unknown_index = identifier.Identify(view, invalid_index);
    assert(unknown_index.status == packet::ProtocolStatus::kUnknown);
    assert(protocol.identify_calls == 3);

    auto malformed_layer = layer;
    malformed_layer.status = packet::LayerStatus::kMalformed;
    auto unknown_malformed = identifier.Identify(view, malformed_layer);
    assert(unknown_malformed.status == packet::ProtocolStatus::kUnknown);
    assert(protocol.identify_calls == 3);

    protocol.identify_result = {};
    auto unknown_result = identifier.Identify(view, layer);
    assert(unknown_result.status == packet::ProtocolStatus::kUnknown);
    assert(unknown_result.id == 0);
    assert(unknown_result.sub_id == 0);
    assert(protocol.identify_calls == 4);
    assert(protocol.layer_calls == 1);
}

}  // namespace

int main() {
    TestAddressReuseAndVariant();
    TestBlockStreamPublicContracts();
    TestContractDefaultsAndInterfaces();
    TestPacketEnvelopeAndSchema();
    TestPacketRecordBatchEncoding();
    TestNpiLayerDecoder();
    TestNpiTunnelEndpointSelection();
    TestNpiProtocolIdentifier();
    std::puts("[PASS] packet contract interface and address layout");
    return 0;
}
