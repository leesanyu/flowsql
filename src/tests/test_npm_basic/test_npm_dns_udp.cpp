// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/dns/npm_dns_udp.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace npm = flowsql::npm;
using Direction = npm::NpmPacketDirection;
using Bytes = std::vector<uint8_t>;

namespace {

void Put16(Bytes* bytes, uint16_t value) {
    bytes->push_back(static_cast<uint8_t>(value >> 8));
    bytes->push_back(static_cast<uint8_t>(value));
}

void Put32(Bytes* bytes, uint32_t value) {
    Put16(bytes, static_cast<uint16_t>(value >> 16));
    Put16(bytes, static_cast<uint16_t>(value));
}

Bytes Message(uint16_t id, uint16_t flags, std::string name = "A", uint16_t qtype = 1, uint16_t qclass = 1,
              uint16_t qdcount = 1) {
    Bytes bytes;
    Put16(&bytes, id);
    Put16(&bytes, flags);
    Put16(&bytes, qdcount);
    Put16(&bytes, 0);
    Put16(&bytes, 0);
    Put16(&bytes, 0);
    bytes.push_back(static_cast<uint8_t>(name.size()));
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.push_back(0);
    Put16(&bytes, qtype);
    Put16(&bytes, qclass);
    return bytes;
}

Bytes Response(uint16_t id, uint16_t flags = 0x8000, std::string name = "a", uint16_t qtype = 1) {
    return Message(id, flags, std::move(name), qtype);
}

void AddOpt(Bytes* bytes, uint8_t extended_rcode, uint8_t version = 0) {
    (*bytes)[11] = 1;
    bytes->push_back(0);
    Put16(bytes, 41);
    Put16(bytes, 4096);
    Put32(bytes, (static_cast<uint32_t>(extended_rcode) << 24) | (static_cast<uint32_t>(version) << 16));
    Put16(bytes, 0);
}

void AddAnswer(Bytes* bytes) {
    (*bytes)[7] = 1;
    bytes->push_back(0xc0);
    bytes->push_back(0x0c);
    Put16(bytes, 1);
    Put16(bytes, 1);
    Put32(bytes, 60);
    Put16(bytes, 4);
    bytes->insert(bytes->end(), {192, 0, 2, 1});
}

flowsql::Span<const uint8_t> View(const Bytes& bytes) { return {bytes.data(), bytes.size()}; }

int Send(npm::NpmDnsUdpTrackerV1* tracker, uint64_t session, Direction direction, const Bytes& bytes, int64_t time,
         std::vector<npm::NpmDnsUdpResultV1>* rows, bool complete = true) {
    return tracker->OnDatagram(session, direction, View(bytes), complete, time, rows);
}

void TestMatchedRcodesAndIsolation() {
    npm::NpmDnsUdpTrackerV1 tracker({{1001}, 5'000'000'000, 256});
    std::vector<npm::NpmDnsUdpResultV1> rows;
    assert(Send(&tracker, 1, Direction::kAToB, Message(7, 0), 100, &rows) == 0);
    assert(Send(&tracker, 2, Direction::kAToB, Message(7, 0), 110, &rows) == 0);
    assert(Send(&tracker, 1, Direction::kBToA, Message(7, 0, "other"), 120, &rows) == 0);
    assert(tracker.PendingCount(1) == 2 && tracker.PendingCount(2) == 1 && rows.empty());
    assert(Send(&tracker, 1, Direction::kBToA, Response(7, 0x8003), 150, &rows) == 0);
    assert(rows.size() == 1 && rows[0].outcome == "matched" && rows[0].response_rcode == 3);
    assert(rows[0].query_at_ns == 100 && rows[0].response_at_ns == 150 && rows[0].latency_ns == 50);
    assert(rows[0].qname == "A" && rows[0].query_direction == Direction::kAToB);
    assert(tracker.PendingCount(1) == 1 && tracker.PendingCount(2) == 1);
    assert(Send(&tracker, 2, Direction::kBToA, Response(7, 0x8002), 170, &rows) == 0);
    assert(rows.size() == 2 && rows[1].response_rcode == 2 && rows[1].session_id == 2);
    assert(rows[0].entity_instance_id != rows[1].entity_instance_id);
    auto edns = Response(7, 0x8203, "other");
    AddOpt(&edns, 1);
    assert(Send(&tracker, 1, Direction::kBToA, edns, 180, &rows) == 0);
    assert(rows.size() == 3 && rows.back().outcome == "response_only");
    assert(!rows.back().latency_ns && !rows.back().query_direction);
    assert(Send(&tracker, 1, Direction::kAToB, Message(7, 0, "other"), 190, &rows) == 0);
    assert(tracker.PendingCount(1) == 2);
    assert(Send(&tracker, 1, Direction::kBToA, edns, 200, &rows) == 0);
    assert(rows.back().outcome == "truncated_response" && rows.back().incomplete_reason == "dns_tc_set");
    assert(rows.back().response_rcode == 19 && rows.back().response_tc == true && rows.back().latency_ns == 10);
    assert(tracker.PendingCount(1) == 1);
}

void TestRetriesOrphansAndEnd() {
    npm::NpmDnsUdpTrackerV1 tracker({{1001}, 5'000'000'000, 2});
    std::vector<npm::NpmDnsUdpResultV1> rows;
    assert(Send(&tracker, 5, Direction::kAToB, Message(42, 0, "A"), 100, &rows) == 0);
    assert(Send(&tracker, 5, Direction::kAToB, Message(42, 0, "a"), 130, &rows) == 0);
    assert(tracker.PendingCount(5) == 1);
    assert(Send(&tracker, 5, Direction::kAToB, Message(42, 0, "b"), 140, &rows) == 0);
    assert(tracker.PendingCount(5) == 2);
    assert(Send(&tracker, 5, Direction::kAToB, Message(42, 0, "c"), 145, &rows) == EOVERFLOW);
    assert(tracker.PendingCount(5) == 2 && rows.empty());
    assert(Send(&tracker, 5, Direction::kAToB, Response(42), 150, &rows) == 0);
    assert(rows.size() == 1 && rows.back().outcome == "response_only" && !rows.back().latency_ns);
    assert(Send(&tracker, 5, Direction::kBToA, Response(42), 160, &rows) == 0);
    assert(rows.size() == 2 && rows.back().outcome == "matched" && rows.back().query_retries == 1);
    assert(rows.back().query_at_ns == 100 && rows.back().latency_ns == 60);
    assert(Send(&tracker, 5, Direction::kBToA, Response(42), 170, &rows) == 0);
    assert(rows.back().outcome == "response_only" && !rows.back().query_at_ns);
    assert(tracker.OnSessionEnd(5, 300, &rows) == 0);
    assert(rows.back().outcome == "query_only" && rows.back().incomplete_reason == "session_end");
    assert(rows.back().qname == "b" && rows.back().observed_at_ns == 300);
    assert(tracker.PendingCount(5) == 0);
    assert(tracker.OnSessionEnd(5, 400, &rows) == 0 && rows.size() == 4);
    assert(Send(&tracker, 5, Direction::kBToA, Response(42, 0x8000, "b"), 500, &rows) == 0);
    assert(rows.back().outcome == "response_only");
    assert(Send(&tracker, 6, Direction::kAToB, Message(43, 0), 200, &rows) == 0);
    assert(Send(&tracker, 6, Direction::kBToA, Response(43), 190, &rows) == 0);
    assert(rows.back().outcome == "matched" && !rows.back().latency_ns);
}

void TestMalformedTruncatedAndUnsupported() {
    npm::NpmDnsUdpTrackerV1 tracker({{1001}, 5'000'000'000, 256});
    std::vector<npm::NpmDnsUdpResultV1> rows;
    auto query = Message(9, 0);
    auto response = Response(9);
    npm::NpmDnsParsedMessageV1 parsed;
    for (size_t length = 0; length < 12; ++length) {
        Bytes short_body(response.begin(), response.begin() + length);
        assert(npm::ParseNpmDnsUdpMessageV1(View(short_body), true, &parsed) == npm::NpmDnsMessageStatusV1::kMalformed);
        assert(!parsed.key_available);
    }
    auto bad_count = Message(9, 0x8000, "a", 1, 1, 2);
    assert(npm::ParseNpmDnsUdpMessageV1(View(bad_count), true, &parsed) == npm::NpmDnsMessageStatusV1::kMalformed);
    assert(!parsed.key_available);
    auto pointer_loop = response;
    pointer_loop[12] = 0xc0;
    pointer_loop[13] = 0x0c;
    assert(npm::ParseNpmDnsUdpMessageV1(View(pointer_loop), true, &parsed) == npm::NpmDnsMessageStatusV1::kMalformed);
    assert(!parsed.key_available);
    auto pointer_outside = response;
    pointer_outside[12] = 0xff;
    pointer_outside[13] = 0xff;
    assert(npm::ParseNpmDnsUdpMessageV1(View(pointer_outside), true, &parsed) ==
           npm::NpmDnsMessageStatusV1::kMalformed);
    assert(!parsed.key_available);
    assert(Send(&tracker, 9, Direction::kAToB, query, 100, &rows) == 0);
    auto clipped = response;
    clipped.pop_back();
    assert(Send(&tracker, 9, Direction::kBToA, clipped, 150, &rows, false) == 0);
    assert(rows.empty() && tracker.PendingCount(9) == 1);
    assert(Send(&tracker, 9, Direction::kBToA, response, 160, &rows, false) == 0);
    assert(rows.size() == 1 && rows.back().outcome == "query_only" &&
           rows.back().incomplete_reason == "capture_truncation");
    assert(!rows.back().response_at_ns && tracker.PendingCount(9) == 0);
    assert(Send(&tracker, 9, Direction::kAToB, query, 200, &rows) == 0);
    auto malformed = response;
    AddAnswer(&malformed);
    malformed.pop_back();
    assert(Send(&tracker, 9, Direction::kBToA, malformed, 250, &rows) == 0);
    assert(rows.back().outcome == "query_only" && rows.back().incomplete_reason == "malformed_response");
    assert(tracker.PendingCount(9) == 0);
    assert(Send(&tracker, 9, Direction::kAToB, query, 300, &rows) == 0);
    assert(Send(&tracker, 9, Direction::kBToA, pointer_loop, 350, &rows) == 0);
    assert(tracker.PendingCount(9) == 1);
    auto unsupported = Message(10, 0x0800);
    assert(Send(&tracker, 9, Direction::kAToB, unsupported, 360, &rows) == 0);
    assert(rows.back().outcome == "unsupported_message" &&
           rows.back().incomplete_reason == "unsupported_opcode_or_transfer");
    assert(Send(&tracker, 9, Direction::kAToB, Message(11, 0, "a", 252), 370, &rows) == 0);
    assert(rows.back().outcome == "unsupported_message" && tracker.PendingCount(9) == 1);
    tracker.Abort();
    assert(tracker.PendingCount(9) == 0);
}

void TestNamesSectionsAndOptBounds() {
    npm::NpmDnsParsedMessageV1 parsed;
    auto valid = Response(21);
    AddAnswer(&valid);
    assert(npm::ParseNpmDnsUdpMessageV1(View(valid), true, &parsed) == npm::NpmDnsMessageStatusV1::kComplete);
    assert(parsed.key_available && parsed.message.wire_qname == std::string("\1a\0", 3));
    auto bad_rr = valid;
    bad_rr.pop_back();
    assert(npm::ParseNpmDnsUdpMessageV1(View(bad_rr), true, &parsed) == npm::NpmDnsMessageStatusV1::kMalformed);
    assert(parsed.key_available);
    auto bad_opt = Response(22);
    AddOpt(&bad_opt, 1, 1);
    assert(npm::ParseNpmDnsUdpMessageV1(View(bad_opt), true, &parsed) == npm::NpmDnsMessageStatusV1::kMalformed);
    assert(parsed.key_available);
    auto duplicate_opt = Response(23);
    AddOpt(&duplicate_opt, 1);
    duplicate_opt[11] = 2;
    const Bytes opt(duplicate_opt.begin() + Response(23).size(), duplicate_opt.end());
    duplicate_opt.insert(duplicate_opt.end(), opt.begin(), opt.end());
    assert(npm::ParseNpmDnsUdpMessageV1(View(duplicate_opt), true, &parsed) == npm::NpmDnsMessageStatusV1::kMalformed);
    auto escaped = Message(24, 0, "a.b");
    assert(npm::ParseNpmDnsUdpMessageV1(View(escaped), true, &parsed) == npm::NpmDnsMessageStatusV1::kComplete);
    assert(parsed.qname_display == "a\\.b");
    auto long_name = Message(25, 0);
    long_name.resize(12);
    for (const size_t length : {63U, 63U, 63U, 61U}) {
        long_name.push_back(static_cast<uint8_t>(length));
        long_name.insert(long_name.end(), length, 'x');
    }
    long_name.push_back(0);
    Put16(&long_name, 1);
    Put16(&long_name, 1);
    assert(npm::ParseNpmDnsUdpMessageV1(View(long_name), true, &parsed) == npm::NpmDnsMessageStatusV1::kComplete);
    long_name.insert(long_name.end() - 5, {'y', 'z'});
    long_name[12 + 3 * 64] = 63;
    assert(npm::ParseNpmDnsUdpMessageV1(View(long_name), true, &parsed) == npm::NpmDnsMessageStatusV1::kMalformed);
    Bytes oversized(65536, 0);
    assert(npm::ParseNpmDnsUdpMessageV1(View(oversized), true, &parsed) == npm::NpmDnsMessageStatusV1::kMalformed);
}

void TestCaptureWatermarkDeadlines() {
    npm::NpmDnsUdpTrackerV1 tracker({{1001}, 1'000'000, 3});
    std::vector<npm::NpmDnsUdpResultV1> rows;
    assert(!tracker.NextEventDeadlineNs());
    assert(Send(&tracker, 50, Direction::kAToB, Message(1, 0), 100, &rows) == 0);
    assert(tracker.NextEventDeadlineNs() == 1'000'100);
    assert(tracker.OnCaptureWatermark(1'000'099, &rows) == 0 && rows.empty());
    assert(tracker.OnCaptureWatermark(1'000'099, &rows) == 0 && rows.empty());
    assert(tracker.OnCaptureWatermark(50, &rows) == 0 && rows.empty());
    assert(tracker.OnCaptureWatermark(1'000'100, &rows) == 0);
    assert(rows.size() == 1 && rows[0].outcome == "query_only" &&
           rows[0].incomplete_reason == "response_not_observed_by_deadline" && rows[0].observed_at_ns == 1'000'100);
    assert(!tracker.NextEventDeadlineNs() && tracker.PendingCount(50) == 0);
    assert(tracker.OnCaptureWatermark(1'000'100, &rows) == 0 && rows.size() == 1);

    assert(Send(&tracker, 51, Direction::kAToB, Message(2, 0), INT64_MAX - 1, &rows) == 0);
    assert(tracker.NextEventDeadlineNs() == INT64_MAX);
    assert(tracker.OnCaptureWatermark(INT64_MAX - 1, &rows) == 0 && rows.size() == 1);
    assert(tracker.OnCaptureWatermark(INT64_MAX, &rows) == 0 && rows.size() == 2);
    assert(rows.back().dns_id == 2 && rows.back().observed_at_ns == INT64_MAX);
    assert(!tracker.NextEventDeadlineNs());

    assert(tracker.OnMessage(52, Direction::kAToB, View(Message(3, 0)), true, std::nullopt, 200, &rows) == 0);
    assert(!tracker.NextEventDeadlineNs() && tracker.PendingCount(52) == 1);
    assert(tracker.OnSessionEnd(52, 300, &rows) == 0 && rows.back().incomplete_reason == "session_end");
    tracker.Abort();
    assert(!tracker.NextEventDeadlineNs());
}

void TestPendingBudgetRelease() {
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
        uint64_t limit = 4096;
    };
    auto budget = std::make_shared<Budget>();
    npm::NpmDnsUdpTrackerV1 tracker({{1001}, 5'000'000'000, 2}, budget);
    std::vector<npm::NpmDnsUdpResultV1> rows;
    assert(Send(&tracker, 60, Direction::kAToB, Message(1, 0), 100, &rows) == 0);
    assert(budget->Usage().module_state_bytes == 4096);
    assert(Send(&tracker, 60, Direction::kAToB, Message(2, 0), 110, &rows) == ENOSPC);
    assert(tracker.PendingCount(60) == 1 && budget->Usage().module_state_bytes == 4096);
    budget->limit = 8192;
    assert(Send(&tracker, 60, Direction::kBToA, Response(1), 120, &rows) == 0);
    assert(rows.size() == 1 && rows[0].outcome == "matched" && budget->Usage().module_state_bytes == 4096);
    tracker.ReleaseResults(&rows);
    assert(budget->Usage().module_state_bytes == 0);
    assert(Send(&tracker, 61, Direction::kAToB, Message(3, 0), 130, &rows) == 0);
    tracker.Abort();
    assert(budget->Usage().module_state_bytes == 0);
}

}  // namespace

int main() {
    TestMatchedRcodesAndIsolation();
    TestRetriesOrphansAndEnd();
    TestMalformedTruncatedAndUnsupported();
    TestNamesSectionsAndOptBounds();
    TestCaptureWatermarkDeadlines();
    TestPendingBudgetRelease();
    std::puts("NPM DNS UDP tests passed");
    return 0;
}
