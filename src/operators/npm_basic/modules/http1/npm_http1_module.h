// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_HTTP1_MODULE_H_
#define FLOWSQL_NPM_HTTP1_MODULE_H_

#include "npm_http1_result_encoder.h"

#include <map>
#include <memory>

namespace flowsql::npm {

/** Task-private HTTP/1 module; the shared stream cursor is borrowed only during callbacks. */
class NpmHttp1ProtocolModuleV1 final : public INpmProtocolModuleV1, public INpmTcpStreamConsumerV1 {
 public:
    NpmHttp1ProtocolModuleV1(NpmHttp1ConfigV1 config, std::shared_ptr<INpmTaskBudget> budget);
    ~NpmHttp1ProtocolModuleV1() override;

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
    void ForgetSession(uint64_t session_id) noexcept;
    int EmitRows(std::vector<NpmHttp1TransactionResultV1>* rows, INpmResultEmitterV1& emitter);

    std::shared_ptr<INpmTaskBudget> budget_;
    NpmHttp1TransactionTrackerV1 transactions_;
    std::map<uint64_t, NpmHttp1SessionIdentityV1> identities_;
    bool finished_ = false;
    bool aborted_ = false;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_HTTP1_MODULE_H_
