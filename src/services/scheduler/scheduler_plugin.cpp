#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
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
static void SetCorsHeaders(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
}

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
    printf("SchedulerPlugin::Load: host=%s, port=%d\n", host_.c_str(), port_);
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
    // 创建预填测试数据
    auto ch = std::make_shared<DataFrameChannel>("test", "data");
    ch->Open();

    DataFrame df;
    df.SetSchema({
        {"src_ip", DataType::STRING, 0, "源IP"},
        {"dst_ip", DataType::STRING, 0, "目的IP"},
        {"src_port", DataType::UINT32, 0, "源端口"},
        {"dst_port", DataType::UINT32, 0, "目的端口"},
        {"protocol", DataType::STRING, 0, "协议"},
        {"bytes_sent", DataType::UINT64, 0, "发送字节"},
        {"bytes_recv", DataType::UINT64, 0, "接收字节"},
    });

    df.AppendRow({std::string("192.168.1.10"), std::string("10.0.0.1"), uint32_t(52341), uint32_t(80),
                  std::string("HTTP"), uint64_t(1024), uint64_t(4096)});
    df.AppendRow({std::string("192.168.1.10"), std::string("8.8.8.8"), uint32_t(53421), uint32_t(53),
                  std::string("DNS"), uint64_t(64), uint64_t(128)});
    df.AppendRow({std::string("192.168.1.20"), std::string("172.16.0.5"), uint32_t(44312), uint32_t(443),
                  std::string("HTTPS"), uint64_t(2048), uint64_t(8192)});

    ch->Write(&df);
    RegisterChannel("test.data", ch);

    RegisterRoutes();

    server_thread_ = std::thread([this]() {
        printf("SchedulerPlugin: listening on %s:%d\n", host_.c_str(), port_);
        if (!server_.listen(host_, port_)) {
            printf("SchedulerPlugin: failed to start HTTP server\n");
        }
    });

    return 0;
}

int SchedulerPlugin::Stop() {
    server_.stop();
    if (server_thread_.joinable()) server_thread_.join();
    channels_.clear();
    printf("SchedulerPlugin::Stop: done\n");
    return 0;
}

// --- 路由注册 ---
void SchedulerPlugin::RegisterRoutes() {
    server_.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    server_.Post("/execute", [this](const httplib::Request& req, httplib::Response& res) {
        HandleExecute(req, res);
    });

    server_.Get("/channels", [this](const httplib::Request& req, httplib::Response& res) {
        HandleGetChannels(req, res);
    });

    server_.Get("/operators", [this](const httplib::Request& req, httplib::Response& res) {
        HandleGetOperators(req, res);
    });

    server_.Post("/refresh-operators", [this](const httplib::Request&, httplib::Response& res) {
        HandleRefreshOperators(res);
    });
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
// Normalize table names in sql_part and replace FROM clause table
// Depends on: NormalizeFromTableName, ExtractTableName

// Normalize table names in SQL FROM clauses
// Supports three-part (catalog.database.table), two-part (database.table), one-part (table)
// Example: FROM sqlite.mydb.users -> FROM users
static std::string NormalizeFromTableName(const std::string& sql) {
    std::string result = sql;

    // Match table name after FROM keyword (supports multi-part names)
    std::regex FROM_PATTERN(R"((\bFROM\s+)((?:[\w]+\.)*)([\w]+))");

    // Replace with: FROM + last segment (table name only)
    result = std::regex_replace(result, FROM_PATTERN, "$1$3");

    return result;
}

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
static std::string BuildQuery(const std::string& source_name, const SqlStatement& stmt) {
    // Database channel: use sql_part + table name replacement
    std::string sql = stmt.sql_part;

    // 1. Normalize all table names (including subqueries)
    sql = NormalizeFromTableName(sql);

    // 2. Replace main FROM clause source with actual table name
    std::string table = ExtractTableName(source_name);

    // Use regex to replace table name after FROM (supports subqueries)
    std::regex FROM_PATTERN(R"((\bFROM\s+)[\w\.]+)");
    sql = std::regex_replace(sql, FROM_PATTERN, "$1" + table);

    return sql;
}

// --- 辅助：对 DataFrame 通道应用 WHERE 过滤 ---
// 读取数据，过滤后写回临时通道，返回过滤后的通道
static std::shared_ptr<DataFrameChannel> ApplyDataFrameFilter(
    IDataFrameChannel* src, const std::string& where_clause) {
    DataFrame data;
    if (src->Read(&data) != 0 || data.RowCount() == 0) return nullptr;

    if (data.Filter(where_clause.c_str()) != 0) return nullptr;

    auto filtered = std::make_shared<DataFrameChannel>("_filter", "tmp");
    filtered->Open();
    filtered->Write(&data);
    return filtered;
}

// --- 无算子：纯数据搬运 ---
int SchedulerPlugin::ExecuteTransfer(IChannel* source, IChannel* sink,
                                      const std::string& source_type,
                                      const std::string& sink_type,
                                      const SqlStatement& stmt, int64_t* rows_affected) {
    if (source_type == "dataframe" && sink_type == "dataframe") {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;

        // DataFrame + WHERE → 先过滤再复制
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause);
            if (!filtered) return -1;
            return ChannelAdapter::CopyDataFrame(filtered.get(), dst);
        }
        return ChannelAdapter::CopyDataFrame(src, dst);
    }

    if (source_type == "dataframe" && sink_type == "database") {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;
        std::string table = ExtractTableName(stmt.dest);

        // DataFrame + WHERE → 先过滤再写入
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause);
            if (!filtered) return -1;
            int64_t rows = ChannelAdapter::WriteFromDataFrame(filtered.get(), dst, table.c_str());
            if (rows_affected) *rows_affected = rows;
            return (rows < 0) ? -1 : 0;
        }
        int64_t rows = ChannelAdapter::WriteFromDataFrame(src, dst, table.c_str());
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (source_type == "database" && sink_type == "dataframe") {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;
        std::string query = BuildQuery(stmt.source, stmt);
        return ChannelAdapter::ReadToDataFrame(src, query.c_str(), dst);
    }

    if (source_type == "database" && sink_type == "database") {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;

        auto tmp = std::make_shared<DataFrameChannel>("_adapter", "tmp");
        tmp->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(src, query.c_str(), tmp.get());
        if (rc != 0) return rc;

        std::string table = ExtractTableName(stmt.dest);
        int64_t rows = ChannelAdapter::WriteFromDataFrame(tmp.get(), dst, table.c_str());
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    printf("SchedulerPlugin: unsupported transfer: %s → %s\n",
           source_type.c_str(), sink_type.c_str());
    return -1;
}

// --- 有算子：自动适配通道类型 ---
int SchedulerPlugin::ExecuteWithOperator(IChannel* source, IChannel* sink,
                                          IOperator* op,
                                          const std::string& source_type,
                                          const std::string& sink_type,
                                          const SqlStatement& stmt, int64_t* rows_affected) {
    IChannel* actual_source = source;
    IChannel* actual_sink = sink;
    std::shared_ptr<DataFrameChannel> tmp_in, tmp_out;

    if (source_type == "database") {
        auto* db_src = dynamic_cast<IDatabaseChannel*>(source);
        if (!db_src) return -1;

        tmp_in = std::make_shared<DataFrameChannel>("_adapter", "in");
        tmp_in->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(db_src, query.c_str(), tmp_in.get());
        if (rc != 0) return rc;

        actual_source = tmp_in.get();
    } else if (source_type == "dataframe" && !stmt.where_clause.empty()) {
        // DataFrame + WHERE → 先过滤
        auto* df_src = dynamic_cast<IDataFrameChannel*>(source);
        if (!df_src) return -1;

        tmp_in = ApplyDataFrameFilter(df_src, stmt.where_clause);
        if (!tmp_in) return -1;
        actual_source = tmp_in.get();
    }

    if (sink_type == "database") {
        tmp_out = std::make_shared<DataFrameChannel>("_adapter", "out");
        tmp_out->Open();
        actual_sink = tmp_out.get();
    }

    auto pipeline = PipelineBuilder()
                        .SetSource(actual_source)
                        .SetOperator(op)
                        .SetSink(actual_sink)
                        .Build();
    pipeline->Run();

    if (pipeline->State() == PipelineState::FAILED) {
        return -1;
    }

    if (sink_type == "database" && tmp_out) {
        auto* db_sink = dynamic_cast<IDatabaseChannel*>(sink);
        if (!db_sink) return -1;

        std::string table = ExtractTableName(stmt.dest);
        int64_t written_rows = ChannelAdapter::WriteFromDataFrame(tmp_out.get(), db_sink, table.c_str());
        if (written_rows < 0) return -1;

        printf("SchedulerPlugin::ExecuteTransfer: wrote %ld rows to database\n", written_rows);

        // 返回写入的行数
        if (rows_affected) {
            *rows_affected = written_rows;
        }
    }

    return 0;
}

// --- HandleExecute ---
void SchedulerPlugin::HandleExecute(const httplib::Request& req, httplib::Response& res) {
    SetCorsHeaders(res);

    rapidjson::Document doc;
    doc.Parse(req.body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        res.status = 400;
        res.set_content(MakeErrorJson("invalid request, expected {\"sql\":\"...\"}"), "application/json");
        return;
    }
    std::string sql_text = doc["sql"].GetString();

    SqlParser parser;
    auto stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        res.status = 400;
        res.set_content(MakeErrorJson(stmt.error), "application/json");
        return;
    }

    IChannel* source = FindChannel(stmt.source);
    if (!source) {
        res.status = 400;
        res.set_content(MakeErrorJson("source channel not found: " + stmt.source), "application/json");
        return;
    }

    IOperator* op = nullptr;
    std::shared_ptr<IOperator> op_holder;  // 持有 shared_ptr 保证算子生命周期
    if (stmt.HasOperator()) {
        op_holder = FindOperator(stmt.op_catelog, stmt.op_name);
        if (!op_holder) {
            res.status = 400;
            res.set_content(MakeErrorJson("operator not found: " + stmt.op_catelog + "." + stmt.op_name),
                            "application/json");
            return;
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
                // 临时通道仅由局部 shared_ptr 持有，不注册到 channels_ 避免累积
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

        if (!op) {
            rc = ExecuteTransfer(source, sink, source_type, sink_type, stmt, &affected_rows);
        } else {
            rc = ExecuteWithOperator(source, sink, op, source_type, sink_type, stmt, &affected_rows);
        }

        if (rc != 0) {
            std::string err = op ? op->LastError() : "";
            if (err.empty()) err = "execution failed";
            res.status = 500;
            res.set_content(MakeErrorJson(err), "application/json");
            return;
        }

        auto* df_sink = dynamic_cast<IDataFrameChannel*>(sink);
        DataFrame result;
        std::string result_json = "[]";
        int row_count = 0;
        if (df_sink && df_sink->Read(&result) == 0 && result.RowCount() > 0) {
            result_json = result.ToJson();
            row_count = result.RowCount();
        } else if (sink_type == "database") {
            // 写入数据库时，使用 ExecuteTransfer 返回的行数
            row_count = affected_rows;
        }

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("status");
        w.String("completed");
        w.Key("rows");
        w.Int(row_count);
        w.Key("data");
        w.RawValue(result_json.c_str(), result_json.size(), rapidjson::kArrayType);
        w.EndObject();
        res.set_content(buf.GetString(), "application/json");

    } catch (const std::exception& e) {
        std::string err = std::string("internal error: ") + e.what();
        printf("SchedulerPlugin::HandleExecute: exception: %s\n", err.c_str());
        res.status = 500;
        res.set_content(MakeErrorJson(err), "application/json");
    } catch (...) {
        printf("SchedulerPlugin::HandleExecute: unknown exception\n");
        res.status = 500;
        res.set_content(MakeErrorJson("internal error: unknown exception"), "application/json");
    }
}

// --- HandleGetChannels ---
void SchedulerPlugin::HandleGetChannels(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);

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
            factory->List([&w](const char* type, const char* name) {
                w.StartObject();
                w.Key("catelog"); w.String(type);
                w.Key("name"); w.String(name);
                w.Key("type"); w.String("database");
                w.Key("schema"); w.String("[]");
                w.EndObject();
            });
        }
    }

    w.EndArray();
    res.set_content(buf.GetString(), "application/json");
}

// --- HandleGetOperators ---
void SchedulerPlugin::HandleGetOperators(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);

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
    res.set_content(buf.GetString(), "application/json");
}

// --- HandleRefreshOperators ---
void SchedulerPlugin::HandleRefreshOperators(httplib::Response& res) {
    SetCorsHeaders(res);
    if (!querier_) {
        res.status = 500;
        res.set_content(R"({"error":"querier not initialized"})", "application/json");
        return;
    }
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        int rc = bridge->Refresh();
        if (rc == 0) {
            res.set_content(R"({"status":"refreshed"})", "application/json");
        } else {
            res.status = 500;
            res.set_content(R"({"error":"refresh failed"})", "application/json");
        }
    } else {
        res.status = 404;
        res.set_content(R"({"error":"bridge not available"})", "application/json");
    }
}

}  // namespace scheduler
}  // namespace flowsql

