// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_HTTP1_MODULE_H_
#define FLOWSQL_NPM_HTTP1_MODULE_H_

#include "npm_http1_result_encoder.h"

#include <operators/npm_basic/modules/npm_transaction_identity.h>
#include <map>
#include <memory>

namespace flowsql::npm {

/** Task-private HTTP/1 module; the shared stream cursor is borrowed only during callbacks. */
class NpmHttp1ProtocolModule final : public INpmProtocolModule, public INpmTcpStreamConsumer {
 public:
    NpmHttp1ProtocolModule(NpmHttp1Config config, std::shared_ptr<INpmTaskBudget> budget);
    ~NpmHttp1ProtocolModule() override;

    int OnInput(const NpmInputEvent& input, INpmResultEmitter& emitter) override;
    int OnSessionSnapshot(const NpmSessionView& session, int64_t observed_at_ns, INpmResultEmitter& emitter) override;
    int OnSessionEnd(const NpmSessionView& session, NpmSessionEndReason reason, int64_t observed_at_ns,
                     INpmResultEmitter& emitter) override;
    std::optional<int64_t> NextEventDeadlineNs() const override;
    int OnTime(const NpmModuleTime& time, INpmResultEmitter& emitter) override;
    int Finish(int64_t observed_at_ns, INpmResultEmitter& emitter) override;
    void Abort() noexcept override;
    int OnTcpStreamReadable(const NpmTcpStreamContext& context, INpmTcpStreamCursor& cursor,
                            INpmResultEmitter& emitter) override;

 private:
    int RememberSession(const NpmSessionView& session);
    void ForgetSession(uint64_t session_id) noexcept;
    int EmitRows(std::vector<NpmHttp1TransactionResult>* rows, INpmResultEmitter& emitter);

    std::shared_ptr<INpmTaskBudget> budget_;
    NpmHttp1TransactionTracker transactions_;
    std::map<uint64_t, NpmTransactionIdentity> identities_;
    bool finished_ = false;
    bool aborted_ = false;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_HTTP1_MODULE_H_
