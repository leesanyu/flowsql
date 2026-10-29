// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/iplugin.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include "baseliner_contract.h"

namespace flowsql::baseliner {
// A task supplies its own immutable snapshot; a provider resolves the request
// through ResolveConfig. Source inspection uses metadata only, never business rows.
int ResolveContractInvocation(IQuerier* querier, const ConfigSnapshot* frozen, const char* request_json,
                              std::string* result_json, DataSemanticsErrorV1* error);
}  // namespace flowsql::baseliner
