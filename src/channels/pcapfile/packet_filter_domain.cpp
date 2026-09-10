// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "packet_filter_domain.h"

#include <framework/core/filter_expression.h>
#include <framework/core/packet_codec.h>

#include <arrow/api.h>
#include <rapidjson/document.h>

#include <arpa/inet.h>

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
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

struct CanonicalFilterNode {
    uint32_t node_id = 0;
    std::string kind;
    std::string type;
    bool nullable = false;
    int32_t field_index = -1;
    std::string field_name;
    std::string value;
    std::string compare_op;
    std::string comparison_type;
    std::vector<CanonicalFilterNode> operands;
};

class CanonicalFilterPlanParser {
 public:
    explicit CanonicalFilterPlanParser(std::string* error) : error_(error) {}

    int Parse(const std::string& json, std::optional<CanonicalFilterNode>* root) {
        rapidjson::Document document;
        document.Parse(json.data(), json.size());
        if (document.HasParseError() || !document.IsObject()) {
            return Fail("canonical filter plan must be a JSON object");
        }
        if (document.MemberCount() != 2 || !document.HasMember("version") ||
            !document["version"].IsUint() || document["version"].GetUint() != 1 ||
            !document.HasMember("root")) {
            return Fail("canonical filter plan must contain only version 1 and root");
        }
        if (document["root"].IsNull()) {
            root->reset();
            return 0;
        }
        CanonicalFilterNode parsed;
        if (!ParseNode(document["root"], &parsed)) return EINVAL;
        *root = std::move(parsed);
        return 0;
    }

 private:
    int Fail(const std::string& message, uint32_t node_id = 0) {
        if (error_) {
            *error_ = message;
            if (node_id != 0) *error_ += " at node " + std::to_string(node_id);
        }
        return EINVAL;
    }

    bool ReadString(const rapidjson::Value& object, const char* name, std::string* output) {
        if (!object.HasMember(name) || !object[name].IsString()) return false;
        output->assign(object[name].GetString(), object[name].GetStringLength());
        return true;
    }

    bool IsAllowedMember(const std::string& kind, std::string_view name) const {
        if (name == "node_id" || name == "kind" || name == "type" || name == "nullable") {
            return true;
        }
        if (kind == "field") return name == "field_index" || name == "field_name";
        if (kind == "literal") return name == "value";
        if (kind == "compare") {
            return name == "compare_op" || name == "comparison_type" || name == "operands";
        }
        if (kind == "in" || kind == "between") {
            return name == "comparison_type" || name == "operands";
        }
        if (kind == "and" || kind == "or" || kind == "not" || kind == "is_null") {
            return name == "operands";
        }
        return false;
    }

    bool HasExpectedArity(const CanonicalFilterNode& node) const {
        if (node.kind == "field" || node.kind == "literal") return node.operands.empty();
        if (node.kind == "and" || node.kind == "or" || node.kind == "compare") {
            return node.operands.size() == 2;
        }
        if (node.kind == "not" || node.kind == "is_null") return node.operands.size() == 1;
        if (node.kind == "between") return node.operands.size() == 3;
        if (node.kind == "in") return node.operands.size() >= 2;
        return false;
    }

    bool HasExpectedNullability(const CanonicalFilterNode& node) const {
        if (node.kind == "field") return true;
        if (node.kind == "literal" || node.kind == "is_null") return !node.nullable;
        bool expected = false;
        if (node.kind == "in" || node.kind == "between") {
            expected = node.operands.front().nullable;
        } else {
            for (const auto& operand : node.operands) expected = expected || operand.nullable;
        }
        return node.nullable == expected;
    }

    bool ParseNode(const rapidjson::Value& value, CanonicalFilterNode* output) {
        if (!value.IsObject() || !value.HasMember("node_id") ||
            !value["node_id"].IsUint() || value["node_id"].GetUint() == 0 ||
            !ReadString(value, "kind", &output->kind) ||
            !ReadString(value, "type", &output->type) ||
            !value.HasMember("nullable") || !value["nullable"].IsBool()) {
            Fail("canonical filter node is incomplete");
            return false;
        }
        output->node_id = value["node_id"].GetUint();
        output->nullable = value["nullable"].GetBool();
        if (!node_ids_.insert(output->node_id).second) {
            Fail("canonical filter node id is duplicated", output->node_id);
            return false;
        }
        std::unordered_set<std::string> member_names;
        for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
            const std::string name(it->name.GetString(), it->name.GetStringLength());
            if (!member_names.insert(name).second || !IsAllowedMember(output->kind, name)) {
                Fail("canonical filter node has an unknown or duplicate member", output->node_id);
                return false;
            }
        }

        if (output->kind == "field") {
            if (!value.HasMember("field_index") || !value["field_index"].IsInt() ||
                !ReadString(value, "field_name", &output->field_name)) {
                Fail("canonical field node is incomplete", output->node_id);
                return false;
            }
            output->field_index = value["field_index"].GetInt();
            const auto schema = packet::PacketSchema();
            if (!schema || output->field_index < 0 || output->field_index >= schema->num_fields()) {
                Fail("canonical field index is outside packet Schema", output->node_id);
                return false;
            }
            const auto& field = schema->field(output->field_index);
            if (!field || field->name() != output->field_name ||
                field->type()->ToString() != output->type || field->nullable() != output->nullable) {
                Fail("canonical field does not match packet Schema", output->node_id);
                return false;
            }
        } else if (output->kind == "literal") {
            if (!ReadString(value, "value", &output->value)) {
                Fail("canonical literal node is incomplete", output->node_id);
                return false;
            }
        } else {
            if (output->type != "bool" || !value.HasMember("operands") ||
                !value["operands"].IsArray()) {
                Fail("canonical predicate node is incomplete", output->node_id);
                return false;
            }
            if (output->kind == "compare") {
                if (!ReadString(value, "compare_op", &output->compare_op) ||
                    !ReadString(value, "comparison_type", &output->comparison_type)) {
                    Fail("canonical comparison node is incomplete", output->node_id);
                    return false;
                }
            } else if (output->kind == "in" || output->kind == "between") {
                if (!ReadString(value, "comparison_type", &output->comparison_type)) {
                    Fail("canonical set or range node is incomplete", output->node_id);
                    return false;
                }
            }
            for (const auto& operand : value["operands"].GetArray()) {
                CanonicalFilterNode parsed;
                if (!ParseNode(operand, &parsed)) return false;
                output->operands.push_back(std::move(parsed));
            }
        }
        if (!HasExpectedArity(*output)) {
            Fail("canonical filter node has invalid arity or kind", output->node_id);
            return false;
        }
        if (!HasExpectedNullability(*output)) {
            Fail("canonical filter node has invalid nullability", output->node_id);
            return false;
        }
        return true;
    }

    std::unordered_set<uint32_t> node_ids_;
    std::string* error_ = nullptr;
};

void IndexCanonicalFilterNodes(
    const CanonicalFilterNode& node,
    std::unordered_map<uint32_t, const CanonicalFilterNode*>* nodes) {
    nodes->emplace(node.node_id, &node);
    for (const auto& operand : node.operands) {
        IndexCanonicalFilterNodes(operand, nodes);
    }
}

void FlattenCanonical(const CanonicalFilterNode& node,
                      const char* kind,
                      std::vector<const CanonicalFilterNode*>* output) {
    if (node.kind == kind) {
        for (const auto& operand : node.operands) FlattenCanonical(operand, kind, output);
        return;
    }
    output->push_back(&node);
}

bool ParseUnsignedValue(const CanonicalFilterNode& literal, uint64_t* output) {
    if (literal.kind != "literal") return false;
    const char* begin = literal.value.data();
    const char* end = begin + literal.value.size();
    const auto result = std::from_chars(begin, end, *output, 10);
    return !literal.value.empty() && result.ec == std::errc() && result.ptr == end;
}

bool ParseSignedValue(const CanonicalFilterNode& literal, int64_t* output) {
    if (literal.kind != "literal") return false;
    const char* begin = literal.value.data();
    const char* end = begin + literal.value.size();
    const auto result = std::from_chars(begin, end, *output, 10);
    return !literal.value.empty() && result.ec == std::errc() && result.ptr == end;
}

bool MatchEquality(const CanonicalFilterNode& node,
                   const CanonicalFilterNode** field,
                   const CanonicalFilterNode** literal) {
    if (node.kind != "compare" || node.compare_op != "equal" || node.operands.size() != 2) {
        return false;
    }
    const auto* lhs = &node.operands[0];
    const auto* rhs = &node.operands[1];
    if (lhs->kind == "literal" && rhs->kind == "field") std::swap(lhs, rhs);
    if (lhs->kind != "field" || rhs->kind != "literal" || lhs->type != rhs->type ||
        node.comparison_type != lhs->type) {
        return false;
    }
    *field = lhs;
    *literal = rhs;
    return true;
}

bool MatchBooleanField(const CanonicalFilterNode& node, const char* name) {
    return node.kind == "field" && node.field_name == name && node.type == "bool";
}

bool MatchMacRule(const CanonicalFilterNode& node, packet::PacketFilterRule* output) {
    std::vector<const CanonicalFilterNode*> terms;
    FlattenCanonical(node, "or", &terms);
    if (node.kind != "or" || terms.size() != 2) return false;
    const CanonicalFilterNode* src_literal = nullptr;
    const CanonicalFilterNode* dst_literal = nullptr;
    for (const auto* term : terms) {
        const CanonicalFilterNode* field = nullptr;
        const CanonicalFilterNode* literal = nullptr;
        if (!MatchEquality(*term, &field, &literal) || literal->type != "fixed_size_binary[6]") {
            return false;
        }
        if (field->field_name == "src_mac" && !src_literal) {
            src_literal = literal;
        } else if (field->field_name == "dst_mac" && !dst_literal) {
            dst_literal = literal;
        } else {
            return false;
        }
    }
    if (!src_literal || !dst_literal || src_literal->value != dst_literal->value ||
        src_literal->value.size() != 6) {
        return false;
    }
    output->kind = packet::PacketFilterRuleKind::kMacAnyOf;
    packet::PacketMacKey key;
    std::copy(src_literal->value.begin(), src_literal->value.end(), key.bytes.begin());
    output->mac_keys.push_back(key);
    return true;
}

bool BuildIpKey(const std::unordered_map<std::string, const CanonicalFilterNode*>& fields,
                const char* prefix,
                packet::PacketIpKey* output) {
    const std::string family_name = std::string(prefix) + "_ip_family";
    const auto family_it = fields.find(family_name);
    if (family_it == fields.end()) return false;
    uint64_t family = 0;
    if (!ParseUnsignedValue(*family_it->second, &family)) return false;
    if (family == static_cast<uint8_t>(packet::AddressFamily::kIPv4)) {
        const auto address_it = fields.find(std::string(prefix) + "_ip_v4");
        uint64_t address = 0;
        if (address_it == fields.end() || fields.count(std::string(prefix) + "_ip_v6") != 0 ||
            !ParseUnsignedValue(*address_it->second, &address) ||
            address > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        output->family = packet::AddressFamily::kIPv4;
        output->ipv4_network_order = static_cast<uint32_t>(address);
        return true;
    }
    if (family == static_cast<uint8_t>(packet::AddressFamily::kIPv6)) {
        const auto address_it = fields.find(std::string(prefix) + "_ip_v6");
        if (address_it == fields.end() || fields.count(std::string(prefix) + "_ip_v4") != 0 ||
            address_it->second->value.size() != output->ipv6.size()) {
            return false;
        }
        output->family = packet::AddressFamily::kIPv6;
        std::copy(address_it->second->value.begin(), address_it->second->value.end(),
                  output->ipv6.begin());
        return true;
    }
    return false;
}

bool CollectEqualities(const std::vector<const CanonicalFilterNode*>& terms,
                       std::unordered_map<std::string, const CanonicalFilterNode*>* fields) {
    for (const auto* term : terms) {
        const CanonicalFilterNode* field = nullptr;
        const CanonicalFilterNode* literal = nullptr;
        if (!MatchEquality(*term, &field, &literal) ||
            !fields->emplace(field->field_name, literal).second) {
            return false;
        }
    }
    return true;
}

bool MatchIpDirection(const CanonicalFilterNode& node,
                      const char* prefix,
                      packet::PacketIpKey* output) {
    std::vector<const CanonicalFilterNode*> terms;
    FlattenCanonical(node, "and", &terms);
    if (terms.size() != 2) return false;
    std::unordered_map<std::string, const CanonicalFilterNode*> fields;
    if (!CollectEqualities(terms, &fields)) return false;
    const std::string family_name = std::string(prefix) + "_ip_family";
    const std::string ipv4_name = std::string(prefix) + "_ip_v4";
    const std::string ipv6_name = std::string(prefix) + "_ip_v6";
    if (fields.size() != 2 || fields.count(family_name) != 1 ||
        (fields.count(ipv4_name) + fields.count(ipv6_name)) != 1) {
        return false;
    }
    return BuildIpKey(fields, prefix, output);
}

bool MatchIpRule(const CanonicalFilterNode& node, packet::PacketFilterRule* output) {
    std::vector<const CanonicalFilterNode*> terms;
    FlattenCanonical(node, "or", &terms);
    if (node.kind != "or" || terms.size() != 2) return false;
    packet::PacketIpKey source;
    packet::PacketIpKey destination;
    if (!MatchIpDirection(*terms[0], "src", &source) ||
        !MatchIpDirection(*terms[1], "dst", &destination)) {
        source = {};
        destination = {};
        if (!MatchIpDirection(*terms[1], "src", &source) ||
            !MatchIpDirection(*terms[0], "dst", &destination)) {
            return false;
        }
    }
    if (!PacketIpEqual(source, destination)) return false;
    output->kind = packet::PacketFilterRuleKind::kIpAnyOf;
    output->ip_keys.push_back(source);
    return true;
}

bool MatchPortRule(const CanonicalFilterNode& node, packet::PacketFilterRule* output) {
    std::vector<const CanonicalFilterNode*> terms;
    FlattenCanonical(node, "and", &terms);
    if (node.kind != "and" || terms.size() != 2) return false;
    const CanonicalFilterNode* ports_valid = nullptr;
    const CanonicalFilterNode* either_port = nullptr;
    for (const auto* term : terms) {
        if (MatchBooleanField(*term, "ports_valid")) {
            if (ports_valid) return false;
            ports_valid = term;
        } else {
            if (either_port) return false;
            either_port = term;
        }
    }
    if (!ports_valid || !either_port || either_port->kind != "or") return false;
    std::vector<const CanonicalFilterNode*> port_terms;
    FlattenCanonical(*either_port, "or", &port_terms);
    if (port_terms.size() != 2) return false;
    uint64_t source = 0;
    uint64_t destination = 0;
    bool have_source = false;
    bool have_destination = false;
    for (const auto* term : port_terms) {
        const CanonicalFilterNode* field = nullptr;
        const CanonicalFilterNode* literal = nullptr;
        uint64_t value = 0;
        if (!MatchEquality(*term, &field, &literal) || !ParseUnsignedValue(*literal, &value) ||
            value > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        if (field->field_name == "src_port" && !have_source) {
            source = value;
            have_source = true;
        } else if (field->field_name == "dst_port" && !have_destination) {
            destination = value;
            have_destination = true;
        } else {
            return false;
        }
    }
    if (!have_source || !have_destination || source != destination) return false;
    output->kind = packet::PacketFilterRuleKind::kPortAnyOf;
    output->ports.push_back(static_cast<uint16_t>(source));
    return true;
}

bool MatchTransportDirection(const CanonicalFilterNode& node,
                             packet::PacketEndpointKey* source,
                             packet::PacketEndpointKey* destination) {
    std::vector<const CanonicalFilterNode*> terms;
    FlattenCanonical(node, "and", &terms);
    if (terms.size() != 6) return false;
    std::unordered_map<std::string, const CanonicalFilterNode*> fields;
    if (!CollectEqualities(terms, &fields) || fields.size() != 6 ||
        !BuildIpKey(fields, "src", &source->address) ||
        !BuildIpKey(fields, "dst", &destination->address)) {
        return false;
    }
    const auto src_port = fields.find("src_port");
    const auto dst_port = fields.find("dst_port");
    uint64_t source_value = 0;
    uint64_t destination_value = 0;
    if (src_port == fields.end() || dst_port == fields.end() ||
        !ParseUnsignedValue(*src_port->second, &source_value) ||
        !ParseUnsignedValue(*dst_port->second, &destination_value) ||
        source_value > std::numeric_limits<uint16_t>::max() ||
        destination_value > std::numeric_limits<uint16_t>::max() ||
        source->address.family != destination->address.family) {
        return false;
    }
    source->port = static_cast<uint16_t>(source_value);
    destination->port = static_cast<uint16_t>(destination_value);
    return true;
}

bool MatchTransportRule(const CanonicalFilterNode& node, packet::PacketFilterRule* output) {
    std::vector<const CanonicalFilterNode*> terms;
    FlattenCanonical(node, "and", &terms);
    if (node.kind != "and" || terms.size() != 3) return false;
    const CanonicalFilterNode* direction_or = nullptr;
    bool have_ports_valid = false;
    uint64_t protocol = 0;
    bool have_protocol = false;
    for (const auto* term : terms) {
        if (MatchBooleanField(*term, "ports_valid")) {
            if (have_ports_valid) return false;
            have_ports_valid = true;
            continue;
        }
        const CanonicalFilterNode* field = nullptr;
        const CanonicalFilterNode* literal = nullptr;
        if (MatchEquality(*term, &field, &literal) &&
            field->field_name == "transport_protocol") {
            if (have_protocol || !ParseUnsignedValue(*literal, &protocol)) return false;
            have_protocol = true;
            continue;
        }
        if (direction_or || term->kind != "or") return false;
        direction_or = term;
    }
    if (!have_ports_valid || !have_protocol || (protocol != 6 && protocol != 17) ||
        !direction_or) {
        return false;
    }
    std::vector<const CanonicalFilterNode*> directions;
    FlattenCanonical(*direction_or, "or", &directions);
    if (directions.size() != 2) return false;
    packet::PacketEndpointKey first_source;
    packet::PacketEndpointKey first_destination;
    packet::PacketEndpointKey second_source;
    packet::PacketEndpointKey second_destination;
    if (!MatchTransportDirection(*directions[0], &first_source, &first_destination) ||
        !MatchTransportDirection(*directions[1], &second_source, &second_destination) ||
        !PacketEndpointEqual(first_source, second_destination) ||
        !PacketEndpointEqual(first_destination, second_source)) {
        return false;
    }
    packet::TransportPairKey pair;
    pair.transport_protocol = static_cast<uint8_t>(protocol);
    pair.first = first_source;
    pair.second = first_destination;
    if (PacketEndpointLess(pair.second, pair.first)) std::swap(pair.first, pair.second);
    output->kind = packet::PacketFilterRuleKind::kTransportPairAnyOf;
    output->transport_pairs.push_back(std::move(pair));
    return true;
}

class PacketPlanCompiler {
 public:
    explicit PacketPlanCompiler(std::string* error) : error_(error) {}

    int Compile(const std::optional<CanonicalFilterNode>& root, packet::PcapFilterPlan* output) {
        output->version = packet::kPcapFilterPlanVersion;
        output->endpoint_scope = packet::EndpointScope::kInnermost;
        if (!root) {
            output->root.kind = packet::PacketFilterRuleKind::kMatchAll;
            return 0;
        }
        if (!CompileNode(*root, &output->root)) return EINVAL;
        CanonicalizePacketFilterRuleKeys(&output->root);
        return 0;
    }

 private:
    bool Fail(const CanonicalFilterNode& node, const std::string& message) {
        if (error_) *error_ = message + " at node " + std::to_string(node.node_id);
        return false;
    }

    bool MakeTimeRange(const CanonicalFilterNode& node,
                       const std::string& op,
                       int64_t value,
                       packet::PacketFilterRule* output) {
        output->kind = packet::PacketFilterRuleKind::kTimeRange;
        if (op == "equal") {
            output->time_range.lower_ns = value;
            output->time_range.upper_ns = value;
            output->time_range.lower_inclusive = true;
            output->time_range.upper_inclusive = true;
        } else if (op == "less" || op == "less_equal") {
            output->time_range.upper_ns = value;
            output->time_range.upper_inclusive = op == "less_equal";
        } else if (op == "greater" || op == "greater_equal") {
            output->time_range.lower_ns = value;
            output->time_range.lower_inclusive = op == "greater_equal";
        } else if (op == "not_equal") {
            output->kind = packet::PacketFilterRuleKind::kOr;
            output->operands.resize(2);
            MakeTimeRange(node, "less", value, &output->operands[0]);
            MakeTimeRange(node, "greater", value, &output->operands[1]);
        } else {
            return Fail(node, "unsupported timestamp comparison");
        }
        return true;
    }

    bool MakeUnsignedRange(const CanonicalFilterNode& node,
                           packet::PacketUnsignedField field,
                           const std::string& op,
                           uint64_t value,
                           packet::PacketFilterRule* output) {
        output->kind = packet::PacketFilterRuleKind::kUnsignedRange;
        output->unsigned_range.field = field;
        if (op == "equal") {
            output->unsigned_range.lower = value;
            output->unsigned_range.upper = value;
            output->unsigned_range.lower_inclusive = true;
            output->unsigned_range.upper_inclusive = true;
        } else if (op == "less" || op == "less_equal") {
            output->unsigned_range.upper = value;
            output->unsigned_range.upper_inclusive = op == "less_equal";
        } else if (op == "greater" || op == "greater_equal") {
            output->unsigned_range.lower = value;
            output->unsigned_range.lower_inclusive = op == "greater_equal";
        } else if (op == "not_equal") {
            output->kind = packet::PacketFilterRuleKind::kOr;
            output->operands.resize(2);
            MakeUnsignedRange(node, field, "less", value, &output->operands[0]);
            MakeUnsignedRange(node, field, "greater", value, &output->operands[1]);
        } else {
            return Fail(node, "unsupported unsigned comparison");
        }
        return true;
    }

    bool ResolveUnsignedField(const CanonicalFilterNode& field,
                              packet::PacketUnsignedField* output) const {
        if (field.field_name == "captured_len") {
            *output = packet::PacketUnsignedField::kCapturedLen;
        } else if (field.field_name == "wire_len") {
            *output = packet::PacketUnsignedField::kWireLen;
        } else if (field.field_name == "source_id") {
            *output = packet::PacketUnsignedField::kSourceId;
        } else if (field.field_name == "sequence") {
            *output = packet::PacketUnsignedField::kSequence;
        } else {
            return false;
        }
        return true;
    }

    std::string ReverseComparison(std::string op) const {
        if (op == "less") return "greater";
        if (op == "less_equal") return "greater_equal";
        if (op == "greater") return "less";
        if (op == "greater_equal") return "less_equal";
        return op;
    }

    bool CompileComparison(const CanonicalFilterNode& node,
                           packet::PacketFilterRule* output) {
        const CanonicalFilterNode* field = &node.operands[0];
        const CanonicalFilterNode* literal = &node.operands[1];
        std::string op = node.compare_op;
        if (field->kind == "literal" && literal->kind == "field") {
            std::swap(field, literal);
            op = ReverseComparison(std::move(op));
        }
        if (field->kind != "field" || literal->kind != "literal" ||
            field->type != literal->type || node.comparison_type != field->type) {
            return Fail(node, "packet comparison must contain one matching field and literal");
        }
        if (field->field_name == "timestamp_ns" && field->type == "int64") {
            int64_t value = 0;
            return ParseSignedValue(*literal, &value)
                       ? MakeTimeRange(node, op, value, output)
                       : Fail(node, "timestamp comparison literal is invalid");
        }
        packet::PacketUnsignedField unsigned_field;
        uint64_t value = 0;
        if (!ResolveUnsignedField(*field, &unsigned_field)) {
            return Fail(node, "directional or unsupported packet comparison cannot be compiled");
        }
        if (!ParseUnsignedValue(*literal, &value)) {
            return Fail(node, "unsigned comparison literal is invalid");
        }
        if ((field->type == "uint32" && value > std::numeric_limits<uint32_t>::max()) ||
            (field->type != "uint32" && field->type != "uint64")) {
            return Fail(node, "unsigned comparison literal is outside the field type");
        }
        return MakeUnsignedRange(node, unsigned_field, op, value, output);
    }

    bool CompileBetween(const CanonicalFilterNode& node,
                        packet::PacketFilterRule* output) {
        const auto& field = node.operands[0];
        if (field.kind != "field" || node.operands[1].kind != "literal" ||
            node.operands[2].kind != "literal" || node.comparison_type != field.type ||
            node.operands[1].type != field.type || node.operands[2].type != field.type) {
            return Fail(node, "packet BETWEEN must contain one field and two matching literals");
        }
        if (field.field_name == "timestamp_ns" && field.type == "int64") {
            int64_t lower = 0;
            int64_t upper = 0;
            if (!ParseSignedValue(node.operands[1], &lower) ||
                !ParseSignedValue(node.operands[2], &upper)) {
                return Fail(node, "timestamp BETWEEN bound is invalid");
            }
            output->kind = packet::PacketFilterRuleKind::kTimeRange;
            output->time_range.lower_ns = lower;
            output->time_range.upper_ns = upper;
            output->time_range.lower_inclusive = true;
            output->time_range.upper_inclusive = true;
            return true;
        }
        packet::PacketUnsignedField unsigned_field;
        uint64_t lower = 0;
        uint64_t upper = 0;
        if (!ResolveUnsignedField(field, &unsigned_field) ||
            !ParseUnsignedValue(node.operands[1], &lower) ||
            !ParseUnsignedValue(node.operands[2], &upper)) {
            return Fail(node, "unsigned BETWEEN bound is invalid or unsupported");
        }
        if ((field.type == "uint32" &&
             (lower > std::numeric_limits<uint32_t>::max() ||
              upper > std::numeric_limits<uint32_t>::max())) ||
            (field.type != "uint32" && field.type != "uint64")) {
            return Fail(node, "unsigned BETWEEN bound is outside the field type");
        }
        output->kind = packet::PacketFilterRuleKind::kUnsignedRange;
        output->unsigned_range.field = unsigned_field;
        output->unsigned_range.lower = lower;
        output->unsigned_range.upper = upper;
        output->unsigned_range.lower_inclusive = true;
        output->unsigned_range.upper_inclusive = true;
        return true;
    }

    bool CompileIn(const CanonicalFilterNode& node, packet::PacketFilterRule* output) {
        const auto& field = node.operands[0];
        if (field.kind != "field" || node.comparison_type != field.type) {
            return Fail(node, "packet IN must begin with one typed field");
        }
        std::vector<packet::PacketFilterRule> values;
        values.reserve(node.operands.size() - 1);
        for (size_t index = 1; index < node.operands.size(); ++index) {
            CanonicalFilterNode equality;
            equality.node_id = node.node_id;
            equality.kind = "compare";
            equality.type = "bool";
            equality.compare_op = "equal";
            equality.comparison_type = node.comparison_type;
            equality.operands = {field, node.operands[index]};
            packet::PacketFilterRule compiled;
            if (!CompileComparison(equality, &compiled)) return false;
            values.push_back(std::move(compiled));
        }
        if (values.size() == 1) {
            *output = std::move(values[0]);
        } else {
            output->kind = packet::PacketFilterRuleKind::kOr;
            output->operands = std::move(values);
        }
        return true;
    }

    bool CompileNode(const CanonicalFilterNode& node, packet::PacketFilterRule* output) {
        *output = {};
        if (MatchTransportRule(node, output) || MatchPortRule(node, output) ||
            MatchIpRule(node, output) || MatchMacRule(node, output)) {
            return true;
        }
        if (node.kind == "literal" && node.type == "bool") {
            if (node.value == "true") {
                output->kind = packet::PacketFilterRuleKind::kMatchAll;
                return true;
            }
            if (node.value == "false") {
                output->kind = packet::PacketFilterRuleKind::kMatchNone;
                return true;
            }
            return Fail(node, "boolean literal is invalid");
        }
        if (node.kind == "compare") return CompileComparison(node, output);
        if (node.kind == "between") return CompileBetween(node, output);
        if (node.kind == "in") return CompileIn(node, output);
        if (node.kind == "and" || node.kind == "or" || node.kind == "not") {
            output->kind = node.kind == "and"   ? packet::PacketFilterRuleKind::kAnd
                           : node.kind == "or" ? packet::PacketFilterRuleKind::kOr
                                                : packet::PacketFilterRuleKind::kNot;
            output->operands.resize(node.operands.size());
            for (size_t index = 0; index < node.operands.size(); ++index) {
                if (!CompileNode(node.operands[index], &output->operands[index])) return false;
            }
            return true;
        }
        return Fail(node, "canonical packet subtree is not exactly representable");
    }

    std::string* error_ = nullptr;
};

template <typename T>
bool InClosedOrOpenRange(T value,
                         const std::optional<T>& lower,
                         bool lower_inclusive,
                         const std::optional<T>& upper,
                         bool upper_inclusive) {
    if (lower && (value < *lower || (!lower_inclusive && value == *lower))) return false;
    if (upper && (value > *upper || (!upper_inclusive && value == *upper))) return false;
    return true;
}

uint64_t PacketUnsignedValue(packet::PacketUnsignedField field,
                             const packet::PacketMeta& meta) {
    switch (field) {
        case packet::PacketUnsignedField::kCapturedLen:
            return meta.captured_len;
        case packet::PacketUnsignedField::kWireLen:
            return meta.wire_len;
        case packet::PacketUnsignedField::kSourceId:
            return meta.source_id;
        case packet::PacketUnsignedField::kSequence:
            return meta.sequence;
    }
    return 0;
}

bool MatchLayerIp(const packet::IpAddress& address, const packet::PacketIpKey& key) {
    if (key.family == packet::AddressFamily::kIPv4) {
        const auto* value = std::get_if<packet::IPv4Address>(&address);
        return value && value->addr == key.ipv4_network_order;
    }
    if (key.family == packet::AddressFamily::kIPv6) {
        const auto* value = std::get_if<packet::IPv6Address>(&address);
        return value && std::equal(key.ipv6.begin(), key.ipv6.end(), value->bytes);
    }
    return false;
}

bool BuildLayerEndpoint(const packet::IpAddress& address,
                        uint16_t port,
                        packet::PacketEndpointKey* output) {
    if (const auto* value = std::get_if<packet::IPv4Address>(&address)) {
        output->address.family = packet::AddressFamily::kIPv4;
        output->address.ipv4_network_order = value->addr;
    } else if (const auto* value = std::get_if<packet::IPv6Address>(&address)) {
        output->address.family = packet::AddressFamily::kIPv6;
        std::copy(std::begin(value->bytes), std::end(value->bytes), output->address.ipv6.begin());
    } else {
        return false;
    }
    output->port = port;
    return true;
}

bool EvaluateDecodedRule(const packet::PacketFilterRule& rule,
                         const packet::PacketMeta& meta,
                         const packet::PacketLayerInfo& layer);

PacketHeaderFilterResult EvaluateHeaderRule(const packet::PacketFilterRule& rule,
                                            const packet::PacketMeta& meta) {
    switch (rule.kind) {
        case packet::PacketFilterRuleKind::kMatchAll:
            return PacketHeaderFilterResult::kMatch;
        case packet::PacketFilterRuleKind::kMatchNone:
            return PacketHeaderFilterResult::kReject;
        case packet::PacketFilterRuleKind::kTimeRange:
            return InClosedOrOpenRange(meta.timestamp_ns, rule.time_range.lower_ns,
                                       rule.time_range.lower_inclusive,
                                       rule.time_range.upper_ns,
                                       rule.time_range.upper_inclusive)
                       ? PacketHeaderFilterResult::kMatch
                       : PacketHeaderFilterResult::kReject;
        case packet::PacketFilterRuleKind::kUnsignedRange: {
            const uint64_t value = PacketUnsignedValue(rule.unsigned_range.field, meta);
            return InClosedOrOpenRange(value, rule.unsigned_range.lower,
                                       rule.unsigned_range.lower_inclusive,
                                       rule.unsigned_range.upper,
                                       rule.unsigned_range.upper_inclusive)
                       ? PacketHeaderFilterResult::kMatch
                       : PacketHeaderFilterResult::kReject;
        }
        case packet::PacketFilterRuleKind::kAnd: {
            bool unknown = false;
            for (const auto& operand : rule.operands) {
                const auto result = EvaluateHeaderRule(operand, meta);
                if (result == PacketHeaderFilterResult::kReject) return result;
                unknown = unknown || result == PacketHeaderFilterResult::kNeedDecoded;
            }
            return unknown ? PacketHeaderFilterResult::kNeedDecoded
                           : PacketHeaderFilterResult::kMatch;
        }
        case packet::PacketFilterRuleKind::kOr: {
            bool unknown = false;
            for (const auto& operand : rule.operands) {
                const auto result = EvaluateHeaderRule(operand, meta);
                if (result == PacketHeaderFilterResult::kMatch) return result;
                unknown = unknown || result == PacketHeaderFilterResult::kNeedDecoded;
            }
            return unknown ? PacketHeaderFilterResult::kNeedDecoded
                           : PacketHeaderFilterResult::kReject;
        }
        case packet::PacketFilterRuleKind::kNot: {
            const auto result = EvaluateHeaderRule(rule.operands.front(), meta);
            if (result == PacketHeaderFilterResult::kReject) return PacketHeaderFilterResult::kMatch;
            if (result == PacketHeaderFilterResult::kMatch) return PacketHeaderFilterResult::kReject;
            return result;
        }
        case packet::PacketFilterRuleKind::kMacAnyOf:
        case packet::PacketFilterRuleKind::kIpAnyOf:
        case packet::PacketFilterRuleKind::kPortAnyOf:
        case packet::PacketFilterRuleKind::kTransportPairAnyOf:
            return PacketHeaderFilterResult::kNeedDecoded;
    }
    return PacketHeaderFilterResult::kReject;
}

bool EvaluateDecodedRule(const packet::PacketFilterRule& rule,
                         const packet::PacketMeta& meta,
                         const packet::PacketLayerInfo& layer) {
    switch (rule.kind) {
        case packet::PacketFilterRuleKind::kMatchAll:
            return true;
        case packet::PacketFilterRuleKind::kMatchNone:
            return false;
        case packet::PacketFilterRuleKind::kTimeRange:
        case packet::PacketFilterRuleKind::kUnsignedRange:
            return EvaluateHeaderRule(rule, meta) == PacketHeaderFilterResult::kMatch;
        case packet::PacketFilterRuleKind::kAnd:
            return std::all_of(rule.operands.begin(), rule.operands.end(), [&](const auto& operand) {
                return EvaluateDecodedRule(operand, meta, layer);
            });
        case packet::PacketFilterRuleKind::kOr:
            return std::any_of(rule.operands.begin(), rule.operands.end(), [&](const auto& operand) {
                return EvaluateDecodedRule(operand, meta, layer);
            });
        case packet::PacketFilterRuleKind::kNot:
            return !EvaluateDecodedRule(rule.operands.front(), meta, layer);
        case packet::PacketFilterRuleKind::kMacAnyOf:
            if (layer.status != packet::LayerStatus::kDecoded) return false;
            return std::any_of(rule.mac_keys.begin(), rule.mac_keys.end(), [&](const auto& key) {
                return (layer.src_mac.valid &&
                        std::equal(key.bytes.begin(), key.bytes.end(), layer.src_mac.value.bytes)) ||
                       (layer.dst_mac.valid &&
                        std::equal(key.bytes.begin(), key.bytes.end(), layer.dst_mac.value.bytes));
            });
        case packet::PacketFilterRuleKind::kIpAnyOf:
            if (layer.status != packet::LayerStatus::kDecoded) return false;
            return std::any_of(rule.ip_keys.begin(), rule.ip_keys.end(), [&](const auto& key) {
                return MatchLayerIp(layer.src_ip, key) || MatchLayerIp(layer.dst_ip, key);
            });
        case packet::PacketFilterRuleKind::kPortAnyOf:
            if (layer.status != packet::LayerStatus::kDecoded || !layer.ports_valid) return false;
            return std::find(rule.ports.begin(), rule.ports.end(), layer.src_port) != rule.ports.end() ||
                   std::find(rule.ports.begin(), rule.ports.end(), layer.dst_port) != rule.ports.end();
        case packet::PacketFilterRuleKind::kTransportPairAnyOf: {
            if (layer.status != packet::LayerStatus::kDecoded || !layer.ports_valid) return false;
            packet::PacketEndpointKey first;
            packet::PacketEndpointKey second;
            if (!BuildLayerEndpoint(layer.src_ip, layer.src_port, &first) ||
                !BuildLayerEndpoint(layer.dst_ip, layer.dst_port, &second) ||
                first.address.family != second.address.family) {
                return false;
            }
            if (PacketEndpointLess(second, first)) std::swap(first, second);
            return std::any_of(rule.transport_pairs.begin(), rule.transport_pairs.end(),
                               [&](const auto& pair) {
                                   return pair.transport_protocol == layer.transport_protocol &&
                                          PacketEndpointEqual(pair.first, first) &&
                                          PacketEndpointEqual(pair.second, second);
                               });
        }
    }
    return false;
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

int CompilePcapFilterPlanJson(const std::string& canonical_json,
                              packet::PcapFilterPlan* output,
                              std::string* error) {
    if (error) error->clear();
    if (!output) {
        if (error) *error = "packet filter plan output must not be null";
        return EINVAL;
    }
    *output = {};
    try {
        std::optional<CanonicalFilterNode> root;
        CanonicalFilterPlanParser parser(error);
        const int parse_rc = parser.Parse(canonical_json, &root);
        if (parse_rc != 0) return parse_rc;
        PacketPlanCompiler compiler(error);
        const int compile_rc = compiler.Compile(root, output);
        if (compile_rc != 0) *output = {};
        return compile_rc;
    } catch (const std::bad_alloc&) {
        *output = {};
        if (error) *error = "packet filter plan compilation allocation failed";
        return ENOMEM;
    }
}

int SelectCompilablePcapFilterNodes(const std::string& canonical_json,
                                    const std::vector<uint32_t>& candidate_node_ids,
                                    std::vector<uint32_t>* accepted_node_ids,
                                    std::string* error) {
    if (error) error->clear();
    if (!accepted_node_ids) {
        if (error) *error = "accepted packet filter node output must not be null";
        return EINVAL;
    }
    accepted_node_ids->clear();
    try {
        std::optional<CanonicalFilterNode> root;
        CanonicalFilterPlanParser parser(error);
        const int parse_rc = parser.Parse(canonical_json, &root);
        if (parse_rc != 0) return parse_rc;

        std::unordered_map<uint32_t, const CanonicalFilterNode*> nodes;
        if (root) IndexCanonicalFilterNodes(*root, &nodes);
        std::unordered_set<uint32_t> requested;
        std::vector<uint32_t> accepted;
        accepted.reserve(candidate_node_ids.size());
        for (const uint32_t candidate_node_id : candidate_node_ids) {
            if (!requested.insert(candidate_node_id).second) {
                if (error) {
                    *error = "packet filter candidate node id is duplicated: " +
                             std::to_string(candidate_node_id);
                }
                return EINVAL;
            }
            const auto node = nodes.find(candidate_node_id);
            if (node == nodes.end()) {
                if (error) {
                    *error = "packet filter candidate node id is unknown: " +
                             std::to_string(candidate_node_id);
                }
                return EINVAL;
            }
            packet::PcapFilterPlan candidate_plan;
            std::string compile_error;
            PacketPlanCompiler compiler(&compile_error);
            if (compiler.Compile(std::optional<CanonicalFilterNode>(*node->second),
                                 &candidate_plan) == 0) {
                accepted.push_back(candidate_node_id);
            }
        }
        *accepted_node_ids = std::move(accepted);
        return 0;
    } catch (const std::bad_alloc&) {
        accepted_node_ids->clear();
        if (error) *error = "packet filter candidate selection allocation failed";
        return ENOMEM;
    }
}

PacketHeaderFilterResult EvaluatePcapHeaderFilter(const packet::PcapFilterPlan& plan,
                                                  const packet::PacketMeta& meta) {
    if (plan.version != packet::kPcapFilterPlanVersion) {
        return PacketHeaderFilterResult::kReject;
    }
    return EvaluateHeaderRule(plan.root, meta);
}

bool EvaluatePcapDecodedFilter(const packet::PcapFilterPlan& plan,
                               const packet::PacketMeta& meta,
                               const packet::PacketLayerInfo& layer) {
    return plan.version == packet::kPcapFilterPlanVersion &&
           EvaluateDecodedRule(plan.root, meta, layer);
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
