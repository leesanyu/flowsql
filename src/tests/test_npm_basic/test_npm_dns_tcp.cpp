// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/core/npm_task_budget.h>
#include <operators/npm_basic/core/npm_tcp_stream_shared.h>
#include <operators/npm_basic/modules/dns/npm_dns_tcp.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace npm = flowsql::npm;
using Direction = npm::NpmPacketDirection;
using Bytes = std::vector<uint8_t>;

namespace {

class QueueCursor final : public npm::INpmTcpStreamCursorV1 {
 public:
    void Data(std::shared_ptr<Bytes> owner, std::optional<int64_t> time) {
        npm::NpmTcpStreamEventV1 event;
        event.kind = npm::NpmTcpStreamEventKindV1::kData;
        event.bytes = {owner->data(), owner->size()};
        event.end = owner->size();
        event.captured_at_ns = time;
        items_.push_back({event, std::move(owner)});
    }
    void Gap(bool capture_truncation = false) {
        npm::NpmTcpStreamEventV1 event;
        event.kind = npm::NpmTcpStreamEventKindV1::kGap;
        event.includes_capture_truncation = capture_truncation;
        event.gap_reason = npm::NpmTcpStreamGapReasonV1::kWaitExpired;
        items_.push_back({event, {}});
    }
    void End(npm::NpmTcpStreamEndReasonV1 reason) {
        npm::NpmTcpStreamEventV1 event;
        event.kind = npm::NpmTcpStreamEventKindV1::kEnd;
        event.end_reason = reason;
        items_.push_back({event, {}});
    }
    bool Peek(npm::NpmTcpStreamEventV1* event) const override {
        if (!event || index_ == items_.size()) return false;
        *event = items_[index_].event;
        return true;
    }
    int Consume(uint64_t bytes) override {
        if (index_ == items_.size()) return EINVAL;
        const auto& event = items_[index_].event;
        if ((event.kind == npm::NpmTcpStreamEventKindV1::kData && bytes != event.bytes.size) ||
            (event.kind != npm::NpmTcpStreamEventKindV1::kData && bytes != 0))
            return EINVAL;
        items_[index_].owner.reset();
        ++index_;
        return 0;
    }
    bool Empty() const { return index_ == items_.size(); }

 private:
    struct Item {
        npm::NpmTcpStreamEventV1 event;
        std::shared_ptr<Bytes> owner;
    };
    std::vector<Item> items_;
    size_t index_ = 0;
};

Bytes Message(uint16_t id, bool response = false, char name = 'a') {
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
            static_cast<uint8_t>(name),
            0,
            0,
            1,
            0,
            1};
}

Bytes Frame(const Bytes& message) {
    Bytes frame = {static_cast<uint8_t>(message.size() >> 8), static_cast<uint8_t>(message.size())};
    frame.insert(frame.end(), message.begin(), message.end());
    return frame;
}

npm::NpmTcpStreamContextV1 Context(npm::NpmSessionView* session, Direction direction, npm::NpmTcpStreamOriginV1 origin,
                                   int64_t observed_at_ns, bool final_drain = false) {
    npm::NpmTcpStreamContextV1 context;
    context.session = session;
    context.direction = direction;
    context.origin = origin;
    context.observed_at_ns = observed_at_ns;
    context.final_drain = final_drain;
    return context;
}

void Deliver(npm::NpmDnsTcpFramerV1* framer, npm::NpmSessionView* session, Direction direction,
             npm::NpmTcpStreamOriginV1 origin, int64_t observed_at_ns, QueueCursor* cursor,
             std::vector<npm::NpmDnsUdpResultV1>* rows, bool final_drain = false) {
    assert(framer->OnReadable(Context(session, direction, origin, observed_at_ns, final_drain), *cursor, rows) == 0);
    assert(cursor->Empty());
}

void TestSplitMultipleFramesAndBorrowedOwners() {
    npm::NpmDnsUdpTrackerV1 transactions({{1001}, 5'000'000'000, 256});
    npm::NpmDnsTcpFramerV1 framer(&transactions);
    npm::NpmSessionView session;
    session.session_id = 7;
    std::vector<npm::NpmDnsUdpResultV1> rows;
    const auto query = Frame(Message(3));
    auto first = std::make_shared<Bytes>(query.begin(), query.begin() + 1);
    std::weak_ptr<Bytes> first_weak = first;
    QueueCursor cursor;
    cursor.Data(first, 100);
    first.reset();
    Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 100, &cursor, &rows);
    assert(first_weak.expired() && transactions.PendingCount(7) == 0);
    cursor = QueueCursor{};
    auto second = std::make_shared<Bytes>(query.begin() + 1, query.begin() + 5);
    std::weak_ptr<Bytes> second_weak = second;
    cursor.Data(second, 110);
    second.reset();
    Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 110, &cursor, &rows);
    assert(second_weak.expired() && rows.empty());
    cursor = QueueCursor{};
    auto rest = std::make_shared<Bytes>(query.begin() + 5, query.end());
    std::weak_ptr<Bytes> rest_weak = rest;
    cursor.Data(rest, 120);
    rest.reset();
    Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 120, &cursor, &rows);
    assert(rest_weak.expired() && transactions.PendingCount(7) == 1);
    const auto response = Frame(Message(3, true));
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(response), 200);
    Deliver(&framer, &session, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, 200, &cursor, &rows);
    assert(rows.size() == 1 && rows[0].outcome == "matched" && rows[0].query_at_ns == 120 &&
           rows[0].response_at_ns == 200 && rows[0].latency_ns == 80);
    assert(framer.ActiveDirections() == 2);

    auto two_queries = Frame(Message(4));
    auto other = Frame(Message(5));
    two_queries.insert(two_queries.end(), other.begin(), other.end());
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(two_queries), 210);
    Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 210, &cursor, &rows);
    assert(transactions.PendingCount(7) == 2);
    auto two_responses = Frame(Message(4, true));
    other = Frame(Message(5, true));
    two_responses.insert(two_responses.end(), other.begin(), other.end());
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(two_responses), 230);
    Deliver(&framer, &session, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, 230, &cursor, &rows);
    assert(rows.size() == 3 && rows[1].dns_id == 4 && rows[2].dns_id == 5);
    assert(rows[1].latency_ns == 20 && rows[2].latency_ns == 20);
}

void TestSegmentationInvariantAndMissingTime() {
    const auto run = [](bool split) {
        npm::NpmDnsUdpTrackerV1 transactions({{1001}, 5'000'000'000, 256});
        npm::NpmDnsTcpFramerV1 framer(&transactions);
        npm::NpmSessionView session;
        session.session_id = 8;
        std::vector<npm::NpmDnsUdpResultV1> rows;
        const auto query = Frame(Message(17));
        const auto response = Frame(Message(17, true));
        if (split) {
            for (size_t position = 0; position < query.size(); position += 3) {
                QueueCursor cursor;
                cursor.Data(std::make_shared<Bytes>(query.begin() + position,
                                                    query.begin() + std::min(query.size(), position + 3)),
                            100);
                Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 100, &cursor, &rows);
            }
            for (size_t position = 0; position < response.size(); position += 4) {
                QueueCursor cursor;
                cursor.Data(std::make_shared<Bytes>(response.begin() + position,
                                                    response.begin() + std::min(response.size(), position + 4)),
                            160);
                Deliver(&framer, &session, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, 160, &cursor, &rows);
            }
        } else {
            QueueCursor cursor;
            cursor.Data(std::make_shared<Bytes>(query), 100);
            Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 100, &cursor, &rows);
            cursor = QueueCursor{};
            cursor.Data(std::make_shared<Bytes>(response), 160);
            Deliver(&framer, &session, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, 160, &cursor, &rows);
        }
        return rows;
    };
    const auto whole = run(false);
    const auto sliced = run(true);
    assert(whole.size() == 1 && sliced.size() == 1);
    assert(whole[0].entity_instance_id == sliced[0].entity_instance_id);
    assert(whole[0].outcome == sliced[0].outcome && whole[0].latency_ns == sliced[0].latency_ns);
    assert(whole[0].qname == sliced[0].qname && whole[0].dns_id == sliced[0].dns_id);

    npm::NpmDnsUdpTrackerV1 transactions({{1001}, 5'000'000'000, 256});
    npm::NpmDnsTcpFramerV1 framer(&transactions);
    npm::NpmSessionView session;
    session.session_id = 9;
    std::vector<npm::NpmDnsUdpResultV1> rows;
    QueueCursor cursor;
    const auto query = Frame(Message(18));
    cursor.Data(std::make_shared<Bytes>(query.begin(), query.begin() + 1), std::nullopt);
    Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 50, &cursor, &rows);
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(query.begin() + 1, query.end()), 100);
    Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 100, &cursor, &rows);
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(18, true))), 150);
    Deliver(&framer, &session, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, 150, &cursor, &rows);
    assert(rows.size() == 1 && rows[0].outcome == "matched" && !rows[0].query_at_ns && !rows[0].latency_ns &&
           rows[0].response_at_ns == 150);
}

void TestGapMidstreamEndAndSessionLifecycle() {
    npm::NpmDnsUdpTrackerV1 transactions({{1001}, 5'000'000'000, 256});
    npm::NpmDnsTcpFramerV1 framer(&transactions);
    npm::NpmSessionView first;
    first.session_id = 10;
    std::vector<npm::NpmDnsUdpResultV1> rows;
    QueueCursor cursor;
    cursor.Data(std::make_shared<Bytes>(Frame(Message(1))), 100);
    Deliver(&framer, &first, Direction::kAToB, npm::NpmTcpStreamOriginV1::kMidstream, 100, &cursor, &rows);
    assert(rows.empty() && transactions.PendingCount(10) == 0);
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(2, true))), 110);
    Deliver(&framer, &first, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, 110, &cursor, &rows);
    assert(rows.size() == 1 && rows[0].outcome == "response_only");

    npm::NpmSessionView second;
    second.session_id = 11;
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(3))), 120);
    Deliver(&framer, &second, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 120, &cursor, &rows);
    assert(transactions.PendingCount(11) == 1);
    cursor = QueueCursor{};
    cursor.End(npm::NpmTcpStreamEndReasonV1::kFin);
    Deliver(&framer, &second, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 125, &cursor, &rows, true);
    assert(transactions.PendingCount(11) == 1 && rows.size() == 1);
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(3, true))), 130);
    Deliver(&framer, &second, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, 130, &cursor, &rows);
    assert(rows.size() == 2 && rows.back().outcome == "matched" && rows.back().latency_ns == 10);
    assert(framer.OnSessionEnd(11, 140, &rows) == 0 && rows.size() == 2);
    assert(framer.OnSessionEnd(11, 141, &rows) == 0 && rows.size() == 2);

    npm::NpmSessionView third;
    third.session_id = 12;
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(4))), 150);
    Deliver(&framer, &third, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 150, &cursor, &rows);
    cursor = QueueCursor{};
    auto partial = Frame(Message(4, true));
    cursor.Data(std::make_shared<Bytes>(partial.begin(), partial.begin() + 5), 160);
    cursor.Gap(true);
    cursor.Data(std::make_shared<Bytes>(partial), 170);
    cursor.End(npm::NpmTcpStreamEndReasonV1::kReset);
    Deliver(&framer, &third, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, 170, &cursor, &rows, true);
    assert(rows.size() == 3 && rows.back().outcome == "query_only" &&
           rows.back().incomplete_reason == "response_path_gap");
    assert(transactions.PendingCount(12) == 0);
    assert(framer.OnSessionEnd(12, 180, &rows) == 0 && rows.size() == 3);

    npm::NpmSessionView fourth;
    fourth.session_id = 13;
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(5))), 200);
    Deliver(&framer, &fourth, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 200, &cursor, &rows);
    cursor = QueueCursor{};
    cursor.End(npm::NpmTcpStreamEndReasonV1::kSessionEnd);
    Deliver(&framer, &fourth, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 210, &cursor, &rows, true);
    assert(transactions.PendingCount(13) == 1 && rows.size() == 3);
    assert(framer.OnSessionEnd(13, 220, &rows) == 0);
    assert(rows.size() == 4 && rows.back().outcome == "query_only" && rows.back().incomplete_reason == "session_end");
    assert(framer.OnSessionEnd(13, 230, &rows) == 0 && rows.size() == 4);

    npm::NpmSessionView reuse;
    reuse.session_id = 14;
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(5, true))), 240);
    Deliver(&framer, &reuse, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, 240, &cursor, &rows);
    assert(rows.back().outcome == "response_only" && !rows.back().latency_ns);

    npm::NpmSessionView bidirectional;
    bidirectional.session_id = 15;
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(6))), 250);
    Deliver(&framer, &bidirectional, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 250, &cursor, &rows);
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(7))), 260);
    cursor.Gap();
    cursor.End(npm::NpmTcpStreamEndReasonV1::kReset);
    Deliver(&framer, &bidirectional, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, 270, &cursor, &rows, true);
    assert(rows.back().outcome == "query_only" && rows.back().dns_id == 6 &&
           rows.back().incomplete_reason == "response_path_gap");
    assert(transactions.PendingCount(15) == 1);
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(7, true))), 280);
    Deliver(&framer, &bidirectional, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 280, &cursor, &rows);
    assert(rows.back().outcome == "matched" && rows.back().dns_id == 7);
    assert(framer.OnSessionEnd(15, 290, &rows) == 0 && transactions.PendingCount(15) == 0);
}

void TestInvalidLengthNeverResynchronizes() {
    npm::NpmDnsUdpTrackerV1 transactions({{1001}, 5'000'000'000, 256});
    npm::NpmDnsTcpFramerV1 framer(&transactions);
    npm::NpmSessionView session;
    session.session_id = 20;
    std::vector<npm::NpmDnsUdpResultV1> rows;
    Bytes input = {0, 0};
    const auto valid = Frame(Message(1));
    input.insert(input.end(), valid.begin(), valid.end());
    QueueCursor cursor;
    cursor.Data(std::make_shared<Bytes>(input), 100);
    Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 100, &cursor, &rows);
    assert(rows.empty() && transactions.PendingCount(20) == 0);
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(valid), 110);
    Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 110, &cursor, &rows);
    assert(transactions.PendingCount(20) == 0);
}

class StreamBridge final : public npm::INpmTcpStreamConsumerV1 {
 public:
    StreamBridge(npm::NpmDnsTcpFramerV1* framer, std::vector<npm::NpmDnsUdpResultV1>* rows)
        : framer_(framer), rows_(rows) {}
    int OnTcpStreamReadable(const npm::NpmTcpStreamContextV1& context, npm::INpmTcpStreamCursorV1& cursor,
                            npm::INpmResultEmitterV1&) override {
        return framer_->OnReadable(context, cursor, rows_);
    }

 private:
    npm::NpmDnsTcpFramerV1* framer_;
    std::vector<npm::NpmDnsUdpResultV1>* rows_;
};

class UnusedEmitter final : public npm::INpmResultEmitterV1 {
 public:
    int Emit(std::string_view, const arrow::RecordBatch&) override { return EINVAL; }
};

void TestActualSharedStreamAcrossPushes() {
    npm::NpmDnsUdpTrackerV1 transactions({{1001}, 5'000'000'000, 256});
    npm::NpmDnsTcpFramerV1 framer(&transactions);
    std::vector<npm::NpmDnsUdpResultV1> rows;
    StreamBridge bridge(&framer, &rows);
    UnusedEmitter emitter;
    auto budget = std::make_shared<npm::NpmTaskBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmSessionView session;
    session.session_id = 30;
    npm::NpmTcpStreamSubscription subscription{"dns", &bridge, &emitter};
    const auto push = [&](npm::NpmTcpStreamSharedDirection* stream, Direction direction, uint32_t sequence,
                          const Bytes& data, bool syn, int64_t time, bool fin = false) {
        npm::NpmPacketView packet;
        packet.direction = direction;
        packet.payload = {data.data(), data.size()};
        packet.packet.meta.timestamp_ns = time;
        packet.transport.payload_captured_bytes = data.size();
        packet.transport.payload_wire_bytes = data.size();
        packet.transport.payload_complete = true;
        packet.transport.tcp.valid = true;
        packet.transport.tcp.sequence = sequence;
        packet.transport.tcp.syn = syn;
        packet.transport.tcp.fin = fin;
        assert(stream->Push(session, packet) == npm::NpmTcpStreamError::kNone);
    };
    std::unique_ptr<npm::NpmTcpStreamSharedDirection> request;
    assert(npm::NpmTcpStreamSharedDirection::Create({}, budget, 30, Direction::kAToB, {&subscription, 1}, &request) ==
           npm::NpmTcpStreamError::kNone);
    push(request.get(), Direction::kAToB, 100, {}, true, 90);
    const auto query = Frame(Message(31));
    Bytes first(query.begin(), query.begin() + 5);
    push(request.get(), Direction::kAToB, 101, first, false, 100);
    first.assign(first.size(), 0xff);
    assert(transactions.PendingCount(30) == 0);
    Bytes second(query.begin() + 5, query.end());
    push(request.get(), Direction::kAToB, 106, second, false, 120);
    second.clear();
    assert(transactions.PendingCount(30) == 1);
    push(request.get(), Direction::kAToB, static_cast<uint32_t>(101 + query.size()), {}, false, 125, true);
    assert(request->Ended());

    std::unique_ptr<npm::NpmTcpStreamSharedDirection> response;
    assert(npm::NpmTcpStreamSharedDirection::Create({}, budget, 30, Direction::kBToA, {&subscription, 1}, &response) ==
           npm::NpmTcpStreamError::kNone);
    push(response.get(), Direction::kBToA, 200, {}, true, 130);
    const auto answer = Frame(Message(31, true));
    push(response.get(), Direction::kBToA, 201, answer, false, 180);
    assert(rows.size() == 1 && rows[0].outcome == "matched" && rows[0].query_at_ns == 120 &&
           rows[0].response_at_ns == 180 && rows[0].latency_ns == 60);
    assert(response->End(session, npm::NpmTcpStreamEndReasonV1::kSessionEnd, npm::NpmSessionEndReason::kEof, 190) ==
           npm::NpmTcpStreamError::kNone);
    assert(framer.OnSessionEnd(30, 190, &rows) == 0 && rows.size() == 1);
    request.reset();
    response.reset();
    assert(budget->Usage().module_state_bytes == 0);
}

void TestFrameBudgetFailureAndRelease() {
    class Budget final : public npm::INpmTaskBudget {
     public:
        npm::NpmBudgetError Reserve(npm::NpmBudgetCategory, uint64_t bytes) override {
            if (used + bytes > limit) return npm::NpmBudgetError::kTrackedLimitExceeded;
            used += bytes;
            return npm::NpmBudgetError::kNone;
        }
        npm::NpmBudgetError Release(npm::NpmBudgetCategory, uint64_t bytes) override {
            assert(bytes <= used);
            used -= bytes;
            return npm::NpmBudgetError::kNone;
        }
        npm::NpmBudgetUsage Usage() const override { return {0, used, 0, 0}; }
        uint64_t used = 0;
        uint64_t limit = 256;
    };
    auto budget = std::make_shared<Budget>();
    npm::NpmDnsUdpTrackerV1 transactions({{1001}, 5'000'000'000, 256});
    npm::NpmDnsTcpFramerV1 framer(&transactions, budget);
    npm::NpmSessionView session;
    session.session_id = 31;
    std::vector<npm::NpmDnsUdpResultV1> rows;
    QueueCursor cursor;
    cursor.Data(std::make_shared<Bytes>(Frame(Message(41))), 100);
    assert(framer.OnReadable(Context(&session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 100), cursor,
                             &rows) == ENOSPC);
    assert(budget->used == 256);
    framer.Abort();
    assert(budget->used == 0);
    budget->limit = 8192;
    cursor = QueueCursor{};
    cursor.Data(std::make_shared<Bytes>(Frame(Message(42))), 110);
    Deliver(&framer, &session, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, 110, &cursor, &rows);
    assert(budget->used == 256);
    assert(framer.OnSessionEnd(31, 120, &rows) == 0 && budget->used == 0);
    assert(rows.size() == 1 && rows[0].incomplete_reason == "session_end");
}

}  // namespace

int main() {
    TestSplitMultipleFramesAndBorrowedOwners();
    TestSegmentationInvariantAndMissingTime();
    TestGapMidstreamEndAndSessionLifecycle();
    TestInvalidLengthNeverResynchronizes();
    TestActualSharedStreamAcrossPushes();
    TestFrameBudgetFailureAndRelease();
    std::puts("NPM DNS TCP tests passed");
    return 0;
}
