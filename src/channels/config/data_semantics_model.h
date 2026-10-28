// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/idata_semantics_registry.h>
#include <rapidjson/document.h>
#include <cstdint>
#include <string>
#include <vector>

namespace flowsql::channels::config {
constexpr uint64_t kSemanticsMaxRevision = 9007199254740991ULL;
constexpr size_t kSemanticsMaxDocument = 8 * 1024 * 1024;
struct SemanticsReference {
    std::string target, path;
};
struct SemanticsIdentity {
    std::string kind, name;
    uint64_t revision = 0;
};
int SemanticsFail(DataSemanticsErrorV1* error, const std::string& code, const std::string& message,
                  const std::string& path = "");
bool SemanticsName(const std::string& name);
bool ParseSemanticsReference(const std::string& text, SemanticsIdentity* out);
std::string SemanticsJson(const rapidjson::Value& value);
int ParseSemanticsJson(const std::string& text, rapidjson::Document* out, DataSemanticsErrorV1* error);
int NormalizeSemantics(const std::string& kind, const std::string& content, rapidjson::Document* out,
                       std::vector<SemanticsReference>* references, DataSemanticsErrorV1* error);
int CheckSemanticsBinding(const rapidjson::Value& binding, const rapidjson::Value& dictionary,
                          std::string* structure_check, DataSemanticsErrorV1* error);
rapidjson::Document SemanticsGaps(const rapidjson::Value& value);
const char* SemanticsDocumentSchema();
}  // namespace flowsql::channels::config
