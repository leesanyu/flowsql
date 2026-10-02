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
    using Emit = std::function<int(const NpmIcmpEventV1&)>;

    NpmIcmpEchoTrackerV1(NpmIcmpConfigV1 config, std::shared_ptr<INpmTaskBudget> budget);
    ~NpmIcmpEchoTrackerV1();

    int OnEcho(const NpmIcmpParsedV1& message, uint64_t observation_domain_id, int64_t captured_at_ns,
               const Emit& emit);
    int OnTime(std::optional<int64_t> watermark_ns, const Emit& emit);
    int Finish(int64_t observed_at_ns, const Emit& emit);
    void Abort() noexcept;

    std::optional<int64_t> NextEventDeadlineNs() const;
    size_t PendingCount() const noexcept { return pending_.size(); }
    uint64_t AllocateEventId() noexcept;

 private:
    struct EchoKeyLess {
        bool operator()(const NpmIcmpEchoKeyV1& left, const NpmIcmpEchoKeyV1& right) const;
    };
    using DeadlineKey = std::pair<int64_t, uint64_t>;
    static constexpr uint64_t kPendingCharge =
        sizeof(NpmIcmpEchoKeyV1) * 2 + sizeof(NpmIcmpPendingEchoV1) + sizeof(DeadlineKey) + 8 * sizeof(void*);

    NpmIcmpEventV1 MakeEchoEvent(const NpmIcmpEchoKeyV1& key, const NpmIcmpPendingEchoV1& pending,
                                 int64_t observed_at_ns, std::string outcome) const;
    void Erase(const NpmIcmpEchoKeyV1& key, const NpmIcmpPendingEchoV1& pending) noexcept;

    NpmIcmpConfigV1 config_;
    std::shared_ptr<INpmTaskBudget> budget_;
    std::map<NpmIcmpEchoKeyV1, NpmIcmpPendingEchoV1, EchoKeyLess> pending_;
    std::map<DeadlineKey, NpmIcmpEchoKeyV1> deadlines_;
    std::optional<int64_t> last_watermark_ns_;
    uint64_t next_event_id_ = 1;
    bool finished_ = false;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_ICMP_ECHO_H_
