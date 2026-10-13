// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_packet_test_support.h"

#include <tests/common/packet_family_test_support.h>

#include <operators/npm_basic/core/npm_task_budget.h>
#include <operators/npm_basic/modules/icmp/npm_icmp_module.h>

#include <common/network/netbase.h>

#include <arrow/api.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

namespace npm = flowsql::npm;

namespace {

class Emitter final : public npm::INpmResultEmitterV1 {
 public:
    std::vector<std::shared_ptr<arrow::RecordBatch>> rows;
    int failure = 0;
    int Emit(std::string_view name, const arrow::RecordBatch& batch) override {
        assert(name == "icmp_event_ipv4" || name == "icmp_event_ipv6");
        if (failure) return failure;
        rows.push_back(batch.Slice(0));
        return 0;
    }
};

struct Input {
    std::vector<uint8_t> bytes;
    flowsql::testing::OwnedPacketLayer layer;
    npm::NpmInputEventV1 event;
    Input(uint8_t type, int64_t timestamp, bool reverse = false, uint64_t domain = 7, bool ipv6 = false) {
        const size_t header = ipv6 ? 40 : 20;
        bytes.resize(header + 8);
        bytes[0] = ipv6 ? 0x60 : 0x45;
        if (ipv6) {
            bytes[5] = 8;
            bytes[6] = 58;
        } else {
            bytes[3] = 28;
            bytes[9] = 1;
        }
        bytes[header] = type;
        bytes[header + 4] = 0x12;
        bytes[header + 5] = 0x34;
        bytes[header + 7] = 9;
        layer.status = flowsql::packet::LayerStatus::kDecoded;
        layer.layer_count = 1;
        layer.layers[0] = {static_cast<uint16_t>(ipv6 ? flowsql::eLayer::IPv6 : flowsql::eLayer::IPv4), 0};
        layer.network_layer_index = 0;
        layer.transport_protocol = ipv6 ? 58 : 1;
        if (ipv6) {
            flowsql::packet::IPv6Address src{}, dst{};
            src.bytes[0] = dst.bytes[0] = 10;
            src.bytes[15] = reverse ? 2 : 1;
            dst.bytes[15] = reverse ? 1 : 2;
            layer.SetEndpoints(src, dst);
        } else {
            flowsql::packet::IPv4Address src{}, dst{};
            src.bytes[0] = dst.bytes[0] = 10;
            src.bytes[3] = reverse ? 2 : 1;
            dst.bytes[3] = reverse ? 1 : 2;
            layer.SetEndpoints(src, dst);
        }
        event.kind = npm::NpmInputKindV1::kControlPacket;
        event.observation_domain_id = domain;
        event.packet.meta.timestamp_ns = timestamp;
        event.packet.meta.captured_len = static_cast<uint32_t>(bytes.size());
        event.packet.bytes = {bytes.data(), bytes.size()};
        event.layer = &layer;
        event.body = {bytes.data() + header, 8};
    }
};

void PutQuotedTcp(Input* input, bool reverse = false) {
    const bool ipv6 = input->layer.Get6() != nullptr;
    const size_t header = ipv6 ? 40 : 20, quote = header + 8, ports = quote + header;
    input->bytes.resize(ports + 4);
    if (ipv6) {
        input->bytes[5] = input->bytes.size() - 40;
        input->bytes[quote] = 0x60;
        input->bytes[quote + 5] = 20;
        input->bytes[quote + 6] = 6;
    } else {
        input->bytes[3] = input->bytes.size();
        input->bytes[quote] = 0x45;
        input->bytes[quote + 3] = 40;
        input->bytes[quote + 9] = 6;
    }
    const size_t src = quote + (ipv6 ? 8 : 12), dst = quote + (ipv6 ? 24 : 16), width = ipv6 ? 16 : 4;
    input->bytes[src] = input->bytes[dst] = 192;
    input->bytes[src + width - 1] = reverse ? 2 : 1;
    input->bytes[dst + width - 1] = reverse ? 1 : 2;
    if (ipv6) {
        input->bytes[src + 3] = reverse ? 2 : 1;
        input->bytes[dst + 3] = reverse ? 1 : 2;
    }
    input->bytes[ports] = reverse ? 0 : 0x12;
    input->bytes[ports + 1] = reverse ? 80 : 0x34;
    input->bytes[ports + 2] = reverse ? 0x12 : 0;
    input->bytes[ports + 3] = reverse ? 0x34 : 80;
    input->event.packet.bytes = {input->bytes.data(), input->bytes.size()};
    input->event.packet.meta.captured_len = static_cast<uint32_t>(input->bytes.size());
    input->event.body = {input->bytes.data() + header, input->bytes.size() - header};
}

template <typename Array>
std::shared_ptr<Array> Column(const std::shared_ptr<arrow::RecordBatch>& rows, const char* name) {
    return std::static_pointer_cast<Array>(rows->GetColumnByName(name));
}

void TestModuleAndBudget(bool ipv6) {
    const auto family = ipv6 ? flowsql::packet::AddressFamily::kIPv6 : flowsql::packet::AddressFamily::kIPv4;
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmIcmpProtocolModuleV1 module({}, budget);
    Emitter emitter;
    Input request(ipv6 ? 128 : 8, 100, false, 7, ipv6), reply(ipv6 ? 129 : 0, 120, true, 7, ipv6),
        error(ipv6 ? 1 : 3, 125, false, 7, ipv6);
    assert(module.OnInput(request.event, emitter) == 0 && emitter.rows.empty());
    assert(module.OnInput(reply.event, emitter) == 0);
    assert(emitter.rows.size() == 1);
    const auto& row = emitter.rows.back();
    assert(row->schema()->Equals(*npm::NpmIcmpEventEntityDescriptorV1(family).schema, true));
    assert(npm::ValidateNpmEntityRowsV1("icmp", npm::NpmIcmpEventEntityDescriptorV1(family), *row).error ==
           npm::NpmProtocolContractErrorV1::kNone);
    assert(Column<arrow::StringArray>(row, "outcome")->GetString(0) == "echo_matched");
    assert(Column<arrow::Int64Array>(row, "latency_ns")->Value(0) == 20);
    assert(module.OnInput(error.event, emitter) == 0);
    assert(emitter.rows.size() == 2);
    assert(Column<arrow::StringArray>(emitter.rows.back(), "outcome")->GetString(0) == "icmp_error");
    assert(Column<arrow::StringArray>(emitter.rows.back(), "quote_status")->GetString(0) == "unavailable");
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), "active_quoted_session_id")->IsNull(0));
    assert(module.Finish(130, emitter) == 0);
    module.Abort();
    assert(budget->Usage().module_state_bytes == 0);

    npm::NpmIcmpProtocolModuleV1 failed({}, budget);
    Emitter rejecting;
    assert(failed.OnInput(request.event, rejecting) == 0);
    assert(budget->Usage().module_state_bytes > 0);
    rejecting.failure = EIO;
    assert(failed.OnInput(reply.event, rejecting) == EIO && rejecting.rows.empty());
    failed.Abort();
    assert(budget->Usage().module_state_bytes == 0);

    config.max_tracked_bytes = 1;
    auto tiny_budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmIcmpProtocolModuleV1 tiny({}, tiny_budget);
    assert(tiny.OnInput(request.event, emitter) == ENOMEM);
    assert(tiny.OnInput(error.event, emitter) == ENOSPC);
    tiny.Abort();
    assert(tiny_budget->Usage().module_state_bytes == 0);
}

template <typename KeyType>
void TestActiveQuotedSession() {
    constexpr bool ipv6 = sizeof(decltype(KeyType{}.a_ip)) == 16;
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmSessionTable sessions(config, budget);
    npm::NpmTestPacketBinding binding;
    KeyType key;
    key.input_namespace = "default";
    key.observation_domain_id = 7;
    key.transport_protocol = 6;
    decltype(key.a_ip) src, dst;
    src.bytes[0] = dst.bytes[0] = 192;
    src.bytes[sizeof(src) - 1] = 1;
    dst.bytes[sizeof(dst) - 1] = 2;
    if constexpr (ipv6) {
        src.bytes[3] = 1;
        dst.bytes[3] = 2;
    }
    key.a_ip = src;
    key.a_port = 0x1234;
    key.b_ip = dst;
    key.b_port = 80;
    binding.key = key;
    flowsql::packet::PacketMeta meta;
    meta.timestamp_ns = 50;
    meta.wire_len = 40;
    npm::NpmSessionObserveResult observed;
    assert(sessions.Observe(binding, meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.has_active_session);
    const uint64_t session_id = observed.active_session.session_id;
    // Same namespace/domain/protocol/ports and common address prefix, but another family.
    using OtherKey = std::conditional_t<ipv6, npm::NpmSessionKey4, npm::NpmSessionKey6>;
    OtherKey other;
    other.input_namespace = key.input_namespace;
    other.observation_domain_id = key.observation_domain_id;
    other.transport_protocol = key.transport_protocol;
    other.a_port = key.a_port;
    other.b_port = key.b_port;
    std::memcpy(other.a_ip.bytes, src.bytes, 4);
    std::memcpy(other.b_ip.bytes, dst.bytes, 4);
    if constexpr (!ipv6) {
        other.a_ip.bytes[15] = 1;
        other.b_ip.bytes[15] = 2;
    }
    npm::NpmTestPacketBinding other_binding;
    other_binding.key = other;
    npm::NpmSessionObserveResult other_observed;
    assert(sessions.Observe(other_binding, meta, &other_observed) == npm::NpmSessionTableError::kNone);
    assert(other_observed.active_session.session_id != session_id);

    npm::NpmIcmpProtocolModuleV1 module({}, budget);
    module.BindSessions(&sessions, "default");
    Emitter emitter;
    Input error(ipv6 ? 1 : 3, 60, false, 7, ipv6);
    PutQuotedTcp(&error);
    assert(module.OnInput(error.event, emitter) == 0 && emitter.rows.size() == 1);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), "active_quoted_session_id")->Value(0) == session_id);
    const auto quoted = emitter.rows.back()->GetColumnByName("quoted_src_ip");
    if constexpr (ipv6)
        assert(std::memcmp(std::static_pointer_cast<arrow::FixedSizeBinaryArray>(quoted)->GetValue(0), src.bytes, 16) ==
               0);
    else
        assert(std::static_pointer_cast<arrow::UInt32Array>(quoted)->Value(0) == src.ToUInt32());
    const auto deadline = sessions.NextEventDeadlineNs();
    PutQuotedTcp(&error, true);
    assert(module.OnInput(error.event, emitter) == 0 && emitter.rows.size() == 2);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), "active_quoted_session_id")->Value(0) == session_id);
    assert(sessions.size() == 2 && sessions.NextEventDeadlineNs() == deadline);
    error.event.observation_domain_id = 8;
    assert(module.OnInput(error.event, emitter) == 0 && emitter.rows.size() == 3);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), "active_quoted_session_id")->IsNull(0));
    error.event.observation_domain_id = 7;
    module.BindSessions(&sessions, "other");
    assert(module.OnInput(error.event, emitter) == 0);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), "active_quoted_session_id")->IsNull(0));
    module.BindSessions(&sessions, "default");

    key.transport_protocol = 17;
    binding.key = key;
    assert(sessions.Observe(binding, meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.has_active_session && observed.active_session.session_id != session_id);
    error.bytes[ipv6 ? 54 : 37] = 17;
    assert(module.OnInput(error.event, emitter) == 0);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), "active_quoted_session_id")->Value(0) ==
           observed.active_session.session_id);
    assert(sessions.size() == 3);

    npm::NpmSessionSnapshotBatch ended;
    assert(sessions.FinishAllAtEof(&ended) == npm::NpmSessionTableError::kNone && ended.size() == 3);
    assert(module.OnInput(error.event, emitter) == 0);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), "active_quoted_session_id")->IsNull(0));
}

}  // namespace

void TestIcmpModuleAndBudget() {
    TestModuleAndBudget(false);
    TestModuleAndBudget(true);
    TestActiveQuotedSession<npm::NpmSessionKey4>();
    TestActiveQuotedSession<npm::NpmSessionKey6>();
}
