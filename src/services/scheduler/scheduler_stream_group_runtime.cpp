// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "scheduler_plugin.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <framework/core/fan_in_stream_channel.h>
#include <framework/core/json_error_builder.h>
#include <framework/core/ring_stream_channel.h>
#include <framework/core/sql_parser.h>

#include "scheduler_internal_utils.h"

namespace flowsql {
namespace scheduler {

namespace {

TaskSnapshot BuildBatchNodeSnapshot(const BatchRuntimeSnapshot& runtime_snapshot) {
    TaskSnapshot snapshot;
    snapshot.task_id = runtime_snapshot.runtime_task_id;
    snapshot.started_ms = runtime_snapshot.started_ms;
    snapshot.last_active_ms = runtime_snapshot.last_active_ms;
    snapshot.finished_ms = runtime_snapshot.finished_ms;
    snapshot.processed_rows = static_cast<uint64_t>(
        runtime_snapshot.result_row_count > 0 ? runtime_snapshot.result_row_count : 0);
    snapshot.output_rows = snapshot.processed_rows;
    snapshot.error_message = runtime_snapshot.error_message;
    snapshot.error_code_text = runtime_snapshot.error_code;
    if (!runtime_snapshot.error_code.empty()) {
        char* endptr = nullptr;
        const long parsed = std::strtol(runtime_snapshot.error_code.c_str(), &endptr, 10);
        if (endptr && *endptr == '\0') snapshot.error_code = static_cast<int>(parsed);
    }
    switch (runtime_snapshot.status) {
        case BatchRuntimeStatus::kPending:
            snapshot.status = StreamTaskStatus::kCreated;
            break;
        case BatchRuntimeStatus::kRunning:
            snapshot.status = StreamTaskStatus::kRunning;
            break;
        case BatchRuntimeStatus::kStopping:
            snapshot.status = StreamTaskStatus::kStopping;
            break;
        case BatchRuntimeStatus::kStopped:
        case BatchRuntimeStatus::kCompleted:
            snapshot.status = StreamTaskStatus::kStopped;
            break;
        case BatchRuntimeStatus::kCancelled:
            snapshot.status = StreamTaskStatus::kCancelled;
            break;
        case BatchRuntimeStatus::kFailed:
        case BatchRuntimeStatus::kTimeout:
            snapshot.status = StreamTaskStatus::kFailed;
            break;
        default:
            snapshot.status = StreamTaskStatus::kFailed;
            break;
    }
    return snapshot;
}

bool ExtractRuntimeTaskId(const std::string& json, std::string* runtime_task_id) {
    if (!runtime_task_id) return false;
    runtime_task_id->clear();
    rapidjson::Document d;
    d.Parse(json.c_str());
    if (d.HasParseError() || !d.IsObject()) return false;
    if (d.HasMember("runtime_task_id") && d["runtime_task_id"].IsString()) {
        *runtime_task_id = d["runtime_task_id"].GetString();
        return !runtime_task_id->empty();
    }
    if (d.HasMember("task_id") && d["task_id"].IsString()) {
        *runtime_task_id = d["task_id"].GetString();
        return !runtime_task_id->empty();
    }
    return false;
}

std::string MakeSafeName(const std::string& input) {
    std::string out = input;
    for (auto& c : out) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
            c = '_';
        }
    }
    return out;
}

}  // namespace

int32_t SchedulerPlugin::AcquireStreamGroupLeases(const std::string& runtime_task_id,
                                                  const StreamGroupBuildArtifacts& build,
                                                  std::string* err_rsp) {
    if (!err_rsp) return error::INTERNAL_ERROR;

    SweepFinishedTaskLeases();
    std::vector<std::string> lease_keys;
    lease_keys.reserve(build.group_source_keys.size() + build.group_sink_keys.size());
    lease_keys.insert(lease_keys.end(), build.group_source_keys.begin(), build.group_source_keys.end());
    lease_keys.insert(lease_keys.end(), build.group_sink_keys.begin(), build.group_sink_keys.end());
    std::unordered_map<std::string, uint64_t> version_snapshot;
    CaptureStreamChannelVersionSnapshot(lease_keys, &version_snapshot);

    std::string conflict_key;
    std::string version_conflict_key;
    bool blocked_by_mutation = false;
    const int lease_rc = TryAcquireStreamTaskLeases(runtime_task_id,
                                                    build.group_source_keys,
                                                    build.group_sink_keys,
                                                    &conflict_key,
                                                    &blocked_by_mutation,
                                                    runtime_task_id,
                                                    &version_snapshot,
                                                    &version_conflict_key);
    if (lease_rc == 0) return error::OK;

    if (lease_rc == EBUSY) {
        if (blocked_by_mutation) {
            *err_rsp = BuildExecutionErrorJson(
                "stream channel is being modified: " + conflict_key,
                ErrorCodeId::kStreamChannelMutating,
                ErrorStageId::kLease);
            return error::CONFLICT;
        }
        *err_rsp = BuildExecutionErrorJson(
            "stream source is in use: " + conflict_key,
            ErrorCodeId::kStreamSourceInUse,
            ErrorStageId::kLease);
        return error::CONFLICT;
    }
    if (lease_rc == EAGAIN) {
        *err_rsp = BuildExecutionErrorJson(
            "stream channel changed during group prepare: " + version_conflict_key,
            ErrorCodeId::kStreamChannelVersionChanged,
            ErrorStageId::kLease);
        return error::CONFLICT;
    }

    *err_rsp = BuildExecutionErrorJson(
        "stream group lease acquire failed",
        ErrorCodeId::kStreamLeaseFailed,
        ErrorStageId::kLease);
    return error::INTERNAL_ERROR;
}

int32_t SchedulerPlugin::PrepareStreamGroupRuntimeResources(const std::string& runtime_task_id,
                                                            const StreamGroupBuildArtifacts& build,
                                                            StreamGroupRuntimeArtifacts* out,
                                                            std::function<void()>* cleanup_local_resources,
                                                            std::string* err_rsp) {
    if (!out || !cleanup_local_resources || !err_rsp) return error::INTERNAL_ERROR;

    out->node_source_overrides.clear();
    out->share_set_runtimes.clear();
    out->created_channel_refs.clear();

    std::unordered_set<std::string> channel_ref_dedup;
    auto cleanup = [this, out]() {
        for (auto& ss : out->share_set_runtimes) {
            if (ss.hub) {
                ss.hub->RequestStop();
                ss.hub->Join();
            }
        }
        for (const auto& channel_ref : out->created_channel_refs) {
            EraseManagedChannel(channel_ref);
        }
        out->created_channel_refs.clear();
    };

    for (const auto& ss : build.share_set_plans) {
        std::shared_ptr<IStreamChannel> shared_source = ss.source_channels.front();
        std::shared_ptr<FanInStreamChannel> fanin;
        if (ss.source_channels.size() > 1) {
            fanin = std::make_shared<FanInStreamChannel>(
                "fanin",
                runtime_task_id + "." + ss.id + ".fanin",
                ss.source_channels);
            shared_source = fanin;
        }

        StreamGroupShareSetRuntimeItem runtime;
        runtime.id = ss.id;
        runtime.source_ref = ss.source_ref;
        runtime.members = ss.members;
        SharedHubOptions hub_opts;
        hub_opts.queue_size = shared_subscriber_queue_size_;
        hub_opts.poll_timeout_ms = shared_hub_poll_timeout_ms_;
        hub_opts.overflow_policy = OverflowPolicy::kDrop;
        hub_opts.ring_mode = RingMode::SPSC;
        hub_opts.coordinated_drop = true;
        runtime.hub = std::make_shared<SharedSourceHub>(
            ss.id,
            SharedHubMode::kFixed,
            ss.source_ref,
            ss.canonical_source_keys,
            shared_source,
            hub_opts);

        for (size_t mi = 0; mi < ss.members.size(); ++mi) {
            const std::string internal_name = MakeSafeName(
                runtime_task_id + "_" + ss.id + "_" + ss.members[mi] + "_in");
            const std::string channel_ref = "stream." + internal_name;
            if (!channel_ref_dedup.insert(channel_ref).second) {
                *err_rsp = BuildExecutionErrorJson(
                    "duplicate internal stream channel reference: " + channel_ref,
                    ErrorCodeId::kStreamGroupBranchBuildFailed,
                    ErrorStageId::kBranchBuild);
                cleanup();
                return error::INTERNAL_ERROR;
            }

            RingStreamChannelOptions opts;
            opts.ring_size = 2048;
            opts.batch_rows = 1024;
            opts.overflow = OverflowPolicy::kDrop;
            opts.ring_mode = RingMode::SPSC;
            opts.finite = false;
            auto internal = std::make_shared<RingStreamChannel>("ring", internal_name, opts);
            const int open_rc = internal->Open();
            if (open_rc != 0) {
                *err_rsp = BuildExecutionErrorJson(
                    "open internal stream channel failed: " + channel_ref,
                    ErrorCodeId::kStreamGroupBranchBuildFailed,
                    ErrorStageId::kBranchBuild);
                cleanup();
                return error::INTERNAL_ERROR;
            }

            RegisterChannel(channel_ref, std::static_pointer_cast<IChannel>(internal));
            out->created_channel_refs.push_back(channel_ref);
            runtime.internal_channel_refs.push_back(channel_ref);
            out->node_source_overrides[ss.members[mi]] = channel_ref;

            std::string sub_err;
            const int add_rc = runtime.hub->AddSubscriber(
                runtime_task_id + ":" + ss.members[mi],
                ss.members[mi],
                true,
                nullptr,
                &sub_err,
                0,
                internal);
            if (add_rc != 0) {
                *err_rsp = BuildExecutionErrorJson(
                    "add share_set subscriber failed: " + ss.members[mi] +
                        (sub_err.empty() ? "" : (", " + sub_err)),
                    ErrorCodeId::kStreamGroupBranchBuildFailed,
                    ErrorStageId::kBranchBuild);
                cleanup();
                return error::INTERNAL_ERROR;
            }
        }
        out->share_set_runtimes.push_back(std::move(runtime));
    }

    *cleanup_local_resources = cleanup;
    return error::OK;
}

std::shared_ptr<StreamTaskGroup> SchedulerPlugin::BuildStreamGroupObject(
    const std::string& runtime_task_id,
    const StreamGroupExecuteRequest& req,
    const StreamGroupBuildArtifacts& build,
    const StreamGroupRuntimeArtifacts& runtime_build,
    std::shared_ptr<StreamGroupCallbackContext>* callback_ctx_out) {
    auto callback_ctx = std::make_shared<StreamGroupCallbackContext>();
    callback_ctx->group_runtime_task_id = runtime_task_id;
    callback_ctx->timeout_s = req.timeout_s;
    callback_ctx->node_source_overrides = runtime_build.node_source_overrides;
    callback_ctx->node_kind_by_id.reserve(build.plans.size());
    for (const auto& plan : build.plans) {
        callback_ctx->node_kind_by_id[plan.id] = plan.kind;
    }

    auto group = std::make_shared<StreamTaskGroup>(
        runtime_task_id,
        runtime_task_id,
        build.plans,
        req.timeout_s,
        [this, callback_ctx](const std::string& node_id,
                             const std::string& sql,
                             std::string* node_runtime_task_id,
                             std::string* error_msg) -> int {
            return SubmitStreamGroupNodeRuntime(callback_ctx.get(),
                                                node_id,
                                                sql,
                                                node_runtime_task_id,
                                                error_msg);
        },
        [this, callback_ctx](const std::string& node_runtime_task_id,
                             TaskSnapshot* snapshot_out) -> int {
            return QueryStreamGroupNodeRuntime(callback_ctx.get(), node_runtime_task_id, snapshot_out);
        },
        [this, callback_ctx](const std::string& node_runtime_task_id) {
            StopStreamGroupNodeRuntime(callback_ctx.get(), node_runtime_task_id);
        },
        [this, runtime_task_id]() {
            StopStreamGroupShareSetHubs(runtime_task_id);
        });

    if (callback_ctx_out) {
        *callback_ctx_out = std::move(callback_ctx);
    }
    return group;
}

int SchedulerPlugin::SubmitStreamGroupNodeRuntime(StreamGroupCallbackContext* ctx,
                                                  const std::string& node_id,
                                                  const std::string& sql,
                                                  std::string* node_runtime_task_id,
                                                  std::string* error_msg) {
    if (!ctx) return EINVAL;

    auto node_kind_it = ctx->node_kind_by_id.find(node_id);
    if (node_kind_it == ctx->node_kind_by_id.end()) {
        if (error_msg) *error_msg = "node " + node_id + " kind not found";
        return EINVAL;
    }
    const GroupNodeKind node_kind = node_kind_it->second;

    if (node_kind == GroupNodeKind::kBatch) {
        if (ctx->node_source_overrides.find(node_id) != ctx->node_source_overrides.end()) {
            if (error_msg) *error_msg = "batch node does not support source override: " + node_id;
            return EINVAL;
        }

        const std::string batch_runtime_task_id = "b_" + NextStreamTaskId();
        std::vector<std::string> sqls;
        sqls.push_back(sql);
        std::string submit_err;
        const int submit_rc = batch_runtime_.Submit(batch_runtime_task_id,
                                                    std::move(sqls),
                                                    ctx->timeout_s,
                                                    &submit_err);
        if (submit_rc != 0) {
            if (error_msg) {
                *error_msg = "node " + node_id + " batch submit failed";
                if (!submit_err.empty()) *error_msg += ": " + submit_err;
            }
            return submit_rc;
        }

        if (node_runtime_task_id) {
            *node_runtime_task_id = batch_runtime_task_id;
        }
        {
            std::lock_guard<std::mutex> lock(ctx->runtime_kind_mu);
            ctx->runtime_kind_by_node_task_id[batch_runtime_task_id] = GroupNodeKind::kBatch;
        }
        {
            std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
            stream_group_node_owners_[batch_runtime_task_id] = ctx->group_runtime_task_id;
        }
        {
            std::lock_guard<std::mutex> lock(stream_group_batch_nodes_mu_);
            stream_group_batch_node_runtime_ids_[ctx->group_runtime_task_id].push_back(batch_runtime_task_id);
        }
        return 0;
    }

    SqlParser parser;
    SqlStatement stmt = parser.Parse(sql);
    if (!stmt.error.empty()) {
        if (error_msg) *error_msg = "node " + node_id + " SQL parse failed: " + stmt.error;
        return EINVAL;
    }
    if (stmt.sources.empty()) {
        if (error_msg) *error_msg = "node " + node_id + " source channel not found";
        return EINVAL;
    }

    auto override_it = ctx->node_source_overrides.find(node_id);
    if (override_it != ctx->node_source_overrides.end()) {
        stmt.sources.clear();
        stmt.sources.push_back(override_it->second);
    }

    std::string exec_rsp;
    const int32_t rc = ExecuteStreamTask(stmt, exec_rsp, ctx->group_runtime_task_id, true);
    if (rc != error::OK) {
        const std::string err = ExtractErrorMessage(exec_rsp);
        if (error_msg) {
            *error_msg = "node " + node_id + " execute failed";
            if (!err.empty()) *error_msg += ": " + err;
        }
        return rc;
    }

    if (!ExtractRuntimeTaskId(exec_rsp, node_runtime_task_id) || node_runtime_task_id->empty()) {
        if (error_msg) *error_msg = "node " + node_id + " execute missing runtime_task_id";
        return EIO;
    }

    {
        std::lock_guard<std::mutex> lock(ctx->runtime_kind_mu);
        ctx->runtime_kind_by_node_task_id[*node_runtime_task_id] = GroupNodeKind::kStream;
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        stream_group_node_owners_[*node_runtime_task_id] = ctx->group_runtime_task_id;
    }
    return 0;
}

int SchedulerPlugin::QueryStreamGroupNodeRuntime(StreamGroupCallbackContext* ctx,
                                                 const std::string& node_runtime_task_id,
                                                 TaskSnapshot* snapshot_out) {
    GroupNodeKind node_kind = GroupNodeKind::kStream;
    if (ctx) {
        std::lock_guard<std::mutex> lock(ctx->runtime_kind_mu);
        auto it = ctx->runtime_kind_by_node_task_id.find(node_runtime_task_id);
        if (it != ctx->runtime_kind_by_node_task_id.end()) {
            node_kind = it->second;
        }
    }

    if (node_kind == GroupNodeKind::kBatch) {
        BatchRuntimeSnapshot batch_snapshot;
        const int rc = batch_runtime_.Query(node_runtime_task_id, &batch_snapshot);
        if (rc != 0) return rc;
        if (snapshot_out) {
            *snapshot_out = BuildBatchNodeSnapshot(batch_snapshot);
        }
        return 0;
    }
    return QueryStreamTaskSnapshotByRuntimeId(node_runtime_task_id, snapshot_out);
}

void SchedulerPlugin::StopStreamGroupNodeRuntime(StreamGroupCallbackContext* ctx,
                                                 const std::string& node_runtime_task_id) {
    GroupNodeKind node_kind = GroupNodeKind::kStream;
    if (ctx) {
        std::lock_guard<std::mutex> lock(ctx->runtime_kind_mu);
        auto it = ctx->runtime_kind_by_node_task_id.find(node_runtime_task_id);
        if (it != ctx->runtime_kind_by_node_task_id.end()) {
            node_kind = it->second;
        }
    }

    if (node_kind == GroupNodeKind::kBatch) {
        std::string stop_err;
        batch_runtime_.RequestStop(node_runtime_task_id, &stop_err);
        return;
    }
    RequestStopStreamTaskByRuntimeId(node_runtime_task_id);
}

void SchedulerPlugin::StopStreamGroupShareSetHubs(const std::string& group_runtime_task_id) {
    std::vector<std::shared_ptr<SharedSourceHub>> hubs;
    {
        std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
        auto it = stream_group_share_sets_.find(group_runtime_task_id);
        if (it != stream_group_share_sets_.end()) {
            for (const auto& ss : it->second) {
                if (ss.hub) hubs.push_back(ss.hub);
            }
        }
    }
    for (auto& hub : hubs) {
        hub->RequestStop();
        hub->Join();
    }
}

int32_t SchedulerPlugin::RegisterAndStartStreamGroup(const std::string& runtime_task_id,
                                                     const StreamGroupBuildArtifacts& build,
                                                     const StreamGroupRuntimeArtifacts& runtime_build,
                                                     const std::shared_ptr<StreamTaskGroup>& group,
                                                     int share_set_ready_timeout_s,
                                                     std::function<void()> cleanup_local_resources,
                                                     std::string* err_rsp) {
    if (!group || !err_rsp) return error::INTERNAL_ERROR;

    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        stream_task_groups_[runtime_task_id] = group;
    }

    {
        std::unordered_map<std::string, GroupNodeResolvedSourceMeta> node_source_snapshot;
        node_source_snapshot.reserve(build.plans.size());
        for (const auto& plan : build.plans) {
            auto it = build.node_resolved.find(plan.id);
            if (it == build.node_resolved.end()) continue;
            GroupNodeResolvedSourceMeta meta;
            meta.sources = it->second.resolved_sources;
            meta.expand_rule = it->second.expand_rule;
            node_source_snapshot.emplace(plan.id, std::move(meta));
        }
        std::lock_guard<std::mutex> lock(stream_group_node_sources_mu_);
        stream_group_node_sources_[runtime_task_id] = std::move(node_source_snapshot);
    }

    if (!runtime_build.share_set_runtimes.empty()) {
        std::vector<StreamGroupShareSetRuntime> share_sets;
        share_sets.reserve(runtime_build.share_set_runtimes.size());
        for (const auto& ss : runtime_build.share_set_runtimes) {
            StreamGroupShareSetRuntime item;
            item.id = ss.id;
            item.source_ref = ss.source_ref;
            item.members = ss.members;
            item.internal_channel_refs = ss.internal_channel_refs;
            item.hub = ss.hub;
            share_sets.push_back(std::move(item));
        }
        std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
        stream_group_share_sets_[runtime_task_id] = std::move(share_sets);
    }

    {
        std::lock_guard<std::mutex> lock(stream_group_share_set_snapshots_mu_);
        stream_group_share_set_snapshots_.erase(runtime_task_id);
    }

    std::string start_err;
    const int start_rc = group->Start(&start_err);
    if (start_rc != 0) {
        {
            std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
            stream_task_groups_.erase(runtime_task_id);
        }
        {
            std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
            stream_group_share_sets_.erase(runtime_task_id);
        }
        {
            std::lock_guard<std::mutex> lock(stream_group_node_sources_mu_);
            stream_group_node_sources_.erase(runtime_task_id);
        }
        if (cleanup_local_resources) cleanup_local_resources();
        *err_rsp = BuildErrorJson("start stream group failed: " +
                                  (start_err.empty() ? std::to_string(start_rc) : start_err));
        return error::INTERNAL_ERROR;
    }

    TouchRuntimeAccess(runtime_task_id);

    for (const auto& ss_runtime : runtime_build.share_set_runtimes) {
        const int64_t deadline_ms = CurrentTimeMs() + static_cast<int64_t>(share_set_ready_timeout_s) * 1000;
        bool ready = false;
        while (CurrentTimeMs() < deadline_ms) {
            auto snapshot = group->Snapshot();
            if (snapshot.status == StreamGroupStatus::kFailed ||
                snapshot.status == StreamGroupStatus::kCancelled ||
                snapshot.status == StreamGroupStatus::kStopped) {
                break;
            }

            bool all_started = true;
            bool has_terminal_fail = false;
            for (const auto& member : ss_runtime.members) {
                bool found = false;
                for (const auto& node : snapshot.nodes) {
                    if (node.node_id != member) continue;
                    found = true;
                    if (node.status == GroupNodeStatus::kPending ||
                        node.status == GroupNodeStatus::kReady) {
                        all_started = false;
                    }
                    if (node.status == GroupNodeStatus::kFailed ||
                        node.status == GroupNodeStatus::kCancelled ||
                        node.status == GroupNodeStatus::kSkipped) {
                        has_terminal_fail = true;
                    }
                    break;
                }
                if (!found) all_started = false;
            }
            if (has_terminal_fail) break;
            if (all_started) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (!ready) {
            group->MarkExternalFailed(
                ETIMEDOUT,
                "share_set ready timeout: " + ss_runtime.id +
                    ", timeout_s=" + std::to_string(share_set_ready_timeout_s),
                ToErrorCode(ErrorCodeId::kStreamGroupShareSetReadyTimeout));
            break;
        }

        std::string hub_err;
        const int hub_rc = ss_runtime.hub ? ss_runtime.hub->Start(&hub_err) : EINVAL;
        if (hub_rc != 0) {
            group->MarkExternalFailed(
                hub_rc,
                "share_set start failed: " + ss_runtime.id +
                    (hub_err.empty() ? "" : (", " + hub_err)),
                ToErrorCode(ErrorCodeId::kStreamGroupShareSetStartFailed));
            break;
        }
    }

    return error::OK;
}

}  // namespace scheduler
}  // namespace flowsql
