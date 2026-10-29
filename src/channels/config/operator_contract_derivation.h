// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/channel_dataset.hpp>
#include "data_semantics_registry.h"
namespace flowsql::channels::config {
// Pure metadata derivation. No row reads, algorithm execution, or persistence.
int DeriveOperatorOutputs(DataSemanticsRegistry& registry, const dataset::Value& contract,
                          const dataset::Value& invocation, const dataset::Value& role_map, dataset::Value* previews,
                          dataset::Alloc& allocator, DataSemanticsErrorV1* error);
}  // namespace flowsql::channels::config
