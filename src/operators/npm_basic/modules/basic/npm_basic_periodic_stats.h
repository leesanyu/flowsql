// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_BASIC_PERIODIC_STATS_H_
#define FLOWSQL_NPM_BASIC_PERIODIC_STATS_H_

#include <operators/npm_basic/core/npm_session_table.h>
#include <map>
#include "npm_basic_result_projector.h"

namespace flowsql::npm {

/** One task's event-time buckets; owned metadata and all retained buckets are budgeted. */
class NpmBasicPeriodicStats final : public INpmAnalysisModule {
 public:
    NpmBasicPeriodicStats(NpmAnalysisConfig config, std::shared_ptr<INpmTaskBudget> budget,
                          NpmBasicResultProjector& projector);
    ~NpmBasicPeriodicStats() override;
    int OnPacket(const NpmPacketView&, const NpmSessionView&, INpmResultWriter&) override;
    int OnSessionSnapshot(const NpmSessionView&, int64_t, INpmResultWriter&) override { return 0; }
    int OnSessionEnd(const NpmSessionView&, NpmSessionEndReason, int64_t, INpmResultWriter&) override;
    int OnTime(int64_t watermark_ns, int64_t observed_at_ns, INpmResultWriter&) override;
    int OnFinish(int64_t observed_at_ns, INpmResultWriter&) override;
    const std::string& LastError() const { return error_; }

 private:
    struct Bucket {
        uint64_t packets_ab = 0, packets_ba = 0, bytes_ab = 0, bytes_ba = 0;
        int64_t first_ns = 0, last_ns = 0;
    };
    struct State {
        NpmSessionSnapshot snapshot;
        std::map<int64_t, Bucket> buckets;
        int64_t next_start_ns = 0;
        std::optional<NpmBasicPeriodStats> last_period;
        std::optional<int64_t> first_ns, last_ns;
        uint64_t packets_ab = 0, packets_ba = 0, bytes_ab = 0, bytes_ba = 0;
        uint64_t charge = 0;
        std::optional<int64_t> end_ns;
    };
    int RetainMetadata(State&, const NpmSessionView&);
    int Drain(State&, int64_t boundary_ns, bool finish, int64_t observed_at_ns, INpmResultWriter&);
    int Emit(State&, bool complete, bool final, int64_t observed_at_ns, INpmResultWriter&);
    int Fail(std::string message);
    void Release(State&);
    void Schedule(const State&);
    NpmAnalysisConfig config_;
    std::shared_ptr<INpmTaskBudget> budget_;
    NpmBasicResultProjector& projector_;
    std::map<uint64_t, State> states_;
    std::optional<int64_t> watermark_ns_, input_end_ns_;
    std::optional<int64_t> next_due_ns_;
    std::string error_;
};

}  // namespace flowsql::npm
#endif  // FLOWSQL_NPM_BASIC_PERIODIC_STATS_H_
