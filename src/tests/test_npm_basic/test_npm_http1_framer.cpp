// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/http1/npm_http1_framer.h>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace npm = flowsql::npm;
using Error = npm::NpmHttp1FramerErrorV1;

namespace {

std::vector<npm::NpmHttp1FramedMessageV1> Feed(std::string_view input, size_t chunk_size,
                                               bool response_to_head = false) {
    npm::NpmHttp1FramerV1 framer(65536);
    std::vector<npm::NpmHttp1FramedMessageV1> messages;
    for (size_t index = 0; index < input.size(); index += chunk_size) {
        std::string owner(input.substr(index, chunk_size));
        assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, owner, static_cast<int64_t>(index + 1), &messages,
                              [response_to_head] { return npm::NpmHttp1ResponseContextV1{response_to_head, false}; }) ==
               Error::kNone);
        owner.assign(owner.size(), 'x');
        assert(framer.RetainedBytes() <= 65536);
    }
    assert(framer.OnEnd(&messages) == Error::kNone);
    std::vector<npm::NpmHttp1FramedMessageV1> combined;
    for (auto& message : messages) {
        if (message.headers_complete) {
            combined.push_back(std::move(message));
        } else {
            assert(message.body_complete && !combined.empty());
            assert(message.message_id == combined.back().message_id);
            combined.back().body_complete = true;
        }
    }
    return combined;
}

void TestBoundaries() {
    const std::string request = "POST /a%25 HTTP/1.1\r\nHost: Example.COM\r\nContent-Length: 5\r\n\r\nGET /";
    const std::string next = "GET /next HTTP/1.0\r\n\r\n";
    const std::string input = request + next;
    for (size_t chunk : {size_t{1}, size_t{2}, size_t{7}, input.size()}) {
        const auto messages = Feed(input, chunk);
        assert(messages.size() == 2);
        assert(messages[0].head.method == "POST" && messages[0].head.target == "/a%2525");
        assert(messages[0].head.host == "Example.COM");
        assert(messages[0].head.framing == npm::NpmHttp1FramingV1::kContentLength);
        assert(messages[0].body_complete);
        assert(messages[1].head.method == "GET" && messages[1].head.target == "/next");
        assert(messages[1].head.version == npm::NpmHttp1VersionV1::k10);
        assert(messages[1].head.framing == npm::NpmHttp1FramingV1::kNoBody);
    }
    const std::string chunked =
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4;foo=bar;note=\"x\\\"y\"\r\nGET!\r\n0\r\nX-End: ok\r\n\r\nGET /next HTTP/1.1\r\n\r\n";
    for (size_t chunk : {size_t{1}, size_t{3}, chunked.size()}) {
        const auto messages = Feed(chunked, chunk);
        assert(messages.size() == 2);
        assert(messages[0].head.framing == npm::NpmHttp1FramingV1::kChunked);
        assert(messages[1].head.method == "GET" && messages[1].head.target == "/next");
    }
    const auto no_body = Feed(
        "HTTP/1.1 204 No Content\r\nContent-Length: 9\r\n\r\nHTTP/1.1 205 Reset Content\r\n\r\nHTTP/1.1 304\r\n\r\n",
        1);
    assert(no_body.size() == 3 && no_body[0].head.status_code == 204 && no_body[1].head.status_code == 205 &&
           no_body[2].head.status_code == 304);
    const auto head_response = Feed("HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\nHTTP/1.1 201\r\n\r\n", 2, true);
    assert(head_response.size() == 2 && head_response[0].head.framing == npm::NpmHttp1FramingV1::kNoBody);
    const auto close_response = Feed("HTTP/1.0 200 OK\r\n\r\nGET / HTTP/1.1\r\n\r\n", 1);
    assert(close_response.size() == 1 && close_response[0].body_complete);
    assert(close_response[0].head.framing == npm::NpmHttp1FramingV1::kCloseDelimited);
    for (const auto& response : {std::string("HTTP/1.1 101 Switching Protocols\r\n\r\n"),
                                 std::string("HTTP/1.1 200 Connection Established\r\n\r\n")}) {
        npm::NpmHttp1FramerV1 tunnel(1024);
        std::vector<npm::NpmHttp1FramedMessageV1> messages;
        const bool connect = response.find("200") != std::string::npos;
        assert(tunnel.Consume(npm::NpmTcpStreamOriginV1::kSyn, response + "not HTTP tunnel bytes", 1, &messages,
                              [connect] { return npm::NpmHttp1ResponseContextV1{false, connect}; }) == Error::kNone);
        assert(messages.size() == 2 && messages[0].headers_complete && messages[1].body_complete);
        assert(messages[0].message_id == messages[1].message_id);
        assert(messages[0].head.framing == npm::NpmHttp1FramingV1::kNoBody);
        assert(tunnel.OnEnd(&messages) == Error::kNone);
    }
}

void TestStreamEvents() {
    npm::NpmHttp1FramerV1 framer(1024);
    std::vector<npm::NpmHttp1FramedMessageV1> messages;
    std::string first = "GET / HTTP/1.1\r\nHo";
    npm::NpmTcpStreamEventV1 event;
    event.kind = npm::NpmTcpStreamEventKindV1::kData;
    event.bytes = {reinterpret_cast<const uint8_t*>(first.data()), first.size()};
    event.captured_at_ns = 10;
    assert(framer.Consume(event, npm::NpmTcpStreamOriginV1::kSyn, &messages) == Error::kNone);
    first.assign(first.size(), 'x');
    std::string second = "st: test\r\n\r\n";
    event.bytes = {reinterpret_cast<const uint8_t*>(second.data()), second.size()};
    event.captured_at_ns = 20;
    assert(framer.Consume(event, npm::NpmTcpStreamOriginV1::kSyn, &messages) == Error::kNone);
    assert(messages.size() == 2 && messages[0].head.host == "test" && messages[1].body_complete);
    assert(messages[0].head.complete_at_ns == 20);
    event.kind = npm::NpmTcpStreamEventKindV1::kGap;
    event.bytes = {};
    assert(framer.Consume(event, npm::NpmTcpStreamOriginV1::kSyn, &messages) == Error::kGap);
    event.kind = npm::NpmTcpStreamEventKindV1::kEnd;
    assert(framer.Consume(event, npm::NpmTcpStreamOriginV1::kSyn, &messages) == Error::kNone);
    assert(framer.Disabled());
}

void TestTimesAndOwnership() {
    npm::NpmHttp1FramerV1 framer(1024);
    std::vector<npm::NpmHttp1FramedMessageV1> messages;
    std::string first = "GET / HTTP/1.1\r\nHost: ex";
    assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, first, 100, &messages) == Error::kNone);
    first.clear();
    assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, "ample\r\n\r\n", 90, &messages) == Error::kNone);
    assert(messages.size() == 2 && messages[0].head.host == "example" && messages[1].body_complete);
    assert(messages[0].head.complete_at_ns == 100);
    assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, "GET /2 HTTP/1.1\r\n", std::nullopt, &messages) ==
           Error::kNone);
    assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, "\r\n", 200, &messages) == Error::kNone);
    assert(messages.size() == 4 && !messages[2].head.complete_at_ns && messages[3].body_complete);
}

void TestHeaderBeforeBody() {
    npm::NpmHttp1FramerV1 framer(1024);
    std::vector<npm::NpmHttp1FramedMessageV1> messages;
    assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, "POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\nAB", 10,
                          &messages) == Error::kNone);
    assert(messages.size() == 1 && messages[0].headers_complete && !messages[0].body_complete);
    assert(messages[0].head.method == "POST" && messages[0].head.complete_at_ns == 10);
    assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, "CDGET / HTTP/1.1\r\n\r\n", 20, &messages) == Error::kNone);
    assert(messages.size() == 4);
    assert(messages[1].body_complete && messages[1].message_id == messages[0].message_id);
    assert(messages[2].head.method == "GET" && messages[2].message_id != messages[0].message_id);
    assert(messages[3].body_complete && messages[3].message_id == messages[2].message_id);
    npm::NpmHttp1FramerV1 close(1024);
    messages.clear();
    assert(close.Consume(npm::NpmTcpStreamOriginV1::kSyn, "HTTP/1.0 200\r\n\r\nbody", 30, &messages) == Error::kNone);
    assert(messages.size() == 1 && messages[0].headers_complete && !messages[0].body_complete);
    assert(close.OnEnd(&messages) == Error::kNone);
    assert(messages.size() == 2 && messages[1].body_complete && messages[1].message_id == messages[0].message_id);
}

void TestIncompleteEndAndControlLimit() {
    for (const char* input : {
             "GET / HTTP/1.1\r\nHost: incomplete",
             "POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\nAB",
             "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nAB",
             "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX: incomplete",
         }) {
        npm::NpmHttp1FramerV1 framer(1024);
        std::vector<npm::NpmHttp1FramedMessageV1> messages;
        assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, input, 1, &messages) == Error::kNone);
        assert(framer.OnEnd(&messages) == Error::kIncomplete && framer.Disabled());
        for (const auto& message : messages) assert(!message.body_complete);
    }
    const std::string header = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    const size_t budget = header.size() + std::string("0\r\n").size() + 2;
    npm::NpmHttp1FramerV1 framer(static_cast<uint32_t>(budget));
    std::vector<npm::NpmHttp1FramedMessageV1> messages;
    assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, header + "0\r\nX: a\r\n\r\n", 1, &messages) ==
           Error::kHeaderLimit);
    assert(messages.size() == 1 && messages[0].headers_complete && !messages[0].body_complete);
    assert(framer.Disabled());
    assert(framer.RetainedBytes() == 0);
}

void ExpectRejected(std::string_view input, Error expected, uint32_t limit = 65536) {
    npm::NpmHttp1FramerV1 framer(limit);
    std::vector<npm::NpmHttp1FramedMessageV1> messages;
    assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, input, 100, &messages) == expected);
    assert(framer.Disabled());
    for (const auto& message : messages) assert(!message.body_complete);
    assert(framer.Consume(npm::NpmTcpStreamOriginV1::kSyn, "GET / HTTP/1.1\r\n\r\n", 101, &messages) != Error::kNone);
    for (const auto& message : messages) assert(!message.body_complete);
}

void TestRejections() {
    ExpectRejected("GET / HTTP/1.1\n\n", Error::kMalformed);
    ExpectRejected("GET / HTTP/1.1\r\n X: y\r\n\r\n", Error::kMalformed);
    ExpectRejected("GET / HTTP/1.1\r\nX : y\r\n\r\n", Error::kMalformed);
    ExpectRejected("GET / HTTP/1.1\r\nX: \x01\r\n\r\n", Error::kMalformed);
    ExpectRejected("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", Error::kMalformed);
    ExpectRejected("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx", Error::kMalformed);
    ExpectRejected("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n", Error::kMalformed);
    ExpectRejected("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
                   Error::kMalformed);
    ExpectRejected("POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n", Error::kUnsupported);
    ExpectRejected("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", Error::kMalformed);
    ExpectRejected("POST / HTTP/1.1\r\nContent-Length: 18446744073709551616\r\n\r\n", Error::kMalformed);
    ExpectRejected("NOT HTTP\r\n\r\nGET / HTTP/1.1\r\n\r\n", Error::kMalformed);
    ExpectRejected("GET / HTTP/2\r\n\r\n", Error::kUnsupported);
    ExpectRejected(std::string("GET / HTTP/1.1\r\nX: ") + std::string(1024, 'a') + "\r\n\r\n", Error::kHeaderLimit,
                   1024);
    ExpectRejected("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nX\r\n", Error::kMalformed);
    ExpectRejected("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1\r\naX", Error::kMalformed);
    ExpectRejected("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n X: y\r\n\r\n", Error::kMalformed);
    ExpectRejected("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1;=bad\r\na\r\n0\r\n\r\n", Error::kMalformed);
    ExpectRejected("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1;foo=\"open\r\na\r\n0\r\n\r\n",
                   Error::kMalformed);
    ExpectRejected(
        std::string("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") + std::string(1000, '0') + "\r\n\r\n",
        Error::kHeaderLimit, 1024);
    npm::NpmHttp1FramerV1 midstream(1024);
    std::vector<npm::NpmHttp1FramedMessageV1> messages;
    assert(midstream.Consume(npm::NpmTcpStreamOriginV1::kMidstream, "GET / HTTP/1.1\r\n\r\n", 1, &messages) ==
           Error::kMidstream);
    assert(midstream.Disabled() && messages.empty());
    npm::NpmHttp1FramerV1 gap(1024);
    assert(gap.OnGap() == Error::kGap);
    assert(gap.Consume(npm::NpmTcpStreamOriginV1::kSyn, "GET / HTTP/1.1\r\n\r\n", 1, &messages) != Error::kNone);
    assert(messages.empty());
}

}  // namespace

int main() {
    TestBoundaries();
    TestStreamEvents();
    TestTimesAndOwnership();
    TestHeaderBeforeBody();
    TestIncompleteEndAndControlLimit();
    TestRejections();
}
