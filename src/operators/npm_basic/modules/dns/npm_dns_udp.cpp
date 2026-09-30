// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_dns_udp.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

bool Read16(Span<const uint8_t> body, size_t* offset, uint16_t* value) {
    if (*offset > body.size || body.size - *offset < 2) return false;
    *value = static_cast<uint16_t>((static_cast<uint16_t>(body.data[*offset]) << 8) | body.data[*offset + 1]);
    *offset += 2;
    return true;
}

bool Read32(Span<const uint8_t> body, size_t* offset, uint32_t* value) {
    uint16_t high = 0, low = 0;
    if (!Read16(body, offset, &high) || !Read16(body, offset, &low)) return false;
    *value = (static_cast<uint32_t>(high) << 16) | low;
    return true;
}

uint8_t FoldAscii(uint8_t byte) { return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte; }

void AppendDisplayByte(uint8_t byte, std::string* display) {
    if (byte >= 33 && byte <= 126 && byte != '.' && byte != '\\') {
        display->push_back(static_cast<char>(byte));
    } else if (byte == '.' || byte == '\\') {
        display->push_back('\\');
        display->push_back(static_cast<char>(byte));
    } else {
        display->push_back('\\');
        display->push_back(static_cast<char>('0' + byte / 100));
        display->push_back(static_cast<char>('0' + (byte / 10) % 10));
        display->push_back(static_cast<char>('0' + byte % 10));
    }
}

bool ReadName(Span<const uint8_t> body, size_t* offset, std::string* wire, std::string* display) {
    size_t cursor = *offset;
    bool jumped = false;
    size_t expanded = 1;
    size_t labels = 0;
    size_t pointers = 0;
    while (cursor < body.size) {
        const uint8_t length = body.data[cursor];
        if ((length & 0xc0) == 0xc0) {
            if (++pointers > 255) return false;
            if (body.size - cursor < 2) return false;
            const size_t target = (static_cast<size_t>(length & 0x3f) << 8) | body.data[cursor + 1];
            if (target >= cursor || target >= body.size) return false;
            if (!jumped) *offset = cursor + 2;
            cursor = target;
            jumped = true;
            continue;
        }
        if ((length & 0xc0) != 0 || length > 63) return false;
        ++cursor;
        if (length == 0) {
            if (!jumped) *offset = cursor;
            if (wire) wire->push_back('\0');
            if (display && labels == 0) *display = ".";
            return true;
        }
        if (body.size - cursor < length || expanded + 1 + length > 255) return false;
        expanded += 1 + length;
        if (wire) wire->push_back(static_cast<char>(length));
        if (display && labels != 0) display->push_back('.');
        for (size_t index = 0; index < length; ++index) {
            const uint8_t byte = body.data[cursor + index];
            if (wire) wire->push_back(static_cast<char>(FoldAscii(byte)));
            if (display) AppendDisplayByte(byte, display);
        }
        cursor += length;
        ++labels;
    }
    return false;
}

NpmPacketDirection Opposite(NpmPacketDirection direction) {
    return direction == NpmPacketDirection::kAToB ? NpmPacketDirection::kBToA : NpmPacketDirection::kAToB;
}

bool SameQuestion(const NpmDnsQuestionKeyV1& left, const NpmDnsQuestionKeyV1& right) {
    return left.session_id == right.session_id && left.query_direction == right.query_direction &&
           left.dns_id == right.dns_id && left.opcode == right.opcode && left.wire_qname == right.wire_qname &&
           left.qtype == right.qtype && left.qclass == right.qclass;
}

NpmDnsUdpResultV1 QueryResult(const NpmDnsTransactionV1& transaction, const std::string& display,
                              int64_t observed_at_ns, std::string reason) {
    NpmDnsUdpResultV1 result;
    result.entity_instance_id = transaction.entity_instance_id;
    result.session_id = transaction.key.session_id;
    result.dns_id = transaction.key.dns_id;
    result.outcome = "query_only";
    result.incomplete_reason = std::move(reason);
    result.query_retries = transaction.query_retries;
    result.query_direction = transaction.key.query_direction;
    result.qname = display;
    result.qtype = transaction.key.qtype;
    result.qclass = transaction.key.qclass;
    result.query_at_ns = transaction.query_at_ns;
    result.observed_at_ns = observed_at_ns;
    return result;
}

}  // namespace

NpmDnsMessageStatusV1 ParseNpmDnsUdpMessageV1(Span<const uint8_t> body, bool body_complete,
                                              NpmDnsParsedMessageV1* output) {
    NpmDnsParsedMessageV1 parsed;
    if (!output || (body.size != 0 && !body.data) || body.size > 65535 || body.size < 12) {
        if (output) *output = std::move(parsed);
        return NpmDnsMessageStatusV1::kMalformed;
    }
    size_t offset = 0;
    uint16_t flags = 0, questions = 0, answers = 0, authority = 0, additional = 0;
    Read16(body, &offset, &parsed.message.dns_id);
    Read16(body, &offset, &flags);
    Read16(body, &offset, &questions);
    Read16(body, &offset, &answers);
    Read16(body, &offset, &authority);
    Read16(body, &offset, &additional);
    parsed.message.is_response = (flags & 0x8000) != 0;
    parsed.message.opcode = static_cast<uint8_t>((flags >> 11) & 0x0f);
    parsed.message.truncated = (flags & 0x0200) != 0;
    parsed.message.response_rcode = flags & 0x000f;
    if (questions != 1) {
        *output = std::move(parsed);
        return NpmDnsMessageStatusV1::kMalformed;
    }
    std::string wire;
    if (!ReadName(body, &offset, &wire, &parsed.qname_display)) {
        *output = std::move(parsed);
        return NpmDnsMessageStatusV1::kMalformed;
    }
    uint16_t qtype = 0, qclass = 0;
    if (!Read16(body, &offset, &qtype) || !Read16(body, &offset, &qclass)) {
        *output = std::move(parsed);
        return NpmDnsMessageStatusV1::kMalformed;
    }
    parsed.message.wire_qname = std::move(wire);
    parsed.message.qtype = qtype;
    parsed.message.qclass = qclass;
    parsed.key_available = true;
    if (!body_complete) {
        *output = std::move(parsed);
        return NpmDnsMessageStatusV1::kCaptureTruncated;
    }

    bool seen_opt = false;
    const uint16_t counts[] = {answers, authority, additional};
    for (size_t section = 0; section < 3; ++section) {
        for (uint16_t index = 0; index < counts[section]; ++index) {
            std::string owner;
            if (!ReadName(body, &offset, &owner, nullptr)) {
                *output = std::move(parsed);
                return NpmDnsMessageStatusV1::kMalformed;
            }
            uint16_t type = 0, rr_class = 0, rdlength = 0;
            uint32_t ttl = 0;
            if (!Read16(body, &offset, &type) || !Read16(body, &offset, &rr_class) || !Read32(body, &offset, &ttl) ||
                !Read16(body, &offset, &rdlength) || offset > body.size || body.size - offset < rdlength) {
                *output = std::move(parsed);
                return NpmDnsMessageStatusV1::kMalformed;
            }
            if (type == 41) {
                if (section != 2 || seen_opt || owner != std::string(1, '\0') || ((ttl >> 16) & 0xff) != 0) {
                    *output = std::move(parsed);
                    return NpmDnsMessageStatusV1::kMalformed;
                }
                seen_opt = true;
                size_t option = offset;
                const size_t end = offset + rdlength;
                while (option < end) {
                    uint16_t code = 0, length = 0;
                    if (end - option < 4 || !Read16(body, &option, &code) || !Read16(body, &option, &length) ||
                        end - option < length) {
                        *output = std::move(parsed);
                        return NpmDnsMessageStatusV1::kMalformed;
                    }
                    option += length;
                }
                parsed.message.response_rcode |= static_cast<uint16_t>((ttl >> 24) << 4);
            }
            offset += rdlength;
        }
    }
    if (offset != body.size) {
        *output = std::move(parsed);
        return NpmDnsMessageStatusV1::kMalformed;
    }
    const bool unsupported = parsed.message.opcode != 0 || qtype == 251 || qtype == 252;
    *output = std::move(parsed);
    return unsupported ? NpmDnsMessageStatusV1::kUnsupported : NpmDnsMessageStatusV1::kComplete;
}

namespace {
// Covers the pending record, bounded wire/display names, vector spare capacity and session index.
constexpr uint64_t kPendingCharge = 4096;
constexpr uint64_t kResultCharge = 4096;
}  // namespace

NpmDnsUdpTrackerV1::NpmDnsUdpTrackerV1(NpmDnsConfigV1 config, std::shared_ptr<INpmTaskBudget> budget)
    : config_(std::move(config)), budget_(std::move(budget)) {}

NpmDnsUdpTrackerV1::~NpmDnsUdpTrackerV1() { Abort(); }

bool NpmDnsUdpTrackerV1::ChargePending() {
    return !budget_ || budget_->Reserve(NpmBudgetCategory::kModuleState, kPendingCharge) == NpmBudgetError::kNone;
}

void NpmDnsUdpTrackerV1::ReleasePending(size_t count) noexcept {
    if (budget_ && count) budget_->Release(NpmBudgetCategory::kModuleState, count * kPendingCharge);
}

bool NpmDnsUdpTrackerV1::ChargeResult() {
    if (!budget_) return true;
    if (budget_->Reserve(NpmBudgetCategory::kModuleState, kResultCharge) != NpmBudgetError::kNone) return false;
    result_charge_bytes_ += kResultCharge;
    return true;
}

uint64_t NpmDnsUdpTrackerV1::NextId() {
    const uint64_t id = next_id_;
    if (id != 0) ++next_id_;
    return id;
}

int NpmDnsUdpTrackerV1::OnDatagram(uint64_t session_id, NpmPacketDirection direction, Span<const uint8_t> body,
                                   bool body_complete, int64_t captured_at_ns,
                                   std::vector<NpmDnsUdpResultV1>* results) {
    return OnMessage(session_id, direction, body, body_complete, captured_at_ns, captured_at_ns, results);
}

int NpmDnsUdpTrackerV1::OnMessage(uint64_t session_id, NpmPacketDirection direction, Span<const uint8_t> body,
                                  bool body_complete, std::optional<int64_t> complete_at_ns, int64_t observed_at_ns,
                                  std::vector<NpmDnsUdpResultV1>* results) {
    if (!results || session_id == 0 ||
        (direction != NpmPacketDirection::kAToB && direction != NpmPacketDirection::kBToA))
        return EINVAL;
    bool uncommitted_charge = false;
    try {
        NpmDnsParsedMessageV1 parsed;
        const auto status = ParseNpmDnsUdpMessageV1(body, body_complete, &parsed);
        if (!parsed.key_available) return 0;
        const auto& message = parsed.message;
        NpmDnsQuestionKeyV1 key;
        key.session_id = session_id;
        key.query_direction = message.is_response ? Opposite(direction) : direction;
        key.dns_id = message.dns_id;
        key.opcode = message.opcode;
        key.wire_qname = *message.wire_qname;
        key.qtype = *message.qtype;
        key.qclass = *message.qclass;
        auto found_session = pending_.find(session_id);
        auto* pending = found_session == pending_.end() ? nullptr : &found_session->second;
        const auto match =
            pending
                ? std::find_if(pending->begin(), pending->end(),
                               [&](const Pending& candidate) { return SameQuestion(candidate.transaction.key, key); })
                : std::list<Pending>::iterator{};
        const bool matched = pending && match != pending->end();
        if (status == NpmDnsMessageStatusV1::kMalformed || status == NpmDnsMessageStatusV1::kCaptureTruncated) {
            if (message.is_response && matched) {
                if (!ChargeResult()) return ENOSPC;
                results->push_back(QueryResult(
                    match->transaction, match->qname_display, observed_at_ns,
                    status == NpmDnsMessageStatusV1::kCaptureTruncated ? "capture_truncation" : "malformed_response"));
                pending->erase(match);
                ReleasePending();
                if (pending->empty()) pending_.erase(found_session);
            }
            return 0;
        }
        if (status == NpmDnsMessageStatusV1::kUnsupported) {
            if (!ChargeResult()) return ENOSPC;
            NpmDnsUdpResultV1 result;
            result.entity_instance_id = NextId();
            if (result.entity_instance_id == 0) return EOVERFLOW;
            result.session_id = session_id;
            result.dns_id = message.dns_id;
            result.outcome = "unsupported_message";
            result.incomplete_reason = "unsupported_opcode_or_transfer";
            (message.is_response ? result.response_direction : result.query_direction) = direction;
            result.qname = parsed.qname_display;
            result.qtype = message.qtype;
            result.qclass = message.qclass;
            (message.is_response ? result.response_at_ns : result.query_at_ns) = complete_at_ns;
            result.observed_at_ns = observed_at_ns;
            results->push_back(std::move(result));
            return 0;
        }
        if (!message.is_response) {
            if (matched) {
                if (match->transaction.query_retries == std::numeric_limits<uint32_t>::max()) return EOVERFLOW;
                ++match->transaction.query_retries;
                return 0;
            }
            if (pending && pending->size() >= config_.max_pending_per_session) return EOVERFLOW;
            if (!ChargePending()) return ENOSPC;
            uncommitted_charge = true;
            Pending next;
            next.transaction.entity_instance_id = NextId();
            if (next.transaction.entity_instance_id == 0) {
                ReleasePending();
                return EOVERFLOW;
            }
            next.transaction.key = std::move(key);
            next.transaction.query_at_ns = complete_at_ns;
            if (complete_at_ns) {
                next.transaction.deadline_ns =
                    *complete_at_ns > std::numeric_limits<int64_t>::max() - config_.response_timeout_ns
                        ? std::numeric_limits<int64_t>::max()
                        : *complete_at_ns + config_.response_timeout_ns;
            }
            next.qname_display = std::move(parsed.qname_display);
            pending_[session_id].push_back(std::move(next));
            uncommitted_charge = false;
            return 0;
        }
        if (!ChargeResult()) return ENOSPC;
        NpmDnsUdpResultV1 result;
        result.session_id = session_id;
        result.dns_id = message.dns_id;
        result.response_direction = direction;
        result.response_at_ns = complete_at_ns;
        result.response_rcode = message.response_rcode;
        result.response_tc = message.truncated;
        result.observed_at_ns = observed_at_ns;
        result.qname = parsed.qname_display;
        result.qtype = message.qtype;
        result.qclass = message.qclass;
        if (matched) {
            result.entity_instance_id = match->transaction.entity_instance_id;
            result.query_direction = match->transaction.key.query_direction;
            result.query_at_ns = match->transaction.query_at_ns;
            result.query_retries = match->transaction.query_retries;
            result.qname = match->qname_display;
            if (complete_at_ns && match->transaction.query_at_ns &&
                *complete_at_ns >= *match->transaction.query_at_ns) {
                const uint64_t difference =
                    static_cast<uint64_t>(*complete_at_ns) - static_cast<uint64_t>(*match->transaction.query_at_ns);
                if (difference <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                    result.latency_ns = static_cast<int64_t>(difference);
            }
            pending->erase(match);
            ReleasePending();
            if (pending->empty()) pending_.erase(found_session);
            result.outcome = message.truncated ? "truncated_response" : "matched";
            if (message.truncated) result.incomplete_reason = "dns_tc_set";
        } else {
            result.entity_instance_id = NextId();
            if (result.entity_instance_id == 0) return EOVERFLOW;
            result.outcome = "response_only";
            result.incomplete_reason = "no_pending_query";
        }
        results->push_back(std::move(result));
        return 0;
    } catch (const std::bad_alloc&) {
        if (uncommitted_charge) ReleasePending();
        return ENOMEM;
    }
}

int NpmDnsUdpTrackerV1::OnResponsePathGap(uint64_t session_id, NpmPacketDirection response_direction,
                                          int64_t observed_at_ns, std::vector<NpmDnsUdpResultV1>* results) {
    if (!results || session_id == 0 ||
        (response_direction != NpmPacketDirection::kAToB && response_direction != NpmPacketDirection::kBToA))
        return EINVAL;
    const auto found = pending_.find(session_id);
    if (found == pending_.end()) return 0;
    try {
        auto& pending = found->second;
        for (auto at = pending.begin(); at != pending.end();) {
            if (at->transaction.key.query_direction == response_direction) {
                ++at;
                continue;
            }
            if (!ChargeResult()) return ENOSPC;
            results->push_back(QueryResult(at->transaction, at->qname_display, observed_at_ns, "response_path_gap"));
            at = pending.erase(at);
            ReleasePending();
        }
        if (pending.empty()) pending_.erase(found);
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

std::optional<int64_t> NpmDnsUdpTrackerV1::NextEventDeadlineNs() const {
    std::optional<int64_t> next;
    for (const auto& session : pending_) {
        for (const auto& pending : session.second) {
            const auto deadline = pending.transaction.deadline_ns;
            if (deadline && (!next || *deadline < *next)) next = deadline;
        }
    }
    return next;
}

int NpmDnsUdpTrackerV1::OnCaptureWatermark(int64_t watermark_ns, std::vector<NpmDnsUdpResultV1>* results) {
    if (!results) return EINVAL;
    if (watermark_ns_ && watermark_ns <= *watermark_ns_) return 0;
    watermark_ns_ = watermark_ns;
    try {
        for (auto session = pending_.begin(); session != pending_.end();) {
            auto& pending = session->second;
            for (auto at = pending.begin(); at != pending.end();) {
                if (!at->transaction.deadline_ns || *at->transaction.deadline_ns > watermark_ns) {
                    ++at;
                    continue;
                }
                if (!ChargeResult()) return ENOSPC;
                results->push_back(
                    QueryResult(at->transaction, at->qname_display, watermark_ns, "response_not_observed_by_deadline"));
                at = pending.erase(at);
                ReleasePending();
            }
            if (pending.empty())
                session = pending_.erase(session);
            else
                ++session;
        }
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int NpmDnsUdpTrackerV1::OnSessionEnd(uint64_t session_id, int64_t observed_at_ns,
                                     std::vector<NpmDnsUdpResultV1>* results) {
    if (!results || session_id == 0) return EINVAL;
    const auto found = pending_.find(session_id);
    if (found == pending_.end()) return 0;
    try {
        for (const auto& pending : found->second) {
            if (!ChargeResult()) return ENOSPC;
            results->push_back(QueryResult(pending.transaction, pending.qname_display, observed_at_ns, "session_end"));
        }
        const size_t released = found->second.size();
        pending_.erase(found);
        ReleasePending(released);
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

size_t NpmDnsUdpTrackerV1::PendingCount(uint64_t session_id) const {
    const auto found = pending_.find(session_id);
    return found == pending_.end() ? 0 : found->second.size();
}

void NpmDnsUdpTrackerV1::ReleaseResults(std::vector<NpmDnsUdpResultV1>* results) noexcept {
    if (results) {
        std::vector<NpmDnsUdpResultV1> empty;
        results->swap(empty);
    }
    if (budget_ && result_charge_bytes_) budget_->Release(NpmBudgetCategory::kModuleState, result_charge_bytes_);
    result_charge_bytes_ = 0;
}

void NpmDnsUdpTrackerV1::Abort() noexcept {
    for (const auto& session : pending_) ReleasePending(session.second.size());
    pending_.clear();
    ReleaseResults(nullptr);
    watermark_ns_.reset();
}

}  // namespace flowsql::npm
