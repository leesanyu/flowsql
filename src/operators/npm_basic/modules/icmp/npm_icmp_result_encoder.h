// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_ICMP_RESULT_ENCODER_H_
#define FLOWSQL_NPM_ICMP_RESULT_ENCODER_H_

#include "npm_icmp_contract.h"

namespace arrow {
class MemoryPool;
}

namespace flowsql::npm {

template <typename Address>
int EmitNpmIcmpEventV1(const NpmIcmpFamilyEvent<Address>& event, const std::shared_ptr<INpmTaskBudget>& budget,
                       INpmResultEmitterV1& emitter, arrow::MemoryPool* pool = nullptr);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_ICMP_RESULT_ENCODER_H_
