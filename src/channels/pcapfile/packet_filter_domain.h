// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_CHANNELS_PCAPFILE_PACKET_FILTER_DOMAIN_H_
#define _FLOWSQL_CHANNELS_PCAPFILE_PACKET_FILTER_DOMAIN_H_

#include <framework/core/packet_filter_plan.h>
#include <framework/interfaces/ifilter_domain_resolver.h>

#include <cstdint>
#include <string>

namespace flowsql::channels::pcapfile {

/** Compile one RFC3339 timestamp with an explicit UTC offset into epoch nanoseconds. */
int ParseRfc3339TimestampNs(const std::string& text, int64_t* output, std::string* error);

/** Compile packet-domain literals once before packet evaluation. */
int CompilePacketMacKey(const std::string& text, packet::PacketMacKey* output, std::string* error);
int CompilePacketIpKey(const std::string& text, packet::PacketIpKey* output, std::string* error);
int CompilePacketPort(const std::string& text, uint16_t* output, std::string* error);

/** Compile a direction-neutral tcp/udp endpoint pair in canonical endpoint order. */
int CompileTransportPairKey(const std::string& function_name,
                            const std::string& endpoint1_ip,
                            const std::string& endpoint1_port,
                            const std::string& endpoint2_ip,
                            const std::string& endpoint2_port,
                            packet::TransportPairKey* output,
                            std::string* error);

/** Recursively sort and deduplicate the typed key vectors in a packet rule tree. */
void CanonicalizePacketFilterRuleKeys(packet::PacketFilterRule* rule);

/** Stateless pcapfile owner for packet-domain filter syntax. */
class PcapFilterDomainResolver final : public IFilterDomainResolverV1 {
 public:
    int Resolve(const FilterDomainResolveRequestV1& request,
                FilterDomainResolveResultV1* result) const override;
};

}  // namespace flowsql::channels::pcapfile

#endif  // _FLOWSQL_CHANNELS_PCAPFILE_PACKET_FILTER_DOMAIN_H_
