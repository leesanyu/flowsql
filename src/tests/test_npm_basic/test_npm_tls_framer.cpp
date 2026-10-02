// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/tls/npm_tls_framer.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace npm = flowsql::npm;

namespace {

void U16(std::string* bytes, uint16_t value) {
    bytes->push_back(static_cast<char>(value >> 8));
    bytes->push_back(static_cast<char>(value));
}

std::string Ext(uint16_t type, std::string value) {
    std::string result;
    U16(&result, type);
    U16(&result, static_cast<uint16_t>(value.size()));
    return result + value;
}

std::string ClientBody() {
    std::string body;
    U16(&body, 0x0303);
    body += std::string(32, 'c');
    body.push_back(0);
    U16(&body, 2);
    U16(&body, 0x1301);
    body.push_back(1);
    body.push_back(0);
    std::string sni;
    U16(&sni, 14);
    sni.push_back(0);
    U16(&sni, 11);
    sni += "example.com";
    std::string alpn;
    U16(&alpn, 3);
    alpn += std::string("\x02h2", 3);
    const auto extensions = Ext(0, sni) + Ext(16, alpn) + Ext(43, std::string("\x02\x03\x04", 3));
    U16(&body, static_cast<uint16_t>(extensions.size()));
    return body + extensions;
}

std::string ServerBody(bool tls13) {
    std::string body;
    U16(&body, 0x0303);
    body += std::string(32, 's');
    body.push_back(0);
    U16(&body, tls13 ? 0x1301 : 0xc02f);
    body.push_back(0);
    std::string extensions = tls13 ? Ext(43, std::string("\x03\x04", 2)) : Ext(16, std::string("\x00\x03\x02h2", 5));
    U16(&body, static_cast<uint16_t>(extensions.size()));
    return body + extensions;
}

std::string Message(uint8_t type, std::string body) {
    std::string message(1, static_cast<char>(type));
    message.push_back(static_cast<char>(body.size() >> 16));
    message.push_back(static_cast<char>(body.size() >> 8));
    message.push_back(static_cast<char>(body.size()));
    return message + body;
}

std::string Record(uint8_t type, std::string body, uint16_t length_override = 0) {
    std::string record(1, static_cast<char>(type));
    record += std::string("\x03\x03", 2);
    U16(&record, length_override == 0 ? static_cast<uint16_t>(body.size()) : length_override);
    return record + body;
}

npm::NpmTlsFramerErrorV1 Feed(npm::NpmTlsFramerV1* framer, npm::NpmPacketDirection direction, std::string bytes,
                              uint64_t* offset, std::optional<int64_t> time,
                              std::vector<npm::NpmTlsFramedEventV1>* output,
                              npm::NpmTcpStreamOriginV1 origin = npm::NpmTcpStreamOriginV1::kSyn) {
    npm::NpmTcpStreamContextV1 context;
    context.direction = direction;
    context.origin = origin;
    npm::NpmTcpStreamEventV1 event;
    event.kind = npm::NpmTcpStreamEventKindV1::kData;
    event.begin = *offset;
    event.end = *offset + bytes.size();
    event.bytes = {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    event.captured_at_ns = time;
    *offset = event.end;
    return framer->Consume(context, event, output);
}

void TestSplitHelloAndOwnedFacts() {
    using Error = npm::NpmTlsFramerErrorV1;
    npm::NpmTlsFramerV1 framer(4096);
    std::vector<npm::NpmTlsFramedEventV1> output;
    uint64_t client_offset = 0;
    uint64_t server_offset = 0;
    const auto client = Message(1, ClientBody());
    const auto first = Record(22, client.substr(0, 17));
    const auto second = Record(22, client.substr(17) + Message(11, std::string(1000, 'x')));
    const auto server_message = Message(2, ServerBody(true));
    const auto server_first = Record(22, server_message.substr(0, 9));
    const auto server_second = Record(22, server_message.substr(9));
    for (size_t index = 0; index < first.size(); ++index) {
        assert(Feed(&framer, npm::NpmPacketDirection::kAToB, first.substr(index, 1), &client_offset, 10, &output) ==
               Error::kNone);
    }
    assert(framer.HasCandidate() && output.empty());
    assert(Feed(&framer, npm::NpmPacketDirection::kAToB, second, &client_offset, 20, &output) == Error::kNone);
    assert(output.size() == 2);
    assert(output[0].kind == npm::NpmTlsFramedKindV1::kClientHello);
    assert(output[0].hello->sni == "example.com" && output[0].hello->offered_alpn[0] == "h2");
    assert(output[0].hello->complete_at_ns == 20 && output[0].first_byte_at_ns == 10);
    assert(output[1].kind == npm::NpmTlsFramedKindV1::kOtherHandshake && output[1].message_type == 11);
    assert(framer.RetainedBytes() == 0);
    assert(Feed(&framer, npm::NpmPacketDirection::kBToA, server_first, &server_offset, 30, &output) == Error::kNone);
    assert(Feed(&framer, npm::NpmPacketDirection::kBToA, server_second, &server_offset, 40, &output) == Error::kNone);
    assert(output.size() == 3 && output[2].kind == npm::NpmTlsFramedKindV1::kServerHello);
    assert(output[2].hello->selected_version == 0x0304 && output[2].hello->cipher_suite == 0x1301);
    assert(output[2].hello->complete_at_ns == 40);
}

void TestNoCandidateAndErrors() {
    using Error = npm::NpmTlsFramerErrorV1;
    std::vector<npm::NpmTlsFramedEventV1> output;
    uint64_t offset = 0;
    npm::NpmTlsFramerV1 non_tls(4096);
    assert(Feed(&non_tls, npm::NpmPacketDirection::kAToB, "GET /", &offset, 1, &output) == Error::kNotTls);
    assert(!non_tls.HasCandidate() && !non_tls.Closed() && output.empty());
    uint64_t reverse_offset = 0;
    assert(Feed(&non_tls, npm::NpmPacketDirection::kBToA, "GET /", &reverse_offset, 2, &output) == Error::kNotTls);
    assert(!non_tls.HasCandidate() && non_tls.Closed() && output.empty());
    npm::NpmTlsFramerV1 reverse_client(4096);
    offset = 0;
    reverse_offset = 0;
    assert(Feed(&reverse_client, npm::NpmPacketDirection::kAToB, "GET /", &offset, 1, &output) == Error::kNotTls);
    assert(Feed(&reverse_client, npm::NpmPacketDirection::kBToA, Record(22, Message(1, ClientBody())), &reverse_offset,
                2, &output) == Error::kNone);
    assert(reverse_client.HasCandidate() && !reverse_client.Closed() && output.size() == 1);
    output.clear();
    offset = 0;
    npm::NpmTlsFramerV1 midstream(4096);
    assert(Feed(&midstream, npm::NpmPacketDirection::kAToB, Record(22, Message(1, ClientBody())), &offset, 1, &output,
                npm::NpmTcpStreamOriginV1::kMidstream) == Error::kMidstream);
    assert(!midstream.HasCandidate());
    offset = 0;
    npm::NpmTlsFramerV1 wrong_first(4096);
    assert(Feed(&wrong_first, npm::NpmPacketDirection::kAToB,
                Record(22, Message(2, ServerBody(true)) + Message(1, ClientBody())), &offset, 1,
                &output) == Error::kNotTls);
    assert(!wrong_first.HasCandidate());
    assert(output.empty());

    offset = 0;
    npm::NpmTlsFramerV1 empty_hello(4096);
    assert(Feed(&empty_hello, npm::NpmPacketDirection::kAToB, Record(22, Message(1, "")), &offset, 1, &output) ==
           Error::kMalformedHello);
    assert(empty_hello.HasCandidate() && empty_hello.Closed());

    offset = 0;
    npm::NpmTlsFramerV1 malformed(4096);
    auto client = Record(22, Message(1, ClientBody()));
    client[5 + 4 + 2 + 32 + 1 + 2 + 2] = 0;
    assert(Feed(&malformed, npm::NpmPacketDirection::kAToB, client, &offset, 1, &output) == Error::kMalformedHello);
    assert(malformed.Closed() && malformed.RetainedBytes() == 0);
    offset = 0;
    npm::NpmTlsFramerV1 too_long(4096);
    const std::string oversized_header("\x16\x03\x03\x48\x01", 5);
    assert(Feed(&too_long, npm::NpmPacketDirection::kAToB, oversized_header, &offset, 1, &output) == Error::kNotTls);
    offset = 0;
    npm::NpmTlsFramerV1 zero_record(4096);
    assert(Feed(&zero_record, npm::NpmPacketDirection::kAToB, Record(22, ""), &offset, 1, &output) == Error::kNotTls);

    offset = 0;
    npm::NpmTlsFramerV1 gap(4096);
    assert(Feed(&gap, npm::NpmPacketDirection::kAToB, Record(22, Message(1, ClientBody())), &offset, 1, &output) ==
           Error::kNone);
    npm::NpmTcpStreamContextV1 context;
    context.direction = npm::NpmPacketDirection::kAToB;
    context.origin = npm::NpmTcpStreamOriginV1::kSyn;
    npm::NpmTcpStreamEventV1 event;
    event.kind = npm::NpmTcpStreamEventKindV1::kGap;
    event.begin = offset;
    event.end = offset + 4;
    event.includes_capture_truncation = true;
    assert(gap.Consume(context, event, &output) == Error::kCaptureTruncation && gap.Closed());
}

void TestControlAndIncomplete() {
    using Error = npm::NpmTlsFramerErrorV1;
    npm::NpmTlsFramerV1 framer(4096);
    std::vector<npm::NpmTlsFramedEventV1> output;
    uint64_t offset = 0;
    assert(Feed(&framer, npm::NpmPacketDirection::kAToB, Record(22, Message(1, ClientBody())), &offset, 1, &output) ==
           Error::kNone);
    assert(Feed(&framer, npm::NpmPacketDirection::kAToB,
                Record(20, std::string("\x01", 1)) + Record(23, std::string(1000, 'e')) +
                    Record(21, std::string("\x02\x28", 2)),
                &offset, 2, &output) == Error::kNone);
    assert(output.size() == 4 && output[1].kind == npm::NpmTlsFramedKindV1::kChangeCipherSpec);
    assert(output[1].control_bytes[0] == 1 && output[2].kind == npm::NpmTlsFramedKindV1::kApplicationData);
    assert(output[3].kind == npm::NpmTlsFramedKindV1::kAlert && output[3].control_bytes[0] == 2 &&
           output[3].control_bytes[1] == 40);
    assert(framer.RetainedBytes() == 0);
    for (int index = 0; index < 8; ++index) {
        assert(Feed(&framer, npm::NpmPacketDirection::kAToB, Record(23, std::string(18432, 'e')), &offset, 3,
                    &output) == Error::kNone);
        assert(framer.RetainedBytes() == 0);
    }

    npm::NpmTlsFramerV1 truncated(4096);
    output.clear();
    offset = 0;
    const auto partial = Record(22, Message(1, ClientBody())).substr(0, 16);
    assert(Feed(&truncated, npm::NpmPacketDirection::kAToB, partial, &offset, 1, &output) == Error::kNone);
    npm::NpmTcpStreamContextV1 context;
    context.direction = npm::NpmPacketDirection::kAToB;
    context.origin = npm::NpmTcpStreamOriginV1::kSyn;
    npm::NpmTcpStreamEventV1 end;
    end.kind = npm::NpmTcpStreamEventKindV1::kEnd;
    end.begin = end.end = offset;
    assert(truncated.Consume(context, end, &output) == Error::kIncomplete);
    assert(truncated.HasCandidate() && truncated.Closed());
}

void TestFramingBoundaries() {
    using Error = npm::NpmTlsFramerErrorV1;
    const auto client = Record(22, Message(1, ClientBody()));
    const auto server = Record(22, Message(2, ServerBody(false)));
    const auto run = [&](size_t chunk) {
        npm::NpmTlsFramerV1 framer(4096);
        std::vector<npm::NpmTlsFramedEventV1> output;
        uint64_t client_offset = 0;
        uint64_t server_offset = 0;
        assert(Feed(&framer, npm::NpmPacketDirection::kAToB, client.substr(0, 5), &client_offset, 10, &output) ==
               Error::kNone);
        for (size_t index = 5; index < client.size(); index += chunk) {
            assert(Feed(&framer, npm::NpmPacketDirection::kAToB, client.substr(index, chunk), &client_offset, 20,
                        &output) == Error::kNone);
        }
        assert(Feed(&framer, npm::NpmPacketDirection::kBToA, server.substr(0, 5), &server_offset, 30, &output) ==
               Error::kNone);
        for (size_t index = 5; index < server.size(); index += chunk) {
            assert(Feed(&framer, npm::NpmPacketDirection::kBToA, server.substr(index, chunk), &server_offset, 40,
                        &output) == Error::kNone);
        }
        assert(output.size() == 2 && output[0].hello && output[1].hello);
        assert(output[0].hello->sni == "example.com" && output[0].hello->complete_at_ns == 20);
        assert(output[1].hello->selected_alpn == "h2" && output[1].hello->complete_at_ns == 40);
        assert(framer.RetainedBytes() == 0);
    };
    run(1);
    run(7);
    run(4096);

    npm::NpmTlsFramerV1 limit(4096);
    std::vector<npm::NpmTlsFramedEventV1> output;
    uint64_t offset = 0;
    auto oversized = Message(1, std::string(4093, 'x'));
    assert(Feed(&limit, npm::NpmPacketDirection::kAToB, Record(22, oversized), &offset, 1, &output) ==
           Error::kHelloLimit);
    assert(limit.HasCandidate() && limit.Closed() && limit.RetainedBytes() == 0);

    npm::NpmTlsFramerV1 crossing(4096);
    offset = 0;
    const auto first = Record(22, client.substr(5, 2));
    const auto second = Record(23, std::string(1, 'e'));
    assert(Feed(&crossing, npm::NpmPacketDirection::kAToB, first + second, &offset, 1, &output) == Error::kNotTls);
    assert(!crossing.HasCandidate());

    npm::NpmTlsFramerV1 malformed_record(4096);
    offset = 0;
    assert(Feed(&malformed_record, npm::NpmPacketDirection::kAToB, client + std::string("\x16\x03\x03\x48\x01", 5),
                &offset, 1, &output) == Error::kMalformedRecord);
    assert(malformed_record.HasCandidate() && malformed_record.Closed());

    npm::NpmTlsFramerV1 crossed_record(4096);
    offset = 0;
    assert(Feed(&crossed_record, npm::NpmPacketDirection::kAToB,
                client + Record(22, Message(11, std::string(16, 'x')).substr(0, 6)) + Record(23, std::string(2, 'e')),
                &offset, 1, &output) == Error::kUnsupportedFraming);
    assert(crossed_record.HasCandidate() && crossed_record.Closed());

    npm::NpmTlsFramerV1 certificate(4096);
    offset = 0;
    output.clear();
    assert(Feed(&certificate, npm::NpmPacketDirection::kAToB, client, &offset, 1, &output) == Error::kNone);
    const auto large_certificate = Message(11, std::string(60000, 'c'));
    for (size_t index = 0; index < large_certificate.size(); index += 16000) {
        assert(Feed(&certificate, npm::NpmPacketDirection::kAToB, Record(22, large_certificate.substr(index, 16000)),
                    &offset, 2, &output) == Error::kNone);
        assert(certificate.RetainedBytes() == 0);
    }
    assert(output.size() == 2 && output[1].kind == npm::NpmTlsFramedKindV1::kOtherHandshake);
}

}  // namespace

int main() {
    TestSplitHelloAndOwnedFacts();
    TestNoCandidateAndErrors();
    TestControlAndIncomplete();
    TestFramingBoundaries();
}
