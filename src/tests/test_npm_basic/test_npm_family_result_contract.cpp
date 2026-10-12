// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/npm_family_result_contract.h>

#include <arrow/api.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <type_traits>

namespace {
namespace npm = flowsql::npm;
namespace packet = flowsql::packet;
namespace network = flowsql::network;
static_assert(std::is_same_v<decltype(npm::NpmBasicResult4::a_ip), network::IPv4Address>);
static_assert(std::is_same_v<decltype(npm::NpmBasicResult6::a_ip), network::IPv6Address>);
static_assert(std::is_same_v<decltype(npm::NpmSessionResult4::b_ip), network::IPv4Address>);
static_assert(std::is_same_v<decltype(npm::NpmSessionResult6::b_ip), network::IPv6Address>);
static_assert(std::is_same_v<decltype(npm::NpmDnsResult4::a_ip), network::IPv4Address>);
static_assert(std::is_same_v<decltype(npm::NpmDnsResult6::a_ip), network::IPv6Address>);
static_assert(std::is_same_v<decltype(npm::NpmHttp1Result4::a_ip), network::IPv4Address>);
static_assert(std::is_same_v<decltype(npm::NpmHttp1Result6::a_ip), network::IPv6Address>);
static_assert(std::is_same_v<decltype(npm::NpmTlsResult4::a_ip), network::IPv4Address>);
static_assert(std::is_same_v<decltype(npm::NpmTlsResult6::a_ip), network::IPv6Address>);
static_assert(std::is_same_v<decltype(npm::NpmIcmpEvent4::quoted_src_ip), std::optional<network::IPv4Address>>);
static_assert(std::is_same_v<decltype(npm::NpmIcmpEvent6::quoted_src_ip), std::optional<network::IPv6Address>>);
static_assert(std::is_abstract_v<npm::INpmFamilyResultWriter>);
static_assert(std::is_abstract_v<npm::INpmFamilyAnalysisModule>);
static_assert(std::is_abstract_v<npm::INpmFamilyProtocolModule>);
static_assert(std::is_abstract_v<npm::INpmFamilyIcmpActiveSessionLookup>);
static_assert(sizeof(npm::NpmFamilySessionHandle) == 16);

void Check(const arrow::Status& status) {
    if (!status.ok()) std::fprintf(stderr, "%s\n", status.ToString().c_str());
    assert(status.ok());
}
template <typename T>
T Get(arrow::Result<T> result) {
    Check(result.status());
    return std::move(result).ValueOrDie();
}

// Independent row fixtures use the actual family encodings; protocol address text never enters the batch.
std::shared_ptr<arrow::RecordBatch> Fixture(const npm::NpmEntityDescriptorV1& entity) {
    arrow::ArrayVector columns;
    for (const auto& field : entity.schema->fields()) {
        std::shared_ptr<arrow::Array> array;
        auto builder = Get(arrow::MakeBuilder(field->type()));
        if (field->metadata() && field->metadata()->Contains("flowsql.semantic")) {
            if (field->type()->id() == arrow::Type::UINT32) {
                auto& addresses = static_cast<arrow::UInt32Builder&>(*builder);
                Check(addresses.Append(0xffffffffu));
                Check(addresses.Append(0x01020304u));
            } else {
                auto& addresses = static_cast<arrow::FixedSizeBinaryBuilder&>(*builder);
                const uint8_t bytes[16] = {0x20, 1, 0x0d, 0xb8, 0, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 1};
                Check(addresses.Append(bytes));
                Check(addresses.Append(bytes));
            }
        } else if (field->name() == entity.identity_column || field->name() == "revision") {
            Check(static_cast<arrow::UInt64Builder&>(*builder).AppendValues({1, 1}));
        } else if (field->name() == "is_final") {
            Check(static_cast<arrow::BooleanBuilder&>(*builder).AppendValues(std::vector<uint8_t>{1, 1}));
        } else if (field->nullable()) {
            Check(builder->AppendNulls(2));
        } else {
            Check(builder->AppendEmptyValues(2));
        }
        columns.push_back(Get(builder->Finish()));
    }
    return arrow::RecordBatch::Make(entity.schema, 2, columns);
}

void TestTwelveEntityContracts() {
    const struct {
        npm::NpmFamilyEntityKind kind;
        const char* module;
        const char* entity;
        int columns;
    } kinds[] = {{npm::NpmFamilyEntityKind::kBasic, "basic", "basic", 31},
                 {npm::NpmFamilyEntityKind::kSession, "session", "session", 49},
                 {npm::NpmFamilyEntityKind::kDns, "dns", "dns_transaction", 25},
                 {npm::NpmFamilyEntityKind::kHttp1, "http1", "http1_transaction", 22},
                 {npm::NpmFamilyEntityKind::kTls, "tls", "tls_handshake", 25},
                 {npm::NpmFamilyEntityKind::kIcmp, "icmp", "icmp_event", 27}};
    std::set<std::string> ids;
    for (const auto& kind : kinds) {
        for (auto family : {packet::AddressFamily::kIPv4, packet::AddressFamily::kIPv6}) {
            const bool ipv4 = family == packet::AddressFamily::kIPv4;
            auto entity = npm::NpmFamilyEntityDescriptor(kind.kind, family);
            assert(entity.entity_id == std::string(kind.entity) + (ipv4 ? "_ipv4" : "_ipv6"));
            assert(entity.module_id == kind.module && entity.schema_version == 1);
            assert(ids.insert(entity.entity_id).second);
            assert(entity.schema->num_fields() == kind.columns);
            assert(!entity.schema->GetFieldByName("ip_family"));
            assert(Get(entity.schema->metadata()->Get("flowsql.entity")) == entity.entity_id);
            assert(Get(entity.schema->metadata()->Get("flowsql.ip_family")) == (ipv4 ? "4" : "6"));
            const bool cumulative =
                kind.kind == npm::NpmFamilyEntityKind::kBasic || kind.kind == npm::NpmFamilyEntityKind::kSession;
            assert(entity.identity_column == (cumulative ? "session_id" : "entity_instance_id"));
            assert(entity.revision_semantics ==
                   (cumulative ? npm::NpmRevisionSemanticsV1::kCumulative : npm::NpmRevisionSemanticsV1::kEvent));
            auto batch = Fixture(entity);
            Check(npm::ValidateNpmFamilyEntityRows(entity, *batch));
            Check(npm::ValidateNpmFamilyEntityRows(entity, *batch->Slice(1, 1)));
            for (const auto& field : entity.schema->fields()) {
                if (!field->metadata() || !field->metadata()->Contains("flowsql.semantic")) continue;
                assert(field->type()->Equals(ipv4 ? arrow::uint32() : arrow::fixed_size_binary(16)));
                assert(Get(field->metadata()->Get("flowsql.semantic")) == (ipv4 ? "ipv4_address" : "ipv6_address"));
                const auto column = batch->GetColumnByName(field->name());
                if (ipv4) {
                    assert(static_cast<const arrow::UInt32Array&>(*column).Value(0) == 0xffffffffu);
                } else {
                    const auto* bytes = static_cast<const arrow::FixedSizeBinaryArray&>(*column).GetValue(0);
                    assert(bytes[4] == 0 && bytes[5] == 0xff && bytes[6] == 0x80 && bytes[15] == 1);
                }
            }
            auto columns = batch->columns();
            const auto identity = entity.schema->GetFieldIndex(entity.identity_column);
            columns[identity] = Get(arrow::MakeArrayOfNull(arrow::uint64(), 2));
            assert(
                !npm::ValidateNpmFamilyEntityRows(entity, *arrow::RecordBatch::Make(entity.schema, 2, columns)).ok());
            auto wrong_family = npm::NpmFamilyEntityDescriptor(
                kind.kind, ipv4 ? packet::AddressFamily::kIPv6 : packet::AddressFamily::kIPv4);
            assert(!npm::ValidateNpmFamilyEntityRows(wrong_family, *batch).ok());
            std::printf("%s fields=%d typed row and null/slice contracts passed\n", entity.entity_id.c_str(),
                        kind.columns);
        }
    }
    assert(ids.size() == 12);
    auto session = npm::NpmFamilyEntityDescriptor(npm::NpmFamilyEntityKind::kSession, packet::AddressFamily::kIPv4);
    assert(session.schema->GetFieldByName("primary_label_id")->nullable());
    assert(session.schema->GetFieldByName("tcp_rtt_mean_ns")->type()->Equals(arrow::int64()));
    assert(session.schema->GetFieldByName("tcp_unique_payload_bps_ab")->type()->Equals(arrow::float64()));
    auto icmp = npm::NpmFamilyEntityDescriptor(npm::NpmFamilyEntityKind::kIcmp, packet::AddressFamily::kIPv6);
    assert(icmp.schema->GetFieldByName("quoted_src_ip")->nullable());
    assert(!icmp.schema->GetFieldByName("src_port"));
    auto batch = Fixture(icmp);
    auto columns = batch->columns();
    columns[icmp.schema->GetFieldIndex("quoted_src_ip")] = Get(arrow::MakeArrayOfNull(arrow::fixed_size_binary(16), 2));
    columns[icmp.schema->GetFieldIndex("quoted_dst_ip")] = Get(arrow::MakeArrayOfNull(arrow::fixed_size_binary(16), 2));
    Check(npm::ValidateNpmFamilyEntityRows(icmp, *arrow::RecordBatch::Make(icmp.schema, 2, columns)));
    arrow::UInt64Builder revisions;
    Check(revisions.AppendValues({2, 2}));
    columns[icmp.schema->GetFieldIndex("revision")] = Get(revisions.Finish());
    assert(!npm::ValidateNpmFamilyEntityRows(icmp, *arrow::RecordBatch::Make(icmp.schema, 2, columns)).ok());
}

void TestSnapshotAndBorrowedInput() {
    npm::NpmSessionSnapshot4 ended;
    {
        npm::NpmSessionSnapshot4 source;
        source.key.a_ip = network::IPv4Address::FromUInt32(0x01020304u);
        source.key.input_namespace = std::string(128, 'n');
        source.facts.session_id = 17;
        source.facts.packets_ab = 3;
        ended = source;
    }
    const auto view = ended.View();
    assert(view.key.Get4() == &ended.key && view.facts.session_id == 17 && view.facts.packets_ab == 3);
    assert(view.key.Get4()->a_ip.ToUInt32() == 0x01020304u && view.key.Get4()->input_namespace.size() == 128);
    packet::PacketLayerFacts layer;
    layer.address_state = packet::PacketAddressState::kAvailable;
    layer.transport_protocol = 6;
    layer.ports_valid = true;
    packet::PacketIpEndpoints4 pair4;
    packet::PacketIpEndpoints6 pair6;
    npm::NpmFamilyPacketView transport;
    transport.layer = &layer;
    npm::NpmFamilyInputEvent input;
    input.layer = &layer;
    input.endpoints = &pair4;
    input.transport = &transport;
    input.session = &view;
    Check(npm::ValidateNpmFamilyInputEvent(input));
    input.observation_domain_id = 1;
    assert(!npm::ValidateNpmFamilyInputEvent(input).ok());
    input.observation_domain_id = 0;
    input.endpoints = &pair6;
    assert(!npm::ValidateNpmFamilyInputEvent(input).ok());
    input.kind = npm::NpmInputKindV1::kControlPacket;
    assert(!npm::ValidateNpmFamilyInputEvent(input).ok());
    input.session = nullptr;
    input.transport = nullptr;
    layer.ports_valid = false;
    layer.transport_protocol = 58;
    Check(npm::ValidateNpmFamilyInputEvent(input));
    input.endpoints = &pair4;
    assert(!npm::ValidateNpmFamilyInputEvent(input).ok());
    layer.transport_protocol = 1;
    Check(npm::ValidateNpmFamilyInputEvent(input));
    input.endpoints = static_cast<const packet::PacketIpEndpoints4*>(nullptr);
    assert(!npm::ValidateNpmFamilyInputEvent(input).ok());

    npm::NpmIcmpEvent4 event;
    event.src_ip = network::IPv4Address::FromUInt32(0);
    assert(!event.quoted_src_ip);
    event.quoted_src_ip = network::IPv4Address::FromUInt32(0);
    assert(event.quoted_src_ip && event.quoted_src_ip->ToUInt32() == 0);
    npm::NpmDnsResult4 dns;
    dns.transaction.qtype = 28;  // AAAA queried over IPv4 still uses a four-byte endpoint.
    npm::NpmDnsResult6 dns6;
    dns6.transaction.qtype = 1;
    assert(sizeof(dns.a_ip) == 4 && sizeof(dns6.a_ip) == 16);
}
}  // namespace

int main() {
    TestTwelveEntityContracts();
    TestSnapshotAndBorrowedInput();
    std::puts("NPM family result contract: 12 entities and snapshot/input lifecycle passed");
}
