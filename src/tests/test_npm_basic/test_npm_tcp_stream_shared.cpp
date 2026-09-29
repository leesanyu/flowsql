// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/core/npm_tcp_stream_shared.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <vector>

// This isolated target intercepts actual allocations, including the core's list nodes and payload arrays.
// The countdown is enabled only around provider operations; test consumers do not allocate during those probes.
static int fail_allocation_after = -1;
void* operator new(std::size_t size) {
    if (fail_allocation_after == 0) {
        fail_allocation_after = -1;
        throw std::bad_alloc();
    }
    if (fail_allocation_after > 0) --fail_allocation_after;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// No Arrow operations are needed here: the emitter test passes an opaque reference through unchanged.
namespace arrow {
class RecordBatch {};
}  // namespace arrow
namespace npm = flowsql::npm;
using Error = npm::NpmTcpStreamError;
using Kind = npm::NpmTcpStreamEventKindV1;
using Stream = npm::NpmTcpStreamSharedDirection;
using EndReason = npm::NpmTcpStreamEndReasonV1;

namespace {
class Budget final : public npm::INpmTaskBudget {
 public:
    npm::NpmBudgetError Reserve(npm::NpmBudgetCategory category, uint64_t bytes) override {
        assert(category == npm::NpmBudgetCategory::kModuleState);
        if (bytes > limit - used) return npm::NpmBudgetError::kTrackedLimitExceeded;
        used += bytes;
        peak = std::max(peak, used);
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
    uint64_t used = 0, peak = 0, limit = 16 * 1024 * 1024;
};
class Emitter final : public npm::INpmResultEmitterV1 {
 public:
    int Emit(std::string_view, const arrow::RecordBatch&) override {
        ++attempts;
        if (attempts == fail_at) return EIO;
        ++accepted;
        return 0;
    }
    int attempts = 0, accepted = 0, fail_at = 0;
};
struct Saved {
    Kind kind;
    uint64_t begin, end;
    std::string bytes;
    std::optional<int64_t> timestamp;
};
class Consumer final : public npm::INpmTcpStreamConsumerV1 {
 public:
    int OnTcpStreamReadable(const npm::NpmTcpStreamContextV1& ctx, npm::INpmTcpStreamCursorV1& cursor,
                            npm::INpmResultEmitterV1& emitter) override {
        ++calls;
        assert(ctx.session && ctx.session->session_id == expected_session);
        final_calls += ctx.final_drain;
        if (callback) return callback(ctx, cursor, emitter);
        uint64_t left = ctx.final_drain && drain_final ? std::numeric_limits<uint64_t>::max() : quota;
        npm::NpmTcpStreamEventV1 event, again;
        while (cursor.Peek(&event) && left) {
            assert(cursor.Peek(&again));
            assert(again.kind == event.kind && again.begin == event.begin && again.end == event.end);
            assert(again.bytes.data == event.bytes.data && again.bytes.size == event.bytes.size);
            const auto size = event.kind == Kind::kData ? std::min<uint64_t>(event.bytes.size, left) : 0;
            if (record) {
                std::string data;
                if (size) data.assign(reinterpret_cast<const char*>(event.bytes.data), size);
                seen.push_back(
                    {event.kind, event.begin, size ? event.begin + size : event.end, data, event.captured_at_ns});
            }
            assert(cursor.Consume(size) == 0);
            left -= size;
        }
        return 0;
    }
    std::string Data() const {
        std::string result;
        for (const auto& e : seen) result += e.bytes;
        return result;
    }
    size_t Count(Kind kind) const {
        return std::count_if(seen.begin(), seen.end(), [=](const auto& e) { return e.kind == kind; });
    }
    uint64_t expected_session = 17;
    uint64_t quota = std::numeric_limits<uint64_t>::max();
    bool drain_final = true, record = true;
    int calls = 0, final_calls = 0;
    std::vector<Saved> seen;
    std::function<int(const npm::NpmTcpStreamContextV1&, npm::INpmTcpStreamCursorV1&, npm::INpmResultEmitterV1&)>
        callback;
};
struct Fixture {
    std::shared_ptr<Budget> budget = std::make_shared<Budget>();
    npm::NpmSessionView session;
    npm::NpmTcpStreamConfigV1 config;
    Emitter emitter;
    Consumer fast, slow;
    std::unique_ptr<Stream> stream;
    Fixture() { session.session_id = 17; }
    void Open(size_t count = 2) {
        npm::NpmTcpStreamSubscription subs[] = {{"fast", &fast, &emitter}, {"slow", &slow, &emitter}};
        assert(Stream::Create(config, budget, 17, npm::NpmPacketDirection::kAToB, {subs, count}, &stream) ==
               Error::kNone);
    }
    Error Push(uint32_t sequence, std::string_view bytes = {}, uint32_t wire = 0, bool syn = false, bool fin = false,
               int64_t time = 1) {
        npm::NpmPacketView packet;
        packet.payload = {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
        packet.packet.meta.timestamp_ns = time;
        packet.transport.payload_captured_bytes = bytes.size();
        packet.transport.payload_wire_bytes = wire ? wire : bytes.size();
        packet.transport.payload_complete =
            packet.transport.payload_captured_bytes == packet.transport.payload_wire_bytes;
        packet.transport.tcp.valid = true;
        packet.transport.tcp.sequence = sequence;
        packet.transport.tcp.syn = syn;
        packet.transport.tcp.fin = fin;
        return stream->Push(session, packet);
    }
    Error Eof() { return stream->End(session, EndReason::kSessionEnd, npm::NpmSessionEndReason::kEof, 99); }
};

void TestIndependentCursorsAndReclaim() {
    Fixture f;
    f.slow.quota = 1;
    f.Open();
    const auto baseline = f.budget->used;
    assert(f.Push(100, {}, 0, true) == Error::kNone);
    std::string input = "ABCDE";
    assert(f.Push(101, input) == Error::kNone);
    input.assign(5, 'x');
    assert(f.fast.Data() == "ABCDE" && f.slow.Data() == "A");
    const auto retained = f.budget->used;
    assert(retained > baseline);
    assert(f.Push(106, "FG", 0, false, false, 2) == Error::kNone);
    assert(f.fast.Data() == "ABCDEFG" && f.slow.Data() == "AB");
    const auto two_events = f.budget->used;
    f.slow.quota = 0;
    assert(f.stream->AdvanceWatermark(f.session, 10) == Error::kNone);
    const int calls = f.slow.calls;
    assert(f.stream->AdvanceWatermark(f.session, 10) == Error::kNone);
    assert(f.stream->AdvanceWatermark(f.session, 9) == Error::kNone);
    assert(f.slow.calls == calls && f.budget->used == two_events);
    f.slow.quota = 3;
    assert(f.stream->AdvanceWatermark(f.session, 11) == Error::kNone);
    assert(f.slow.Data() == "ABCDE");
    assert(f.budget->used < two_events && f.budget->used > baseline);
    f.slow.quota = 2;
    assert(f.stream->AdvanceWatermark(f.session, 12) == Error::kNone);
    assert(f.slow.Data() == f.fast.Data() && f.budget->used == baseline);
    assert(f.slow.seen.back().timestamp == 2);
    assert(f.Eof() == Error::kNone && f.stream->Ended());
    assert(f.fast.Count(Kind::kEnd) == 1 && f.slow.Count(Kind::kEnd) == 1);
    assert(f.budget->used == 0);
    assert(f.Eof() == Error::kNone && f.slow.final_calls == 1);
    f.stream->Abort();
    assert(f.budget->used == 0);
}

void TestSharedGapAndFinalDrain() {
    Fixture f;
    f.config.gap_timeout_ns = 5;
    f.slow.quota = 0;
    f.Open();
    assert(f.Push(101, "ABC", 5) == Error::kNone);
    assert(f.stream->NextEventDeadlineNs() == 6);
    assert(f.stream->AdvanceWatermark(f.session, 5) == Error::kNone);
    assert(f.fast.Count(Kind::kGap) == 0);
    assert(f.stream->AdvanceWatermark(f.session, 6) == Error::kNone);
    assert(f.fast.Count(Kind::kGap) == 1);
    assert(f.Push(106, "FG", 0, false, true, 7) == Error::kNone);
    assert(f.stream->Ended() && f.budget->used == 0);
    assert(f.fast.Data() == "ABCFG" && f.slow.Data() == f.fast.Data());
    assert(f.slow.seen.size() == f.fast.seen.size());
    for (size_t i = 0; i < f.slow.seen.size(); ++i) {
        const auto& a = f.fast.seen[i];
        const auto& b = f.slow.seen[i];
        assert(a.kind == b.kind && a.begin == b.begin && a.end == b.end && a.bytes == b.bytes);
    }
    assert(f.slow.final_calls == 1 && !f.stream->NextEventDeadlineNs());

    Fixture bad;
    bad.slow.quota = 0;
    bad.slow.drain_final = false;
    bad.Open();
    assert(bad.Push(1, "data") == Error::kNone);
    const auto calls = bad.slow.calls;
    assert(bad.Eof() == Error::kNotDrained);
    assert(bad.slow.calls == calls + 1 && bad.budget->used == 0);
    assert(bad.stream->Failure().failing_consumer == "slow");
    assert(bad.stream->Failure().retaining_consumer == "slow");
    assert(bad.fast.Count(Kind::kEnd) == 1);  // Successful prefix is not rolled back.
}

void TestInvalidConsumptionIsLatched() {
    for (int mode = 0; mode < 5; ++mode) {
        Fixture f;
        f.fast.callback = [mode](const auto&, auto& cursor, auto&) {
            npm::NpmTcpStreamEventV1 e;
            assert(cursor.Peek(&e));
            if (mode == 0) assert(cursor.Consume(0) != 0);
            if (mode == 1) assert(cursor.Consume(e.bytes.size + 1) != 0);
            if (mode == 2) {
                assert(cursor.Consume(e.bytes.size) == 0);
                assert(!cursor.Peek(&e));
                assert(cursor.Consume(0) != 0);
            }
            if (mode == 3) assert(cursor.Consume(1) != 0);  // Gap.
            if (mode == 4) assert(cursor.Consume(1) != 0);  // End.
            return 0;                                       // Deliberately ignore failure: provider must still fail.
        };
        f.Open();
        Error result;
        if (mode < 3)
            result = f.Push(1, "abc");
        else if (mode == 3) {
            assert(f.Push(1, {}, 2) == Error::kNone);
            result = f.stream->AdvanceWatermark(f.session, 2000000000);
        } else
            result = f.Eof();
        assert(result == Error::kInvalidConsume && f.budget->used == 0);
        assert(f.stream->Failure().failing_consumer == "fast");
        assert(f.slow.calls == 0);
    }
}

void TestBudgetsAndSinglePayloadCharge() {
    uint64_t small = 0, large = 0;
    for (int size : {100, 10100}) {
        Fixture f;
        f.fast.quota = f.slow.quota = 0;
        f.Open();
        const std::string data(size, 'x');
        assert(f.Push(1, data) == Error::kNone);
        if (size == 100)
            small = f.budget->used;
        else
            large = f.budget->used;
        f.stream->Abort();
        assert(f.budget->used == 0 && f.fast.Count(Kind::kEnd) == 0);
    }
    assert(large - small == 10000);  // One payload, regardless of consumer count.
    for (bool task_limit : {false, true}) {
        Fixture f;
        f.config.max_buffered_bytes_per_direction = 65536;
        if (task_limit) f.budget->limit = 50000;
        f.slow.quota = 0;
        f.Open();
        std::string data(22000, 'z');
        assert(f.Push(1, data) == Error::kNone);
        // An unresolved gap makes the core retain another payload while the slow reader retains the first.
        assert(f.Push(22002, data) == Error::kNone);
        // Exhaust exactly one limit: the task probe stays below the 64 KiB direction bound.
        const auto result = f.Push(44002, std::string_view(data).substr(0, task_limit ? 10000 : data.size()));
        assert(result == (task_limit ? Error::kTaskBudgetExceeded : Error::kDirectionLimitExceeded));
        const auto& failure = f.stream->Failure();
        assert(failure.session_id == 17 && failure.direction == npm::NpmPacketDirection::kAToB);
        assert(failure.retaining_consumer == "slow");
        assert(f.budget->peak <= (task_limit ? 50000 : 65536));
        assert(f.budget->used == 0 && f.fast.Data() == data && f.fast.Count(Kind::kGap) == 0);
    }
    Fixture transferred;
    transferred.config.max_buffered_bytes_per_direction = 65536;
    transferred.slow.quota = 0;
    transferred.Open();
    assert(transferred.Push(1, std::string(40000, 'a')) == Error::kNone);  // Copying twice would exceed 64 KiB.
    assert(transferred.budget->peak < 65536);
    transferred.stream->Abort();
    assert(transferred.budget->used == 0);
}

void TestCreateAndAllocationFailures() {
    Fixture none;
    assert(Stream::Create({}, none.budget, 17, npm::NpmPacketDirection::kAToB, {}, &none.stream) == Error::kNone);
    assert(!none.stream && none.budget->used == 0);
    npm::NpmTcpStreamSubscription sub{"fast", &none.fast, &none.emitter};
    npm::NpmTcpStreamFailure failure;
    none.budget->limit = 1;
    assert(Stream::Create({}, none.budget, 17, npm::NpmPacketDirection::kAToB, {&sub, 1}, &none.stream, &failure) ==
           Error::kTaskBudgetExceeded);
    assert(!none.stream && none.budget->used == 0 && failure.session_id == 17);
    none.budget->limit = 1000000;
    int create_failures = 0;
    for (int allocation = 0; allocation < 8; ++allocation) {
        fail_allocation_after = allocation;
        const auto result =
            Stream::Create({}, none.budget, 17, npm::NpmPacketDirection::kAToB, {&sub, 1}, &none.stream, &failure);
        fail_allocation_after = -1;
        if (result == Error::kNone) {
            none.stream.reset();
            break;
        }
        assert(result == Error::kAllocationFailed && !none.stream && none.budget->used == 0);
        ++create_failures;
    }
    assert(create_failures >= 2 && none.budget->used == 0);
    int push_failures = 0;
    for (int allocation = 0; allocation < 16; ++allocation) {
        Fixture f;
        f.fast.record = f.slow.record = false;
        f.fast.quota = f.slow.quota = 0;
        f.Open();
        fail_allocation_after = allocation;
        const auto result = f.Push(1, "payload");
        fail_allocation_after = -1;
        if (result == Error::kNone) {
            f.stream->Abort();
            assert(f.budget->used == 0);
            break;
        }
        assert(result == Error::kAllocationFailed && f.budget->used == 0);
        ++push_failures;
    }
    assert(push_failures >= 3);  // Core range, payload and shared event allocation sites.

    int transfer_failures = 0;
    for (int allocation = 0; allocation < 16; ++allocation) {
        Fixture f;
        f.fast.record = f.slow.record = false;
        f.fast.quota = f.slow.quota = 0;
        f.Open();
        assert(f.Push(0, {}, 0, true) == Error::kNone);
        assert(f.Push(4, "def") == Error::kNone);
        fail_allocation_after = allocation;
        const auto result = f.Push(1, "abc");
        fail_allocation_after = -1;
        if (result == Error::kNone) {
            f.stream->Abort();
            assert(f.budget->used == 0);
            break;
        }
        assert(result == Error::kAllocationFailed && f.budget->used == 0);
        ++transfer_failures;
    }
    assert(transfer_failures >= 3);  // Payload and two shared nodes: the last failure follows a successful transfer.
    for (int allocation = 0; allocation < 2; ++allocation) {
        Fixture f;
        f.fast.record = f.slow.record = false;
        f.fast.quota = f.slow.quota = 0;
        f.Open();
        assert(f.Push(1, "abc", 5) == Error::kNone);
        fail_allocation_after = allocation;
        const auto result = f.Eof();
        fail_allocation_after = -1;
        assert(result == Error::kAllocationFailed && f.budget->used == 0);  // Gap or End allocation.
        assert(f.fast.final_calls == 0 && f.slow.final_calls == 0);
    }
}

void TestSharedTaskBudgetAndDestruction() {
    Fixture a, b;
    b.budget = a.budget;
    a.slow.quota = b.slow.quota = 0;
    a.Open();
    assert(a.Push(1, "retained") == Error::kNone);
    const auto baseline = a.budget->used;
    b.Open();
    assert(b.Push(1, "other") == Error::kNone);
    a.budget->limit = a.budget->used;
    assert(b.Push(6, "fail") == Error::kTaskBudgetExceeded);
    assert(a.budget->used == baseline);  // A failed direction cannot release another direction's charge.
    a.budget->limit = 16 * 1024 * 1024;
    assert(a.Eof() == Error::kNone && a.budget->used == 0);
    Fixture abandoned;
    abandoned.slow.quota = 0;
    abandoned.Open();
    assert(abandoned.Push(1, "abc", 10) == Error::kNone);
    abandoned.stream.reset();
    assert(abandoned.budget->used == 0 && abandoned.fast.Count(Kind::kEnd) == 0);
}

void TestEmitterConsumerAndAbortFailures() {
    Fixture f;
    arrow::RecordBatch rows;
    f.fast.callback = [&](const auto&, auto& cursor, auto& emitter) {
        npm::NpmTcpStreamEventV1 e;
        while (cursor.Peek(&e)) {
            emitter.Emit("test", rows);  // Ignore emitter failure deliberately.
            cursor.Consume(e.bytes.size);
        }
        return 0;
    };
    f.emitter.fail_at = 2;
    f.Open();
    assert(f.Push(1, "abc") == Error::kNone);
    assert(f.Push(4, "def") == Error::kEmitterError);
    assert(f.emitter.accepted == 1 && f.budget->used == 0 && f.slow.Data() == "abc");
    assert(f.stream->Failure().failing_consumer == "fast");
    for (int mode = 0; mode < 4; ++mode) {
        Fixture bad;
        bad.fast.callback = [&](const auto&, auto&, auto&) {
            if (mode == 0) return EIO;
            if (mode == 1) throw std::bad_alloc();
            if (mode == 2) bad.stream->Abort();
            if (mode == 3) assert(bad.Eof() == Error::kReentrantCall);
            return 0;
        };
        bad.Open();
        const Error expected[] = {Error::kConsumerError, Error::kAllocationFailed, Error::kAborted,
                                  Error::kReentrantCall};
        assert(bad.Push(1, "abc") == expected[mode]);
        assert(bad.budget->used == 0 && bad.slow.calls == 0);
        bad.stream->Abort();
        assert(bad.budget->used == 0);
    }
    Fixture cancelled;
    cancelled.slow.quota = 0;
    cancelled.Open();
    assert(cancelled.Push(1, "abc", 5) == Error::kNone);
    cancelled.stream->Abort();
    assert(cancelled.budget->used == 0 && cancelled.fast.Count(Kind::kEnd) == 0);
    assert(cancelled.Eof() == Error::kAborted && cancelled.slow.final_calls == 0);
}
}  // namespace

int main() {
    TestIndependentCursorsAndReclaim();
    TestSharedGapAndFinalDrain();
    TestInvalidConsumptionIsLatched();
    TestBudgetsAndSinglePayloadCharge();
    TestCreateAndAllocationFailures();
    TestSharedTaskBudgetAndDestruction();
    TestEmitterConsumerAndAbortFailures();
    std::puts("NPM shared TCP stream: 7 test groups passed");
}
