// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/api.h>
#include <framework/interfaces/idatabase_atomic_target.h>
#include <atomic>
#include <unordered_map>
namespace flowsql::database {
// Driver-local optional capability. Does not change IDbDriver or legacy session vtables.
class AtomicTargetConfiguration {
 public:
    virtual ~AtomicTargetConfiguration() = default;
    std::unordered_map<std::string, std::string> atomic_parameters;
};
IBatchReader* MakeAtomicBufferedReader(std::shared_ptr<arrow::RecordBatch> batch,
                                       std::shared_ptr<std::atomic<uint32_t>> readers);
DatabaseAtomicStatusV1 MakeServerAtomicSession(const std::string& backend,
                                               const std::unordered_map<std::string, std::string>& parameters,
                                               const DatabaseAtomicSessionOptionsV1& options,
                                               std::shared_ptr<IDatabaseAtomicSessionV1>* output);
DatabaseAtomicStatusV1 MakeAtomicSession(const std::string& backend,
                                         const std::unordered_map<std::string, std::string>& parameters,
                                         const DatabaseAtomicSessionOptionsV1& options,
                                         std::shared_ptr<IDatabaseAtomicSessionV1>* output);
}  // namespace flowsql::database
