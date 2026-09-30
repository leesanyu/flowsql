// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/dns/npm_dns_module.h>

#include <arrow/api.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace npm = flowsql::npm;
using Bytes = std::vector<uint8_t>;

namespace {

class Budget final : public npm::INpmTaskBudget {
 public:
    npm::NpmBudgetError Reserve(npm::NpmBudgetCategory, uint64_t bytes) override {
        if (bytes > limit - used) return npm::NpmBudgetError::kTrackedLimitExceeded;
        used += bytes;
        return npm::NpmBudgetError::kNone;
    }
    npm::NpmBudgetError Release(npm::NpmBudgetCategory, uint64_t bytes) override {
        assert(bytes <= used);
        used -= bytes;
        return npm::NpmBudgetError::kNone;
    }
    npm::NpmBudgetUsage Usage() const override { return {0, used, 0, 0}; }
    uint64_t limit = 1 << 20;
    uint64_t used = 0;
};

class Emitter final : public npm::INpmResultEmitterV1 {
 public:
    int Emit(std::string_view entity, const arrow::RecordBatch& batch) override {
        assert(entity == "dns_transaction" && batch.num_rows() == 1);
        auto outcome = std::static_pointer_cast<arrow::StringArray>(batch.GetColumnByName("outcome"));
        auto reason = std::static_pointer_cast<arrow::StringArray>(batch.GetColumnByName("incomplete_reason"));
        outcomes.push_back(outcome->GetString(0));
        reasons.push_back(reason->IsNull(0) ? "" : reason->GetString(0));
        return error;
    }
    int error = 0;
    std::vector<std::string> outcomes;
    std::vector<std::string> reasons;
};

class Cursor final : public npm::INpmTcpStreamCursorV1 {
 public:
    explicit Cursor(const Bytes& bytes, int64_t time) : bytes_(bytes), time_(time) {}
    bool Peek(npm::NpmTcpStreamEventV1* event) const override {
        if (consumed_) return false;
        event->kind = npm::NpmTcpStreamEventKindV1::kData;
        event->bytes = {bytes_.data(), bytes_.size()};
        event->captured_at_ns = time_;
        return true;
    }
    int Consume(uint64_t bytes) override {
        if (consumed_ || bytes != bytes_.size()) return EINVAL;
        consumed_ = true;
        return 0;
    }

 private:
    const Bytes& bytes_;
    int64_t time_;
    bool consumed_ = false;
};

Bytes Message(uint16_t id, bool response = false) {
    return {static_cast<uint8_t>(id >> 8),
            static_cast<uint8_t>(id),
            static_cast<uint8_t>(response ? 0x80 : 0),
            0,
            0,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            1,
            'a',
            0,
            0,
            1,
            0,
            1};
}

npm::NpmSessionView Session(npm::NpmSessionKey* key, uint64_t id) {
    npm::NpmSessionView session;
    session.session_id = id;
    session.key = key;
    return session;
}

npm::NpmSessionKey Key(uint8_t transport) {
    npm::NpmSessionKey key;
    key.ip_family = flowsql::packet::AddressFamily::kIPv4;
    key.transport_protocol = transport;
    flowsql::packet::IPv4Address a{}, b{};
    a.bytes[0] = 192;
    a.bytes[1] = 0;
    a.bytes[2] = 2;
    a.bytes[3] = 1;
    b.bytes[0] = 198;
    b.bytes[1] = 51;
    b.bytes[2] = 100;
    b.bytes[3] = 2;
    key.a.ip = a;
    key.b.ip = b;
    key.a.port = 53000;
    key.b.port = 53;
    key.observation_domain_id = 44;
    return key;
}

int Send(npm::NpmDnsProtocolModuleV1* module, const npm::NpmSessionView& session, const Bytes& bytes,
         npm::NpmPacketDirection direction, int64_t time, Emitter* emitter) {
    npm::NpmPacketView packet;
    packet.direction = direction;
    packet.payload = {bytes.data(), bytes.size()};
    packet.packet.meta.timestamp_ns = time;
    npm::NpmInputEventV1 input;
    input.kind = npm::NpmInputKindV1::kUdpDatagram;
    input.session = &session;
    input.transport = &packet;
    input.packet = packet.packet;
    input.body = packet.payload;
    return module->OnInput(input, *emitter);
}

void TestUdpTimeEndAndFailure() {
    auto budget = std::make_shared<Budget>();
    npm::NpmDnsProtocolModuleV1 module({{1001}, 1'000'000, 2}, budget);
    auto key = Key(17);
    auto session = Session(&key, 7);
    Emitter emitter;
    assert(Send(&module, session, Message(1), npm::NpmPacketDirection::kAToB, 100, &emitter) == 0);
    assert(module.NextEventDeadlineNs() == 1'000'100);
    assert(module.OnTime({1'000'099, 1'000'099}, emitter) == 0 && emitter.outcomes.empty());
    assert(module.OnTime({1'000'100, 1'000'100}, emitter) == 0);
    assert(emitter.outcomes == std::vector<std::string>{"query_only"});
    assert(emitter.reasons.back() == "response_not_observed_by_deadline");
    assert(!module.NextEventDeadlineNs());
    assert(Send(&module, session, Message(2), npm::NpmPacketDirection::kAToB, 200, &emitter) == 0);
    assert(Send(&module, session, Message(2, true), npm::NpmPacketDirection::kBToA, 300, &emitter) == 0);
    assert(emitter.outcomes.back() == "matched");
    assert(Send(&module, session, Message(3), npm::NpmPacketDirection::kAToB, 400, &emitter) == 0);
    assert(module.OnSessionEnd(session, npm::NpmSessionEndReason::kEof, 500, emitter) == 0);
    assert(emitter.reasons.back() == "session_end");
    assert(module.Finish(500, emitter) == 0 && budget->used == 0);

    npm::NpmDnsProtocolModuleV1 failing({{1001}, 1'000'000, 2}, budget);
    assert(Send(&failing, session, Message(4), npm::NpmPacketDirection::kAToB, 600, &emitter) == 0);
    emitter.error = EIO;
    assert(Send(&failing, session, Message(4, true), npm::NpmPacketDirection::kBToA, 700, &emitter) == EIO);
    failing.Abort();
    assert(budget->used == 0);
}

void TestTcpStreamAndBudget() {
    auto budget = std::make_shared<Budget>();
    npm::NpmDnsProtocolModuleV1 module({{1001}, 1'000'000, 2}, budget);
    auto key = Key(6);
    auto session = Session(&key, 8);
    Emitter emitter;
    npm::NpmTcpStreamContextV1 context;
    context.session = &session;
    context.origin = npm::NpmTcpStreamOriginV1::kSyn;
    context.direction = npm::NpmPacketDirection::kAToB;
    context.observed_at_ns = 100;
    const auto query = Message(5);
    Bytes frame = {0, static_cast<uint8_t>(query.size())};
    frame.insert(frame.end(), query.begin(), query.end());
    Cursor request(frame, 100);
    assert(module.OnTcpStreamReadable(context, request, emitter) == 0 && emitter.outcomes.empty());
    assert(module.NextEventDeadlineNs() == 1'000'100);
    const auto answer = Message(5, true);
    frame = {0, static_cast<uint8_t>(answer.size())};
    frame.insert(frame.end(), answer.begin(), answer.end());
    context.direction = npm::NpmPacketDirection::kBToA;
    context.observed_at_ns = 150;
    Cursor response(frame, 150);
    assert(module.OnTcpStreamReadable(context, response, emitter) == 0);
    assert(emitter.outcomes == std::vector<std::string>{"matched"});
    assert(module.OnSessionEnd(session, npm::NpmSessionEndReason::kEof, 200, emitter) == 0);
    assert(module.Finish(200, emitter) == 0 && budget->used == 0);

    budget->limit = 1;
    npm::NpmDnsProtocolModuleV1 rejected({{1001}, 1'000'000, 2}, budget);
    assert(Send(&rejected, session, Message(6), npm::NpmPacketDirection::kAToB, 300, &emitter) == ENOSPC);
    rejected.Abort();
    assert(budget->used == 0);

    budget->limit = 4608;
    npm::NpmDnsProtocolModuleV1 output_rejected({{1001}, 1'000'000, 2}, budget);
    auto udp_key = Key(17);
    auto udp_session = Session(&udp_key, 9);
    assert(Send(&output_rejected, udp_session, Message(7), npm::NpmPacketDirection::kAToB, 400, &emitter) == 0);
    assert(Send(&output_rejected, udp_session, Message(7, true), npm::NpmPacketDirection::kBToA, 500, &emitter) ==
           ENOSPC);
    output_rejected.Abort();
    assert(budget->used == 0);
}

}  // namespace

int main() {
    TestUdpTimeEndAndFailure();
    TestTcpStreamAndBudget();
    std::puts("NPM DNS module tests passed");
    return 0;
}
