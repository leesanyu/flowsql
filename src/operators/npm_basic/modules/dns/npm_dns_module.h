// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_DNS_MODULE_H_
#define FLOWSQL_NPM_DNS_MODULE_H_

#include "npm_dns_result_encoder.h"
#include "npm_dns_tcp.h"

#include <map>
#include <memory>

namespace flowsql::npm {

/** Task-private DNS module; the caller owns protocol and borrows its stream interface. */
class NpmDnsProtocolModuleV1 final : public INpmProtocolModuleV1, public INpmTcpStreamConsumerV1 {
 public:
    NpmDnsProtocolModuleV1(NpmDnsConfigV1 config, std::shared_ptr<INpmTaskBudget> budget);
    ~NpmDnsProtocolModuleV1() override;

    int OnInput(const NpmInputEventV1& input, INpmResultEmitterV1& emitter) override;
    int OnSessionSnapshot(const NpmSessionView& session, int64_t observed_at_ns, INpmResultEmitterV1& emitter) override;
    int OnSessionEnd(const NpmSessionView& session, NpmSessionEndReason reason, int64_t observed_at_ns,
                     INpmResultEmitterV1& emitter) override;
    std::optional<int64_t> NextEventDeadlineNs() const override;
    int OnTime(const NpmModuleTimeV1& time, INpmResultEmitterV1& emitter) override;
    int Finish(int64_t observed_at_ns, INpmResultEmitterV1& emitter) override;
    void Abort() noexcept override;
    int OnTcpStreamReadable(const NpmTcpStreamContextV1& context, INpmTcpStreamCursorV1& cursor,
                            INpmResultEmitterV1& emitter) override;

 private:
    int RememberSession(const NpmSessionView& session);
    int EmitRows(std::vector<NpmDnsUdpResultV1>* rows, INpmResultEmitterV1& emitter);
    void ForgetSession(uint64_t session_id) noexcept;

    std::shared_ptr<INpmTaskBudget> budget_;
    NpmDnsUdpTrackerV1 transactions_;
    NpmDnsTcpFramerV1 framer_;
    std::map<uint64_t, NpmDnsSessionIdentityV1> identities_;
    bool finished_ = false;
    bool aborted_ = false;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_DNS_MODULE_H_
