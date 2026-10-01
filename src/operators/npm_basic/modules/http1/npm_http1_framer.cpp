// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_http1_framer.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

using Error = NpmHttp1FramerErrorV1;

bool TokenByte(unsigned char c) {
    if (c >= '0' && c <= '9') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    return std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
}

bool Printable(unsigned char c) { return c >= 0x20 && c <= 0x7e; }

std::string_view TrimOws(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

bool EqualsAsciiNoCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        unsigned char a = static_cast<unsigned char>(left[index]);
        unsigned char b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}

std::string EscapeDisplay(std::string_view raw) {
    std::string value;
    value.reserve(raw.size());
    for (char byte : raw) {
        if (byte == '%')
            value += "%25";
        else
            value += byte;
    }
    return value;
}

bool ParseDecimal(std::string_view text, uint64_t* value) {
    if (text.empty()) return false;
    uint64_t number = 0;
    for (unsigned char digit : text) {
        if (digit < '0' || digit > '9') return false;
        const uint64_t next = digit - '0';
        if (number > (std::numeric_limits<uint64_t>::max() - next) / 10) return false;
        number = number * 10 + next;
    }
    *value = number;
    return true;
}

bool ParseHex(std::string_view text, uint64_t* value) {
    if (text.empty()) return false;
    uint64_t number = 0;
    for (unsigned char c : text) {
        uint8_t digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return false;
        if (number > (std::numeric_limits<uint64_t>::max() - digit) / 16) return false;
        number = number * 16 + digit;
    }
    *value = number;
    return true;
}

bool ParseChunkLine(std::string_view line, uint64_t* length) {
    const size_t separator = line.find(';');
    if (!ParseHex(line.substr(0, separator), length)) return false;
    size_t cursor = separator;
    while (cursor != std::string_view::npos && cursor < line.size()) {
        if (line[cursor++] != ';') return false;
        const size_t name_begin = cursor;
        while (cursor < line.size() && TokenByte(static_cast<unsigned char>(line[cursor]))) ++cursor;
        if (cursor == name_begin) return false;
        if (cursor < line.size() && line[cursor] == '=') {
            ++cursor;
            if (cursor < line.size() && line[cursor] == '"') {
                ++cursor;
                bool closed = false;
                while (cursor < line.size()) {
                    const unsigned char byte = static_cast<unsigned char>(line[cursor++]);
                    if (byte == '"') {
                        closed = true;
                        break;
                    }
                    if (byte == '\\') {
                        if (cursor == line.size() || !Printable(static_cast<unsigned char>(line[cursor++])))
                            return false;
                    } else if (byte != '\t' && !Printable(byte)) {
                        return false;
                    }
                }
                if (!closed) return false;
            } else {
                const size_t value_begin = cursor;
                while (cursor < line.size() && TokenByte(static_cast<unsigned char>(line[cursor]))) ++cursor;
                if (cursor == value_begin) return false;
            }
        }
        if (cursor == line.size()) break;
        if (line[cursor] != ';') return false;
    }
    return true;
}

Error ParseField(std::string_view line, std::string_view* name, std::string_view* value) {
    if (line.empty() || line.front() == ' ' || line.front() == '\t') return Error::kMalformed;
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return Error::kMalformed;
    *name = line.substr(0, colon);
    for (unsigned char c : *name) {
        if (!TokenByte(c)) return Error::kMalformed;
    }
    *value = line.substr(colon + 1);
    for (unsigned char c : *value) {
        if (c != '\t' && !Printable(c)) return Error::kMalformed;
    }
    *value = TrimOws(*value);
    return Error::kNone;
}

Error ParseStartLine(std::string_view line, NpmHttp1MessageHeadV1* head) {
    if (line.rfind("HTTP/", 0) == 0) {
        head->is_response = true;
        if (line.rfind("HTTP/1.0 ", 0) == 0)
            head->version = NpmHttp1VersionV1::k10;
        else if (line.rfind("HTTP/1.1 ", 0) == 0)
            head->version = NpmHttp1VersionV1::k11;
        else
            return Error::kUnsupported;
        if (line.size() < 12) return Error::kMalformed;
        const std::string_view code = line.substr(9, 3);
        uint64_t status = 0;
        if (!ParseDecimal(code, &status) || status < 100 || status > 599) return Error::kMalformed;
        if (line.size() > 12 && line[12] != ' ') return Error::kMalformed;
        for (unsigned char c : line.substr(std::min<size_t>(13, line.size()))) {
            if (!Printable(c)) return Error::kMalformed;
        }
        head->status_code = static_cast<uint16_t>(status);
        return Error::kNone;
    }
    const size_t first = line.find(' ');
    if (first == std::string_view::npos || first == 0) return Error::kMalformed;
    const size_t second = line.find(' ', first + 1);
    if (second == std::string_view::npos || second == first + 1) return Error::kMalformed;
    const std::string_view version = line.substr(second + 1);
    if (version == "HTTP/1.0")
        head->version = NpmHttp1VersionV1::k10;
    else if (version == "HTTP/1.1")
        head->version = NpmHttp1VersionV1::k11;
    else
        return Error::kUnsupported;
    for (unsigned char c : line.substr(0, first)) {
        if (!TokenByte(c)) return Error::kMalformed;
    }
    const std::string_view target = line.substr(first + 1, second - first - 1);
    for (unsigned char c : target) {
        if (c < 0x21 || c > 0x7e) return Error::kMalformed;
    }
    head->method = std::string(line.substr(0, first));
    head->target = EscapeDisplay(target);
    return Error::kNone;
}

}  // namespace

NpmHttp1FramerV1::NpmHttp1FramerV1(uint32_t max_header_bytes) : max_header_bytes_(max_header_bytes) {}

NpmHttp1FramerErrorV1 NpmHttp1FramerV1::Fail(NpmHttp1FramerErrorV1 error) noexcept {
    stage_ = Stage::kDisabled;
    header_.clear();
    line_.clear();
    current_ = {};
    return error;
}

NpmHttp1FramerErrorV1 NpmHttp1FramerV1::AppendControlByte(char byte, std::string* target) {
    if (control_bytes_seen_ >= max_header_bytes_) return Fail(Error::kHeaderLimit);
    ++control_bytes_seen_;
    target->push_back(byte);
    const size_t size = target->size();
    if (byte == '\n' && (size < 2 || (*target)[size - 2] != '\r')) return Fail(Error::kMalformed);
    if (size >= 2 && (*target)[size - 2] == '\r' && byte != '\n') return Fail(Error::kMalformed);
    return Error::kNone;
}

NpmHttp1FramerErrorV1 NpmHttp1FramerV1::ParseHeaders(const std::function<NpmHttp1ResponseContextV1()>& response_context,
                                                     std::vector<NpmHttp1FramedMessageV1>* messages) {
    NpmHttp1MessageHeadV1 head;
    size_t begin = 0;
    size_t end = header_.find("\r\n");
    if (end == std::string::npos || end == 0) return Fail(Error::kMalformed);
    Error error = ParseStartLine(std::string_view(header_).substr(0, end), &head);
    if (error != Error::kNone) return Fail(error);
    begin = end + 2;
    bool content_length_seen = false;
    bool transfer_encoding_seen = false;
    bool host_seen = false;
    bool transfer_encoding_supported = true;
    uint64_t content_length = 0;
    while (begin < header_.size() - 2) {
        end = header_.find("\r\n", begin);
        if (end == std::string::npos || end == begin) return Fail(Error::kMalformed);
        std::string_view name;
        std::string_view value;
        error = ParseField(std::string_view(header_).substr(begin, end - begin), &name, &value);
        if (error != Error::kNone) return Fail(error);
        if (EqualsAsciiNoCase(name, "host")) {
            if (host_seen) return Fail(Error::kMalformed);
            host_seen = true;
            head.host = EscapeDisplay(value);
        } else if (EqualsAsciiNoCase(name, "content-length")) {
            if (content_length_seen || !ParseDecimal(value, &content_length)) return Fail(Error::kMalformed);
            content_length_seen = true;
        } else if (EqualsAsciiNoCase(name, "transfer-encoding")) {
            if (transfer_encoding_seen) return Fail(Error::kMalformed);
            transfer_encoding_seen = true;
            transfer_encoding_supported = EqualsAsciiNoCase(value, "chunked");
        }
        begin = end + 2;
    }
    if (content_length_seen && transfer_encoding_seen) return Fail(Error::kMalformed);
    if (transfer_encoding_seen && !transfer_encoding_supported) return Fail(Error::kUnsupported);
    head.complete_at_ns = header_times_known_ ? max_header_time_ns_ : std::nullopt;
    const NpmHttp1ResponseContextV1 request =
        head.is_response && response_context ? response_context() : NpmHttp1ResponseContextV1{};
    const uint16_t status = head.status_code.value_or(0);
    const bool connect_tunnel = request.request_is_connect && status >= 200 && status < 300;
    stop_after_current_ = head.is_response && (status == 101 || connect_tunnel);
    const bool no_body_response = head.is_response && (request.request_is_head || connect_tunnel || status < 200 ||
                                                       status == 204 || status == 205 || status == 304);
    if (no_body_response)
        head.framing = NpmHttp1FramingV1::kNoBody;
    else if (transfer_encoding_seen)
        head.framing = NpmHttp1FramingV1::kChunked;
    else if (content_length_seen)
        head.framing = NpmHttp1FramingV1::kContentLength;
    else if (head.is_response)
        head.framing = NpmHttp1FramingV1::kCloseDelimited;
    else
        head.framing = NpmHttp1FramingV1::kNoBody;
    current_ = std::move(head);
    header_.clear();
    if (next_message_id_ == 0) return Fail(Error::kUnsupported);
    current_message_id_ = next_message_id_++;
    messages->push_back({current_message_id_, current_, true, false});
    if (current_.framing == NpmHttp1FramingV1::kNoBody ||
        (current_.framing == NpmHttp1FramingV1::kContentLength && content_length == 0)) {
        return FinishMessage(messages);
    }
    if (current_.framing == NpmHttp1FramingV1::kContentLength) {
        remaining_ = content_length;
        stage_ = Stage::kFixedBody;
    } else if (current_.framing == NpmHttp1FramingV1::kChunked) {
        stage_ = Stage::kChunkLine;
    } else {
        stage_ = Stage::kCloseBody;
    }
    return Error::kNone;
}

NpmHttp1FramerErrorV1 NpmHttp1FramerV1::FinishMessage(std::vector<NpmHttp1FramedMessageV1>* messages) {
    messages->push_back({current_message_id_, {}, false, true});
    current_ = {};
    current_message_id_ = 0;
    stage_ = stop_after_current_ ? Stage::kCloseBody : Stage::kHeaders;
    stop_after_current_ = false;
    control_bytes_seen_ = 0;
    header_times_known_ = true;
    max_header_time_ns_.reset();
    return Error::kNone;
}

NpmHttp1FramerErrorV1 NpmHttp1FramerV1::Consume(NpmTcpStreamOriginV1 origin, std::string_view bytes,
                                                std::optional<int64_t> captured_at_ns,
                                                std::vector<NpmHttp1FramedMessageV1>* messages,
                                                const std::function<NpmHttp1ResponseContextV1()>& response_context) {
    if (!messages) return Error::kInvalidOutput;
    if (stage_ == Stage::kDisabled) return Error::kIncomplete;
    if (!started_) {
        if (origin != NpmTcpStreamOriginV1::kSyn) return Fail(Error::kMidstream);
        started_ = true;
    }
    try {
        size_t index = 0;
        while (index < bytes.size()) {
            if (stage_ == Stage::kCloseBody) return Error::kNone;
            if (stage_ == Stage::kFixedBody || stage_ == Stage::kChunkBody) {
                const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining_, bytes.size() - index));
                index += count;
                remaining_ -= count;
                if (remaining_ == 0) {
                    if (stage_ == Stage::kFixedBody)
                        FinishMessage(messages);
                    else
                        stage_ = Stage::kChunkCr;
                }
                continue;
            }
            const char byte = bytes[index++];
            if (stage_ == Stage::kChunkCr) {
                if (byte != '\r') return Fail(Error::kMalformed);
                stage_ = Stage::kChunkLf;
                continue;
            }
            if (stage_ == Stage::kChunkLf) {
                if (byte != '\n') return Fail(Error::kMalformed);
                stage_ = Stage::kChunkLine;
                continue;
            }
            std::string* target = stage_ == Stage::kHeaders ? &header_ : &line_;
            Error error = AppendControlByte(byte, target);
            if (error != Error::kNone) return error;
            if (stage_ == Stage::kHeaders) {
                if (!captured_at_ns)
                    header_times_known_ = false;
                else if (!max_header_time_ns_ || *max_header_time_ns_ < *captured_at_ns)
                    max_header_time_ns_ = *captured_at_ns;
                if (header_.size() >= 4 && header_.compare(header_.size() - 4, 4, "\r\n\r\n") == 0) {
                    error = ParseHeaders(response_context, messages);
                    if (error != Error::kNone) return error;
                }
            } else if (line_.size() >= 2 && line_.compare(line_.size() - 2, 2, "\r\n") == 0) {
                const std::string_view content(line_.data(), line_.size() - 2);
                if (stage_ == Stage::kChunkLine) {
                    uint64_t length = 0;
                    if (!ParseChunkLine(content, &length)) return Fail(Error::kMalformed);
                    stage_ = length == 0 ? Stage::kTrailers : Stage::kChunkBody;
                    remaining_ = length;
                } else {
                    if (content.empty()) {
                        line_.clear();
                        FinishMessage(messages);
                        continue;
                    }
                    std::string_view name;
                    std::string_view value;
                    if (ParseField(content, &name, &value) != Error::kNone) return Fail(Error::kMalformed);
                }
                line_.clear();
            }
        }
        return Error::kNone;
    } catch (const std::bad_alloc&) {
        return Fail(Error::kAllocationFailed);
    }
}

NpmHttp1FramerErrorV1 NpmHttp1FramerV1::Consume(const NpmTcpStreamEventV1& event, NpmTcpStreamOriginV1 origin,
                                                std::vector<NpmHttp1FramedMessageV1>* messages,
                                                const std::function<NpmHttp1ResponseContextV1()>& response_context) {
    if (event.kind == NpmTcpStreamEventKindV1::kGap) return OnGap();
    if (event.kind == NpmTcpStreamEventKindV1::kEnd) return OnEnd(messages);
    if (event.kind != NpmTcpStreamEventKindV1::kData || !event.bytes.data || event.bytes.size == 0) {
        return Fail(Error::kMalformed);
    }
    return Consume(origin, {reinterpret_cast<const char*>(event.bytes.data), event.bytes.size}, event.captured_at_ns,
                   messages, response_context);
}

NpmHttp1FramerErrorV1 NpmHttp1FramerV1::OnGap() noexcept { return Fail(Error::kGap); }

NpmHttp1FramerErrorV1 NpmHttp1FramerV1::OnEnd(std::vector<NpmHttp1FramedMessageV1>* messages) noexcept {
    if (stage_ == Stage::kDisabled) return Error::kNone;
    if (!messages) return Error::kInvalidOutput;
    if (stage_ == Stage::kCloseBody) {
        if (current_message_id_ != 0) {
            try {
                messages->push_back({current_message_id_, {}, false, true});
            } catch (const std::bad_alloc&) {
                return Fail(Error::kAllocationFailed);
            }
        }
        stage_ = Stage::kDisabled;
        return Error::kNone;
    }
    if (stage_ == Stage::kHeaders && header_.empty()) {
        stage_ = Stage::kDisabled;
        return Error::kNone;
    }
    return Fail(Error::kIncomplete);
}

}  // namespace flowsql::npm
