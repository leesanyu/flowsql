// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/core/npm_task_budget.h>
#include <operators/npm_basic/modules/icmp/npm_icmp_module.h>

#include <common/network/netbase.h>

#include <arrow/api.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <vector>

namespace npm = flowsql::npm;

namespace {

class Emitter final : public npm::INpmResultEmitterV1 {
 public:
    std::vector<std::shared_ptr<arrow::RecordBatch>> rows;
    int failure = 0;
    int Emit(std::string_view name, const arrow::RecordBatch& batch) override {
        assert(name == "icmp_event");
        if (failure) return failure;
        rows.push_back(batch.Slice(0));
        return 0;
    }
};

struct Input {
    std::vector<uint8_t> bytes;
    flowsql::packet::PacketLayerInfo layer;
    npm::NpmInputEventV1 event;
    Input(uint8_t type, int64_t timestamp, bool reverse = false, uint64_t domain = 7) {
        bytes.resize(28);
        bytes[0] = 0x45;
        bytes[2] = 0;
        bytes[3] = 28;
        bytes[9] = 1;
        bytes[20] = type;
        bytes[24] = 0x12;
        bytes[25] = 0x34;
        bytes[27] = 9;
        layer.status = flowsql::packet::LayerStatus::kDecoded;
        layer.layer_count = 1;
        layer.layers[0] = {static_cast<uint16_t>(flowsql::eLayer::IPv4), 0};
        layer.network_layer_index = 0;
        layer.transport_protocol = 1;
        flowsql::packet::IPv4Address src, dst;
        src.bytes[0] = dst.bytes[0] = 10;
        src.bytes[3] = reverse ? 2 : 1;
        dst.bytes[3] = reverse ? 1 : 2;
        layer.src_ip = src;
        layer.dst_ip = dst;
        event.kind = npm::NpmInputKindV1::kControlPacket;
        event.observation_domain_id = domain;
        event.packet.meta.timestamp_ns = timestamp;
        event.packet.meta.captured_len = static_cast<uint32_t>(bytes.size());
        event.packet.bytes = {bytes.data(), bytes.size()};
        event.layer = &layer;
        event.body = {bytes.data() + 20, 8};
    }
};

void PutQuotedTcp(Input* input, bool reverse = false) {
    input->bytes.resize(52);
    input->bytes[3] = 52;
    input->bytes[28] = 0x45;
    input->bytes[30] = 0;
    input->bytes[31] = 40;
    input->bytes[37] = 6;
    input->bytes[40] = input->bytes[44] = 192;
    input->bytes[43] = reverse ? 2 : 1;
    input->bytes[47] = reverse ? 1 : 2;
    input->bytes[48] = reverse ? 0 : 0x12;
    input->bytes[49] = reverse ? 80 : 0x34;
    input->bytes[50] = reverse ? 0x12 : 0;
    input->bytes[51] = reverse ? 0x34 : 80;
    input->event.packet.bytes = {input->bytes.data(), input->bytes.size()};
    input->event.packet.meta.captured_len = static_cast<uint32_t>(input->bytes.size());
    input->event.body = {input->bytes.data() + 20, input->bytes.size() - 20};
}

template <typename Array>
std::shared_ptr<Array> Column(const std::shared_ptr<arrow::RecordBatch>& rows, int index) {
    return std::static_pointer_cast<Array>(rows->column(index));
}

void TestModuleAndBudget() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmIcmpProtocolModuleV1 module({}, budget);
    Emitter emitter;
    Input request(8, 100), reply(0, 120, true), error(3, 125);
    assert(module.OnInput(request.event, emitter) == 0 && emitter.rows.empty());
    assert(module.OnInput(reply.event, emitter) == 0);
    assert(emitter.rows.size() == 1);
    const auto& row = emitter.rows.back();
    assert(row->schema()->Equals(*npm::NpmIcmpEventEntityDescriptorV1().schema, true));
    assert(npm::ValidateNpmEntityRowsV1("icmp", npm::NpmIcmpEventEntityDescriptorV1(), *row).error ==
           npm::NpmProtocolContractErrorV1::kNone);
    assert(Column<arrow::StringArray>(row, 8)->GetString(0) == "echo_matched");
    assert(Column<arrow::Int64Array>(row, 18)->Value(0) == 20);
    assert(module.OnInput(error.event, emitter) == 0);
    assert(emitter.rows.size() == 2);
    assert(Column<arrow::StringArray>(emitter.rows.back(), 8)->GetString(0) == "icmp_error");
    assert(Column<arrow::StringArray>(emitter.rows.back(), 19)->GetString(0) == "unavailable");
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), 26)->IsNull(0));
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

void TestActiveQuotedSession() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmSessionTable sessions(config, budget);
    npm::NpmSessionPacketBinding binding;
    binding.key.input_namespace = "default";
    binding.key.observation_domain_id = 7;
    binding.key.ip_family = flowsql::packet::AddressFamily::kIPv4;
    binding.key.transport_protocol = 6;
    flowsql::packet::IPv4Address src, dst;
    src.bytes[0] = dst.bytes[0] = 192;
    src.bytes[3] = 1;
    dst.bytes[3] = 2;
    binding.key.a = {src, 0x1234};
    binding.key.b = {dst, 80};
    flowsql::packet::PacketMeta meta;
    meta.timestamp_ns = 50;
    meta.wire_len = 40;
    npm::NpmSessionObserveResult observed;
    assert(sessions.Observe(binding, meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.has_active_session);
    const uint64_t session_id = observed.active_session.session_id;
    npm::NpmIcmpProtocolModuleV1 module({}, budget);
    module.BindSessions(&sessions, "default");
    Emitter emitter;
    Input error(3, 60);
    PutQuotedTcp(&error);
    assert(module.OnInput(error.event, emitter) == 0 && emitter.rows.size() == 1);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), 26)->Value(0) == session_id);
    const auto deadline = sessions.NextEventDeadlineNs();
    PutQuotedTcp(&error, true);
    assert(module.OnInput(error.event, emitter) == 0 && emitter.rows.size() == 2);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), 26)->Value(0) == session_id);
    assert(sessions.size() == 1 && sessions.NextEventDeadlineNs() == deadline);
    error.event.observation_domain_id = 8;
    assert(module.OnInput(error.event, emitter) == 0 && emitter.rows.size() == 3);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), 26)->IsNull(0));
    error.event.observation_domain_id = 7;
    module.BindSessions(&sessions, "other");
    assert(module.OnInput(error.event, emitter) == 0);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), 26)->IsNull(0));
    module.BindSessions(&sessions, "default");

    binding.key.transport_protocol = 17;
    assert(sessions.Observe(binding, meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.has_active_session && observed.active_session.session_id != session_id);
    error.bytes[37] = 17;
    assert(module.OnInput(error.event, emitter) == 0);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), 26)->Value(0) == observed.active_session.session_id);
    assert(sessions.size() == 2);

    std::vector<npm::NpmSessionSnapshot> ended;
    assert(sessions.FinishAllAtEof(&ended) == npm::NpmSessionTableError::kNone && ended.size() == 2);
    assert(module.OnInput(error.event, emitter) == 0);
    assert(Column<arrow::UInt64Array>(emitter.rows.back(), 26)->IsNull(0));
}

}  // namespace

void TestIcmpModuleAndBudget() {
    TestModuleAndBudget();
    TestActiveQuotedSession();
}
