// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/type.h>
#include <algorithm>
#include <cctype>
#include <string>

namespace flowsql {
// The production snapshot reader and metadata contract adapter share this exact
// physical-to-requested-type rule. Value/range validation still occurs on read.
inline bool DatabaseSnapshotPhysicalType(const std::string& backend, const std::string& physical,
                                         arrow::Type::type logical) {
    std::string p = physical;
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return std::tolower(c); });
    bool integer = p.find("int") != std::string::npos;
    bool number = integer || p.find("numeric") != std::string::npos || p.find("decimal") != std::string::npos;
    bool real = p.find("float") != std::string::npos || p.find("double") != std::string::npos || p == "real";
    bool text = p.find("text") != std::string::npos || p.find("char") != std::string::npos ||
                p.find("string") != std::string::npos;
    switch (logical) {
        case arrow::Type::INT64:
            return integer && p.find("unsigned") == std::string::npos && p.find("uint") == std::string::npos;
        case arrow::Type::UINT64:
            return number || (backend == "sqlite" && text);
        case arrow::Type::DOUBLE:
            return number || real;
        case arrow::Type::BOOL:
            return p.find("bool") != std::string::npos || integer;
        case arrow::Type::STRING:
            return text;
        default:
            return false;
    }
}
}  // namespace flowsql
