// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_ATOMIC_TARGET_H_
#define FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_ATOMIC_TARGET_H_

#include <framework/interfaces/idatabase_channel.h>

#include <cstdint>
#include <memory>
#include <string>

namespace flowsql {
const Guid IID_DATABASE_ATOMIC_TARGET = {0x5a8d0e13, 0xb8c2, 0x44f6, {0xab, 0x43, 0x4c, 0x8f, 0x12, 0x94, 0x1b, 0x5a}};
constexpr uint32_t kDatabaseAtomicTargetVersion = 1;

enum class DatabaseAtomicCode : int32_t {
    kOk = 0,
    kInvalidArgument,
    kUnsupported,
    kBusy,
    kCancelled,
    kTimeout,
    kError,
    kCommitUnknown,
};
struct DatabaseAtomicStatus {
    DatabaseAtomicCode code = DatabaseAtomicCode::kOk;
    std::string message;
    bool ok() const { return code == DatabaseAtomicCode::kOk; }
};
enum class DatabaseCommitOutcome : int32_t { kCommitted, kRolledBack, kUnknown };
struct DatabaseCommitResult {
    DatabaseAtomicStatus status;
    DatabaseCommitOutcome outcome = DatabaseCommitOutcome::kUnknown;
};
inline bool ValidDatabaseCommitResult(const DatabaseCommitResult& result) {
    if (result.outcome == DatabaseCommitOutcome::kCommitted) return result.status.ok();
    if (result.outcome == DatabaseCommitOutcome::kUnknown)
        return result.status.code == DatabaseAtomicCode::kCommitUnknown;
    return result.outcome == DatabaseCommitOutcome::kRolledBack && !result.status.ok() &&
           result.status.code != DatabaseAtomicCode::kCommitUnknown;
}
struct DatabaseAtomicSessionOptions {
    uint32_t struct_size = sizeof(DatabaseAtomicSessionOptions);
    uint32_t contract_version = kDatabaseAtomicTargetVersion;
    uint32_t operation_timeout_ms = 5000;
};
inline bool ValidDatabaseAtomicSessionOptions(const DatabaseAtomicSessionOptions& options) {
    return options.struct_size == sizeof(options) && options.contract_version == kDatabaseAtomicTargetVersion &&
           options.operation_timeout_ms > 0 && options.operation_timeout_ms <= 3600000;
}

// Exclusive physical session, never an existing shared channel connection. No auto-reconnect/retry.
// Caller serializes all methods except Cancel. Every blocking method has the configured finite timeout.
// Factory/plugin/driver library and channel lease must outlive session, readers and returned products.
interface IDatabaseAtomicSession {
    virtual ~IDatabaseAtomicSession() = default;
    // Idle -> transaction. May not nest; configured isolation must hide uncommitted publication rows.
    virtual DatabaseAtomicStatus Begin() = 0;
    // Runs in the current transaction. All parameters/SQL borrowed only until return.
    // affected_rows is exact and zeroed on failure; enables task_key+writer_epoch+generation CAS.
    virtual DatabaseAtomicStatus ExecutePrepared(const char* sql, const DatabaseParameter* parameters,
                                                 size_t parameter_count, uint64_t* affected_rows) = 0;
    // Task-private reader, same physical session/transaction. Close/Release it before Commit/Rollback.
    // IBatchReader::Cancel remains concurrent-safe; UINT64 is preserved without double narrowing.
    virtual DatabaseAtomicStatus CreateReader(const char* sql, const DatabaseParameter* parameters,
                                              size_t parameter_count, IBatchReader** reader) = 0;
    // Database/server UTC epoch milliseconds, evaluated at call time (not transaction-start time).
    // Business storage uses this for conditional writer acquisition/renewal, not the host clock.
    virtual DatabaseAtomicStatus ReadDatabaseTime(int64_t * unix_ms) = 0;
    // Exactly one outcome. kUnknown means the server may have committed: poison session, terminate
    // task, never automatically retry/rollback-and-claim-success. Restart inspects durable generation.
    virtual DatabaseCommitResult Commit() = 0;
    // Only uncommitted active transaction. A known successful rollback returns kOk.
    // Failure cannot make an unknown Commit known; discard session and preserve last confirmed state.
    virtual DatabaseAtomicStatus Rollback() = 0;
    // Concurrent-safe signal/wakeup only; permanently cancels this session. No model/container access.
    // An interrupted Commit still reports unknown when its effect cannot be proved.
    virtual void Cancel() = 0;
    // No in-flight calls/readers. Roll back active uncommitted work and close physical session.
    // Idempotent; no implicit commit. Destructor provides the same cleanup; cannot claim unknown effect.
    virtual void Close() = 0;
};

// Independent optional channel capability: SQLite/MySQL/PostgreSQL implement later; ClickHouse rejects.
interface IDatabaseAtomicTarget {
    virtual ~IDatabaseAtomicTarget() = default;
    // Validate size/version; output reset on failure. A fresh exclusively owned session per request.
    // Its deleter retains the driver allocation domain; caller retains provider/library ownership.
    virtual DatabaseAtomicStatus AcquireAtomicSession(const DatabaseAtomicSessionOptions& options,
                                                      std::shared_ptr<IDatabaseAtomicSession>* session) = 0;
};
}  // namespace flowsql
#endif
