// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/core/npm_tcp_stream_direction.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace npm = flowsql::npm;
using Kind = npm::NpmTcpStreamEventKindV1;
using EndReason = npm::NpmTcpStreamEndReasonV1;
using Error = npm::NpmTcpStreamError;

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
        npm::NpmBudgetUsage usage;
        usage.module_state_bytes = used;
        return usage;
    }
    uint64_t used = 0, peak = 0, limit = 16 * 1024 * 1024;
};

struct SavedEvent {
    npm::NpmTcpStreamEventV1 facts;
    std::string bytes;
};
class Sink final : public npm::INpmTcpStreamEventSink {
 public:
    int Emit(const npm::NpmTcpStreamEventV1& event) override {
        if (++attempts == fail_at) return EIO;
        assert(event.begin == next && event.end >= event.begin);
        SavedEvent saved{event, {}};
        if (event.kind == Kind::kData) {
            assert(event.end > event.begin && event.bytes.size == event.end - event.begin);
            assert(event.bytes.data && event.captured_at_ns && !event.gap_reason && !event.end_reason);
            assert(!event.includes_capture_truncation && !event.session_end_reason);
            saved.bytes.assign(reinterpret_cast<const char*>(event.bytes.data), event.bytes.size);
        } else {
            assert(event.bytes.size == 0 && !event.captured_at_ns);
            if (event.kind == Kind::kGap) {
                assert(event.end > event.begin && event.gap_reason && !event.end_reason && !event.session_end_reason);
            } else {
                assert(event.begin == event.end && event.end_reason && !event.gap_reason);
                assert(!event.includes_capture_truncation);
            }
        }
        saved.facts.bytes = {};  // Never retain borrowed pointers.
        events.push_back(std::move(saved));
        next = event.end;
        return 0;
    }
    std::string Data() const {
        std::string result;
        for (const auto& event : events) result += event.bytes;
        return result;
    }
    size_t Count(Kind kind) const {
        return std::count_if(events.begin(), events.end(), [kind](const auto& e) { return e.facts.kind == kind; });
    }
    uint64_t next = 0;
    int attempts = 0, fail_at = -1;
    std::vector<SavedEvent> events;
};

constexpr uint8_t kSyn = 1, kFin = 2, kRst = 4;
Error Push(npm::NpmTcpStreamDirection& stream, Sink& sink, uint32_t seq, std::string data = {}, int64_t time = 0,
           uint8_t flags = 0, std::optional<uint32_t> wire = {}) {
    npm::NpmPacketView packet;
    packet.packet.meta.timestamp_ns = time;
    packet.payload = {reinterpret_cast<const uint8_t*>(data.data()), data.size()};
    packet.transport.payload_captured_bytes = data.size();
    packet.transport.payload_wire_bytes = wire.value_or(data.size());
    packet.transport.payload_complete = packet.transport.payload_captured_bytes == packet.transport.payload_wire_bytes;
    auto& tcp = packet.transport.tcp;
    tcp.valid = true;
    tcp.sequence = seq;
    tcp.syn = flags & kSyn;
    tcp.fin = flags & kFin;
    tcp.rst = flags & kRst;
    tcp.ack = !(flags & kSyn);
    tcp.acknowledgment = 0xdeadbeef;  // Never infer opposite-direction data from ACK.
    return stream.Push(packet, sink);
}
void Eof(npm::NpmTcpStreamDirection& stream, Sink& sink) {
    assert(stream.End(EndReason::kSessionEnd, npm::NpmSessionEndReason::kEof, sink) == Error::kNone);
}

void TestOrderedRetransmissionAndOwnedBytes() {
    auto budget = std::make_shared<Budget>();
    {
        npm::NpmTcpStreamDirection stream({}, budget);
        Sink sink;
        assert(Push(stream, sink, 100, {}, 0, kSyn) == Error::kNone);
        assert(stream.Origin() == npm::NpmTcpStreamOriginV1::kSyn && sink.events.empty());
        assert(Push(stream, sink, 101, "ABC", 1) == Error::kNone);
        assert(Push(stream, sink, 106, "FG", 2) == Error::kNone);
        assert(Push(stream, sink, 106, "xx", 3) == Error::kNone);
        assert(sink.Data() == "ABC");
        assert(Push(stream, sink, 104, "DE", 4) == Error::kNone);
        assert(sink.Data() == "ABCDEFG" && sink.Count(Kind::kGap) == 0);
        assert(sink.events[1].facts.captured_at_ns == 4 && sink.events[2].facts.captured_at_ns == 2);
        const auto count = sink.events.size();
        assert(Push(stream, sink, 101, "xxxxxxx", 5) == Error::kNone);
        assert(sink.events.size() == count && !stream.NextEventDeadlineNs());
        Eof(stream, sink);
        assert(stream.Ended() && sink.Count(Kind::kEnd) == 1);
        Eof(stream, sink);
        assert(Push(stream, sink, 108, "ignored") == Error::kNone && sink.events.size() == count + 1);
    }
    assert(budget->used == 0);
}

void TestOverlapAndMidstream() {
    auto budget = std::make_shared<Budget>();
    npm::NpmTcpStreamDirection stream({}, budget);
    Sink sink;
    assert(Push(stream, sink, 0xf0000000) == Error::kNone);  // Pure ACK does not anchor a stream.
    assert(stream.Origin() == npm::NpmTcpStreamOriginV1::kUnknown && sink.events.empty());
    assert(Push(stream, sink, 101, "ABCDE", 10) == Error::kNone);
    assert(stream.Origin() == npm::NpmTcpStreamOriginV1::kMidstream);
    assert(Push(stream, sink, 103, "CxyFG", 20) == Error::kNone);
    assert(sink.Data() == "ABCDEFG" && sink.events.back().facts.captured_at_ns == 20);
    assert(Push(stream, sink, 98, "oldABCDEFGH", 30) == Error::kNone);
    assert(sink.Data() == "ABCDEFGH");
    assert(Push(stream, sink, 97, {}, 40, kSyn) == Error::kNone);
    assert(stream.Origin() == npm::NpmTcpStreamOriginV1::kMidstream);
    Eof(stream, sink);

    npm::NpmTcpStreamDirection pending({}, budget);
    Sink out;
    assert(Push(pending, out, 100, {}, 0, kSyn) == Error::kNone);
    assert(Push(pending, out, 104, "DEF", 1) == Error::kNone);
    assert(Push(pending, out, 102, "BCxxxGH", 2) == Error::kNone);
    assert(out.events.empty());
    assert(Push(pending, out, 101, "A", 3) == Error::kNone);
    assert(out.Data() == "ABCDEFGH");
    assert(out.events[1].facts.captured_at_ns == 2 && out.events[2].facts.captured_at_ns == 1);
}

void TestWrapAndAmbiguity() {
    auto budget = std::make_shared<Budget>();
    npm::NpmTcpStreamDirection stream({}, budget);
    Sink sink;
    assert(Push(stream, sink, 0xfffffffc, "ABC", 0, kSyn) == Error::kNone);
    assert(Push(stream, sink, 0, "DE", 1) == Error::kNone);
    assert(Push(stream, sink, 0xfffffffd, "zzz", 2) == Error::kNone);
    assert(sink.Data() == "ABCDE");
    assert(Push(stream, sink, 2, {}, 3, kFin) == Error::kNone);
    assert(stream.Ended() && sink.events.back().facts.end_reason == EndReason::kFin);
    assert(sink.next == 5);

    npm::NpmTcpStreamConfigV1 config;
    config.gap_timeout_ns = 0;
    npm::NpmTcpStreamDirection laps(config, budget);
    Sink out;
    assert(Push(laps, out, 0xffffffff, {}, 0, kSyn) == Error::kNone);
    uint64_t offset = 0;
    for (int i = 0; i < 12; ++i) {
        offset += 0x60000000ULL;
        assert(Push(laps, out, static_cast<uint32_t>(offset), "x", i) == Error::kNone);
        assert(laps.AdvanceWatermark(i, out) == Error::kNone);
        assert(out.next == offset + 1);
    }
    assert(out.next > (uint64_t{1} << 32));
    const auto before = out.next;
    assert(Push(laps, out, static_cast<uint32_t>(before) + 0x80000000U, "?") == Error::kNone);
    assert(laps.Ended() && out.next == before && out.events.back().facts.end_reason == EndReason::kSequenceAmbiguous);

    npm::NpmTcpStreamDirection gap({}, budget);
    Sink known;
    assert(Push(gap, known, 100, {}, 0, kSyn) == Error::kNone);
    assert(Push(gap, known, 104, "D") == Error::kNone);
    assert(Push(gap, known, 105U + 0x80000000U, "?") == Error::kNone);
    assert(known.Data() == "D" && known.Count(Kind::kGap) == 1 && known.next == 4);
    assert(known.events.back().facts.end_reason == EndReason::kSequenceAmbiguous);
}

void TestTruncationAndWatermarks() {
    auto budget = std::make_shared<Budget>();
    npm::NpmTcpStreamConfigV1 config;
    config.gap_timeout_ns = 10;
    npm::NpmTcpStreamDirection stream(config, budget);
    Sink sink;
    assert(Push(stream, sink, 100, "ABC", 100, 0, 5) == Error::kNone);
    assert(stream.CaptureTruncationSeen() && stream.NextEventDeadlineNs() == 110);
    assert(stream.AdvanceWatermark(109, sink) == Error::kNone);
    assert(Push(stream, sink, 103, "DE", 109) == Error::kNone);
    assert(!stream.NextEventDeadlineNs() && stream.CaptureTruncationSeen());
    assert(stream.AdvanceWatermark(110, sink) == Error::kNone);
    assert(sink.Data() == "ABCDE" && sink.Count(Kind::kGap) == 0);

    npm::NpmTcpStreamDirection missing(config, budget);
    Sink out;
    assert(Push(missing, out, 100, "ABC", 100, 0, 5) == Error::kNone);
    assert(Push(missing, out, 105, "FG", 103) == Error::kNone);
    assert(missing.AdvanceWatermark(109, out) == Error::kNone && out.Data() == "ABC");
    assert(missing.AdvanceWatermark(90, out) == Error::kNone && out.events.size() == 1);
    assert(missing.AdvanceWatermark(110, out) == Error::kNone);
    assert(out.Data() == "ABCFG" && out.Count(Kind::kGap) == 1);
    const auto& gap = out.events[1].facts;
    assert(gap.begin == 3 && gap.end == 5 && gap.includes_capture_truncation);
    assert(gap.gap_reason == npm::NpmTcpStreamGapReasonV1::kWaitExpired);
    const auto count = out.events.size();
    assert(Push(missing, out, 103, "DE", 111) == Error::kNone && out.events.size() == count);
    Eof(missing, out);
    assert(out.next == 7);  // No invented tail after known bytes.

    npm::NpmTcpStreamDirection partial(config, budget);
    Sink chunks;
    assert(Push(partial, chunks, 100, "A", 100, 0, 6) == Error::kNone);
    assert(Push(partial, chunks, 103, "D", 109) == Error::kNone);
    assert(partial.NextEventDeadlineNs() == 110);
    assert(partial.AdvanceWatermark(110, chunks) == Error::kNone);
    assert(chunks.Count(Kind::kGap) == 2 && chunks.Data() == "AD" && chunks.next == 6);
    for (const auto& event : chunks.events) {
        if (event.facts.kind == Kind::kGap) assert(event.facts.includes_capture_truncation);
    }

    config.gap_timeout_ns = 0;
    npm::NpmTcpStreamDirection zero(config, budget);
    Sink none;
    assert(Push(zero, none, 100, {}, 10, 0, 2) == Error::kNone);
    assert(none.events.empty());
    assert(zero.AdvanceWatermark(9, none) == Error::kNone && none.events.empty());
    assert(zero.AdvanceWatermark(10, none) == Error::kNone && none.Count(Kind::kGap) == 1);
}

void TestFinResetAndSessionTermination() {
    auto budget = std::make_shared<Budget>();
    npm::NpmTcpStreamConfigV1 config;
    config.gap_timeout_ns = 10;
    npm::NpmTcpStreamDirection ab(config, budget), ba(config, budget);
    Sink forward, reverse;
    assert(Push(ab, forward, 100, "ABC", 0, kSyn) == Error::kNone);
    assert(Push(ab, forward, 106, "FG", 1, kFin) == Error::kNone);
    assert(!ab.Ended() && forward.Data() == "ABC");
    assert(Push(ba, reverse, 700, "reply", 2) == Error::kNone);
    assert(Push(ab, forward, 104, "DE", 3) == Error::kNone);
    assert(ab.Ended() && forward.Data() == "ABCDEFG" && forward.Count(Kind::kEnd) == 1);
    assert(!ba.Ended());
    assert(Push(ab, forward, 108, "ignored", 4, kFin) == Error::kNone);
    assert(forward.Count(Kind::kEnd) == 1 && forward.Data() == "ABCDEFG");
    assert(Push(ba, reverse, 705, {}, 5, kFin) == Error::kNone && ba.Ended());

    npm::NpmTcpStreamDirection fin_gap(config, budget);
    Sink gap;
    assert(Push(fin_gap, gap, 10, {}, 0, kSyn) == Error::kNone);
    assert(Push(fin_gap, gap, 13, {}, 1, kFin) == Error::kNone);
    assert(!fin_gap.Ended());
    assert(fin_gap.AdvanceWatermark(11, gap) == Error::kNone && fin_gap.Ended());
    assert(gap.Count(Kind::kGap) == 1 && gap.next == 2 && gap.events.back().facts.end_reason == EndReason::kFin);

    npm::NpmTcpStreamDirection reset(config, budget);
    Sink rst;
    assert(Push(reset, rst, 100, {}, 0, kSyn) == Error::kNone);
    assert(Push(reset, rst, 104, "D", 1, kRst) == Error::kNone);
    assert(rst.Data() == "D" && rst.Count(Kind::kGap) == 1 && reset.Ended());
    assert(rst.events.back().facts.end_reason == EndReason::kReset);
    assert(rst.events[0].facts.gap_reason == npm::NpmTcpStreamGapReasonV1::kTermination);
    for (const auto reason : {npm::NpmSessionEndReason::kClosed, npm::NpmSessionEndReason::kIdleTimeout,
                              npm::NpmSessionEndReason::kTupleReuse, npm::NpmSessionEndReason::kEof}) {
        npm::NpmTcpStreamDirection old(config, budget);
        Sink out;
        assert(Push(old, out, 100, {}, 0, kSyn) == Error::kNone);
        assert(Push(old, out, 102, "B", 1) == Error::kNone);
        assert(old.End(EndReason::kSessionEnd, reason, out) == Error::kNone);
        assert(out.next == 2 && out.events.back().facts.session_end_reason == reason);
        assert(out.events.back().facts.end_reason == EndReason::kSessionEnd);
        npm::NpmTcpStreamDirection fresh(config, budget);
        Sink next;
        assert(Push(fresh, next, 100, "new") == Error::kNone && next.Data() == "new");
    }
    npm::NpmTcpStreamDirection empty(config, budget);
    Sink no_data;
    assert(Push(empty, no_data, 100, {}, 0, kRst) == Error::kNone);
    assert(no_data.Count(Kind::kEnd) == 1 && no_data.next == 0 &&
           empty.Origin() == npm::NpmTcpStreamOriginV1::kUnknown);
}

void TestTruncationMarksOnlyMissingBytesAndKeepsFirstDeadline() {
    auto budget = std::make_shared<Budget>();
    npm::NpmTcpStreamConfigV1 config;
    config.gap_timeout_ns = 10;
    npm::NpmTcpStreamDirection stream(config, budget);
    Sink out;
    assert(Push(stream, out, 100, {}, 0, kSyn) == Error::kNone);
    assert(Push(stream, out, 108, "H", 100) == Error::kNone);
    assert(Push(stream, out, 103, "C", 105, 0, 3) == Error::kNone);
    assert(Push(stream, out, 107, "G", 106) == Error::kNone);
    assert(stream.NextEventDeadlineNs() == 110);
    assert(stream.AdvanceWatermark(110, out) == Error::kNone);
    assert(out.Data() == "CGH" && out.next == 8);
    for (const auto& event : out.events) {
        if (event.facts.kind == Kind::kGap) {
            assert(event.facts.includes_capture_truncation == (event.facts.begin == 3));
        }
    }
    config.gap_timeout_ns = 10;
    npm::NpmTcpStreamDirection overflow(config, budget);
    Sink edge;
    assert(Push(overflow, edge, 1, {}, std::numeric_limits<int64_t>::max() - 5, 0, 1) == Error::kNone);
    assert(overflow.NextEventDeadlineNs() == std::numeric_limits<int64_t>::max());
    assert(overflow.AdvanceWatermark(std::numeric_limits<int64_t>::max(), edge) == Error::kNone);
    assert(edge.next == 1);
}

void TestBoundedFailuresAndAbort() {
    auto budget = std::make_shared<Budget>();
    npm::NpmTcpStreamConfigV1 config;
    config.max_buffered_bytes_per_direction = 65536;
    {
        npm::NpmTcpStreamDirection stream(config, budget);
        Sink sink;
        assert(Push(stream, sink, 0, {}, 0, kSyn) == Error::kNone);
        assert(Push(stream, sink, 2, std::string(65536, 'x')) == Error::kDirectionLimitExceeded);
        assert(stream.TrackedBytes() == 0 && sink.events.empty());
        assert(Push(stream, sink, 1, "A") == Error::kDirectionLimitExceeded);
    }
    assert(budget->used == 0 && budget->peak <= 65536);
    {
        npm::NpmTcpStreamDirection stream({}, budget);
        Sink sink;
        budget->limit = budget->used;
        assert(Push(stream, sink, 1, "x") == Error::kTaskBudgetExceeded);
        assert(budget->used == 0 && sink.events.empty());
    }
    budget->limit = 16 * 1024 * 1024;
    {
        npm::NpmTcpStreamDirection stream({}, budget);
        Sink sink;
        sink.fail_at = 2;
        assert(Push(stream, sink, 1, "A") == Error::kNone);
        assert(Push(stream, sink, 2, "B") == Error::kSinkError);
        assert(sink.Data() == "A" && !stream.Ended() && stream.TrackedBytes() == 0);
        assert(stream.End(EndReason::kSessionEnd, npm::NpmSessionEndReason::kEof, sink) == Error::kSinkError);
        assert(sink.Count(Kind::kEnd) == 0);
    }
    {
        npm::NpmTcpStreamDirection stream({}, budget);
        Sink sink;
        assert(Push(stream, sink, 0, {}, 0, kSyn) == Error::kNone);
        assert(Push(stream, sink, 2, "B") == Error::kNone);
        stream.Abort();
        stream.Abort();
        assert(stream.TrackedBytes() == 0 && sink.events.empty() && stream.Status() == Error::kAborted);
    }
    assert(budget->used == 0);
}

void TestWindowFinAndInvalidBoundaries() {
    auto budget = std::make_shared<Budget>();
    npm::NpmTcpStreamDirection window({}, budget);
    Sink out;
    assert(Push(window, out, 0xffffffff, {}, 0, kSyn) == Error::kNone);
    assert(Push(window, out, 0x60000000, "A") == Error::kNone);
    assert(Push(window, out, 0xc0000000, "B") == Error::kNone);
    assert(window.Ended() && out.Data() == "A" && out.next == 0x60000001);
    assert(out.events.back().facts.end_reason == EndReason::kSequenceAmbiguous);

    npm::NpmTcpStreamDirection trim({}, budget);
    Sink fin;
    assert(Push(trim, fin, 100, {}, 0, kSyn) == Error::kNone);
    assert(Push(trim, fin, 105, "EFGH") == Error::kNone);
    assert(Push(trim, fin, 107, {}, 1, kFin) == Error::kNone);
    assert(Push(trim, fin, 109, "ignored") == Error::kNone);
    assert(Push(trim, fin, 101, "ABCD") == Error::kNone);
    assert(fin.Data() == "ABCDEF" && fin.next == 6 && trim.Ended());
    assert(trim.TrackedBytes() == sizeof(npm::NpmTcpStreamDirection));

    npm::NpmTcpStreamDirection conflict({}, budget);
    Sink sequence;
    assert(Push(conflict, sequence, 100, "ABCDE") == Error::kNone);
    assert(Push(conflict, sequence, 103, {}, 1, kFin) == Error::kNone);
    assert(sequence.events.back().facts.end_reason == EndReason::kSequenceAmbiguous && sequence.Data() == "ABCDE");

    npm::NpmTcpStreamConfigV1 config;
    config.gap_timeout_ns = 10;
    npm::NpmTcpStreamDirection first_proof(config, budget);
    Sink capture;
    assert(Push(first_proof, capture, 100, {}, 0, kSyn) == Error::kNone);
    assert(Push(first_proof, capture, 104, "D", 100) == Error::kNone);
    assert(Push(first_proof, capture, 103, "C", 90) == Error::kNone);
    assert(first_proof.NextEventDeadlineNs() == 110);
    assert(first_proof.AdvanceWatermark(100, capture) == Error::kNone && capture.events.empty());
    assert(first_proof.AdvanceWatermark(110, capture) == Error::kNone && capture.Data() == "CD");
    assert(capture.events[1].facts.captured_at_ns == 90);

    config.gap_timeout_ns = -1;
    npm::NpmTcpStreamDirection invalid_config(config, budget);
    assert(invalid_config.Status() == Error::kInvalidConfig && invalid_config.TrackedBytes() == 0);
    npm::NpmTcpStreamDirection no_budget({}, {});
    assert(no_budget.Status() == Error::kInvalidConfig);
    npm::NpmTcpStreamDirection invalid({}, budget);
    Sink nothing;
    npm::NpmPacketView packet;
    assert(invalid.Push(packet, nothing) == Error::kInvalidInput && invalid.TrackedBytes() == 0);
    npm::NpmTcpStreamDirection length({}, budget);
    assert(Push(length, nothing, 100, "AB", 0, 0, 1) == Error::kInvalidInput);
    npm::NpmTcpStreamDirection terminal({}, budget);
    assert(terminal.End(EndReason::kSessionEnd, {}, nothing) == Error::kInvalidInput);
    assert(nothing.events.empty());
}

void TestDeterministicCaptureOracle() {
    std::mt19937 random(0x51a7);
    auto budget = std::make_shared<Budget>();
    for (int trial = 0; trial < 200; ++trial) {
        npm::NpmTcpStreamDirection stream({}, budget);
        Sink sink;
        const uint32_t base = trial % 2 ? 0xfffffff0U : 100U;
        assert(Push(stream, sink, base - 1, {}, 0, kSyn) == Error::kNone);
        std::vector<char> expected(128, 0);
        std::vector<int64_t> times(128, 0);
        size_t extent = 0;
        for (int64_t step = 1; step <= 30; ++step) {
            const size_t begin = random() % 100;
            const size_t wire = 1 + random() % 20;
            const size_t captured = random() % (wire + 1);
            const char value = 'a' + random() % 26;
            extent = std::max(extent, begin + wire);
            for (size_t at = begin; at < begin + captured; ++at) {
                if (expected[at] == 0) {
                    expected[at] = value;
                    times[at] = step;
                }
            }
            assert(Push(stream, sink, base + begin, std::string(captured, value), step, 0, wire) == Error::kNone);
        }
        Eof(stream, sink);
        assert(sink.next == extent && sink.Count(Kind::kEnd) == 1);
        for (const auto& event : sink.events) {
            for (auto at = event.facts.begin; at < event.facts.end; ++at) {
                if (event.facts.kind == Kind::kData) {
                    assert(event.bytes[at - event.facts.begin] == expected[at]);
                    assert(event.facts.captured_at_ns == times[at]);
                } else {
                    assert(event.facts.kind == Kind::kGap && expected[at] == 0);
                }
            }
        }
    }
    assert(budget->used == 0);
}
}  // namespace

int main() {
    TestOrderedRetransmissionAndOwnedBytes();
    TestOverlapAndMidstream();
    TestWrapAndAmbiguity();
    TestTruncationAndWatermarks();
    TestFinResetAndSessionTermination();
    TestTruncationMarksOnlyMissingBytesAndKeepsFirstDeadline();
    TestBoundedFailuresAndAbort();
    TestWindowFinAndInvalidBoundaries();
    TestDeterministicCaptureOracle();
    std::puts("NPM TCP stream: 9 test groups passed (including 200 deterministic capture traces).");
}
