// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <sqlite3.h>
#include <common/channel_dataset.hpp>

#include <mutex>

namespace flowsql::access {
// All records are owned by the authenticated principal. Registration precedes any
// execution; result publication and completion commit in one SQLite transaction.
class AccessStore {
 public:
    explicit AccessStore(const std::string& path);
    ~AccessStore();
    dataset::Json Register(const std::string& owner, const std::string& request, const std::string& digest,
                           const std::string& kind, bool* created);
    dataset::Json Find(const std::string& owner, const std::string& id, const std::string& request = "");
    void Update(const std::string& owner, const dataset::Value& operation);
    dataset::Json Outputs(const std::string& owner);
    dataset::Json Complete(const std::string& owner, const std::string& id, const dataset::Value& payload,
                           uint64_t ttl_ms);
    dataset::Json Read(const std::string& owner, const std::string& result, uint64_t offset, uint32_t limit,
                       const std::string& lease = "");
    dataset::Json AcquireLease(const std::string& owner, const std::string& result, uint64_t duration_ms);
    dataset::Json ReleaseLease(const std::string& owner, const std::string& lease);

 private:
    dataset::Json FindLocked(const std::string& owner, const std::string& id, const std::string& request);
    void UpdateLocked(const std::string& owner, const dataset::Value& operation);
    void Cleanup();
    sqlite3* db_ = nullptr;
    int lock_ = -1;
    std::mutex mutex_;
};
}  // namespace flowsql::access
