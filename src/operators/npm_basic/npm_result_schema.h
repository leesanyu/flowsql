// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/api.h>
#include <algorithm>
#include <sstream>

namespace flowsql::npm {
inline std::string MetadataCanonical(const std::shared_ptr<const arrow::KeyValueMetadata>& metadata) {
    if (!metadata) return "0:";
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(metadata->size());
    for (int index = 0; index < metadata->size(); ++index) {
        entries.emplace_back(metadata->key(index), metadata->value(index));
    }
    std::sort(entries.begin(), entries.end());
    std::ostringstream canonical;
    canonical << entries.size() << ':';
    for (const auto& [key, value] : entries) {
        canonical << key.size() << ':' << key << value.size() << ':' << value;
    }
    return canonical.str();
}

inline std::string SchemaCanonical(const arrow::Schema& schema) {
    std::ostringstream canonical;
    canonical << "fields=" << schema.num_fields() << ';';
    for (const auto& field : schema.fields()) {
        canonical << field->name().size() << ':' << field->name() << ';' << field->type()->ToString().size() << ':'
                  << field->type()->ToString() << ';' << (field->nullable() ? '1' : '0') << ';'
                  << MetadataCanonical(field->metadata()) << ';';
    }
    canonical << "schema_metadata=" << MetadataCanonical(schema.metadata());
    return canonical.str();
}

}  // namespace flowsql::npm
