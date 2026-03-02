#include "web_server.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "framework/core/plugin_registry.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ioperator.h"

namespace flowsql {
namespace web {

// 内嵌 schema SQL
static const char* kSchemaSql = R"(
CREATE TABLE IF NOT EXISTS channels (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    catelog TEXT NOT NULL,
    name TEXT NOT NULL,
    type TEXT NOT NULL DEFAULT 'dataframe',
    schema_json TEXT DEFAULT '[]',
    status TEXT NOT NULL DEFAULT 'active',
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(catelog, name)
);
CREATE TABLE IF NOT EXISTS operators (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    catelog TEXT NOT NULL,
    name TEXT NOT NULL,
    description TEXT DEFAULT '',
    position TEXT NOT NULL DEFAULT 'DATA',
    source TEXT NOT NULL DEFAULT 'builtin',
    file_path TEXT DEFAULT '',
    active INTEGER NOT NULL DEFAULT 1,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(catelog, name)
);
CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sql_text TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    result_json TEXT DEFAULT '',
    error_msg TEXT DEFAULT '',
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    finished_at DATETIME
);
)";

WebServer::WebServer() {}

void WebServer::SetWorkerAddress(const std::string& host, int port) {
    worker_host_ = host;
    worker_port_ = port;
}

void WebServer::SetSchedulerAddress(const std::string& host, int port) {
    scheduler_host_ = host;
    scheduler_port_ = port;
}

void WebServer::NotifyWorkerReload() {
    // 通过 Gateway 转发 reload 请求到 Python Worker
    httplib::Client client(worker_host_, worker_port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(5);
    auto result = client.Post("/pyworker/reload", "", "application/json");
    if (result && result->status == 200) {
        printf("WebServer: Worker reload OK\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        SyncRegistryToDb();
    } else {
        printf("WebServer: Worker reload failed (Worker may not be running)\n");
    }
}

int WebServer::Init(const std::string& db_path, PluginRegistry* registry) {
    registry_ = registry;

    if (db_.Open(db_path) != 0) {
        printf("WebServer::Init: failed to open database: %s\n", db_path.c_str());
        return -1;
    }

    if (db_.InitSchema(kSchemaSql) != 0) {
        printf("WebServer::Init: failed to init schema\n");
        return -1;
    }

    SyncRegistryToDb();
    RegisterRoutes();

    printf("WebServer::Init: OK (db=%s)\n", db_path.c_str());
    return 0;
}

int WebServer::Start(const std::string& host, int port) {
    printf("WebServer: listening on %s:%d\n", host.c_str(), port);
    if (!server_.listen(host, port)) {
        printf("WebServer: failed to start\n");
        return -1;
    }
    return 0;
}

void WebServer::Stop() {
    server_.stop();
}

void WebServer::RegisterRoutes() {
    // CORS 预检
    server_.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    // 健康检查
    server_.Get("/api/health", [this](const httplib::Request& req, httplib::Response& res) {
        HandleHealth(req, res);
    });

    // 通道
    server_.Get("/api/channels", [this](const httplib::Request& req, httplib::Response& res) {
        HandleGetChannels(req, res);
    });

    // 算子
    server_.Get("/api/operators", [this](const httplib::Request& req, httplib::Response& res) {
        HandleGetOperators(req, res);
    });
    server_.Post("/api/operators/upload", [this](const httplib::Request& req, httplib::Response& res) {
        HandleUploadOperator(req, res);
    });
    server_.Post(R"(/api/operators/([^/]+)/activate)",
        [this](const httplib::Request& req, httplib::Response& res) {
            HandleActivateOperator(req, res);
        });
    server_.Post(R"(/api/operators/([^/]+)/deactivate)",
        [this](const httplib::Request& req, httplib::Response& res) {
            HandleDeactivateOperator(req, res);
        });

    // 任务
    server_.Get("/api/tasks", [this](const httplib::Request& req, httplib::Response& res) {
        HandleGetTasks(req, res);
    });
    server_.Post("/api/tasks", [this](const httplib::Request& req, httplib::Response& res) {
        HandleCreateTask(req, res);
    });
    server_.Get(R"(/api/tasks/(\d+)/result)",
        [this](const httplib::Request& req, httplib::Response& res) {
            HandleGetTaskResult(req, res);
        });

    // 静态文件（Vue.js 构建产物）— 基于可执行文件位置定位
    std::string static_dir = "static";
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        std::string exe_dir(exe_path);
        size_t pos = exe_dir.find_last_of('/');
        if (pos != std::string::npos) {
            static_dir = exe_dir.substr(0, pos) + "/static";
        }
    }
    if (!server_.set_mount_point("/", static_dir)) {
        printf("WebServer: WARNING - static dir not found: %s\n", static_dir.c_str());
    } else {
        printf("WebServer: serving static files from %s\n", static_dir.c_str());
    }
}

// --- JSON 辅助 ---
static void SetCorsHeaders(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
}

// 构造安全的错误 JSON 响应（防止特殊字符破坏 JSON 格式）
static std::string MakeErrorJson(const std::string& error, int64_t task_id = -1) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error"); w.String(error.c_str());
    if (task_id >= 0) { w.Key("task_id"); w.Int64(task_id); }
    w.EndObject();
    return buf.GetString();
}

static std::string RowsToJson(const std::vector<Row>& rows) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for (auto& row : rows) {
        w.StartObject();
        for (auto& [k, v] : row) {
            w.Key(k.c_str());
            w.String(v.c_str());
        }
        w.EndObject();
    }
    w.EndArray();
    return buf.GetString();
}

// --- SyncRegistryToDb ---
void WebServer::SyncRegistryToDb() {
    if (!registry_) return;

    // 同步通道
    registry_->Traverse<IChannel>(IID_CHANNEL, [this](IChannel* ch) {
        db_.ExecuteParams("INSERT OR IGNORE INTO channels (catelog, name, type, schema_json) VALUES (?1, ?2, ?3, ?4)",
                          {ch->Catelog(), ch->Name(), ch->Type(), ch->Schema()});
    });

    // 同步算子
    registry_->Traverse<IOperator>(IID_OPERATOR, [this](IOperator* op) {
        std::string pos = (op->Position() == OperatorPosition::STORAGE) ? "STORAGE" : "DATA";
        db_.ExecuteParams("INSERT OR IGNORE INTO operators (catelog, name, description, position) VALUES (?1, ?2, ?3, ?4)",
                          {op->Catelog(), op->Name(), op->Description(), pos});
    });
}

// --- Health ---
void WebServer::HandleHealth(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);
    res.set_content(R"({"status":"ok"})", "application/json");
}

// --- Channels ---
void WebServer::HandleGetChannels(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);
    // 从 Scheduler 获取实时通道列表
    httplib::Client client(scheduler_host_, scheduler_port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(5);
    auto result = client.Get("/scheduler/channels");
    if (result && result->status == 200) {
        res.set_content(result->body, "application/json");
    } else {
        // 回退到本地数据库
        auto rows = db_.Query("SELECT id, catelog, name, type, schema_json, status, created_at FROM channels");
        res.set_content(RowsToJson(rows), "application/json");
    }
}

// --- Operators ---
void WebServer::HandleGetOperators(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);
    // 从 Scheduler 获取实时算子列表
    httplib::Client client(scheduler_host_, scheduler_port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(5);
    auto result = client.Get("/scheduler/operators");
    if (result && result->status == 200) {
        res.set_content(result->body, "application/json");
    } else {
        // 回退到本地数据库
        auto rows = db_.Query("SELECT id, catelog, name, description, position, source, active, created_at FROM operators");
        res.set_content(RowsToJson(rows), "application/json");
    }
}

void WebServer::HandleUploadOperator(const httplib::Request& req, httplib::Response& res) {
    SetCorsHeaders(res);
    // 从 multipart form 获取文件
    if (!req.has_file("file")) {
        res.status = 400;
        res.set_content(R"({"error":"missing file field"})", "application/json");
        return;
    }
    auto file = req.get_file_value("file");
    if (file.filename.empty() || file.content.empty()) {
        res.status = 400;
        res.set_content(R"({"error":"empty file"})", "application/json");
        return;
    }

    // 保存到 operators 目录（基于可执行文件位置推导绝对路径，问题 16）
    std::string operators_dir;
    {
        char op_exe_path[1024];
        ssize_t op_len = readlink("/proc/self/exe", op_exe_path, sizeof(op_exe_path) - 1);
        if (op_len > 0) {
            op_exe_path[op_len] = '\0';
            std::string op_exe_dir(op_exe_path);
            size_t op_pos = op_exe_dir.find_last_of('/');
            if (op_pos != std::string::npos) {
                operators_dir = op_exe_dir.substr(0, op_pos) + "/operators";
            }
        }
        if (operators_dir.empty()) {
            operators_dir = "operators";
        }
    }

    // 安全校验：文件名不能包含路径分隔符（防止路径穿越）
    if (file.filename.find('/') != std::string::npos || file.filename.find('\\') != std::string::npos ||
        file.filename.find("..") != std::string::npos) {
        res.status = 400;
        res.set_content(R"({"error":"invalid filename"})", "application/json");
        return;
    }

    // 确保目录存在（忽略已存在的错误）
    ::mkdir(operators_dir.c_str(), 0755);

    std::string filepath = operators_dir + "/" + file.filename;

    FILE* fp = fopen(filepath.c_str(), "wb");
    if (!fp) {
        res.status = 500;
        res.set_content(R"({"error":"failed to save file"})", "application/json");
        return;
    }
    fwrite(file.content.data(), 1, file.content.size(), fp);
    fclose(fp);

    // 解析文件名提取 catelog 和 name
    // 文件名格式: catelog_name.py
    std::string filename = file.filename;
    if (filename.size() > 3 && filename.substr(filename.size() - 3) == ".py") {
        filename = filename.substr(0, filename.size() - 3);  // 去掉 .py
    }

    size_t underscore_pos = filename.find('_');
    if (underscore_pos != std::string::npos) {
        std::string catelog = filename.substr(0, underscore_pos);
        std::string name = filename.substr(underscore_pos + 1);

        // 插入数据库（如果已存在则忽略）
        db_.ExecuteParams(
            "INSERT OR IGNORE INTO operators (catelog, name, description, position, source, file_path, active) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
            {catelog, name, "", "DATA", "uploaded", filepath, "0"});
    }

    rapidjson::StringBuffer ubuf;
    rapidjson::Writer<rapidjson::StringBuffer> uw(ubuf);
    uw.StartObject();
    uw.Key("status"); uw.String("uploaded");
    uw.Key("filename"); uw.String(file.filename.c_str());
    uw.EndObject();
    res.set_content(ubuf.GetString(), "application/json");
}

void WebServer::HandleActivateOperator(const httplib::Request& req, httplib::Response& res) {
    SetCorsHeaders(res);
    std::string op_key = req.matches[1];
    db_.ExecuteParams("UPDATE operators SET active=1 WHERE catelog||'.'||name=?1", {op_key});

    // 通知 Python Worker 重新加载算子
    NotifyWorkerReload();

    // 使用 rapidjson 构造响应，防止 op_key 中特殊字符破坏 JSON（问题 10）
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status"); w.String("activated");
    w.Key("operator"); w.String(op_key.c_str());
    w.EndObject();
    res.set_content(buf.GetString(), "application/json");
}

void WebServer::HandleDeactivateOperator(const httplib::Request& req, httplib::Response& res) {
    SetCorsHeaders(res);
    std::string op_key = req.matches[1];
    db_.ExecuteParams("UPDATE operators SET active=0 WHERE catelog||'.'||name=?1", {op_key});

    // TODO: 通知 Worker 移除算子（需要 Worker 支持按名称卸载）

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status"); w.String("deactivated");
    w.Key("operator"); w.String(op_key.c_str());
    w.EndObject();
    res.set_content(buf.GetString(), "application/json");
}

// --- Tasks ---
void WebServer::HandleGetTasks(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);
    auto rows = db_.Query("SELECT id, sql_text, status, error_msg, created_at, finished_at FROM tasks ORDER BY id DESC");
    res.set_content(RowsToJson(rows), "application/json");
}

void WebServer::HandleCreateTask(const httplib::Request& req, httplib::Response& res) {
    SetCorsHeaders(res);

    // 解析请求 JSON: {"sql": "SELECT * FROM ..."}
    rapidjson::Document doc;
    doc.Parse(req.body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        res.status = 400;
        res.set_content(R"({"error":"invalid request, expected {\"sql\":\"...\"}"})", "application/json");
        return;
    }
    std::string sql_text = doc["sql"].GetString();

    // 创建任务记录
    int64_t task_id = db_.InsertParams("INSERT INTO tasks (sql_text, status) VALUES (?1, 'running')", {sql_text});
    if (task_id < 0) {
        res.status = 500;
        res.set_content(R"({"error":"failed to create task"})", "application/json");
        return;
    }

    // 转发到 Scheduler 执行
    httplib::Client client(scheduler_host_, scheduler_port_);
    client.set_connection_timeout(5);
    client.set_read_timeout(30);

    // 构造转发请求体
    rapidjson::StringBuffer fwd_buf;
    rapidjson::Writer<rapidjson::StringBuffer> fwd_w(fwd_buf);
    fwd_w.StartObject();
    fwd_w.Key("sql");
    fwd_w.String(sql_text.c_str());
    fwd_w.EndObject();

    auto result = client.Post("/scheduler/execute", fwd_buf.GetString(), "application/json");

    if (!result) {
        // 网络错误
        std::string err = "scheduler unreachable";
        db_.ExecuteParams(
            "UPDATE tasks SET status='failed', error_msg=?1, finished_at=CURRENT_TIMESTAMP WHERE id=?2",
            {err, std::to_string(task_id)});
        res.status = 502;
        res.set_content(MakeErrorJson(err, task_id), "application/json");
        return;
    }

    // 解析 Scheduler 响应
    rapidjson::Document sched_doc;
    sched_doc.Parse(result->body.c_str());

    if (result->status != 200) {
        // Scheduler 返回错误
        std::string err = "scheduler error";
        if (!sched_doc.HasParseError() && sched_doc.HasMember("error") && sched_doc["error"].IsString()) {
            err = sched_doc["error"].GetString();
        }
        db_.ExecuteParams(
            "UPDATE tasks SET status='failed', error_msg=?1, finished_at=CURRENT_TIMESTAMP WHERE id=?2",
            {err, std::to_string(task_id)});
        res.status = result->status;
        res.set_content(MakeErrorJson(err, task_id), "application/json");
        return;
    }

    // Scheduler 执行成功，提取结果
    std::string result_json = "[]";
    int row_count = 0;
    if (!sched_doc.HasParseError()) {
        if (sched_doc.HasMember("data")) {
            // 将 data 字段序列化为 JSON 字符串存入数据库
            rapidjson::StringBuffer data_buf;
            rapidjson::Writer<rapidjson::StringBuffer> data_w(data_buf);
            sched_doc["data"].Accept(data_w);
            result_json = data_buf.GetString();
        }
        if (sched_doc.HasMember("rows") && sched_doc["rows"].IsInt()) {
            row_count = sched_doc["rows"].GetInt();
        }
    }

    // 更新任务状态
    db_.ExecuteParams(
        "UPDATE tasks SET status='completed', result_json=?1, finished_at=CURRENT_TIMESTAMP WHERE id=?2",
        {result_json, std::to_string(task_id)});

    // 返回结果
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_id");
    w.Int64(task_id);
    w.Key("status");
    w.String("completed");
    w.Key("rows");
    w.Int(row_count);
    w.EndObject();
    res.set_content(buf.GetString(), "application/json");
}

void WebServer::HandleGetTaskResult(const httplib::Request& req, httplib::Response& res) {
    SetCorsHeaders(res);
    std::string task_id = req.matches[1];
    auto rows = db_.QueryParams("SELECT status, result_json, error_msg FROM tasks WHERE id=?1", {task_id});
    if (rows.empty()) {
        res.status = 404;
        res.set_content(R"({"error":"task not found"})", "application/json");
        return;
    }

    auto& row = rows[0];
    std::string status, result_json, error_msg;
    for (auto& [k, v] : row) {
        if (k == "status") status = v;
        else if (k == "result_json") result_json = v;
        else if (k == "error_msg") error_msg = v;
    }

    if (status == "failed") {
        res.set_content(MakeErrorJson(error_msg), "application/json");
        return;
    }

    // 直接返回存储的 result_json
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status"); w.String(status.c_str());
    w.Key("data"); w.RawValue(result_json.empty() ? "[]" : result_json.c_str(),
                               result_json.empty() ? 2 : result_json.size(),
                               rapidjson::kArrayType);
    w.EndObject();
    res.set_content(buf.GetString(), "application/json");
}

}  // namespace web
}  // namespace flowsql
