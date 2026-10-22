// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <framework/interfaces/idataframe_snapshot.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace flowsql::catalog {

struct DataFrameTransferInfoV1 {
    std::string source_id;
    std::string transfer_id;
    std::string snapshot_id;
    std::string schema_revision;
    std::string captured_at;
    uint64_t row_count = 0;
    uint64_t ipc_bytes = 0;
    std::chrono::system_clock::time_point expires_at;
    std::shared_ptr<arrow::Schema> schema;
};

class DataFrameTransferStore {
 public:
    DataFrameTransferStore() = default;
    ~DataFrameTransferStore();

    int Init(const std::string& directory, std::chrono::seconds lease = std::chrono::seconds(60));
    int Acquire(IDataFrameSnapshotProviderV1& provider, const std::string& request_id,
                const DataFrameSnapshotRequestV1& request, DataFrameTransferInfoV1* output, std::string* error);
    bool Cancel(const std::string& request_id);
    bool Release(const std::string& transfer_id);
    void Stop();

 private:
    struct Transfer {
        std::string request_id;
        DataFrameSnapshotV1 snapshot;
        std::string path;
        uint64_t ipc_bytes = 0;
        std::chrono::steady_clock::time_point expires_at;
    };
    struct Pending {
        std::string source_id;
        uint64_t max_bytes = 0;
        std::shared_ptr<std::atomic_bool> cancelled;
    };

    void ExpireLoop();
    std::string directory_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    uint32_t active_acquires_ = 0;
    uint64_t reserved_bytes_ = 0;
    std::unordered_map<std::string, Pending> pending_;
    std::unordered_map<std::string, Transfer> transfers_;
    std::thread expiry_thread_;
    std::chrono::seconds lease_{60};
    int directory_lock_fd_ = -1;
};

}  // namespace flowsql::catalog
