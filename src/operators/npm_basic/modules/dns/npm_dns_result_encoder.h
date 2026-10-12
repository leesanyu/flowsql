// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_DNS_RESULT_ENCODER_H_
#define FLOWSQL_NPM_DNS_RESULT_ENCODER_H_

#include <operators/npm_basic/npm_family_result_contract.h>

#include <memory>
#include <string>

namespace arrow {
class MemoryPool;
}

namespace flowsql::npm {

/** Owned session facts retained for DNS rows that outlive the borrowed session view. */

/** Builds and emits one final event row; the temporary module-state charge spans the emitter call. */
template <typename Address>
int EmitNpmDnsTransactionV1(const NpmDnsFamilyResult<Address>& family_result,
                            const std::shared_ptr<INpmTaskBudget>& budget, INpmResultEmitterV1& emitter,
                            arrow::MemoryPool* pool = nullptr);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_DNS_RESULT_ENCODER_H_
