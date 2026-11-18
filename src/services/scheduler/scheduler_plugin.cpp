// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <common/error_code.h>
#include <common/log.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <regex>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_set>

#include "framework/core/channel_adapter.h"
#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/fan_in_stream_channel.h"
#include "framework/core/fan_out_stream_channel.h"
#include "framework/core/json_error_builder.h"
#include "framework/core/pipeline.h"
#include "framework/core/ring_stream_channel.h"
#include "framework/core/sql_parser.h"
#include "framework/core/sql_text_splitter.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ichannel_registry.h"
#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idatabase_factory.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/ibuiltin_registry.h"
#include "framework/interfaces/ibridge.h"
#include "framework/interfaces/ioperator.h"
#include "framework/interfaces/ioperator_catalog.h"
#include "framework/interfaces/ioperator_registry.h"
#include "framework/interfaces/istream_channel.h"
#include "framework/interfaces/istream_factory.h"
#include "framework/interfaces/istream_manager.h"
#include "scheduler_json_codec.h"
#include "scheduler_internal_utils.h"

namespace flowsql {
namespace scheduler {

int SchedulerPlugin::Option(const char* arg) {
    if (!arg) return 0;

    std::string opts(arg);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();

        std::string key = opts.substr(pos, eq - pos);
        std::string val = opts.substr(eq + 1, end - eq - 1);

        if (key == "host") host_ = val;
        else if (key == "port") port_ = std::stoi(val);
        else if (key == "stream_workers") stream_worker_count_ = static_cast<size_t>(std::stoull(val));
        else if (key == "batch_workers") batch_worker_count_ = static_cast<size_t>(std::stoull(val));
        else if (key == "max_resolved_sources") {
            const size_t parsed = static_cast<size_t>(std::stoull(val));
            max_resolved_sources_ = std::max<size_t>(1, parsed);
        } else if (key == "max_stream_group_timeout_s") {
            const int parsed = std::stoi(val);
            max_stream_group_timeout_s_ = std::max(1, parsed);
        } else if (key == "stream_runtime_retention_s") {
            const int parsed = std::stoi(val);
            stream_runtime_retention_s_ = std::max(0, parsed);
        } else if (key == "stream_runtime_max_count") {
            const size_t parsed = static_cast<size_t>(std::stoull(val));
            stream_runtime_max_count_ = std::max<size_t>(1, parsed);
        } else if (key == "max_shared_hubs") {
            const size_t parsed = static_cast<size_t>(std::stoull(val));
            max_shared_hubs_ = std::max<size_t>(1, parsed);
        } else if (key == "max_subscribers_per_hub") {
            const size_t parsed = static_cast<size_t>(std::stoull(val));
            max_subscribers_per_hub_ = std::max<size_t>(1, parsed);
        } else if (key == "shared_subscriber_queue_size") {
            const size_t parsed = static_cast<size_t>(std::stoull(val));
            shared_subscriber_queue_size_ = std::max<size_t>(64, parsed);
        } else if (key == "shared_hub_poll_timeout_ms") {
            const int parsed = std::stoi(val);
            shared_hub_poll_timeout_ms_ = std::max(1, parsed);
        }

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int SchedulerPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    LOG_INFO("SchedulerPlugin::Load: host=%s, port=%d", host_.c_str(), port_);
    return 0;
}

int SchedulerPlugin::Unload() {
    return 0;
}

// --- IPlugin::Start ---
int SchedulerPlugin::Start() {
    auto* catalog = querier_ ? static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG)) : nullptr;
    if (catalog) {
        std::vector<OperatorMeta> ops;
        std::unordered_set<std::string> seen;
        auto append_unique = [&](OperatorMeta meta) {
            const std::string key = ToLowerAscii(meta.category) + "." + ToLowerAscii(meta.name);
            if (seen.insert(key).second) {
                ops.push_back(std::move(meta));
            }
        };
        querier_->Traverse(IID_OPERATOR, [&](void* p) -> int {
            auto* op = static_cast<IOperator*>(p);
            if (!op || op->Category().empty() || op->Name().empty()) return 0;
            OperatorMeta meta;
            meta.category = op->Category();
            meta.name = op->Name();
            meta.type = IEquals(meta.category, "builtin") ? "builtin" : "cpp";
            meta.source = "scheduler";
            meta.description = op->Description();
            meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
            append_unique(std::move(meta));
            return 0;
        });
        auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));
        if (op_registry) {
            op_registry->List([&](const char* name) {
                if (!name || name[0] == '\0') return;
                const std::string full_name(name);
                const auto separator = full_name.find('.');
                OperatorMeta meta;
                meta.category = full_name.substr(0, separator);
                meta.name = full_name.substr(separator + 1);
                meta.type = IEquals(meta.category, "builtin") ? "builtin" : "cpp";
                meta.source = "scheduler";
                meta.position = "data";
                IOperator* op = op_registry->Create(name);
                if (op) {
                    meta.description = op->Description();
                    meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
                    delete op;
                }
                append_unique(std::move(meta));
            });
        }
        UpsertResult upsert = catalog->UpsertBatch(ops);
        if (upsert.failed_count > 0) {
            LOG_ERROR("SchedulerPlugin::Start: catalog upsert failed, success=%d failed=%d err=%s",
                      upsert.success_count, upsert.failed_count, upsert.error_message.c_str());
        } else {
            LOG_INFO("SchedulerPlugin::Start: synced %d C++ operators to Catalog", upsert.success_count);
        }
    } else {
        LOG_ERROR("SchedulerPlugin::Start: IOperatorCatalog not found");
    }

    size_t batch_workers = batch_worker_count_;
    if (batch_workers == 0) {
        batch_workers = static_cast<size_t>(std::thread::hardware_concurrency());
    }
    if (batch_workers == 0) batch_workers = 1;
    batch_runtime_.Start(
        batch_workers,
        [this](const std::string& sql, std::string* rsp) -> int32_t {
            if (!rsp) return error::INTERNAL_ERROR;
            rapidjson::StringBuffer req_buf;
            rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
            req_w.StartObject();
            req_w.Key("sql");
            req_w.String(sql.c_str());
            req_w.EndObject();
            return HandleExecute("/scheduler/batch/execute", req_buf.GetString(), *rsp);
        });

    size_t workers = stream_worker_count_;
    if (workers == 0) {
        workers = static_cast<size_t>(std::thread::hardware_concurrency());
    }
    if (workers == 0) workers = 1;
    stream_runtime_.Start(workers);

    LOG_INFO("SchedulerPlugin::Start: ready, stream_workers=%zu, batch_workers=%zu", workers, batch_workers);
    return 0;
}

int SchedulerPlugin::Stop() {
    std::vector<std::pair<std::string, std::shared_ptr<StreamTaskGroup>>> groups;
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        groups.reserve(stream_task_groups_.size());
        for (const auto& kv : stream_task_groups_) {
            groups.push_back(kv);
        }
    }
    for (const auto& entry : groups) {
        if (entry.second) entry.second->RequestStop();
    }
    for (const auto& entry : groups) {
        if (entry.second) entry.second->Join();
    }
    for (const auto& entry : groups) {
        StreamGroupSnapshot snapshot;
        StreamGroupSnapshot* snapshot_ptr = nullptr;
        if (entry.second) {
            snapshot = entry.second->Snapshot();
            snapshot_ptr = &snapshot;
        }
        CleanupGroupRuntimeResources(entry.first, snapshot_ptr);
    }

    std::vector<std::shared_ptr<StreamTask>> tasks;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        tasks.reserve(stream_tasks_.size());
        for (const auto& kv : stream_tasks_) {
            if (kv.second) tasks.push_back(kv.second);
        }
    }

    for (const auto& task : tasks) {
        const StreamTaskStatus st = task->Status();
        if (st == StreamTaskStatus::kRunning ||
            st == StreamTaskStatus::kStopping ||
            st == StreamTaskStatus::kCreated) {
            task->RequestStop();
        }
    }
    for (const auto& task : tasks) {
        task->Join();
    }
    std::vector<std::string> runtime_ids;
    {
        std::lock_guard<std::mutex> lock(runtime_subscriptions_mu_);
        runtime_ids.reserve(runtime_subscriptions_.size());
        for (const auto& kv : runtime_subscriptions_) {
            runtime_ids.push_back(kv.first);
        }
    }
    for (const auto& runtime_id : runtime_ids) {
        ReleaseRuntimeSubscriptions(runtime_id);
    }
    PruneSharedHubs(true);
    stream_runtime_.Stop();
    batch_runtime_.Stop();

    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        stream_task_groups_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        stream_group_node_owners_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_node_sources_mu_);
        stream_group_node_sources_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_batch_nodes_mu_);
        stream_group_batch_node_runtime_ids_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
        stream_group_share_sets_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_share_set_snapshots_mu_);
        stream_group_share_set_snapshots_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        stream_tasks_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
        stream_task_leases_.clear();
        stream_source_leases_.clear();
        stream_channel_ref_counts_.clear();
        stream_channel_mutating_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(runtime_subscriptions_mu_);
        runtime_subscriptions_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(shared_hubs_mu_);
        shared_hubs_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_runtime_retention_mu_);
        stream_runtime_terminal_ms_.clear();
        stream_runtime_last_access_ms_.clear();
        stream_runtime_kind_.clear();
    }
    ClearManagedChannels();
    LOG_INFO("SchedulerPlugin::Stop: done");
    return 0;
}

// --- IRouterHandle ---

int32_t SchedulerPlugin::ClassifySql(const std::string& req_json, std::string* rsp_json) {
    if (!rsp_json) return error::INTERNAL_ERROR;
    return HandleSqlClassify("/scheduler/sql/classify", req_json, *rsp_json);
}

int32_t SchedulerPlugin::ExecuteBatch(const std::string& req_json, std::string* rsp_json) {
    if (!rsp_json) return error::INTERNAL_ERROR;
    return HandleExecute("/scheduler/batch/execute", req_json, *rsp_json);
}

int32_t SchedulerPlugin::SubmitBatch(const std::string& req_json, std::string* rsp_json) {
    if (!rsp_json) return error::INTERNAL_ERROR;
    return HandleBatchSubmit("/scheduler/batch/submit", req_json, *rsp_json);
}

int32_t SchedulerPlugin::QueryBatchStatus(const std::string& req_json, std::string* rsp_json) {
    if (!rsp_json) return error::INTERNAL_ERROR;
    return HandleBatchStatus("/scheduler/batch/status", req_json, *rsp_json);
}

int32_t SchedulerPlugin::StopBatch(const std::string& req_json, std::string* rsp_json) {
    if (!rsp_json) return error::INTERNAL_ERROR;
    return HandleBatchStop("/scheduler/batch/stop", req_json, *rsp_json);
}

int32_t SchedulerPlugin::ExecuteStream(const std::string& req_json, std::string* rsp_json) {
    if (!rsp_json) return error::INTERNAL_ERROR;
    return HandleStreamExecute("/scheduler/stream/execute", req_json, *rsp_json);
}

int32_t SchedulerPlugin::StopStream(const std::string& req_json, std::string* rsp_json) {
    if (!rsp_json) return error::INTERNAL_ERROR;
    return HandleStreamStop("/scheduler/stream/stop", req_json, *rsp_json);
}

int32_t SchedulerPlugin::QueryStreamStatus(const std::string& req_json, std::string* rsp_json) {
    if (!rsp_json) return error::INTERNAL_ERROR;
    return HandleStreamStatus("/scheduler/stream/status", req_json, *rsp_json);
}

int32_t SchedulerPlugin::QueryRuntimeGraph(const std::string& req_json, std::string* rsp_json) {
    if (!rsp_json) return error::INTERNAL_ERROR;
    return HandleRuntimeGraphQuery("/scheduler/runtime/graph/query", req_json, *rsp_json);
}

// --- 算子查找 ---
// 先查 C++ 静态算子（IQuerier），再查 Python 算子（IBridge）

std::string SchedulerPlugin::NextStreamTaskId() {
    const uint64_t seq = stream_task_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::ostringstream oss;
    oss << "stream_task_" << CurrentTimeMs() << "_" << seq;
    return oss.str();
}

}  // namespace scheduler
}  // namespace flowsql
