// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/interfaces/icpp_operator_plugin_registry.h>
#include <framework/interfaces/ioperator_contract_catalog.h>

namespace flowsql {
inline const Guid IID_CPP_OPERATOR_CONTRACT_PROVIDER_V1 = {
    0x12ae1f04, 0x286b, 0x4eb1, {0xac, 0x31, 0x61, 0x62, 0x22, 0xd9, 0xb8, 0x40}};

// Independent optional host capability. Both outputs refer to the same loaded
// component, even after its catalog association changes. The unchanged lease
// keeps the capability and its shared library alive. Errors clear both outputs.
interface ICppOperatorContractProviderV1 {
    virtual ~ICppOperatorContractProviderV1() = default;
    virtual int AcquireContract(const char* category, const char* name, const Guid& execution_iid,
                                CppOperatorCapabilityLeaseV1* lease, OperatorComponentContractV1* component) = 0;
};
}  // namespace flowsql
