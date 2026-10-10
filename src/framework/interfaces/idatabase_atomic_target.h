// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_ATOMIC_TARGET_H_
#define FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_ATOMIC_TARGET_H_

#include <framework/interfaces/idatabase_channel.h>

#include <cstdint>
#include <memory>
#include <string>

namespace flowsql {
const Guid IID_DATABASE_ATOMIC_TARGET_V1 = {
    0x5a8d0e13, 0xb8c2, 0x44f6, {0xab, 0x43, 0x4c, 0x8f, 0x12, 0x94, 0x1b, 0x5a}};
constexpr uint32_t kDatabaseAtomicTargetVersionV1 = 1;

enum class DatabaseAtomicCodeV1 : int32_t {
    kOk = 0,
    kInvalidArgument,
    kUnsupported,
    kBusy,
    kCancelled,
    kTimeout,
    kError,
    kCommitUnknown,
};
struct DatabaseAtomicStatusV1 {
    DatabaseAtomicCodeV1 code = DatabaseAtomicCodeV1::kOk;
    std::string message;
    bool ok() const { return code == DatabaseAtomicCodeV1::kOk; }
};
enum class DatabaseCommitOutcomeV1 : int32_t { kCommitted, kRolledBack, kUnknown };
struct DatabaseCommitResultV1 {
    DatabaseAtomicStatusV1 status;
    DatabaseCommitOutcomeV1 outcome = DatabaseCommitOutcomeV1::kUnknown;
};
inline bool ValidDatabaseCommitResultV1(const DatabaseCommitResultV1& result) {
    if (result.outcome == DatabaseCommitOutcomeV1::kCommitted) return result.status.ok();
    if (result.outcome == DatabaseCommitOutcomeV1::kUnknown)
        return result.status.code == DatabaseAtomicCodeV1::kCommitUnknown;
    return result.outcome == DatabaseCommitOutcomeV1::kRolledBack && !result.status.ok() &&
           result.status.code != DatabaseAtomicCodeV1::kCommitUnknown;
}
struct DatabaseAtomicSessionOptionsV1 {
    uint32_t struct_size = sizeof(DatabaseAtomicSessionOptionsV1);
    uint32_t contract_version = kDatabaseAtomicTargetVersionV1;
    uint32_t operation_timeout_ms = 5000;
};
inline bool ValidDatabaseAtomicSessionOptionsV1(const DatabaseAtomicSessionOptionsV1& options) {
    return options.struct_size == sizeof(options) && options.contract_version == kDatabaseAtomicTargetVersionV1 &&
           options.operation_timeout_ms > 0 && options.operation_timeout_ms <= 3600000;
}

// Exclusive physical session, never an existing shared channel connection. No auto-reconnect/retry.
// Caller serializes all methods except Cancel. Every blocking method has the configured finite timeout.
// Factory/plugin/driver library and channel lease must outlive session, readers and returned products.
interface IDatabaseAtomicSessionV1 {
    virtual ~IDatabaseAtomicSessionV1() = default;
    // Idle -> transaction. May not nest; configured isolation must hide uncommitted publication rows.
    virtual DatabaseAtomicStatusV1 Begin() = 0;
    // Runs in the current transaction. All parameters/SQL borrowed only until return.
    // affected_rows is exact and zeroed on failure; enables task_key+writer_epoch+generation CAS.
    virtual DatabaseAtomicStatusV1 ExecutePrepared(const char* sql, const DatabaseParameterV1* parameters,
                                                   size_t parameter_count, uint64_t* affected_rows) = 0;
    // Task-private reader, same physical session/transaction. Close/Release it before Commit/Rollback.
    // IBatchReader::Cancel remains concurrent-safe; UINT64 is preserved without double narrowing.
    virtual DatabaseAtomicStatusV1 CreateReader(const char* sql, const DatabaseParameterV1* parameters,
                                                size_t parameter_count, IBatchReader** reader) = 0;
    // Database/server UTC epoch milliseconds, evaluated at call time (not transaction-start time).
    // Business storage uses this for conditional writer acquisition/renewal, not the host clock.
    virtual DatabaseAtomicStatusV1 ReadDatabaseTime(int64_t * unix_ms) = 0;
    // Exactly one outcome. kUnknown means the server may have committed: poison session, terminate
    // task, never automatically retry/rollback-and-claim-success. Restart inspects durable generation.
    virtual DatabaseCommitResultV1 Commit() = 0;
    // Only uncommitted active transaction. A known successful rollback returns kOk.
    // Failure cannot make an unknown Commit known; discard session and preserve last confirmed state.
    virtual DatabaseAtomicStatusV1 Rollback() = 0;
    // Concurrent-safe signal/wakeup only; permanently cancels this session. No model/container access.
    // An interrupted Commit still reports unknown when its effect cannot be proved.
    virtual void Cancel() = 0;
    // No in-flight calls/readers. Roll back active uncommitted work and close physical session.
    // Idempotent; no implicit commit. Destructor provides the same cleanup; cannot claim unknown effect.
    virtual void Close() = 0;
};

// Independent optional channel capability: SQLite/MySQL/PostgreSQL implement later; ClickHouse rejects.
interface IDatabaseAtomicTargetV1 {
    virtual ~IDatabaseAtomicTargetV1() = default;
    // Validate size/version; output reset on failure. A fresh exclusively owned session per request.
    // Its deleter retains the driver allocation domain; caller retains provider/library ownership.
    virtual DatabaseAtomicStatusV1 AcquireAtomicSession(const DatabaseAtomicSessionOptionsV1& options,
                                                        std::shared_ptr<IDatabaseAtomicSessionV1>* session) = 0;
};
}  // namespace flowsql
#endif
