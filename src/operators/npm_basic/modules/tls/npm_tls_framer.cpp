// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tls_framer.h"

#include <algorithm>
#include <new>
#include <string_view>
#include <utility>

namespace flowsql::npm {
namespace {

using Error = NpmTlsFramerErrorV1;

void MarkTime(std::optional<int64_t> capture, bool* known, std::optional<int64_t>* maximum) {
    if (!capture) {
        *known = false;
    } else if (!*maximum || *capture > **maximum) {
        *maximum = *capture;
    }
}

NpmTlsFramedKindV1 RecordKind(uint8_t type) {
    if (type == 20) return NpmTlsFramedKindV1::kChangeCipherSpec;
    if (type == 21) return NpmTlsFramedKindV1::kAlert;
    return NpmTlsFramedKindV1::kApplicationData;
}

}  // namespace

NpmTlsFramerV1::NpmTlsFramerV1(uint32_t max_hello_bytes) : max_hello_bytes_(max_hello_bytes) {}

size_t NpmTlsFramerV1::RetainedBytes() const noexcept {
    return directions_[0].framing.hello_bytes.size() + directions_[1].framing.hello_bytes.size();
}

NpmTlsFramerErrorV1 NpmTlsFramerV1::Fail(NpmTlsFramerErrorV1 error) noexcept {
    closed_ = true;
    for (auto& direction : directions_) {
        direction.framing.hello_bytes.clear();
        direction.disabled = true;
    }
    return error;
}

NpmTlsFramerErrorV1 NpmTlsFramerV1::NoCandidate(Direction* direction, NpmTlsFramerErrorV1 error) noexcept {
    direction->disabled = true;
    direction->framing.hello_bytes.clear();
    if (directions_[0].disabled && directions_[1].disabled) closed_ = true;
    return error;
}

NpmTlsFramerErrorV1 NpmTlsFramerV1::FinishMessage(Direction* direction, NpmPacketDirection packet_direction,
                                                  std::vector<NpmTlsFramedEventV1>* output) {
    NpmTlsFramedEventV1 event;
    event.direction = packet_direction;
    event.message_type = direction->message_type;
    event.first_byte_at_ns = direction->message_first_at_ns;
    event.complete_at_ns = direction->message_times_known ? direction->message_max_at_ns : std::nullopt;
    if (direction->message_type == 1 || direction->message_type == 2) {
        NpmTlsHelloFactsV1 facts;
        const auto error =
            ParseNpmTlsHelloV1(direction->message_type, direction->framing.hello_bytes, event.complete_at_ns, &facts);
        if (error == NpmTlsHelloErrorV1::kLimit) return Fail(Error::kHelloLimit);
        if (error == NpmTlsHelloErrorV1::kAllocationFailed) return Fail(Error::kAllocationFailed);
        if (error != NpmTlsHelloErrorV1::kNone) return Fail(Error::kMalformedHello);
        event.kind = direction->message_type == 1 ? NpmTlsFramedKindV1::kClientHello : NpmTlsFramedKindV1::kServerHello;
        event.hello = std::move(facts);
    } else {
        event.kind = NpmTlsFramedKindV1::kOtherHandshake;
    }
    output->push_back(std::move(event));
    direction->framing.handshake_header_bytes = 0;
    direction->framing.handshake_remaining = 0;
    direction->framing.hello_bytes.clear();
    direction->message_type = 0;
    direction->message_first_at_ns.reset();
    direction->message_max_at_ns.reset();
    direction->message_times_known = true;
    return Error::kNone;
}

NpmTlsFramerErrorV1 NpmTlsFramerV1::FinishRecord(Direction* direction, NpmPacketDirection packet_direction,
                                                 std::vector<NpmTlsFramedEventV1>* output) {
    if (direction->record_type != 22) {
        NpmTlsFramedEventV1 event;
        event.direction = packet_direction;
        event.kind = RecordKind(direction->record_type);
        event.record_length = direction->record_length;
        event.control_bytes = direction->control_bytes;
        event.complete_at_ns = direction->record_times_known ? direction->record_max_at_ns : std::nullopt;
        output->push_back(std::move(event));
    }
    direction->framing.record_header_bytes = 0;
    direction->framing.record_remaining = 0;
    direction->record_type = 0;
    direction->record_length = 0;
    direction->control_bytes_seen = 0;
    direction->control_bytes = {};
    direction->record_times_known = true;
    direction->record_max_at_ns.reset();
    return Error::kNone;
}

NpmTlsFramerErrorV1 NpmTlsFramerV1::ConsumeData(Direction* direction, NpmPacketDirection packet_direction,
                                                const NpmTcpStreamEventV1& event,
                                                std::vector<NpmTlsFramedEventV1>* output) {
    const auto* bytes = event.bytes.data;
    const size_t length = event.bytes.size;
    size_t offset = 0;
    while (offset < length) {
        auto& framing = direction->framing;
        if (framing.record_header_bytes < framing.record_header.size() && framing.record_remaining == 0) {
            const size_t take =
                std::min<size_t>(framing.record_header.size() - framing.record_header_bytes, length - offset);
            for (size_t index = 0; index < take; ++index) {
                framing.record_header[framing.record_header_bytes++] = bytes[offset++];
            }
            MarkTime(event.captured_at_ns, &direction->record_times_known, &direction->record_max_at_ns);
            if (framing.record_header_bytes < framing.record_header.size()) break;
            const uint8_t type = framing.record_header[0];
            const uint8_t major = framing.record_header[1];
            const uint8_t minor = framing.record_header[2];
            const uint16_t record_length = (framing.record_header[3] << 8) | framing.record_header[4];
            if (type < 20 || type > 23 || major != 3 || minor < 1 || minor > 3 || record_length == 0 ||
                record_length > 18432) {
                return candidate_ ? Fail(Error::kMalformedRecord) : NoCandidate(direction, Error::kNotTls);
            }
            if (!candidate_ && type != 22) return NoCandidate(direction, Error::kNotTls);
            if (type != 22 && (framing.handshake_header_bytes != 0 || framing.handshake_remaining != 0)) {
                return candidate_ ? Fail(Error::kUnsupportedFraming) : NoCandidate(direction, Error::kNotTls);
            }
            framing.first_record_checked = true;
            direction->record_type = type;
            direction->record_length = record_length;
            framing.record_remaining = record_length;
            continue;
        }
        const size_t record_take = std::min<size_t>(framing.record_remaining, length - offset);
        if (direction->record_type != 22) {
            const size_t captured = std::min<size_t>(record_take, 2 - direction->control_bytes_seen);
            for (size_t index = 0; index < captured; ++index) {
                direction->control_bytes[direction->control_bytes_seen++] = bytes[offset + index];
            }
            MarkTime(event.captured_at_ns, &direction->record_times_known, &direction->record_max_at_ns);
            offset += record_take;
            framing.record_remaining -= record_take;
        } else {
            size_t available = record_take;
            while (available > 0) {
                if (framing.handshake_header_bytes < framing.handshake_header.size() &&
                    framing.handshake_remaining == 0) {
                    if (framing.handshake_header_bytes == 0) {
                        direction->message_first_at_ns = event.captured_at_ns;
                        direction->message_times_known = true;
                        direction->message_max_at_ns.reset();
                    }
                    const size_t take =
                        std::min<size_t>(framing.handshake_header.size() - framing.handshake_header_bytes, available);
                    for (size_t index = 0; index < take; ++index) {
                        framing.handshake_header[framing.handshake_header_bytes++] = bytes[offset++];
                    }
                    available -= take;
                    framing.record_remaining -= take;
                    MarkTime(event.captured_at_ns, &direction->message_times_known, &direction->message_max_at_ns);
                    if (framing.handshake_header_bytes < framing.handshake_header.size()) continue;
                    direction->message_type = framing.handshake_header[0];
                    framing.handshake_remaining = (framing.handshake_header[1] << 16) |
                                                  (framing.handshake_header[2] << 8) | framing.handshake_header[3];
                    if (!candidate_) {
                        if (direction->first_message_seen || direction->message_type != 1) {
                            return NoCandidate(direction, Error::kNotTls);
                        }
                        candidate_ = true;
                        client_direction_ = packet_direction;
                        first_client_byte_at_ns_ = direction->message_first_at_ns;
                    }
                    direction->first_message_seen = true;
                    if ((direction->message_type == 1 || direction->message_type == 2) &&
                        framing.handshake_remaining > max_hello_bytes_ - std::min<uint32_t>(max_hello_bytes_, 4)) {
                        return Fail(Error::kHelloLimit);
                    }
                    if (framing.handshake_remaining == 0) {
                        const auto error = FinishMessage(direction, packet_direction, output);
                        if (error != Error::kNone) return error;
                    }
                    continue;
                }
                const size_t take = std::min<size_t>(framing.handshake_remaining, available);
                if (direction->message_type == 1 || direction->message_type == 2) {
                    framing.hello_bytes.append(reinterpret_cast<const char*>(bytes + offset), take);
                }
                MarkTime(event.captured_at_ns, &direction->message_times_known, &direction->message_max_at_ns);
                offset += take;
                available -= take;
                framing.record_remaining -= take;
                framing.handshake_remaining -= take;
                if (framing.handshake_remaining == 0) {
                    const auto error = FinishMessage(direction, packet_direction, output);
                    if (error != Error::kNone) return error;
                }
            }
        }
        if (framing.record_remaining == 0) {
            const auto error = FinishRecord(direction, packet_direction, output);
            if (error != Error::kNone) return error;
        }
    }
    return Error::kNone;
}

NpmTlsFramerErrorV1 NpmTlsFramerV1::Consume(const NpmTcpStreamContextV1& context, const NpmTcpStreamEventV1& event,
                                            std::vector<NpmTlsFramedEventV1>* output) {
    if (output == nullptr) return Error::kInvalidInput;
    if (closed_) return Error::kNone;
    const auto direction_index = static_cast<uint8_t>(context.direction);
    if (direction_index > 1) return Error::kInvalidInput;
    auto* direction = &directions_[direction_index];
    if (direction->disabled) return Error::kNone;
    if (context.origin != NpmTcpStreamOriginV1::kSyn) {
        return candidate_ ? Fail(Error::kUnsupportedFraming) : NoCandidate(direction, Error::kMidstream);
    }
    if (event.begin != direction->next_offset || event.end < event.begin) {
        return candidate_ ? Fail(Error::kUnsupportedFraming) : NoCandidate(direction, Error::kNotTls);
    }
    if (event.kind == NpmTcpStreamEventKindV1::kGap) {
        direction->framing.gap_seen = true;
        direction->next_offset = event.end;
        const auto error = event.includes_capture_truncation ? Error::kCaptureTruncation : Error::kGap;
        return candidate_ ? Fail(error) : NoCandidate(direction, Error::kNotTls);
    }
    if (event.kind == NpmTcpStreamEventKindV1::kEnd) {
        direction->framing.ended = true;
        const auto& framing = direction->framing;
        if (candidate_ && (framing.record_header_bytes != 0 || framing.record_remaining != 0 ||
                           framing.handshake_header_bytes != 0 || framing.handshake_remaining != 0)) {
            return Fail(Error::kIncomplete);
        }
        direction->disabled = true;
        if (!candidate_ && directions_[0].disabled && directions_[1].disabled) closed_ = true;
        return Error::kNone;
    }
    if (event.kind != NpmTcpStreamEventKindV1::kData || event.bytes.empty() ||
        event.end - event.begin != event.bytes.size) {
        return Error::kInvalidInput;
    }
    direction->next_offset = event.end;
    try {
        return ConsumeData(direction, context.direction, event, output);
    } catch (const std::bad_alloc&) {
        return Fail(Error::kAllocationFailed);
    }
}

}  // namespace flowsql::npm
