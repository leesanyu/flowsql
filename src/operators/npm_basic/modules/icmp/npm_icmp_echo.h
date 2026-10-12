// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_ICMP_ECHO_H_
#define FLOWSQL_NPM_ICMP_ECHO_H_

#include "npm_icmp_parser.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace flowsql::npm {

class NpmIcmpEchoTrackerV1 {
 public:
    struct Emit {
        std::function<int(const NpmIcmpEvent4&)> ipv4;
        std::function<int(const NpmIcmpEvent6&)> ipv6;
        template <typename F>
        Emit(const F& callback) : ipv4(callback), ipv6(callback) {}
        int operator()(const NpmIcmpEvent4& row) const { return ipv4(row); }
        int operator()(const NpmIcmpEvent6& row) const { return ipv6(row); }
        explicit operator bool() const { return bool(ipv4) && bool(ipv6); }
    };

    NpmIcmpEchoTrackerV1(NpmIcmpConfigV1 config, std::shared_ptr<INpmTaskBudget> budget);
    ~NpmIcmpEchoTrackerV1();

    template <typename Address>
    int OnEcho(const NpmIcmpFamilyParsed<Address>& message, uint64_t observation_domain_id, int64_t captured_at_ns,
               const Emit& emit);
    int OnTime(std::optional<int64_t> watermark_ns, const Emit& emit);
    int Finish(int64_t observed_at_ns, const Emit& emit);
    void Abort() noexcept;

    std::optional<int64_t> NextEventDeadlineNs() const;
    size_t PendingCount() const noexcept { return pending4_.size() + pending6_.size(); }
    uint64_t AllocateEventId() noexcept;

 private:
    template <typename Address>
    struct EchoKeyLess {
        bool operator()(const NpmIcmpFamilyEchoKey<Address>&, const NpmIcmpFamilyEchoKey<Address>&) const;
    };
    template <typename Address>
    using PendingMap = std::map<NpmIcmpFamilyEchoKey<Address>, NpmIcmpPendingEchoV1, EchoKeyLess<Address>>;
    using DeadlineKey = std::pair<int64_t, uint64_t>;
    // std::map nodes remain stable until retirement. The deadline owns no duplicate address key.
    using DeadlineTarget = std::variant<const NpmIcmpEchoKey4*, const NpmIcmpEchoKey6*>;
    template <typename Address>
    static constexpr uint64_t PendingCharge() {
        return sizeof(NpmIcmpFamilyEchoKey<Address>) + sizeof(NpmIcmpPendingEchoV1) + sizeof(DeadlineKey) +
               sizeof(DeadlineTarget) + 8 * sizeof(void*);
    }
    template <typename Address>
    NpmIcmpFamilyEvent<Address> MakeEchoEvent(const NpmIcmpFamilyEchoKey<Address>&, const NpmIcmpPendingEchoV1&,
                                              int64_t observed_at_ns, std::string outcome) const;
    template <typename Address>
    void Erase(const NpmIcmpFamilyEchoKey<Address>&, const NpmIcmpPendingEchoV1&) noexcept;
    template <typename Address>
    PendingMap<Address>& Pending() {
        if constexpr (sizeof(Address) == 4)
            return pending4_;
        else
            return pending6_;
    }
    int EmitDeadline(int64_t observed_at_ns, const char* reason, const Emit&);

    NpmIcmpConfigV1 config_;
    std::shared_ptr<INpmTaskBudget> budget_;
    PendingMap<network::IPv4Address> pending4_;
    PendingMap<network::IPv6Address> pending6_;
    std::map<DeadlineKey, DeadlineTarget> deadlines_;
    std::optional<int64_t> last_watermark_ns_;
    uint64_t next_event_id_ = 1;
    bool finished_ = false;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_ICMP_ECHO_H_
