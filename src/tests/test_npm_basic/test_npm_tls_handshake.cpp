// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/tls/npm_tls_handshake.h>

#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace npm = flowsql::npm;

namespace {

void U16(std::string* bytes, uint16_t value) {
    bytes->push_back(static_cast<char>(value >> 8));
    bytes->push_back(static_cast<char>(value));
}

std::string Extension(uint16_t type, const std::string& value) {
    std::string bytes;
    U16(&bytes, type);
    U16(&bytes, static_cast<uint16_t>(value.size()));
    return bytes + value;
}

std::string Client(bool tls13 = true) {
    std::string body("\x03\x03", 2);
    body += std::string(32, 'c');
    body += std::string(1, 0);
    U16(&body, 2);
    U16(&body, 0x1301);
    body += std::string("\x01\x00", 2);
    if (tls13) {
        const auto versions = Extension(43, std::string("\x02\x03\x04", 3));
        U16(&body, static_cast<uint16_t>(versions.size()));
        body += versions;
    }
    return body;
}

std::string Server(bool tls13, bool retry = false) {
    std::string body("\x03\x03", 2);
    if (retry) {
        const uint8_t random[] = {0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c,
                                  0x02, 0x1e, 0x65, 0xb8, 0x91, 0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb,
                                  0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};
        body.append(reinterpret_cast<const char*>(random), sizeof(random));
    } else {
        body += std::string(32, 's');
    }
    body += std::string(1, 0);
    U16(&body, tls13 ? 0x1301 : 0xc02f);
    body += std::string(1, 0);
    if (tls13) {
        const auto version = Extension(43, std::string("\x03\x04", 2));
        U16(&body, static_cast<uint16_t>(version.size()));
        body += version;
    }
    return body;
}

std::string Record(uint8_t content_type, uint8_t message_type, std::string body) {
    if (content_type == 22) {
        std::string header(1, static_cast<char>(message_type));
        header.push_back(static_cast<char>(body.size() >> 16));
        header.push_back(static_cast<char>(body.size() >> 8));
        header.push_back(static_cast<char>(body.size()));
        body = header + body;
    }
    std::string record(1, static_cast<char>(content_type));
    record += std::string("\x03\x03", 2);
    U16(&record, static_cast<uint16_t>(body.size()));
    return record + body;
}

struct Harness {
    npm::NpmSessionView session;
    npm::NpmTlsHandshakeTrackerV1 tracker;
    uint64_t offsets[2]{};
    std::optional<npm::NpmTlsHandshakeResultV1> result;

    explicit Harness(int64_t timeout = 100) : tracker(7, Config(timeout)) { session.session_id = 7; }
    static npm::NpmTlsConfigV1 Config(int64_t timeout) {
        npm::NpmTlsConfigV1 config;
        config.primary_label_ids = {1001};
        config.handshake_timeout_ns = timeout;
        config.max_hello_bytes = 4096;
        return config;
    }
    void Send(npm::NpmPacketDirection direction, std::string bytes, std::optional<int64_t> capture, int64_t observed) {
        const auto index = static_cast<uint8_t>(direction);
        npm::NpmTcpStreamContextV1 context;
        context.session = &session;
        context.direction = direction;
        context.origin = npm::NpmTcpStreamOriginV1::kSyn;
        context.observed_at_ns = observed;
        npm::NpmTcpStreamEventV1 event;
        event.kind = npm::NpmTcpStreamEventKindV1::kData;
        event.begin = offsets[index];
        event.end = event.begin + bytes.size();
        event.bytes = {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
        event.captured_at_ns = capture;
        offsets[index] = event.end;
        assert(tracker.Consume(context, event, &result) == npm::NpmTlsTrackerErrorV1::kNone);
    }
    void Gap(npm::NpmPacketDirection direction, bool truncated, int64_t observed) {
        const auto index = static_cast<uint8_t>(direction);
        npm::NpmTcpStreamContextV1 context;
        context.session = &session;
        context.direction = direction;
        context.origin = npm::NpmTcpStreamOriginV1::kSyn;
        context.observed_at_ns = observed;
        npm::NpmTcpStreamEventV1 event;
        event.kind = npm::NpmTcpStreamEventKindV1::kGap;
        event.begin = offsets[index];
        event.end = event.begin + 3;
        event.includes_capture_truncation = truncated;
        offsets[index] = event.end;
        assert(tracker.Consume(context, event, &result) == npm::NpmTlsTrackerErrorV1::kNone);
    }
    void End(npm::NpmPacketDirection direction, npm::NpmTcpStreamEndReasonV1 reason, int64_t observed) {
        const auto index = static_cast<uint8_t>(direction);
        npm::NpmTcpStreamContextV1 context;
        context.session = &session;
        context.direction = direction;
        context.origin = npm::NpmTcpStreamOriginV1::kSyn;
        context.observed_at_ns = observed;
        npm::NpmTcpStreamEventV1 event;
        event.kind = npm::NpmTcpStreamEventKindV1::kEnd;
        event.begin = event.end = offsets[index];
        event.end_reason = reason;
        assert(tracker.Consume(context, event, &result) == npm::NpmTlsTrackerErrorV1::kNone);
    }
    void Watermark(int64_t value) {
        assert(tracker.OnCaptureWatermark(value, &result) == npm::NpmTlsTrackerErrorV1::kNone);
    }
    void SessionEnd(npm::NpmSessionEndReason reason, int64_t observed) {
        assert(tracker.OnSessionEnd(reason, observed, &result) == npm::NpmTlsTrackerErrorV1::kNone);
    }
};

void TestTls13AndTls12() {
    using Outcome = npm::NpmTlsOutcomeV1;
    using Reason = npm::NpmTlsIncompleteReasonV1;
    Harness tls13;
    tls13.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
    assert(tls13.tracker.HasCandidate() && !tls13.result && tls13.tracker.NextEventDeadlineNs() == 110);
    tls13.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(true)), 30, 30);
    assert(tls13.result && tls13.result->outcome == Outcome::kServerHelloObserved);
    assert(tls13.result->incomplete_reason == Reason::kEncryptedAfterServerHello);
    assert(tls13.result->server_hello_latency_ns == 20);
    assert(tls13.result->handshake.final_server_hello->selected_version == 0x0304);
    assert(!tls13.result->handshake.final_server_hello->selected_alpn);
    assert(tls13.tracker.Closed() && !tls13.tracker.NextEventDeadlineNs());

    Harness tls12;
    tls12.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client(false)), 100, 100);
    tls12.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(false)), 125, 125);
    assert(!tls12.result && !tls12.tracker.Closed());
    tls12.Send(npm::NpmPacketDirection::kBToA, Record(20, 0, std::string("\x01", 1)), 140, 140);
    assert(tls12.result && tls12.result->outcome == Outcome::kServerHelloObserved);
    assert(tls12.result->incomplete_reason == Reason::kEncryptedAfterChangeCipherSpec);
    assert(tls12.result->server_hello_latency_ns == 25);
    assert(!tls12.result->handshake.final_server_hello->selected_version);
}

void TestRetryAndBadOrder() {
    using Outcome = npm::NpmTlsOutcomeV1;
    using Reason = npm::NpmTlsIncompleteReasonV1;
    Harness retry;
    retry.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
    retry.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(true, true)), 20, 20);
    assert(!retry.result);
    retry.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 25, 25);
    retry.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(true)), 40, 40);
    assert(retry.result && retry.result->outcome == Outcome::kServerHelloObserved);
    assert(retry.result->handshake.hello_retry_count == 1);
    assert(retry.result->handshake.first_client_hello->complete_at_ns == 10);
    assert(retry.result->handshake.retry_client_hello->complete_at_ns == 25);
    assert(retry.result->server_hello_latency_ns == 30);

    Harness invalid;
    invalid.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
    invalid.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(true, true)), 20, 20);
    invalid.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(true)), 30, 30);
    assert(invalid.result && invalid.result->outcome == Outcome::kIncomplete);
    assert(invalid.result->incomplete_reason == Reason::kUnsupportedFraming);
}

void TestFatalAndVisibility() {
    using Outcome = npm::NpmTlsOutcomeV1;
    using Reason = npm::NpmTlsIncompleteReasonV1;
    Harness fatal;
    fatal.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
    fatal.Send(npm::NpmPacketDirection::kAToB, Record(23, 0, std::string(100, 'e')), 11, 11);
    fatal.Send(npm::NpmPacketDirection::kBToA, Record(21, 0, std::string("\x01\x00", 2)), 12, 12);
    assert(!fatal.result && !fatal.tracker.Closed());
    fatal.Send(npm::NpmPacketDirection::kBToA, Record(21, 0, std::string("\x02\x28", 2)), 13, 13);
    assert(fatal.result && fatal.result->outcome == Outcome::kFatalAlertObserved);
    assert(fatal.result->incomplete_reason == Reason::kNone);
    assert(fatal.result->alert_level == 2 && fatal.result->alert_description == 40);
    fatal.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(true)), 14, 14);
    assert(!fatal.result);
    fatal.SessionEnd(npm::NpmSessionEndReason::kEof, 15);
    assert(!fatal.result);

    Harness encrypted;
    encrypted.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 20, 20);
    encrypted.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(true)), 30, 30);
    assert(encrypted.result && encrypted.result->outcome == Outcome::kServerHelloObserved);
    encrypted.Send(npm::NpmPacketDirection::kBToA, Record(23, 0, std::string("\x02\x28", 2)), 31, 31);
    assert(!encrypted.result);
}

void TestDeadlinesAndTime() {
    using Outcome = npm::NpmTlsOutcomeV1;
    using Reason = npm::NpmTlsIncompleteReasonV1;
    Harness deadline;
    deadline.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
    deadline.Watermark(109);
    deadline.Watermark(109);
    deadline.Watermark(90);
    assert(!deadline.result && deadline.tracker.NextEventDeadlineNs() == 110);
    deadline.Watermark(110);
    assert(deadline.result && deadline.result->outcome == Outcome::kIncomplete);
    assert(deadline.result->incomplete_reason == Reason::kServerHelloNotObservedByDeadline);
    assert(deadline.result->observed_at_ns == 110);
    deadline.Watermark(111);
    assert(!deadline.result && deadline.tracker.Closed());

    Harness after_server;
    after_server.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client(false)), 100, 100);
    after_server.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(false)), 90, 110);
    assert(!after_server.result);
    after_server.Watermark(200);
    assert(after_server.result && after_server.result->outcome == Outcome::kServerHelloObserved);
    assert(after_server.result->incomplete_reason == Reason::kDeadlineAfterServerHello);
    assert(!after_server.result->server_hello_latency_ns);

    Harness missing;
    missing.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), std::nullopt, 10);
    assert(!missing.tracker.NextEventDeadlineNs());
    missing.Watermark(100000);
    assert(!missing.result);
    missing.SessionEnd(npm::NpmSessionEndReason::kEof, 100001);
    assert(missing.result && missing.result->incomplete_reason == Reason::kSessionEnd);

    Harness saturated;
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    saturated.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), maximum - 10, maximum - 10);
    assert(saturated.tracker.NextEventDeadlineNs() == maximum);
    saturated.Watermark(maximum - 1);
    assert(!saturated.result);
    saturated.Watermark(maximum);
    assert(saturated.result && saturated.result->incomplete_reason == Reason::kServerHelloNotObservedByDeadline);

    Harness retry_timeout;
    retry_timeout.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
    retry_timeout.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(true, true)), 20, 20);
    retry_timeout.Watermark(110);
    assert(retry_timeout.result && retry_timeout.result->incomplete_reason == Reason::kHelloRetryRequestUnfinished);
}

void TestEndGapAbortAndNoCandidate() {
    using Outcome = npm::NpmTlsOutcomeV1;
    using Reason = npm::NpmTlsIncompleteReasonV1;
    Harness empty;
    empty.Send(npm::NpmPacketDirection::kAToB, "GET /", 1, 1);
    empty.SessionEnd(npm::NpmSessionEndReason::kEof, 2);
    assert(!empty.result && !empty.tracker.HasCandidate());

    Harness half_closed;
    half_closed.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client(false)), 10, 10);
    half_closed.End(npm::NpmPacketDirection::kAToB, npm::NpmTcpStreamEndReasonV1::kFin, 11);
    assert(!half_closed.result);
    half_closed.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(false)), 20, 20);
    assert(!half_closed.result);
    half_closed.SessionEnd(npm::NpmSessionEndReason::kClosed, 30);
    assert(half_closed.result && half_closed.result->outcome == Outcome::kServerHelloObserved);
    assert(half_closed.result->incomplete_reason == Reason::kSessionEndAfterServerHello);

    Harness server_fin;
    server_fin.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
    server_fin.End(npm::NpmPacketDirection::kBToA, npm::NpmTcpStreamEndReasonV1::kFin, 15);
    assert(server_fin.result && server_fin.result->incomplete_reason == Reason::kServerStreamEnd);
    server_fin.SessionEnd(npm::NpmSessionEndReason::kClosed, 16);
    assert(!server_fin.result);

    for (bool truncated : {false, true}) {
        Harness gap;
        gap.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
        gap.Gap(npm::NpmPacketDirection::kBToA, truncated, 20);
        assert(gap.result && gap.result->outcome == Outcome::kIncomplete);
        assert(gap.result->incomplete_reason == (truncated ? Reason::kCaptureTruncation : Reason::kServerPathGap));
        gap.Gap(npm::NpmPacketDirection::kAToB, false, 21);
        assert(!gap.result);
    }

    Harness client_gap;
    client_gap.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
    client_gap.Gap(npm::NpmPacketDirection::kAToB, false, 20);
    assert(client_gap.result && client_gap.result->incomplete_reason == Reason::kClientPathGap);

    Harness partial;
    partial.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()).substr(0, 9), 10, 10);
    assert(partial.tracker.HasCandidate());
    partial.SessionEnd(npm::NpmSessionEndReason::kEof, 20);
    assert(partial.result && partial.result->incomplete_reason == Reason::kClientHelloIncomplete);

    Harness gap_before_candidate;
    gap_before_candidate.Gap(npm::NpmPacketDirection::kAToB, true, 10);
    gap_before_candidate.SessionEnd(npm::NpmSessionEndReason::kEof, 20);
    assert(!gap_before_candidate.result && !gap_before_candidate.tracker.HasCandidate());

    for (const auto reason : {npm::NpmSessionEndReason::kClosed, npm::NpmSessionEndReason::kIdleTimeout,
                              npm::NpmSessionEndReason::kTupleReuse, npm::NpmSessionEndReason::kEof}) {
        Harness ended;
        ended.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
        ended.End(npm::NpmPacketDirection::kAToB, npm::NpmTcpStreamEndReasonV1::kReset, 20);
        assert(!ended.result);
        ended.SessionEnd(reason, 21);
        assert(ended.result && ended.result->incomplete_reason == Reason::kSessionEnd);
        ended.SessionEnd(reason, 22);
        assert(!ended.result);
    }

    Harness aborted;
    aborted.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
    aborted.tracker.Abort();
    assert(aborted.tracker.Closed() && !aborted.tracker.NextEventDeadlineNs());
    aborted.SessionEnd(npm::NpmSessionEndReason::kEof, 20);
    assert(!aborted.result);
}

void TestPartialAndOrdering() {
    using Outcome = npm::NpmTlsOutcomeV1;
    using Reason = npm::NpmTlsIncompleteReasonV1;
    Harness partial;
    auto client = Record(22, 1, Client());
    partial.Send(npm::NpmPacketDirection::kAToB, client.substr(0, 12), 10, 10);
    partial.Send(npm::NpmPacketDirection::kBToA, Record(21, 0, std::string("\x02\x28", 2)), 11, 11);
    assert(partial.result && partial.result->outcome == Outcome::kIncomplete);
    assert(partial.result->incomplete_reason == Reason::kClientHelloIncomplete);

    Harness malformed;
    auto bad = Record(22, 1, Client());
    bad[5 + 4 + 2 + 32 + 1 + 2 + 2] = 0;
    malformed.Send(npm::NpmPacketDirection::kAToB, bad, 10, 10);
    assert(malformed.result && malformed.result->incomplete_reason == Reason::kMalformedHello);
    malformed.Send(npm::NpmPacketDirection::kBToA, Record(21, 0, std::string("\x02\x28", 2)), 11, 11);
    assert(!malformed.result);

    Harness retry;
    retry.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 10, 10);
    retry.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(true, true)), 20, 20);
    retry.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client()), 30, 30);
    retry.SessionEnd(npm::NpmSessionEndReason::kEof, 40);
    assert(retry.result && retry.result->incomplete_reason == Reason::kHelloRetryRequestUnfinished);

    Harness tls12_alert;
    tls12_alert.Send(npm::NpmPacketDirection::kAToB, Record(22, 1, Client(false)), 10, 10);
    tls12_alert.Send(npm::NpmPacketDirection::kBToA, Record(22, 2, Server(false)), 20, 20);
    tls12_alert.Send(npm::NpmPacketDirection::kBToA, Record(21, 0, std::string("\x02\x28", 2)), 30, 30);
    assert(tls12_alert.result && tls12_alert.result->outcome == Outcome::kFatalAlertObserved);
    assert(tls12_alert.result->handshake.final_server_hello);
    assert(tls12_alert.result->server_hello_latency_ns == 10);
}

}  // namespace

int main() {
    TestTls13AndTls12();
    TestRetryAndBadOrder();
    TestFatalAndVisibility();
    TestDeadlinesAndTime();
    TestEndGapAbortAndNoCandidate();
    TestPartialAndOrdering();
}
