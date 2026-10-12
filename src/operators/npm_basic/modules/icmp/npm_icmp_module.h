// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_ICMP_MODULE_H_
#define FLOWSQL_NPM_ICMP_MODULE_H_

#include "npm_icmp_echo.h"

#include <operators/npm_basic/core/npm_session_table.h>

#include <memory>
#include <string>

namespace flowsql::npm {

class NpmIcmpProtocolModuleV1 final : public INpmProtocolModuleV1, private INpmFamilyIcmpActiveSessionLookup {
 public:
    NpmIcmpProtocolModuleV1(NpmIcmpConfigV1 config, std::shared_ptr<INpmTaskBudget> budget);
    void BindSessions(const NpmSessionTable* sessions, std::string input_namespace);

    int OnInput(const NpmInputEventV1&, INpmResultEmitterV1&) override;
    int OnSessionSnapshot(const NpmSessionView&, int64_t, INpmResultEmitterV1&) override { return 0; }
    int OnSessionEnd(const NpmSessionView&, NpmSessionEndReason, int64_t, INpmResultEmitterV1&) override { return 0; }
    std::optional<int64_t> NextEventDeadlineNs() const override;
    int OnTime(const NpmModuleTimeV1&, INpmResultEmitterV1&) override;
    int Finish(int64_t, INpmResultEmitterV1&) override;
    void Abort() noexcept override;

 private:
    std::optional<uint64_t> FindActive(const NpmIcmpQuotedFlow4&) const override;
    std::optional<uint64_t> FindActive(const NpmIcmpQuotedFlow6&) const override;
    template <typename Address>
    std::optional<uint64_t> Lookup(const NpmIcmpFamilyQuotedFlow<Address>&) const;
    template <typename Address>
    int OnFamilyInput(const NpmInputEventV1&, INpmResultEmitterV1&);
    template <typename Address>
    int Emit(const NpmIcmpFamilyEvent<Address>&, INpmResultEmitterV1&);

    std::shared_ptr<INpmTaskBudget> budget_;
    NpmIcmpEchoTrackerV1 echoes_;
    const NpmSessionTable* sessions_ = nullptr;
    std::string input_namespace_;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_ICMP_MODULE_H_
