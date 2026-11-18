// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "scheduler_plugin.h"

#include <framework/core/json_error_builder.h>
#include <framework/core/sql_parser.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace scheduler {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string ToLowerAsciiCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string ReadString(const rapidjson::Value& obj,
                       const char* key,
                       const std::string& def = "") {
    if (!obj.IsObject() || !key) return def;
    auto it = obj.FindMember(key);
    if (it == obj.MemberEnd() || !it->value.IsString()) return def;
    return it->value.GetString();
}

uint64_t ReadUint64(const rapidjson::Value& obj, const char* key, uint64_t def = 0) {
    if (!obj.IsObject() || !key) return def;
    auto it = obj.FindMember(key);
    if (it == obj.MemberEnd()) return def;
    if (it->value.IsUint64()) return it->value.GetUint64();
    if (it->value.IsUint()) return static_cast<uint64_t>(it->value.GetUint());
    if (it->value.IsInt64()) {
        const int64_t v = it->value.GetInt64();
        return v < 0 ? 0 : static_cast<uint64_t>(v);
    }
    if (it->value.IsInt()) {
        const int v = it->value.GetInt();
        return v < 0 ? 0 : static_cast<uint64_t>(v);
    }
    return def;
}

int64_t ReadInt64(const rapidjson::Value& obj, const char* key, int64_t def = 0) {
    if (!obj.IsObject() || !key) return def;
    auto it = obj.FindMember(key);
    if (it == obj.MemberEnd()) return def;
    if (it->value.IsInt64()) return it->value.GetInt64();
    if (it->value.IsInt()) return static_cast<int64_t>(it->value.GetInt());
    if (it->value.IsUint64()) return static_cast<int64_t>(it->value.GetUint64());
    if (it->value.IsUint()) return static_cast<int64_t>(it->value.GetUint());
    return def;
}

bool IsTerminalStatus(const std::string& status) {
    const std::string s = ToLowerAsciiCopy(status);
    return s == "completed" || s == "stopped" || s == "cancelled" || s == "failed" || s == "timeout";
}

int StatusRank(const std::string& status) {
    const std::string s = ToLowerAsciiCopy(status);
    if (s == "failed" || s == "timeout") return 5;
    if (s == "cancelled") return 4;
    if (s == "completed" || s == "stopped") return 3;
    if (s == "running" || s == "stopping" || s == "preparing" || s == "created") return 2;
    if (s == "pending") return 1;
    return 0;
}

std::string EdgeStatusFromNode(const std::string& status) {
    const std::string s = ToLowerAsciiCopy(status);
    if (s == "running" || s == "stopping") return "active";
    if (s == "failed" || s == "timeout") return "blocked";
    if (IsTerminalStatus(s)) return "done";
    return "idle";
}

std::string BuildOperatorName(const SqlStatement& stmt) {
    if (!stmt.operators.empty()) {
        std::ostringstream oss;
        for (size_t i = 0; i < stmt.operators.size(); ++i) {
            if (i != 0) oss << " -> ";
            oss << stmt.operators[i].category << "." << stmt.operators[i].name;
        }
        return oss.str();
    }
    return "transfer";
}

struct RuntimeNodeState {
    std::string node_id;
    std::string node_kind;
    std::string status;
    std::string phase;
    std::string error_code;
    std::string error_message;
    std::string start_condition;
    std::vector<std::string> depends_on;
    uint64_t processed_rows = 0;
    uint64_t output_rows = 0;
    int64_t started_ms = 0;
    int64_t finished_ms = 0;
};

struct SqlView {
    std::vector<std::string> sources;
    std::string operator_name;
    std::string sink;
};

struct ChannelNodeAggregate {
    int64_t sql_index = -1;
    std::string status = "pending";
    uint64_t processed_rows = 0;
    uint64_t output_rows = 0;
    int64_t started_ms = 0;
    int64_t finished_ms = 0;
};

struct GraphEdge {
    std::string id;
    std::string from;
    std::string to;
    std::string edge_kind;
    std::string trigger;
    std::string status;
    uint64_t rows = 0;
    uint64_t fire_count = 0;
    int64_t last_fire_at_ms = 0;
};

std::string MakeEdgeId(const std::string& edge_kind,
                       const std::string& trigger,
                       const std::string& from,
                       const std::string& to,
                       size_t salt) {
    std::ostringstream oss;
    oss << "edge:" << edge_kind << ":" << trigger << ":" << from << "->" << to << "#" << salt;
    return oss.str();
}

void ParseRuntimeNodes(const rapidjson::Document& status_doc,
                       std::map<size_t, RuntimeNodeState>* runtime_nodes,
                       std::unordered_map<std::string, size_t>* node_id_to_sql,
                       std::map<size_t, std::vector<std::string>>* resolved_sources) {
    if (!runtime_nodes || !node_id_to_sql || !resolved_sources) return;
    runtime_nodes->clear();
    node_id_to_sql->clear();
    resolved_sources->clear();

    if (!status_doc.IsObject()) return;

    if (status_doc.HasMember("nodes") && status_doc["nodes"].IsArray()) {
        for (const auto& node : status_doc["nodes"].GetArray()) {
            if (!node.IsObject()) continue;
            size_t sql_index = 0;
            if (node.HasMember("sql_index") && node["sql_index"].IsUint64()) {
                sql_index = static_cast<size_t>(node["sql_index"].GetUint64());
            } else if (node.HasMember("sql_index") && node["sql_index"].IsUint()) {
                sql_index = static_cast<size_t>(node["sql_index"].GetUint());
            }
            RuntimeNodeState s;
            s.node_id = ReadString(node, "id");
            s.node_kind = ReadString(node, "node_kind");
            s.status = ReadString(node, "status", "pending");
            s.phase = ReadString(node, "phase");
            s.error_code = ReadString(node, "error_code");
            s.error_message = ReadString(node, "error_message", ReadString(node, "last_error"));
            s.start_condition = ReadString(node, "start_condition", "on_running");
            s.processed_rows = ReadUint64(node, "processed_rows", 0);
            s.output_rows = ReadUint64(node, "output_rows", 0);
            s.started_ms = ReadInt64(node, "started_ms", 0);
            s.finished_ms = ReadInt64(node, "finished_ms", 0);
            if (node.HasMember("depends_on") && node["depends_on"].IsArray()) {
                for (const auto& dep : node["depends_on"].GetArray()) {
                    if (dep.IsString()) s.depends_on.push_back(dep.GetString());
                }
            }
            (*runtime_nodes)[sql_index] = std::move(s);
            const auto& stored = runtime_nodes->at(sql_index);
            if (!stored.node_id.empty()) {
                (*node_id_to_sql)[stored.node_id] = sql_index;
            }
        }
    }

    if (status_doc.HasMember("resolved_sources") && status_doc["resolved_sources"].IsArray()) {
        const auto& rs = status_doc["resolved_sources"];
        if (!rs.Empty() && rs[0].IsString()) {
            std::vector<std::string> list;
            list.reserve(rs.Size());
            for (const auto& it : rs.GetArray()) {
                if (it.IsString()) list.push_back(it.GetString());
            }
            if (!list.empty()) (*resolved_sources)[0] = std::move(list);
        } else {
            for (const auto& item : rs.GetArray()) {
                if (!item.IsObject()) continue;
                const std::string node_id = ReadString(item, "node_id");
                auto idx_it = node_id_to_sql->find(node_id);
                if (idx_it == node_id_to_sql->end()) continue;
                if (!item.HasMember("sources") || !item["sources"].IsArray()) continue;
                std::vector<std::string> list;
                for (const auto& src : item["sources"].GetArray()) {
                    if (src.IsString()) list.push_back(src.GetString());
                }
                if (!list.empty()) (*resolved_sources)[idx_it->second] = std::move(list);
            }
        }
    }

    if (runtime_nodes->empty()) {
        RuntimeNodeState fallback;
        fallback.status = ReadString(status_doc, "status", "pending");
        fallback.phase = ReadString(status_doc, "runtime_status");
        fallback.error_code = ReadString(status_doc, "error_code");
        fallback.error_message = ReadString(status_doc, "error_message");
        fallback.processed_rows = ReadUint64(status_doc, "processed_rows", 0);
        fallback.output_rows = ReadUint64(status_doc, "output_rows",
                                          ReadUint64(status_doc, "result_row_count", 0));
        fallback.started_ms = ReadInt64(status_doc, "started_ms", 0);
        fallback.finished_ms = ReadInt64(status_doc, "finished_ms", 0);
        (*runtime_nodes)[0] = std::move(fallback);
    }
}

}  // namespace

int32_t SchedulerPlugin::HandleRuntimeGraphQuery(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = BuildErrorWithCodeJson("invalid request, expected {\"task_id\":\"...\"}",
                                     "RUNTIME_GRAPH_CURSOR_INVALID");
        return error::BAD_REQUEST;
    }

    const std::string runtime_task_id = doc["task_id"].GetString();
    if (runtime_task_id.empty()) {
        rsp = BuildErrorWithCodeJson("invalid request, task_id must not be empty",
                                     "RUNTIME_GRAPH_CURSOR_INVALID");
        return error::BAD_REQUEST;
    }

    uint64_t cursor = 0;
    if (doc.HasMember("cursor")) {
        const auto& c = doc["cursor"];
        if (c.IsUint64()) {
            cursor = c.GetUint64();
        } else if (c.IsUint()) {
            cursor = c.GetUint();
        } else if (c.IsInt64()) {
            if (c.GetInt64() < 0) {
                rsp = BuildErrorWithCodeJson("invalid request, cursor must be >= 0",
                                             "RUNTIME_GRAPH_CURSOR_INVALID");
                return error::BAD_REQUEST;
            }
            cursor = static_cast<uint64_t>(c.GetInt64());
        } else if (c.IsInt()) {
            if (c.GetInt() < 0) {
                rsp = BuildErrorWithCodeJson("invalid request, cursor must be >= 0",
                                             "RUNTIME_GRAPH_CURSOR_INVALID");
                return error::BAD_REQUEST;
            }
            cursor = static_cast<uint64_t>(c.GetInt());
        } else {
            rsp = BuildErrorWithCodeJson("invalid request, cursor must be integer",
                                         "RUNTIME_GRAPH_CURSOR_INVALID");
            return error::BAD_REQUEST;
        }
    }

    bool include_events = true;
    if (doc.HasMember("include_events")) {
        if (!doc["include_events"].IsBool()) {
            rsp = BuildErrorWithCodeJson("invalid request, include_events must be bool",
                                         "RUNTIME_GRAPH_CURSOR_INVALID");
            return error::BAD_REQUEST;
        }
        include_events = doc["include_events"].GetBool();
    }

    std::string task_kind = "stream";
    if (doc.HasMember("task_kind") && doc["task_kind"].IsString()) {
        task_kind = ToLowerAsciiCopy(doc["task_kind"].GetString());
    }

    std::vector<std::string> sqls;
    if (doc.HasMember("sqls")) {
        if (!doc["sqls"].IsArray()) {
            rsp = BuildErrorWithCodeJson("invalid request, sqls must be string array",
                                         "RUNTIME_GRAPH_CURSOR_INVALID");
            return error::BAD_REQUEST;
        }
        sqls.reserve(doc["sqls"].Size());
        for (const auto& it : doc["sqls"].GetArray()) {
            if (!it.IsString()) {
                rsp = BuildErrorWithCodeJson("invalid request, sqls must be string array",
                                             "RUNTIME_GRAPH_CURSOR_INVALID");
                return error::BAD_REQUEST;
            }
            const std::string sql = it.GetString();
            if (!sql.empty()) sqls.push_back(sql);
        }
    }

    rapidjson::Document status_doc;
    std::string status_rsp;
    int32_t status_rc = error::OK;
    if (task_kind == "batch") {
        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
        req_w.StartObject();
        req_w.Key("runtime_task_id");
        req_w.String(runtime_task_id.c_str());
        req_w.EndObject();
        status_rc = HandleBatchStatus("/scheduler/batch/status", req_buf.GetString(), status_rsp);
    } else {
        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
        req_w.StartObject();
        req_w.Key("task_id");
        req_w.String(runtime_task_id.c_str());
        req_w.EndObject();
        status_rc = HandleStreamStatus("/scheduler/stream/status", req_buf.GetString(), status_rsp);
    }
    if (status_rc != error::OK) {
        if (status_rc == error::NOT_FOUND) {
            rsp = BuildErrorWithCodeJson("runtime task not found: " + runtime_task_id,
                                         "RUNTIME_GRAPH_RUNTIME_NOT_FOUND");
            return error::NOT_FOUND;
        }
        rsp = status_rsp.empty()
            ? BuildErrorWithCodeJson("runtime graph build failed", "RUNTIME_GRAPH_BUILD_FAILED")
            : status_rsp;
        return status_rc;
    }

    status_doc.Parse(status_rsp.c_str());
    if (status_doc.HasParseError() || !status_doc.IsObject()) {
        rsp = BuildErrorWithCodeJson("invalid runtime status payload", "RUNTIME_GRAPH_BUILD_FAILED");
        return error::INTERNAL_ERROR;
    }

    std::map<size_t, RuntimeNodeState> runtime_nodes;
    std::unordered_map<std::string, size_t> node_id_to_sql;
    std::map<size_t, std::vector<std::string>> resolved_sources;
    ParseRuntimeNodes(status_doc, &runtime_nodes, &node_id_to_sql, &resolved_sources);

    std::map<size_t, SqlView> sql_views;
    SqlParser parser;
    for (size_t i = 0; i < sqls.size(); ++i) {
        SqlView view;
        auto stmt = parser.Parse(sqls[i]);
        if (!stmt.error.empty()) {
            view.sources = {"unknown.source"};
            view.operator_name = "operator";
            view.sink = "unknown.sink";
        } else {
            view.sources = stmt.sources;
            if (view.sources.empty()) view.sources.push_back("unknown.source");
            view.operator_name = BuildOperatorName(stmt);
            view.sink = stmt.dest.empty() ? "unknown.sink" : stmt.dest;
        }
        auto it = resolved_sources.find(i);
        if (it != resolved_sources.end() && !it->second.empty()) {
            view.sources = it->second;
        }
        sql_views[i] = std::move(view);
    }

    if (sql_views.empty()) {
        for (const auto& kv : runtime_nodes) {
            SqlView view;
            auto it = resolved_sources.find(kv.first);
            if (it != resolved_sources.end() && !it->second.empty()) {
                view.sources = it->second;
            } else {
                view.sources = {"unknown.source"};
            }
            view.operator_name = "operator";
            view.sink = "unknown.sink";
            sql_views[kv.first] = std::move(view);
        }
    }

    std::map<std::string, ChannelNodeAggregate> channels;
    std::vector<GraphEdge> edges;
    size_t edge_seq = 0;

    auto update_channel = [&channels](const std::string& name,
                                      size_t sql_index,
                                      const RuntimeNodeState& st) {
        auto& c = channels[name];
        if (c.sql_index < 0) c.sql_index = static_cast<int64_t>(sql_index);
        if (StatusRank(st.status) >= StatusRank(c.status)) c.status = st.status;
        c.processed_rows = std::max(c.processed_rows, st.processed_rows);
        c.output_rows = std::max(c.output_rows, st.output_rows);
        if (st.started_ms > 0 && (c.started_ms == 0 || st.started_ms < c.started_ms)) c.started_ms = st.started_ms;
        if (st.finished_ms > c.finished_ms) c.finished_ms = st.finished_ms;
    };

    for (const auto& kv : sql_views) {
        const size_t sql_index = kv.first;
        const auto node_it = runtime_nodes.find(sql_index);
        RuntimeNodeState st;
        if (node_it != runtime_nodes.end()) {
            st = node_it->second;
        } else {
            st.status = ReadString(status_doc, "status", "pending");
            st.phase = ReadString(status_doc, "runtime_status");
            st.error_code = ReadString(status_doc, "error_code");
            st.error_message = ReadString(status_doc, "error_message");
            st.processed_rows = ReadUint64(status_doc, "processed_rows", 0);
            st.output_rows = ReadUint64(status_doc, "output_rows", ReadUint64(status_doc, "result_row_count", 0));
            st.started_ms = ReadInt64(status_doc, "started_ms", 0);
            st.finished_ms = ReadInt64(status_doc, "finished_ms", 0);
        }

        const std::string op_id = "operator:sql" + std::to_string(sql_index);
        for (const auto& src : kv.second.sources) {
            const std::string src_name = src.empty() ? "unknown.source" : src;
            const std::string src_id = "channel:" + src_name;
            update_channel(src_name, sql_index, st);

            GraphEdge edge;
            edge.id = MakeEdgeId("data", "on_data", src_id, op_id, edge_seq++);
            edge.from = src_id;
            edge.to = op_id;
            edge.edge_kind = "data";
            edge.trigger = "on_data";
            edge.status = EdgeStatusFromNode(st.status);
            edge.rows = st.processed_rows;
            edges.push_back(std::move(edge));
        }

        const std::string sink_name = kv.second.sink.empty() ? "unknown.sink" : kv.second.sink;
        const std::string sink_id = "channel:" + sink_name;
        update_channel(sink_name, sql_index, st);

        GraphEdge sink_edge;
        sink_edge.id = MakeEdgeId("data", "on_data", op_id, sink_id, edge_seq++);
        sink_edge.from = op_id;
        sink_edge.to = sink_id;
        sink_edge.edge_kind = "data";
        sink_edge.trigger = "on_data";
        sink_edge.status = EdgeStatusFromNode(st.status);
        sink_edge.rows = st.output_rows;
        edges.push_back(std::move(sink_edge));
    }

    auto has_data_dependency = [&sql_views](size_t upstream_sql_index, size_t downstream_sql_index) {
        auto up_it = sql_views.find(upstream_sql_index);
        auto down_it = sql_views.find(downstream_sql_index);
        if (up_it == sql_views.end() || down_it == sql_views.end()) return false;
        const std::string& sink = up_it->second.sink;
        if (sink.empty()) return false;
        for (const auto& src : down_it->second.sources) {
            if (src == sink) return true;
        }
        return false;
    };

    for (const auto& kv : runtime_nodes) {
        const size_t sql_index = kv.first;
        const RuntimeNodeState& st = kv.second;
        for (const auto& dep_id : st.depends_on) {
            auto dep_it = node_id_to_sql.find(dep_id);
            if (dep_it == node_id_to_sql.end()) continue;
            const size_t dep_sql_index = dep_it->second;
            if (dep_sql_index == sql_index) continue;
            if (has_data_dependency(dep_sql_index, sql_index)) {
                // 数据边已表达 upstream -> downstream 依赖时，不重复生成 on_finish 控制边。
                continue;
            }
            const std::string from = "operator:sql" + std::to_string(dep_sql_index);
            const std::string to = "operator:sql" + std::to_string(sql_index);

            GraphEdge edge;
            edge.id = MakeEdgeId("control",
                                 ToLowerAsciiCopy(st.start_condition) == "on_finished" ? "on_finish" : "on_start",
                                 from,
                                 to,
                                 edge_seq++);
            edge.from = from;
            edge.to = to;
            edge.edge_kind = "control";
            edge.trigger = (ToLowerAsciiCopy(st.start_condition) == "on_finished") ? "on_finish" : "on_start";
            edge.status = EdgeStatusFromNode(st.status);
            edges.push_back(std::move(edge));
        }
    }

    std::sort(edges.begin(), edges.end(), [](const GraphEdge& a, const GraphEdge& b) {
        return a.id < b.id;
    });

    const int64_t now_ms = NowMs();
    const std::string runtime_kind = ReadString(
        status_doc,
        "runtime_kind",
        task_kind == "batch" ? "batch" : "single");
    const std::string overall_status = ReadString(status_doc, "status", "unknown");

    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> w(out);
    w.StartObject();
    w.Key("task_id");
    w.String(runtime_task_id.c_str());
    w.Key("runtime_task_id");
    w.String(runtime_task_id.c_str());
    w.Key("task_kind");
    w.String(task_kind.c_str());
    w.Key("runtime_kind");
    w.String(runtime_kind.c_str());
    w.Key("status");
    w.String(overall_status.c_str());
    w.Key("snapshot_time_ms");
    w.Int64(now_ms);

    w.Key("nodes");
    w.StartArray();
    for (const auto& ch : channels) {
        w.StartObject();
        w.Key("id");
        w.String(("channel:" + ch.first).c_str());
        w.Key("kind");
        w.String("channel");
        w.Key("name");
        w.String(ch.first.c_str());
        w.Key("sql_index");
        w.Int64(ch.second.sql_index);
        w.Key("status");
        w.String(ch.second.status.c_str());
        w.Key("phase");
        w.String("");
        w.Key("processed_rows");
        w.Uint64(ch.second.processed_rows);
        w.Key("output_rows");
        w.Uint64(ch.second.output_rows);
        w.Key("error_code");
        w.String("");
        w.Key("error_message");
        w.String("");
        w.Key("start_at_ms");
        w.Int64(ch.second.started_ms);
        w.Key("end_at_ms");
        w.Int64(ch.second.finished_ms);
        w.EndObject();
    }

    for (const auto& kv : sql_views) {
        const size_t sql_index = kv.first;
        RuntimeNodeState st;
        auto it = runtime_nodes.find(sql_index);
        if (it != runtime_nodes.end()) {
            st = it->second;
        } else {
            st.status = overall_status;
            st.phase = ReadString(status_doc, "runtime_status");
            st.error_code = ReadString(status_doc, "error_code");
            st.error_message = ReadString(status_doc, "error_message");
            st.processed_rows = ReadUint64(status_doc, "processed_rows", 0);
            st.output_rows = ReadUint64(status_doc, "output_rows", ReadUint64(status_doc, "result_row_count", 0));
            st.started_ms = ReadInt64(status_doc, "started_ms", 0);
            st.finished_ms = ReadInt64(status_doc, "finished_ms", 0);
        }
        w.StartObject();
        w.Key("id");
        w.String(("operator:sql" + std::to_string(sql_index)).c_str());
        w.Key("kind");
        w.String("operator");
        w.Key("name");
        w.String(kv.second.operator_name.c_str());
        w.Key("sql_index");
        w.Uint64(static_cast<uint64_t>(sql_index));
        w.Key("status");
        w.String(st.status.c_str());
        w.Key("phase");
        w.String(st.phase.c_str());
        w.Key("processed_rows");
        w.Uint64(st.processed_rows);
        w.Key("output_rows");
        w.Uint64(st.output_rows);
        w.Key("error_code");
        w.String(st.error_code.c_str());
        w.Key("error_message");
        w.String(st.error_message.c_str());
        w.Key("start_at_ms");
        w.Int64(st.started_ms);
        w.Key("end_at_ms");
        w.Int64(st.finished_ms);
        w.EndObject();
    }
    w.EndArray();

    w.Key("edges");
    w.StartArray();
    for (const auto& e : edges) {
        w.StartObject();
        w.Key("id");
        w.String(e.id.c_str());
        w.Key("from");
        w.String(e.from.c_str());
        w.Key("to");
        w.String(e.to.c_str());
        w.Key("edge_kind");
        w.String(e.edge_kind.c_str());
        w.Key("trigger");
        w.String(e.trigger.c_str());
        w.Key("status");
        w.String(e.status.c_str());
        w.Key("rows");
        w.Uint64(e.rows);
        w.Key("fire_count");
        w.Uint64(e.fire_count);
        w.Key("last_fire_at_ms");
        w.Int64(e.last_fire_at_ms);
        w.EndObject();
    }
    w.EndArray();

    w.Key("events");
    w.StartArray();
    if (include_events) {
        // Sprint 18 阶段一先输出空事件流，占位 cursor 契约。
    }
    w.EndArray();

    w.Key("next_cursor");
    w.Uint64(cursor);
    w.EndObject();
    rsp = out.GetString();
    return error::OK;
}

}  // namespace scheduler
}  // namespace flowsql
