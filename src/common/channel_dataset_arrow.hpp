// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/type.h>
#include <set>
#include "channel_dataset.hpp"

namespace flowsql::dataset {
inline Value ArrowFields(const arrow::Schema& schema, Alloc& a) {
    Value fields(rapidjson::kArrayType);
    std::set<std::string> names;
    for (const auto& f : schema.fields()) {
        if (f->name().empty() || !names.insert(f->name()).second)
            throw std::invalid_argument("DUPLICATE_OR_EMPTY_FIELD");
        // Arrow's canonical type string includes nested fields, precision, scale, time unit and timezone.
        fields.PushBack(Field(f->name(), Type("arrow", f->type()->ToString(), a), f->nullable(), a), a);
    }
    return fields;
}
}  // namespace flowsql::dataset
