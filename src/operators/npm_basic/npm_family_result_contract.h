// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <framework/interfaces/ipacket_family.h>
#include <operators/npm_basic/modules/dns/npm_dns_udp.h>
#include <operators/npm_basic/modules/http1/npm_http1_transactions.h>
#include <operators/npm_basic/modules/tls/npm_tls_handshake.h>
#include <operators/npm_basic/npm_ip_family_contract.h>

namespace flowsql::npm {

// Family-independent session facts are copied into views. Key storage remains owned by a concrete family table.
struct NpmFamilySessionFacts {
    uint64_t session_id = 0;
    int64_t first_ns = 0;
    int64_t last_ns = 0;
    uint64_t packets_ab = 0;
    uint64_t packets_ba = 0;
    uint64_t wire_bytes_ab = 0;
    uint64_t wire_bytes_ba = 0;
    uint32_t primary_label_id = 0;
    NpmProtocolStatus protocol_status = NpmProtocolStatus::kPending;
    std::optional<uint16_t> protocol_id;
    std::optional<uint16_t> protocol_sub_id;
};
struct NpmFamilySessionView {
    NpmSessionKeyView key;
    NpmFamilySessionFacts facts;
};
template <typename Key>
struct NpmFamilySessionSnapshot {
    Key key;
    NpmFamilySessionFacts facts;
    NpmSessionEndReason end_reason = NpmSessionEndReason::kIdleTimeout;
    NpmFamilySessionView View() const& { return {NpmSessionKeyView(key), facts}; }
    NpmFamilySessionView View() const&& = delete;
};
using NpmSessionSnapshot4 = NpmFamilySessionSnapshot<NpmSessionKey4>;
using NpmSessionSnapshot6 = NpmFamilySessionSnapshot<NpmSessionKey6>;

// session_id is unique for the entire task and never reused. Resolve against the family table at use time.
// Retirement invalidates the handle; timeout/admission indexes never retain key pointers across mutation.
struct NpmFamilySessionHandle {
    uint64_t session_id = 0;
    packet::AddressFamily family = packet::AddressFamily::kNone;
};

// These templates have only the two concrete aliases below; no owning maximum-width result variant exists.

template <typename Address>
struct NpmDnsFamilyResult {
    Address a_ip;
    Address b_ip;
    uint64_t observation_domain_id = 0;
    uint16_t a_port = 0;
    uint16_t b_port = 0;
    uint8_t transport_protocol = 0;
    NpmDnsUdpResultV1 transaction;
};
using NpmDnsResult4 = NpmDnsFamilyResult<network::IPv4Address>;
using NpmDnsResult6 = NpmDnsFamilyResult<network::IPv6Address>;

template <typename Address>
struct NpmHttp1FamilyResult {
    Address a_ip;
    Address b_ip;
    uint64_t observation_domain_id = 0;
    uint16_t a_port = 0;
    uint16_t b_port = 0;
    NpmHttp1TransactionResultV1 transaction;
};
using NpmHttp1Result4 = NpmHttp1FamilyResult<network::IPv4Address>;
using NpmHttp1Result6 = NpmHttp1FamilyResult<network::IPv6Address>;

template <typename Address>
struct NpmTlsFamilyResult {
    Address a_ip;
    Address b_ip;
    uint64_t observation_domain_id = 0;
    uint16_t a_port = 0;
    uint16_t b_port = 0;
    NpmTlsHandshakeResultV1 transaction;
};
using NpmTlsResult4 = NpmTlsFamilyResult<network::IPv4Address>;
using NpmTlsResult6 = NpmTlsFamilyResult<network::IPv6Address>;

template <typename Address>
struct NpmIcmpFamilyEchoKey {
    uint64_t observation_domain_id = 0;
    Address src_ip;
    Address dst_ip;
    uint16_t identifier = 0;
    uint16_t sequence = 0;
};
using NpmIcmpEchoKey4 = NpmIcmpFamilyEchoKey<network::IPv4Address>;
using NpmIcmpEchoKey6 = NpmIcmpFamilyEchoKey<network::IPv6Address>;

template <typename Address>
struct NpmIcmpFamilyQuotedFlow {
    uint64_t observation_domain_id = 0;
    uint8_t protocol = 0;
    Address src_ip;
    Address dst_ip;
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
};
using NpmIcmpQuotedFlow4 = NpmIcmpFamilyQuotedFlow<network::IPv4Address>;
using NpmIcmpQuotedFlow6 = NpmIcmpFamilyQuotedFlow<network::IPv6Address>;

template <typename Address>
struct NpmIcmpFamilyEvent {
    uint64_t entity_instance_id = 0;
    int64_t observed_at = 0;
    uint64_t observation_domain_id = 0;
    Address src_ip;
    Address dst_ip;
    std::string outcome;
    uint8_t icmp_type = 0;
    uint8_t icmp_code = 0;
    bool outer_truncated = false;
    std::optional<std::string> incomplete_reason;
    std::optional<uint16_t> echo_id;
    std::optional<uint16_t> echo_sequence;
    std::optional<uint32_t> echo_retries;
    std::optional<int64_t> request_at_ns;
    std::optional<int64_t> reply_at_ns;
    std::optional<int64_t> latency_ns;
    std::optional<std::string> quote_status;
    std::optional<uint8_t> quoted_ip_family;
    std::optional<uint8_t> quoted_protocol;
    std::optional<Address> quoted_src_ip;
    std::optional<Address> quoted_dst_ip;
    std::optional<uint16_t> quoted_src_port;
    std::optional<uint16_t> quoted_dst_port;
    std::optional<uint64_t> active_quoted_session_id;
    std::optional<uint32_t> next_hop_mtu;
};
using NpmIcmpEvent4 = NpmIcmpFamilyEvent<network::IPv4Address>;
using NpmIcmpEvent6 = NpmIcmpFamilyEvent<network::IPv6Address>;

// All views below borrow the batch/table only until the callback returns. Snapshot/stream state copies its family key.
struct NpmFamilyPacketView {
    packet::PacketView packet;
    const packet::PacketLayerFacts* layer = nullptr;
    Span<const uint8_t> payload;
    NpmPacketDirection direction = NpmPacketDirection::kAToB;
    NpmTransportPacketFacts transport;
};
struct NpmFamilyInputEvent {
    NpmInputKindV1 kind = NpmInputKindV1::kTcpPacket;
    uint64_t observation_domain_id = 0;
    packet::PacketView packet;
    const packet::PacketLayerFacts* layer = nullptr;
    std::variant<std::monostate, const packet::PacketIpEndpoints4*, const packet::PacketIpEndpoints6*> endpoints;
    Span<const uint8_t> body;
    bool body_complete = true;
    const NpmFamilyPacketView* transport = nullptr;
    const NpmFamilySessionView* session = nullptr;
};

interface INpmFamilyResultWriter {
    virtual ~INpmFamilyResultWriter() = default;
    // Synchronously copy/encode the borrowed result. Nonzero aborts the task; retain no argument reference.
    virtual int WriteBasic(const NpmBasicResult4&) = 0;
    virtual int WriteBasic(const NpmBasicResult6&) = 0;
    virtual int WriteSession(const NpmSessionResult4&) = 0;
    virtual int WriteSession(const NpmSessionResult6&) = 0;
};
interface INpmFamilyAnalysisModule {
    virtual ~INpmFamilyAnalysisModule() = default;
    virtual int OnPacket(const NpmFamilyPacketView&, const NpmFamilySessionView&, INpmFamilyResultWriter&) = 0;
    virtual int OnSessionSnapshot(const NpmFamilySessionView&, int64_t observed_at_ns, INpmFamilyResultWriter&) = 0;
    virtual int OnSessionEnd(const NpmFamilySessionView&, NpmSessionEndReason, int64_t observed_at_ns,
                             INpmFamilyResultWriter&) = 0;
    virtual int OnTime(int64_t watermark_ns, int64_t observed_at_ns, INpmFamilyResultWriter&) = 0;
    virtual int OnFinish(int64_t observed_at_ns, INpmFamilyResultWriter&) = 0;
};
interface INpmFamilyProtocolModule {
    virtual ~INpmFamilyProtocolModule() = default;
    virtual int OnInput(const NpmFamilyInputEvent&, INpmResultEmitterV1&) = 0;
    virtual int OnSessionSnapshot(const NpmFamilySessionView&, int64_t observed_at_ns, INpmResultEmitterV1&) = 0;
    virtual int OnSessionEnd(const NpmFamilySessionView&, NpmSessionEndReason, int64_t observed_at_ns,
                             INpmResultEmitterV1&) = 0;
    virtual std::optional<int64_t> NextEventDeadlineNs() const = 0;
    virtual int OnTime(const NpmModuleTimeV1&, INpmResultEmitterV1&) = 0;
    virtual int Finish(int64_t observed_at_ns, INpmResultEmitterV1&) = 0;
    // Idempotent; called after in-flight callbacks exit. Cancel/failure emits no normal terminal rows.
    virtual void Abort() noexcept = 0;
};
interface INpmFamilyIcmpActiveSessionLookup {
    virtual ~INpmFamilyIcmpActiveSessionLookup() = default;
    // Same namespace/domain/family tuple, currently active only. Never creates or extends a session.
    virtual std::optional<uint64_t> FindActive(const NpmIcmpQuotedFlow4&) const = 0;
    virtual std::optional<uint64_t> FindActive(const NpmIcmpQuotedFlow6&) const = 0;
};

enum class NpmFamilyEntityKind : uint8_t { kBasic, kSession, kDns, kHttp1, kTls, kIcmp };
NpmEntityDescriptorV1 NpmFamilyEntityDescriptor(NpmFamilyEntityKind kind, packet::AddressFamily family);
arrow::Status ValidateNpmFamilyEntityRows(const NpmEntityDescriptorV1& entity, const arrow::RecordBatch& rows);
arrow::Status ValidateNpmFamilyInputEvent(const NpmFamilyInputEvent& input);

}  // namespace flowsql::npm
