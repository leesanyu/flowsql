// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_TLS_MODULE_H_
#define FLOWSQL_NPM_TLS_MODULE_H_

#include "npm_tls_result_encoder.h"

#include <operators/npm_basic/modules/npm_transaction_identity.h>
#include <map>
#include <memory>

namespace flowsql::npm {

/** Task-private TLS module; shared stream views live only within callbacks. */
class NpmTlsProtocolModule final : public INpmProtocolModule, public INpmTcpStreamConsumer {
 public:
    NpmTlsProtocolModule(NpmTlsConfig config, std::shared_ptr<INpmTaskBudget> budget);
    ~NpmTlsProtocolModule() override;

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
    struct SessionState {
        NpmTransactionIdentity identity;
        std::unique_ptr<NpmTlsHandshakeTracker> tracker;
        uint64_t reserved_bytes = 0;
    };
    int RememberSession(const NpmSessionView& session);
    int EmitResult(SessionState& state, std::optional<NpmTlsHandshakeResult>* result, INpmResultEmitter& emitter);
    void ForgetSession(uint64_t session_id) noexcept;
    void ReleaseTracker(SessionState& state) noexcept;

    NpmTlsConfig config_;
    std::shared_ptr<INpmTaskBudget> budget_;
    std::map<uint64_t, SessionState> sessions_;
    bool finished_ = false;
    bool aborted_ = false;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_TLS_MODULE_H_
