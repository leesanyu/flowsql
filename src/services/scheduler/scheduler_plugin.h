// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_
#define _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <common/error_code.h>
#include <common/iplugin.h>
#include <common/span.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/ibridge.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/ischeduler_control_service.h>
#include <framework/interfaces/istream_channel.h>
#include <framework/interfaces/iblock_stream_channel.h>
#include <framework/interfaces/iblock_stream_operator.h>
#include <framework/interfaces/idataframe_channel.h>

#include <rapidjson/document.h>

#include "stream_runtime.h"
#include "scheduler_batch_runtime.h"
#include "shared_source_hub.h"
#include "stream_task_group.h"
#include "stream_execution_plan.h"
#include "scheduler_stream_group_internal.h"

namespace flowsql {

class IChannel;
class IBlockStreamFactory;
class IBlockStreamManager;
class IOperator;
struct SqlStatement;
struct BoundFilterExpr;

namespace scheduler {

struct GroupNodeResolvedSourceMeta {
    std::vector<std::string> sources;
    std::string expand_rule = "explicit";
};

// SchedulerPlugin — SQL 执行调度插件
// 通过 IRouterHandle 声明路由，对 HTTP 完全无感知
class SchedulerPlugin : public IPlugin, public IRouterHandle, public ISchedulerControlService {
 public:
    SchedulerPlugin() = default;
    ~SchedulerPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // IRouterHandle — 声明路由
    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;

    // ISchedulerControlService
    int32_t ClassifySql(const std::string& req_json, std::string* rsp_json) override;
    int32_t ExecuteBatch(const std::string& req_json, std::string* rsp_json) override;
    int32_t SubmitBatch(const std::string& req_json, std::string* rsp_json) override;
    int32_t QueryBatchStatus(const std::string& req_json, std::string* rsp_json) override;
    int32_t StopBatch(const std::string& req_json, std::string* rsp_json) override;
    int32_t ExecuteStream(const std::string& req_json, std::string* rsp_json) override;
    int32_t StopStream(const std::string& req_json, std::string* rsp_json) override;
    int32_t QueryStreamStatus(const std::string& req_json, std::string* rsp_json) override;
    int32_t QueryRuntimeGraph(const std::string& req_json, std::string* rsp_json) override;

 private:
    friend struct SchedulerPluginTestAccessor;

    // 路由处理（fnRouterHandler 签名）
    int32_t HandleExecute(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleBatchSubmit(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleBatchStatus(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleBatchStop(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleSqlClassify(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamExecute(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamStop(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamStatus(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamList(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleRuntimeGraphQuery(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleGetChannels(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleQueryStreamChannelDefinitions(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleQueryStreamChannels(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleAddStreamChannel(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleModifyStreamChannel(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleResetStreamChannel(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleRemoveStreamChannel(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleRefreshOperators(const std::string& uri, const std::string& req, std::string& rsp);

    // 通道管理
    void RegisterManagedChannel(const std::string& key, std::shared_ptr<IChannel> ch);
    void EraseManagedChannel(const std::string& key);
    void ClearManagedChannels();
    std::shared_ptr<IChannel> FindManagedChannelShared(const std::string& key);
    std::vector<std::pair<std::string, std::shared_ptr<IChannel>>> SnapshotManagedChannels();
    IChannel* FindChannel(const std::string& name);
    IChannel* FindChannel(const std::string& name, std::shared_ptr<IChannel>* owner_out);
    IChannel* FindChannel(const std::string& name,
                          std::shared_ptr<IChannel>* owner_out,
                          bool* ambiguous_out);
    void RegisterChannel(const std::string& key, std::shared_ptr<IChannel> ch);
    size_t TraverseBlockFactories(const std::function<int(IBlockStreamFactory*)>& visitor) const;
    size_t TraverseBlockManagers(const std::function<int(IBlockStreamManager*)>& visitor) const;

    struct BlockManagerRouteResult {
        size_t provider_count = 0;
        size_t accepted_count = 0;
        size_t not_found_count = 0;
        int accepted_rc = 0;
        bool conflict = false;
    };
    BlockManagerRouteResult RouteBlockManagers(
        const std::function<int(IBlockStreamManager*)>& operation,
        bool ignore_not_found) const;
    std::vector<IBlockStreamManager*> FindBlockManagerOwners(
        const std::string& type,
        const std::string& name) const;

    // 算子查找
    std::shared_ptr<IOperator> FindOperator(const std::string& category, const std::string& name);
    std::shared_ptr<IOperator> CreateOperator(const std::string& category, const std::string& name);
    IBlockStreamOperator* FindBlockOperator(const std::string& category, const std::string& name);
    IBlockTransformOperatorV1* FindBlockTransformOperator(
        const std::string& category,
        const std::string& name,
        bool* ambiguous,
        int* traverse_error);
    enum class BlockExecutionTerminal {
        kCompleted,
        kStopped,
        kCancelled,
        kFailed,
    };
    struct BlockSourceFilterPlan {
        std::string pushed_filter_plan_json;
        std::shared_ptr<const BoundFilterExpr> exclusive_residual;
        std::shared_ptr<const BoundFilterExpr> shared_residual;
    };
    int BuildBlockSourceFilterPlan(IBlockStreamChannel* source,
                                   const SqlStatement& stmt,
                                   BlockSourceFilterPlan* plan,
                                   std::string* error);
    int ExecuteBlockOperator(IBlockStreamChannel* source,
                             IBlockStreamOperator* op,
                             const std::shared_ptr<const BoundFilterExpr>& source_residual,
                             BlockExecutionTerminal* terminal,
                             int64_t* rows_affected,
                             std::string* error);
    int ExecuteBlockTransformPipeline(IBlockStreamChannel* source,
                                      const std::vector<IBlockTransformOperatorV1*>& providers,
                                      IDataFrameChannel* sink,
                                      const SqlStatement& stmt,
                                      const std::shared_ptr<const BoundFilterExpr>& source_residual,
                                      BlockExecutionTerminal* terminal,
                                      int64_t* rows_affected,
                                      std::string* error);
    int ExecuteSingleBlockTransformPipeline(IBlockStreamChannel* source,
                                            IBlockTransformOperatorV1* provider,
                                            IDataFrameChannel* sink,
                                            const SqlStatement& stmt,
                                            const std::shared_ptr<const BoundFilterExpr>& source_residual,
                                            BlockExecutionTerminal* terminal,
                                            int64_t* rows_affected,
                                            std::string* error);

    // 执行路径
    int ExecuteTransfer(IChannel* source, IChannel* sink,
                        const std::string& source_type, const std::string& sink_type,
                        const SqlStatement& stmt,
                        const std::shared_ptr<const BoundFilterExpr>& source_residual = nullptr,
                        int64_t* rows_affected = nullptr,
                        std::string* error = nullptr);

    int ExecuteWithOperator(IChannel* source, IChannel* sink, IOperator* op,
                            const std::string& sink_type,
                            const SqlStatement& stmt, int64_t* rows_affected = nullptr,
                            std::string* error = nullptr);
    int ExecuteWithOperatorChain(Span<IChannel*> inputs, IChannel* sink, const std::vector<IOperator*>& ops,
                                 const std::string& sink_type,
                                 const SqlStatement& stmt, int64_t* rows_affected = nullptr,
                                 std::string* error = nullptr);

    int32_t ExecuteStreamTask(const SqlStatement& stmt,
                              std::string& rsp,
                              const std::string& lease_owner_id = "",
                              bool skip_lease_acquire = false);
    int32_t BuildStreamExecutionPlan(const SqlStatement& stmt,
                                     const std::string& lease_owner_id,
                                     bool skip_lease_acquire,
                                     StreamExecutionPlan* plan,
                                     std::string* err_rsp);
    int32_t ValidateStreamExecutionPlan(StreamExecutionPlan* plan, std::string* err_rsp);
    int32_t AcquireStreamExecutionLease(StreamExecutionPlan* plan,
                                        LeaseToken* lease_token,
                                        std::string* err_rsp);
    int32_t AcquireSharedSourceSubscription(StreamExecutionPlan* plan,
                                            std::shared_ptr<IStreamChannel>* source_override,
                                            std::string* err_rsp);
    int32_t HandleStreamExecuteSingle(const rapidjson::Document& doc, std::string& rsp);
    int32_t HandleStreamExecuteGroup(const rapidjson::Document& doc, std::string& rsp);
    int32_t ParseStreamGroupExecuteRequest(const rapidjson::Document& doc,
                                           StreamGroupExecuteRequest* out,
                                           std::string* err_rsp);
    int32_t BuildStreamGroupPlan(const StreamGroupExecuteRequest& req,
                                 StreamGroupBuildArtifacts* out,
                                 std::string* err_rsp);
    int32_t ValidateStreamGroupPlan(const StreamGroupBuildArtifacts& build,
                                    std::string* err_rsp);
    int32_t AcquireStreamGroupLeases(const std::string& runtime_task_id,
                                     const StreamGroupBuildArtifacts& build,
                                     std::string* err_rsp);
    int32_t PrepareStreamGroupRuntimeResources(const std::string& runtime_task_id,
                                               const StreamGroupBuildArtifacts& build,
                                               StreamGroupRuntimeArtifacts* out,
                                               std::function<void()>* cleanup_local_resources,
                                               std::string* err_rsp);
    std::shared_ptr<StreamTaskGroup> BuildStreamGroupObject(
        const std::string& runtime_task_id,
        const StreamGroupExecuteRequest& req,
        const StreamGroupBuildArtifacts& build,
        const StreamGroupRuntimeArtifacts& runtime_build,
        std::shared_ptr<StreamGroupCallbackContext>* callback_ctx_out);
    int32_t RegisterAndStartStreamGroup(const std::string& runtime_task_id,
                                        const StreamGroupBuildArtifacts& build,
                                        const StreamGroupRuntimeArtifacts& runtime_build,
                                        const std::shared_ptr<StreamTaskGroup>& group,
                                        int share_set_ready_timeout_s,
                                        std::function<void()> cleanup_local_resources,
                                        std::string* err_rsp);
    int SubmitStreamGroupNodeRuntime(StreamGroupCallbackContext* ctx,
                                     const std::string& node_id,
                                     const std::string& sql,
                                     std::string* node_runtime_task_id,
                                     std::string* error_msg);
    int QueryStreamGroupNodeRuntime(StreamGroupCallbackContext* ctx,
                                    const std::string& node_runtime_task_id,
                                    TaskSnapshot* snapshot_out);
    void StopStreamGroupNodeRuntime(StreamGroupCallbackContext* ctx,
                                    const std::string& node_runtime_task_id);
    void StopStreamGroupShareSetHubs(const std::string& group_runtime_task_id);
    int32_t ClassifySqlTaskKind(const std::string& sql_text, std::string* task_kind, std::string* err_rsp);
    int QueryStreamTaskSnapshotByRuntimeId(const std::string& runtime_task_id, TaskSnapshot* snapshot_out);
    int QueryRuntimeSharedHubSnapshot(const std::string& runtime_task_id, SharedHubSnapshot* snapshot_out);
    void RequestStopStreamTaskByRuntimeId(const std::string& runtime_task_id);
    std::vector<SharedHubSnapshot> QueryGroupShareSetSnapshots(const std::string& group_runtime_task_id);
    std::unordered_map<std::string, GroupNodeResolvedSourceMeta> QueryGroupNodeResolvedSources(
        const std::string& group_runtime_task_id);
    void CleanupGroupRuntimeResources(const std::string& group_runtime_task_id,
                                      const StreamGroupSnapshot* group_snapshot = nullptr);
    std::string NextStreamTaskId();

    struct SinkBinding {
        std::shared_ptr<IChannel> sink_channel;
        std::string sink_type;
        std::string db_type;
        std::string db_name;
        std::string table_name;
    };

    int32_t ResolveStreamSink(const SqlStatement& stmt,
                              SinkBinding* binding,
                              std::string* err_out);
    struct SourceResolveResult {
        std::vector<IChannel*> channels;
        std::vector<std::shared_ptr<IChannel>> channel_holders;
        std::vector<std::shared_ptr<IStreamChannel>> stream_channels;
        std::vector<std::shared_ptr<IBlockStreamChannel>> block_channels;
        std::vector<std::string> source_keys;
        std::vector<std::string> resolved_sources;
        std::string source_expand_rule = "explicit";
        bool has_stream_source = false;
        bool has_non_stream_source = false;
        bool has_block_source = false;
    };
    int32_t ResolveSourceBindings(const SqlStatement& stmt,
                                  SourceResolveResult* out,
                                  std::string* err_rsp);
    std::string QueryStreamChannelRole(const std::string& type, const std::string& name);
    int TryAcquireStreamTaskLeases(const std::string& runtime_task_id,
                                   const std::vector<std::string>& source_keys,
                                   const std::vector<std::string>& sink_keys,
                                   std::string* conflict_key_out,
                                   bool* blocked_by_mutation_out = nullptr,
                                   const std::string& lease_owner_id = "",
                                   const std::unordered_map<std::string, uint64_t>* expected_versions = nullptr,
                                   std::string* version_conflict_key_out = nullptr);
    void CaptureStreamChannelVersionSnapshot(
        const std::vector<std::string>& keys,
        std::unordered_map<std::string, uint64_t>* snapshot_out);
    int TryBeginStreamChannelMutation(const std::string& key, std::string* reason_out);
    void EndStreamChannelMutation(const std::string& key);
    void ReleaseStreamTaskLeases(const std::string& runtime_task_id);
    void ReleaseRuntimeSubscriptions(const std::string& runtime_task_id);
    void PruneSharedHubs(bool force_stop = false);
    void SweepFinishedTaskLeases();
    void MarkRuntimeTerminal(const std::string& runtime_task_id,
                             const std::string& runtime_kind,
                             int64_t terminal_ms = 0);
    void TouchRuntimeAccess(const std::string& runtime_task_id, int64_t now_ms = 0);
    void SweepRuntimeRetainedObjects(int64_t now_ms = 0);

    IQuerier* querier_ = nullptr;

    // 通道表
    mutable std::mutex channels_mu_;
    std::unordered_map<std::string, std::shared_ptr<IChannel>> channels_;

    std::string host_ = "127.0.0.1";
    int port_ = 18803;
    size_t max_resolved_sources_ = 64;
    int max_stream_group_timeout_s_ = 86400;
    int stream_runtime_retention_s_ = 600;
    size_t stream_runtime_max_count_ = 2000;
    size_t max_shared_hubs_ = 4096;
    size_t max_subscribers_per_hub_ = 128;
    size_t shared_subscriber_queue_size_ = 2048;
    int shared_hub_poll_timeout_ms_ = 50;

    // 用于生成唯一临时通道名
    std::atomic<uint64_t> tmp_channel_seq_{0};

    // 流式任务调度
    size_t stream_worker_count_ = 0;
    size_t batch_worker_count_ = 0;
    SchedulerBatchRuntime batch_runtime_;
    StreamRuntime stream_runtime_;
    std::atomic<uint64_t> stream_task_seq_{0};
    mutable std::mutex stream_tasks_mu_;
    std::unordered_map<std::string, std::shared_ptr<StreamTask>> stream_tasks_;
    mutable std::mutex stream_task_groups_mu_;
    std::unordered_map<std::string, std::shared_ptr<StreamTaskGroup>> stream_task_groups_;
    mutable std::mutex stream_group_nodes_mu_;
    std::unordered_map<std::string, std::string> stream_group_node_owners_;
    mutable std::mutex stream_group_node_sources_mu_;
    std::unordered_map<std::string, std::unordered_map<std::string, GroupNodeResolvedSourceMeta>>
        stream_group_node_sources_;
    mutable std::mutex stream_group_batch_nodes_mu_;
    std::unordered_map<std::string, std::vector<std::string>> stream_group_batch_node_runtime_ids_;
    struct StreamGroupShareSetRuntime {
        std::string id;
        std::string source_ref;
        std::vector<std::string> members;
        std::vector<std::string> internal_channel_refs;
        std::shared_ptr<SharedSourceHub> hub;
    };
    mutable std::mutex stream_group_share_sets_mu_;
    std::unordered_map<std::string, std::vector<StreamGroupShareSetRuntime>> stream_group_share_sets_;
    mutable std::mutex stream_group_share_set_snapshots_mu_;
    std::unordered_map<std::string, std::vector<SharedHubSnapshot>> stream_group_share_set_snapshots_;

    struct StreamTaskLeaseInfo {
        std::vector<std::string> all_keys;
        std::vector<std::string> source_keys;
        std::string lease_owner_id;
    };
    struct StreamSourceLeaseState {
        std::string owner_id;
        uint32_t ref_count = 0;
    };
    std::mutex stream_channel_refs_mu_;
    std::unordered_map<std::string, uint32_t> stream_channel_ref_counts_;
    std::unordered_map<std::string, uint64_t> stream_channel_versions_;
    std::unordered_map<std::string, StreamSourceLeaseState> stream_source_leases_;
    std::unordered_set<std::string> stream_channel_mutating_;
    std::unordered_map<std::string, StreamTaskLeaseInfo> stream_task_leases_;

    struct RuntimeSharedSubscription {
        std::string hub_key;
        SharedSubscriberHandle handle;
    };
    mutable std::mutex shared_hubs_mu_;
    std::unordered_map<std::string, std::shared_ptr<SharedSourceHub>> shared_hubs_;
    mutable std::mutex runtime_subscriptions_mu_;
    std::unordered_map<std::string, std::vector<RuntimeSharedSubscription>> runtime_subscriptions_;

    mutable std::mutex stream_runtime_retention_mu_;
    std::unordered_map<std::string, int64_t> stream_runtime_terminal_ms_;
    std::unordered_map<std::string, int64_t> stream_runtime_last_access_ms_;
    std::unordered_map<std::string, std::string> stream_runtime_kind_;
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_
