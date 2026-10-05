// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "task_plugin.h"

#include <common/error_code.h>
#include <common/log.h>
#include <framework/core/json_error_builder.h>
#include <framework/core/sql_text_splitter.h>
#include <framework/core/sql_parser.h>
#include <framework/interfaces/ichannel_registry.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <set>
#include <unordered_map>

#include "task_store_sqlite.h"
#include "task_sql_utils.h"

namespace flowsql {
namespace task {

namespace {

const char* TaskStatusToName(TaskStatus s) {
    switch (s) {
        case TaskStatus::kPending: return "pending";
        case TaskStatus::kRunning: return "running";
        case TaskStatus::kCompleted: return "completed";
        case TaskStatus::kFailed: return "failed";
        case TaskStatus::kStopped: return "stopped";
        case TaskStatus::kCancelled: return "cancelled";
        case TaskStatus::kTimeout: return "timeout";
        default: return "failed";
    }
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string BuildRuntimeGraphOperatorName(const SqlStatement& stmt) {
    if (!stmt.operators.empty()) {
        std::string out;
        for (size_t i = 0; i < stmt.operators.size(); ++i) {
            if (i != 0) out += " -> ";
            out += stmt.operators[i].category;
            out += ".";
            out += stmt.operators[i].name;
        }
        return out;
    }
    if (!stmt.op_category.empty() && !stmt.op_name.empty()) {
        return stmt.op_category + "." + stmt.op_name;
    }
    return "transfer";
}

std::string NormalizeRuntimeGraphChannelName(const std::string& raw_name) {
    const std::string name = raw_name.empty() ? "unknown.channel" : raw_name;
    if (name.rfind("stream_hub.", 0) != 0) {
        return name;
    }

    const size_t left = name.find('[');
    if (left == std::string::npos || left == 0 || name.back() != ']') {
        return name;
    }
    if (left + 1 >= name.size() - 1) {
        return name;
    }

    bool valid_selector = true;
    for (size_t i = left + 1; i + 1 < name.size(); ++i) {
        const char ch = name[i];
        if (ch == '*') continue;
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            valid_selector = false;
            break;
        }
    }
    if (!valid_selector) {
        return name;
    }

    return name.substr(0, left);
}

std::string BuildLocalRuntimeGraph(const TaskRecord& rec,
                                   const std::vector<std::string>& sqls,
                                   uint64_t cursor,
                                   bool include_events,
                                   const std::string& runtime_kind) {
    struct SqlView {
        std::vector<std::string> sources;
        std::string sink;
        std::string op_name;
    };
    struct Edge {
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

    std::vector<SqlView> sql_views;
    sql_views.reserve(sqls.size());
    SqlParser parser;
    for (const auto& sql : sqls) {
        SqlView view;
        const auto stmt = parser.Parse(sql);
        if (!stmt.error.empty()) {
            view.sources = {"unknown.source"};
            view.sink = "unknown.sink";
            view.op_name = "operator";
        } else {
            view.sources = stmt.sources;
            if (view.sources.empty() && !stmt.source.empty()) {
                view.sources.push_back(stmt.source);
            }
            if (view.sources.empty()) view.sources = {"unknown.source"};
            for (auto& src : view.sources) {
                src = NormalizeRuntimeGraphChannelName(src);
            }
            view.sink = stmt.dest.empty() ? "unknown.sink" : stmt.dest;
            view.sink = NormalizeRuntimeGraphChannelName(view.sink);
            view.op_name = BuildRuntimeGraphOperatorName(stmt);
        }
        sql_views.push_back(std::move(view));
    }
    if (sql_views.empty()) {
        SqlView view;
        view.sources = {"unknown.source"};
        view.sink = NormalizeRuntimeGraphChannelName(rec.result_target.empty() ? "unknown.sink" : rec.result_target);
        view.op_name = rec.task_kind == "batch" ? "batch_worker" : "operator";
        sql_views.push_back(std::move(view));
    }

    const std::string overall_status = TaskStatusToName(rec.status);
    const int64_t now_ms = NowMs();
    const std::string runtime_task_id = rec.runtime_task_id.empty() ? rec.task_id : rec.runtime_task_id;

    std::unordered_map<std::string, size_t> channel_indices;
    std::unordered_map<std::string, int> channel_sql_index;
    std::vector<std::string> channel_names;
    channel_names.reserve(sql_views.size() * 2);
    auto ensure_channel = [&](const std::string& name, int sql_index) {
        const std::string key = NormalizeRuntimeGraphChannelName(name);
        auto it = channel_indices.find(key);
        if (it == channel_indices.end()) {
            channel_indices[key] = channel_names.size();
            channel_names.push_back(key);
            channel_sql_index[key] = sql_index;
            return;
        }
        auto idx_it = channel_sql_index.find(key);
        if (idx_it == channel_sql_index.end()) {
            channel_sql_index[key] = sql_index;
            return;
        }
        idx_it->second = std::min(idx_it->second, sql_index);
    };
    for (size_t i = 0; i < sql_views.size(); ++i) {
        const auto& view = sql_views[i];
        for (const auto& src : view.sources) ensure_channel(src, static_cast<int>(i));
        ensure_channel(view.sink, static_cast<int>(i));
    }

    std::vector<Edge> edges;
    edges.reserve(sql_views.size() * 2);
    size_t edge_seq = 0;
    const std::string data_edge_status =
        (overall_status == "running" || overall_status == "pending") ? "active" : "done";
    for (size_t i = 0; i < sql_views.size(); ++i) {
        const std::string op_id = "operator:sql" + std::to_string(i);
        for (const auto& src : sql_views[i].sources) {
            Edge edge;
            edge.id = "edge:data:on_data:" + std::to_string(edge_seq++);
            edge.from = "channel:" + src;
            edge.to = op_id;
            edge.edge_kind = "data";
            edge.trigger = "on_data";
            edge.status = data_edge_status;
            edge.rows = (i == sql_views.size() - 1) ? static_cast<uint64_t>(std::max<int64_t>(rec.result_row_count, 0)) : 0;
            edges.push_back(std::move(edge));
        }
        Edge sink_edge;
        sink_edge.id = "edge:data:on_data:" + std::to_string(edge_seq++);
        sink_edge.from = op_id;
        sink_edge.to = "channel:" + sql_views[i].sink;
        sink_edge.edge_kind = "data";
        sink_edge.trigger = "on_data";
        sink_edge.status = data_edge_status;
        sink_edge.rows = (i == sql_views.size() - 1) ? static_cast<uint64_t>(std::max<int64_t>(rec.result_row_count, 0)) : 0;
        edges.push_back(std::move(sink_edge));
    }
    for (size_t i = 1; i < sql_views.size(); ++i) {
        Edge control_edge;
        control_edge.id = "edge:control:on_finish:" + std::to_string(edge_seq++);
        control_edge.from = "operator:sql" + std::to_string(i - 1);
        control_edge.to = "operator:sql" + std::to_string(i);
        control_edge.edge_kind = "control";
        control_edge.trigger = "on_finish";
        control_edge.status = data_edge_status;
        control_edge.rows = 0;
        edges.push_back(std::move(control_edge));
    }

    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> w(out);
    w.StartObject();
    w.Key("task_id");
    w.String(rec.task_id.c_str());
    w.Key("runtime_task_id");
    w.String(runtime_task_id.c_str());
    w.Key("task_kind");
    w.String(rec.task_kind.c_str());
    w.Key("runtime_kind");
    w.String(runtime_kind.c_str());
    w.Key("status");
    w.String(overall_status.c_str());
    w.Key("snapshot_time_ms");
    w.Int64(now_ms);

    w.Key("nodes");
    w.StartArray();
    for (const auto& channel : channel_names) {
        int channel_index = 0;
        auto idx_it = channel_sql_index.find(channel);
        if (idx_it != channel_sql_index.end()) {
            channel_index = idx_it->second;
        }
        w.StartObject();
        w.Key("id");
        w.String(("channel:" + channel).c_str());
        w.Key("kind");
        w.String("channel");
        w.Key("name");
        w.String(channel.c_str());
        w.Key("sql_index");
        w.Int(channel_index);
        w.Key("status");
        w.String(overall_status.c_str());
        w.Key("phase");
        w.String("");
        w.Key("processed_rows");
        w.Uint64(0);
        w.Key("output_rows");
        w.Uint64(0);
        w.Key("error_code");
        w.String("");
        w.Key("error_message");
        w.String("");
        w.Key("start_at_ms");
        w.Int64(0);
        w.Key("end_at_ms");
        w.Int64(0);
        w.EndObject();
    }
    for (size_t i = 0; i < sql_views.size(); ++i) {
        w.StartObject();
        w.Key("id");
        w.String(("operator:sql" + std::to_string(i)).c_str());
        w.Key("kind");
        w.String("operator");
        w.Key("name");
        w.String(sql_views[i].op_name.c_str());
        w.Key("sql_index");
        w.Uint64(static_cast<uint64_t>(i));
        w.Key("status");
        w.String(overall_status.c_str());
        w.Key("phase");
        w.String(overall_status == "running" ? "on_data" : "");
        w.Key("processed_rows");
        w.Uint64(i == sql_views.size() - 1 ? static_cast<uint64_t>(std::max<int64_t>(rec.result_row_count, 0)) : 0);
        w.Key("output_rows");
        w.Uint64(i == sql_views.size() - 1 ? static_cast<uint64_t>(std::max<int64_t>(rec.result_row_count, 0)) : 0);
        w.Key("error_code");
        w.String((i == sql_views.size() - 1) ? rec.error_code.c_str() : "");
        w.Key("error_message");
        w.String((i == sql_views.size() - 1) ? rec.error_message.c_str() : "");
        w.Key("start_at_ms");
        w.Int64(0);
        w.Key("end_at_ms");
        w.Int64(0);
        w.EndObject();
    }
    w.EndArray();

    w.Key("edges");
    w.StartArray();
    for (const auto& edge : edges) {
        w.StartObject();
        w.Key("id");
        w.String(edge.id.c_str());
        w.Key("from");
        w.String(edge.from.c_str());
        w.Key("to");
        w.String(edge.to.c_str());
        w.Key("edge_kind");
        w.String(edge.edge_kind.c_str());
        w.Key("trigger");
        w.String(edge.trigger.c_str());
        w.Key("status");
        w.String(edge.status.c_str());
        w.Key("rows");
        w.Uint64(edge.rows);
        w.Key("fire_count");
        w.Uint64(edge.fire_count);
        w.Key("last_fire_at_ms");
        w.Int64(edge.last_fire_at_ms);
        w.EndObject();
    }
    w.EndArray();

    w.Key("events");
    w.StartArray();
    if (include_events) {
        // 本地 batch 图仅返回快照，不维护增量事件流。
    }
    w.EndArray();
    w.Key("next_cursor");
    w.Uint64(cursor);
    w.EndObject();
    return out.GetString();
}

bool LoadTaskSqlPayloadFromStore(TaskStoreSqlite* store,
                                 const std::string& task_id,
                                 std::string* raw_sql_text,
                                 std::vector<std::string>* sqls) {
    if (!store || task_id.empty() || !raw_sql_text || !sqls) return false;
    std::string sqls_json;
    int sql_count = 0;
    if (store->QueryTaskSqlPayload(task_id, raw_sql_text, &sqls_json, &sql_count) != 0) return false;
    if (raw_sql_text->empty()) return false;
    if (!ParseSqlsJson(sqls_json, sqls)) return false;
    if (sql_count > 0 && static_cast<int>(sqls->size()) != sql_count) return false;
    return true;
}

bool LoadTaskSqlsFromStore(TaskStoreSqlite* store, const std::string& task_id, std::vector<std::string>* sqls) {
    if (!sqls) return false;
    std::string raw_sql_text;
    return LoadTaskSqlPayloadFromStore(store, task_id, &raw_sql_text, sqls);
}

bool LoadTaskRawSqlTextFromStore(TaskStoreSqlite* store, const std::string& task_id, std::string* raw_sql_text) {
    if (!raw_sql_text) return false;
    std::vector<std::string> sqls;
    return LoadTaskSqlPayloadFromStore(store, task_id, raw_sql_text, &sqls);
}

}  // namespace

TaskPlugin::TaskPlugin() = default;
TaskPlugin::~TaskPlugin() = default;

int TaskPlugin::Option(const char* arg) {
    if (!arg || !*arg) return 0;
    std::string opts(arg);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();
        std::string key = opts.substr(pos, eq - pos);
        std::string val = opts.substr(eq + 1, end - eq - 1);
        if (key == "db_dir" && !val.empty()) db_dir_ = val;
        if (key == "db_path" && !val.empty()) db_path_ = val;
        if (key == "retention_days" && !val.empty()) {
            char* endp = nullptr;
            long n = std::strtol(val.c_str(), &endp, 10);
            if (endp != val.c_str() && endp && *endp == '\0') {
                if (n < 0) n = 0;
                if (n > 3650) n = 3650;
                retention_days_ = static_cast<int>(n);
            }
        }
        if (key == "retention_max_count" && !val.empty()) {
            char* endp = nullptr;
            long n = std::strtol(val.c_str(), &endp, 10);
            if (endp != val.c_str() && endp && *endp == '\0') {
                if (n < 0) n = 0;
                if (n > 1000000) n = 1000000;
                retention_max_count_ = static_cast<int>(n);
            }
        }
        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int TaskPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    scheduler_client_.ResetQuerier(querier);
    return 0;
}

int TaskPlugin::Unload() {
    Stop();
    return 0;
}

int TaskPlugin::EnsureDb() {
    if (!store_) {
        store_ = std::make_unique<TaskStoreSqlite>();
    }
    TaskStoreSqlite::OpenOptions options;
    options.db_dir = db_dir_;
    options.db_path = db_path_;
    options.retention_days = retention_days_;
    options.retention_max_count = retention_max_count_;
    return store_->Open(options);
}

int TaskPlugin::CleanupOrphans() {
    return store_ ? store_->CleanupOrphans() : -1;
}

int TaskPlugin::Start() {
    if (EnsureDb() != 0) return -1;
    if (CleanupOrphans() != 0) return -1;

    {
        std::string last;
        if (store_ && store_->QueryLastTaskId(&last) == 0 && !last.empty()) {
            const size_t pos = last.rfind('_');
            if (pos != std::string::npos) {
                seq_.store(static_cast<uint64_t>(std::strtoull(last.c_str() + pos + 1, nullptr, 10)));
            }
        }
    }

    running_.store(true);
    timeout_thread_ = std::thread([this]() { TimeoutLoop(); });
    return 0;
}

int TaskPlugin::Stop() {
    running_.store(false);
    if (timeout_thread_.joinable()) timeout_thread_.join();
    if (store_) (void)store_->Close();
    return 0;
}

const char* TaskPlugin::StatusName(TaskStatus s) {
    switch (s) {
        case TaskStatus::kPending: return "pending";
        case TaskStatus::kRunning: return "running";
        case TaskStatus::kCompleted: return "completed";
        case TaskStatus::kFailed: return "failed";
        case TaskStatus::kStopped: return "stopped";
        case TaskStatus::kCancelled: return "cancelled";
        case TaskStatus::kTimeout: return "timeout";
        default: return "failed";
    }
}

TaskStatus TaskPlugin::ParseStatus(const std::string& s) {
    if (s == "pending") return TaskStatus::kPending;
    if (s == "running") return TaskStatus::kRunning;
    if (s == "completed") return TaskStatus::kCompleted;
    if (s == "failed") return TaskStatus::kFailed;
    if (s == "stopped") return TaskStatus::kStopped;
    if (s == "cancelled") return TaskStatus::kCancelled;
    if (s == "timeout") return TaskStatus::kTimeout;
    return TaskStatus::kFailed;
}

const char* TaskPlugin::RuntimeStatusName(TaskStatus s) {
    switch (s) {
        case TaskStatus::kPending: return "pending";
        case TaskStatus::kRunning: return "running";
        case TaskStatus::kStopped: return "stopped";
        case TaskStatus::kCancelled: return "cancelled";
        case TaskStatus::kFailed: return "failed";
        case TaskStatus::kCompleted: return "completed";
        case TaskStatus::kTimeout: return "timeout";
        default: return "unknown";
    }
}

TaskStatus TaskPlugin::MapStreamRuntimeStatus(const std::string& runtime_status) {
    if (runtime_status == "submitted" ||
        runtime_status == "pending" ||
        runtime_status == "created") {
        return TaskStatus::kPending;
    }
    if (runtime_status == "running" ||
        runtime_status == "stopping") {
        return TaskStatus::kRunning;
    }
    if (runtime_status == "stopped") return TaskStatus::kStopped;
    if (runtime_status == "cancelled") return TaskStatus::kCancelled;
    if (runtime_status == "failed") return TaskStatus::kFailed;
    return TaskStatus::kPending;
}

TaskStatus TaskPlugin::MapBatchRuntimeStatus(const std::string& runtime_status) {
    if (runtime_status == "submitted" ||
        runtime_status == "pending" ||
        runtime_status == "created") {
        return TaskStatus::kPending;
    }
    if (runtime_status == "running" ||
        runtime_status == "stopping") {
        return TaskStatus::kRunning;
    }
    if (runtime_status == "stopped") return TaskStatus::kStopped;
    if (runtime_status == "cancelled") return TaskStatus::kCancelled;
    if (runtime_status == "completed") return TaskStatus::kCompleted;
    if (runtime_status == "timeout") return TaskStatus::kTimeout;
    if (runtime_status == "failed") return TaskStatus::kFailed;
    return TaskStatus::kPending;
}

bool TaskPlugin::IsTerminal(TaskStatus s) {
    return s == TaskStatus::kCompleted ||
           s == TaskStatus::kFailed ||
           s == TaskStatus::kStopped ||
           s == TaskStatus::kCancelled ||
           s == TaskStatus::kTimeout;
}

int TaskPlugin::WriteTaskEvent(const std::string& task_id, const std::string& from_status,
                               const std::string& to_status, const std::string& message) {
    return store_ ? store_->WriteTaskEvent(task_id, from_status, to_status, message) : -1;
}

int TaskPlugin::WriteDiagnostic(const std::string& task_id,
                                int sql_index,
                                const std::string& sql_text,
                                int64_t duration_ms,
                                int64_t source_rows,
                                int64_t sink_rows,
                                const std::string& operator_chain) {
    return store_ ? store_->WriteDiagnostic(task_id,
                                            sql_index,
                                            sql_text,
                                            duration_ms,
                                            source_rows,
                                            sink_rows,
                                            operator_chain)
                  : -1;
}

std::string TaskPlugin::MakeNowTaskId(uint64_t seq) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &t);
#else
    localtime_r(&t, &tm_now);
#endif
    char ts[32] = {0};
    std::strftime(ts, sizeof(ts), "%Y%m%d%H%M%S", &tm_now);
    return std::string("tsk_") + ts + "_" + std::to_string(seq);
}

int32_t TaskPlugin::ClassifySqlTaskKindViaScheduler(const std::string& sql, std::string* task_kind_out,
                                                    std::string* err_rsp, bool* requires_async) {
    if (task_kind_out) task_kind_out->clear();
    if (err_rsp) err_rsp->clear();
    if (requires_async) *requires_async = false;
    if (sql.empty()) {
        if (err_rsp) *err_rsp = BuildErrorJson("invalid request, sql must not be empty");
        return error::BAD_REQUEST;
    }

    rapidjson::StringBuffer classify_req_buf;
    rapidjson::Writer<rapidjson::StringBuffer> classify_req_w(classify_req_buf);
    classify_req_w.StartObject();
    classify_req_w.Key("sql");
    classify_req_w.String(sql.c_str());
    classify_req_w.EndObject();

    std::string classify_rsp;
    const int32_t classify_rc = scheduler_client_.ClassifySql(classify_req_buf.GetString(), &classify_rsp);
    if (classify_rc != error::OK) {
        if (err_rsp) {
            *err_rsp = classify_rsp.empty() ? BuildErrorJson("scheduler sql classify failed") : classify_rsp;
        }
        return classify_rc;
    }

    rapidjson::Document classify_doc;
    classify_doc.Parse(classify_rsp.c_str());
    if (classify_doc.HasParseError() || !classify_doc.IsObject() ||
        !classify_doc.HasMember("task_kind") || !classify_doc["task_kind"].IsString()) {
        if (err_rsp) *err_rsp = BuildErrorJson("invalid scheduler classify response");
        return error::INTERNAL_ERROR;
    }
    const std::string task_kind = classify_doc["task_kind"].GetString();
    if (task_kind != "batch" && task_kind != "stream") {
        if (err_rsp) *err_rsp = BuildErrorJson("invalid scheduler classify task_kind");
        return error::INTERNAL_ERROR;
    }
    if (task_kind_out) *task_kind_out = task_kind;
    if (classify_doc.HasMember("requires_async")) {
        if (!classify_doc["requires_async"].IsBool()) {
            if (err_rsp) *err_rsp = BuildErrorJson("invalid scheduler classify requires_async");
            return error::INTERNAL_ERROR;
        }
        if (requires_async) *requires_async = classify_doc["requires_async"].GetBool();
    }
    return error::OK;
}

int TaskPlugin::UpdateRuntimeTaskId(const std::string& task_id, const std::string& runtime_task_id) {
    return store_ ? store_->UpdateRuntimeTaskId(task_id, runtime_task_id) : -1;
}

int TaskPlugin::UpdateTaskKindAndRuntimeId(const std::string& task_id,
                                           const std::string& task_kind,
                                           const std::string& runtime_task_id) {
    return store_ ? store_->UpdateTaskKindAndRuntimeId(task_id, task_kind, runtime_task_id) : -1;
}

int TaskPlugin::CreateTask(const std::string& request_sql, std::string* task_id) {
    return CreateTaskInternal(request_sql, request_sql, BuildSqlsJson({request_sql}), 1, 0, task_id);
}

int TaskPlugin::CreateTaskInternal(const std::string& request_sql,
                                   const std::string& raw_sql_text,
                                   const std::string& sqls_json,
                                   int sql_count,
                                   int timeout_s,
                                   std::string* task_id,
                                   const std::string& task_kind,
                                   const std::string& runtime_task_id) {
    if (!task_id || request_sql.empty() || raw_sql_text.empty() || sqls_json.empty() || sql_count <= 0) return -1;
    if (EnsureDb() != 0) return -1;
    const std::string id = MakeNowTaskId(++seq_);
    TaskStoreSqlite::TaskCreateParams params;
    params.task_id = id;
    params.request_sql = TruncateSummary(request_sql);
    params.raw_sql_text = raw_sql_text;
    params.sqls_json = sqls_json;
    params.sql_count = sql_count;
    params.timeout_s = timeout_s;
    params.task_kind = task_kind;
    params.runtime_task_id = runtime_task_id;
    if (!store_ || store_->CreateTask(params) != 0) return -1;
    *task_id = id;
    return 0;
}

void TaskPlugin::CleanupIntermediateChannels(const std::set<std::string>& channels) {
    if (channels.empty() || !querier_) return;
    auto* registry = static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY));
    if (!registry) return;
    for (const auto& full_name : channels) {
        if (!IsDataFrameRef(full_name)) continue;
        const std::string name = DataFrameNamePart(full_name);
        if (!name.empty()) (void)registry->Unregister(name.c_str());
    }
}

int TaskPlugin::UpdateStatus(const std::string& task_id,
                             TaskStatus new_status,
                             const std::string& error_code,
                             const std::string& error_message,
                             const std::string& error_stage,
                             int64_t result_row_count,
                             int64_t result_col_count,
                             const std::string& result_target) {
    if (!store_) return -1;
    TaskStoreSqlite::TaskStatusUpdate update;
    update.task_id = task_id;
    update.new_status = new_status;
    update.error_code = error_code;
    update.error_message = error_message;
    update.error_stage = error_stage;
    update.result_row_count = result_row_count;
    update.result_col_count = result_col_count;
    update.result_target = result_target;
    return store_->UpdateStatus(update);
}

int TaskPlugin::GetTask(const std::string& task_id, TaskRecord* out) {
    return store_ ? store_->GetTask(task_id, out) : -1;
}

int TaskPlugin::ListTasks(int page,
                          int page_size,
                          const std::string& status_filter,
                          std::vector<TaskRecord>* items,
                          int64_t* total) {
    return store_ ? store_->ListTasks(page, page_size, status_filter, items, total) : -1;
}

int TaskPlugin::ListTasksByKind(const std::string& task_kind,
                                int page,
                                int page_size,
                                const std::string& status_filter,
                                std::vector<TaskRecord>* items,
                                int64_t* total) {
    return store_ ? store_->ListTasksByKind(task_kind, page, page_size, status_filter, items, total) : -1;
}

int TaskPlugin::DeleteTask(const std::string& task_id) {
    return store_ ? store_->DeleteTask(task_id) : -1;
}

int TaskPlugin::RunRetentionCleanup() {
    return store_ ? store_->RunRetentionCleanup() : -1;
}

void TaskPlugin::TimeoutLoop() {
    auto last_retention_run = std::chrono::steady_clock::now();
    constexpr auto kRetentionInterval = std::chrono::seconds(1);
    while (running_.load()) {
        std::vector<std::string> timed_out_ids;
        if (store_) (void)store_->ListTimedOutTaskIds(&timed_out_ids);

        for (const auto& id : timed_out_ids) {
            TaskRecord rec;
            if (GetTask(id, &rec) != 0) continue;
            if (rec.task_kind == "batch" && !rec.runtime_task_id.empty()) {
                // batch async 任务超时由 Scheduler runtime 统一负责。
                continue;
            }
            (void)UpdateStatus(id, TaskStatus::kTimeout, "TIMEOUT", "task execution timeout", "timeout", 0, 0, "");
        }

        if (retention_days_ > 0 || retention_max_count_ > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_retention_run >= kRetentionInterval) {
                (void)RunRetentionCleanup();
                last_retention_run = now;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

int TaskPlugin::ExecuteOneTask(const std::string& task_id, std::string* execute_rsp) {
    // 逻辑链：
    // 1) 读取任务与 SQL 列表，提前收集中间 dataframe sink 以便失败回收；
    // 2) 按语句顺序执行 batch SQL，并在每步写入诊断与进度索引；
    // 3) 统一处理 cancel/timeout/执行失败，并映射错误码与阶段；
    // 4) 结束时写最终状态并清理中间通道。
    TaskRecord rec;
    if (GetTask(task_id, &rec) != 0) return -1;
    if (rec.task_kind != "batch") return 0;
    if (IsTerminal(rec.status)) return 0;
    if (UpdateStatus(task_id, TaskStatus::kRunning, "", "", "", 0, 0, "") != 0) return 0;

    std::vector<std::string> sqls;
    const bool sql_payload_ok = LoadTaskSqlsFromStore(store_.get(), task_id, &sqls);
    if (sqls.empty()) {
        const std::string err = sql_payload_ok ? "empty sql list" : "invalid sql payload";
        return UpdateStatus(task_id, TaskStatus::kFailed, "INVALID_SQLS", err, "parse", 0, 0, "");
    }

    std::set<std::string> intermediate_channels;
    SqlParser parser;
    for (size_t i = 0; i + 1 < sqls.size(); ++i) {
        auto stmt = parser.Parse(sqls[i]);
        if (!stmt.error.empty()) continue;
        if (stmt.dest.empty()) continue;
        if (IsDataFrameRef(stmt.dest)) intermediate_channels.insert(stmt.dest);
    }

    auto update_current_index = [this, &task_id](int index) {
        if (store_) (void)store_->UpdateCurrentSqlIndex(task_id, index);
    };

    int64_t final_rows = 0;
    int64_t final_cols = 0;
    std::string final_target;
    std::string last_rsp;
    int64_t prev_sink_rows = 0;

    for (size_t i = 0; i < sqls.size(); ++i) {
        // running 任务的取消/超时在下一条 SQL 执行前生效（不抢占正在执行的 SQL）
        bool terminal_now = false;
        bool cancel_requested = false;
        if (store_) (void)store_->QueryTaskRuntimeFlags(task_id, &terminal_now, &cancel_requested);
        if (terminal_now) {
            CleanupIntermediateChannels(intermediate_channels);
            return 0;
        }
        if (cancel_requested) {
            const int urc = UpdateStatus(task_id, TaskStatus::kCancelled, "CANCELLED",
                                         "cancelled by user", "cancel", 0, 0, "");
            CleanupIntermediateChannels(intermediate_channels);
            return urc == 0 ? 0 : urc;
        }

        update_current_index(static_cast<int>(i));
        const auto sql_start = std::chrono::steady_clock::now();
        const int64_t source_rows = (i == 0) ? 0 : prev_sink_rows;
        const std::string operator_chain = BuildOperatorChainFromSql(sqls[i]);

        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
        req_w.StartObject();
        req_w.Key("sql");
        req_w.String(sqls[i].c_str());
        req_w.EndObject();

        std::string rsp;
        int32_t rc = scheduler_client_.ExecuteBatch(req_buf.GetString(), &rsp);
        if (rc != error::OK) {
            const auto sql_end = std::chrono::steady_clock::now();
            const int64_t duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sql_end - sql_start).count();
            rapidjson::Document d;
            d.Parse(rsp.c_str());
            std::string err = "execution failed";
            std::string err_code = "EXECUTION_FAILED";
            std::string err_stage = "execute";
            if (!d.HasParseError() && d.IsObject() && d.HasMember("error") && d["error"].IsString()) err = d["error"].GetString();
            if (!d.HasParseError() && d.IsObject() && d.HasMember("error_code") && d["error_code"].IsString()) {
                std::string v = d["error_code"].GetString();
                if (!v.empty()) err_code = v;
            }
            if (!d.HasParseError() && d.IsObject() && d.HasMember("error_stage") && d["error_stage"].IsString()) {
                std::string v = d["error_stage"].GetString();
                if (!v.empty()) err_stage = v;
            }
            if (rc == error::UNAVAILABLE) {
                err_code = "SCHEDULER_UNAVAILABLE";
                err_stage = "dispatch";
                if (err.empty() || err == "execution failed") {
                    err = "execute route not found";
                }
            }
            if (err_stage == "execute") {
                std::string inferred = ExtractStageFromErrorMessage(err);
                if (!inferred.empty()) err_stage = inferred;
            }
            (void)WriteDiagnostic(task_id, static_cast<int>(i), sqls[i], duration_ms, source_rows, 0, operator_chain);
            if (execute_rsp) *execute_rsp = rsp;
            const int urc = UpdateStatus(task_id, TaskStatus::kFailed, err_code, err, err_stage, 0, 0, "");
            CleanupIntermediateChannels(intermediate_channels);
            return urc;
        }

        const auto sql_end = std::chrono::steady_clock::now();
        const int64_t duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sql_end - sql_start).count();
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        int64_t rows = 0;
        int64_t cols = 0;
        std::string result_target;
        int64_t derived_rows = 0;
        int64_t derived_cols = 0;
        if (!d.HasParseError() && d.IsObject() && d.HasMember("result_row_count") && d["result_row_count"].IsInt64()) {
            rows = d["result_row_count"].GetInt64();
        } else if (!d.HasParseError() && d.IsObject() && d.HasMember("rows") && d["rows"].IsInt64()) {
            rows = d["rows"].GetInt64();
        }
        if (!d.HasParseError() && d.IsObject() && d.HasMember("result_col_count") && d["result_col_count"].IsInt64()) {
            cols = d["result_col_count"].GetInt64();
        } else if (!d.HasParseError() && d.IsObject() && d.HasMember("cols") && d["cols"].IsInt64()) {
            cols = d["cols"].GetInt64();
        }
        if (!d.HasParseError() && d.IsObject() && d.HasMember("result_target") && d["result_target"].IsString()) {
            result_target = d["result_target"].GetString();
        }
        if (!d.HasParseError() && d.IsObject() && d.HasMember("data")) {
            const auto& data = d["data"];
            if (data.IsObject()) {
                if (data.HasMember("columns") && data["columns"].IsArray()) {
                    derived_cols = static_cast<int64_t>(data["columns"].Size());
                }
                if (data.HasMember("data") && data["data"].IsArray()) {
                    derived_rows = static_cast<int64_t>(data["data"].Size());
                }
            } else if (data.IsArray()) {
                derived_rows = static_cast<int64_t>(data.Size());
                if (data.Size() > 0 && data[0].IsObject()) {
                    derived_cols = static_cast<int64_t>(data[0].MemberCount());
                }
            }
        }
        if (rows <= 0 && derived_rows > 0) rows = derived_rows;
        cols = std::max<int64_t>(cols, derived_cols);
        (void)WriteDiagnostic(task_id, static_cast<int>(i), sqls[i], duration_ms, source_rows, rows, operator_chain);

        final_rows = rows;
        final_cols = cols;
        final_target = result_target;
        prev_sink_rows = rows;
        last_rsp = std::move(rsp);
    }

    if (execute_rsp) *execute_rsp = last_rsp;
    const int urc = UpdateStatus(task_id, TaskStatus::kCompleted, "", "", "", final_rows, final_cols, final_target);
    CleanupIntermediateChannels(intermediate_channels);
    return urc;
}

int TaskPlugin::SyncBatchRuntimeTask(TaskRecord* rec) {
    if (!rec) return EINVAL;
    if (rec->task_kind != "batch") return 0;
    if (rec->runtime_task_id.empty()) return 0;

    rapidjson::StringBuffer req_buf;
    rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
    req_w.StartObject();
    req_w.Key("runtime_task_id");
    req_w.String(rec->runtime_task_id.c_str());
    req_w.EndObject();

    std::string scheduler_rsp;
    const int32_t rc = scheduler_client_.QueryBatchStatus(req_buf.GetString(), &scheduler_rsp);
    if (rc != error::OK) return rc;

    rapidjson::Document runtime_doc;
    runtime_doc.Parse(scheduler_rsp.c_str());
    if (runtime_doc.HasParseError() || !runtime_doc.IsObject() ||
        !runtime_doc.HasMember("status") || !runtime_doc["status"].IsString()) {
        return error::INTERNAL_ERROR;
    }

    const std::string runtime_status = runtime_doc["status"].GetString();
    const TaskStatus mapped = MapBatchRuntimeStatus(runtime_status);
    std::string runtime_error_code;
    std::string runtime_error_message;
    std::string runtime_error_stage;
    int64_t result_rows = rec->result_row_count;
    int64_t result_cols = rec->result_col_count;
    std::string result_target = rec->result_target;

    if (runtime_doc.HasMember("error_code") && runtime_doc["error_code"].IsString()) {
        runtime_error_code = runtime_doc["error_code"].GetString();
    }
    if (runtime_doc.HasMember("error_message") && runtime_doc["error_message"].IsString()) {
        runtime_error_message = runtime_doc["error_message"].GetString();
    }
    if (runtime_doc.HasMember("error_stage") && runtime_doc["error_stage"].IsString()) {
        runtime_error_stage = runtime_doc["error_stage"].GetString();
    }
    if (runtime_doc.HasMember("result_row_count") && runtime_doc["result_row_count"].IsInt64()) {
        result_rows = runtime_doc["result_row_count"].GetInt64();
    }
    if (runtime_doc.HasMember("result_col_count") && runtime_doc["result_col_count"].IsInt64()) {
        result_cols = runtime_doc["result_col_count"].GetInt64();
    }
    if (runtime_doc.HasMember("result_target") && runtime_doc["result_target"].IsString()) {
        result_target = runtime_doc["result_target"].GetString();
    }

    bool should_update = false;
    if (mapped != rec->status) {
        should_update = true;
    }
    if ((mapped == TaskStatus::kCompleted || mapped == TaskStatus::kFailed || mapped == TaskStatus::kTimeout) &&
        (result_rows != rec->result_row_count ||
         result_cols != rec->result_col_count ||
         result_target != rec->result_target)) {
        should_update = true;
    }
    if ((mapped == TaskStatus::kFailed || mapped == TaskStatus::kTimeout) &&
        (runtime_error_code != rec->error_code ||
         runtime_error_message != rec->error_message ||
         runtime_error_stage != rec->error_stage)) {
        should_update = true;
    }
    if (mapped == TaskStatus::kCancelled && rec->error_code != "CANCELLED") {
        should_update = true;
    }

    if (should_update) {
        std::string err_code;
        std::string err_msg;
        std::string err_stage;
        if (mapped == TaskStatus::kFailed || mapped == TaskStatus::kTimeout) {
            err_code = runtime_error_code;
            err_msg = runtime_error_message;
            err_stage = runtime_error_stage;
        } else if (mapped == TaskStatus::kCancelled) {
            err_code = "CANCELLED";
            err_msg = "cancelled by user";
            err_stage = "cancel";
        }
        (void)UpdateStatus(rec->task_id,
                           mapped,
                           err_code,
                           err_msg,
                           err_stage,
                           result_rows,
                           result_cols,
                           result_target);
    }
    (void)GetTask(rec->task_id, rec);
    return 0;
}

int32_t TaskPlugin::HandleBatchExecute(const std::string&, const std::string& req, std::string& rsp) {
    // 逻辑链：
    // 1) 仅接受 sql_text 入口并做多 SQL 拆分；
    // 2) 对每条 SQL 走 scheduler classify，禁止 stream SQL 混入 batch API；
    // 3) 创建任务元数据，async 返回 pending；sync 直接串行执行并回填结果。
    rapidjson::Document d;
    d.Parse(req.c_str());
    if (d.HasParseError() || !d.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }

    if (d.HasMember("sql") || d.HasMember("sqls")) {
        rsp = BuildErrorWithCodeJson(
            "batch execution accepts only sql_text/mode/timeout_s",
            ErrorCodeId::kBatchSqlTextInvalid);
        return error::BAD_REQUEST;
    }

    if (!d.HasMember("sql_text") || !d["sql_text"].IsString()) {
        rsp = BuildErrorWithCodeJson(
            "invalid request, expected {\"sql_text\":\"...\"}",
            ErrorCodeId::kBatchSqlTextInvalid);
        return error::BAD_REQUEST;
    }
    const std::string sql_text = d["sql_text"].GetString();

    std::vector<std::string> sqls;
    SqlTextSplitError split_err;
    if (SplitSqlText(sql_text, &sqls, &split_err) != 0) {
        std::string err = "invalid request, sql_text split failed";
        if (!split_err.message.empty()) {
            err += ": " + split_err.message;
        }
        rsp = BuildErrorWithCodeAndSqlIndexJson(
            err,
            ErrorCodeId::kBatchSqlTextInvalid,
            split_err.statement_index);
        return error::BAD_REQUEST;
    }

    size_t async_sql_index = sqls.size();
    for (size_t i = 0; i < sqls.size(); ++i) {
        const auto& sql = sqls[i];
        std::string task_kind;
        std::string classify_err_rsp;
        bool requires_async = false;
        const int32_t classify_rc =
            ClassifySqlTaskKindViaScheduler(sql, &task_kind, &classify_err_rsp, &requires_async);
        if (classify_rc != error::OK) {
            rsp = classify_err_rsp.empty() ? BuildErrorJson("sql classify failed") : classify_err_rsp;
            return classify_rc;
        }
        if (task_kind == "stream") {
            rsp = BuildErrorWithCodeAndSqlIndexJson(
                "stream SQL must use /tasks/stream/execute",
                ErrorCodeId::kStreamSqlUseStreamApi,
                i);
            return error::BAD_REQUEST;
        }
        if (requires_async && async_sql_index == sqls.size()) async_sql_index = i;
    }

    std::string mode = "async";
    if (d.HasMember("mode")) {
        if (!d["mode"].IsString()) {
            rsp = BuildErrorJson("invalid request, mode must be string: sync|async");
            return error::BAD_REQUEST;
        }
        mode = d["mode"].GetString();
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (mode != "sync" && mode != "async") {
            rsp = BuildErrorJson("invalid mode, expected sync|async");
            return error::BAD_REQUEST;
        }
    }
    const bool sync = (mode == "sync");
    if (sync && async_sql_index < sqls.size()) {
        rsp = BuildErrorWithCodeAndSqlIndexJson("连续采集任务不支持同步执行，请使用 mode='async'。",
                                                "ASYNC_EXECUTION_REQUIRED", async_sql_index);
        return error::BAD_REQUEST;
    }
    int timeout_s = 0;
    if (d.HasMember("timeout_s")) {
        if (!d["timeout_s"].IsInt()) {
            rsp = BuildErrorJson("invalid request, timeout_s must be integer seconds");
            return error::BAD_REQUEST;
        }
        timeout_s = d["timeout_s"].GetInt();
        if (timeout_s < 0) {
            rsp = BuildErrorJson("invalid request, timeout_s must be >= 0");
            return error::BAD_REQUEST;
        }
    }

    const std::string request_summary = TruncateSummary(sqls.front());
    const std::string sqls_json = BuildSqlsJson(sqls);

    std::string task_id;
    if (CreateTaskInternal(request_summary,
                           sql_text,
                           sqls_json,
                           static_cast<int>(sqls.size()),
                           timeout_s,
                           &task_id) != 0) {
        rsp = BuildErrorJson("failed to create task");
        return error::INTERNAL_ERROR;
    }

    if (!sync) {
        rapidjson::StringBuffer submit_req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> submit_req_w(submit_req_buf);
        submit_req_w.StartObject();
        submit_req_w.Key("runtime_task_id");
        submit_req_w.String(task_id.c_str());
        submit_req_w.Key("timeout_s");
        submit_req_w.Int(timeout_s);
        submit_req_w.Key("sqls");
        submit_req_w.StartArray();
        for (const auto& sql : sqls) {
            submit_req_w.String(sql.c_str());
        }
        submit_req_w.EndArray();
        submit_req_w.EndObject();

        std::string submit_rsp;
        const int32_t submit_rc = scheduler_client_.SubmitBatch(submit_req_buf.GetString(), &submit_rsp);
        if (submit_rc != error::OK) {
            (void)UpdateStatus(task_id,
                               TaskStatus::kFailed,
                               "SCHEDULER_UNAVAILABLE",
                               "scheduler batch submit failed",
                               "dispatch",
                               0,
                               0,
                               "");
            rsp = submit_rsp.empty() ? BuildErrorJson("scheduler batch submit failed") : submit_rsp;
            return submit_rc;
        }

        rapidjson::Document submit_doc;
        submit_doc.Parse(submit_rsp.c_str());
        std::string runtime_task_id = task_id;
        if (!submit_doc.HasParseError() && submit_doc.IsObject() &&
            submit_doc.HasMember("runtime_task_id") && submit_doc["runtime_task_id"].IsString()) {
            runtime_task_id = submit_doc["runtime_task_id"].GetString();
        }
        if (runtime_task_id.empty()) runtime_task_id = task_id;
        (void)UpdateRuntimeTaskId(task_id, runtime_task_id);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("task_id");
        w.String(task_id.c_str());
        w.Key("runtime_task_id");
        w.String(runtime_task_id.c_str());
        w.Key("status");
        w.String("pending");
        w.Key("runtime_kind");
        w.String("batch");
        w.EndObject();
        rsp = buf.GetString();
        return error::OK;
    }

    std::string exec_rsp;
    if (ExecuteOneTask(task_id, &exec_rsp) != 0) {
        rsp = BuildErrorJson("failed to execute task");
        return error::INTERNAL_ERROR;
    }

    TaskRecord rec;
    if (GetTask(task_id, &rec) != 0) {
        rsp = BuildErrorJson("failed to fetch task result");
        return error::INTERNAL_ERROR;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_id");
    w.String(task_id.c_str());
    w.Key("status");
    w.String(StatusName(rec.status));
    if (rec.status == TaskStatus::kCompleted) {
        w.Key("rows");
        w.Int64(rec.result_row_count);
        w.Key("data");
        rapidjson::Document exec_doc;
        exec_doc.Parse(exec_rsp.c_str());
        if (!exec_doc.HasParseError() && exec_doc.IsObject() && exec_doc.HasMember("data") && exec_doc["data"].IsArray()) {
            exec_doc["data"].Accept(w);
        } else if (!exec_doc.HasParseError() && exec_doc.IsObject() && exec_doc.HasMember("data") && exec_doc["data"].IsObject()) {
            const auto& df = exec_doc["data"];
            if (df.HasMember("columns") && df["columns"].IsArray() && df.HasMember("data") && df["data"].IsArray()) {
                const auto& cols = df["columns"];
                const auto& rows = df["data"];
                w.StartArray();
                for (rapidjson::SizeType r = 0; r < rows.Size(); ++r) {
                    if (!rows[r].IsArray()) continue;
                    const auto& row = rows[r];
                    const rapidjson::SizeType n = std::min(cols.Size(), row.Size());
                    w.StartObject();
                    for (rapidjson::SizeType i = 0; i < n; ++i) {
                        if (!cols[i].IsString()) continue;
                        w.Key(cols[i].GetString());
                        row[i].Accept(w);
                    }
                    w.EndObject();
                }
                w.EndArray();
            } else {
                w.StartArray();
                w.EndArray();
            }
        } else {
            w.StartArray();
            w.EndArray();
        }
    } else if (rec.status == TaskStatus::kFailed) {
        w.Key("error");
        w.String(rec.error_message.empty() ? "execution failed" : rec.error_message.c_str());
        w.Key("error_code");
        w.String(rec.error_code.c_str());
        w.Key("error_stage");
        w.String(rec.error_stage.c_str());
    }
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleSqlClassify(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("sql") || !d["sql"].IsString() ||
        std::string(d["sql"].GetString()).empty()) {
        rsp = BuildErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }

    std::string task_kind;
    std::string classify_err_rsp;
    bool requires_async = false;
    const int32_t rc =
        ClassifySqlTaskKindViaScheduler(d["sql"].GetString(), &task_kind, &classify_err_rsp, &requires_async);
    if (rc != error::OK) {
        rsp = classify_err_rsp.empty() ? BuildErrorJson("scheduler sql classify failed") : classify_err_rsp;
        return rc;
    }

    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> w(out);
    w.StartObject();
    w.Key("task_kind");
    w.String(task_kind.c_str());
    w.Key("requires_async");
    w.Bool(requires_async);
    w.EndObject();
    rsp = out.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleSqlAnalyze(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("sql_text") || !d["sql_text"].IsString()) {
        rsp = BuildErrorWithCodeJson("invalid request, expected {\"sql_text\":\"...\"}", ErrorCodeId::kSqlTextInvalid);
        return error::BAD_REQUEST;
    }
    const std::string sql_text = d["sql_text"].GetString();

    std::vector<std::string> sqls;
    SqlTextSplitError split_err;
    if (SplitSqlText(sql_text, &sqls, &split_err) != 0) {
        std::string err = "invalid request, sql_text split failed";
        if (!split_err.message.empty()) {
            err += ": " + split_err.message;
        }
        rsp = BuildErrorWithCodeAndSqlIndexJson(err, ErrorCodeId::kSqlTextInvalid, split_err.statement_index);
        return error::BAD_REQUEST;
    }

    std::vector<std::string> statement_kinds;
    statement_kinds.reserve(sqls.size());
    bool requires_async = false;
    for (size_t i = 0; i < sqls.size(); ++i) {
        std::string task_kind;
        std::string classify_err_rsp;
        bool statement_requires_async = false;
        const int32_t classify_rc =
            ClassifySqlTaskKindViaScheduler(sqls[i], &task_kind, &classify_err_rsp, &statement_requires_async);
        if (classify_rc != error::OK) {
            std::string err_text = "sql classify failed";
            std::string err_code = ToErrorCode(ErrorCodeId::kSqlAnalyzeClassifyFailed);
            if (!classify_err_rsp.empty()) {
                rapidjson::Document err_doc;
                err_doc.Parse(classify_err_rsp.c_str());
                if (!err_doc.HasParseError() && err_doc.IsObject()) {
                    if (err_doc.HasMember("error") && err_doc["error"].IsString()) {
                        err_text = err_doc["error"].GetString();
                    }
                    if (err_doc.HasMember("error_code") && err_doc["error_code"].IsString()) {
                        const std::string code = err_doc["error_code"].GetString();
                        if (!code.empty()) err_code = code;
                    }
                } else {
                    err_text = classify_err_rsp;
                }
            }
            rsp = BuildErrorWithCodeAndSqlIndexJson(err_text, err_code, i);
            return classify_rc;
        }
        requires_async = requires_async || statement_requires_async;
        statement_kinds.push_back(std::move(task_kind));
    }

    bool has_batch = false;
    bool has_stream = false;
    for (const auto& kind : statement_kinds) {
        has_batch = has_batch || (kind == "batch");
        has_stream = has_stream || (kind == "stream");
    }
    std::string task_kind = "unknown";
    if (has_batch && has_stream) task_kind = "mixed";
    else if (has_stream) task_kind = "stream";
    else if (has_batch) task_kind = "batch";

    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> w(out);
    w.StartObject();
    w.Key("statement_count");
    w.Uint(static_cast<unsigned>(sqls.size()));
    w.Key("statements");
    w.StartArray();
    for (const auto& sql : sqls) {
        w.String(sql.c_str());
    }
    w.EndArray();
    w.Key("statement_kinds");
    w.StartArray();
    for (const auto& kind : statement_kinds) {
        w.String(kind.c_str());
    }
    w.EndArray();
    w.Key("task_kind");
    w.String(task_kind.c_str());
    w.Key("requires_async");
    w.Bool(requires_async);
    w.EndObject();
    rsp = out.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleList(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    int page = 1;
    int page_size = 20;
    std::string status_filter;
    if (!d.HasParseError() && d.IsObject()) {
        if (d.HasMember("page") && d["page"].IsInt()) page = d["page"].GetInt();
        if (d.HasMember("page_size") && d["page_size"].IsInt()) page_size = d["page_size"].GetInt();
        if (d.HasMember("status") && d["status"].IsString()) status_filter = d["status"].GetString();
    }
    std::vector<TaskRecord> items;
    int64_t total = 0;
    if (ListTasks(page, page_size, status_filter, &items, &total) != 0) {
        rsp = BuildErrorJson("failed to list tasks");
        return error::INTERNAL_ERROR;
    }
    for (auto& item : items) {
        if (item.task_kind != "batch") continue;
        if (IsTerminal(item.status)) continue;
        (void)SyncBatchRuntimeTask(&item);
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("total");
    w.Int64(total);
    w.Key("items");
    w.StartArray();
    for (const auto& it : items) {
        std::string sql_text;
        if (!LoadTaskRawSqlTextFromStore(store_.get(), it.task_id, &sql_text)) {
            rsp = BuildErrorWithCodeJson("invalid task sql payload", "TASK_SQL_PAYLOAD_INVALID");
            return error::INTERNAL_ERROR;
        }
        w.StartObject();
        w.Key("id");
        w.String(it.task_id.c_str());
        w.Key("task_id");
        w.String(it.task_id.c_str());
        w.Key("sql_text");
        w.String(sql_text.c_str());
        w.Key("task_kind");
        w.String(it.task_kind.c_str());
        w.Key("runtime_task_id");
        w.String(it.runtime_task_id.c_str());
        w.Key("status");
        w.String(StatusName(it.status));
        w.Key("error_code");
        w.String(it.error_code.c_str());
        w.Key("error_message");
        w.String(it.error_message.c_str());
        w.Key("result_row_count");
        w.Int64(it.result_row_count);
        w.Key("result_col_count");
        w.Int64(it.result_col_count);
        w.Key("created_at");
        w.String(it.created_at.c_str());
        w.Key("started_at");
        w.String(it.started_at.c_str());
        w.Key("updated_at");
        w.String(it.updated_at.c_str());
        w.Key("finished_at");
        w.String(it.finished_at.c_str());
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleDetail(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("task_id") || !d["task_id"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    TaskRecord rec;
    if (GetTask(d["task_id"].GetString(), &rec) != 0) {
        rsp = BuildErrorJson("task not found");
        return error::NOT_FOUND;
    }
    if (rec.task_kind == "batch" && !IsTerminal(rec.status)) {
        (void)SyncBatchRuntimeTask(&rec);
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_id");
    w.String(rec.task_id.c_str());
    w.Key("task_kind");
    w.String(rec.task_kind.c_str());
    w.Key("runtime_task_id");
    w.String(rec.runtime_task_id.c_str());
    w.Key("status");
    w.String(StatusName(rec.status));
    std::string sql_text;
    if (!LoadTaskRawSqlTextFromStore(store_.get(), rec.task_id, &sql_text)) {
        rsp = BuildErrorWithCodeJson("invalid task sql payload", "TASK_SQL_PAYLOAD_INVALID");
        return error::INTERNAL_ERROR;
    }
    w.Key("sql_text");
    w.String(sql_text.c_str());
    w.Key("result_row_count");
    w.Int64(rec.result_row_count);
    w.Key("result_col_count");
    w.Int64(rec.result_col_count);
    w.Key("result_target");
    w.String(rec.result_target.c_str());
    w.Key("error_code");
    w.String(rec.error_code.c_str());
    w.Key("error_message");
    w.String(rec.error_message.c_str());
    w.Key("error_stage");
    w.String(rec.error_stage.c_str());
    w.Key("created_at");
    w.String(rec.created_at.c_str());
    w.Key("started_at");
    w.String(rec.started_at.c_str());
    w.Key("updated_at");
    w.String(rec.updated_at.c_str());
    w.Key("finished_at");
    w.String(rec.finished_at.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleDiagnostics(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("task_id") || !d["task_id"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = d["task_id"].GetString();
    TaskRecord rec;
    if (GetTask(task_id, &rec) != 0) {
        rsp = BuildErrorJson("task not found");
        return error::NOT_FOUND;
    }

    std::vector<TaskStoreSqlite::TaskDiagnostic> diagnostics;
    if (!store_ || store_->ListDiagnostics(task_id, &diagnostics) != 0) {
        rsp = BuildErrorJson("failed to query diagnostics");
        return error::INTERNAL_ERROR;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_id");
    w.String(task_id.c_str());
    w.Key("items");
    w.StartArray();
    for (const auto& item : diagnostics) {
        w.StartObject();
        w.Key("sql_index");
        w.Int(item.sql_index);
        w.Key("sql_text");
        w.String(item.sql_text.c_str());
        w.Key("duration_ms");
        w.Int64(item.duration_ms);
        w.Key("source_rows");
        w.Int64(item.source_rows);
        w.Key("sink_rows");
        w.Int64(item.sink_rows);
        w.Key("operator_chain");
        w.String(item.operator_chain.c_str());
        w.Key("created_at");
        w.String(item.created_at.c_str());
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleDelete(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("task_id") || !d["task_id"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const int rc = DeleteTask(d["task_id"].GetString());
    if (rc == 1) {
        rsp = BuildErrorJson("non-terminal task cannot be deleted");
        return error::CONFLICT;
    }
    if (rc != 0) {
        rsp = BuildErrorJson("task not found");
        return error::NOT_FOUND;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t TaskPlugin::HandleCancel(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("task_id") || !d["task_id"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = d["task_id"].GetString();
    if (task_id.empty()) {
        rsp = BuildErrorJson("task_id is empty");
        return error::BAD_REQUEST;
    }

    TaskRecord rec;
    if (GetTask(task_id, &rec) != 0) {
        rsp = BuildErrorJson("task not found");
        return error::NOT_FOUND;
    }
    if (rec.task_kind == "stream") {
        rsp = BuildErrorJson("stream task should use /tasks/stream/stop");
        return error::CONFLICT;
    }
    if (rec.task_kind == "batch" && !IsTerminal(rec.status) && !rec.runtime_task_id.empty()) {
        rapidjson::StringBuffer stop_req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> stop_req_w(stop_req_buf);
        stop_req_w.StartObject();
        stop_req_w.Key("runtime_task_id");
        stop_req_w.String(rec.runtime_task_id.c_str());
        stop_req_w.EndObject();

        std::string stop_rsp;
        const int32_t stop_rc = scheduler_client_.StopBatch(stop_req_buf.GetString(), &stop_rsp);
        if (stop_rc != error::OK) {
            rsp = stop_rsp.empty() ? BuildErrorJson("scheduler batch stop failed") : stop_rsp;
            return stop_rc;
        }

        rapidjson::Document stop_doc;
        stop_doc.Parse(stop_rsp.c_str());
        std::string runtime_status = "stopping";
        std::string runtime_error_code;
        std::string runtime_error_message;
        std::string runtime_error_stage;
        int64_t result_rows = rec.result_row_count;
        int64_t result_cols = rec.result_col_count;
        std::string result_target = rec.result_target;
        if (!stop_doc.HasParseError() && stop_doc.IsObject()) {
            if (stop_doc.HasMember("status") && stop_doc["status"].IsString()) {
                runtime_status = stop_doc["status"].GetString();
            }
            if (stop_doc.HasMember("error_code") && stop_doc["error_code"].IsString()) {
                runtime_error_code = stop_doc["error_code"].GetString();
            }
            if (stop_doc.HasMember("error_message") && stop_doc["error_message"].IsString()) {
                runtime_error_message = stop_doc["error_message"].GetString();
            }
            if (stop_doc.HasMember("error_stage") && stop_doc["error_stage"].IsString()) {
                runtime_error_stage = stop_doc["error_stage"].GetString();
            }
            if (stop_doc.HasMember("result_row_count") && stop_doc["result_row_count"].IsInt64()) {
                result_rows = stop_doc["result_row_count"].GetInt64();
            }
            if (stop_doc.HasMember("result_col_count") && stop_doc["result_col_count"].IsInt64()) {
                result_cols = stop_doc["result_col_count"].GetInt64();
            }
            if (stop_doc.HasMember("result_target") && stop_doc["result_target"].IsString()) {
                result_target = stop_doc["result_target"].GetString();
            }
        }

        const TaskStatus mapped = MapBatchRuntimeStatus(runtime_status);
        if (mapped == TaskStatus::kStopped ||
            mapped == TaskStatus::kCancelled ||
            mapped == TaskStatus::kFailed ||
            mapped == TaskStatus::kTimeout ||
            mapped == TaskStatus::kCompleted) {
            std::string err_code;
            std::string err_message;
            std::string err_stage;
            if (mapped == TaskStatus::kCancelled) {
                err_code = "CANCELLED";
                err_message = "cancelled by user";
                err_stage = "cancel";
            } else if (mapped == TaskStatus::kFailed || mapped == TaskStatus::kTimeout) {
                err_code = runtime_error_code;
                err_message = runtime_error_message;
                err_stage = runtime_error_stage;
            }
            (void)UpdateStatus(task_id,
                               mapped,
                               err_code,
                               err_message,
                               err_stage,
                               result_rows,
                               result_cols,
                               result_target);
        } else {
            (void)WriteTaskEvent(task_id, StatusName(rec.status), StatusName(rec.status), "CANCEL_REQUESTED");
        }

        rapidjson::StringBuffer out;
        rapidjson::Writer<rapidjson::StringBuffer> w(out);
        w.StartObject();
        w.Key("task_id");
        w.String(task_id.c_str());
        w.Key("runtime_task_id");
        w.String(rec.runtime_task_id.c_str());
        w.Key("status");
        if (mapped == TaskStatus::kRunning || mapped == TaskStatus::kPending) {
            w.String("cancelling");
        } else {
            w.String(StatusName(mapped));
        }
        w.Key("runtime_status");
        w.String(runtime_status.c_str());
        w.EndObject();
        rsp = out.GetString();
        return error::OK;
    }

    if (IsTerminal(rec.status)) {
        rsp = BuildErrorJson("terminal task cannot be cancelled");
        return error::CONFLICT;
    }

    if (rec.status == TaskStatus::kPending) {
        const int urc = UpdateStatus(task_id, TaskStatus::kCancelled, "CANCELLED",
                                     "cancelled by user", "cancel", 0, 0, "");
        if (urc != 0) {
            rsp = BuildErrorJson("failed to cancel pending task");
            return error::INTERNAL_ERROR;
        }
        rsp = R"({"status":"cancelled"})";
        return error::OK;
    }

    if (!store_) {
        rsp = BuildErrorJson("task store unavailable");
        return error::INTERNAL_ERROR;
    }
    const int cancel_rc = store_->RequestCancelRunningTask(task_id);
    if (cancel_rc < 0) {
        rsp = BuildErrorJson("failed to request task cancel");
        return error::INTERNAL_ERROR;
    }
    if (cancel_rc > 0) {
        rsp = BuildErrorJson("task is not cancellable at current state");
        return error::CONFLICT;
    }

    (void)WriteTaskEvent(task_id, "running", "running", "CANCEL_REQUESTED");
    rsp = R"({"status":"cancelling"})";
    return error::OK;
}

int32_t TaskPlugin::HandleStreamExecute(const std::string&, const std::string& req, std::string& rsp) {
    // 逻辑链：
    // 1) 解析 execution_kind/sql_text/timeout 等参数并做约束校验；
    // 2) single 模式强校验单语句且必须是 stream SQL；group 模式校验 dag 前提；
    // 3) 组装 scheduler 请求执行 runtime task；
    // 4) 回写 task store，并把 runtime 元信息透传给前端。
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }

    int timeout_s = 0;
    if (doc.HasMember("timeout_s")) {
        if (!doc["timeout_s"].IsInt()) {
            rsp = BuildErrorJson("invalid request, timeout_s must be integer");
            return error::BAD_REQUEST;
        }
        timeout_s = doc["timeout_s"].GetInt();
        if (timeout_s < 0) {
            rsp = BuildErrorJson("invalid request, timeout_s must be >= 0");
            return error::BAD_REQUEST;
        }
    }
    int share_set_ready_timeout_s = 30;
    bool has_share_set_ready_timeout_s = false;
    if (doc.HasMember("share_set_ready_timeout_s")) {
        if (!doc["share_set_ready_timeout_s"].IsInt()) {
            rsp = BuildErrorWithCodeJson("invalid request, share_set_ready_timeout_s must be integer",
                                    ErrorCodeId::kStreamGroupSqlTextInvalid);
            return error::BAD_REQUEST;
        }
        has_share_set_ready_timeout_s = true;
        share_set_ready_timeout_s = doc["share_set_ready_timeout_s"].GetInt();
        if (share_set_ready_timeout_s <= 0) {
            rsp = BuildErrorWithCodeJson("invalid request, share_set_ready_timeout_s must be > 0",
                                    ErrorCodeId::kStreamGroupSqlTextInvalid);
            return error::BAD_REQUEST;
        }
    }

    std::string execution_kind = "single";
    if (doc.HasMember("execution_kind")) {
        if (!doc["execution_kind"].IsString()) {
            rsp = BuildErrorJson("invalid request, execution_kind must be string");
            return error::BAD_REQUEST;
        }
        execution_kind = doc["execution_kind"].GetString();
        std::transform(execution_kind.begin(), execution_kind.end(), execution_kind.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    if (!doc.HasMember("sql_text") || !doc["sql_text"].IsString()) {
        rsp = BuildErrorWithCodeJson("invalid request, expected {\"sql_text\":\"...\"}",
                                ErrorCodeId::kStreamGroupSqlTextInvalid);
        return error::BAD_REQUEST;
    }
    const std::string sql_text = doc["sql_text"].GetString();

    const bool has_legacy_sql = doc.HasMember("sql");
    const bool has_legacy_sqls = doc.HasMember("sqls");
    const bool has_legacy_dag = doc.HasMember("dag") || doc.HasMember("nodes") || doc.HasMember("source_share_sets");

    std::vector<std::string> sqls;
    SqlTextSplitError split_err;
    if (SplitSqlText(sql_text, &sqls, &split_err) != 0) {
        std::string err = "invalid request, sql_text split failed";
        if (!split_err.message.empty()) {
            err += ": " + split_err.message;
        }
        rsp = BuildErrorWithCodeAndSqlIndexJson(
            err,
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            split_err.statement_index);
        return error::BAD_REQUEST;
    }

    if (execution_kind == "single") {
        if (doc.HasMember("group_mode") || has_legacy_dag || has_legacy_sql || has_legacy_sqls) {
            rsp = BuildErrorWithCodeJson(
                "single execution accepts only sql_text/timeout_s",
                ErrorCodeId::kStreamGroupSqlTextInvalid);
            return error::BAD_REQUEST;
        }
        if (has_share_set_ready_timeout_s) {
            rsp = BuildErrorWithCodeJson(
                "single execution must not contain share_set_ready_timeout_s",
                ErrorCodeId::kStreamGroupSqlTextInvalid);
            return error::BAD_REQUEST;
        }
        if (sqls.size() != 1) {
            rsp = BuildErrorWithCodeJson(
                "single execution requires exactly one SQL statement in sql_text",
                ErrorCodeId::kStreamGroupSqlTextInvalid);
            return error::BAD_REQUEST;
        }

        rapidjson::StringBuffer classify_req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> classify_req_w(classify_req_buf);
        classify_req_w.StartObject();
        classify_req_w.Key("sql");
        classify_req_w.String(sqls.front().c_str());
        classify_req_w.EndObject();

        std::string classify_rsp;
        const int32_t classify_rc = scheduler_client_.ClassifySql(classify_req_buf.GetString(), &classify_rsp);
        if (classify_rc != error::OK) {
            rsp = classify_rsp.empty() ? BuildErrorJson("scheduler sql classify failed") : classify_rsp;
            return classify_rc;
        }
        rapidjson::Document classify_doc;
        classify_doc.Parse(classify_rsp.c_str());
        if (classify_doc.HasParseError() || !classify_doc.IsObject() ||
            !classify_doc.HasMember("task_kind") || !classify_doc["task_kind"].IsString()) {
            rsp = BuildErrorJson("invalid scheduler classify response");
            return error::INTERNAL_ERROR;
        }
        if (std::string(classify_doc["task_kind"].GetString()) != "stream") {
            rsp = BuildErrorWithCodeJson(
                "batch SQL must use /tasks/batch/execute",
                ErrorCodeId::kBatchSqlUseBatchApi);
            return error::BAD_REQUEST;
        }
    } else if (execution_kind == "group") {
        if (has_legacy_sql || has_legacy_sqls || has_legacy_dag) {
            rsp = BuildErrorWithCodeJson(
                "group execution accepts only sql_text/group_mode/timeout fields",
                ErrorCodeId::kStreamGroupSqlTextInvalid);
            return error::BAD_REQUEST;
        }
        if (!doc.HasMember("group_mode") || !doc["group_mode"].IsString()) {
            rsp = BuildErrorJson("invalid request, group execution requires group_mode");
            return error::BAD_REQUEST;
        }
        if (std::string(doc["group_mode"].GetString()) != "dag") {
            rsp = BuildErrorWithCodeJson("invalid request, group_mode must be dag",
                                    ErrorCodeId::kStreamGroupModeInvalid);
            return error::BAD_REQUEST;
        }
        if (sqls.size() < 2) {
            rsp = BuildErrorWithCodeJson(
                "group execution requires at least two SQL statements in sql_text",
                ErrorCodeId::kStreamGroupSqlTextInvalid);
            return error::BAD_REQUEST;
        }
    } else {
        rsp = BuildErrorWithCodeJson(
            "invalid request, execution_kind must be single or group",
            ErrorCodeId::kStreamGroupSqlTextInvalid);
        return error::BAD_REQUEST;
    }

    rapidjson::StringBuffer scheduler_req_buf;
    rapidjson::Writer<rapidjson::StringBuffer> scheduler_req_w(scheduler_req_buf);
    scheduler_req_w.StartObject();
    scheduler_req_w.Key("execution_kind");
    scheduler_req_w.String(execution_kind.c_str());
    scheduler_req_w.Key("sql_text");
    scheduler_req_w.String(sql_text.c_str());
    scheduler_req_w.Key("timeout_s");
    scheduler_req_w.Int(timeout_s);
    if (execution_kind == "group") {
        scheduler_req_w.Key("group_mode");
        scheduler_req_w.String("dag");
        if (has_share_set_ready_timeout_s) {
            scheduler_req_w.Key("share_set_ready_timeout_s");
            scheduler_req_w.Int(share_set_ready_timeout_s);
        }
    }
    scheduler_req_w.EndObject();

    std::string scheduler_rsp;
    const int32_t rc = scheduler_client_.ExecuteStream(scheduler_req_buf.GetString(), &scheduler_rsp);
    if (rc != error::OK) {
        rsp = scheduler_rsp.empty() ? BuildErrorJson("scheduler stream execute failed") : scheduler_rsp;
        return rc;
    }

    rapidjson::Document exec_doc;
    exec_doc.Parse(scheduler_rsp.c_str());
    if (exec_doc.HasParseError() || !exec_doc.IsObject() ||
        !exec_doc.HasMember("runtime_task_id") || !exec_doc["runtime_task_id"].IsString()) {
        rsp = BuildErrorJson("invalid scheduler response: missing runtime_task_id");
        return error::INTERNAL_ERROR;
    }
    const std::string runtime_task_id = exec_doc["runtime_task_id"].GetString();
    if (runtime_task_id.empty()) {
        rsp = BuildErrorJson("invalid scheduler response: empty runtime_task_id");
        return error::INTERNAL_ERROR;
    }

    std::string task_id;
    const std::string request_summary =
        (execution_kind == "single")
            ? sqls.front()
            : ("[group] " + std::to_string(sqls.size()) + " SQL nodes");
    const std::string sqls_json = BuildSqlsJson(sqls);
    if (CreateTaskInternal(TruncateSummary(request_summary),
                           sql_text,
                           sqls_json,
                           static_cast<int>(sqls.size()),
                           timeout_s,
                           &task_id,
                           "stream",
                           runtime_task_id) != 0) {
        // 元数据写入失败时尝试停止 runtime task，避免悬挂任务。
        rapidjson::StringBuffer stop_req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> stop_w(stop_req_buf);
        stop_w.StartObject();
        stop_w.Key("task_id");
        stop_w.String(runtime_task_id.c_str());
        stop_w.EndObject();
        std::string ignored;
        (void)scheduler_client_.StopStream(stop_req_buf.GetString(), &ignored);

        rsp = BuildErrorJson("failed to create stream task");
        return error::INTERNAL_ERROR;
    }

    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> w(out);
    w.StartObject();
    w.Key("task_id");
    w.String(task_id.c_str());
    w.Key("runtime_task_id");
    w.String(runtime_task_id.c_str());
    w.Key("status");
    w.String("submitted");
    w.Key("runtime_kind");
    if (exec_doc.HasMember("runtime_kind") && exec_doc["runtime_kind"].IsString()) {
        w.String(exec_doc["runtime_kind"].GetString());
    } else {
        w.String(execution_kind == "group" ? "group" : "single");
    }
    if (exec_doc.HasMember("group_mode") && exec_doc["group_mode"].IsString()) {
        w.Key("group_mode");
        w.String(exec_doc["group_mode"].GetString());
    }
    if (exec_doc.HasMember("node_count") && exec_doc["node_count"].IsUint()) {
        w.Key("node_count");
        w.Uint(exec_doc["node_count"].GetUint());
    }
    if (exec_doc.HasMember("share_set_count") && exec_doc["share_set_count"].IsUint()) {
        w.Key("share_set_count");
        w.Uint(exec_doc["share_set_count"].GetUint());
    }
    w.EndObject();
    rsp = out.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleStreamStop(const std::string&, const std::string& req, std::string& rsp) {
    // 逻辑链：
    // 1) 校验任务归属与 runtime_task_id；
    // 2) 转发 stop 请求到 scheduler；
    // 3) 将 runtime status 映射为 task store 状态并更新错误信息；
    // 4) 回传 task/runtime 双视角状态，便于前端展示。
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = doc["task_id"].GetString();
    TaskRecord rec;
    if (GetTask(task_id, &rec) != 0) {
        rsp = BuildErrorJson("task not found");
        return error::NOT_FOUND;
    }
    if (rec.task_kind != "stream") {
        rsp = BuildErrorJson("task is not stream type");
        return error::BAD_REQUEST;
    }
    if (rec.runtime_task_id.empty()) {
        rsp = BuildErrorJson("runtime_task_id is empty");
        return error::CONFLICT;
    }

    rapidjson::StringBuffer req_buf;
    rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
    req_w.StartObject();
    req_w.Key("task_id");
    req_w.String(rec.runtime_task_id.c_str());
    req_w.EndObject();

    std::string scheduler_rsp;
    const int32_t rc = scheduler_client_.StopStream(req_buf.GetString(), &scheduler_rsp);
    if (rc != error::OK) {
        rsp = scheduler_rsp.empty() ? BuildErrorJson("scheduler stream stop failed") : scheduler_rsp;
        return rc;
    }

    rapidjson::Document stop_doc;
    stop_doc.Parse(scheduler_rsp.c_str());
    std::string runtime_status = "stopped";
    std::string runtime_kind = "single";
    std::string group_mode;
    std::string error_message;
    std::string runtime_error_code;
    int runtime_error_no = 0;
    if (!stop_doc.HasParseError() && stop_doc.IsObject()) {
        if (stop_doc.HasMember("status") && stop_doc["status"].IsString()) {
            runtime_status = stop_doc["status"].GetString();
        }
        if (stop_doc.HasMember("runtime_kind") && stop_doc["runtime_kind"].IsString()) {
            runtime_kind = stop_doc["runtime_kind"].GetString();
        }
        if (stop_doc.HasMember("group_mode") && stop_doc["group_mode"].IsString()) {
            group_mode = stop_doc["group_mode"].GetString();
        }
        if (stop_doc.HasMember("error_message") && stop_doc["error_message"].IsString()) {
            error_message = stop_doc["error_message"].GetString();
        }
        if (stop_doc.HasMember("error_code")) {
            ParseRuntimeErrorCode(stop_doc["error_code"], &runtime_error_code, &runtime_error_no);
        }
        if (stop_doc.HasMember("error_no") && stop_doc["error_no"].IsInt()) {
            runtime_error_no = stop_doc["error_no"].GetInt();
        }
    }

    TaskStatus mapped = MapStreamRuntimeStatus(runtime_status);
    if (mapped == TaskStatus::kPending || mapped == TaskStatus::kRunning) {
        mapped = TaskStatus::kStopped;
        runtime_status = "stopped";
    }
    if (mapped == TaskStatus::kStopped || mapped == TaskStatus::kCancelled || mapped == TaskStatus::kFailed) {
        const std::string err_code = (mapped == TaskStatus::kFailed) ? runtime_error_code : "";
        (void)UpdateStatus(task_id,
                           mapped,
                           err_code,
                           error_message,
                           "stream_stop",
                           0,
                           0,
                           "");
    }

    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> w(out);
    w.StartObject();
    w.Key("task_id");
    w.String(task_id.c_str());
    w.Key("runtime_task_id");
    w.String(rec.runtime_task_id.c_str());
    w.Key("status");
    w.String(StatusName(mapped));
    w.Key("runtime_status");
    w.String(runtime_status.c_str());
    w.Key("runtime_kind");
    w.String(runtime_kind.c_str());
    w.Key("error_code");
    w.String(runtime_error_code.c_str());
    w.Key("error_no");
    w.Int(runtime_error_no);
    if (!group_mode.empty()) {
        w.Key("group_mode");
        w.String(group_mode.c_str());
    }
    w.EndObject();
    rsp = out.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleStreamStatus(const std::string&, const std::string& req, std::string& rsp) {
    // 逻辑链：
    // 1) 读取 task store 快照，作为兜底状态；
    // 2) 若存在 runtime_task_id，则向 scheduler 拉取最新运行态；
    // 3) 将 runtime 状态回写 task store（仅在必要状态迁移时）；
    // 4) 输出融合后的状态、统计、节点与 share_set 视图。
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }

    const std::string task_id = doc["task_id"].GetString();
    TaskRecord rec;
    if (GetTask(task_id, &rec) != 0) {
        rsp = BuildErrorJson("task not found");
        return error::NOT_FOUND;
    }
    if (rec.task_kind != "stream") {
        rsp = BuildErrorJson("task is not stream type");
        return error::BAD_REQUEST;
    }

    std::string runtime_status = RuntimeStatusName(rec.status);
    std::string runtime_kind = "single";
    std::string group_mode;
    std::string runtime_error_code = rec.error_code;
    int runtime_error_no = 0;
    std::string runtime_error_message = rec.error_message;
    int64_t processed_rows = 0;
    int64_t output_rows = rec.result_row_count;
    std::string shared_hub_id;
    uint64_t subscriber_count = 0;
    rapidjson::Document shared_source_keys_doc;
    shared_source_keys_doc.SetArray();
    bool has_shared_source_keys = false;
    rapidjson::Document subscriber_stats_doc;
    subscriber_stats_doc.SetArray();
    bool has_subscriber_stats = false;
    rapidjson::Document op_stats_doc;
    bool has_op_stats = false;
    rapidjson::Document nodes_doc;
    bool has_nodes = false;
    rapidjson::Document share_sets_doc;
    bool has_share_sets = false;
    rapidjson::Document resolved_sources_doc;
    bool has_resolved_sources = false;

    if (!rec.runtime_task_id.empty()) {
        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
        req_w.StartObject();
        req_w.Key("task_id");
        req_w.String(rec.runtime_task_id.c_str());
        req_w.EndObject();

        std::string scheduler_rsp;
        const int32_t rc = scheduler_client_.QueryStreamStatus(req_buf.GetString(), &scheduler_rsp);
        if (rc == error::OK) {
            rapidjson::Document runtime_doc;
            runtime_doc.Parse(scheduler_rsp.c_str());
            if (!runtime_doc.HasParseError() && runtime_doc.IsObject()) {
                if (runtime_doc.HasMember("status") && runtime_doc["status"].IsString()) {
                    runtime_status = runtime_doc["status"].GetString();
                }
                if (runtime_doc.HasMember("runtime_kind") && runtime_doc["runtime_kind"].IsString()) {
                    runtime_kind = runtime_doc["runtime_kind"].GetString();
                }
                if (runtime_doc.HasMember("group_mode") && runtime_doc["group_mode"].IsString()) {
                    group_mode = runtime_doc["group_mode"].GetString();
                }
                if (runtime_doc.HasMember("error_message") && runtime_doc["error_message"].IsString()) {
                    runtime_error_message = runtime_doc["error_message"].GetString();
                }
                if (runtime_doc.HasMember("error_code")) {
                    ParseRuntimeErrorCode(runtime_doc["error_code"], &runtime_error_code, &runtime_error_no);
                }
                if (runtime_doc.HasMember("error_no") && runtime_doc["error_no"].IsInt()) {
                    runtime_error_no = runtime_doc["error_no"].GetInt();
                }
                if (runtime_doc.HasMember("processed_rows") && runtime_doc["processed_rows"].IsUint64()) {
                    processed_rows = static_cast<int64_t>(runtime_doc["processed_rows"].GetUint64());
                }
                if (runtime_doc.HasMember("output_rows") && runtime_doc["output_rows"].IsUint64()) {
                    output_rows = static_cast<int64_t>(runtime_doc["output_rows"].GetUint64());
                }
                if (runtime_doc.HasMember("shared_hub_id") && runtime_doc["shared_hub_id"].IsString()) {
                    shared_hub_id = runtime_doc["shared_hub_id"].GetString();
                }
                if (runtime_doc.HasMember("shared_source_keys") && runtime_doc["shared_source_keys"].IsArray()) {
                    shared_source_keys_doc.CopyFrom(runtime_doc["shared_source_keys"],
                                                    shared_source_keys_doc.GetAllocator());
                    has_shared_source_keys = true;
                }
                if (runtime_doc.HasMember("subscriber_count") && runtime_doc["subscriber_count"].IsUint64()) {
                    subscriber_count = runtime_doc["subscriber_count"].GetUint64();
                } else if (runtime_doc.HasMember("subscriber_count") && runtime_doc["subscriber_count"].IsUint()) {
                    subscriber_count = runtime_doc["subscriber_count"].GetUint();
                }
                if (runtime_doc.HasMember("subscriber_stats") && runtime_doc["subscriber_stats"].IsArray()) {
                    subscriber_stats_doc.CopyFrom(runtime_doc["subscriber_stats"],
                                                  subscriber_stats_doc.GetAllocator());
                    has_subscriber_stats = true;
                }
                if (runtime_doc.HasMember("op_stats")) {
                    op_stats_doc.CopyFrom(runtime_doc["op_stats"], op_stats_doc.GetAllocator());
                    has_op_stats = true;
                }
                if (runtime_doc.HasMember("nodes")) {
                    nodes_doc.CopyFrom(runtime_doc["nodes"], nodes_doc.GetAllocator());
                    has_nodes = true;
                }
                if (runtime_doc.HasMember("share_sets")) {
                    share_sets_doc.CopyFrom(runtime_doc["share_sets"], share_sets_doc.GetAllocator());
                    has_share_sets = true;
                }
                if (runtime_doc.HasMember("resolved_sources")) {
                    resolved_sources_doc.CopyFrom(runtime_doc["resolved_sources"], resolved_sources_doc.GetAllocator());
                    has_resolved_sources = true;
                }
            }

            const TaskStatus mapped = MapStreamRuntimeStatus(runtime_status);
            if (mapped != rec.status) {
                if (mapped == TaskStatus::kStopped ||
                    mapped == TaskStatus::kCancelled ||
                    mapped == TaskStatus::kFailed) {
                    const std::string err_code = (mapped == TaskStatus::kFailed) ? runtime_error_code : "";
                    (void)UpdateStatus(task_id,
                                       mapped,
                                       err_code,
                                       runtime_error_message,
                                       "stream_status",
                                       output_rows,
                                       0,
                                       "");
                    (void)GetTask(task_id, &rec);
                } else if (mapped == TaskStatus::kRunning) {
                    (void)UpdateStatus(task_id, TaskStatus::kRunning, "", "", "", output_rows, 0, "");
                    (void)GetTask(task_id, &rec);
                }
            }
        } else if (!IsTerminal(rec.status)) {
            rsp = scheduler_rsp.empty() ? BuildErrorJson("scheduler stream status failed") : scheduler_rsp;
            return rc;
        }
    }

    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> w(out);
    w.StartObject();
    w.Key("task_id");
    w.String(task_id.c_str());
    w.Key("runtime_task_id");
    w.String(rec.runtime_task_id.c_str());
    w.Key("status");
    w.String(StatusName(rec.status));
    w.Key("runtime_status");
    w.String(runtime_status.c_str());
    w.Key("runtime_kind");
    w.String(runtime_kind.c_str());
    if (!group_mode.empty()) {
        w.Key("group_mode");
        w.String(group_mode.c_str());
    }
    w.Key("terminal_reason");
    w.String(rec.error_stage.c_str());
    w.Key("error_code");
    const std::string out_error_code = rec.error_code.empty() ? runtime_error_code : rec.error_code;
    w.String(out_error_code.c_str());
    w.Key("error_no");
    int out_error_no = runtime_error_no;
    if (!rec.error_code.empty()) {
        char* endptr = nullptr;
        const long v = std::strtol(rec.error_code.c_str(), &endptr, 10);
        if (endptr && *endptr == '\0') out_error_no = static_cast<int>(v);
    }
    w.Int(out_error_no);
    w.Key("error_message");
    w.String(rec.error_message.empty() ? runtime_error_message.c_str() : rec.error_message.c_str());
    w.Key("processed_rows");
    w.Int64(processed_rows);
    w.Key("output_rows");
    w.Int64(output_rows);
    w.Key("shared_hub_id");
    w.String(shared_hub_id.c_str());
    w.Key("shared_source_keys");
    if (has_shared_source_keys) {
        shared_source_keys_doc.Accept(w);
    } else {
        w.StartArray();
        w.EndArray();
    }
    w.Key("subscriber_count");
    w.Uint64(subscriber_count);
    w.Key("subscriber_stats");
    if (has_subscriber_stats) {
        subscriber_stats_doc.Accept(w);
    } else {
        w.StartArray();
        w.EndArray();
    }
    w.Key("op_stats");
    if (has_op_stats) {
        op_stats_doc.Accept(w);
    } else {
        w.StartObject();
        w.EndObject();
    }
    w.Key("resolved_sources");
    if (has_resolved_sources) {
        resolved_sources_doc.Accept(w);
    } else {
        w.StartArray();
        w.EndArray();
    }
    w.Key("nodes");
    if (has_nodes) {
        nodes_doc.Accept(w);
    } else {
        w.StartArray();
        w.EndArray();
    }
    w.Key("share_sets");
    if (has_share_sets) {
        share_sets_doc.Accept(w);
    } else {
        w.StartArray();
        w.EndArray();
    }
    w.EndObject();
    rsp = out.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleStreamList(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    int page = 1;
    int page_size = 20;
    std::string status_filter;
    if (!d.HasParseError() && d.IsObject()) {
        if (d.HasMember("page") && d["page"].IsInt()) page = d["page"].GetInt();
        if (d.HasMember("page_size") && d["page_size"].IsInt()) page_size = d["page_size"].GetInt();
        if (d.HasMember("status") && d["status"].IsString()) status_filter = d["status"].GetString();
    }

    std::vector<TaskRecord> items;
    int64_t total = 0;
    if (ListTasksByKind("stream", page, page_size, status_filter, &items, &total) != 0) {
        rsp = BuildErrorJson("failed to list stream tasks");
        return error::INTERNAL_ERROR;
    }

    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> w(out);
    w.StartObject();
    w.Key("total");
    w.Int64(total);
    w.Key("tasks");
    w.StartArray();
    for (const auto& item : items) {
        std::string sql_text;
        if (!LoadTaskRawSqlTextFromStore(store_.get(), item.task_id, &sql_text)) {
            rsp = BuildErrorWithCodeJson("invalid task sql payload", "TASK_SQL_PAYLOAD_INVALID");
            return error::INTERNAL_ERROR;
        }
        std::string shared_hub_id;
        uint64_t subscriber_count = 0;
        rapidjson::Document shared_source_keys_doc;
        shared_source_keys_doc.SetArray();
        bool has_shared_source_keys = false;
        rapidjson::Document subscriber_stats_doc;
        subscriber_stats_doc.SetArray();
        bool has_subscriber_stats = false;
        if (!item.runtime_task_id.empty()) {
            rapidjson::StringBuffer req_buf;
            rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
            req_w.StartObject();
            req_w.Key("task_id");
            req_w.String(item.runtime_task_id.c_str());
            req_w.EndObject();

            std::string scheduler_rsp;
            if (scheduler_client_.QueryStreamStatus(req_buf.GetString(), &scheduler_rsp) == error::OK) {
                rapidjson::Document runtime_doc;
                runtime_doc.Parse(scheduler_rsp.c_str());
                if (!runtime_doc.HasParseError() && runtime_doc.IsObject()) {
                    if (runtime_doc.HasMember("shared_hub_id") && runtime_doc["shared_hub_id"].IsString()) {
                        shared_hub_id = runtime_doc["shared_hub_id"].GetString();
                    }
                    if (runtime_doc.HasMember("shared_source_keys") && runtime_doc["shared_source_keys"].IsArray()) {
                        shared_source_keys_doc.CopyFrom(runtime_doc["shared_source_keys"],
                                                        shared_source_keys_doc.GetAllocator());
                        has_shared_source_keys = true;
                    }
                    if (runtime_doc.HasMember("subscriber_count") && runtime_doc["subscriber_count"].IsUint64()) {
                        subscriber_count = runtime_doc["subscriber_count"].GetUint64();
                    } else if (runtime_doc.HasMember("subscriber_count") &&
                               runtime_doc["subscriber_count"].IsUint()) {
                        subscriber_count = runtime_doc["subscriber_count"].GetUint();
                    }
                    if (runtime_doc.HasMember("subscriber_stats") && runtime_doc["subscriber_stats"].IsArray()) {
                        subscriber_stats_doc.CopyFrom(runtime_doc["subscriber_stats"],
                                                      subscriber_stats_doc.GetAllocator());
                        has_subscriber_stats = true;
                    }
                }
            }
        }
        w.StartObject();
        w.Key("task_id");
        w.String(item.task_id.c_str());
        w.Key("runtime_task_id");
        w.String(item.runtime_task_id.c_str());
        w.Key("status");
        w.String(StatusName(item.status));
        w.Key("sql_text");
        w.String(sql_text.c_str());
        w.Key("error_code");
        w.String(item.error_code.c_str());
        w.Key("error_message");
        w.String(item.error_message.c_str());
        w.Key("shared_hub_id");
        w.String(shared_hub_id.c_str());
        w.Key("shared_source_keys");
        if (has_shared_source_keys) {
            shared_source_keys_doc.Accept(w);
        } else {
            w.StartArray();
            w.EndArray();
        }
        w.Key("subscriber_count");
        w.Uint64(subscriber_count);
        w.Key("subscriber_stats");
        if (has_subscriber_stats) {
            subscriber_stats_doc.Accept(w);
        } else {
            w.StartArray();
            w.EndArray();
        }
        w.Key("created_at");
        w.String(item.created_at.c_str());
        w.Key("updated_at");
        w.String(item.updated_at.c_str());
        w.Key("finished_at");
        w.String(item.finished_at.c_str());
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    rsp = out.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleRuntimeGraphQuery(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = BuildErrorWithCodeJson(
            "invalid request, expected {\"task_id\":\"...\"}",
            "RUNTIME_GRAPH_CURSOR_INVALID");
        return error::BAD_REQUEST;
    }

    const std::string task_id = doc["task_id"].GetString();
    if (task_id.empty()) {
        rsp = BuildErrorWithCodeJson("invalid request, task_id must not be empty",
                                     "RUNTIME_GRAPH_CURSOR_INVALID");
        return error::BAD_REQUEST;
    }

    uint64_t cursor = 0;
    if (doc.HasMember("cursor")) {
        const auto& cursor_v = doc["cursor"];
        if (cursor_v.IsUint64()) {
            cursor = cursor_v.GetUint64();
        } else if (cursor_v.IsUint()) {
            cursor = cursor_v.GetUint();
        } else if (cursor_v.IsInt64()) {
            if (cursor_v.GetInt64() < 0) {
                rsp = BuildErrorWithCodeJson("invalid request, cursor must be >= 0",
                                             "RUNTIME_GRAPH_CURSOR_INVALID");
                return error::BAD_REQUEST;
            }
            cursor = static_cast<uint64_t>(cursor_v.GetInt64());
        } else if (cursor_v.IsInt()) {
            if (cursor_v.GetInt() < 0) {
                rsp = BuildErrorWithCodeJson("invalid request, cursor must be >= 0",
                                             "RUNTIME_GRAPH_CURSOR_INVALID");
                return error::BAD_REQUEST;
            }
            cursor = static_cast<uint64_t>(cursor_v.GetInt());
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

    TaskRecord rec;
    if (GetTask(task_id, &rec) != 0) {
        rsp = BuildErrorWithCodeJson("task not found", "RUNTIME_GRAPH_TASK_NOT_FOUND");
        return error::NOT_FOUND;
    }

    std::string raw_sql_text;
    std::vector<std::string> sqls;
    if (!LoadTaskSqlPayloadFromStore(store_.get(), task_id, &raw_sql_text, &sqls)) {
        rsp = BuildErrorWithCodeJson("invalid task sql payload", "RUNTIME_GRAPH_SQL_PAYLOAD_INVALID");
        return error::INTERNAL_ERROR;
    }

    const std::string degraded_runtime_kind =
        rec.task_kind == "batch" ? "batch" : "single";

    auto InjectSqlPayload = [&](rapidjson::Document* graph_doc) {
        if (!graph_doc || !graph_doc->IsObject()) return;
        auto& alloc = graph_doc->GetAllocator();

        if (graph_doc->HasMember("sql_text")) {
            (*graph_doc)["sql_text"].SetString(raw_sql_text.c_str(), alloc);
        } else {
            rapidjson::Value sql_text_v;
            sql_text_v.SetString(raw_sql_text.c_str(), alloc);
            graph_doc->AddMember("sql_text", sql_text_v, alloc);
        }

        rapidjson::Value sqls_v(rapidjson::kArrayType);
        for (const auto& sql : sqls) {
            rapidjson::Value one;
            one.SetString(sql.c_str(), alloc);
            sqls_v.PushBack(one, alloc);
        }
        if (graph_doc->HasMember("sqls")) {
            (*graph_doc)["sqls"].Swap(sqls_v);
        } else {
            graph_doc->AddMember("sqls", sqls_v, alloc);
        }
    };

    auto UpsertStringField = [](rapidjson::Document* graph_doc, const char* key, const std::string& value) {
        if (!graph_doc || !graph_doc->IsObject() || !key) return;
        auto& alloc = graph_doc->GetAllocator();
        if (graph_doc->HasMember(key)) {
            (*graph_doc)[key].SetString(value.c_str(), alloc);
        } else {
            rapidjson::Value k;
            k.SetString(key, alloc);
            rapidjson::Value v;
            v.SetString(value.c_str(), alloc);
            graph_doc->AddMember(k, v, alloc);
        }
    };

    auto UpsertBoolField = [](rapidjson::Document* graph_doc, const char* key, bool value) {
        if (!graph_doc || !graph_doc->IsObject() || !key) return;
        auto& alloc = graph_doc->GetAllocator();
        if (graph_doc->HasMember(key)) {
            (*graph_doc)[key].SetBool(value);
        } else {
            rapidjson::Value k;
            k.SetString(key, alloc);
            rapidjson::Value v;
            v.SetBool(value);
            graph_doc->AddMember(k, v, alloc);
        }
    };

    if (rec.runtime_task_id.empty()) {
        const std::string local_graph = BuildLocalRuntimeGraph(rec, sqls, cursor, include_events, degraded_runtime_kind);
        rapidjson::Document graph_doc;
        graph_doc.Parse(local_graph.c_str());
        if (graph_doc.HasParseError() || !graph_doc.IsObject()) {
            rsp = BuildErrorWithCodeJson("invalid runtime graph payload", "RUNTIME_GRAPH_BUILD_FAILED");
            return error::INTERNAL_ERROR;
        }
        InjectSqlPayload(&graph_doc);
        UpsertStringField(&graph_doc, "graph_source", "reconstructed");
        UpsertBoolField(&graph_doc, "degraded", true);
        UpsertStringField(&graph_doc, "degrade_reason", "runtime_task_id_missing");
        rapidjson::StringBuffer out;
        rapidjson::Writer<rapidjson::StringBuffer> out_w(out);
        graph_doc.Accept(out_w);
        rsp = out.GetString();
        return error::OK;
    }

    rapidjson::StringBuffer scheduler_req_buf;
    rapidjson::Writer<rapidjson::StringBuffer> scheduler_req_w(scheduler_req_buf);
    scheduler_req_w.StartObject();
    scheduler_req_w.Key("task_id");
    scheduler_req_w.String(rec.runtime_task_id.c_str());
    scheduler_req_w.Key("task_kind");
    scheduler_req_w.String(rec.task_kind.c_str());
    scheduler_req_w.Key("cursor");
    scheduler_req_w.Uint64(cursor);
    scheduler_req_w.Key("include_events");
    scheduler_req_w.Bool(include_events);
    scheduler_req_w.Key("sqls");
    scheduler_req_w.StartArray();
    for (const auto& sql : sqls) {
        scheduler_req_w.String(sql.c_str());
    }
    scheduler_req_w.EndArray();
    scheduler_req_w.EndObject();

    std::string scheduler_rsp;
    const int32_t rc = scheduler_client_.QueryRuntimeGraph(scheduler_req_buf.GetString(), &scheduler_rsp);
    if (rc != error::OK) {
        bool runtime_not_found = (rc == error::NOT_FOUND);
        if (!runtime_not_found && !scheduler_rsp.empty()) {
            rapidjson::Document err_doc;
            err_doc.Parse(scheduler_rsp.c_str());
            if (!err_doc.HasParseError() && err_doc.IsObject()) {
                if (err_doc.HasMember("error_code") && err_doc["error_code"].IsString()) {
                    runtime_not_found =
                        std::string(err_doc["error_code"].GetString()) == "RUNTIME_GRAPH_RUNTIME_NOT_FOUND";
                }
                if (!runtime_not_found && err_doc.HasMember("error") && err_doc["error"].IsString()) {
                    const std::string err = err_doc["error"].GetString();
                    runtime_not_found =
                        err.find("runtime task not found") != std::string::npos ||
                        err.find("stream task not found") != std::string::npos;
                }
            }
        }
        if (runtime_not_found) {
            const std::string local_graph = BuildLocalRuntimeGraph(rec, sqls, cursor, include_events, degraded_runtime_kind);
            rapidjson::Document graph_doc;
            graph_doc.Parse(local_graph.c_str());
            if (graph_doc.HasParseError() || !graph_doc.IsObject()) {
                rsp = BuildErrorWithCodeJson("invalid runtime graph payload", "RUNTIME_GRAPH_BUILD_FAILED");
                return error::INTERNAL_ERROR;
            }
            InjectSqlPayload(&graph_doc);
            UpsertStringField(&graph_doc, "graph_source", "reconstructed");
            UpsertBoolField(&graph_doc, "degraded", true);
            UpsertStringField(&graph_doc, "degrade_reason", "runtime_not_found");
            rapidjson::StringBuffer out;
            rapidjson::Writer<rapidjson::StringBuffer> out_w(out);
            graph_doc.Accept(out_w);
            rsp = out.GetString();
            return error::OK;
        }
        rsp = scheduler_rsp.empty()
            ? BuildErrorWithCodeJson(
                  rc == error::NOT_FOUND ? "runtime graph runtime not found" : "runtime graph build failed",
                  rc == error::NOT_FOUND ? "RUNTIME_GRAPH_RUNTIME_NOT_FOUND" : "RUNTIME_GRAPH_BUILD_FAILED")
            : scheduler_rsp;
        return rc;
    }

    rapidjson::Document graph_doc;
    graph_doc.Parse(scheduler_rsp.c_str());
    if (graph_doc.HasParseError() || !graph_doc.IsObject()) {
        rsp = BuildErrorWithCodeJson("invalid runtime graph payload", "RUNTIME_GRAPH_BUILD_FAILED");
        return error::INTERNAL_ERROR;
    }
    auto& alloc = graph_doc.GetAllocator();
    if (graph_doc.HasMember("task_id")) {
        graph_doc["task_id"].SetString(task_id.c_str(), alloc);
    } else {
        rapidjson::Value task_id_v;
        task_id_v.SetString(task_id.c_str(), alloc);
        graph_doc.AddMember("task_id", task_id_v, alloc);
    }
    if (!graph_doc.HasMember("runtime_task_id")) {
        rapidjson::Value runtime_task_id_v;
        runtime_task_id_v.SetString(rec.runtime_task_id.c_str(), alloc);
        graph_doc.AddMember("runtime_task_id", runtime_task_id_v, alloc);
    }
    if (!graph_doc.HasMember("task_kind")) {
        rapidjson::Value task_kind_v;
        task_kind_v.SetString(rec.task_kind.c_str(), alloc);
        graph_doc.AddMember("task_kind", task_kind_v, alloc);
    }
    InjectSqlPayload(&graph_doc);
    UpsertStringField(&graph_doc, "graph_source", "live");
    UpsertBoolField(&graph_doc, "degraded", false);
    UpsertStringField(&graph_doc, "degrade_reason", "");

    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> out_w(out);
    graph_doc.Accept(out_w);
    rsp = out.GetString();
    return error::OK;
}

void TaskPlugin::EnumRoutes(std::function<void(const RouteItem&)> callback) {
    callback({"POST", "/tasks/batch/execute",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleBatchExecute(u, req, rsp);
              }});
    callback({"POST", "/tasks/sql/classify",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleSqlClassify(u, req, rsp);
              }});
    callback({"POST", "/tasks/sql/analyze",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleSqlAnalyze(u, req, rsp);
              }});
    callback({"POST", "/tasks/list",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleList(u, req, rsp);
              }});
    callback({"POST", "/tasks/detail",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleDetail(u, req, rsp);
              }});
    callback({"POST", "/tasks/diagnostics",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleDiagnostics(u, req, rsp);
              }});
    callback({"POST", "/tasks/delete",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleDelete(u, req, rsp);
              }});
    callback({"POST", "/tasks/cancel",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleCancel(u, req, rsp);
              }});
    callback({"POST", "/tasks/stream/execute",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleStreamExecute(u, req, rsp);
              }});
    callback({"POST", "/tasks/stream/stop",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleStreamStop(u, req, rsp);
              }});
    callback({"POST", "/tasks/stream/status",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleStreamStatus(u, req, rsp);
              }});
    callback({"POST", "/tasks/stream/list",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleStreamList(u, req, rsp);
              }});
    callback({"POST", "/tasks/runtime/graph/query",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleRuntimeGraphQuery(u, req, rsp);
              }});
}

}  // namespace task
}  // namespace flowsql
