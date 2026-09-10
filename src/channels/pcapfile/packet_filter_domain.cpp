// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "packet_filter_domain.h"

#include <framework/core/filter_expression.h>
#include <framework/core/packet_codec.h>

#include <arrow/api.h>

#include <arpa/inet.h>

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace flowsql::channels::pcapfile {
namespace {

int Fail(int64_t* output, std::string* error, int code, const char* message) {
    if (output) *output = 0;
    if (error) *error = message;
    return code;
}

template <typename T>
int FailValue(T* output, std::string* error, int code, const std::string& message) {
    if (output) *output = T{};
    if (error) *error = message;
    return code;
}

bool ParseDigits(std::string_view text, size_t offset, size_t count, int* output) {
    if (!output || offset > text.size() || count > text.size() - offset) return false;
    int value = 0;
    for (size_t index = 0; index < count; ++index) {
        const char digit = text[offset + index];
        if (digit < '0' || digit > '9') return false;
        value = value * 10 + (digit - '0');
    }
    *output = value;
    return true;
}

bool IsLeapYear(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int DaysInMonth(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeapYear(year)) return 29;
    return kDays[month - 1];
}

// Gregorian civil date to days since 1970-01-01.
int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned shifted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

int HexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool PacketIpLess(const packet::PacketIpKey& lhs, const packet::PacketIpKey& rhs) {
    if (lhs.family != rhs.family) {
        return static_cast<uint8_t>(lhs.family) < static_cast<uint8_t>(rhs.family);
    }
    if (lhs.family == packet::AddressFamily::kIPv4) {
        const auto* lhs_bytes = reinterpret_cast<const uint8_t*>(&lhs.ipv4_network_order);
        const auto* rhs_bytes = reinterpret_cast<const uint8_t*>(&rhs.ipv4_network_order);
        return std::lexicographical_compare(lhs_bytes, lhs_bytes + sizeof(lhs.ipv4_network_order),
                                            rhs_bytes, rhs_bytes + sizeof(rhs.ipv4_network_order));
    }
    if (lhs.family == packet::AddressFamily::kIPv6) return lhs.ipv6 < rhs.ipv6;
    return false;
}

bool PacketIpEqual(const packet::PacketIpKey& lhs, const packet::PacketIpKey& rhs) {
    if (lhs.family != rhs.family) return false;
    if (lhs.family == packet::AddressFamily::kIPv4) {
        return lhs.ipv4_network_order == rhs.ipv4_network_order;
    }
    if (lhs.family == packet::AddressFamily::kIPv6) return lhs.ipv6 == rhs.ipv6;
    return true;
}

bool PacketEndpointLess(const packet::PacketEndpointKey& lhs,
                        const packet::PacketEndpointKey& rhs) {
    if (PacketIpLess(lhs.address, rhs.address)) return true;
    if (PacketIpLess(rhs.address, lhs.address)) return false;
    return lhs.port < rhs.port;
}

bool PacketEndpointEqual(const packet::PacketEndpointKey& lhs,
                         const packet::PacketEndpointKey& rhs) {
    return PacketIpEqual(lhs.address, rhs.address) && lhs.port == rhs.port;
}

bool TransportPairLess(const packet::TransportPairKey& lhs,
                       const packet::TransportPairKey& rhs) {
    if (lhs.transport_protocol != rhs.transport_protocol) {
        return lhs.transport_protocol < rhs.transport_protocol;
    }
    if (PacketEndpointLess(lhs.first, rhs.first)) return true;
    if (PacketEndpointLess(rhs.first, lhs.first)) return false;
    return PacketEndpointLess(lhs.second, rhs.second);
}

bool TransportPairEqual(const packet::TransportPairKey& lhs,
                        const packet::TransportPairKey& rhs) {
    return lhs.transport_protocol == rhs.transport_protocol &&
           PacketEndpointEqual(lhs.first, rhs.first) && PacketEndpointEqual(lhs.second, rhs.second);
}

template <typename T, typename Less, typename Equal>
void SortAndUnique(std::vector<T>* values, Less less, Equal equal) {
    std::sort(values->begin(), values->end(), less);
    values->erase(std::unique(values->begin(), values->end(), equal), values->end());
}

std::string LowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return value;
}

class PcapFilterAstLowerer {
 public:
    int Lower(const std::shared_ptr<const FilterExpr>& input,
              std::shared_ptr<FilterExpr>* output,
              std::string* error) {
        if (output) output->reset();
        if (error) error->clear();
        error_ = error;
        if (!input || !output) return Fail(EINVAL, "filter expression and output are required", 0);
        if (!CollectInputIds(input)) return error_code_;
        if (!ValidateTypedLiteralContexts(input, false)) return error_code_;

        auto lowered = LowerNode(input);
        if (!lowered) return error_code_ == 0 ? EINVAL : error_code_;
        *output = std::move(lowered);
        return 0;
    }

 private:
    int Fail(int code, const std::string& message, uint32_t node_id) {
        if (error_code_ == 0) {
            error_code_ = code;
            if (error_) {
                *error_ = message;
                if (node_id != 0) *error_ += " at node " + std::to_string(node_id);
            }
        }
        return error_code_;
    }

    bool CollectInputIds(const std::shared_ptr<const FilterExpr>& node) {
        if (!node) {
            Fail(EINVAL, "filter expression contains a null node", 0);
            return false;
        }
        if (node->node_id == 0 || !used_ids_.insert(node->node_id).second) {
            Fail(EINVAL, "filter expression node ids must be nonzero and unique", node->node_id);
            return false;
        }
        for (const auto& operand : node->operands) {
            if (!CollectInputIds(operand)) return false;
        }
        return true;
    }

    bool IsTimestampField(const std::shared_ptr<const FilterExpr>& node) const {
        return node && node->kind == FilterExprKind::kField && node->field_name == "timestamp_ns";
    }

    bool ValidateTypedLiteralContexts(const std::shared_ptr<const FilterExpr>& node,
                                      bool timestamp_allowed) {
        if (node->kind == FilterExprKind::kLiteral &&
            node->literal.kind == FilterLiteralKind::kTyped) {
            if (LowerAscii(node->literal.type_name) == "timestamp" && !timestamp_allowed) {
                Fail(EINVAL, "TIMESTAMP literal must be compared with timestamp_ns", node->node_id);
                return false;
            }
            return true;
        }
        if (node->kind == FilterExprKind::kCompare && node->operands.size() == 2) {
            return ValidateTypedLiteralContexts(node->operands[0], IsTimestampField(node->operands[1])) &&
                   ValidateTypedLiteralContexts(node->operands[1], IsTimestampField(node->operands[0]));
        }
        if ((node->kind == FilterExprKind::kIn || node->kind == FilterExprKind::kBetween) &&
            !node->operands.empty()) {
            if (!ValidateTypedLiteralContexts(node->operands[0], false)) return false;
            const bool values_are_timestamps = IsTimestampField(node->operands[0]);
            for (size_t index = 1; index < node->operands.size(); ++index) {
                if (!ValidateTypedLiteralContexts(node->operands[index], values_are_timestamps)) return false;
            }
            return true;
        }
        for (const auto& operand : node->operands) {
            if (!ValidateTypedLiteralContexts(operand, false)) return false;
        }
        return true;
    }

    uint32_t NewSyntheticId() {
        while (used_ids_.count(next_synthetic_id_) != 0) {
            if (next_synthetic_id_ == std::numeric_limits<uint32_t>::max()) {
                Fail(EOVERFLOW, "filter expression exhausted synthetic node ids", 0);
                return 0;
            }
            ++next_synthetic_id_;
        }
        const uint32_t result = next_synthetic_id_;
        used_ids_.insert(result);
        if (next_synthetic_id_ == std::numeric_limits<uint32_t>::max()) {
            synthetic_ids_exhausted_ = true;
        } else {
            ++next_synthetic_id_;
        }
        return result;
    }

    std::shared_ptr<FilterExpr> NewSyntheticNode(FilterExprKind kind) {
        if (synthetic_ids_exhausted_) {
            Fail(EOVERFLOW, "filter expression exhausted synthetic node ids", 0);
            return nullptr;
        }
        const uint32_t node_id = NewSyntheticId();
        if (node_id == 0) return nullptr;
        auto node = std::make_shared<FilterExpr>();
        node->kind = kind;
        node->node_id = node_id;
        return node;
    }

    std::shared_ptr<FilterExpr> MakeField(const std::string& name) {
        auto field = NewSyntheticNode(FilterExprKind::kField);
        if (field) field->field_name = name;
        return field;
    }

    std::shared_ptr<FilterExpr> MakeLiteral(FilterLiteralKind kind, std::string text) {
        auto literal = NewSyntheticNode(FilterExprKind::kLiteral);
        if (literal) literal->literal = {kind, {}, std::move(text)};
        return literal;
    }

    std::shared_ptr<FilterExpr> MakeEqual(const std::string& field_name,
                                          FilterLiteralKind literal_kind,
                                          std::string literal_text) {
        auto compare = NewSyntheticNode(FilterExprKind::kCompare);
        auto field = MakeField(field_name);
        auto literal = MakeLiteral(literal_kind, std::move(literal_text));
        if (!compare || !field || !literal) return nullptr;
        compare->compare_op = FilterCompareOp::kEqual;
        compare->operands = {std::move(field), std::move(literal)};
        return compare;
    }

    std::shared_ptr<FilterExpr> MakeLogical(
        FilterExprKind kind,
        std::vector<std::shared_ptr<FilterExpr>> operands,
        uint32_t root_id) {
        if ((kind != FilterExprKind::kAnd && kind != FilterExprKind::kOr) ||
            operands.size() < 2 || root_id == 0) {
            Fail(EINVAL, "generated logical expression is invalid", root_id);
            return nullptr;
        }
        auto current = std::move(operands.front());
        for (size_t index = 1; index < operands.size(); ++index) {
            std::shared_ptr<FilterExpr> node;
            if (index + 1 == operands.size()) {
                node = std::make_shared<FilterExpr>();
                node->kind = kind;
                node->node_id = root_id;
            } else {
                node = NewSyntheticNode(kind);
            }
            if (!current || !operands[index] || !node) return nullptr;
            node->operands = {std::move(current), std::move(operands[index])};
            current = std::move(node);
        }
        return current;
    }

    std::shared_ptr<FilterExpr> MakeSyntheticLogical(
        FilterExprKind kind,
        std::vector<std::shared_ptr<FilterExpr>> operands) {
        const uint32_t root_id = NewSyntheticId();
        if (root_id == 0) return nullptr;
        return MakeLogical(kind, std::move(operands), root_id);
    }

    const FilterLiteral* RequireLiteral(const std::shared_ptr<const FilterExpr>& call,
                                        size_t index,
                                        FilterLiteralKind kind) {
        if (index >= call->operands.size() || !call->operands[index] ||
            call->operands[index]->kind != FilterExprKind::kLiteral ||
            call->operands[index]->literal.kind != kind) {
            Fail(EINVAL, "packet domain function argument has the wrong literal type", call->node_id);
            return nullptr;
        }
        return &call->operands[index]->literal;
    }

    std::shared_ptr<FilterExpr> LowerNode(const std::shared_ptr<const FilterExpr>& input) {
        if (input->kind == FilterExprKind::kCall) return LowerCall(input);
        if (input->kind == FilterExprKind::kLiteral &&
            input->literal.kind == FilterLiteralKind::kTyped) {
            return LowerTypedLiteral(input);
        }

        auto output = std::make_shared<FilterExpr>(*input);
        output->operands.clear();
        output->operands.reserve(input->operands.size());
        for (const auto& operand : input->operands) {
            auto lowered = LowerNode(operand);
            if (!lowered) return nullptr;
            output->operands.push_back(std::move(lowered));
        }
        return output;
    }

    std::shared_ptr<FilterExpr> LowerTypedLiteral(
        const std::shared_ptr<const FilterExpr>& input) {
        if (!input->operands.empty() || LowerAscii(input->literal.type_name) != "timestamp") {
            Fail(EINVAL, "pcapfile only supports the TIMESTAMP typed literal", input->node_id);
            return nullptr;
        }
        int64_t timestamp_ns = 0;
        std::string detail;
        const int rc = ParseRfc3339TimestampNs(input->literal.text, &timestamp_ns, &detail);
        if (rc != 0) {
            Fail(rc, "invalid TIMESTAMP literal: " + detail, input->node_id);
            return nullptr;
        }
        auto output = std::make_shared<FilterExpr>();
        output->kind = FilterExprKind::kLiteral;
        output->node_id = input->node_id;
        output->literal = {FilterLiteralKind::kInteger, {}, std::to_string(timestamp_ns)};
        return output;
    }

    std::shared_ptr<FilterExpr> LowerCall(const std::shared_ptr<const FilterExpr>& call) {
        const std::string function = LowerAscii(call->function_name);
        if (function == "mac") return LowerMac(call);
        if (function == "ip") return LowerIp(call);
        if (function == "port") return LowerPort(call);
        if (function == "tcp" || function == "udp") return LowerTransportPair(call, function);
        Fail(EINVAL, "unknown pcapfile filter function: " + call->function_name, call->node_id);
        return nullptr;
    }

    std::shared_ptr<FilterExpr> LowerMac(const std::shared_ptr<const FilterExpr>& call) {
        if (call->operands.size() != 1) {
            Fail(EINVAL, "mac() requires exactly one string argument", call->node_id);
            return nullptr;
        }
        const auto* literal = RequireLiteral(call, 0, FilterLiteralKind::kString);
        if (!literal) return nullptr;
        packet::PacketMacKey key;
        std::string detail;
        if (CompilePacketMacKey(literal->text, &key, &detail) != 0) {
            Fail(EINVAL, "invalid mac() argument: " + detail, call->node_id);
            return nullptr;
        }
        const std::string bytes(reinterpret_cast<const char*>(key.bytes.data()), key.bytes.size());
        return MakeLogical(FilterExprKind::kOr,
                           {MakeEqual("src_mac", FilterLiteralKind::kString, bytes),
                            MakeEqual("dst_mac", FilterLiteralKind::kString, bytes)},
                           call->node_id);
    }

    std::shared_ptr<FilterExpr> MakeAddressMatch(const char* prefix,
                                                 const packet::PacketIpKey& key) {
        const std::string family_field = std::string(prefix) + "_ip_family";
        const std::string address_field =
            std::string(prefix) + (key.family == packet::AddressFamily::kIPv4 ? "_ip_v4" : "_ip_v6");
        std::string address_value;
        FilterLiteralKind address_kind = FilterLiteralKind::kInteger;
        if (key.family == packet::AddressFamily::kIPv4) {
            address_value = std::to_string(key.ipv4_network_order);
        } else {
            address_kind = FilterLiteralKind::kString;
            address_value.assign(reinterpret_cast<const char*>(key.ipv6.data()), key.ipv6.size());
        }
        return MakeSyntheticLogical(
            FilterExprKind::kAnd,
            {MakeEqual(family_field, FilterLiteralKind::kInteger,
                       std::to_string(static_cast<uint8_t>(key.family))),
             MakeEqual(address_field, address_kind, std::move(address_value))});
    }

    std::shared_ptr<FilterExpr> LowerIp(const std::shared_ptr<const FilterExpr>& call) {
        if (call->operands.size() != 1) {
            Fail(EINVAL, "ip() requires exactly one string argument", call->node_id);
            return nullptr;
        }
        const auto* literal = RequireLiteral(call, 0, FilterLiteralKind::kString);
        if (!literal) return nullptr;
        packet::PacketIpKey key;
        std::string detail;
        if (CompilePacketIpKey(literal->text, &key, &detail) != 0) {
            Fail(EINVAL, "invalid ip() argument: " + detail, call->node_id);
            return nullptr;
        }
        return MakeLogical(FilterExprKind::kOr,
                           {MakeAddressMatch("src", key), MakeAddressMatch("dst", key)},
                           call->node_id);
    }

    std::shared_ptr<FilterExpr> LowerPort(const std::shared_ptr<const FilterExpr>& call) {
        if (call->operands.size() != 1) {
            Fail(EINVAL, "port() requires exactly one integer argument", call->node_id);
            return nullptr;
        }
        const auto* literal = RequireLiteral(call, 0, FilterLiteralKind::kInteger);
        if (!literal) return nullptr;
        uint16_t port = 0;
        std::string detail;
        if (CompilePacketPort(literal->text, &port, &detail) != 0) {
            Fail(EINVAL, "invalid port() argument: " + detail, call->node_id);
            return nullptr;
        }
        auto either_port = MakeSyntheticLogical(
            FilterExprKind::kOr,
            {MakeEqual("src_port", FilterLiteralKind::kInteger, std::to_string(port)),
             MakeEqual("dst_port", FilterLiteralKind::kInteger, std::to_string(port))});
        return MakeLogical(FilterExprKind::kAnd,
                           {MakeField("ports_valid"), std::move(either_port)},
                           call->node_id);
    }

    void AppendEndpointMatch(const char* prefix,
                             const packet::PacketEndpointKey& endpoint,
                             std::vector<std::shared_ptr<FilterExpr>>* predicates) {
        const std::string family_field = std::string(prefix) + "_ip_family";
        const std::string address_field =
            std::string(prefix) +
            (endpoint.address.family == packet::AddressFamily::kIPv4 ? "_ip_v4" : "_ip_v6");
        predicates->push_back(MakeEqual(
            family_field, FilterLiteralKind::kInteger,
            std::to_string(static_cast<uint8_t>(endpoint.address.family))));
        if (endpoint.address.family == packet::AddressFamily::kIPv4) {
            predicates->push_back(MakeEqual(
                address_field, FilterLiteralKind::kInteger,
                std::to_string(endpoint.address.ipv4_network_order)));
        } else {
            predicates->push_back(MakeEqual(
                address_field, FilterLiteralKind::kString,
                std::string(reinterpret_cast<const char*>(endpoint.address.ipv6.data()),
                            endpoint.address.ipv6.size())));
        }
        predicates->push_back(MakeEqual(
            std::string(prefix) + "_port", FilterLiteralKind::kInteger,
            std::to_string(endpoint.port)));
    }

    std::shared_ptr<FilterExpr> MakeTransportDirection(
        const packet::PacketEndpointKey& source,
        const packet::PacketEndpointKey& destination) {
        std::vector<std::shared_ptr<FilterExpr>> predicates;
        AppendEndpointMatch("src", source, &predicates);
        AppendEndpointMatch("dst", destination, &predicates);
        return MakeSyntheticLogical(FilterExprKind::kAnd, std::move(predicates));
    }

    std::shared_ptr<FilterExpr> LowerTransportPair(
        const std::shared_ptr<const FilterExpr>& call,
        const std::string& function) {
        if (call->operands.size() != 4) {
            Fail(EINVAL, function + "() requires IP, port, IP, port arguments", call->node_id);
            return nullptr;
        }
        const auto* endpoint1_ip = RequireLiteral(call, 0, FilterLiteralKind::kString);
        const auto* endpoint1_port = RequireLiteral(call, 1, FilterLiteralKind::kInteger);
        const auto* endpoint2_ip = RequireLiteral(call, 2, FilterLiteralKind::kString);
        const auto* endpoint2_port = RequireLiteral(call, 3, FilterLiteralKind::kInteger);
        if (!endpoint1_ip || !endpoint1_port || !endpoint2_ip || !endpoint2_port) return nullptr;

        packet::TransportPairKey pair;
        std::string detail;
        const int rc = CompileTransportPairKey(
            function, endpoint1_ip->text, endpoint1_port->text, endpoint2_ip->text,
            endpoint2_port->text, &pair, &detail);
        if (rc != 0) {
            Fail(rc, "invalid " + function + "() arguments: " + detail, call->node_id);
            return nullptr;
        }
        auto either_direction = MakeSyntheticLogical(
            FilterExprKind::kOr,
            {MakeTransportDirection(pair.first, pair.second),
             MakeTransportDirection(pair.second, pair.first)});
        return MakeLogical(
            FilterExprKind::kAnd,
            {MakeEqual("transport_protocol", FilterLiteralKind::kInteger,
                       std::to_string(pair.transport_protocol)),
             MakeField("ports_valid"), std::move(either_direction)},
            call->node_id);
    }

    std::unordered_set<uint32_t> used_ids_;
    uint32_t next_synthetic_id_ = kFilterDomainSyntheticNodeIdBaseV1;
    bool synthetic_ids_exhausted_ = false;
    int error_code_ = 0;
    std::string* error_ = nullptr;
};

}  // namespace

int ParseRfc3339TimestampNs(const std::string& text, int64_t* output, std::string* error) {
    if (error) error->clear();
    if (!output) return Fail(nullptr, error, EINVAL, "timestamp output must not be null");
    *output = 0;

    const std::string_view value(text);
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != 't') || value[13] != ':' || value[16] != ':') {
        return Fail(output, error, EINVAL, "timestamp must be RFC3339 with an explicit UTC offset");
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!ParseDigits(value, 0, 4, &year) || !ParseDigits(value, 5, 2, &month) ||
        !ParseDigits(value, 8, 2, &day) || !ParseDigits(value, 11, 2, &hour) ||
        !ParseDigits(value, 14, 2, &minute) || !ParseDigits(value, 17, 2, &second)) {
        return Fail(output, error, EINVAL, "timestamp date and time components must be decimal digits");
    }
    if (year == 0 || month < 1 || month > 12 || day < 1 || day > DaysInMonth(year, month) ||
        hour > 23 || minute > 59 || second > 59) {
        return Fail(output, error, EINVAL, "timestamp date or time component is out of range");
    }

    size_t position = 19;
    int64_t fractional_ns = 0;
    if (position < value.size() && value[position] == '.') {
        ++position;
        const size_t fraction_begin = position;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
            if (position - fraction_begin == 9) {
                return Fail(output, error, EINVAL, "timestamp fractional seconds exceed nanosecond precision");
            }
            fractional_ns = fractional_ns * 10 + (value[position] - '0');
            ++position;
        }
        const size_t fraction_digits = position - fraction_begin;
        if (fraction_digits == 0) {
            return Fail(output, error, EINVAL, "timestamp fractional seconds must contain digits");
        }
        for (size_t index = fraction_digits; index < 9; ++index) fractional_ns *= 10;
    }

    int offset_minutes = 0;
    if (position < value.size() && (value[position] == 'Z' || value[position] == 'z')) {
        ++position;
    } else {
        if (position >= value.size() || (value[position] != '+' && value[position] != '-') ||
            value.size() - position != 6 || value[position + 3] != ':') {
            return Fail(output, error, EINVAL, "timestamp must include Z or an explicit HH:MM offset");
        }
        int offset_hour = 0;
        int offset_minute = 0;
        if (!ParseDigits(value, position + 1, 2, &offset_hour) ||
            !ParseDigits(value, position + 4, 2, &offset_minute) || offset_hour > 23 ||
            offset_minute > 59) {
            return Fail(output, error, EINVAL, "timestamp UTC offset is invalid");
        }
        offset_minutes = offset_hour * 60 + offset_minute;
        if (value[position] == '-') offset_minutes = -offset_minutes;
        position += 6;
    }
    if (position != value.size()) {
        return Fail(output, error, EINVAL, "timestamp contains trailing characters");
    }

    const int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const __int128 local_seconds = static_cast<__int128>(days) * 86400 + hour * 3600 + minute * 60 + second;
    const __int128 utc_seconds = local_seconds - static_cast<__int128>(offset_minutes) * 60;
    const __int128 epoch_ns = utc_seconds * 1000000000 + fractional_ns;
    if (epoch_ns < std::numeric_limits<int64_t>::min() ||
        epoch_ns > std::numeric_limits<int64_t>::max()) {
        return Fail(output, error, EOVERFLOW, "timestamp is outside the epoch-nanosecond int64 range");
    }

    *output = static_cast<int64_t>(epoch_ns);
    return 0;
}

int CompilePacketMacKey(const std::string& text,
                        packet::PacketMacKey* output,
                        std::string* error) {
    if (error) error->clear();
    if (!output) return FailValue(output, error, EINVAL, "MAC output must not be null");
    *output = {};
    if (text.size() != 17) {
        return FailValue(output, error, EINVAL, "MAC address must contain six colon-separated bytes");
    }
    for (size_t index = 0; index < output->bytes.size(); ++index) {
        const size_t offset = index * 3;
        const int high = HexDigit(text[offset]);
        const int low = HexDigit(text[offset + 1]);
        if (high < 0 || low < 0 || (index + 1 != output->bytes.size() && text[offset + 2] != ':')) {
            return FailValue(output, error, EINVAL,
                             "MAC address must contain six colon-separated hexadecimal bytes");
        }
        output->bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return 0;
}

int CompilePacketIpKey(const std::string& text,
                       packet::PacketIpKey* output,
                       std::string* error) {
    if (error) error->clear();
    if (!output) return FailValue(output, error, EINVAL, "IP output must not be null");
    *output = {};
    if (text.empty() || text.find('\0') != std::string::npos) {
        return FailValue(output, error, EINVAL, "IP address is empty or contains a null byte");
    }

    uint32_t ipv4 = 0;
    if (inet_pton(AF_INET, text.c_str(), &ipv4) == 1) {
        output->family = packet::AddressFamily::kIPv4;
        output->ipv4_network_order = ipv4;
        return 0;
    }
    std::array<uint8_t, 16> ipv6{};
    if (inet_pton(AF_INET6, text.c_str(), ipv6.data()) == 1) {
        output->family = packet::AddressFamily::kIPv6;
        output->ipv6 = ipv6;
        return 0;
    }
    return FailValue(output, error, EINVAL, "IP address is not valid IPv4 or IPv6 text");
}

int CompilePacketPort(const std::string& text, uint16_t* output, std::string* error) {
    if (error) error->clear();
    if (!output) return FailValue(output, error, EINVAL, "port output must not be null");
    *output = 0;
    uint32_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (text.empty() || result.ec != std::errc() || result.ptr != end || parsed > 65535) {
        return FailValue(output, error, EINVAL, "port must be a decimal integer in the range 0 through 65535");
    }
    *output = static_cast<uint16_t>(parsed);
    return 0;
}

int CompileTransportPairKey(const std::string& function_name,
                            const std::string& endpoint1_ip,
                            const std::string& endpoint1_port,
                            const std::string& endpoint2_ip,
                            const std::string& endpoint2_port,
                            packet::TransportPairKey* output,
                            std::string* error) {
    if (error) error->clear();
    if (!output) return FailValue(output, error, EINVAL, "transport pair output must not be null");
    *output = {};

    std::string lower_name = function_name;
    for (char& value : lower_name) {
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
    }
    uint8_t protocol = 0;
    if (lower_name == "tcp") {
        protocol = 6;
    } else if (lower_name == "udp") {
        protocol = 17;
    } else {
        return FailValue(output, error, EINVAL, "transport pair function must be tcp or udp");
    }

    packet::PacketEndpointKey first;
    packet::PacketEndpointKey second;
    std::string detail;
    if (CompilePacketIpKey(endpoint1_ip, &first.address, &detail) != 0) {
        return FailValue(output, error, EINVAL, "endpoint1 IP is invalid: " + detail);
    }
    if (CompilePacketIpKey(endpoint2_ip, &second.address, &detail) != 0) {
        return FailValue(output, error, EINVAL, "endpoint2 IP is invalid: " + detail);
    }
    if (first.address.family != second.address.family) {
        return FailValue(output, error, EINVAL, "transport pair endpoints must use the same IP family");
    }
    if (CompilePacketPort(endpoint1_port, &first.port, &detail) != 0) {
        return FailValue(output, error, EINVAL, "endpoint1 port is invalid: " + detail);
    }
    if (CompilePacketPort(endpoint2_port, &second.port, &detail) != 0) {
        return FailValue(output, error, EINVAL, "endpoint2 port is invalid: " + detail);
    }

    if (PacketEndpointLess(second, first)) std::swap(first, second);
    output->transport_protocol = protocol;
    output->first = std::move(first);
    output->second = std::move(second);
    return 0;
}

void CanonicalizePacketFilterRuleKeys(packet::PacketFilterRule* rule) {
    if (!rule) return;
    for (auto& operand : rule->operands) CanonicalizePacketFilterRuleKeys(&operand);

    switch (rule->kind) {
        case packet::PacketFilterRuleKind::kMacAnyOf:
            SortAndUnique(&rule->mac_keys,
                          [](const packet::PacketMacKey& lhs, const packet::PacketMacKey& rhs) {
                              return lhs.bytes < rhs.bytes;
                          },
                          [](const packet::PacketMacKey& lhs, const packet::PacketMacKey& rhs) {
                              return lhs.bytes == rhs.bytes;
                          });
            break;
        case packet::PacketFilterRuleKind::kIpAnyOf:
            SortAndUnique(&rule->ip_keys, PacketIpLess, PacketIpEqual);
            break;
        case packet::PacketFilterRuleKind::kPortAnyOf:
            std::sort(rule->ports.begin(), rule->ports.end());
            rule->ports.erase(std::unique(rule->ports.begin(), rule->ports.end()), rule->ports.end());
            break;
        case packet::PacketFilterRuleKind::kTransportPairAnyOf:
            for (auto& pair : rule->transport_pairs) {
                if (PacketEndpointLess(pair.second, pair.first)) std::swap(pair.first, pair.second);
            }
            SortAndUnique(&rule->transport_pairs, TransportPairLess, TransportPairEqual);
            break;
        default:
            break;
    }
}

int PcapFilterDomainResolver::Resolve(const FilterDomainResolveRequestV1& request,
                                      FilterDomainResolveResultV1* result) const {
    if (result) *result = {};
    if (!result) return EINVAL;
    if (request.contract_version != kFilterDomainResolverContractVersionV1 ||
        !request.target_category || !request.target_name || !request.output_schema ||
        !request.expression || request.target_name[0] == '\0') {
        result->diagnostic = "pcapfile domain resolve request is incomplete or has the wrong version";
        return EINVAL;
    }
    if (request.target_kind != FilterDomainTargetKindV1::kSource ||
        LowerAscii(request.target_category) != "pcapfile") {
        return ENOTSUP;
    }
    const auto expected_schema = packet::PacketSchema();
    if (!expected_schema || !request.output_schema->Equals(*expected_schema, true)) {
        result->diagnostic = "pcapfile domain resolver requires the packet output Schema";
        return EINVAL;
    }

    try {
        PcapFilterAstLowerer lowerer;
        const int rc = lowerer.Lower(
            request.expression, &result->lowered_expression, &result->diagnostic);
        if (rc != 0) {
            result->lowered_expression.reset();
            return rc;
        }
        result->diagnostic = "pcapfile packet filter domain resolved";
        return 0;
    } catch (const std::bad_alloc&) {
        result->lowered_expression.reset();
        result->diagnostic = "pcapfile domain resolver allocation failed";
        return ENOMEM;
    }
}

}  // namespace flowsql::channels::pcapfile
