// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include <framework/interfaces/idatabase_factory.h>
#include <httplib.h>
#include <rapidjson/document.h>
#include <common/iquerier.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "dataframe_transfer.h"
#include "workflow.h"

namespace flowsql::visualization {
struct QueryLimits {
    uint32_t max_rows = 10000;
    uint64_t max_bytes = 8 * 1024 * 1024;
    uint32_t timeout_ms = 30000;
    uint32_t max_active = 2;
};
class ExplorationService {
 public:
    bool Init(IQuerier* querier, QueryLimits limits, std::string dataframe_upstream = {},
              std::string dataframe_shared_dir = {}, std::string semantics_upstream = {});
    DataFrameTransferClient& DataFrames() { return dataframes_; }
    void Start();
    void Routes(httplib::Server& server);
    void Stop();

 private:
    struct Source {
        std::string id, channel, table;
        std::string dialect = "sqlite", schema;
        // DataFrame sources carry identity only; the batch lives in the transfer client.
        bool dataframe = false;
        std::string label;
    };
    struct Active {
        std::shared_ptr<std::atomic_bool> acquire_cancel;
        std::shared_ptr<IDatabaseChannel> lease;
        std::shared_ptr<IDatabaseExplorationSession> session;
        std::chrono::steady_clock::time_point deadline;
        bool cancelled = false;
        bool timeout = false;
    };
    std::shared_ptr<IDatabaseChannel> Lease(const std::string& dialect, const std::string& channel);
    std::shared_ptr<IDatabaseExplorationSession> Session(const std::shared_ptr<IDatabaseChannel>& lease);
    ExplorationResult Query(IDatabaseExplorationSession& session, const std::string& sql,
                            const std::vector<ExplorationValue>& values = {});
    std::vector<Source> Sources();
    Source FindSource(const std::string& id);
    SourceSchema Schema(IDatabaseExplorationSession& session, const Source& source);
    void Compute(const httplib::Request& request, httplib::Response& response);
    void AcquireSnapshot(const httplib::Request& request, httplib::Response& response);
    void ComputeDataFrame(const httplib::Request& request, const rapidjson::Document& document,
                          httplib::Response& response);
    std::vector<Source> DataFrameSources(std::string* error) const;
    IDatabaseFactory* factory_ = nullptr;
    IDatabaseChannelLeaseProvider* leases_ = nullptr;
    QueryLimits limits_;
    std::mutex mutex_;
    bool stopping_ = false;
    std::unordered_map<std::string, Active> active_;
    std::unordered_map<std::string, Source> sources_;
    size_t sessions_count_ = 0;
    std::vector<std::weak_ptr<IDatabaseExplorationSession>> sessions_;
    std::condition_variable wake_;
    std::thread monitor_;
    std::mutex dataframe_mutex_;
    DataFrameTransferClient dataframes_;
    std::string semantics_upstream_;
    std::string SemanticTarget(const Source& source) const;
};
}  // namespace flowsql::visualization
