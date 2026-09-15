// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ICPP_OPERATOR_PLUGIN_REGISTRY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ICPP_OPERATOR_PLUGIN_REGISTRY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <memory>

namespace flowsql {

/**
 * Acquired capability plus the owning lifetime token supplied by the C++
 * operator plugin host. capability is non-owning and remains valid only while
 * lifetime is retained. The token keeps the capability, its provider, and the
 * shared-library handle alive as one unit.
 */
struct CppOperatorCapabilityLeaseV1 {
    void* capability = nullptr;
    std::shared_ptr<void> lifetime;
};

// {0x668b58d2-3735-4c89-9b45-5dea776c01a7}
const Guid IID_CPP_OPERATOR_PLUGIN_REGISTRY_V1 = {
    0x668b58d2, 0x3735, 0x4c89, {0x9b, 0x45, 0x5d, 0xea, 0x77, 0x6c, 0x01, 0xa7}};

/** Runtime registry for capabilities exported by activated C++ operator plugins. */
interface ICppOperatorPluginRegistryV1 {
    virtual ~ICppOperatorPluginRegistryV1() = default;

    /**
     * Acquires the unique capability matching category.name and contract_iid.
     * On success, both lease fields are non-null. On any nonzero return, the
     * implementation must reset the output lease to its empty state.
     */
    virtual int Acquire(const char* category, const char* name, const Guid& contract_iid,
                        CppOperatorCapabilityLeaseV1* lease) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ICPP_OPERATOR_PLUGIN_REGISTRY_H_
