// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include <framework/interfaces/idatabase_snapshot_source.h>

#include <variant>

namespace flowsql {

// Preserve native values, including exact decimals and integers outside JavaScript's safe range.
struct ExplorationDecimalV1 {
    std::string text;
};
using ExplorationValueV1 =
    std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>, ExplorationDecimalV1>;
struct ExplorationColumnV1 {
    std::string name;
    std::string native_type;
};
struct ExplorationResultV1 {
    std::vector<ExplorationColumnV1> columns;
    std::vector<std::vector<ExplorationValueV1>> rows;
};

// Calls serialized except Cancel. Caller retains the channel lease and provider library.
interface IDatabaseExplorationSessionV1 {
    virtual ~IDatabaseExplorationSessionV1() = default;
    // Single SELECT, bound values, bounded result. Failure clears output.
    virtual int Query(const char* sql, const DatabaseParameterV1* parameters, size_t parameter_count, uint32_t max_rows,
                      uint64_t max_bytes, ExplorationResultV1* output) = 0;
    virtual void Cancel() = 0;
    virtual std::string QueryError() const = 0;
};
interface IDatabaseExplorationSourceV1 {
    virtual ~IDatabaseExplorationSourceV1() = default;
    // An exclusive session in a read-only transaction; destroy it before its source lease.
    virtual int CreateExplorationSession(const DatabaseSnapshotOptionsV1& options,
                                         std::shared_ptr<IDatabaseExplorationSessionV1>* output) = 0;
};
}  // namespace flowsql
