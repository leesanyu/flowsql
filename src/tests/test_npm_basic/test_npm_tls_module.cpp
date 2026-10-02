// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/tls/npm_tls_module.h>

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
    uint64_t limit = 1024 * 1024;
};

class Emitter final : public npm::INpmResultEmitterV1 {
 public:
    int Emit(std::string_view entity, const arrow::RecordBatch& rows) override {
        assert(entity == "tls_handshake" && rows.num_rows() == 1);
        assert(npm::ValidateNpmEntityRowsV1("tls", npm::NpmTlsHandshakeEntityDescriptorV1(), rows).error ==
               npm::NpmProtocolContractErrorV1::kNone);
        ++calls;
        auto ids = std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("entity_instance_id"));
        auto domains = std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("observation_domain_id"));
        auto outcomes = std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("outcome"));
        assert(ids->Value(0) == 7 && domains->Value(0) == 44);
        outcome = outcomes->GetString(0);
        return error;
    }
    int calls = 0;
    int error = 0;
    std::string outcome;
};

class Cursor final : public npm::INpmTcpStreamCursorV1 {
 public:
    explicit Cursor(std::string bytes, int64_t time, uint64_t begin)
        : bytes_(std::move(bytes)), time_(time), begin_(begin) {}
    bool Peek(npm::NpmTcpStreamEventV1* event) const override {
        if (offset_ == bytes_.size()) return false;
        event->kind = npm::NpmTcpStreamEventKindV1::kData;
        event->begin = begin_ + offset_;
        event->end = begin_ + bytes_.size();
        event->bytes = {reinterpret_cast<const uint8_t*>(bytes_.data() + offset_), bytes_.size() - offset_};
        event->captured_at_ns = time_;
        return true;
    }
    int Consume(uint64_t bytes) override {
        if (!bytes || bytes > bytes_.size() - offset_) return EINVAL;
        offset_ += bytes;
        return 0;
    }
    bool consumed() const { return offset_ == bytes_.size(); }

 private:
    std::string bytes_;
    int64_t time_;
    uint64_t begin_;
    size_t offset_ = 0;
};

void U16(std::string* bytes, uint16_t value) {
    bytes->push_back(static_cast<char>(value >> 8));
    bytes->push_back(static_cast<char>(value));
}

std::string Record(bool server) {
    std::string body;
    U16(&body, 0x0303);
    body.append(32, server ? '\x02' : '\x01');
    body += '\0';
    if (server) {
        U16(&body, 0x1301);
        body += '\0';
    } else {
        U16(&body, 2);
        U16(&body, 0x1301);
        body.push_back('\x01');
        body.push_back('\0');
    }
    std::string versions;
    U16(&versions, 0x002b);
    U16(&versions, server ? 2 : 3);
    if (!server) versions += '\x02';
    U16(&versions, 0x0304);
    U16(&body, versions.size());
    body += versions;
    std::string message;
    message += server ? '\x02' : '\x01';
    message += '\0';
    U16(&message, body.size());
    message += body;
    std::string record;
    record.push_back('\x16');
    record.push_back('\x03');
    record.push_back('\x03');
    U16(&record, message.size());
    record += message;
    return record;
}

npm::NpmSessionKey Key() {
    npm::NpmSessionKey key;
    key.ip_family = flowsql::packet::AddressFamily::kIPv4;
    key.transport_protocol = 6;
    flowsql::packet::IPv4Address a{}, b{};
    a.bytes[0] = 192;
    a.bytes[2] = 2;
    a.bytes[3] = 1;
    b.bytes[0] = 198;
    b.bytes[1] = 51;
    b.bytes[2] = 100;
    b.bytes[3] = 2;
    key.a.ip = a;
    key.b.ip = b;
    key.a.port = 50000;
    key.b.port = 443;
    key.observation_domain_id = 44;
    return key;
}

int Send(npm::NpmTlsProtocolModuleV1* module, const npm::NpmSessionView& session, npm::NpmPacketDirection direction,
         std::string bytes, int64_t time, Emitter* emitter, uint64_t begin = 0) {
    npm::NpmTcpStreamContextV1 context;
    context.session = &session;
    context.direction = direction;
    context.origin = npm::NpmTcpStreamOriginV1::kSyn;
    context.observed_at_ns = time;
    Cursor cursor(std::move(bytes), time, begin);
    const int error = module->OnTcpStreamReadable(context, cursor, *emitter);
    if (!error) assert(cursor.consumed());
    return error;
}

void TestModule() {
    auto budget = std::make_shared<Budget>();
    npm::NpmTlsConfigV1 config;
    config.primary_label_ids = {1001};
    auto key = Key();
    npm::NpmSessionView session;
    session.session_id = 7;
    session.key = &key;
    Emitter emitter;
    npm::NpmTlsProtocolModuleV1 module(config, budget);
    npm::NpmInputEventV1 input;
    npm::NpmPacketView packet;
    input.kind = npm::NpmInputKindV1::kTcpPacket;
    input.session = &session;
    input.transport = &packet;
    assert(module.OnInput(input, emitter) == 0 && budget->used == 0);
    const auto client_record = Record(false);
    assert(Send(&module, session, npm::NpmPacketDirection::kAToB, client_record, 100, &emitter) == 0);
    assert(budget->used > 0 && module.NextEventDeadlineNs() == 5'000'000'100);
    const uint64_t retained_charge = budget->used;
    std::string ciphertext;
    for (int index = 0; index < 64; ++index) {
        ciphertext.push_back('\x17');
        ciphertext.push_back('\x03');
        ciphertext.push_back('\x03');
        U16(&ciphertext, 16000);
        ciphertext.append(16000, '\x5a');
    }
    assert(Send(&module, session, npm::NpmPacketDirection::kAToB, ciphertext, 150, &emitter, client_record.size()) ==
           0);
    assert(budget->used == retained_charge && emitter.calls == 0);
    assert(Send(&module, session, npm::NpmPacketDirection::kBToA, Record(true), 200, &emitter) == 0);
    assert(emitter.calls == 1 && emitter.outcome == "server_hello_observed");
    assert(!module.NextEventDeadlineNs() && budget->used == 512);
    assert(module.OnSessionEnd(session, npm::NpmSessionEndReason::kEof, 300, emitter) == 0);
    assert(module.Finish(300, emitter) == 0 && budget->used == 0);

    budget->limit = 1;
    npm::NpmTlsProtocolModuleV1 rejected(config, budget);
    assert(Send(&rejected, session, npm::NpmPacketDirection::kAToB, Record(false), 400, &emitter) == ENOSPC);
    rejected.Abort();
    assert(budget->used == 0);

    budget->limit = 1024 * 1024;
    npm::NpmTlsProtocolModuleV1 failing(config, budget);
    emitter.error = EIO;
    assert(Send(&failing, session, npm::NpmPacketDirection::kAToB, Record(false), 500, &emitter) == 0);
    assert(Send(&failing, session, npm::NpmPacketDirection::kBToA, Record(true), 600, &emitter) == EIO);
    failing.Abort();
    assert(budget->used == 0);

    npm::NpmTlsProtocolModuleV1 cancelled(config, budget);
    emitter.error = 0;
    assert(Send(&cancelled, session, npm::NpmPacketDirection::kAToB, Record(false), 700, &emitter) == 0);
    cancelled.Abort();
    assert(budget->used == 0);

    npm::NpmTlsProtocolModuleV1 non_tls(config, budget);
    const int prior_calls = emitter.calls;
    assert(Send(&non_tls, session, npm::NpmPacketDirection::kAToB, "GET /", 800, &emitter) == 0);
    assert(budget->used > 512);
    assert(Send(&non_tls, session, npm::NpmPacketDirection::kBToA, "GET /", 801, &emitter) == 0);
    assert(budget->used == 512 && emitter.calls == prior_calls);
    assert(Send(&non_tls, session, npm::NpmPacketDirection::kAToB, std::string(1024 * 1024, 'x'), 802, &emitter, 5) ==
           0);
    assert(budget->used == 512 && emitter.calls == prior_calls);
    assert(non_tls.OnSessionEnd(session, npm::NpmSessionEndReason::kEof, 803, emitter) == 0);
    assert(non_tls.Finish(803, emitter) == 0 && budget->used == 0 && emitter.calls == prior_calls);
}

}  // namespace

int main() { TestModule(); }
