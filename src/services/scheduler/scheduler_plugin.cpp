#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/pipeline.h"
#include "framework/core/plugin_registry.h"
#include "framework/core/sql_parser.h"
#include "framework/interfaces/ichannel.h"
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

int SchedulerPlugin::Load() {
    registry_ = PluginRegistry::Instance();
    if (!registry_) {
        printf("SchedulerPlugin::Load: PluginRegistry not available\n");
        return -1;
    }
    printf("SchedulerPlugin::Load: host=%s, port=%d\n", host_.c_str(), port_);
    return 0;
}

int SchedulerPlugin::Unload() {
    return 0;
}

// --- IPlugin::Start ---
int SchedulerPlugin::Start() {
    // 创建预填测试数据（通道注册在 Scheduler 进程中）
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
    registry_->Register(IID_CHANNEL, "test.data", ch);
    registry_->Register(IID_DATAFRAME_CHANNEL, "test.data", ch);

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
    printf("SchedulerPlugin::Stop: done\n");
    return 0;
}

// --- 路由注册 ---
void SchedulerPlugin::RegisterRoutes() {
    // CORS 预检
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
}

// --- HandleExecute: 核心 SQL 执行逻辑（从 web_server.cpp HandleCreateTask 提取） ---
void SchedulerPlugin::HandleExecute(const httplib::Request& req, httplib::Response& res) {
    SetCorsHeaders(res);

    // 解析请求 JSON: {"sql": "SELECT * FROM ..."}
    rapidjson::Document doc;
    doc.Parse(req.body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        res.status = 400;
        res.set_content(MakeErrorJson("invalid request, expected {\"sql\":\"...\"}"), "application/json");
        return;
    }
    std::string sql_text = doc["sql"].GetString();

    // 解析 SQL
    SqlParser parser;
    auto stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        res.status = 400;
        res.set_content(MakeErrorJson(stmt.error), "application/json");
        return;
    }

    // 查找 source channel
    std::string source_cat = stmt.source.substr(0, stmt.source.find('.'));
    std::string source_name = stmt.source.find('.') != std::string::npos
                                  ? stmt.source.substr(stmt.source.find('.') + 1)
                                  : stmt.source;
    IChannel* source = registry_->Get<IChannel>(IID_CHANNEL, source_cat + "." + source_name);
    if (!source) {
        registry_->Traverse<IChannel>(IID_CHANNEL, [&](IChannel* ch) {
            if (std::string(ch->Name()) == stmt.source ||
                (std::string(ch->Catelog()) + "." + ch->Name()) == stmt.source) {
                source = ch;
            }
        });
    }
    if (!source) {
        res.status = 400;
        res.set_content(MakeErrorJson("source channel not found: " + stmt.source), "application/json");
        return;
    }

    // 查找 operator
    IOperator* op = registry_->Get<IOperator>(IID_OPERATOR, stmt.op_catelog + "." + stmt.op_name);
    if (!op) {
        registry_->Traverse<IOperator>(IID_OPERATOR, [&](IOperator* o) {
            if (o->Catelog() == stmt.op_catelog && o->Name() == stmt.op_name) op = o;
        });
    }
    if (!op) {
        res.status = 400;
        res.set_content(MakeErrorJson("operator not found: " + stmt.op_catelog + "." + stmt.op_name),
                        "application/json");
        return;
    }

    try {
        // 传递 WITH 参数
        for (auto& [k, v] : stmt.with_params) {
            op->Configure(k.c_str(), v.c_str());
        }

        // 准备 sink channel
        std::shared_ptr<DataFrameChannel> temp_sink;
        IChannel* sink = nullptr;

        if (!stmt.dest.empty()) {
            // INTO 指定了目标通道，查找或创建
            registry_->Traverse<IChannel>(IID_CHANNEL, [&](IChannel* ch) {
                if (std::string(ch->Name()) == stmt.dest ||
                    (std::string(ch->Catelog()) + "." + ch->Name()) == stmt.dest) {
                    sink = ch;
                }
            });
            if (!sink) {
                temp_sink = std::make_shared<DataFrameChannel>("result", stmt.dest);
                temp_sink->Open();
                registry_->Register(IID_CHANNEL, "result." + stmt.dest, temp_sink);
                registry_->Register(IID_DATAFRAME_CHANNEL, "result." + stmt.dest, temp_sink);
                sink = temp_sink.get();
            }
        } else {
            // 无 INTO，创建临时 sink
            temp_sink = std::make_shared<DataFrameChannel>("_temp", "sink");
            temp_sink->Open();
            sink = temp_sink.get();
        }

        // 执行 Pipeline
        auto pipeline = PipelineBuilder().SetSource(source).SetOperator(op).SetSink(sink).Build();
        pipeline->Run();

        if (pipeline->State() == PipelineState::FAILED) {
            std::string err = op->LastError();
            if (err.empty()) err = pipeline->ErrorMessage();
            if (err.empty()) err = "pipeline execution failed";
            res.status = 500;
            res.set_content(MakeErrorJson(err), "application/json");
            return;
        }

        // 从 sink 读取结果
        auto* df_sink = dynamic_cast<IDataFrameChannel*>(sink);
        DataFrame result;
        std::string result_json = "[]";
        if (df_sink && df_sink->Read(&result) == 0 && result.RowCount() > 0) {
            result_json = result.ToJson();
        }

        // 返回执行结果
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("status");
        w.String("completed");
        w.Key("rows");
        w.Int(result.RowCount());
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

// --- HandleGetChannels: 返回已注册通道列表 ---
void SchedulerPlugin::HandleGetChannels(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    registry_->Traverse<IChannel>(IID_CHANNEL, [&w](IChannel* ch) {
        w.StartObject();
        w.Key("catelog");
        w.String(ch->Catelog());
        w.Key("name");
        w.String(ch->Name());
        w.Key("type");
        w.String(ch->Type());
        w.Key("schema");
        w.String(ch->Schema());
        w.EndObject();
    });
    w.EndArray();
    res.set_content(buf.GetString(), "application/json");
}

// --- HandleGetOperators: 返回已注册算子列表 ---
void SchedulerPlugin::HandleGetOperators(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    registry_->Traverse<IOperator>(IID_OPERATOR, [&w](IOperator* op) {
        w.StartObject();
        w.Key("catelog");
        w.String(op->Catelog().c_str());
        w.Key("name");
        w.String(op->Name().c_str());
        w.Key("description");
        w.String(op->Description().c_str());
        std::string pos = (op->Position() == OperatorPosition::STORAGE) ? "STORAGE" : "DATA";
        w.Key("position");
        w.String(pos.c_str());
        w.EndObject();
    });
    w.EndArray();
    res.set_content(buf.GetString(), "application/json");
}

}  // namespace scheduler
}  // namespace flowsql
