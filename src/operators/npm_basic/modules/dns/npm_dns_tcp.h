// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_DNS_TCP_H_
#define FLOWSQL_NPM_DNS_TCP_H_

#include "npm_dns_udp.h"

#include <operators/npm_basic/npm_tcp_stream_contract.h>

#include <map>
#include <utility>
#include <vector>

namespace flowsql::npm {

/** Task-private DNS/TCP framer over borrowed shared-stream events. */
class NpmDnsTcpFramerV1 final {
 public:
    explicit NpmDnsTcpFramerV1(NpmDnsUdpTrackerV1* transactions, std::shared_ptr<INpmTaskBudget> budget = {})
        : transactions_(transactions), budget_(std::move(budget)) {}
    ~NpmDnsTcpFramerV1() { Abort(); }
    int OnReadable(const NpmTcpStreamContextV1& context, INpmTcpStreamCursorV1& cursor,
                   std::vector<NpmDnsUdpResultV1>* results);
    int OnSessionEnd(uint64_t session_id, int64_t observed_at_ns, std::vector<NpmDnsUdpResultV1>* results);
    void Abort() noexcept;
    size_t ActiveDirections() const noexcept { return directions_.size(); }

 private:
    struct DirectionState {
        bool disabled = false;
        uint8_t prefix[2] = {};
        uint8_t prefix_size = 0;
        uint16_t frame_length = 0;
        uint64_t frame_charge = 0;
        std::vector<uint8_t> frame;
        bool all_times_known = true;
        std::optional<int64_t> max_time_ns;
    };
    using DirectionKey = std::pair<uint64_t, NpmPacketDirection>;
    int ConsumeData(DirectionState* state, const NpmTcpStreamContextV1& context, const NpmTcpStreamEventV1& event,
                    std::vector<NpmDnsUdpResultV1>* results);
    void ForgetFrame(DirectionState* state) noexcept;
    void EraseDirection(const DirectionKey& key) noexcept;

    NpmDnsUdpTrackerV1* transactions_ = nullptr;  // Owned by the enclosing DNS module.
    std::shared_ptr<INpmTaskBudget> budget_;
    std::map<DirectionKey, DirectionState> directions_;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_DNS_TCP_H_
