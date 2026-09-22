// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_SESSION_ANALYSIS_MODULE_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_SESSION_ANALYSIS_MODULE_H_

#include "npm_tcp_performance_tracker.h"

#include <operators/npm_basic/core/npm_protocol_context.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace flowsql::npm {

/** Task-private Session result module. Borrowed callback views are never retained. */
class NpmSessionAnalysisModule final : public INpmAnalysisModule {
 public:
    NpmSessionAnalysisModule(const NpmProtocolContext& protocol_context, std::shared_ptr<INpmTaskBudget> budget,
                             uint32_t max_ranges_per_direction);
    ~NpmSessionAnalysisModule() override;

    NpmSessionAnalysisModule(const NpmSessionAnalysisModule&) = delete;
    NpmSessionAnalysisModule& operator=(const NpmSessionAnalysisModule&) = delete;
    NpmSessionAnalysisModule(NpmSessionAnalysisModule&&) = delete;
    NpmSessionAnalysisModule& operator=(NpmSessionAnalysisModule&&) = delete;

    int OnPacket(const NpmPacketView& packet, const NpmSessionView& session, INpmResultWriter& writer) override;
    int OnSessionSnapshot(const NpmSessionView& session, int64_t observed_at_ns, INpmResultWriter& writer) override;
    int OnSessionEnd(const NpmSessionView& session, NpmSessionEndReason reason, int64_t observed_at_ns,
                     INpmResultWriter& writer) override;

    void Clear() noexcept;
    size_t tracked_sessions() const noexcept;
    uint64_t tracked_bytes() const noexcept;

 private:
    int Emit(const NpmSessionView& session, bool is_final, NpmSessionEndReason reason, int64_t observed_at_ns,
             INpmResultWriter& writer);
    void ReleaseRevision(uint64_t bytes) noexcept;

    const NpmProtocolContext& protocol_context_;
    std::shared_ptr<INpmTaskBudget> budget_;
    NpmTcpPerformanceTracker tracker_;
    std::unordered_map<uint64_t, uint64_t> revisions_;
    uint64_t revision_bytes_ = 0;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_SESSION_ANALYSIS_MODULE_H_
