// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_TLS_MODULE_H_
#define FLOWSQL_NPM_TLS_MODULE_H_

#include "npm_tls_result_encoder.h"

#include <map>
#include <memory>

namespace flowsql::npm {

/** Task-private TLS module; shared stream views live only within callbacks. */
class NpmTlsProtocolModuleV1 final : public INpmProtocolModuleV1, public INpmTcpStreamConsumerV1 {
 public:
    NpmTlsProtocolModuleV1(NpmTlsConfigV1 config, std::shared_ptr<INpmTaskBudget> budget);
    ~NpmTlsProtocolModuleV1() override;

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
    struct SessionState {
        NpmTlsSessionIdentityV1 identity;
        std::unique_ptr<NpmTlsHandshakeTrackerV1> tracker;
        uint64_t reserved_bytes = 0;
    };
    int RememberSession(const NpmSessionView& session);
    int EmitResult(SessionState& state, std::optional<NpmTlsHandshakeResultV1>* result, INpmResultEmitterV1& emitter);
    void ForgetSession(uint64_t session_id) noexcept;
    void ReleaseTracker(SessionState& state) noexcept;

    NpmTlsConfigV1 config_;
    std::shared_ptr<INpmTaskBudget> budget_;
    std::map<uint64_t, SessionState> sessions_;
    bool finished_ = false;
    bool aborted_ = false;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_TLS_MODULE_H_
