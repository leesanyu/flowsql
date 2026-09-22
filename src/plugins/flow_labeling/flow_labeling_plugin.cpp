// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "flow_labeling_plugin.h"

#include <rte_acl.h>
#include <rte_byteorder.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_memory.h>
#include <yaml-cpp/yaml.h>

#include <arpa/inet.h>
#include <sched.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flowsql {
namespace {

constexpr const char* kSchemaId = "flowsql.io/flow-labeling/v1alpha1";
constexpr const char* kKind = "FlowLabelingSet";
constexpr uint32_t kClassificationBatch = 256;
constexpr uint64_t kMaxSnapshotBytes = 8ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMatcherOverheadBytes = 64ULL * 1024ULL;
std::atomic<uint64_t> g_context_sequence{0};
thread_local std::string g_diagnostic_path;
thread_local std::string g_diagnostic_detail;

void SetDiagnostic(FlowLabelingDiagnosticV1* diagnostic, FlowLabelingErrorV1 error, std::string path,
                   std::string detail) {
    if (diagnostic == nullptr) return;
    g_diagnostic_path = std::move(path);
    g_diagnostic_detail = std::move(detail);
    diagnostic->error = error;
    diagnostic->path = g_diagnostic_path.empty() ? nullptr : g_diagnostic_path.c_str();
    diagnostic->detail = g_diagnostic_detail.empty() ? nullptr : g_diagnostic_detail.c_str();
}

struct LabelRecord {
    uint32_t id = 0;
    int32_t priority = 0;
    std::string name;
    std::string display_name;
    std::string description;
};

#pragma pack(push, 1)
struct AclTuple {
    uint8_t ip_family = 0;
    uint8_t first_padding[3]{};
    uint32_t presence_bits = 0;

    uint64_t observation_domain_range = 0;
    uint64_t observation_domain_bitmask = 0;

    uint16_t source_mac_head = 0;
    uint16_t destination_mac_head = 0;
    uint32_t source_mac_tail = 0;
    uint32_t destination_mac_tail = 0;

    uint16_t outer_vlan_tpid_bitmask = 0;
    uint16_t outer_vlan_vid_range = 0;
    uint16_t inner_vlan_tpid_bitmask = 0;
    uint16_t inner_vlan_vid_range = 0;
    uint16_t outer_vlan_tpid_mask = 0;
    uint16_t inner_vlan_tpid_mask = 0;

    uint8_t source_ip_prefix[16]{};
    uint8_t destination_ip_prefix[16]{};
    uint32_t source_ipv4_range = 0;
    uint32_t destination_ipv4_range = 0;
    uint32_t source_ipv4_bitmask = 0;
    uint32_t destination_ipv4_bitmask = 0;

    uint8_t protocol_bitmask = 0;
    uint8_t protocol_mask = 0;
    uint8_t protocol_range = 0;
    uint8_t protocol_padding = 0;
    uint16_t source_port_range = 0;
    uint16_t destination_port_range = 0;
};
#pragma pack(pop)

static_assert(sizeof(AclTuple) == 104, "ACL tuple layout must remain fixed");

enum PresenceBit : uint32_t {
    kSourceMacPresent = uint32_t{1} << 0,
    kDestinationMacPresent = uint32_t{1} << 1,
    kSourceIpPresent = uint32_t{1} << 2,
    kDestinationIpPresent = uint32_t{1} << 3,
    kTransportPresent = uint32_t{1} << 4,
    kSourcePortPresent = uint32_t{1} << 5,
    kDestinationPortPresent = uint32_t{1} << 6,
    kOuterVlanPresent = uint32_t{1} << 7,
    kInnerVlanPresent = uint32_t{1} << 8,
    kImpossiblePresence = uint32_t{1} << 31,
};

enum AclField : size_t {
    kIpFamily = 0,
    kPresenceBits,
    kObservationDomainRange,
    kObservationDomainBitmask,
    kSourceMacHead,
    kDestinationMacHead,
    kSourceMacTail,
    kDestinationMacTail,
    kOuterVlanTpidBitmask,
    kOuterVlanVidRange,
    kInnerVlanTpidBitmask,
    kInnerVlanVidRange,
    kOuterVlanTpidMask,
    kInnerVlanTpidMask,
    kSourceIpPrefix0,
    kSourceIpPrefix1,
    kSourceIpPrefix2,
    kSourceIpPrefix3,
    kDestinationIpPrefix0,
    kDestinationIpPrefix1,
    kDestinationIpPrefix2,
    kDestinationIpPrefix3,
    kSourceIpv4Range,
    kDestinationIpv4Range,
    kSourceIpv4Bitmask,
    kDestinationIpv4Bitmask,
    kProtocolBitmask,
    kProtocolMask,
    kProtocolRange,
    kProtocolPadding,
    kSourcePortRange,
    kDestinationPortRange,
    kAclFieldCount,
};

struct FlowAclRule {
    rte_acl_rule_data data{};
    rte_acl_field field[kAclFieldCount]{};
};

static_assert(kAclFieldCount == 32, "single-context ACL layout must use 32 fields");
static_assert(sizeof(FlowAclRule) == 528, "compressed ACL rule size must remain fixed");

constexpr rte_acl_field_def Field(uint8_t type, uint8_t size, uint8_t index, uint8_t input, uint32_t offset) {
    return {type, size, index, input, offset};
}

const std::array<rte_acl_field_def, kAclFieldCount> kFieldDefinitions = {
    Field(RTE_ACL_FIELD_TYPE_BITMASK, 1, kIpFamily, 0, offsetof(AclTuple, ip_family)),
    Field(RTE_ACL_FIELD_TYPE_BITMASK, 4, kPresenceBits, 1, offsetof(AclTuple, presence_bits)),
    Field(RTE_ACL_FIELD_TYPE_RANGE, 8, kObservationDomainRange, 2, offsetof(AclTuple, observation_domain_range)),
    Field(RTE_ACL_FIELD_TYPE_BITMASK, 8, kObservationDomainBitmask, 4, offsetof(AclTuple, observation_domain_bitmask)),
    Field(RTE_ACL_FIELD_TYPE_MASK, 2, kSourceMacHead, 6, offsetof(AclTuple, source_mac_head)),
    Field(RTE_ACL_FIELD_TYPE_MASK, 2, kDestinationMacHead, 6, offsetof(AclTuple, destination_mac_head)),
    Field(RTE_ACL_FIELD_TYPE_MASK, 4, kSourceMacTail, 7, offsetof(AclTuple, source_mac_tail)),
    Field(RTE_ACL_FIELD_TYPE_MASK, 4, kDestinationMacTail, 8, offsetof(AclTuple, destination_mac_tail)),
    Field(RTE_ACL_FIELD_TYPE_BITMASK, 2, kOuterVlanTpidBitmask, 9, offsetof(AclTuple, outer_vlan_tpid_bitmask)),
    Field(RTE_ACL_FIELD_TYPE_RANGE, 2, kOuterVlanVidRange, 9, offsetof(AclTuple, outer_vlan_vid_range)),
    Field(RTE_ACL_FIELD_TYPE_BITMASK, 2, kInnerVlanTpidBitmask, 10, offsetof(AclTuple, inner_vlan_tpid_bitmask)),
    Field(RTE_ACL_FIELD_TYPE_RANGE, 2, kInnerVlanVidRange, 10, offsetof(AclTuple, inner_vlan_vid_range)),
    Field(RTE_ACL_FIELD_TYPE_MASK, 2, kOuterVlanTpidMask, 11, offsetof(AclTuple, outer_vlan_tpid_mask)),
    Field(RTE_ACL_FIELD_TYPE_MASK, 2, kInnerVlanTpidMask, 11, offsetof(AclTuple, inner_vlan_tpid_mask)),
    Field(RTE_ACL_FIELD_TYPE_MASK, 4, kSourceIpPrefix0, 12, offsetof(AclTuple, source_ip_prefix[0])),
    Field(RTE_ACL_FIELD_TYPE_MASK, 4, kSourceIpPrefix1, 13, offsetof(AclTuple, source_ip_prefix[4])),
    Field(RTE_ACL_FIELD_TYPE_MASK, 4, kSourceIpPrefix2, 14, offsetof(AclTuple, source_ip_prefix[8])),
    Field(RTE_ACL_FIELD_TYPE_MASK, 4, kSourceIpPrefix3, 15, offsetof(AclTuple, source_ip_prefix[12])),
    Field(RTE_ACL_FIELD_TYPE_MASK, 4, kDestinationIpPrefix0, 16, offsetof(AclTuple, destination_ip_prefix[0])),
    Field(RTE_ACL_FIELD_TYPE_MASK, 4, kDestinationIpPrefix1, 17, offsetof(AclTuple, destination_ip_prefix[4])),
    Field(RTE_ACL_FIELD_TYPE_MASK, 4, kDestinationIpPrefix2, 18, offsetof(AclTuple, destination_ip_prefix[8])),
    Field(RTE_ACL_FIELD_TYPE_MASK, 4, kDestinationIpPrefix3, 19, offsetof(AclTuple, destination_ip_prefix[12])),
    Field(RTE_ACL_FIELD_TYPE_RANGE, 4, kSourceIpv4Range, 20, offsetof(AclTuple, source_ipv4_range)),
    Field(RTE_ACL_FIELD_TYPE_RANGE, 4, kDestinationIpv4Range, 21, offsetof(AclTuple, destination_ipv4_range)),
    Field(RTE_ACL_FIELD_TYPE_BITMASK, 4, kSourceIpv4Bitmask, 22, offsetof(AclTuple, source_ipv4_bitmask)),
    Field(RTE_ACL_FIELD_TYPE_BITMASK, 4, kDestinationIpv4Bitmask, 23, offsetof(AclTuple, destination_ipv4_bitmask)),
    Field(RTE_ACL_FIELD_TYPE_BITMASK, 1, kProtocolBitmask, 24, offsetof(AclTuple, protocol_bitmask)),
    Field(RTE_ACL_FIELD_TYPE_MASK, 1, kProtocolMask, 24, offsetof(AclTuple, protocol_mask)),
    Field(RTE_ACL_FIELD_TYPE_RANGE, 1, kProtocolRange, 24, offsetof(AclTuple, protocol_range)),
    Field(RTE_ACL_FIELD_TYPE_BITMASK, 1, kProtocolPadding, 24, offsetof(AclTuple, protocol_padding)),
    Field(RTE_ACL_FIELD_TYPE_RANGE, 2, kSourcePortRange, 25, offsetof(AclTuple, source_port_range)),
    Field(RTE_ACL_FIELD_TYPE_RANGE, 2, kDestinationPortRange, 25, offsetof(AclTuple, destination_port_range)),
};

static_assert(kAclFieldCount <= RTE_ACL_MAX_FIELDS, "ACL field count exceeds DPDK limit");

uint64_t MaxForSize(uint8_t size) {
    return size == 8 ? std::numeric_limits<uint64_t>::max() : ((uint64_t{1} << (size * 8)) - 1);
}

void StoreField(union rte_acl_field_types* output, uint8_t size, uint64_t value) {
    switch (size) {
        case 1:
            output->u8 = static_cast<uint8_t>(value);
            break;
        case 2:
            output->u16 = static_cast<uint16_t>(value);
            break;
        case 4:
            output->u32 = static_cast<uint32_t>(value);
            break;
        case 8:
            output->u64 = value;
            break;
    }
}

FlowAclRule WildcardRule(uint32_t label_id, int32_t priority) {
    FlowAclRule rule{};
    rule.data.category_mask = 1;
    rule.data.priority = priority;
    rule.data.userdata = label_id;
    for (size_t index = 0; index < kFieldDefinitions.size(); ++index) {
        if (kFieldDefinitions[index].type == RTE_ACL_FIELD_TYPE_RANGE) {
            StoreField(&rule.field[index].mask_range, kFieldDefinitions[index].size,
                       MaxForSize(kFieldDefinitions[index].size));
        }
    }
    return rule;
}

void SetMask(FlowAclRule* rule, size_t field, uint64_t value, uint64_t prefix_bits) {
    StoreField(&rule->field[field].value, kFieldDefinitions[field].size, value);
    StoreField(&rule->field[field].mask_range, kFieldDefinitions[field].size, prefix_bits);
}

void SetRange(FlowAclRule* rule, size_t field, uint64_t minimum, uint64_t maximum) {
    StoreField(&rule->field[field].value, kFieldDefinitions[field].size, minimum);
    StoreField(&rule->field[field].mask_range, kFieldDefinitions[field].size, maximum);
}

void SetBitmask(FlowAclRule* rule, size_t field, uint64_t value, uint64_t mask) {
    StoreField(&rule->field[field].value, kFieldDefinitions[field].size, value);
    StoreField(&rule->field[field].mask_range, kFieldDefinitions[field].size, mask);
}

void RequirePresent(FlowAclRule* rule, uint32_t bits) {
    const uint32_t required = rule->field[kPresenceBits].mask_range.u32 | bits;
    SetBitmask(rule, kPresenceBits, required, required);
}

uint64_t PrefixMask(uint32_t width, uint32_t prefix) {
    if (prefix == 0) return 0;
    const uint64_t all = width == 64 ? std::numeric_limits<uint64_t>::max() : (uint64_t{1} << width) - 1;
    return all << (width - prefix);
}

void MergeBitmask(FlowAclRule* rule, size_t field, uint64_t value, uint64_t mask) {
    const uint64_t current_value = rule->field[field].value.u64;
    const uint64_t current_mask = rule->field[field].mask_range.u64;
    if (((current_value ^ value) & current_mask & mask) != 0) {
        RequirePresent(rule, kImpossiblePresence);
    }
    const uint64_t merged_mask = current_mask | mask;
    const uint64_t merged_value = (current_value & current_mask) | (value & mask);
    SetBitmask(rule, field, merged_value, merged_mask);
}

bool FlowAclRuleLess(const FlowAclRule& left, const FlowAclRule& right) {
    if (left.data.category_mask != right.data.category_mask) {
        return left.data.category_mask < right.data.category_mask;
    }
    if (left.data.priority != right.data.priority) return left.data.priority < right.data.priority;
    if (left.data.userdata != right.data.userdata) return left.data.userdata < right.data.userdata;
    for (size_t index = 0; index < kAclFieldCount; ++index) {
        if (left.field[index].value.u64 != right.field[index].value.u64) {
            return left.field[index].value.u64 < right.field[index].value.u64;
        }
        if (left.field[index].mask_range.u64 != right.field[index].mask_range.u64) {
            return left.field[index].mask_range.u64 < right.field[index].mask_range.u64;
        }
    }
    return false;
}

bool FlowAclRuleEqual(const FlowAclRule& left, const FlowAclRule& right) {
    return !FlowAclRuleLess(left, right) && !FlowAclRuleLess(right, left);
}

template <typename T>
bool ParseUnsigned(const YAML::Node& node, T* output) {
    static_assert(std::is_unsigned_v<T>);
    if (!node.IsScalar()) return false;
    const std::string value = node.Scalar();
    T parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
    *output = parsed;
    return true;
}

bool ParseSigned(const YAML::Node& node, int32_t* output) {
    if (!node.IsScalar()) return false;
    const std::string value = node.Scalar();
    int32_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
    *output = parsed;
    return true;
}

bool ValidIdentifier(const std::string& value) {
    if (value.empty() || value.size() > 64 || value[0] < 'a' || value[0] > 'z') return false;
    for (char ch : value) {
        if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '-' && ch != '_') return false;
    }
    return true;
}

bool ParseMac(const YAML::Node& node, std::array<uint8_t, 6>* output) {
    if (!node.IsScalar()) return false;
    const std::string value = node.Scalar();
    if (value.size() != 17) return false;
    for (size_t index = 0; index < output->size(); ++index) {
        const size_t offset = index * 3;
        if (index != 0 && value[offset - 1] != ':') return false;
        unsigned byte = 0;
        const auto result = std::from_chars(value.data() + offset, value.data() + offset + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != value.data() + offset + 2 || byte > 255) return false;
        (*output)[index] = static_cast<uint8_t>(byte);
    }
    return true;
}

bool ParseAddress(const YAML::Node& node, int family, uint8_t* output) {
    return node.IsScalar() && inet_pton(family, node.Scalar().c_str(), output) == 1;
}

uint32_t Host32(const uint8_t* bytes) {
    uint32_t network = 0;
    std::memcpy(&network, bytes, sizeof(network));
    return rte_be_to_cpu_32(network);
}

uint16_t Host16(const uint8_t* bytes) {
    uint16_t network = 0;
    std::memcpy(&network, bytes, sizeof(network));
    return rte_be_to_cpu_16(network);
}

enum class LogicalField {
    kObservationDomain,
    kObservationDomainPrefix,
    kObservationDomainBitmask,
    kIpFamily,
    kSourceMacPrefix,
    kDestinationMacPrefix,
    kOuterVlanTpid,
    kOuterVlanTpidPrefix,
    kOuterVlanVid,
    kInnerVlanTpid,
    kInnerVlanTpidPrefix,
    kInnerVlanVid,
    kSourceIpv4Prefix,
    kDestinationIpv4Prefix,
    kSourceIpv4Range,
    kDestinationIpv4Range,
    kSourceIpv4Bitmask,
    kDestinationIpv4Bitmask,
    kSourceIpv6Prefix,
    kDestinationIpv6Prefix,
    kTransportProtocol,
    kTransportProtocolPrefix,
    kTransportProtocolRange,
    kSourcePort,
    kDestinationPort,
};

bool ParseLogicalField(const std::string& value, LogicalField* field) {
    static const std::unordered_map<std::string, LogicalField> fields = {
        {"observation_domain", LogicalField::kObservationDomain},
        {"observation_domain_prefix", LogicalField::kObservationDomainPrefix},
        {"observation_domain_bitmask", LogicalField::kObservationDomainBitmask},
        {"ip_family", LogicalField::kIpFamily},
        {"source_mac_prefix", LogicalField::kSourceMacPrefix},
        {"destination_mac_prefix", LogicalField::kDestinationMacPrefix},
        {"outer_vlan_tpid", LogicalField::kOuterVlanTpid},
        {"outer_vlan_tpid_prefix", LogicalField::kOuterVlanTpidPrefix},
        {"outer_vlan_vid", LogicalField::kOuterVlanVid},
        {"inner_vlan_tpid", LogicalField::kInnerVlanTpid},
        {"inner_vlan_tpid_prefix", LogicalField::kInnerVlanTpidPrefix},
        {"inner_vlan_vid", LogicalField::kInnerVlanVid},
        {"source_ipv4_prefix", LogicalField::kSourceIpv4Prefix},
        {"destination_ipv4_prefix", LogicalField::kDestinationIpv4Prefix},
        {"source_ipv4_range", LogicalField::kSourceIpv4Range},
        {"destination_ipv4_range", LogicalField::kDestinationIpv4Range},
        {"source_ipv4_bitmask", LogicalField::kSourceIpv4Bitmask},
        {"destination_ipv4_bitmask", LogicalField::kDestinationIpv4Bitmask},
        {"source_ipv6_prefix", LogicalField::kSourceIpv6Prefix},
        {"destination_ipv6_prefix", LogicalField::kDestinationIpv6Prefix},
        {"transport_protocol", LogicalField::kTransportProtocol},
        {"transport_protocol_prefix", LogicalField::kTransportProtocolPrefix},
        {"transport_protocol_range", LogicalField::kTransportProtocolRange},
        {"source_port", LogicalField::kSourcePort},
        {"destination_port", LogicalField::kDestinationPort},
    };
    const auto found = fields.find(value);
    if (found == fields.end()) return false;
    *field = found->second;
    return true;
}

LogicalField SwapEndpoints(LogicalField field) {
    switch (field) {
        case LogicalField::kSourceMacPrefix:
            return LogicalField::kDestinationMacPrefix;
        case LogicalField::kDestinationMacPrefix:
            return LogicalField::kSourceMacPrefix;
        case LogicalField::kSourceIpv4Prefix:
            return LogicalField::kDestinationIpv4Prefix;
        case LogicalField::kDestinationIpv4Prefix:
            return LogicalField::kSourceIpv4Prefix;
        case LogicalField::kSourceIpv4Range:
            return LogicalField::kDestinationIpv4Range;
        case LogicalField::kDestinationIpv4Range:
            return LogicalField::kSourceIpv4Range;
        case LogicalField::kSourceIpv4Bitmask:
            return LogicalField::kDestinationIpv4Bitmask;
        case LogicalField::kDestinationIpv4Bitmask:
            return LogicalField::kSourceIpv4Bitmask;
        case LogicalField::kSourceIpv6Prefix:
            return LogicalField::kDestinationIpv6Prefix;
        case LogicalField::kDestinationIpv6Prefix:
            return LogicalField::kSourceIpv6Prefix;
        case LogicalField::kSourcePort:
            return LogicalField::kDestinationPort;
        case LogicalField::kDestinationPort:
            return LogicalField::kSourcePort;
        default:
            return field;
    }
}

bool IsRange(LogicalField field) {
    return field == LogicalField::kObservationDomain || field == LogicalField::kOuterVlanVid ||
           field == LogicalField::kInnerVlanVid || field == LogicalField::kSourceIpv4Range ||
           field == LogicalField::kDestinationIpv4Range || field == LogicalField::kTransportProtocolRange ||
           field == LogicalField::kSourcePort || field == LogicalField::kDestinationPort;
}

bool IsPrefix(LogicalField field) {
    return field == LogicalField::kObservationDomainPrefix || field == LogicalField::kSourceMacPrefix ||
           field == LogicalField::kDestinationMacPrefix || field == LogicalField::kOuterVlanTpidPrefix ||
           field == LogicalField::kInnerVlanTpidPrefix || field == LogicalField::kSourceIpv4Prefix ||
           field == LogicalField::kDestinationIpv4Prefix || field == LogicalField::kSourceIpv6Prefix ||
           field == LogicalField::kDestinationIpv6Prefix || field == LogicalField::kTransportProtocolPrefix;
}

struct CompileResult {
    enum rte_acl_classify_alg algorithm = RTE_ACL_CLASSIFY_DEFAULT;
    size_t max_runtime_bytes = 0;
    std::vector<LabelRecord> labels;
    std::vector<FlowAclRule> rules;
};

class ConfigCompiler {
 public:
    ConfigCompiler(const FlowLabelingCompileRequestV1& request, FlowLabelingDiagnosticV1* diagnostic)
        : request_(request), diagnostic_(diagnostic) {}

    FlowLabelingErrorV1 Compile(CompileResult* output) {
        const ConfigChannelSnapshot& snapshot = *request_.snapshot;
        YAML::Node root;
        try {
            root = YAML::Load(*snapshot.content);
        } catch (const YAML::Exception&) {
            return Fail(FlowLabelingErrorV1::kInvalidConfig, "/", "snapshot content is not valid YAML");
        }
        if (!CheckMap(root, {"api_version", "kind", "metadata", "spec"}, {"api_version", "kind", "spec"}, "/")) {
            return error_;
        }
        if (!ExactScalar(root["api_version"], kSchemaId)) {
            return Fail(FlowLabelingErrorV1::kInvalidConfig, "/api_version", "unsupported api_version");
        }
        if (!ExactScalar(root["kind"], kKind)) {
            return Fail(FlowLabelingErrorV1::kInvalidConfig, "/kind", "kind must be FlowLabelingSet");
        }
        if (root["metadata"].IsDefined() && !ParseMetadata(root["metadata"])) return error_;

        const YAML::Node spec = root["spec"];
        if (!CheckMap(spec, {"engine", "labels", "rules"}, {"engine", "labels", "rules"}, "/spec")) return error_;
        if (!ParseEngine(spec["engine"], output)) return error_;
        if (!ParseLabels(spec["labels"], &output->labels)) return error_;
        if (!ParseRules(spec["rules"], output->labels, &output->rules)) return error_;

        // DPDK retains one raw copy of every unique rule after the temporary compiler vector is destroyed.
        uint64_t matcher_state_bytes = kMatcherOverheadBytes + output->labels.capacity() * sizeof(LabelRecord) +
                                       output->rules.size() * sizeof(FlowAclRule);
        for (const LabelRecord& label : output->labels) {
            matcher_state_bytes +=
                label.name.capacity() + label.display_name.capacity() + label.description.capacity() + 3;
        }
        if (output->max_runtime_bytes > request_.reserved_module_state_bytes ||
            matcher_state_bytes > request_.reserved_module_state_bytes - output->max_runtime_bytes) {
            return Fail(FlowLabelingErrorV1::kBudgetExceeded, "/spec/engine/max_runtime_bytes",
                        "ACL runtime, retained rules, and matcher-owned state exceed the reserved task budget");
        }
        SetDiagnostic(diagnostic_, FlowLabelingErrorV1::kNone, "", "");
        return FlowLabelingErrorV1::kNone;
    }

 private:
    FlowLabelingErrorV1 Fail(FlowLabelingErrorV1 error, std::string path, std::string detail) {
        error_ = error;
        SetDiagnostic(diagnostic_, error, std::move(path), std::move(detail));
        return error;
    }

    std::string ChildPath(const std::string& path, const std::string& field) const {
        return path == "/" ? path + field : path + "/" + field;
    }

    bool CheckMap(const YAML::Node& node, std::initializer_list<const char*> allowed,
                  std::initializer_list<const char*> required, const std::string& path) {
        if (!node.IsMap()) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path, "value must be a mapping");
            return false;
        }
        std::unordered_set<std::string> allowed_fields;
        for (const char* field : allowed) allowed_fields.emplace(field);
        std::unordered_set<std::string> seen;
        for (const auto& item : node) {
            if (!item.first.IsScalar()) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, path, "mapping keys must be strings");
                return false;
            }
            const std::string field = item.first.Scalar();
            if (!seen.insert(field).second) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, ChildPath(path, field), "duplicate field");
                return false;
            }
            if (allowed_fields.find(field) == allowed_fields.end()) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, ChildPath(path, field), "unknown field");
                return false;
            }
        }
        for (const char* field : required) {
            if (seen.find(field) == seen.end()) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, ChildPath(path, field), "required field is missing");
                return false;
            }
        }
        return true;
    }

    bool ExactScalar(const YAML::Node& node, const char* expected) {
        return node.IsScalar() && node.Scalar() == expected;
    }

    bool Scalar(const YAML::Node& node, const std::string& path, size_t max_bytes, std::string* output,
                bool required = true) {
        if (!node.IsDefined()) {
            if (!required) {
                output->clear();
                return true;
            }
            Fail(FlowLabelingErrorV1::kInvalidConfig, path, "required string is missing");
            return false;
        }
        if (!node.IsScalar() || node.Scalar().empty() || node.Scalar().size() > max_bytes) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path, "string is empty, not scalar, or exceeds its limit");
            return false;
        }
        *output = node.Scalar();
        return true;
    }

    bool ParseMetadata(const YAML::Node& metadata) {
        if (!CheckMap(metadata, {"name", "display_name", "description"}, {"name"}, "/metadata")) return false;
        std::string name;
        std::string ignored;
        if (!Scalar(metadata["name"], "/metadata/name", 64, &name) || !ValidIdentifier(name)) {
            if (error_ == FlowLabelingErrorV1::kNone) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, "/metadata/name", "invalid metadata name");
            }
            return false;
        }
        return Scalar(metadata["display_name"], "/metadata/display_name", 255, &ignored, false) &&
               Scalar(metadata["description"], "/metadata/description", 1024, &ignored, false);
    }

    bool ParseEngine(const YAML::Node& engine, CompileResult* output) {
        if (!CheckMap(engine, {"type", "categories", "algorithm", "numa_socket_id", "max_runtime_bytes", "limits"},
                      {"type", "categories", "algorithm", "numa_socket_id", "max_runtime_bytes", "limits"},
                      "/spec/engine")) {
            return false;
        }
        if (!ExactScalar(engine["type"], "dpdk-acl")) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, "/spec/engine/type", "engine type must be dpdk-acl");
            return false;
        }
        uint32_t categories = 0;
        if (!ParseUnsigned(engine["categories"], &categories) || categories != 1) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, "/spec/engine/categories", "exactly one category is required");
            return false;
        }
        if (!ExactScalar(engine["numa_socket_id"], "any")) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, "/spec/engine/numa_socket_id", "numa_socket_id must be any");
            return false;
        }
        if (!engine["algorithm"].IsScalar() || !ParseAlgorithm(engine["algorithm"].Scalar(), &output->algorithm)) {
            Fail(FlowLabelingErrorV1::kUnsupportedAlgorithm, "/spec/engine/algorithm",
                 "requested DPDK ACL algorithm is unsupported");
            return false;
        }
        uint64_t max_runtime = 0;
        if (!ParseUnsigned(engine["max_runtime_bytes"], &max_runtime) || max_runtime == 0 ||
            max_runtime > std::numeric_limits<size_t>::max()) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, "/spec/engine/max_runtime_bytes",
                 "max_runtime_bytes must be a positive platform-sized integer");
            return false;
        }
        output->max_runtime_bytes = static_cast<size_t>(max_runtime);
        return ParseLimits(engine["limits"]);
    }

    bool ParseAlgorithm(const std::string& value, enum rte_acl_classify_alg* output) {
        static const std::unordered_map<std::string, enum rte_acl_classify_alg> algorithms = {
            {"default", RTE_ACL_CLASSIFY_DEFAULT},
            {"scalar", RTE_ACL_CLASSIFY_SCALAR},
            {"sse", RTE_ACL_CLASSIFY_SSE},
            {"avx2", RTE_ACL_CLASSIFY_AVX2},
            {"neon", RTE_ACL_CLASSIFY_NEON},
            {"altivec", RTE_ACL_CLASSIFY_ALTIVEC},
            {"avx512x16", RTE_ACL_CLASSIFY_AVX512X16},
            {"avx512x32", RTE_ACL_CLASSIFY_AVX512X32},
        };
        const auto found = algorithms.find(value);
        if (found == algorithms.end()) return false;
        *output = found->second;
        return true;
    }

    bool ParseLimits(const YAML::Node& limits) {
        if (!CheckMap(limits,
                      {"max_labels", "max_logical_rules", "max_compiled_rules", "max_expanded_fields",
                       "reject_duplicate_label_priorities"},
                      {"max_labels", "max_logical_rules", "max_compiled_rules", "max_expanded_fields",
                       "reject_duplicate_label_priorities"},
                      "/spec/engine/limits")) {
            return false;
        }
        if (!ReadLimit(limits["max_labels"], request_.max_labels, "/spec/engine/limits/max_labels", &max_labels_) ||
            !ReadLimit(limits["max_logical_rules"], request_.max_logical_rules, "/spec/engine/limits/max_logical_rules",
                       &max_logical_rules_) ||
            !ReadLimit(limits["max_compiled_rules"], request_.max_compiled_rules,
                       "/spec/engine/limits/max_compiled_rules", &max_compiled_rules_)) {
            return false;
        }
        uint32_t fields = 0;
        if (!ParseUnsigned(limits["max_expanded_fields"], &fields) || fields < kAclFieldCount ||
            fields > RTE_ACL_MAX_FIELDS) {
            Fail(FlowLabelingErrorV1::kLimitExceeded, "/spec/engine/limits/max_expanded_fields",
                 "max_expanded_fields cannot represent the frozen tuple");
            return false;
        }
        if (!ExactScalar(limits["reject_duplicate_label_priorities"], "true")) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, "/spec/engine/limits/reject_duplicate_label_priorities",
                 "duplicate label priorities must be rejected");
            return false;
        }
        return true;
    }

    bool ReadLimit(const YAML::Node& node, uint32_t request_limit, const std::string& path, uint32_t* output) {
        if (request_limit == 0 || !ParseUnsigned(node, output) || *output == 0 || *output > request_limit) {
            Fail(FlowLabelingErrorV1::kLimitExceeded, path,
                 "configured limit is zero or exceeds the compile request limit");
            return false;
        }
        return true;
    }

    bool ParseLabels(const YAML::Node& labels, std::vector<LabelRecord>* output) {
        if (!labels.IsSequence()) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, "/spec/labels", "labels must be a sequence");
            return false;
        }
        if (labels.size() > max_labels_) {
            Fail(FlowLabelingErrorV1::kLimitExceeded, "/spec/labels", "label count exceeds max_labels");
            return false;
        }
        std::unordered_set<uint32_t> ids;
        std::unordered_set<int32_t> priorities;
        std::unordered_set<std::string> names;
        output->reserve(labels.size());
        for (size_t index = 0; index < labels.size(); ++index) {
            const std::string path = "/spec/labels/" + std::to_string(index);
            const YAML::Node node = labels[index];
            if (!CheckMap(node, {"id", "priority", "name", "display_name", "description"}, {"id", "priority", "name"},
                          path)) {
                return false;
            }
            LabelRecord label;
            if (!ParseUnsigned(node["id"], &label.id) || label.id == 0) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, path + "/id", "label id must be a nonzero uint32");
                return false;
            }
            if (!ParseSigned(node["priority"], &label.priority) || label.priority < RTE_ACL_MIN_PRIORITY ||
                label.priority > RTE_ACL_MAX_PRIORITY) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, path + "/priority",
                     "label priority is outside the DPDK ACL range");
                return false;
            }
            if (!Scalar(node["name"], path + "/name", 64, &label.name) || !ValidIdentifier(label.name)) {
                if (error_ == FlowLabelingErrorV1::kNone) {
                    Fail(FlowLabelingErrorV1::kInvalidConfig, path + "/name", "invalid label name");
                }
                return false;
            }
            if (!Scalar(node["display_name"], path + "/display_name", 255, &label.display_name, false) ||
                !Scalar(node["description"], path + "/description", 1024, &label.description, false)) {
                return false;
            }
            if (!ids.insert(label.id).second || !priorities.insert(label.priority).second ||
                !names.insert(label.name).second) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, path, "label id, name, and priority must each be unique");
                return false;
            }
            output->push_back(std::move(label));
        }
        std::sort(output->begin(), output->end(),
                  [](const LabelRecord& left, const LabelRecord& right) { return left.id < right.id; });
        return true;
    }

    bool ParseRules(const YAML::Node& rules, const std::vector<LabelRecord>& labels, std::vector<FlowAclRule>* output) {
        if (!rules.IsSequence()) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, "/spec/rules", "rules must be a sequence");
            return false;
        }
        if (rules.size() > max_logical_rules_) {
            Fail(FlowLabelingErrorV1::kLimitExceeded, "/spec/rules", "logical rule count exceeds max_logical_rules");
            return false;
        }
        std::unordered_map<uint32_t, int32_t> priorities;
        for (const LabelRecord& label : labels) priorities.emplace(label.id, label.priority);
        std::unordered_set<std::string> rule_ids;
        output->reserve(rules.size() * 2);

        for (size_t index = 0; index < rules.size(); ++index) {
            const std::string path = "/spec/rules/" + std::to_string(index);
            const YAML::Node node = rules[index];
            if (!CheckMap(node, {"id", "label_id", "direction", "match_all", "matches"},
                          {"id", "label_id", "direction"}, path)) {
                return false;
            }
            std::string id;
            if (!Scalar(node["id"], path + "/id", 64, &id) || !ValidIdentifier(id) || !rule_ids.insert(id).second) {
                if (error_ == FlowLabelingErrorV1::kNone) {
                    Fail(FlowLabelingErrorV1::kInvalidConfig, path + "/id", "rule id is invalid or duplicated");
                }
                return false;
            }
            uint32_t label_id = 0;
            if (!ParseUnsigned(node["label_id"], &label_id) || priorities.find(label_id) == priorities.end()) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, path + "/label_id", "rule references an unknown label");
                return false;
            }
            if (!ExactScalar(node["direction"], "bidirectional")) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, path + "/direction", "direction must be bidirectional");
                return false;
            }
            const bool have_match_all = node["match_all"].IsDefined();
            const bool have_matches = node["matches"].IsDefined();
            if (have_match_all == have_matches || (have_match_all && !ExactScalar(node["match_all"], "true")) ||
                (have_matches && (!node["matches"].IsSequence() || node["matches"].size() == 0))) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, path,
                     "rule requires exactly one of match_all: true or nonempty matches");
                return false;
            }
            FlowAclRule forward = WildcardRule(label_id, priorities.at(label_id));
            FlowAclRule reverse = WildcardRule(label_id, priorities.at(label_id));
            if (have_matches && (!ApplyMatches(node["matches"], false, path + "/matches", &forward) ||
                                 !ApplyMatches(node["matches"], true, path + "/matches", &reverse))) {
                return false;
            }
            output->push_back(forward);
            output->push_back(reverse);
        }
        std::sort(output->begin(), output->end(), FlowAclRuleLess);
        output->erase(std::unique(output->begin(), output->end(), FlowAclRuleEqual), output->end());
        if (output->size() > max_compiled_rules_) {
            Fail(FlowLabelingErrorV1::kLimitExceeded, "/spec/rules",
                 "bidirectional expansion exceeds max_compiled_rules");
            return false;
        }
        return true;
    }

    bool ApplyMatches(const YAML::Node& matches, bool reverse, const std::string& path, FlowAclRule* rule) {
        std::unordered_set<std::string> logical_fields;
        int required_family = 0;
        for (size_t index = 0; index < matches.size(); ++index) {
            const std::string match_path = path + "/" + std::to_string(index);
            const YAML::Node match = matches[index];
            if (!match.IsMap() || !match["field"].IsScalar()) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, match_path, "match must be a mapping with field");
                return false;
            }
            const std::string name = match["field"].Scalar();
            if (!logical_fields.insert(name).second) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, match_path + "/field",
                     "a logical rule cannot repeat a field");
                return false;
            }
            LogicalField field;
            if (!ParseLogicalField(name, &field)) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, match_path + "/field", "unknown match field");
                return false;
            }
            if (reverse) field = SwapEndpoints(field);
            if (!ApplyMatch(match, field, match_path, &required_family, rule)) return false;
        }
        return true;
    }

    bool ApplyMatch(const YAML::Node& match, LogicalField field, const std::string& path, int* required_family,
                    FlowAclRule* rule) {
        if (IsRange(field)) {
            if (!CheckMap(match, {"field", "min", "max"}, {"field", "min", "max"}, path)) return false;
            return ApplyRange(match, field, path, required_family, rule);
        }
        if (IsPrefix(field)) {
            if (!CheckMap(match, {"field", "value", "prefix_bits"}, {"field", "value", "prefix_bits"}, path)) {
                return false;
            }
            return ApplyPrefix(match, field, path, required_family, rule);
        }
        if (!CheckMap(match, {"field", "value", "mask"}, {"field", "value", "mask"}, path)) return false;
        return ApplyBitmask(match, field, path, required_family, rule);
    }

    bool Family(int family, const std::string& path, int* required_family, FlowAclRule* rule) {
        if (*required_family != 0 && *required_family != family) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path, "rule mixes incompatible IP families");
            return false;
        }
        const uint8_t explicit_mask = rule->field[kIpFamily].mask_range.u8;
        const uint8_t explicit_value = rule->field[kIpFamily].value.u8;
        if (explicit_mask != 0 && (static_cast<uint8_t>(family) & explicit_mask) != (explicit_value & explicit_mask)) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path, "ip_family contradicts the address field");
            return false;
        }
        *required_family = family;
        SetBitmask(rule, kIpFamily, family, 0xff);
        return true;
    }

    bool ApplyRange(const YAML::Node& match, LogicalField field, const std::string& path, int* required_family,
                    FlowAclRule* rule) {
        if (field == LogicalField::kSourceIpv4Range || field == LogicalField::kDestinationIpv4Range) {
            std::array<uint8_t, 4> minimum{};
            std::array<uint8_t, 4> maximum{};
            if (!ParseAddress(match["min"], AF_INET, minimum.data()) ||
                !ParseAddress(match["max"], AF_INET, maximum.data()) ||
                Host32(minimum.data()) > Host32(maximum.data()) || !Family(4, path, required_family, rule)) {
                if (error_ == FlowLabelingErrorV1::kNone) {
                    Fail(FlowLabelingErrorV1::kInvalidConfig, path, "invalid IPv4 range");
                }
                return false;
            }
            const bool source = field == LogicalField::kSourceIpv4Range;
            SetRange(rule, source ? kSourceIpv4Range : kDestinationIpv4Range, Host32(minimum.data()),
                     Host32(maximum.data()));
            RequirePresent(rule, source ? kSourceIpPresent : kDestinationIpPresent);
            return true;
        }
        uint64_t minimum = 0;
        uint64_t maximum = 0;
        if (!ParseUnsigned(match["min"], &minimum) || !ParseUnsigned(match["max"], &maximum) || minimum > maximum) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path, "invalid numeric range");
            return false;
        }
        size_t physical = kObservationDomainRange;
        uint64_t limit = std::numeric_limits<uint64_t>::max();
        uint32_t presence = 0;
        switch (field) {
            case LogicalField::kObservationDomain:
                break;
            case LogicalField::kOuterVlanVid:
                physical = kOuterVlanVidRange;
                limit = 4095;
                presence = kOuterVlanPresent;
                break;
            case LogicalField::kInnerVlanVid:
                physical = kInnerVlanVidRange;
                limit = 4095;
                presence = kInnerVlanPresent;
                break;
            case LogicalField::kTransportProtocolRange:
                physical = kProtocolRange;
                limit = 255;
                presence = kTransportPresent;
                break;
            case LogicalField::kSourcePort:
                physical = kSourcePortRange;
                limit = 65535;
                presence = kSourcePortPresent;
                break;
            case LogicalField::kDestinationPort:
                physical = kDestinationPortRange;
                limit = 65535;
                presence = kDestinationPortPresent;
                break;
            default:
                return false;
        }
        if (maximum > limit) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path, "range exceeds field width");
            return false;
        }
        SetRange(rule, physical, minimum, maximum);
        if (presence != 0) RequirePresent(rule, presence);
        return true;
    }

    bool ApplyPrefix(const YAML::Node& match, LogicalField field, const std::string& path, int* required_family,
                     FlowAclRule* rule) {
        uint32_t prefix = 0;
        if (!ParseUnsigned(match["prefix_bits"], &prefix)) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path + "/prefix_bits", "invalid prefix length");
            return false;
        }
        if (field == LogicalField::kSourceMacPrefix || field == LogicalField::kDestinationMacPrefix) {
            std::array<uint8_t, 6> address{};
            if (prefix > 48 || !ParseMac(match["value"], &address)) {
                Fail(FlowLabelingErrorV1::kInvalidConfig, path, "invalid MAC prefix");
                return false;
            }
            const bool source = field == LogicalField::kSourceMacPrefix;
            SetMask(rule, source ? kSourceMacHead : kDestinationMacHead, Host16(address.data()), std::min(prefix, 16U));
            SetMask(rule, source ? kSourceMacTail : kDestinationMacTail, Host32(address.data() + 2),
                    prefix > 16 ? prefix - 16 : 0);
            RequirePresent(rule, source ? kSourceMacPresent : kDestinationMacPresent);
            return true;
        }
        if (field == LogicalField::kSourceIpv4Prefix || field == LogicalField::kDestinationIpv4Prefix) {
            std::array<uint8_t, 4> address{};
            if (prefix > 32 || !ParseAddress(match["value"], AF_INET, address.data()) ||
                !Family(4, path, required_family, rule)) {
                if (error_ == FlowLabelingErrorV1::kNone) {
                    Fail(FlowLabelingErrorV1::kInvalidConfig, path, "invalid IPv4 prefix");
                }
                return false;
            }
            const bool source = field == LogicalField::kSourceIpv4Prefix;
            SetMask(rule, source ? kSourceIpPrefix0 : kDestinationIpPrefix0, Host32(address.data()), prefix);
            RequirePresent(rule, source ? kSourceIpPresent : kDestinationIpPresent);
            return true;
        }
        if (field == LogicalField::kSourceIpv6Prefix || field == LogicalField::kDestinationIpv6Prefix) {
            std::array<uint8_t, 16> address{};
            if (prefix > 128 || !ParseAddress(match["value"], AF_INET6, address.data()) ||
                !Family(6, path, required_family, rule)) {
                if (error_ == FlowLabelingErrorV1::kNone) {
                    Fail(FlowLabelingErrorV1::kInvalidConfig, path, "invalid IPv6 prefix");
                }
                return false;
            }
            const bool source = field == LogicalField::kSourceIpv6Prefix;
            const size_t first = source ? kSourceIpPrefix0 : kDestinationIpPrefix0;
            for (size_t part = 0; part < 4; ++part) {
                const uint32_t part_prefix =
                    prefix > part * 32 ? std::min<uint32_t>(32, prefix - static_cast<uint32_t>(part * 32)) : 0;
                SetMask(rule, first + part, Host32(address.data() + part * 4), part_prefix);
            }
            RequirePresent(rule, source ? kSourceIpPresent : kDestinationIpPresent);
            return true;
        }
        uint64_t value = 0;
        if (!ParseUnsigned(match["value"], &value)) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path + "/value", "invalid prefix value");
            return false;
        }
        size_t physical = kObservationDomainBitmask;
        uint32_t width = 64;
        uint32_t presence = 0;
        if (field == LogicalField::kOuterVlanTpidPrefix || field == LogicalField::kInnerVlanTpidPrefix) {
            physical = field == LogicalField::kOuterVlanTpidPrefix ? kOuterVlanTpidMask : kInnerVlanTpidMask;
            width = 16;
            presence = field == LogicalField::kOuterVlanTpidPrefix ? kOuterVlanPresent : kInnerVlanPresent;
        } else if (field == LogicalField::kTransportProtocolPrefix) {
            physical = kProtocolMask;
            width = 8;
            presence = kTransportPresent;
        }
        if (prefix > width || value > MaxForSize(kFieldDefinitions[physical].size)) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path, "prefix exceeds field width");
            return false;
        }
        if (field == LogicalField::kObservationDomainPrefix) {
            MergeBitmask(rule, physical, value, PrefixMask(width, prefix));
        } else {
            SetMask(rule, physical, value, prefix);
        }
        if (presence != 0) RequirePresent(rule, presence);
        return true;
    }

    bool ApplyBitmask(const YAML::Node& match, LogicalField field, const std::string& path, int* required_family,
                      FlowAclRule* rule) {
        if (field == LogicalField::kSourceIpv4Bitmask || field == LogicalField::kDestinationIpv4Bitmask) {
            std::array<uint8_t, 4> value{};
            std::array<uint8_t, 4> mask{};
            if (!ParseAddress(match["value"], AF_INET, value.data()) ||
                !ParseAddress(match["mask"], AF_INET, mask.data()) || !Family(4, path, required_family, rule)) {
                if (error_ == FlowLabelingErrorV1::kNone) {
                    Fail(FlowLabelingErrorV1::kInvalidConfig, path, "invalid IPv4 bitmask");
                }
                return false;
            }
            const bool source = field == LogicalField::kSourceIpv4Bitmask;
            SetBitmask(rule, source ? kSourceIpv4Bitmask : kDestinationIpv4Bitmask, Host32(value.data()),
                       Host32(mask.data()));
            RequirePresent(rule, source ? kSourceIpPresent : kDestinationIpPresent);
            return true;
        }
        uint64_t value = 0;
        uint64_t mask = 0;
        if (!ParseUnsigned(match["value"], &value) || !ParseUnsigned(match["mask"], &mask)) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path, "invalid numeric bitmask");
            return false;
        }
        size_t physical = kObservationDomainBitmask;
        uint64_t limit = std::numeric_limits<uint64_t>::max();
        uint32_t presence = 0;
        switch (field) {
            case LogicalField::kObservationDomainBitmask:
                break;
            case LogicalField::kIpFamily:
                physical = kIpFamily;
                limit = 255;
                if (*required_family != 0 && (static_cast<uint64_t>(*required_family) & mask) != (value & mask)) {
                    Fail(FlowLabelingErrorV1::kInvalidConfig, path, "ip_family contradicts the address field");
                    return false;
                }
                break;
            case LogicalField::kOuterVlanTpid:
                physical = kOuterVlanTpidBitmask;
                limit = 65535;
                presence = kOuterVlanPresent;
                break;
            case LogicalField::kInnerVlanTpid:
                physical = kInnerVlanTpidBitmask;
                limit = 65535;
                presence = kInnerVlanPresent;
                break;
            case LogicalField::kTransportProtocol:
                physical = kProtocolBitmask;
                limit = 255;
                presence = kTransportPresent;
                break;
            default:
                return false;
        }
        if (value > limit || mask > limit) {
            Fail(FlowLabelingErrorV1::kInvalidConfig, path, "bitmask exceeds field width");
            return false;
        }
        if (field == LogicalField::kObservationDomainBitmask) {
            MergeBitmask(rule, physical, value, mask);
        } else {
            SetBitmask(rule, physical, value, mask);
        }
        if (presence != 0) RequirePresent(rule, presence);
        return true;
    }

    const FlowLabelingCompileRequestV1& request_;
    FlowLabelingDiagnosticV1* diagnostic_;
    FlowLabelingErrorV1 error_ = FlowLabelingErrorV1::kNone;
    uint32_t max_labels_ = 0;
    uint32_t max_logical_rules_ = 0;
    uint32_t max_compiled_rules_ = 0;
};

void CopyEndpoint(const FlowLabelEndpointFactsV1& source, bool source_side, AclTuple* tuple) {
    const uint32_t mac_present = source_side ? kSourceMacPresent : kDestinationMacPresent;
    const uint32_t ip_present = source_side ? kSourceIpPresent : kDestinationIpPresent;
    const uint32_t port_present = source_side ? kSourcePortPresent : kDestinationPortPresent;
    uint16_t* mac_head = source_side ? &tuple->source_mac_head : &tuple->destination_mac_head;
    uint32_t* mac_tail = source_side ? &tuple->source_mac_tail : &tuple->destination_mac_tail;
    uint8_t* ip_prefix = source_side ? tuple->source_ip_prefix : tuple->destination_ip_prefix;
    uint32_t* ipv4_range = source_side ? &tuple->source_ipv4_range : &tuple->destination_ipv4_range;
    uint32_t* ipv4_bitmask = source_side ? &tuple->source_ipv4_bitmask : &tuple->destination_ipv4_bitmask;
    uint16_t* port = source_side ? &tuple->source_port_range : &tuple->destination_port_range;

    if (source.mac_valid) {
        tuple->presence_bits |= rte_cpu_to_be_32(mac_present);
        const uint16_t head = static_cast<uint16_t>((uint16_t{source.mac[0]} << 8) | source.mac[1]);
        const uint32_t tail = (uint32_t{source.mac[2]} << 24) | (uint32_t{source.mac[3]} << 16) |
                              (uint32_t{source.mac[4]} << 8) | source.mac[5];
        *mac_head = rte_cpu_to_be_16(head);
        *mac_tail = rte_cpu_to_be_32(tail);
    }
    if (source.ip_valid) {
        tuple->presence_bits |= rte_cpu_to_be_32(ip_present);
        std::memcpy(ip_prefix, source.ip, 16);
        std::memcpy(ipv4_range, source.ip, sizeof(*ipv4_range));
        std::memcpy(ipv4_bitmask, source.ip, sizeof(*ipv4_bitmask));
    }
    if (source.port_valid) {
        tuple->presence_bits |= rte_cpu_to_be_32(port_present);
        *port = rte_cpu_to_be_16(source.port);
    }
}

AclTuple EncodeTuple(const FlowLabelFactsV1& facts) {
    AclTuple tuple{};
    tuple.ip_family = facts.ip_family;
    tuple.observation_domain_range = rte_cpu_to_be_64(facts.observation_domain_id);
    tuple.observation_domain_bitmask = tuple.observation_domain_range;
    if (facts.transport_valid) {
        tuple.presence_bits |= rte_cpu_to_be_32(kTransportPresent);
        tuple.protocol_bitmask = facts.transport_protocol;
        tuple.protocol_mask = facts.transport_protocol;
        tuple.protocol_range = facts.transport_protocol;
    }
    for (size_t index = 0; index < 2; ++index) {
        const FlowLabelVlanFactsV1& vlan = facts.vlan[index];
        const uint32_t present = index == 0 ? kOuterVlanPresent : kInnerVlanPresent;
        uint16_t* tpid_bitmask = index == 0 ? &tuple.outer_vlan_tpid_bitmask : &tuple.inner_vlan_tpid_bitmask;
        uint16_t* tpid_mask = index == 0 ? &tuple.outer_vlan_tpid_mask : &tuple.inner_vlan_tpid_mask;
        uint16_t* vid = index == 0 ? &tuple.outer_vlan_vid_range : &tuple.inner_vlan_vid_range;
        if (vlan.valid) {
            tuple.presence_bits |= rte_cpu_to_be_32(present);
            *tpid_bitmask = rte_cpu_to_be_16(vlan.tpid);
            *tpid_mask = *tpid_bitmask;
            *vid = rte_cpu_to_be_16(vlan.vid);
        }
    }
    CopyEndpoint(facts.source, true, &tuple);
    CopyEndpoint(facts.destination, false, &tuple);
    return tuple;
}

int CurrentCpu() {
    const int cpu = sched_getcpu();
    return cpu < 0 ? 0 : cpu;
}

int InitializeEal(const FlowLabelingStartupOptions& options) {
    std::vector<std::string> arguments = BuildFlowLabelingEalArguments(options, CurrentCpu());
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) argv.push_back(argument.data());
    return rte_eal_init(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

class FlowLabelingPlugin::Matcher final : public IFlowLabelMatcherV1 {
 public:
    Matcher(FlowLabelingPlugin* owner, rte_acl_ctx* context, enum rte_acl_classify_alg algorithm, bool have_rules,
            std::vector<LabelRecord> labels)
        : owner_(owner),
          context_(context),
          algorithm_(algorithm),
          have_rules_(have_rules),
          labels_(std::move(labels)) {}

    int ClassifyBatch(const FlowLabelFactsV1* facts, uint32_t count, uint32_t* primary_label_ids) const override {
        if (facts == nullptr || count == 0 || primary_label_ids == nullptr) return -1;
        for (uint32_t index = 0; index < count; ++index) {
            if (facts[index].struct_size != kFlowLabelFactsV1Size) return -1;
        }
        if (!have_rules_) {
            std::fill(primary_label_ids, primary_label_ids + count, 0);
            return 0;
        }
        std::array<AclTuple, kClassificationBatch> tuples;
        std::array<const uint8_t*, kClassificationBatch> inputs{};
        for (uint32_t offset = 0; offset < count; offset += kClassificationBatch) {
            const uint32_t batch = std::min(kClassificationBatch, count - offset);
            for (uint32_t index = 0; index < batch; ++index) {
                tuples[index] = EncodeTuple(facts[offset + index]);
                inputs[index] = reinterpret_cast<const uint8_t*>(&tuples[index]);
            }
            const int status =
                rte_acl_classify_alg(context_, inputs.data(), primary_label_ids + offset, batch, 1, algorithm_);
            if (status != 0) return status;
        }
        return 0;
    }

    bool FindLabel(uint32_t label_id, FlowPrimaryLabelViewV1* output) const override {
        if (label_id == 0 || output == nullptr) return false;
        const auto found = std::lower_bound(labels_.begin(), labels_.end(), label_id,
                                            [](const LabelRecord& label, uint32_t id) { return label.id < id; });
        if (found == labels_.end() || found->id != label_id) return false;
        FlowPrimaryLabelViewV1 view;
        view.label_id = found->id;
        view.priority = found->priority;
        view.name = found->name.c_str();
        view.display_name = found->display_name.c_str();
        view.description = found->description.c_str();
        *output = view;
        return true;
    }

    void Release() noexcept override {
        rte_acl_free(context_);
        context_ = nullptr;
        owner_->ReleaseMatcher();
        delete this;
    }

 private:
    FlowLabelingPlugin* owner_ = nullptr;
    rte_acl_ctx* context_ = nullptr;
    enum rte_acl_classify_alg algorithm_ = RTE_ACL_CLASSIFY_DEFAULT;
    bool have_rules_ = false;
    std::vector<LabelRecord> labels_;
};

int FlowLabelingPlugin::Option(const char* arg) {
    FlowLabelingStartupOptions parsed;
    if (ParseFlowLabelingStartupOptions(arg, &parsed) != 0) return -1;

    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_ || ready_ || eal_initialized_) return -1;
    startup_options_ = std::move(parsed);
    return 0;
}

int FlowLabelingPlugin::Load(IQuerier*) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) return -1;
    loaded_ = true;
    return 0;
}

int FlowLabelingPlugin::Unload() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_ || ready_ || eal_initialized_ || active_matchers_ != 0) return -1;
    loaded_ = false;
    return 0;
}

int FlowLabelingPlugin::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) return -1;
    if (ready_) return 0;
    if (eal_initialized_) return -1;
    if (InitializeEal(startup_options_) < 0) return -1;
    eal_initialized_ = true;
    ready_ = true;
    return 0;
}

int FlowLabelingPlugin::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) return -1;
    if (!ready_ && !eal_initialized_) return 0;
    if (active_matchers_ != 0) return -1;

    ready_ = false;
    if (eal_initialized_ && rte_eal_cleanup() != 0) return -1;
    eal_initialized_ = false;
    return 0;
}

FlowLabelingErrorV1 FlowLabelingPlugin::RuntimeStatus(FlowLabelingDiagnosticV1* diagnostic) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
        SetDiagnostic(diagnostic, FlowLabelingErrorV1::kUnavailable, "/runtime",
                      "flow-labeling DPDK runtime is not started");
        return FlowLabelingErrorV1::kUnavailable;
    }
    SetDiagnostic(diagnostic, FlowLabelingErrorV1::kNone, "", "");
    return FlowLabelingErrorV1::kNone;
}

FlowLabelingErrorV1 FlowLabelingPlugin::CreateMatcher(const FlowLabelingCompileRequestV1& request,
                                                      IFlowLabelMatcherV1** output,
                                                      FlowLabelingDiagnosticV1* diagnostic) {
    if (output == nullptr) {
        SetDiagnostic(diagnostic, FlowLabelingErrorV1::kInvalidSnapshot, "/output", "matcher output is null");
        return FlowLabelingErrorV1::kInvalidSnapshot;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
        SetDiagnostic(diagnostic, FlowLabelingErrorV1::kUnavailable, "/runtime",
                      "flow-labeling DPDK runtime is not started");
        return FlowLabelingErrorV1::kUnavailable;
    }
    if (request.struct_size != kFlowLabelingCompileRequestV1Size || request.snapshot == nullptr ||
        request.max_labels == 0 || request.max_logical_rules == 0 || request.max_compiled_rules == 0 ||
        request.reserved_module_state_bytes == 0) {
        SetDiagnostic(diagnostic, FlowLabelingErrorV1::kInvalidSnapshot, "/snapshot",
                      "compile request or snapshot is invalid");
        return FlowLabelingErrorV1::kInvalidSnapshot;
    }

    const ConfigChannelSnapshot& snapshot = *request.snapshot;
    if (snapshot.schema_id != kSchemaId || snapshot.format != "yaml" || snapshot.content == nullptr ||
        snapshot.content_bytes == 0 || snapshot.content_bytes > kMaxSnapshotBytes ||
        snapshot.content_bytes != snapshot.content->size()) {
        SetDiagnostic(diagnostic, FlowLabelingErrorV1::kInvalidSnapshot, "/snapshot",
                      "snapshot metadata does not identify an owned flow-labeling YAML document");
        return FlowLabelingErrorV1::kInvalidSnapshot;
    }

    CompileResult compiled;
    try {
        ConfigCompiler compiler(request, diagnostic);
        const FlowLabelingErrorV1 status = compiler.Compile(&compiled);
        if (status != FlowLabelingErrorV1::kNone) return status;
    } catch (const std::bad_alloc&) {
        SetDiagnostic(diagnostic, FlowLabelingErrorV1::kAllocationFailed, "/compile",
                      "configuration compilation allocation failed");
        return FlowLabelingErrorV1::kAllocationFailed;
    }

    char context_name[RTE_ACL_NAMESIZE]{};
    const uint64_t sequence = g_context_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    std::snprintf(context_name, sizeof(context_name), "flowsql-label-%llu", static_cast<unsigned long long>(sequence));
    const rte_acl_param parameters = {
        context_name,
        SOCKET_ID_ANY,
        static_cast<uint32_t>(sizeof(FlowAclRule)),
        std::max<uint32_t>(1, compiled.rules.size()),
    };
    rte_acl_ctx* context = rte_acl_create(&parameters);
    if (context == nullptr) {
        SetDiagnostic(diagnostic, FlowLabelingErrorV1::kDpdkFailure, "/runtime/acl",
                      "DPDK failed to create the task-private ACL context");
        return FlowLabelingErrorV1::kDpdkFailure;
    }

    if (!compiled.rules.empty()) {
        const int add_status = rte_acl_add_rules(context, reinterpret_cast<const rte_acl_rule*>(compiled.rules.data()),
                                                 compiled.rules.size());
        if (add_status != 0) {
            rte_acl_free(context);
            SetDiagnostic(diagnostic, FlowLabelingErrorV1::kDpdkFailure, "/spec/rules",
                          "DPDK rejected compiled ACL rules");
            return FlowLabelingErrorV1::kDpdkFailure;
        }
        rte_acl_config configuration{};
        configuration.num_categories = 1;
        configuration.num_fields = kAclFieldCount;
        configuration.max_size = compiled.max_runtime_bytes;
        std::copy(kFieldDefinitions.begin(), kFieldDefinitions.end(), configuration.defs);
        const int build_status = rte_acl_build(context, &configuration);
        if (build_status != 0) {
            rte_acl_free(context);
            const FlowLabelingErrorV1 error =
                build_status == -ENOMEM ? FlowLabelingErrorV1::kBudgetExceeded : FlowLabelingErrorV1::kDpdkFailure;
            SetDiagnostic(diagnostic, error, "/spec/engine/max_runtime_bytes", "DPDK failed to build the ACL runtime");
            return error;
        }
    }
    if (compiled.algorithm != RTE_ACL_CLASSIFY_DEFAULT) {
        const int algorithm_status = rte_acl_set_ctx_classify(context, compiled.algorithm);
        if (algorithm_status != 0) {
            rte_acl_free(context);
            SetDiagnostic(diagnostic, FlowLabelingErrorV1::kUnsupportedAlgorithm, "/spec/engine/algorithm",
                          "requested DPDK ACL algorithm is unavailable on this platform");
            return FlowLabelingErrorV1::kUnsupportedAlgorithm;
        }
    }

    Matcher* matcher = new (std::nothrow)
        Matcher(this, context, compiled.algorithm, !compiled.rules.empty(), std::move(compiled.labels));
    if (matcher == nullptr) {
        rte_acl_free(context);
        SetDiagnostic(diagnostic, FlowLabelingErrorV1::kAllocationFailed, "/runtime/matcher",
                      "matcher allocation failed");
        return FlowLabelingErrorV1::kAllocationFailed;
    }

    ++active_matchers_;
    *output = matcher;
    SetDiagnostic(diagnostic, FlowLabelingErrorV1::kNone, "", "");
    return FlowLabelingErrorV1::kNone;
}

void FlowLabelingPlugin::ReleaseMatcher() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_matchers_ > 0) --active_matchers_;
}

}  // namespace flowsql
