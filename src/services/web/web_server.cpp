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

#include <common/error_code.h>

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
    httplib::Client client(worker_host_, worker_port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(5);
    auto result = client.Post("/operators/python/reload", "", "application/json");
    if (result && result->status == 200) {
        printf("WebServer: Worker reload OK\n");
    } else {
        printf("WebServer: Worker reload failed (Worker may not be running)\n");
    }
}

void WebServer::NotifySchedulerRefresh() {
    httplib::Client client(scheduler_host_, scheduler_port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(5);
    auto result = client.Post("/operators/python/refresh", "", "application/json");
    if (result && result->status == 200) {
        printf("WebServer: Scheduler refresh OK\n");
    } else {
        printf("WebServer: Scheduler refresh failed\n");
    }
}

int WebServer::Init(const std::string& db_path) {
    if (db_.Open(db_path) != 0) {
        printf("WebServer::Init: failed to open database: %s\n", db_path.c_str());
        return -1;
    }
    if (db_.InitSchema(kSchemaSql) != 0) {
        printf("WebServer::Init: failed to init schema\n");
        return -1;
    }

    // 静态文件服务（基于可执行文件位置定位 static 目录）
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

    // CORS 统一处理
    server_.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server_.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // 注册 /api/* 路由（通过 IRouterHandle 机制声明，此处直接绑定到 httplib）
    server_.Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
        std::string rsp; HandleHealth("", "", rsp);
        res.set_content(rsp, "application/json");
    });
    server_.Get("/api/channels", [this](const httplib::Request&, httplib::Response& res) {
        std::string rsp; HandleGetChannels("", "", rsp);
        res.set_content(rsp, "application/json");
    });
    server_.Get("/api/operators", [this](const httplib::Request&, httplib::Response& res) {
        std::string rsp; HandleGetOperators("", "", rsp);
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/operators/upload", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleUploadOperator("", req.body, rsp);
        res.status = (rc == 0) ? 200 : 400;
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/operators/activate", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; HandleActivateOperator("", req.body, rsp);
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/operators/deactivate", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; HandleDeactivateOperator("", req.body, rsp);
        res.set_content(rsp, "application/json");
    });
    server_.Get("/api/tasks", [this](const httplib::Request&, httplib::Response& res) {
        std::string rsp; HandleGetTasks("", "", rsp);
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleCreateTask("", req.body, rsp);
        res.status = (rc == 0) ? 200 : 400;
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/result", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; HandleGetTaskResult("", req.body, rsp);
        res.set_content(rsp, "application/json");
    });

    // 数据库通道管理路由：转发给内部服务，去掉 /api 前缀
    auto db_proxy = [this](const std::string& target_uri, const std::string& req_body, httplib::Response& res) {
        httplib::Client client(scheduler_host_, scheduler_port_);
        client.set_connection_timeout(5);
        client.set_read_timeout(10);
        auto result = client.Post(target_uri.c_str(), req_body, "application/json");
        if (!result) {
            res.status = 503;
            res.set_content(R"({"error":"service unreachable"})", "application/json");
            return;
        }
        res.status = result->status;
        res.set_content(result->body, "application/json");
    };
    server_.Post("/api/channels/database/query", [db_proxy](const httplib::Request& req, httplib::Response& res) {
        db_proxy("/channels/database/query", req.body, res);
    });
    server_.Post("/api/channels/database/add", [db_proxy](const httplib::Request& req, httplib::Response& res) {
        db_proxy("/channels/database/add", req.body, res);
    });
    server_.Post("/api/channels/database/remove", [db_proxy](const httplib::Request& req, httplib::Response& res) {
        db_proxy("/channels/database/remove", req.body, res);
    });
    server_.Post("/api/channels/database/modify", [db_proxy](const httplib::Request& req, httplib::Response& res) {
        db_proxy("/channels/database/modify", req.body, res);
    });

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

// --- 声明管理 API 路由 ---

void WebServer::EnumApiRoutes(std::function<void(const RouteItem&)> cb) {
    cb({"GET",  "/api/health",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleHealth(u, req, rsp);
        }});
    cb({"GET",  "/api/channels",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetChannels(u, req, rsp);
        }});
    cb({"GET",  "/api/operators",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetOperators(u, req, rsp);
        }});
    cb({"POST", "/api/operators/upload",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleUploadOperator(u, req, rsp);
        }});
    cb({"POST", "/api/operators/activate",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleActivateOperator(u, req, rsp);
        }});
    cb({"POST", "/api/operators/deactivate",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleDeactivateOperator(u, req, rsp);
        }});
    cb({"GET",  "/api/tasks",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetTasks(u, req, rsp);
        }});
    cb({"POST", "/api/tasks",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleCreateTask(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/result",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetTaskResult(u, req, rsp);
        }});

    // 代理 /api/channels/database/* → Gateway /channels/database/*
    auto proxy = [this](const std::string& target_uri, const std::string& req, std::string& rsp) -> int32_t {
        httplib::Client client(scheduler_host_, scheduler_port_);
        client.set_connection_timeout(5);
        client.set_read_timeout(10);
        auto result = client.Post(target_uri.c_str(), req, "application/json");
        if (!result) {
            rsp = R"({"error":"gateway unreachable"})";
            return error::UNAVAILABLE;
        }
        rsp = result->body;
        return (result->status == 200) ? error::OK : error::INTERNAL_ERROR;
    };
    cb({"POST", "/api/channels/database/add",
        [proxy](const std::string&, const std::string& req, std::string& rsp) {
            return proxy("/channels/database/add", req, rsp);
        }});
    cb({"POST", "/api/channels/database/remove",
        [proxy](const std::string&, const std::string& req, std::string& rsp) {
            return proxy("/channels/database/remove", req, rsp);
        }});
    cb({"POST", "/api/channels/database/query",
        [proxy](const std::string&, const std::string& req, std::string& rsp) {
            return proxy("/channels/database/query", req, rsp);
        }});
    cb({"POST", "/api/channels/database/modify",
        [proxy](const std::string&, const std::string& req, std::string& rsp) {
            return proxy("/channels/database/modify", req, rsp);
        }});
}

// --- JSON 辅助 ---

static std::string MakeErrorJson(const std::string& error) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error"); w.String(error.c_str());
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

// --- 管理 API handler 实现 ---

int32_t WebServer::HandleHealth(const std::string&, const std::string&, std::string& rsp) {
    rsp = R"({"status":"ok"})";
    return error::OK;
}

int32_t WebServer::HandleGetChannels(const std::string&, const std::string&, std::string& rsp) {
    // 从 Scheduler 获取实时通道列表
    httplib::Client client(scheduler_host_, scheduler_port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(5);
    auto result = client.Post("/channels/dataframe/query", "{}", "application/json");
    if (result && result->status == 200) {
        rsp = result->body;
    } else {
        // 回退到本地数据库
        auto rows = db_.Query("SELECT id, catelog, name, type, schema_json, status, created_at FROM channels");
        rsp = RowsToJson(rows);
    }
    return error::OK;
}

int32_t WebServer::HandleGetOperators(const std::string&, const std::string&, std::string& rsp) {
    httplib::Client client(scheduler_host_, scheduler_port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(5);
    auto result = client.Post("/operators/native/query", "{}", "application/json");
    if (result && result->status == 200) {
        rsp = result->body;
    } else {
        auto rows = db_.Query("SELECT id, catelog, name, description, position, source, active, created_at FROM operators");
        rsp = RowsToJson(rows);
    }
    return error::OK;
}

int32_t WebServer::HandleUploadOperator(const std::string&, const std::string& req, std::string& rsp) {
    // 通过 JSON body 传递 base64 编码的文件内容
    // Body: {"filename":"xxx.py","content":"<base64>"}
    // 注意：multipart 上传需要 httplib 直接处理，此处简化为 JSON 上传
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("filename") || !doc.HasMember("content")) {
        rsp = R"({"error":"invalid request, expected {\"filename\":\"...\",\"content\":\"...\"}"})" ;
        return error::BAD_REQUEST;
    }
    std::string filename = doc["filename"].GetString();
    std::string content  = doc["content"].GetString();

    // 安全校验
    if (filename.find('/') != std::string::npos || filename.find("..") != std::string::npos) {
        rsp = R"({"error":"invalid filename"})";
        return error::BAD_REQUEST;
    }

    std::string operators_dir;
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        std::string exe_dir(exe_path);
        size_t pos = exe_dir.find_last_of('/');
        if (pos != std::string::npos) operators_dir = exe_dir.substr(0, pos) + "/operators";
    }
    if (operators_dir.empty()) operators_dir = "operators";
    ::mkdir(operators_dir.c_str(), 0755);

    std::string filepath = operators_dir + "/" + filename;
    FILE* fp = fopen(filepath.c_str(), "wb");
    if (!fp) {
        rsp = R"({"error":"failed to save file"})";
        return error::INTERNAL_ERROR;
    }
    fwrite(content.data(), 1, content.size(), fp);
    fclose(fp);

    // 解析 catelog_name.py 格式
    std::string base = filename;
    if (base.size() > 3 && base.substr(base.size() - 3) == ".py") base = base.substr(0, base.size() - 3);
    size_t us = base.find('_');
    if (us != std::string::npos) {
        db_.ExecuteParams(
            "INSERT OR IGNORE INTO operators (catelog, name, description, position, source, file_path, active) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
            {base.substr(0, us), base.substr(us + 1), "", "DATA", "uploaded", filepath, "0"});
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status"); w.String("uploaded");
    w.Key("filename"); w.String(filename.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

// POST /api/operators/activate — Body: {"name":"catelog.opname"}
int32_t WebServer::HandleActivateOperator(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("name")) {
        rsp = R"({"error":"invalid request, expected {\"name\":\"catelog.opname\"}"})" ;
        return error::BAD_REQUEST;
    }
    std::string op_key = doc["name"].GetString();
    db_.ExecuteParams("UPDATE operators SET active=1 WHERE catelog||'.'||name=?1", {op_key});
    NotifyWorkerReload();
    NotifySchedulerRefresh();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status"); w.String("activated");
    w.Key("operator"); w.String(op_key.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

// POST /api/operators/deactivate — Body: {"name":"catelog.opname"}
int32_t WebServer::HandleDeactivateOperator(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("name")) {
        rsp = R"({"error":"invalid request, expected {\"name\":\"catelog.opname\"}"})" ;
        return error::BAD_REQUEST;
    }
    std::string op_key = doc["name"].GetString();
    db_.ExecuteParams("UPDATE operators SET active=0 WHERE catelog||'.'||name=?1", {op_key});
    NotifyWorkerReload();
    NotifySchedulerRefresh();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status"); w.String("deactivated");
    w.Key("operator"); w.String(op_key.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t WebServer::HandleGetTasks(const std::string&, const std::string&, std::string& rsp) {
    auto rows = db_.Query("SELECT id, sql_text, status, error_msg, created_at, finished_at FROM tasks ORDER BY id DESC");
    rsp = RowsToJson(rows);
    return error::OK;
}

int32_t WebServer::HandleCreateTask(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = R"({"error":"invalid request, expected {\"sql\":\"...\"}"})" ;
        return error::BAD_REQUEST;
    }
    std::string sql_text = doc["sql"].GetString();

    int64_t task_id = db_.InsertParams("INSERT INTO tasks (sql_text, status) VALUES (?1, 'running')", {sql_text});
    if (task_id < 0) {
        rsp = R"({"error":"failed to create task"})";
        return error::INTERNAL_ERROR;
    }

    // 转发到 Scheduler 执行（通过 RouterAgencyPlugin 分发的新 URI）
    httplib::Client client(scheduler_host_, scheduler_port_);
    client.set_connection_timeout(5);
    client.set_read_timeout(30);

    rapidjson::StringBuffer fwd_buf;
    rapidjson::Writer<rapidjson::StringBuffer> fwd_w(fwd_buf);
    fwd_w.StartObject();
    fwd_w.Key("sql"); fwd_w.String(sql_text.c_str());
    fwd_w.EndObject();

    auto result = client.Post("/tasks/instant/execute", fwd_buf.GetString(), "application/json");

    if (!result) {
        std::string err = "scheduler unreachable";
        db_.ExecuteParams(
            "UPDATE tasks SET status='failed', error_msg=?1, finished_at=CURRENT_TIMESTAMP WHERE id=?2",
            {err, std::to_string(task_id)});
        rsp = MakeErrorJson(err);
        return error::UNAVAILABLE;
    }

    rapidjson::Document sched_doc;
    sched_doc.Parse(result->body.c_str());

    if (result->status != 200) {
        std::string err = "scheduler error";
        if (!sched_doc.HasParseError() && sched_doc.HasMember("error") && sched_doc["error"].IsString()) {
            err = sched_doc["error"].GetString();
        }
        db_.ExecuteParams(
            "UPDATE tasks SET status='failed', error_msg=?1, finished_at=CURRENT_TIMESTAMP WHERE id=?2",
            {err, std::to_string(task_id)});
        rsp = MakeErrorJson(err);
        return error::INTERNAL_ERROR;
    }

    std::string result_json = "[]";
    int row_count = 0;
    if (!sched_doc.HasParseError()) {
        if (sched_doc.HasMember("data")) {
            rapidjson::StringBuffer data_buf;
            rapidjson::Writer<rapidjson::StringBuffer> data_w(data_buf);
            sched_doc["data"].Accept(data_w);
            result_json = data_buf.GetString();
        }
        if (sched_doc.HasMember("rows") && sched_doc["rows"].IsInt()) {
            row_count = sched_doc["rows"].GetInt();
        }
    }

    db_.ExecuteParams(
        "UPDATE tasks SET status='completed', result_json=?1, finished_at=CURRENT_TIMESTAMP WHERE id=?2",
        {result_json, std::to_string(task_id)});

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_id"); w.Int64(task_id);
    w.Key("status"); w.String("completed");
    w.Key("rows"); w.Int(row_count);
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

// POST /api/tasks/result — Body: {"task_id":123}
int32_t WebServer::HandleGetTaskResult(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id")) {
        rsp = R"({"error":"invalid request, expected {\"task_id\":123}"})" ;
        return error::BAD_REQUEST;
    }
    std::string task_id = std::to_string(doc["task_id"].GetInt64());

    auto rows = db_.QueryParams("SELECT status, result_json, error_msg FROM tasks WHERE id=?1", {task_id});
    if (rows.empty()) {
        rsp = R"({"error":"task not found"})";
        return error::NOT_FOUND;
    }

    auto& row = rows[0];
    std::string status, result_json, error_msg;
    for (auto& [k, v] : row) {
        if (k == "status") status = v;
        else if (k == "result_json") result_json = v;
        else if (k == "error_msg") error_msg = v;
    }

    if (status == "failed") {
        rsp = MakeErrorJson(error_msg);
        return error::INTERNAL_ERROR;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status"); w.String(status.c_str());
    w.Key("data"); w.RawValue(result_json.empty() ? "[]" : result_json.c_str(),
                               result_json.empty() ? 2 : result_json.size(),
                               rapidjson::kArrayType);
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

}  // namespace web
}  // namespace flowsql
