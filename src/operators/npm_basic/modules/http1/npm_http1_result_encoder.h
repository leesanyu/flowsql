// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_HTTP1_RESULT_ENCODER_H_
#define FLOWSQL_NPM_HTTP1_RESULT_ENCODER_H_

#include <operators/npm_basic/npm_family_result_contract.h>

#include <memory>
#include <string>

namespace arrow {
class MemoryPool;
}

namespace flowsql::npm {

/** Encodes one final event and synchronously hands it to the task's result router. */
template <typename Address>
int EmitNpmHttp1TransactionV1(const NpmHttp1FamilyResult<Address>& family_result,
                              const std::shared_ptr<INpmTaskBudget>& budget, INpmResultEmitterV1& emitter,
                              arrow::MemoryPool* pool = nullptr);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_HTTP1_RESULT_ENCODER_H_
