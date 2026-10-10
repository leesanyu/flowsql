// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_SNAPSHOT_SOURCE_H_
#define FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_SNAPSHOT_SOURCE_H_
#include <framework/interfaces/idatabase_channel.h>

namespace flowsql {
const Guid IID_DATABASE_SNAPSHOT_SOURCE_V1 = {
    0x65bf71cd, 0x462a, 0x412f, {0xb8, 0x39, 0x61, 0x93, 0xda, 0xa8, 0x4c, 0x52}};
struct DatabaseSnapshotOptionsV1 {
    uint32_t struct_size = sizeof(DatabaseSnapshotOptionsV1);
    uint32_t contract_version = 1;
    bool consistent_transaction = true;
    uint32_t operation_timeout_ms = 5000;
};
// Task-private read-only physical session. Caller retains source lease and plugin ownership.
// Calls serialized except Cancel; Cancel never closes a handle under an in-flight call.
interface IDatabaseSnapshotSessionV1 {
    virtual ~IDatabaseSnapshotSessionV1() = default;
    // SELECT only, values bound, expected contains explicitly selected logical fields.
    // Query result <= max_rows and max_bytes. Empty batch retains schema; error output is empty.
    // The adapter quotes identifiers and validates physical types. No automatic reconnect/retry.
    virtual int ReadPage(const char* sql, const DatabaseParameterV1* parameters, size_t parameter_count,
                         std::shared_ptr<arrow::Schema> expected, uint32_t max_rows, uint64_t max_bytes,
                         std::shared_ptr<arrow::RecordBatch>* output) = 0;
    virtual void Cancel() = 0;
    virtual std::string LastError() const = 0;
};
interface IDatabaseSnapshotSourceV1 {
    virtual ~IDatabaseSnapshotSourceV1() = default;
    // New exclusive read session; failure output empty. Read-only transaction held until destruction.
    // ClickHouse rejects consistent_transaction; immutable scopes are proved by the caller.
    virtual int CreateSnapshotSession(const DatabaseSnapshotOptionsV1& options,
                                      std::shared_ptr<IDatabaseSnapshotSessionV1>* output) = 0;
};
}  // namespace flowsql
#endif
