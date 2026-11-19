// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <common/guid.h>
#include <common/typedef.h>
#include <rapidjson/document.h>
#include <memory>
#include <string>
#include <vector>

namespace flowsql {
const Guid IID_DATA_SEMANTICS_REGISTRY = {0x57b13e82, 0x05ab, 0x451c, {0xa6, 0xf1, 0x33, 0x98, 0x81, 0x70, 0x41, 0x2d}};
struct DataSemanticsError {
    std::string code, message, field_path;
};
struct DataSemanticsSnapshot {
    std::string exact_reference, schema_id, sha256;
    std::shared_ptr<const std::string> document_json;
    // Immutable, owned DOM of the same verified exact revision. Copy before editing.
    std::shared_ptr<const rapidjson::Document> document;
    size_t document_bytes = 0;
    std::shared_ptr<const std::vector<std::string>> references;
    // Normalized dictionaries under the same /resources/<key> paths, owned and budgeted with this revision.
    std::shared_ptr<const rapidjson::Document> dictionary_resources;
};
interface IDataSemanticsRegistry {
    virtual ~IDataSemanticsRegistry() = default;
    virtual int Resolve(const char* exact_reference, DataSemanticsSnapshot* out, DataSemanticsError* error) = 0;
};
}  // namespace flowsql
