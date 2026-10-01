// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_HTTP1_RESULT_ENCODER_H_
#define FLOWSQL_NPM_HTTP1_RESULT_ENCODER_H_

#include "npm_http1_transactions.h"

#include <memory>
#include <string>

namespace arrow {
class MemoryPool;
}

namespace flowsql::npm {

struct NpmHttp1SessionIdentityV1 {
    uint64_t session_id = 0;
    uint64_t observation_domain_id = 0;
    std::string a_ip;
    std::string b_ip;
    uint16_t a_port = 0;
    uint16_t b_port = 0;
};

/** Encodes one final event and synchronously hands it to the task's result router. */
int EmitNpmHttp1TransactionV1(const NpmHttp1TransactionResultV1& result, const NpmHttp1SessionIdentityV1& session,
                              const std::shared_ptr<INpmTaskBudget>& budget, INpmResultEmitterV1& emitter,
                              arrow::MemoryPool* pool = nullptr);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_HTTP1_RESULT_ENCODER_H_
