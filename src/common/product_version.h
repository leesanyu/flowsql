// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <optional>
#include <string>

#include "flowsql_build_info.h"

namespace flowsql {

struct ProductBuildInfo {
    std::string version;
    std::optional<std::string> revision;
    std::optional<bool> dirty;
};

// Internal linkage keeps each loaded plugin's identity local to that artifact.
[[gnu::used]] static const char kProductBuildRecord[] = FLOWSQL_PRODUCT_RECORD;

static inline const ProductBuildInfo& GetProductBuildInfo() noexcept {
    static const ProductBuildInfo info{
        FLOWSQL_PRODUCT_VERSION,
        std::string(FLOWSQL_PRODUCT_REVISION).empty() ? std::nullopt
                                                      : std::optional<std::string>(FLOWSQL_PRODUCT_REVISION),
        FLOWSQL_PRODUCT_DIRTY < 0 ? std::nullopt : std::optional<bool>(FLOWSQL_PRODUCT_DIRTY != 0)};
    return info;
}

static inline std::string ProductBuildInfoJson() {
    const std::string record(kProductBuildRecord);
    constexpr size_t begin = sizeof("FLOWSQL_BUILD_INFO_BEGIN") - 1;
    constexpr size_t end = sizeof("FLOWSQL_BUILD_INFO_END") - 1;
    return record.substr(begin, record.size() - begin - end);
}

static inline std::string ProductVersionText() {
    const auto& info = GetProductBuildInfo();
    return "FlowSQL " + info.version + "\nrevision=" + info.revision.value_or("unknown") +
           "\ndirty=" + (info.dirty ? (*info.dirty ? "true" : "false") : "unknown") + "\n";
}

}  // namespace flowsql
