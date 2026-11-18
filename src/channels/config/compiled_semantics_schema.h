// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <rapidjson/document.h>
#include <rapidjson/schema.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "data_semantics_api_schema.h"

namespace flowsql::channels::config {
// Definition names come from the registered control operations, not request payloads.
inline const rapidjson::SchemaDocument& CompiledSemanticsApiSchema(const std::string& definition) {
    static std::mutex mutex;
    static std::map<std::string, std::unique_ptr<const rapidjson::SchemaDocument>> schemas;
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = schemas.find(definition);
    if (found != schemas.end()) return *found->second;
    rapidjson::Document source;
    source.Parse(kSemanticsApiSchema);
    const auto reference = "#/%24defs/" + definition;
    auto& allocator = source.GetAllocator();
    source.AddMember("$ref", rapidjson::Value(reference.c_str(), allocator), allocator);
    auto compiled = std::make_unique<const rapidjson::SchemaDocument>(source);
    return *schemas.emplace(definition, std::move(compiled)).first->second;
}
}  // namespace flowsql::channels::config
