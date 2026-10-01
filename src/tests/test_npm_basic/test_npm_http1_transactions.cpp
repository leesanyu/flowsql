// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/http1/npm_http1_transactions.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace npm = flowsql::npm;
using Direction = npm::NpmPacketDirection;
using Outcome = npm::NpmHttp1OutcomeV1;
using Reason = npm::NpmHttp1IncompleteReasonV1;

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

struct Fixture {
    Fixture() : tracker(Config()) {}
    static npm::NpmHttp1ConfigV1 Config() {
        npm::NpmHttp1ConfigV1 config;
        config.primary_label_ids = {1001};
        return config;
    }
    void Data(uint64_t session_id, Direction direction, std::string_view bytes, std::optional<int64_t> capture_ns,
              npm::NpmTcpStreamOriginV1 origin = npm::NpmTcpStreamOriginV1::kSyn) {
        npm::NpmTcpStreamEventV1 event;
        event.kind = npm::NpmTcpStreamEventKindV1::kData;
        event.bytes = {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
        event.captured_at_ns = capture_ns;
        assert(tracker.OnStreamEvent(session_id, direction, origin, event, capture_ns.value_or(0), &rows) == 0);
    }
    void End(uint64_t session_id, Direction direction, npm::NpmTcpStreamEndReasonV1 reason, int64_t time_ns) {
        npm::NpmTcpStreamEventV1 event;
        event.kind = npm::NpmTcpStreamEventKindV1::kEnd;
        event.end_reason = reason;
        assert(tracker.OnStreamEvent(session_id, direction, npm::NpmTcpStreamOriginV1::kSyn, event, time_ns, &rows) ==
               0);
    }
    void Gap(uint64_t session_id, Direction direction, bool capture_truncation, int64_t time_ns) {
        npm::NpmTcpStreamEventV1 event;
        event.kind = npm::NpmTcpStreamEventKindV1::kGap;
        event.includes_capture_truncation = capture_truncation;
        assert(tracker.OnStreamEvent(session_id, direction, npm::NpmTcpStreamOriginV1::kSyn, event, time_ns, &rows) ==
               0);
    }
    npm::NpmHttp1TransactionTrackerV1 tracker;
    std::vector<npm::NpmHttp1TransactionResultV1> rows;
};

void TestPipelineAndHeadContext() {
    Fixture fixture;
    fixture.Data(1, Direction::kAToB, "HEAD /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n", 10);
    assert(fixture.rows.empty());
    fixture.Data(1, Direction::kBToA,
                 "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"
                 "HTTP/1.1 201 Created\r\nContent-Length: 4\r\n\r\nBODY",
                 20);
    assert(fixture.rows.size() == 2);
    assert(fixture.rows[0].outcome == Outcome::kMatched && fixture.rows[0].request->method == "HEAD");
    assert(fixture.rows[0].response->status_code == 200 && fixture.rows[0].latency_ns == 10);
    assert(fixture.rows[1].outcome == Outcome::kMatched && fixture.rows[1].request->target == "/b");
    assert(fixture.rows[1].response->status_code == 201 && fixture.rows[1].latency_ns == 10);
    assert(fixture.rows[0].entity_instance_id != fixture.rows[1].entity_instance_id);
    assert(fixture.rows[0].entity_instance_id != 0 && fixture.rows[1].entity_instance_id != 0);
    assert(fixture.rows[0].incomplete_reason == Reason::kNone && fixture.rows[1].incomplete_reason == Reason::kNone);
}

void TestInformationalAndOrphan() {
    Fixture fixture;
    fixture.Data(1, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 100);
    fixture.Data(1, Direction::kBToA,
                 "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\n\r\n"
                 "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
                 150);
    assert(fixture.rows.size() == 1 && fixture.rows[0].outcome == Outcome::kMatched);
    assert(fixture.rows[0].informational_count == 2 && fixture.rows[0].latency_ns == 50);
    fixture.Data(2, Direction::kBToA, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 200);
    assert(fixture.rows.size() == 2 && fixture.rows[1].outcome == Outcome::kResponseOnly);
    assert(fixture.rows[1].incomplete_reason == Reason::kNoPendingRequest);
    assert(!fixture.rows[1].request && !fixture.rows[1].latency_ns);
    assert(fixture.rows[1].response->status_code == 404);
}

void TestTunnelsStopParsing() {
    for (const auto& pair :
         {std::pair{std::string("GET / HTTP/1.1\r\n\r\n"), std::string("HTTP/1.1 101 Switching Protocols\r\n\r\n")},
          std::pair{std::string("CONNECT example:443 HTTP/1.1\r\n\r\n"),
                    std::string("HTTP/1.1 200 Connection Established\r\n\r\n")}}) {
        Fixture fixture;
        fixture.Data(1, Direction::kAToB, pair.first, 10);
        fixture.Data(1, Direction::kBToA, pair.second + "binary tunnel data", 20);
        assert(fixture.rows.size() == 1 && fixture.rows[0].outcome == Outcome::kMatched);
        fixture.Data(1, Direction::kAToB, "GET /not-http HTTP/1.1\r\n\r\n", 30);
        fixture.End(1, Direction::kBToA, npm::NpmTcpStreamEndReasonV1::kFin, 40);
        assert(fixture.rows.size() == 1);
    }
}

void TestNoMatchBeforeSafeBody() {
    Fixture fixture;
    fixture.Data(1, Direction::kAToB, "POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\nAB", 10);
    fixture.Data(1, Direction::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 20);
    assert(fixture.rows.empty());
    fixture.Data(1, Direction::kAToB, "CD", 11);
    assert(fixture.rows.size() == 1 && fixture.rows[0].outcome == Outcome::kMatched);
    Fixture malformed;
    malformed.Data(1, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 10);
    malformed.Data(1, Direction::kBToA, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nX\r\n", 20);
    assert(malformed.rows.size() == 1 && malformed.rows[0].outcome == Outcome::kRequestOnly);
    assert(malformed.rows[0].incomplete_reason == Reason::kMalformedResponse);
}

void TestDeadlineAndLatency() {
    Fixture fixture;
    fixture.Data(1, Direction::kAToB, "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n", 10);
    assert(fixture.tracker.NextEventDeadlineNs() == 5'000'000'010);
    assert(fixture.tracker.OnCaptureWatermark(std::nullopt, &fixture.rows) == 0 && fixture.rows.empty());
    assert(fixture.tracker.OnCaptureWatermark(5'000'000'009, &fixture.rows) == 0 && fixture.rows.empty());
    assert(fixture.tracker.OnCaptureWatermark(5'000'000'009, &fixture.rows) == 0 && fixture.rows.empty());
    assert(fixture.tracker.OnCaptureWatermark(5'000'000'008, &fixture.rows) == 0 && fixture.rows.empty());
    assert(fixture.tracker.OnCaptureWatermark(5'000'000'010, &fixture.rows) == 0);
    assert(fixture.rows.size() == 2);
    assert(fixture.rows[0].incomplete_reason == Reason::kResponseNotObservedByDeadline);
    assert(fixture.rows[1].incomplete_reason == Reason::kPipelineAlignmentLost);
    assert(fixture.rows[0].observed_at_ns == 5'000'000'010 && fixture.rows[1].observed_at_ns == 5'000'000'010);
    assert(!fixture.tracker.NextEventDeadlineNs());
    fixture.Data(1, Direction::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 5'000'000'011);
    assert(fixture.rows.size() == 2);

    Fixture saturation;
    saturation.Data(2, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", INT64_MAX - 1);
    assert(saturation.tracker.NextEventDeadlineNs() == INT64_MAX);
    saturation.tracker.OnCaptureWatermark(INT64_MAX - 1, &saturation.rows);
    assert(saturation.rows.empty());
    saturation.tracker.OnCaptureWatermark(INT64_MAX, &saturation.rows);
    assert(saturation.rows.size() == 1 &&
           saturation.rows[0].incomplete_reason == Reason::kResponseNotObservedByDeadline);

    Fixture timestamps;
    timestamps.Data(3, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 100);
    timestamps.Data(3, Direction::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 90);
    assert(timestamps.rows.size() == 1 && !timestamps.rows[0].latency_ns);
    timestamps.Data(4, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", std::nullopt);
    timestamps.Data(4, Direction::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 200);
    assert(timestamps.rows.size() == 2 && !timestamps.rows[1].latency_ns);
}

void TestTerminationAndAbort() {
    Fixture half_close;
    half_close.Data(1, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 10);
    half_close.End(1, Direction::kAToB, npm::NpmTcpStreamEndReasonV1::kFin, 11);
    assert(half_close.rows.empty());
    half_close.Data(1, Direction::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 20);
    assert(half_close.rows.size() == 1 && half_close.rows[0].outcome == Outcome::kMatched);

    Fixture response_fin;
    response_fin.Data(1, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 10);
    response_fin.End(1, Direction::kBToA, npm::NpmTcpStreamEndReasonV1::kFin, 20);
    assert(response_fin.rows.size() == 1 && response_fin.rows[0].incomplete_reason == Reason::kResponseStreamEnd);
    response_fin.End(1, Direction::kAToB, npm::NpmTcpStreamEndReasonV1::kReset, 21);
    assert(response_fin.rows.size() == 1);

    for (auto reason : {npm::NpmTcpStreamEndReasonV1::kReset, npm::NpmTcpStreamEndReasonV1::kSessionEnd}) {
        Fixture reset;
        reset.Data(1, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 10);
        reset.End(1, Direction::kBToA, reason, 20);
        assert(reset.rows.size() == 1 && reset.rows[0].incomplete_reason == Reason::kSessionEnd);
    }
    for (auto reason : {npm::NpmSessionEndReason::kIdleTimeout, npm::NpmSessionEndReason::kTupleReuse,
                        npm::NpmSessionEndReason::kEof}) {
        Fixture ended;
        ended.Data(1, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 10);
        assert(ended.tracker.OnSessionEnd(1, reason, 20, &ended.rows) == 0);
        assert(ended.rows.size() == 1 && ended.rows[0].incomplete_reason == Reason::kSessionEnd);
        assert(ended.tracker.ActiveSessions() == 0);
    }
    Fixture cancelled;
    cancelled.Data(1, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 10);
    cancelled.tracker.Abort();
    assert(cancelled.tracker.ActiveSessions() == 0 && cancelled.rows.empty());
}

void TestGapAndTruncation() {
    for (Direction direction : {Direction::kAToB, Direction::kBToA}) {
        for (bool truncated : {false, true}) {
            Fixture fixture;
            fixture.Data(1, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 10);
            fixture.Gap(1, direction, truncated, 20);
            assert(fixture.rows.size() == 1 && fixture.rows[0].outcome == Outcome::kRequestOnly);
            const Reason expected = truncated                       ? Reason::kCaptureTruncation
                                    : direction == Direction::kAToB ? Reason::kRequestPathGap
                                                                    : Reason::kResponsePathGap;
            assert(fixture.rows[0].incomplete_reason == expected);
            fixture.Data(1, Direction::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 30);
            assert(fixture.rows.size() == 1);
        }
    }
    Fixture gap_first;
    gap_first.Gap(7, Direction::kAToB, false, 1);
    gap_first.Data(7, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 2);
    gap_first.Data(7, Direction::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 3);
    assert(gap_first.rows.empty());
    Fixture end_first;
    end_first.End(8, Direction::kAToB, npm::NpmTcpStreamEndReasonV1::kFin, 1);
    end_first.Data(8, Direction::kAToB, "GET / HTTP/1.1\r\n\r\n", 2);
    assert(end_first.rows.empty());
}

void TestDirectionAndSessionIsolation() {
    Fixture fixture;
    fixture.Data(1, Direction::kBToA, "GET /one HTTP/1.1\r\n\r\n", 10);
    fixture.Data(2, Direction::kAToB, "GET /two HTTP/1.1\r\n\r\n", 15);
    fixture.Data(2, Direction::kBToA, "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n", 25);
    fixture.Data(1, Direction::kAToB, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 20);
    assert(fixture.rows.size() == 2);
    assert(fixture.rows[0].session_id == 2 && fixture.rows[0].request_direction == Direction::kAToB);
    assert(fixture.rows[0].response_direction == Direction::kBToA);
    assert(fixture.rows[1].session_id == 1 && fixture.rows[1].request_direction == Direction::kBToA);
    assert(fixture.rows[1].response_direction == Direction::kAToB);
    assert(fixture.rows[0].entity_instance_id != fixture.rows[1].entity_instance_id);
}

void TestNoMatchAfterUnsafeRequestFraming() {
    Fixture fixture;
    fixture.Data(1, Direction::kAToB, "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nA", 10);
    fixture.Data(1, Direction::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 20);
    assert(fixture.rows.empty());
    fixture.Data(1, Direction::kAToB, "XGET /not-safe HTTP/1.1\r\n\r\n", 11);
    assert(fixture.rows.size() == 1 && fixture.rows[0].outcome == Outcome::kRequestOnly);
    assert(fixture.rows[0].incomplete_reason == Reason::kFramingUnsupported);
    fixture.Data(1, Direction::kBToA, "HTTP/1.1 201\r\nContent-Length: 0\r\n\r\n", 30);
    assert(fixture.rows.size() == 1);
}

void TestAcrossBatchesAndOrphanClose() {
    Fixture fixture;
    fixture.Data(1, Direction::kAToB, "GET /one HTTP/1.0\r\n\r\n", 10);
    fixture.Data(1, Direction::kAToB, "GET /two HTTP/1.1\r\n\r\n", 20);
    fixture.Data(1, Direction::kBToA, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nA", 30);
    assert(fixture.rows.empty());
    fixture.Data(1, Direction::kBToA, "BC", 31);
    assert(fixture.rows.size() == 1 && fixture.rows[0].request->target == "/one");
    assert(fixture.rows[0].latency_ns == 20);
    fixture.Data(1, Direction::kBToA, "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n", 40);
    assert(fixture.rows.size() == 2 && fixture.rows[1].request->target == "/two");
    assert(fixture.rows[1].latency_ns == 20);
    fixture.End(1, Direction::kBToA, npm::NpmTcpStreamEndReasonV1::kFin, 50);
    assert(fixture.tracker.OnSessionEnd(1, npm::NpmSessionEndReason::kClosed, 51, &fixture.rows) == 0);
    assert(fixture.rows.size() == 2 && fixture.tracker.ActiveSessions() == 0);

    Fixture orphan;
    orphan.Data(2, Direction::kBToA, "HTTP/1.0 503 Service Unavailable\r\n\r\nbody", 100);
    assert(orphan.rows.size() == 1 && orphan.rows[0].outcome == Outcome::kResponseOnly);
    orphan.End(2, Direction::kBToA, npm::NpmTcpStreamEndReasonV1::kFin, 110);
    assert(orphan.rows.size() == 1);
    assert(orphan.rows[0].incomplete_reason == Reason::kNoPendingRequest);
    assert(orphan.rows[0].observed_at_ns == 100 && orphan.rows[0].response->status_code == 503);
    assert(orphan.tracker.OnSessionEnd(2, npm::NpmSessionEndReason::kClosed, 120, &orphan.rows) == 0);
    assert(orphan.rows.size() == 1);
}

void TestBudgetAndPendingLimit() {
    auto budget = std::make_shared<Budget>();
    auto config = Fixture::Config();
    config.max_header_bytes = 1024;
    config.max_pending_per_session = 1;
    npm::NpmHttp1TransactionTrackerV1 tracker(config, budget);
    std::vector<npm::NpmHttp1TransactionResultV1> rows;
    npm::NpmTcpStreamEventV1 event;
    const std::string request = "GET /one HTTP/1.1\r\n\r\n";
    event.kind = npm::NpmTcpStreamEventKindV1::kData;
    event.bytes = {reinterpret_cast<const uint8_t*>(request.data()), request.size()};
    event.captured_at_ns = 10;
    assert(tracker.OnStreamEvent(1, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, event, 10, &rows) == 0);
    assert(budget->used > 0);
    assert(tracker.OnStreamEvent(1, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, event, 11, &rows) == ENOSPC);
    tracker.Abort();
    assert(budget->used == 0 && rows.empty());

    budget->limit = 1;
    npm::NpmHttp1TransactionTrackerV1 rejected(config, budget);
    assert(rejected.OnStreamEvent(2, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, event, 10, &rows) == ENOSPC);
    assert(budget->used == 0);
    budget->limit = 2 * 1024 * 1024;
    npm::NpmHttp1TransactionTrackerV1 completed(config, budget);
    assert(completed.OnStreamEvent(3, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, event, 10, &rows) == 0);
    npm::NpmTcpStreamEventV1 response;
    const std::string response_bytes = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    response.kind = npm::NpmTcpStreamEventKindV1::kData;
    response.bytes = {reinterpret_cast<const uint8_t*>(response_bytes.data()), response_bytes.size()};
    response.captured_at_ns = 20;
    assert(completed.OnStreamEvent(3, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, response, 20, &rows) == 0);
    assert(rows.size() == 1 && budget->used > 0);
    completed.ReleaseResults(&rows);
    assert(rows.empty() && budget->used > 0);
    assert(completed.OnSessionEnd(3, npm::NpmSessionEndReason::kEof, 30, &rows) == 0);
    assert(budget->used == 0);

    npm::NpmHttp1TransactionTrackerV1 header_limit(config, budget);
    assert(header_limit.OnStreamEvent(4, Direction::kAToB, npm::NpmTcpStreamOriginV1::kSyn, event, 10, &rows) == 0);
    const std::string oversized = "HTTP/1.1 200 OK\r\nX: " + std::string(1024, 'a') + "\r\n\r\n";
    response.bytes = {reinterpret_cast<const uint8_t*>(oversized.data()), oversized.size()};
    assert(header_limit.OnStreamEvent(4, Direction::kBToA, npm::NpmTcpStreamOriginV1::kSyn, response, 20, &rows) == 0);
    assert(rows.size() == 1 && rows[0].incomplete_reason == Reason::kHeaderLimitExceeded);
    header_limit.ReleaseResults(&rows);
    assert(header_limit.OnSessionEnd(4, npm::NpmSessionEndReason::kEof, 30, &rows) == 0);
    assert(budget->used == 0);
}

}  // namespace

int main() {
    TestPipelineAndHeadContext();
    TestInformationalAndOrphan();
    TestTunnelsStopParsing();
    TestNoMatchBeforeSafeBody();
    TestDeadlineAndLatency();
    TestTerminationAndAbort();
    TestGapAndTruncation();
    TestDirectionAndSessionIsolation();
    TestNoMatchAfterUnsafeRequestFraming();
    TestAcrossBatchesAndOrphanClose();
    TestBudgetAndPendingLimit();
}
