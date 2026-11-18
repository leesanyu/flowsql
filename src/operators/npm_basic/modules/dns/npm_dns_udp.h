// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_DNS_UDP_H_
#define FLOWSQL_NPM_DNS_UDP_H_

#include "npm_dns_contract.h"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace flowsql::npm {

enum class NpmDnsMessageStatus : uint8_t {
    kComplete = 0,
    kUnsupported,
    kCaptureTruncated,
    kMalformed,
};

struct NpmDnsParsedMessage {
    NpmDnsMessage message;
    bool key_available = false;
    std::string qname_display;
};

/** Does not retain body. A malformed response may still have a complete, safe question key. */
NpmDnsMessageStatus ParseNpmDnsUdpMessage(Span<const uint8_t> body, bool body_complete, NpmDnsParsedMessage* output);

struct NpmDnsUdpResult {
    uint64_t entity_instance_id = 0;
    uint64_t session_id = 0;
    uint16_t dns_id = 0;
    std::string outcome;
    std::optional<std::string> incomplete_reason;
    uint32_t query_retries = 0;
    std::optional<NpmPacketDirection> query_direction;
    std::optional<NpmPacketDirection> response_direction;
    std::optional<std::string> qname;
    std::optional<uint16_t> qtype;
    std::optional<uint16_t> qclass;
    std::optional<int64_t> query_at_ns;
    std::optional<int64_t> response_at_ns;
    std::optional<int64_t> latency_ns;
    std::optional<uint16_t> response_rcode;
    std::optional<bool> response_tc;
    int64_t observed_at_ns = 0;
};

/** DNS message and transaction core shared by UDP datagrams and TCP frames. */
class NpmDnsUdpTracker final {
 public:
    explicit NpmDnsUdpTracker(NpmDnsConfig config, std::shared_ptr<INpmTaskBudget> budget = {});
    ~NpmDnsUdpTracker();
    int OnDatagram(uint64_t session_id, NpmPacketDirection direction, Span<const uint8_t> body, bool body_complete,
                   int64_t captured_at_ns, std::vector<NpmDnsUdpResult>* results);
    int OnMessage(uint64_t session_id, NpmPacketDirection direction, Span<const uint8_t> body, bool body_complete,
                  std::optional<int64_t> complete_at_ns, int64_t observed_at_ns, std::vector<NpmDnsUdpResult>* results);
    int OnResponsePathGap(uint64_t session_id, NpmPacketDirection response_direction, int64_t observed_at_ns,
                          std::vector<NpmDnsUdpResult>* results);
    std::optional<int64_t> NextEventDeadlineNs() const;
    int OnCaptureWatermark(int64_t watermark_ns, std::vector<NpmDnsUdpResult>* results);
    int OnSessionEnd(uint64_t session_id, int64_t observed_at_ns, std::vector<NpmDnsUdpResult>* results);
    size_t PendingCount(uint64_t session_id) const;
    /** Clears callback-local terminal rows and returns their reserved module-state workspace. */
    void ReleaseResults(std::vector<NpmDnsUdpResult>* results) noexcept;
    void Abort() noexcept;

 private:
    struct Pending {
        NpmDnsTransaction transaction;
        std::string qname_display;
    };
    uint64_t NextId();
    bool ChargePending();
    void ReleasePending(size_t count = 1) noexcept;
    bool ChargeResult();

    NpmDnsConfig config_;
    std::shared_ptr<INpmTaskBudget> budget_;
    uint64_t next_id_ = 1;
    std::optional<int64_t> watermark_ns_;
    uint64_t result_charge_bytes_ = 0;
    std::map<uint64_t, std::list<Pending>> pending_;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_DNS_UDP_H_
