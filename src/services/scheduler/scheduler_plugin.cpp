#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <common/error_code.h>
#include <common/log.h>
#include <cstdlib>
#include <cstring>
#include <regex>

#include "framework/core/channel_adapter.h"
#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/pipeline.h"
#include "framework/core/sql_parser.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idatabase_factory.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/ioperator.h"

namespace flowsql {
namespace scheduler {

// --- JSON 辅助 ---
static std::string MakeErrorJson(const std::string& error) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.EndObject();
    return buf.GetString();
}

// --- IPlugin ---
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

// --- 通道管理 ---
void SchedulerPlugin::RegisterChannel(const std::string& key, std::shared_ptr<IChannel> ch) {
    channels_[key] = std::move(ch);
}

// --- IPlugin::Start ---
int SchedulerPlugin::Start() {
    LOG_INFO("SchedulerPlugin::Start: ready");
    return 0;
}

int SchedulerPlugin::Stop() {
    channels_.clear();
    LOG_INFO("SchedulerPlugin::Stop: done");
    return 0;
}

// --- IRouterHandle ---
void SchedulerPlugin::EnumRoutes(std::function<void(const RouteItem&)> cb) {
    // 任务执行
    cb({"POST", "/tasks/instant/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleExecute(u, req, rsp);
        }});
    // 内存通道查询
    cb({"POST", "/channels/dataframe/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetChannels(u, req, rsp);
        }});
    // 内存通道数据预览
    cb({"POST", "/channels/dataframe/preview",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandlePreviewDataframe(u, req, rsp);
        }});
    // 算子查询
    cb({"POST", "/operators/native/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetOperators(u, req, rsp);
        }});
    // Python 算子刷新
    cb({"POST", "/operators/python/refresh",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRefreshOperators(u, req, rsp);
        }});
}

// --- 通道查找辅助 ---
IChannel* SchedulerPlugin::FindChannel(const std::string& name) {
    // 先在内部通道表中查找
    auto it = channels_.find(name);
    if (it != channels_.end()) return it->second.get();

    // 按 catelog.name 格式拆分后查找
    auto dot = name.find('.');
    if (dot != std::string::npos) {
        std::string key = name.substr(0, dot) + "." + name.substr(dot + 1);
        it = channels_.find(key);
        if (it != channels_.end()) return it->second.get();
    }

    // 通过 IQuerier 遍历静态注册的通道
    IChannel* found = nullptr;
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&](void* p) -> int {
            auto* c = static_cast<IChannel*>(p);
            std::string full_name = std::string(c->Catelog()) + "." + c->Name();
            if (full_name == name || std::string(c->Name()) == name) {
                found = c;
                return -1;  // 找到了，停止遍历
            }
            return 0;
        });
    }

    // 模糊匹配内部通道表
    if (!found) {
        // 【第四层】尝试通过 IDatabaseFactory 获取数据库通道
        // 支持三段式（type.name.table）和两段式（type.name）
        if (querier_) {
            auto* factory = static_cast<IDatabaseFactory*>(
                querier_->First(IID_DATABASE_FACTORY));
            if (factory) {
                // 尝试解析 type.name 格式
                auto pos = name.find('.');
                if (pos != std::string::npos) {
                    std::string type = name.substr(0, pos);
                    std::string rest = name.substr(pos + 1);
                    // 对于三段式 type.name.table，取前两段作为 type.name
                    auto pos2 = rest.find('.');
                    std::string db_name = (pos2 != std::string::npos) ? rest.substr(0, pos2) : rest;

                    auto* db_ch = factory->Get(type.c_str(), db_name.c_str());
                    if (db_ch) found = db_ch;
                }
            }
        }
    }

    return found;
}

// --- 算子查找 ---
// 先查 C++ 静态算子（IQuerier），再查 Python 算子（IBridge）
std::shared_ptr<IOperator> SchedulerPlugin::FindOperator(const std::string& catelog, const std::string& name) {
    if (!querier_) return nullptr;

    // 1. 先查 C++ 静态算子
    IOperator* found = nullptr;
    querier_->Traverse(IID_OPERATOR, [&](void* p) -> int {
        auto* op = static_cast<IOperator*>(p);
        if (op->Catelog() == catelog && op->Name() == name) {
            found = op;
            return -1;
        }
        return 0;
    });
    // C++ 算子由 PluginLoader 管理生命周期，用空 deleter 包装
    if (found) return std::shared_ptr<IOperator>(found, [](IOperator*) {});

    // 2. 再查 Python 算子（通过 IBridge）
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) return bridge->FindOperator(catelog, name);
    return nullptr;
}

// --- Build Database Query ---

// 从目标名称中提取表名（支持三段式 type.name.table）
static std::string ExtractTableName(const std::string& dest_name) {
    auto pos1 = dest_name.find('.');
    if (pos1 != std::string::npos) {
        auto pos2 = dest_name.find('.', pos1 + 1);
        if (pos2 != std::string::npos) {
            return dest_name.substr(pos2 + 1);  // 三段式
        }
        return dest_name.substr(pos1 + 1);  // 两段式
    }
    return dest_name;
}

// BuildQuery: 构建数据库查询语句
// 将 sql_part 中的第一个 FROM 子句替换为实际表名（支持多段式表名规范化）
// 子查询中的 FROM 保持原样，不做替换
static std::string BuildQuery(const std::string& source_name, const SqlStatement& stmt) {
    std::string sql = stmt.sql_part;
    std::string table = ExtractTableName(source_name);

    // 匹配第一个 FROM 子句（支持多段式表名如 catalog.db.table）
    std::regex FROM_PATTERN(R"((\bFROM\s+)((?:[\w]+\.)*[\w]+))");
    std::smatch m;
    if (std::regex_search(sql, m, FROM_PATTERN)) {
        // 只替换第一个匹配（主查询的 FROM），子查询不受影响
        sql = sql.substr(0, m.position()) +
              m[1].str() + table +
              sql.substr(m.position() + m.length());
    }

    return sql;
}

// --- 辅助：对 DataFrame 通道应用 WHERE 过滤 ---
static std::shared_ptr<DataFrameChannel> ApplyDataFrameFilter(
    IDataFrameChannel* src, const std::string& where_clause, uint64_t seq) {
    DataFrame data;
    if (src->Read(&data) != 0 || data.RowCount() == 0) return nullptr;

    if (data.Filter(where_clause.c_str()) != 0) return nullptr;

    auto filtered = std::make_shared<DataFrameChannel>("_filter", std::to_string(seq));
    filtered->Open();
    filtered->Write(&data);
    return filtered;
}

// --- 无算子：纯数据搬运 ---
int SchedulerPlugin::ExecuteTransfer(IChannel* source, IChannel* sink,
                                      const std::string& source_type,
                                      const std::string& sink_type,
                                      const SqlStatement& stmt, int64_t* rows_affected,
                                      std::string* error) {
    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;

        // DataFrame + WHERE → 先过滤再复制
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            return ChannelAdapter::CopyDataFrame(filtered.get(), dst);
        }
        return ChannelAdapter::CopyDataFrame(src, dst);
    }

    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kDatabase) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;
        std::string table = ExtractTableName(stmt.dest);

        // DataFrame + WHERE → 先过滤再写入
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            int64_t rows = ChannelAdapter::WriteFromDataFrame(filtered.get(), dst, table.c_str(), error);
            if (rows_affected) *rows_affected = rows;
            return (rows < 0) ? -1 : 0;
        }
        int64_t rows = ChannelAdapter::WriteFromDataFrame(src, dst, table.c_str(), error);
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (source_type == ChannelType::kDatabase && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;
        std::string query = BuildQuery(stmt.source, stmt);
        return ChannelAdapter::ReadToDataFrame(src, query.c_str(), dst, error);
    }

    if (source_type == ChannelType::kDatabase && sink_type == ChannelType::kDatabase) {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;

        auto tmp = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
        tmp->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(src, query.c_str(), tmp.get(), error);
        if (rc != 0) return rc;

        std::string table = ExtractTableName(stmt.dest);
        int64_t rows = ChannelAdapter::WriteFromDataFrame(tmp.get(), dst, table.c_str(), error);
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (error) *error = "unsupported transfer: " + source_type + " → " + sink_type;
    return -1;
}

// --- 有算子：自动适配通道类型 ---
int SchedulerPlugin::ExecuteWithOperator(IChannel* source, IChannel* sink,
                                          IOperator* op,
                                          const std::string& source_type,
                                          const std::string& sink_type,
                                          const SqlStatement& stmt, int64_t* rows_affected,
                                          std::string* error) {
    IChannel* actual_source = source;
    IChannel* actual_sink = sink;
    std::shared_ptr<DataFrameChannel> tmp_in, tmp_out;

    if (source_type == ChannelType::kDatabase) {
        auto* db_src = dynamic_cast<IDatabaseChannel*>(source);
        if (!db_src) return -1;

        tmp_in = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
        tmp_in->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(db_src, query.c_str(), tmp_in.get(), error);
        if (rc != 0) return rc;

        actual_source = tmp_in.get();
    } else if (source_type == ChannelType::kDataFrame && !stmt.where_clause.empty()) {
        // DataFrame + WHERE → 先过滤
        auto* df_src = dynamic_cast<IDataFrameChannel*>(source);
        if (!df_src) return -1;

        tmp_in = ApplyDataFrameFilter(df_src, stmt.where_clause, ++tmp_channel_seq_);
        if (!tmp_in) return -1;
        actual_source = tmp_in.get();
    }

    if (sink_type == ChannelType::kDatabase) {
        tmp_out = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
        tmp_out->Open();
        actual_sink = tmp_out.get();
    }

    auto pipeline = PipelineBuilder()
                        .SetSource(actual_source)
                        .SetOperator(op)
                        .SetSink(actual_sink)
                        .Build();
    // 注意：Run() 当前为同步执行，返回时 pipeline 已完成或失败。
    // 若未来改为异步，需在此处等待完成（如 pipeline->Wait()）再检查 State()。
    pipeline->Run();

    if (pipeline->State() == PipelineState::FAILED) {
        if (error) *error = pipeline->ErrorMessage();
        return -1;
    }

    if (sink_type == ChannelType::kDatabase && tmp_out) {
        auto* db_sink = dynamic_cast<IDatabaseChannel*>(sink);
        if (!db_sink) return -1;

        std::string table = ExtractTableName(stmt.dest);
        int64_t written_rows = ChannelAdapter::WriteFromDataFrame(tmp_out.get(), db_sink, table.c_str(), error);
        if (written_rows < 0) return -1;

        if (rows_affected) *rows_affected = written_rows;
    }

    return 0;
}

// --- HandleExecute ---
int32_t SchedulerPlugin::HandleExecute(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }
    std::string sql_text = doc["sql"].GetString();

    static constexpr size_t kMaxSqlLength = 64 * 1024;
    if (sql_text.size() > kMaxSqlLength) {
        rsp = MakeErrorJson("SQL too long (max 64KB)");
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    auto stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        rsp = MakeErrorJson(stmt.error);
        return error::BAD_REQUEST;
    }

    IChannel* source = FindChannel(stmt.source);
    if (!source) {
        rsp = MakeErrorJson("source channel not found: " + stmt.source);
        return error::BAD_REQUEST;
    }

    IOperator* op = nullptr;
    std::shared_ptr<IOperator> op_holder;
    if (stmt.HasOperator()) {
        op_holder = FindOperator(stmt.op_catelog, stmt.op_name);
        if (!op_holder) {
            rsp = MakeErrorJson("operator not found: " + stmt.op_catelog + "." + stmt.op_name);
            return error::BAD_REQUEST;
        }
        op = op_holder.get();
    }

    try {
        if (op) {
            for (auto& [k, v] : stmt.with_params) {
                op->Configure(k.c_str(), v.c_str());
            }
        }

        std::shared_ptr<DataFrameChannel> temp_sink;
        IChannel* sink = nullptr;

        if (!stmt.dest.empty()) {
            sink = FindChannel(stmt.dest);
            if (!sink) {
                temp_sink = std::make_shared<DataFrameChannel>("result", stmt.dest);
                temp_sink->Open();
                sink = temp_sink.get();
            }
        } else {
            temp_sink = std::make_shared<DataFrameChannel>("_temp", "sink");
            temp_sink->Open();
            sink = temp_sink.get();
        }

        std::string source_type(source->Type());
        std::string sink_type(sink->Type());

        int rc = 0;
        int64_t affected_rows = 0;
        std::string exec_error;

        if (!op) {
            rc = ExecuteTransfer(source, sink, source_type, sink_type, stmt, &affected_rows, &exec_error);
        } else {
            rc = ExecuteWithOperator(source, sink, op, source_type, sink_type, stmt, &affected_rows, &exec_error);
        }

        if (rc != 0) {
            std::string err = exec_error;
            if (err.empty() && op) err = op->LastError();
            if (err.empty()) err = "execution failed";
            rsp = MakeErrorJson(err);
            return error::INTERNAL_ERROR;
        }

        auto* df_sink = dynamic_cast<IDataFrameChannel*>(sink);
        DataFrame result;
        std::string result_json = "[]";
        int64_t row_count = 0;
        if (df_sink && df_sink->Read(&result) == 0 && result.RowCount() > 0) {
            result_json = result.ToJson();
            row_count = result.RowCount();
        } else if (sink_type == ChannelType::kDatabase) {
            row_count = affected_rows;
        }

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("status"); w.String("completed");
        w.Key("rows"); w.Int64(row_count);
        w.Key("data"); w.RawValue(result_json.c_str(), result_json.size(), rapidjson::kArrayType);
        w.EndObject();
        rsp = buf.GetString();
        return error::OK;

    } catch (const std::exception& e) {
        std::string err = std::string("internal error: ") + e.what();
        LOG_ERROR("SchedulerPlugin::HandleExecute: exception: %s", err.c_str());
        rsp = MakeErrorJson(err);
        return error::INTERNAL_ERROR;
    } catch (...) {
        LOG_ERROR("SchedulerPlugin::HandleExecute: unknown exception");
        rsp = MakeErrorJson("internal error: unknown exception");
        return error::INTERNAL_ERROR;
    }
}

// --- HandleGetChannels ---
int32_t SchedulerPlugin::HandleGetChannels(const std::string&, const std::string&, std::string& rsp) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();

    // 内部通道表
    for (auto& [key, ch_ptr] : channels_) {
        auto* ch = ch_ptr.get();
        w.StartObject();
        w.Key("catelog"); w.String(ch->Catelog());
        w.Key("name"); w.String(ch->Name());
        w.Key("type"); w.String(ch->Type());
        w.Key("schema"); w.String(ch->Schema());
        w.EndObject();
    }

    // 静态注册的通道（通过 IQuerier）
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&w](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            w.StartObject();
            w.Key("catelog"); w.String(ch->Catelog());
            w.Key("name"); w.String(ch->Name());
            w.Key("type"); w.String(ch->Type());
            w.Key("schema"); w.String(ch->Schema());
            w.EndObject();
            return 0;
        });

        // 数据库通道（通过 IDatabaseFactory）
        auto* factory = static_cast<IDatabaseFactory*>(querier_->First(IID_DATABASE_FACTORY));
        if (factory) {
            factory->List([&w](const char* type, const char* name, const char* config_json) {
                w.StartObject();
                w.Key("catelog"); w.String(type);
                w.Key("name"); w.String(name);
                w.Key("type"); w.String(ChannelType::kDatabase);
                // 从 config_json 提取 database 字段作为 schema 展示
                std::string db_label;
                if (config_json) {
                    rapidjson::Document cfg;
                    cfg.Parse(config_json);
                    if (!cfg.HasParseError() && cfg.IsObject()) {
                        if (cfg.HasMember("database") && cfg["database"].IsString()) {
                            db_label = cfg["database"].GetString();
                        } else if (cfg.HasMember("path") && cfg["path"].IsString()) {
                            db_label = cfg["path"].GetString();
                        }
                    }
                }
                w.Key("schema"); w.String(db_label.c_str());
                w.EndObject();
            });
        }
    }

    w.EndArray();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandlePreviewDataframe ---
// POST /channels/dataframe/preview — Body: {"catelog":"...","name":"..."}
int32_t SchedulerPlugin::HandlePreviewDataframe(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("catelog") || !doc.HasMember("name")) {
        rsp = R"({"error":"missing 'catelog' or 'name'"})";
        return error::BAD_REQUEST;
    }
    std::string catelog = doc["catelog"].GetString();
    std::string name    = doc["name"].GetString();
    std::string key     = catelog + "." + name;

    // 先在内部通道表查找
    IChannel* raw_ch = nullptr;
    auto it = channels_.find(key);
    if (it != channels_.end()) {
        raw_ch = it->second.get();
    }

    // 再去 IQuerier 静态注册通道查找
    if (!raw_ch && querier_) {
        querier_->Traverse(IID_CHANNEL, [&](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            if (std::string(ch->Catelog()) == catelog && std::string(ch->Name()) == name) {
                raw_ch = ch;
                return 1;  // 找到，停止遍历
            }
            return 0;
        });
    }

    if (!raw_ch) {
        rsp = "{\"error\":\"channel not found: " + key + "\"}";
        return error::NOT_FOUND;
    }

    auto* df_ch = dynamic_cast<IDataFrameChannel*>(raw_ch);
    if (!df_ch) {
        rsp = R"({"error":"not a dataframe channel"})";
        return error::BAD_REQUEST;
    }

    DataFrame data;
    if (df_ch->Read(&data) != 0 || data.RowCount() == 0) {
        rsp = R"({"columns":[],"types":[],"data":[],"rows":0})";
        return error::OK;
    }

    rsp = data.ToJson();
    return error::OK;
}
int32_t SchedulerPlugin::HandleGetOperators(const std::string&, const std::string&, std::string& rsp) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();

    if (querier_) {
        querier_->Traverse(IID_OPERATOR, [&w](void* p) -> int {
            auto* op = static_cast<IOperator*>(p);
            w.StartObject();
            w.Key("catelog"); w.String(op->Catelog().c_str());
            w.Key("name"); w.String(op->Name().c_str());
            w.Key("description"); w.String(op->Description().c_str());
            std::string pos = (op->Position() == OperatorPosition::STORAGE) ? "STORAGE" : "DATA";
            w.Key("position"); w.String(pos.c_str());
            w.EndObject();
            return 0;
        });

        // Python 算子（通过 IBridge 遍历）
        auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
        if (bridge) {
            bridge->TraverseOperators([&w](IOperator* op) -> int {
                w.StartObject();
                w.Key("catelog"); w.String(op->Catelog().c_str());
                w.Key("name"); w.String(op->Name().c_str());
                w.Key("description"); w.String(op->Description().c_str());
                std::string pos = (op->Position() == OperatorPosition::STORAGE) ? "STORAGE" : "DATA";
                w.Key("position"); w.String(pos.c_str());
                w.EndObject();
                return 0;
            });
        }
    }

    w.EndArray();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandleRefreshOperators ---
int32_t SchedulerPlugin::HandleRefreshOperators(const std::string&, const std::string&, std::string& rsp) {
    if (!querier_) {
        rsp = R"({"error":"querier not initialized"})";
        return error::INTERNAL_ERROR;
    }
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        int rc = bridge->Refresh();
        if (rc == 0) {
            rsp = R"({"status":"refreshed"})";
            return error::OK;
        } else {
            rsp = R"({"error":"refresh failed"})";
            return error::INTERNAL_ERROR;
        }
    } else {
        rsp = R"({"error":"bridge not available"})";
        return error::NOT_FOUND;
    }
}

// ==================== 数据库通道动态管理端点（Epic 6）====================
// 已移交 DatabasePlugin 处理，此处删除

}  // namespace scheduler
}  // namespace flowsql

