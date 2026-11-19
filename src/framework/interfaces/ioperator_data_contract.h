// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/interfaces/idata_semantics_registry.h>
#include <memory>
#include <string>

namespace flowsql {
inline const Guid IID_OPERATOR_DATA_CONTRACT = {
    0x732bba45, 0x819a, 0x442e, {0x8d, 0x52, 0x5d, 0xa8, 0x84, 0xa9, 0xe1, 0x10}};
inline const Guid IID_OPERATOR_CONFIG_ADAPTER = {
    0xafc9b742, 0x48c1, 0x4f0c, {0xb3, 0x56, 0x60, 0xd0, 0x18, 0x0b, 0x36, 0xf5}};

struct OperatorContractSnapshot {
    std::string exact_reference;
    std::string sha256;
    std::shared_ptr<const std::string> document_json;
    std::shared_ptr<const rapidjson::Document> document;
};

// Methods are thread safe, return zero on success, and clear outputs on failure.
// Borrowed arguments live for the call only; returned snapshots own their JSON.
interface IOperatorDataContract {
    virtual ~IOperatorDataContract() = default;
    virtual int Resolve(const char* exact_reference, OperatorContractSnapshot* out, DataSemanticsError* error) = 0;
    virtual int CheckAndDerive(const char* request_json, std::string* result_json, DataSemanticsError* error) = 0;
    // Trusted execution path only. Generic save/import must not produce observations.
    virtual int RecordUse(const char* observation_json, OperatorContractSnapshot* out, DataSemanticsError* error) = 0;
};

// Optional capability independent of all execution ABIs. The component owns business
// parsing/defaults; the caller freezes the response for both checking and execution.
// Request: {operator:{category,name,contract_ref?,component_revision?},parameters,inputs,execution_context}.
// Trusted execution supplies the frozen component identity; a task adapter may
// retain that identity and report actual source/runtime facts after Open.
// Response: {effective_parameters,normalized_config?,config_refs,inputs,execution_context?}.
// inputs retains port/item identity, actual object/source revision, and adaptation facts.
// No algorithm execution, business-row loading, mutation, credentials, or latest refs.
interface IOperatorConfigAdapter {
    virtual ~IOperatorConfigAdapter() = default;
    virtual int ResolveInvocation(const char* request_json, std::string* result_json, DataSemanticsError* error) = 0;
};
}  // namespace flowsql
