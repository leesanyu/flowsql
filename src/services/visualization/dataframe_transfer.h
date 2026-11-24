// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <arrow/api.h>
#include <httplib.h>

#include <arrow/io/file.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql::visualization {

struct DataFrameSourceInfo {
    std::string source_id;
    std::string label;
};

struct DataFrameSnapshotInfo {
    std::string source_id;
    std::string snapshot_id;
    std::string schema_revision;
    std::string captured_at;
    std::string semantic_context_json;
    uint64_t row_count = 0;
    uint64_t ipc_bytes = 0;
    int64_t expires_at_unix = 0;
    // Read-only mapping backing batch; declared first so it outlives every buffer view.
    std::shared_ptr<arrow::io::MemoryMappedFile> mapping;
    std::shared_ptr<arrow::RecordBatch> batch;
};

class DataFrameTransferClient {
 public:
    ~DataFrameTransferClient() { Stop(); }
    bool Init(std::string upstream, std::string shared_dir, uint32_t timeout_ms);
    bool Available() const;
    std::vector<DataFrameSourceInfo> Sources(std::string* error) const;
    bool Acquire(const std::string& request_id, const std::string& source_id, uint64_t max_bytes,
                 const std::shared_ptr<std::atomic_bool>& cancelled, uint32_t timeout_ms, DataFrameSnapshotInfo* output,
                 std::string* error);
    bool Lookup(const std::string& snapshot_id, std::shared_ptr<arrow::RecordBatch>* batch,
                DataFrameSnapshotInfo* meta);
    void StoreSemantics(const std::string& snapshot_id, std::string context_json);
    bool Release(const std::string& snapshot_id);
    bool Renew(const std::string& snapshot_id, int64_t* expires_at_unix);
    void Stop();

 private:
    struct Snapshot {
        DataFrameSnapshotInfo info;
        std::string transfer_id;
        std::chrono::steady_clock::time_point expires_at;
    };
    std::shared_ptr<httplib::Client> Client() const;
    void ExpireLoop();

    std::string upstream_;
    std::string shared_dir_;
    uint32_t timeout_ms_ = 30000;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::unordered_map<std::string, Snapshot> snapshots_;
    std::thread expiry_thread_;
};

}  // namespace flowsql::visualization
