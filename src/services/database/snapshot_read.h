// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_DATABASE_SNAPSHOT_READ_H_
#define FLOWSQL_DATABASE_SNAPSHOT_READ_H_
#include <arrow/api.h>
#include <framework/interfaces/idatabase_snapshot_source.h>

#include <atomic>
#include <chrono>
#include <optional>

namespace flowsql::database {
class IDbSnapshotSession {
 public:
    virtual ~IDbSnapshotSession() = default;
    virtual int BeginSnapshot(const DatabaseSnapshotOptionsV1& options) = 0;
    virtual int SnapshotPage(const char* sql, const DatabaseParameterV1* parameters, size_t count,
                             std::shared_ptr<arrow::Schema> expected, uint32_t rows, uint64_t bytes,
                             std::shared_ptr<arrow::RecordBatch>* output) = 0;
    virtual void CancelSnapshot() = 0;

 protected:
    std::atomic<bool> snapshot_cancelled_{false};
    uint32_t snapshot_timeout_ms_ = 5000;
    bool snapshot_transaction_ = false;
};
using SnapshotCell = std::optional<std::string>;
using SnapshotRows = std::vector<std::vector<SnapshotCell>>;
// Parses exact full-domain integers; rejects malformed or incompatible conversions; metrics interpret nonfinite
// doubles.
int MakeSnapshotBatch(const std::shared_ptr<arrow::Schema>& schema, const SnapshotRows& rows, uint64_t max_bytes,
                      std::shared_ptr<arrow::RecordBatch>* output, std::string* error);
bool SnapshotPhysicalType(const std::string& backend, const std::string& physical, arrow::Type::type logical);
}  // namespace flowsql::database
#endif
