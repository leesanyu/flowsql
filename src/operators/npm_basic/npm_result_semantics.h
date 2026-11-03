// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/interfaces/ioperator_contract_catalog.h>
#include <common/iquerier.hpp>

namespace flowsql::npm {
// Called only after this managed run created/verified its actual public objects.
// Returns stable execution_context.npm data; it neither reads analysis rows nor
// infers ownership from relation names. Outputs are cleared on failure.
__attribute__((visibility("default"))) int PublishNpmResultSemantics(
    IQuerier* querier, const OperatorComponentContractV1& component, const std::string& category,
    const std::string& name, const std::string& input_namespace, const std::string& actual_result_json,
    std::string* context_json, DataSemanticsErrorV1* error);
}  // namespace flowsql::npm
