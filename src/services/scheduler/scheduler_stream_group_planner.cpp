// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "scheduler_plugin.h"

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <framework/core/json_error_builder.h>
#include <framework/core/sql_parser.h>

#include "scheduler_internal_utils.h"

namespace flowsql {
namespace scheduler {

namespace {

constexpr size_t kDefaultMaxGroupEdges = 256;
constexpr size_t kDefaultMaxGroupShareSets = 16;
constexpr size_t kDefaultMaxGroupSqlBytes = 256 * 1024;

GroupNodeKind ParseNodeKind(const std::string& task_kind) {
    return task_kind == "batch" ? GroupNodeKind::kBatch : GroupNodeKind::kStream;
}

bool ParseChannelBaseLocal(const std::string& ref, std::string* base, std::string* err) {
    ParsedChannelRef parsed;
    if (!ParseChannelRef(ref, &parsed, err)) return false;
    if (!base) return false;
    *base = parsed.base;
    return true;
}

std::vector<std::string> CanonicalSourceKeySet(const std::vector<std::string>& keys) {
    std::set<std::string> uniq;
    for (const auto& k : keys) {
        if (!k.empty()) uniq.insert(k);
    }
    return std::vector<std::string>(uniq.begin(), uniq.end());
}

std::string JoinStrings(const std::vector<std::string>& values, const char* sep) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += sep;
        out += values[i];
    }
    return out;
}

std::string CanonicalKeysHash(const std::vector<std::string>& keys) {
    return JoinStrings(keys, "\x1f");
}

}  // namespace

int32_t SchedulerPlugin::BuildStreamGroupPlan(const StreamGroupExecuteRequest& req,
                                              StreamGroupBuildArtifacts* out,
                                              std::string* err_rsp) {
    if (!out || !err_rsp) return error::INTERNAL_ERROR;

    out->plans.clear();
    out->node_index.clear();
    out->node_resolved.clear();
    out->group_source_keys.clear();
    out->group_sink_keys.clear();
    out->share_set_plans.clear();

    std::vector<GroupNodePlan> plans;
    plans.reserve(req.sqls.size());
    std::unordered_map<std::string, size_t> node_index;
    std::unordered_map<std::string, StreamGroupNodeResolvedMeta> node_resolved;
    std::unordered_map<std::string, uint32_t> stream_sink_writer_counts;
    std::unordered_map<std::string, StreamChannelCapabilities> stream_sink_caps_map;
    std::unordered_map<std::string, uint32_t> non_stream_sink_writer_counts;
    std::unordered_map<std::string, std::vector<size_t>> sink_producers;
    std::unordered_map<std::string, bool> sink_base_stream_flags;
    std::vector<std::string> node_stream_sink_keys;
    node_stream_sink_keys.reserve(req.sqls.size());

    size_t edges = 0;
    size_t sql_bytes = 0;

    for (size_t i = 0; i < req.sqls.size(); ++i) {
        GroupNodePlan plan;
        plan.id = "n" + std::to_string(i + 1);
        plan.sql_index = i;
        plan.sql = req.sqls[i];
        node_index.emplace(plan.id, plans.size());

        sql_bytes += plan.sql.size();
        if (sql_bytes > kDefaultMaxGroupSqlBytes) {
            *err_rsp = BuildExecutionErrorJson(
                "group sql bytes exceed max_group_sql_bytes",
                ErrorCodeId::kStreamGroupDagTooLarge,
                ErrorStageId::kDagValidate);
            return error::BAD_REQUEST;
        }

        SqlParser parser;
        SqlStatement parsed = parser.Parse(plan.sql);
        if (!parsed.error.empty()) {
            *err_rsp = BuildExecutionErrorWithSqlIndexJson(
                "group node SQL parse failed: node=" + plan.id + ", " + parsed.error,
                ErrorCodeId::kStreamGroupDagInvalid,
                ErrorStageId::kDagValidate,
                i);
            return error::BAD_REQUEST;
        }
        if (parsed.sources.empty()) {
            *err_rsp = BuildExecutionErrorWithSqlIndexJson(
                "group node source channel not found: node=" + plan.id,
                ErrorCodeId::kStreamGroupDagInvalid,
                ErrorStageId::kDagValidate,
                i);
            return error::BAD_REQUEST;
        }

        std::unordered_set<std::string> dep_ids;
        std::vector<std::string> source_bases;
        source_bases.reserve(parsed.sources.size());
        for (const auto& source_ref : parsed.sources) {
            std::string source_base;
            std::string source_base_err;
            if (!ParseChannelBaseLocal(source_ref, &source_base, &source_base_err)) {
                *err_rsp = BuildExecutionErrorJson(
                    "group node source selector invalid: node=" + plan.id + ", source=" + source_ref,
                    ErrorCodeId::kStreamGroupDagInvalid,
                    ErrorStageId::kDagValidate);
                return error::BAD_REQUEST;
            }
            const std::string source_base_lc = ToLowerAscii(source_base);
            source_bases.push_back(source_base_lc);
            auto it = sink_producers.find(source_base_lc);
            if (it == sink_producers.end()) continue;
            for (size_t dep_idx : it->second) {
                if (dep_idx >= plans.size()) {
                    *err_rsp = BuildExecutionErrorJson(
                        "dependency index out of range: node=" + plan.id,
                        ErrorCodeId::kStreamGroupDagInvalid,
                        ErrorStageId::kDagValidate);
                    return error::BAD_REQUEST;
                }
                dep_ids.insert(plans[dep_idx].id);
            }
        }

        if (!dep_ids.empty()) {
            std::vector<size_t> dep_order;
            dep_order.reserve(dep_ids.size());
            for (const auto& dep_id : dep_ids) {
                auto dep_it = node_index.find(dep_id);
                if (dep_it != node_index.end()) dep_order.push_back(dep_it->second);
            }
            std::sort(dep_order.begin(), dep_order.end());
            dep_order.erase(std::unique(dep_order.begin(), dep_order.end()), dep_order.end());
            for (size_t dep_idx : dep_order) {
                plan.depends_on.push_back(plans[dep_idx].id);
            }
        }

        edges += plan.depends_on.size();
        if (edges > kDefaultMaxGroupEdges) {
            *err_rsp = BuildExecutionErrorJson(
                "group edges exceed max_group_edges",
                ErrorCodeId::kStreamGroupDagTooLarge,
                ErrorStageId::kDagValidate);
            return error::BAD_REQUEST;
        }

        std::string task_kind;
        std::string classify_err_rsp;
        const int32_t classify_rc = ClassifySqlTaskKind(plan.sql, &task_kind, &classify_err_rsp);
        if (classify_rc == error::OK) {
            plan.kind = ParseNodeKind(task_kind);
        } else {
            bool inferred_from_upstream = true;
            bool has_stream = false;
            bool has_non_stream = false;
            for (const auto& source_base_lc : source_bases) {
                auto producer_it = sink_producers.find(source_base_lc);
                if (producer_it == sink_producers.end()) {
                    inferred_from_upstream = false;
                    break;
                }
                auto flag_it = sink_base_stream_flags.find(source_base_lc);
                if (flag_it == sink_base_stream_flags.end()) {
                    inferred_from_upstream = false;
                    break;
                }
                if (flag_it->second) has_stream = true;
                else has_non_stream = true;
            }
            if (!inferred_from_upstream) {
                const std::string node_err = ExtractErrorMessage(classify_err_rsp);
                *err_rsp = BuildExecutionErrorWithSqlIndexJson(
                    "group node task kind classify failed: node=" + plan.id +
                        (node_err.empty() ? "" : (", " + node_err)),
                    ErrorCodeId::kStreamGroupNodeKindInvalid,
                    ErrorStageId::kDagValidate,
                    i);
                return classify_rc;
            }
            if (has_stream && has_non_stream) {
                *err_rsp = BuildExecutionErrorWithSqlIndexJson(
                    "group node has mixed source kinds: node=" + plan.id,
                    ErrorCodeId::kStreamGroupNodeKindInvalid,
                    ErrorStageId::kDagValidate,
                    i);
                return error::BAD_REQUEST;
            }
            plan.kind = has_stream ? GroupNodeKind::kStream : GroupNodeKind::kBatch;
        }

        SourceResolveResult source_resolved;
        std::string source_err_rsp;
        const int32_t source_rc = ResolveSourceBindings(parsed, &source_resolved, &source_err_rsp);
        if (source_rc != error::OK) {
            bool unresolved_from_upstream = true;
            for (const auto& source_base_lc : source_bases) {
                if (sink_producers.find(source_base_lc) == sink_producers.end()) {
                    unresolved_from_upstream = false;
                    break;
                }
            }
            if (!unresolved_from_upstream || plan.kind == GroupNodeKind::kStream) {
                const std::string node_err = ExtractErrorMessage(source_err_rsp);
                *err_rsp = BuildExecutionErrorWithSqlIndexJson(
                    "group node source resolve failed: node=" + plan.id +
                        (node_err.empty() ? "" : (", " + node_err)),
                    ErrorCodeId::kStreamGroupDagInvalid,
                    ErrorStageId::kDagValidate,
                    i);
                return source_rc;
            }
        }

        StreamGroupNodeResolvedMeta meta;
        meta.source_keys = source_resolved.source_keys;
        meta.resolved_sources = source_resolved.resolved_sources;
        meta.expand_rule = source_resolved.source_expand_rule;
        meta.stream_channels = source_resolved.stream_channels;
        meta.has_stream_source = source_resolved.has_stream_source;
        meta.has_non_stream_source = source_resolved.has_non_stream_source;
        node_resolved[plan.id] = std::move(meta);

        std::string sink_base;
        std::string sink_base_err;
        if (!ParseChannelBaseLocal(parsed.dest, &sink_base, &sink_base_err)) {
            *err_rsp = BuildExecutionErrorJson(
                "group node sink selector invalid: node=" + plan.id + ", sink=" + parsed.dest,
                ErrorCodeId::kStreamGroupDagInvalid,
                ErrorStageId::kDagValidate);
            return error::BAD_REQUEST;
        }

        const bool sink_is_stream = StartsWithIgnoreCase(sink_base, "stream.");
        const std::string sink_base_lc = ToLowerAscii(sink_base);
        auto sink_flag_it = sink_base_stream_flags.find(sink_base_lc);
        if (sink_flag_it == sink_base_stream_flags.end()) {
            sink_base_stream_flags.emplace(sink_base_lc, sink_is_stream);
        } else if (sink_flag_it->second != sink_is_stream) {
            *err_rsp = BuildExecutionErrorWithSqlIndexJson(
                "sink type conflict on same sink key: " + sink_base,
                ErrorCodeId::kStreamGroupDagInvalid,
                ErrorStageId::kDagValidate,
                i);
            return error::BAD_REQUEST;
        }

        if (sink_is_stream) {
            std::shared_ptr<IChannel> sink_owner;
            IChannel* sink_ch = FindChannel(sink_base, &sink_owner);
            auto* sink_stream = dynamic_cast<IStreamChannel*>(sink_ch);
            if (!sink_stream) {
                *err_rsp = BuildExecutionErrorJson(
                    "group node stream sink not found: node=" + plan.id + ", sink=" + sink_base,
                    ErrorCodeId::kStreamGroupDagInvalid,
                    ErrorStageId::kDagValidate);
                return error::BAD_REQUEST;
            }
            const std::string sink_key = MakeStreamChannelKey(sink_stream->Category(), sink_stream->Name());
            stream_sink_writer_counts[sink_key] += 1;
            stream_sink_caps_map[sink_key] = sink_stream->Capabilities();
            node_stream_sink_keys.push_back(sink_key);
        } else {
            non_stream_sink_writer_counts[sink_base_lc] += 1;
            node_stream_sink_keys.push_back("");
        }

        sink_producers[sink_base_lc].push_back(i);
        plans.push_back(std::move(plan));
    }

    for (auto& plan : plans) {
        plan.start_condition = GroupStartCondition::kOnRunning;
        for (const auto& dep_id : plan.depends_on) {
            auto dep_it = node_index.find(dep_id);
            if (dep_it == node_index.end() || dep_it->second >= plans.size()) continue;
            const auto dep_kind = plans[dep_it->second].kind;
            if (dep_kind == GroupNodeKind::kBatch || plan.kind == GroupNodeKind::kBatch) {
                plan.start_condition = GroupStartCondition::kOnFinished;
                break;
            }
        }
    }

    for (const auto& kv : stream_sink_writer_counts) {
        if (kv.second <= 1) continue;
        const auto caps_it = stream_sink_caps_map.find(kv.first);
        if (caps_it == stream_sink_caps_map.end()) continue;
        const auto& caps = caps_it->second;
        const bool put_mode_ok = caps.concurrency.put_mode == ProducerMode::MULTI;
        const bool producers_ok = caps.concurrency.max_producers == 0 ||
                                  caps.concurrency.max_producers >= kv.second;
        if (!put_mode_ok || !producers_ok) {
            *err_rsp = BuildSinkCapabilityMismatchJson(
                "shared stream sink capability mismatch: sink=" + kv.first +
                    ", writers=" + std::to_string(kv.second) +
                    ", required.put_mode=MULTI, actual.put_mode=" +
                    std::string(caps.concurrency.put_mode == ProducerMode::MULTI ? "MULTI" : "SINGLE") +
                    ", actual.max_producers=" + std::to_string(caps.concurrency.max_producers),
                ErrorStageId::kCapabilityCheck,
                kv.first,
                kv.second,
                caps.concurrency.put_mode,
                caps.concurrency.max_producers);
            return error::BAD_REQUEST;
        }
    }
    for (const auto& kv : non_stream_sink_writer_counts) {
        if (kv.second <= 1) continue;
        *err_rsp = BuildExecutionErrorJson(
            "shared non-stream sink only supports single writer in current sprint: sink=" + kv.first +
                ", writers=" + std::to_string(kv.second),
            ErrorCodeId::kStreamGroupNonStreamSinkMultiWriter,
            ErrorStageId::kCapabilityCheck);
        return error::BAD_REQUEST;
    }

    std::vector<uint32_t> indegree(plans.size(), 0);
    std::vector<std::vector<size_t>> graph(plans.size());
    for (size_t i = 0; i < plans.size(); ++i) {
        std::unordered_set<std::string> dep_dedup;
        for (const auto& dep : plans[i].depends_on) {
            if (dep == plans[i].id) {
                *err_rsp = BuildExecutionErrorJson(
                    "self dependency is not allowed: node=" + plans[i].id,
                    ErrorCodeId::kStreamGroupDagInvalid,
                    ErrorStageId::kDagValidate);
                return error::BAD_REQUEST;
            }
            if (!dep_dedup.insert(dep).second) continue;
            auto dep_it = node_index.find(dep);
            if (dep_it == node_index.end()) {
                *err_rsp = BuildExecutionErrorJson(
                    "dependency node not found: " + dep + ", node=" + plans[i].id,
                    ErrorCodeId::kStreamGroupNodeNotFound,
                    ErrorStageId::kDagValidate);
                return error::BAD_REQUEST;
            }
            graph[dep_it->second].push_back(i);
            indegree[i] += 1;
        }
    }

    std::queue<size_t> q;
    for (size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) q.push(i);
    }
    size_t visited = 0;
    while (!q.empty()) {
        const size_t u = q.front();
        q.pop();
        ++visited;
        for (size_t v : graph[u]) {
            if (--indegree[v] == 0) q.push(v);
        }
    }
    if (visited != plans.size()) {
        *err_rsp = BuildExecutionErrorJson(
            "dag has cycle dependency",
            ErrorCodeId::kStreamGroupDagCycleDetected,
            ErrorStageId::kDagValidate);
        return error::BAD_REQUEST;
    }

    std::vector<StreamGroupShareSetPlan> share_set_plans;
    std::map<std::string, std::vector<size_t>> stream_source_groups;
    for (size_t i = 0; i < plans.size(); ++i) {
        if (plans[i].kind != GroupNodeKind::kStream) continue;
        const auto resolved_it = node_resolved.find(plans[i].id);
        if (resolved_it == node_resolved.end()) continue;
        const auto canonical = CanonicalSourceKeySet(resolved_it->second.source_keys);
        if (canonical.empty()) {
            *err_rsp = BuildSourceMismatchErrorJson(
                "stream node has empty canonical source keys: node=" + plans[i].id,
                ErrorStageId::kDagValidate,
                "",
                plans[i].id,
                {},
                {});
            return error::BAD_REQUEST;
        }
        stream_source_groups[CanonicalKeysHash(canonical)].push_back(i);
    }

    int share_set_seq = 0;
    for (const auto& group_item : stream_source_groups) {
        const auto& members = group_item.second;
        if (members.size() < 2) continue;

        StreamGroupShareSetPlan ss;
        ss.id = "s" + std::to_string(++share_set_seq);

        const size_t first_idx = members.front();
        const auto resolved_it = node_resolved.find(plans[first_idx].id);
        if (resolved_it == node_resolved.end()) continue;

        ss.canonical_source_keys = CanonicalSourceKeySet(resolved_it->second.source_keys);
        ss.source_ref = JoinStrings(resolved_it->second.resolved_sources, ",");
        ss.source_channels = resolved_it->second.stream_channels;
        if (ss.source_channels.empty()) {
            *err_rsp = BuildSourceMismatchErrorJson(
                "auto source_share_set has empty source channels: set=" + ss.id,
                ErrorStageId::kDagValidate,
                ss.id,
                plans[first_idx].id,
                ss.canonical_source_keys,
                {});
            return error::BAD_REQUEST;
        }

        for (size_t member_idx : members) {
            const std::string& member_node_id = plans[member_idx].id;
            const auto member_it = node_resolved.find(member_node_id);
            if (member_it == node_resolved.end()) continue;
            const auto member_keys = CanonicalSourceKeySet(member_it->second.source_keys);
            if (member_keys != ss.canonical_source_keys) {
                *err_rsp = BuildSourceMismatchErrorJson(
                    "source_share_set canonical source mismatch: set=" + ss.id + ", node=" + member_node_id,
                    ErrorStageId::kDagValidate,
                    ss.id,
                    member_node_id,
                    ss.canonical_source_keys,
                    member_keys);
                return error::BAD_REQUEST;
            }
            ss.members.push_back(member_node_id);
        }
        share_set_plans.push_back(std::move(ss));
    }
    if (share_set_plans.size() > kDefaultMaxGroupShareSets) {
        *err_rsp = BuildExecutionErrorJson(
            "auto source_share_sets exceed max_group_share_sets",
            ErrorCodeId::kStreamGroupDagTooLarge,
            ErrorStageId::kDagValidate);
        return error::BAD_REQUEST;
    }

    std::vector<std::string> group_source_keys;
    std::vector<std::string> group_sink_keys;
    for (size_t i = 0; i < plans.size(); ++i) {
        auto resolved_it = node_resolved.find(plans[i].id);
        if (resolved_it != node_resolved.end()) {
            group_source_keys.insert(group_source_keys.end(),
                                     resolved_it->second.source_keys.begin(),
                                     resolved_it->second.source_keys.end());
        }
        if (i < node_stream_sink_keys.size() && !node_stream_sink_keys[i].empty()) {
            group_sink_keys.push_back(node_stream_sink_keys[i]);
        }
    }

    out->plans = std::move(plans);
    out->node_index = std::move(node_index);
    out->node_resolved = std::move(node_resolved);
    out->group_source_keys = std::move(group_source_keys);
    out->group_sink_keys = std::move(group_sink_keys);
    out->share_set_plans = std::move(share_set_plans);
    return error::OK;
}

int32_t SchedulerPlugin::ValidateStreamGroupPlan(const StreamGroupBuildArtifacts& build,
                                                 std::string* err_rsp) {
    if (!err_rsp) return error::INTERNAL_ERROR;
    if (build.plans.size() < 2) {
        *err_rsp = BuildExecutionErrorJson(
            "group execution requires at least two SQL statements",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }
    for (const auto& plan : build.plans) {
        if (build.node_index.find(plan.id) == build.node_index.end()) {
            *err_rsp = BuildExecutionErrorJson(
                "group node index missing: " + plan.id,
                ErrorCodeId::kStreamGroupDagInvalid,
                ErrorStageId::kDagValidate);
            return error::BAD_REQUEST;
        }
    }
    return error::OK;
}

}  // namespace scheduler
}  // namespace flowsql
