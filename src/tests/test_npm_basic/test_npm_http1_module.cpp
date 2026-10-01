// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/http1/npm_http1_module.h>

#include <arrow/api.h>

#include <cassert>
#include <cerrno>
#include <memory>
#include <string>
#include <vector>

namespace npm = flowsql::npm;

namespace {

class Budget final : public npm::INpmTaskBudget {
 public:
    npm::NpmBudgetError Reserve(npm::NpmBudgetCategory category, uint64_t bytes) override {
        assert(category == npm::NpmBudgetCategory::kModuleState);
        if (bytes > limit - used) return npm::NpmBudgetError::kTrackedLimitExceeded;
        used += bytes;
        return npm::NpmBudgetError::kNone;
    }
    npm::NpmBudgetError Release(npm::NpmBudgetCategory category, uint64_t bytes) override {
        assert(category == npm::NpmBudgetCategory::kModuleState && bytes <= used);
        used -= bytes;
        return npm::NpmBudgetError::kNone;
    }
    npm::NpmBudgetUsage Usage() const override { return {0, used, 0, 0}; }
    uint64_t used = 0;
    uint64_t limit = 2 * 1024 * 1024;
};

class Emitter final : public npm::INpmResultEmitterV1 {
 public:
    int Emit(std::string_view entity, const arrow::RecordBatch& rows) override {
        assert(entity == "http1_transaction" && rows.num_rows() == 1);
        assert(npm::ValidateNpmEntityRowsV1("http1", npm::NpmHttp1TransactionEntityDescriptorV1(), rows).error ==
               npm::NpmProtocolContractErrorV1::kNone);
        ++calls;
        domains.push_back(
            std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("observation_domain_id"))->Value(0));
        outcomes.push_back(std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("outcome"))->GetString(0));
        return error;
    }
    int calls = 0;
    int error = 0;
    std::vector<uint64_t> domains;
    std::vector<std::string> outcomes;
};

class Cursor final : public npm::INpmTcpStreamCursorV1 {
 public:
    explicit Cursor(std::string bytes, int64_t capture_time) : bytes_(std::move(bytes)), capture_time_(capture_time) {}
    bool Peek(npm::NpmTcpStreamEventV1* event) const override {
        if (consumed_) return false;
        event->kind = npm::NpmTcpStreamEventKindV1::kData;
        event->bytes = {reinterpret_cast<const uint8_t*>(bytes_.data()), bytes_.size()};
        event->captured_at_ns = capture_time_;
        return true;
    }
    int Consume(uint64_t bytes) override {
        if (consumed_ || bytes != bytes_.size()) return EINVAL;
        consumed_ = true;
        return 0;
    }
    bool consumed() const { return consumed_; }

 private:
    std::string bytes_;
    int64_t capture_time_;
    bool consumed_ = false;
};

npm::NpmHttp1ConfigV1 Config() {
    npm::NpmHttp1ConfigV1 config;
    config.primary_label_ids = {1001};
    config.max_header_bytes = 1024;
    return config;
}

npm::NpmSessionKey Key(uint64_t domain) {
    npm::NpmSessionKey key;
    key.ip_family = flowsql::packet::AddressFamily::kIPv4;
    key.transport_protocol = 6;
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
    key.a.port = 50000;
    key.b.port = 80;
    key.observation_domain_id = domain;
    return key;
}

int Send(npm::NpmHttp1ProtocolModuleV1* module, const npm::NpmSessionView& session, npm::NpmPacketDirection direction,
         std::string bytes, int64_t time, Emitter* emitter) {
    npm::NpmTcpStreamContextV1 context;
    context.session = &session;
    context.direction = direction;
    context.origin = npm::NpmTcpStreamOriginV1::kSyn;
    context.observed_at_ns = time;
    Cursor cursor(std::move(bytes), time);
    const int error = module->OnTcpStreamReadable(context, cursor, *emitter);
    if (!error) assert(cursor.consumed());
    return error;
}

void TestStreamLifecycleAndFailure() {
    auto budget = std::make_shared<Budget>();
    npm::NpmHttp1ProtocolModuleV1 module(Config(), budget);
    auto key = Key(44);
    npm::NpmSessionView session;
    session.session_id = 7;
    session.key = &key;
    Emitter emitter;
    npm::NpmPacketView packet;
    npm::NpmInputEventV1 input;
    input.kind = npm::NpmInputKindV1::kTcpPacket;
    input.session = &session;
    input.transport = &packet;
    const std::string ignored_payload = "GET /not-from-packet HTTP/1.1\r\n\r\n";
    input.body = {reinterpret_cast<const uint8_t*>(ignored_payload.data()), ignored_payload.size()};
    assert(module.OnInput(input, emitter) == 0 && emitter.calls == 0);
    assert(Send(&module, session, npm::NpmPacketDirection::kAToB, "GET / HTTP/1.1\r\n\r\n", 100, &emitter) == 0);
    assert(module.NextEventDeadlineNs() == 5'000'000'100);
    assert(Send(&module, session, npm::NpmPacketDirection::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 200,
                &emitter) == 0);
    assert(emitter.outcomes == (std::vector<std::string>{"matched"}) && emitter.domains[0] == 44);
    assert(!module.NextEventDeadlineNs());
    assert(module.OnSessionEnd(session, npm::NpmSessionEndReason::kEof, 300, emitter) == 0);
    assert(module.Finish(300, emitter) == 0 && budget->used == 0);

    npm::NpmHttp1ProtocolModuleV1 failing(Config(), budget);
    emitter.error = EIO;
    assert(Send(&failing, session, npm::NpmPacketDirection::kAToB, "GET / HTTP/1.1\r\n\r\n", 400, &emitter) == 0);
    assert(Send(&failing, session, npm::NpmPacketDirection::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 500,
                &emitter) == EIO);
    failing.Abort();
    assert(budget->used == 0);
    budget->limit = 1;
    npm::NpmHttp1ProtocolModuleV1 rejected(Config(), budget);
    assert(rejected.OnInput(input, emitter) == ENOSPC);
    rejected.Abort();
    assert(budget->used == 0);

    budget->limit = 2 * 1024 * 1024;
    npm::NpmHttp1ProtocolModuleV1 cancelled(Config(), budget);
    emitter.error = 0;
    assert(Send(&cancelled, session, npm::NpmPacketDirection::kAToB, "GET / HTTP/1.1\r\n\r\n", 600, &emitter) == 0);
    const int prior_calls = emitter.calls;
    cancelled.Abort();
    assert(budget->used == 0 && emitter.calls == prior_calls);
}

}  // namespace

int main() { TestStreamLifecycleAndFailure(); }
