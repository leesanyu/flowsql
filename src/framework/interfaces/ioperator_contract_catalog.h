// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/interfaces/idata_semantics_registry.h>
#include <vector>

namespace flowsql {
inline const Guid IID_OPERATOR_CONTRACT_CATALOG = {
    0xb7595180, 0x701c, 0x492a, {0xbc, 0xa2, 0x5e, 0xe2, 0x3a, 0x4f, 0xd2, 0x70}};

struct OperatorComponentContract {
    std::string category, name, component_revision, operator_version, contract_ref;
    std::string contract_state = "missing";
};
struct OperatorContractDeclaration {
    std::string category, name;
    // Host-observed SHA256, never an author-supplied version or file name.
    std::string component_revision;
    // Empty means undeclared; otherwise a document or {"contract_ref": exact_reference}.
    std::string declaration_json;
    // Empty means no previous association; all commits use compare-and-swap.
    std::string expected_component_revision;
};
struct PreparedOperatorContract {
    OperatorComponentContract association;
    std::string expected_component_revision;
};

// Independent of IOperatorCatalog/OperatorMeta and every execution ABI.
// Prepare validates ALL declarations before publishing any definition. It does not
// switch associations. Commit switches the entire batch atomically, without callbacks.
// Definition persistence and component activation are separate observable operations.
// Errors clear outputs. All returned strings are owned and methods are thread safe.
interface IOperatorContractCatalog {
    virtual ~IOperatorContractCatalog() = default;
    virtual int Prepare(const std::vector<OperatorContractDeclaration>& declarations,
                        std::vector<PreparedOperatorContract>* prepared, DataSemanticsError* error) = 0;
    virtual int Commit(const std::vector<PreparedOperatorContract>& prepared, DataSemanticsError* error) = 0;
    virtual int Lookup(const char* category, const char* name, OperatorComponentContract* out,
                       DataSemanticsError* error) = 0;
};

// Optional instance capability. A proxy/lease returns the identity captured from
// its own component, so a later catalog refresh cannot change an in-flight basis.
interface IOperatorContractIdentity {
    virtual ~IOperatorContractIdentity() = default;
    virtual OperatorComponentContract ContractIdentity() const = 0;
};
}  // namespace flowsql
