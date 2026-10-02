// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/icmp/npm_icmp_echo.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace npm = flowsql::npm;

namespace {

class Budget final : public npm::INpmTaskBudget {
 public:
    uint64_t limit = std::numeric_limits<uint64_t>::max();
    uint64_t used = 0;
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
    npm::NpmBudgetUsage Usage() const override {
        npm::NpmBudgetUsage result;
        result.module_state_bytes = used;
        return result;
    }
};

npm::NpmIcmpParsedV1 Echo(bool request, bool ipv6 = false, uint16_t id = 1, uint16_t sequence = 2) {
    npm::NpmIcmpParsedV1 parsed;
    parsed.kind = request ? npm::NpmIcmpMessageKindV1::kEchoRequest : npm::NpmIcmpMessageKindV1::kEchoReply;
    parsed.ip_family = ipv6 ? 6 : 4;
    parsed.icmp_type = ipv6 ? (request ? 128 : 129) : (request ? 8 : 0);
    parsed.echo_id = id;
    parsed.echo_sequence = sequence;
    if (ipv6) {
        flowsql::packet::IPv6Address src, dst;
        src.bytes[15] = request ? 1 : 2;
        dst.bytes[15] = request ? 2 : 1;
        parsed.src_ip = src;
        parsed.dst_ip = dst;
    } else {
        flowsql::packet::IPv4Address src, dst;
        src.bytes[0] = dst.bytes[0] = 10;
        src.bytes[3] = request ? 1 : 2;
        dst.bytes[3] = request ? 2 : 1;
        parsed.src_ip = src;
        parsed.dst_ip = dst;
    }
    return parsed;
}

void TestMatchRetryAndIsolation() {
    auto budget = std::make_shared<Budget>();
    npm::NpmIcmpConfigV1 config;
    config.echo_timeout_ns = 100;
    npm::NpmIcmpEchoTrackerV1 tracker(config, budget);
    std::vector<npm::NpmIcmpEventV1> rows;
    const auto emit = [&](const auto& row) {
        rows.push_back(row);
        return 0;
    };
    auto request = Echo(true);
    auto reply = Echo(false);
    assert(tracker.OnEcho(request, 7, 100, emit) == 0);
    assert(tracker.OnEcho(request, 7, 105, emit) == 0);
    assert(tracker.PendingCount() == 1 && budget->used != 0 && rows.empty());
    assert(tracker.NextEventDeadlineNs() == 200);
    assert(tracker.OnEcho(reply, 8, 110, emit) == 0);
    assert(rows.size() == 1 && rows.back().outcome == "echo_reply_only" &&
           rows.back().incomplete_reason == "no_pending_request");
    assert(tracker.OnEcho(reply, 7, 120, emit) == 0);
    assert(rows.size() == 2 && rows.back().outcome == "echo_matched");
    assert(rows.back().echo_retries == 1 && rows.back().latency_ns == 20);
    assert(rows.back().src_ip == "10.0.0.1" && rows.back().dst_ip == "10.0.0.2");
    assert(rows.back().entity_instance_id != rows.front().entity_instance_id);
    assert(tracker.PendingCount() == 0 && budget->used == 0);
    assert(tracker.OnEcho(reply, 7, 121, emit) == 0);
    assert(rows.back().outcome == "echo_reply_only" && rows.back().reply_at_ns == 121);

    auto distinct_request = Echo(true, false, 1, 9);
    assert(tracker.OnEcho(distinct_request, 7, 130, emit) == 0);
    auto wrong_endpoint = Echo(false, false, 1, 9);
    auto changed = std::get<flowsql::packet::IPv4Address>(wrong_endpoint.src_ip);
    changed.bytes[3] = 3;
    wrong_endpoint.src_ip = changed;
    for (const auto& wrong :
         {Echo(false, false, 2, 9), Echo(false, false, 1, 10), Echo(false, true, 1, 9), wrong_endpoint}) {
        assert(tracker.OnEcho(wrong, 7, 140, emit) == 0);
        assert(rows.back().outcome == "echo_reply_only" && tracker.PendingCount() == 1);
    }
    assert(tracker.OnEcho(Echo(false, false, 1, 9), 7, 150, emit) == 0);
    assert(rows.back().outcome == "echo_matched" && rows.back().latency_ns == 20);
    assert(tracker.PendingCount() == 0 && budget->used == 0);
}

void TestTruncationAndTime() {
    auto budget = std::make_shared<Budget>();
    npm::NpmIcmpConfigV1 config;
    config.echo_timeout_ns = 100;
    npm::NpmIcmpEchoTrackerV1 tracker(config, budget);
    std::vector<npm::NpmIcmpEventV1> rows;
    const auto emit = [&](const auto& row) {
        rows.push_back(row);
        return 0;
    };
    auto request = Echo(true, true);
    auto reply = Echo(false, true);
    request.outer_truncated = true;
    assert(tracker.OnEcho(request, 1, 100, emit) == 0);
    assert(rows.back().outcome == "echo_request_only" && rows.back().incomplete_reason == "capture_truncation");
    assert(tracker.PendingCount() == 0);
    request.outer_truncated = false;
    assert(tracker.OnEcho(request, 1, 100, emit) == 0);
    reply.outer_truncated = true;
    assert(tracker.OnEcho(reply, 1, 150, emit) == 0);
    assert(rows.back().outcome == "echo_reply_only" && rows.back().incomplete_reason == "capture_truncation");
    assert(tracker.PendingCount() == 1);
    assert(tracker.OnTime(199, emit) == 0 && tracker.PendingCount() == 1);
    assert(tracker.OnTime(199, emit) == 0 && tracker.PendingCount() == 1);
    assert(tracker.OnTime(200, emit) == 0 && tracker.PendingCount() == 0);
    assert(rows.back().outcome == "echo_request_only" &&
           rows.back().incomplete_reason == "response_not_observed_by_deadline");
    assert(rows.back().observed_at == 200 && budget->used == 0);
    assert(tracker.OnEcho(reply, 1, 201, emit) == 0);
    assert(rows.back().outcome == "echo_reply_only");
    assert(tracker.OnTime(150, emit) == 0 && tracker.PendingCount() == 0);
}

void TestEofBudgetAndFailure() {
    auto budget = std::make_shared<Budget>();
    npm::NpmIcmpConfigV1 config;
    config.max_pending_echo = 1;
    npm::NpmIcmpEchoTrackerV1 tracker(config, budget);
    std::vector<npm::NpmIcmpEventV1> rows;
    const auto emit = [&](const auto& row) {
        rows.push_back(row);
        return 0;
    };
    assert(tracker.OnEcho(Echo(true), 1, 100, emit) == 0);
    assert(tracker.OnEcho(Echo(true, false, 2), 1, 100, emit) == ENOSPC);
    assert(tracker.Finish(150, emit) == 0);
    assert(rows.size() == 1 && rows[0].incomplete_reason == "task_eof");
    assert(budget->used == 0 && tracker.PendingCount() == 0);
    assert(tracker.Finish(151, emit) == 0 && rows.size() == 1);

    npm::NpmIcmpEchoTrackerV1 failing(config, budget);
    budget->limit = 0;
    assert(failing.OnEcho(Echo(true), 1, 100, emit) == ENOMEM && failing.PendingCount() == 0);
    budget->limit = std::numeric_limits<uint64_t>::max();
    assert(failing.OnEcho(Echo(true), 1, 100, emit) == 0);
    const auto reject = [&](const auto&) { return EIO; };
    assert(failing.Finish(200, reject) == EIO && failing.PendingCount() == 1 && budget->used != 0);
    failing.Abort();
    assert(budget->used == 0);
}

void TestTimestampBounds() {
    auto budget = std::make_shared<Budget>();
    npm::NpmIcmpConfigV1 config;
    config.echo_timeout_ns = 100;
    npm::NpmIcmpEchoTrackerV1 tracker(config, budget);
    std::vector<npm::NpmIcmpEventV1> rows;
    const auto emit = [&](const auto& row) {
        rows.push_back(row);
        return 0;
    };
    assert(tracker.OnEcho(Echo(true), 1, std::numeric_limits<int64_t>::max() - 1, emit) == 0);
    assert(tracker.NextEventDeadlineNs() == std::numeric_limits<int64_t>::max());
    assert(tracker.OnTime(std::numeric_limits<int64_t>::max() - 1, emit) == 0 && rows.empty());
    assert(tracker.OnTime(std::numeric_limits<int64_t>::max(), emit) == 0 && rows.size() == 1);
    assert(tracker.OnEcho(Echo(true), 1, 200, emit) == 0);
    assert(tracker.OnEcho(Echo(false), 1, 100, emit) == 0);
    assert(rows.back().outcome == "echo_matched" && !rows.back().latency_ns);
    assert(budget->used == 0);
}

}  // namespace

int main() {
    TestMatchRetryAndIsolation();
    TestTruncationAndTime();
    TestEofBudgetAndFailure();
    TestTimestampBounds();
}
