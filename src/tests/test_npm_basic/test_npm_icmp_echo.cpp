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

template <bool ipv6 = false>
auto Echo(bool request, uint16_t id = 1, uint16_t sequence = 2) {
    using Address = std::conditional_t<ipv6, flowsql::network::IPv6Address, flowsql::network::IPv4Address>;
    npm::NpmIcmpFamilyParsed<Address> parsed;
    parsed.kind = request ? npm::NpmIcmpMessageKindV1::kEchoRequest : npm::NpmIcmpMessageKindV1::kEchoReply;
    parsed.icmp_type = ipv6 ? (request ? 128 : 129) : (request ? 8 : 0);
    parsed.echo_id = id;
    parsed.echo_sequence = sequence;
    if constexpr (!ipv6) parsed.src_ip.bytes[0] = parsed.dst_ip.bytes[0] = 10;
    parsed.src_ip.bytes[sizeof(Address) - 1] = request ? 1 : 2;
    parsed.dst_ip.bytes[sizeof(Address) - 1] = request ? 2 : 1;
    return parsed;
}

struct ObservedEvent {
    uint64_t entity_instance_id;
    int64_t observed_at;
    std::string outcome;
    std::optional<std::string> incomplete_reason;
    std::optional<uint32_t> echo_retries;
    std::optional<int64_t> latency_ns, reply_at_ns;
    std::vector<uint8_t> src_ip, dst_ip;
    template <typename Address>
    ObservedEvent(const npm::NpmIcmpFamilyEvent<Address>& row)
        : entity_instance_id(row.entity_instance_id),
          observed_at(row.observed_at),
          outcome(row.outcome),
          incomplete_reason(row.incomplete_reason),
          echo_retries(row.echo_retries),
          latency_ns(row.latency_ns),
          reply_at_ns(row.reply_at_ns),
          src_ip(row.src_ip.bytes, row.src_ip.bytes + sizeof(Address)),
          dst_ip(row.dst_ip.bytes, row.dst_ip.bytes + sizeof(Address)) {}
};

void TestMatchRetryAndIsolation() {
    auto budget = std::make_shared<Budget>();
    npm::NpmIcmpConfigV1 config;
    config.echo_timeout_ns = 100;
    npm::NpmIcmpEchoTrackerV1 tracker(config, budget);
    std::vector<ObservedEvent> rows;
    const auto emit = [&](const auto& row) {
        rows.emplace_back(row);
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
    assert(rows.back().src_ip == (std::vector<uint8_t>{10, 0, 0, 1}) &&
           rows.back().dst_ip == (std::vector<uint8_t>{10, 0, 0, 2}));
    assert(rows.back().entity_instance_id != rows.front().entity_instance_id);
    assert(tracker.PendingCount() == 0 && budget->used == 0);
    assert(tracker.OnEcho(reply, 7, 121, emit) == 0);
    assert(rows.back().outcome == "echo_reply_only" && rows.back().reply_at_ns == 121);

    auto distinct_request = Echo(true, 1, 9);
    assert(tracker.OnEcho(distinct_request, 7, 130, emit) == 0);
    auto wrong_endpoint = Echo(false, 1, 9);
    auto changed = wrong_endpoint.src_ip;
    changed.bytes[3] = 3;
    wrong_endpoint.src_ip = changed;
    assert(tracker.OnEcho(Echo<true>(false, 1, 9), 7, 140, emit) == 0);
    assert(rows.back().outcome == "echo_reply_only" && tracker.PendingCount() == 1);
    for (const auto& wrong : {Echo(false, 2, 9), Echo(false, 1, 10), wrong_endpoint}) {
        assert(tracker.OnEcho(wrong, 7, 140, emit) == 0);
        assert(rows.back().outcome == "echo_reply_only" && tracker.PendingCount() == 1);
    }
    assert(tracker.OnEcho(Echo(false, 1, 9), 7, 150, emit) == 0);
    assert(rows.back().outcome == "echo_matched" && rows.back().latency_ns == 20);
    assert(tracker.PendingCount() == 0 && budget->used == 0);
}

void TestTruncationAndTime() {
    auto budget = std::make_shared<Budget>();
    npm::NpmIcmpConfigV1 config;
    config.echo_timeout_ns = 100;
    npm::NpmIcmpEchoTrackerV1 tracker(config, budget);
    std::vector<ObservedEvent> rows;
    const auto emit = [&](const auto& row) {
        rows.emplace_back(row);
        return 0;
    };
    auto request = Echo<true>(true);
    auto reply = Echo<true>(false);
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
    std::vector<ObservedEvent> rows;
    const auto emit = [&](const auto& row) {
        rows.emplace_back(row);
        return 0;
    };
    assert(tracker.OnEcho(Echo(true), 1, 100, emit) == 0);
    assert(tracker.OnEcho(Echo(true, 2), 1, 100, emit) == ENOSPC);
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
    std::vector<ObservedEvent> rows;
    const auto emit = [&](const auto& row) {
        rows.emplace_back(row);
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

void TestSharedFamilyCapacityAndCharges() {
    auto budget = std::make_shared<Budget>();
    npm::NpmIcmpConfigV1 config;
    config.max_pending_echo = 2;
    config.echo_timeout_ns = 100;
    npm::NpmIcmpEchoTrackerV1 tracker(config, budget);
    std::vector<ObservedEvent> rows;
    const auto emit = [&](const auto& row) {
        rows.emplace_back(row);
        return 0;
    };
    assert(tracker.OnEcho(Echo(true), 7, 100, emit) == 0);
    const uint64_t charge4 = budget->used;
    assert(tracker.OnEcho(Echo<true>(true), 7, 100, emit) == 0);
    const uint64_t charge6 = budget->used - charge4;
    assert(charge6 > charge4 && tracker.PendingCount() == 2);
    assert(tracker.OnEcho(Echo(true, 2), 7, 101, emit) == ENOSPC);
    assert(tracker.OnEcho(Echo<true>(true, 2), 7, 101, emit) == ENOSPC);
    assert(tracker.OnEcho(Echo<true>(false), 7, 110, emit) == 0);
    assert(budget->used == charge4 && tracker.PendingCount() == 1);
    assert(tracker.OnEcho(Echo(false), 7, 120, emit) == 0);
    assert(rows.size() == 2 && rows[0].entity_instance_id != rows[1].entity_instance_id);
    assert(rows[0].src_ip.size() == 16 && rows[1].src_ip.size() == 4 && budget->used == 0);
    assert(tracker.OnEcho(Echo(true), 7, 130, emit) == 0);
    assert(tracker.OnEcho(Echo<true>(true), 7, 130, emit) == 0);
    assert(tracker.OnTime(230, emit) == 0 && tracker.PendingCount() == 0 && budget->used == 0);
    assert(rows.size() == 4 && rows[2].entity_instance_id != rows[3].entity_instance_id);
}

}  // namespace

int main() {
    TestMatchRetryAndIsolation();
    TestTruncationAndTime();
    TestEofBudgetAndFailure();
    TestTimestampBounds();
    TestSharedFamilyCapacityAndCharges();
}
