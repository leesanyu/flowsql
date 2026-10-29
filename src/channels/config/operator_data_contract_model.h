// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include "data_semantics_model.h"
namespace flowsql::channels::config {
int NormalizeOperatorDocument(const std::string& kind, rapidjson::Document* document,
                              std::vector<SemanticsReference>* references, DataSemanticsErrorV1* error);
// Validate the documented finite JSON Schema vocabulary without loading external resources.
int ValidateOperatorConfiguration(const rapidjson::Value& schema, const rapidjson::Value& instance,
                                  const rapidjson::Value& contract, DataSemanticsErrorV1* error);
int ValidateOperatorParameters(const rapidjson::Value& contract, const rapidjson::Value& parameters,
                               DataSemanticsErrorV1* error);
}  // namespace flowsql::channels::config
