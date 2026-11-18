// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include <framework/interfaces/idatabase_snapshot_source.h>

#include <variant>

namespace flowsql {

// Preserve native values, including exact decimals and integers outside JavaScript's safe range.
struct ExplorationDecimal {
    std::string text;
};
using ExplorationValue =
    std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>, ExplorationDecimal>;
struct ExplorationColumn {
    std::string name;
    std::string native_type;
};
struct ExplorationResult {
    std::vector<ExplorationColumn> columns;
    std::vector<std::vector<ExplorationValue>> rows;
};

// Calls serialized except Cancel. Caller retains the channel lease and provider library.
interface IDatabaseExplorationSession {
    virtual ~IDatabaseExplorationSession() = default;
    // Single SELECT, bound values, bounded result. Failure clears output.
    virtual int Query(const char* sql, const DatabaseParameter* parameters, size_t parameter_count, uint32_t max_rows,
                      uint64_t max_bytes, ExplorationResult* output) = 0;
    virtual void Cancel() = 0;
    virtual std::string QueryError() const = 0;
};
interface IDatabaseExplorationSource {
    virtual ~IDatabaseExplorationSource() = default;
    // An exclusive session in a read-only transaction; destroy it before its source lease.
    virtual int CreateExplorationSession(const DatabaseSnapshotOptions& options,
                                         std::shared_ptr<IDatabaseExplorationSession>* output) = 0;
};
}  // namespace flowsql
