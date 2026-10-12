// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_family_result_contract.h"
#include <arrow/api.h>
#include <stdexcept>

namespace flowsql::npm {
namespace {
std::shared_ptr<arrow::Field> AddressField(const char* name, packet::AddressFamily family, bool nullable) {
    const bool ipv4 = family == packet::AddressFamily::kIPv4;
    return arrow::field(name, ipv4 ? arrow::uint32() : arrow::fixed_size_binary(16), nullable,
                        arrow::key_value_metadata(
                            {"flowsql.semantic", "flowsql.address_encoding"},
                            {ipv4 ? "ipv4_address" : "ipv6_address", ipv4 ? "unsigned_numeric" : "network_octets"}));
}
}  // namespace

NpmEntityDescriptorV1 NpmFamilyEntityDescriptor(NpmFamilyEntityKind kind, packet::AddressFamily family) {
    if (family != packet::AddressFamily::kIPv4 && family != packet::AddressFamily::kIPv6)
        throw std::invalid_argument("NPM entity requires IPv4 or IPv6");
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::string module, base;
    switch (kind) {
        case NpmFamilyEntityKind::kBasic:
            module = "basic";
            base = "basic";
            fields = {
                arrow::field("session_id", arrow::uint64(), false),
                arrow::field("observation_domain_id", arrow::uint64(), false),
                arrow::field("revision", arrow::uint64(), false),
                arrow::field("observed_at", arrow::int64(), false),
                arrow::field("is_final", arrow::boolean(), false),
                arrow::field("transport_protocol", arrow::uint8(), false),
                AddressField("a_ip", family, false),
                AddressField("b_ip", family, false),
                arrow::field("a_port", arrow::uint16(), false),
                arrow::field("b_port", arrow::uint16(), false),
                arrow::field("first_ns", arrow::int64(), false),
                arrow::field("last_ns", arrow::int64(), false),
                arrow::field("packets_ab", arrow::uint64(), false),
                arrow::field("packets_ba", arrow::uint64(), false),
                arrow::field("wire_bytes_ab", arrow::uint64(), false),
                arrow::field("wire_bytes_ba", arrow::uint64(), false),
                arrow::field("primary_label_id", arrow::uint32(), true),
                arrow::field("protocol_status", arrow::utf8(), false),
                arrow::field("protocol_id", arrow::uint16(), true),
                arrow::field("protocol_sub_id", arrow::uint16(), true),
                arrow::field("protocol", arrow::utf8(), true),
                arrow::field("end_reason", arrow::utf8(), true),
                arrow::field("wire_bytes_total", arrow::uint64(), false),
                arrow::field("period_start_ns", arrow::int64(), true),
                arrow::field("period_end_ns", arrow::int64(), true),
                arrow::field("period_complete", arrow::boolean(), true),
                arrow::field("interval_packets_ab", arrow::uint64(), true),
                arrow::field("interval_packets_ba", arrow::uint64(), true),
                arrow::field("interval_wire_bytes_ab", arrow::uint64(), true),
                arrow::field("interval_wire_bytes_ba", arrow::uint64(), true),
                arrow::field("interval_wire_bytes_total", arrow::uint64(), true),
            };
            break;
        case NpmFamilyEntityKind::kSession:
            module = "session";
            base = "session";
            fields = {
                arrow::field("session_id", arrow::uint64(), false),
                arrow::field("observation_domain_id", arrow::uint64(), false),
                arrow::field("revision", arrow::uint64(), false),
                arrow::field("observed_at", arrow::int64(), false),
                arrow::field("is_final", arrow::boolean(), false),
                arrow::field("transport_protocol", arrow::uint8(), false),
                AddressField("a_ip", family, false),
                AddressField("b_ip", family, false),
                arrow::field("a_port", arrow::uint16(), false),
                arrow::field("b_port", arrow::uint16(), false),
                arrow::field("first_ns", arrow::int64(), false),
                arrow::field("last_ns", arrow::int64(), false),
                arrow::field("duration_ns", arrow::int64(), false),
                arrow::field("primary_label_id", arrow::uint32(), true),
                arrow::field("protocol_status", arrow::utf8(), false),
                arrow::field("protocol_id", arrow::uint16(), true),
                arrow::field("protocol_sub_id", arrow::uint16(), true),
                arrow::field("protocol", arrow::utf8(), true),
                arrow::field("end_reason", arrow::utf8(), true),
                arrow::field("packets_ab", arrow::uint64(), false),
                arrow::field("packets_ba", arrow::uint64(), false),
                arrow::field("wire_bytes_ab", arrow::uint64(), false),
                arrow::field("wire_bytes_ba", arrow::uint64(), false),
                arrow::field("payload_bytes_ab", arrow::uint64(), false),
                arrow::field("payload_bytes_ba", arrow::uint64(), false),
                arrow::field("rate_status", arrow::utf8(), false),
                arrow::field("wire_bps_ab", arrow::float64(), true),
                arrow::field("wire_bps_ba", arrow::float64(), true),
                arrow::field("payload_bps_ab", arrow::float64(), true),
                arrow::field("payload_bps_ba", arrow::float64(), true),
                arrow::field("tcp_unique_payload_bytes_ab", arrow::uint64(), true),
                arrow::field("tcp_unique_payload_bytes_ba", arrow::uint64(), true),
                arrow::field("tcp_unique_payload_bps_ab", arrow::float64(), true),
                arrow::field("tcp_unique_payload_bps_ba", arrow::float64(), true),
                arrow::field("tcp_handshake_status", arrow::utf8(), false),
                arrow::field("tcp_initiator", arrow::utf8(), true),
                arrow::field("tcp_handshake_duration_ns", arrow::int64(), true),
                arrow::field("tcp_synack_rtt_ns", arrow::int64(), true),
                arrow::field("tcp_rtt_status", arrow::utf8(), false),
                arrow::field("tcp_rtt_samples", arrow::uint64(), true),
                arrow::field("tcp_rtt_min_ns", arrow::int64(), true),
                arrow::field("tcp_rtt_mean_ns", arrow::int64(), true),
                arrow::field("tcp_rtt_max_ns", arrow::int64(), true),
                arrow::field("tcp_retransmission_status", arrow::utf8(), false),
                arrow::field("tcp_retrans_packets_ab", arrow::uint64(), true),
                arrow::field("tcp_retrans_packets_ba", arrow::uint64(), true),
                arrow::field("tcp_retrans_payload_bytes_ab", arrow::uint64(), true),
                arrow::field("tcp_retrans_payload_bytes_ba", arrow::uint64(), true),
                arrow::field("measurement_flags", arrow::uint32(), false),
            };
            break;
        case NpmFamilyEntityKind::kDns:
            module = "dns";
            base = "dns_transaction";
            fields = {
                arrow::field("entity_instance_id", arrow::uint64(), false),
                arrow::field("revision", arrow::uint64(), false),
                arrow::field("observed_at", arrow::int64(), false),
                arrow::field("is_final", arrow::boolean(), false),
                arrow::field("session_id", arrow::uint64(), false),
                arrow::field("observation_domain_id", arrow::uint64(), false),
                arrow::field("transport_protocol", arrow::uint8(), false),
                AddressField("a_ip", family, false),
                AddressField("b_ip", family, false),
                arrow::field("a_port", arrow::uint16(), false),
                arrow::field("b_port", arrow::uint16(), false),
                arrow::field("dns_id", arrow::uint16(), false),
                arrow::field("outcome", arrow::utf8(), false),
                arrow::field("query_retries", arrow::uint32(), false),
                arrow::field("query_direction", arrow::uint8(), true),
                arrow::field("response_direction", arrow::uint8(), true),
                arrow::field("qname", arrow::utf8(), true),
                arrow::field("qtype", arrow::uint16(), true),
                arrow::field("qclass", arrow::uint16(), true),
                arrow::field("query_at_ns", arrow::int64(), true),
                arrow::field("response_at_ns", arrow::int64(), true),
                arrow::field("latency_ns", arrow::int64(), true),
                arrow::field("response_rcode", arrow::uint16(), true),
                arrow::field("response_tc", arrow::boolean(), true),
                arrow::field("incomplete_reason", arrow::utf8(), true),
            };
            break;
        case NpmFamilyEntityKind::kHttp1:
            module = "http1";
            base = "http1_transaction";
            fields = {
                arrow::field("entity_instance_id", arrow::uint64(), false),
                arrow::field("revision", arrow::uint64(), false),
                arrow::field("observed_at", arrow::int64(), false),
                arrow::field("is_final", arrow::boolean(), false),
                arrow::field("session_id", arrow::uint64(), false),
                arrow::field("observation_domain_id", arrow::uint64(), false),
                AddressField("a_ip", family, false),
                AddressField("b_ip", family, false),
                arrow::field("a_port", arrow::uint16(), false),
                arrow::field("b_port", arrow::uint16(), false),
                arrow::field("outcome", arrow::utf8(), false),
                arrow::field("informational_count", arrow::uint32(), false),
                arrow::field("request_direction", arrow::uint8(), true),
                arrow::field("response_direction", arrow::uint8(), true),
                arrow::field("method", arrow::utf8(), true),
                arrow::field("target", arrow::utf8(), true),
                arrow::field("host", arrow::utf8(), true),
                arrow::field("status_code", arrow::uint16(), true),
                arrow::field("request_headers_at_ns", arrow::int64(), true),
                arrow::field("response_headers_at_ns", arrow::int64(), true),
                arrow::field("latency_ns", arrow::int64(), true),
                arrow::field("incomplete_reason", arrow::utf8(), true),
            };
            break;
        case NpmFamilyEntityKind::kTls:
            module = "tls";
            base = "tls_handshake";
            fields = {
                arrow::field("entity_instance_id", arrow::uint64(), false),
                arrow::field("revision", arrow::uint64(), false),
                arrow::field("observed_at", arrow::int64(), false),
                arrow::field("is_final", arrow::boolean(), false),
                arrow::field("session_id", arrow::uint64(), false),
                arrow::field("observation_domain_id", arrow::uint64(), false),
                AddressField("a_ip", family, false),
                AddressField("b_ip", family, false),
                arrow::field("a_port", arrow::uint16(), false),
                arrow::field("b_port", arrow::uint16(), false),
                arrow::field("outcome", arrow::utf8(), false),
                arrow::field("hello_retry_count", arrow::uint8(), false),
                arrow::field("incomplete_reason", arrow::utf8(), true),
                arrow::field("client_direction", arrow::uint8(), false),
                arrow::field("client_hello_at_ns", arrow::int64(), true),
                arrow::field("server_hello_at_ns", arrow::int64(), true),
                arrow::field("server_hello_latency_ns", arrow::int64(), true),
                arrow::field("client_sni", arrow::utf8(), true),
                arrow::field("client_alpn_protocols", arrow::utf8(), true),
                arrow::field("client_offered_tls13", arrow::boolean(), true),
                arrow::field("selected_version", arrow::uint16(), true),
                arrow::field("cipher_suite", arrow::uint16(), true),
                arrow::field("selected_alpn", arrow::utf8(), true),
                arrow::field("alert_level", arrow::uint8(), true),
                arrow::field("alert_description", arrow::uint8(), true),
            };
            break;
        case NpmFamilyEntityKind::kIcmp:
            module = "icmp";
            base = "icmp_event";
            fields = {
                arrow::field("entity_instance_id", arrow::uint64(), false),
                arrow::field("revision", arrow::uint64(), false),
                arrow::field("observed_at", arrow::int64(), false),
                arrow::field("is_final", arrow::boolean(), false),
                arrow::field("observation_domain_id", arrow::uint64(), false),
                AddressField("src_ip", family, false),
                AddressField("dst_ip", family, false),
                arrow::field("outcome", arrow::utf8(), false),
                arrow::field("icmp_type", arrow::uint8(), false),
                arrow::field("icmp_code", arrow::uint8(), false),
                arrow::field("outer_truncated", arrow::boolean(), false),
                arrow::field("incomplete_reason", arrow::utf8(), true),
                arrow::field("echo_id", arrow::uint16(), true),
                arrow::field("echo_sequence", arrow::uint16(), true),
                arrow::field("echo_retries", arrow::uint32(), true),
                arrow::field("request_at_ns", arrow::int64(), true),
                arrow::field("reply_at_ns", arrow::int64(), true),
                arrow::field("latency_ns", arrow::int64(), true),
                arrow::field("quote_status", arrow::utf8(), true),
                arrow::field("quoted_ip_family", arrow::uint8(), true),
                arrow::field("quoted_protocol", arrow::uint8(), true),
                AddressField("quoted_src_ip", family, true),
                AddressField("quoted_dst_ip", family, true),
                arrow::field("quoted_src_port", arrow::uint16(), true),
                arrow::field("quoted_dst_port", arrow::uint16(), true),
                arrow::field("active_quoted_session_id", arrow::uint64(), true),
                arrow::field("next_hop_mtu", arrow::uint32(), true),
            };
            break;
        default:
            throw std::invalid_argument("unknown NPM entity kind");
    }
    NpmEntityDescriptorV1 entity;
    entity.module_id = module;
    entity.entity_id = base + (family == packet::AddressFamily::kIPv4 ? "_ipv4" : "_ipv6");
    const bool cumulative = kind == NpmFamilyEntityKind::kBasic || kind == NpmFamilyEntityKind::kSession;
    entity.identity_column = cumulative ? "session_id" : "entity_instance_id";
    entity.revision_semantics = cumulative ? NpmRevisionSemanticsV1::kCumulative : NpmRevisionSemanticsV1::kEvent;
    auto metadata = arrow::key_value_metadata(
        {"flowsql.entity", "flowsql.module", "flowsql.schema_version", "flowsql.timestamp_unit", "flowsql.ip_family",
         "flowsql.revision_semantics", "flowsql.measurement_scope"},
        {entity.entity_id, module, "1", "ns", family == packet::AddressFamily::kIPv4 ? "4" : "6",
         cumulative ? "cumulative" : "event", "single_capture_observed_packets"});
    if (kind == NpmFamilyEntityKind::kBasic) {
        metadata->Append("flowsql.cumulative_scope", "session_until_period_boundary");
        metadata->Append("flowsql.interval_scope", "non_overlapping_period");
    }
    entity.schema = arrow::schema(std::move(fields), std::move(metadata));
    return entity;
}

arrow::Status ValidateNpmFamilyEntityRows(const NpmEntityDescriptorV1& entity, const arrow::RecordBatch& rows) {
    if (!entity.schema || !rows.schema()->Equals(*entity.schema, true))
        return arrow::Status::Invalid("family result Schema mismatch");
    ARROW_RETURN_NOT_OK(rows.ValidateFull());
    for (int column = 0; column < rows.num_columns(); ++column) {
        if (!entity.schema->field(column)->nullable() && rows.column(column)->null_count())
            return arrow::Status::Invalid("NULL required result field: ", entity.schema->field(column)->name());
    }
    auto identity = rows.GetColumnByName(entity.identity_column);
    auto revision = rows.GetColumnByName(entity.revision_column);
    auto final = rows.GetColumnByName(entity.is_final_column);
    if (!identity || !revision || !final || identity->type_id() != arrow::Type::UINT64 ||
        revision->type_id() != arrow::Type::UINT64 || final->type_id() != arrow::Type::BOOL)
        return arrow::Status::Invalid("invalid family identity/revision fields");
    for (int64_t row = 0; row < rows.num_rows(); ++row) {
        if (static_cast<const arrow::UInt64Array&>(*identity).Value(row) == 0 ||
            static_cast<const arrow::UInt64Array&>(*revision).Value(row) == 0)
            return arrow::Status::Invalid("zero family identity/revision");
        if (entity.revision_semantics == NpmRevisionSemanticsV1::kEvent &&
            (static_cast<const arrow::UInt64Array&>(*revision).Value(row) != 1 ||
             !static_cast<const arrow::BooleanArray&>(*final).Value(row)))
            return arrow::Status::Invalid("event requires revision 1 and is_final=true");
    }
    return arrow::Status::OK();
}

arrow::Status ValidateNpmFamilyInputEvent(const NpmFamilyInputEvent& input) {
    if (!input.layer || input.layer->address_state != packet::PacketAddressState::kAvailable)
        return arrow::Status::Invalid("family input requires available endpoints");
    packet::AddressFamily family;
    if (const auto* value = std::get_if<const packet::PacketIpEndpoints4*>(&input.endpoints); value && *value)
        family = packet::AddressFamily::kIPv4;
    else if (const auto* value = std::get_if<const packet::PacketIpEndpoints6*>(&input.endpoints); value && *value)
        family = packet::AddressFamily::kIPv6;
    else
        return arrow::Status::Invalid("family input has no endpoint view");
    if (input.kind == NpmInputKindV1::kControlPacket) {
        if (input.transport || input.session || input.layer->ports_valid ||
            input.layer->transport_protocol != (family == packet::AddressFamily::kIPv4 ? 1 : 58))
            return arrow::Status::Invalid("control packet must be portless and sessionless");
    } else {
        const auto protocol = input.kind == NpmInputKindV1::kTcpPacket ? 6 : 17;
        if ((input.kind != NpmInputKindV1::kTcpPacket && input.kind != NpmInputKindV1::kUdpDatagram) ||
            !input.transport || !input.session || !input.session->facts.session_id ||
            input.session->key.Family() != family || !input.layer->ports_valid ||
            input.layer->transport_protocol != protocol || input.transport->layer != input.layer)
            return arrow::Status::Invalid("transport/session family binding mismatch");
        const auto domain = family == packet::AddressFamily::kIPv4 ? input.session->key.Get4()->observation_domain_id
                                                                   : input.session->key.Get6()->observation_domain_id;
        if (domain != input.observation_domain_id)
            return arrow::Status::Invalid("transport/session observation domain mismatch");
    }
    return arrow::Status::OK();
}
}  // namespace flowsql::npm
