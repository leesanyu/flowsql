#include "web_server.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <common/error_code.h>
#include <common/log.h>

namespace flowsql {
namespace web {

namespace {

int32_t ProxyPostJson(const std::string& host, int port, const std::string& path, const std::string& req, std::string* rsp) {
    if (!rsp) return error::INTERNAL_ERROR;
    httplib::Client client(host, port);
    client.set_connection_timeout(5);
    client.set_read_timeout(10);
    auto result = client.Post(path.c_str(), req, "application/json");
    if (!result) {
        *rsp = R"({"error":"service unreachable"})";
        return error::UNAVAILABLE;
    }
    *rsp = result->body;
    if (result->status == 200) return error::OK;
    if (result->status == 400) return error::BAD_REQUEST;
    if (result->status == 404) return error::NOT_FOUND;
    if (result->status == 409) return error::CONFLICT;
    if (result->status == 503) return error::UNAVAILABLE;
    return error::INTERNAL_ERROR;
}

int32_t ProxyGetJson(const std::string& host, int port, const std::string& path, std::string* rsp) {
    if (!rsp) return error::INTERNAL_ERROR;
    httplib::Client client(host, port);
    client.set_connection_timeout(5);
    client.set_read_timeout(10);
    auto result = client.Get(path.c_str());
    if (!result) {
        *rsp = R"({"error":"service unreachable"})";
        return error::UNAVAILABLE;
    }
    *rsp = result->body;
    if (result->status == 200) return error::OK;
    if (result->status == 400) return error::BAD_REQUEST;
    if (result->status == 404) return error::NOT_FOUND;
    if (result->status == 409) return error::CONFLICT;
    if (result->status == 503) return error::UNAVAILABLE;
    return error::INTERNAL_ERROR;
}

}  // namespace

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
        LOG_INFO("WebServer: Worker reload OK");
    } else {
        LOG_WARN("WebServer: Worker reload failed (Worker may not be running)");
    }
}

void WebServer::NotifySchedulerRefresh() {
    httplib::Client client(scheduler_host_, scheduler_port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(5);
    auto result = client.Post("/operators/python/refresh", "", "application/json");
    if (result && result->status == 200) {
        LOG_INFO("WebServer: Scheduler refresh OK");
    } else {
        LOG_WARN("WebServer: Scheduler refresh failed");
    }
}

int WebServer::Init(const std::string& db_path) {
    if (db_.Open(db_path) != 0) {
        LOG_ERROR("WebServer::Init: failed to open database: %s", db_path.c_str());
        return -1;
    }
    if (db_.InitSchema(kSchemaSql) != 0) {
        LOG_ERROR("WebServer::Init: failed to init schema");
        return -1;
    }
    std::error_code ec;
    std::filesystem::create_directories(upload_dir_, ec);

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
        LOG_WARN("WebServer: static dir not found: %s", static_dir.c_str());
    } else {
        LOG_INFO("WebServer: serving static files from %s", static_dir.c_str());
    }
    const std::string index_html = static_dir + "/index.html";

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

    // SPA 历史路由回退：
    // 非 /api 的 GET（且不是静态资源文件）返回 index.html，支持直接访问 /channels、/tasks 等路径。
    server_.set_error_handler([index_html](const httplib::Request& req, httplib::Response& res) {
        if (res.status != 404) return;
        if (req.method != "GET") return;
        if (req.path == "/api" || req.path.rfind("/api/", 0) == 0) return;
        if (req.path.find('.') != std::string::npos) return;  // 静态资源 404 保持原样

        std::ifstream ifs(index_html, std::ios::binary);
        if (!ifs.good()) return;
        std::string html((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
        res.status = 200;
        res.set_content(html, "text/html; charset=utf-8");
    });

    // 注册 /api/* 路由（通过 IRouterHandle 机制声明，此处直接绑定到 httplib）
    server_.Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
        std::string rsp; HandleHealth("", "", rsp);
        res.set_content(rsp, "application/json");
    });
    // list 风格新路由（保留旧路由兼容）
    server_.Get("/api/channels/list", [this](const httplib::Request&, httplib::Response& res) {
        std::string rsp; HandleGetChannels("", "", rsp);
        res.set_content(rsp, "application/json");
    });
    server_.Get("/api/channels", [this](const httplib::Request&, httplib::Response& res) {
        std::string rsp; HandleGetChannels("", "", rsp);
        res.set_content(rsp, "application/json");
    });
    server_.Get("/api/operators/list", [this](const httplib::Request&, httplib::Response& res) {
        std::string rsp; HandleGetOperators("", "", rsp);
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
    server_.Post("/api/operators/detail", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleGetOperatorDetail("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : 400;
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/operators/update", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleUpdateOperator("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : 400;
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/submit", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleCreateTask("", req.body, rsp);
        res.status = (rc == 0) ? 200 : 400;
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/list", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; HandleGetTasks("", req.body, rsp);
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/result", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; HandleGetTaskResult("", req.body, rsp);
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/delete", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleDeleteTask("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::CONFLICT ? 409 : (rc == error::NOT_FOUND ? 404 : 400));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/cancel", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleCancelTask("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::CONFLICT ? 409 : (rc == error::NOT_FOUND ? 404 : 400));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/diagnostics", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleTaskDiagnostics("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::NOT_FOUND ? 404 : 400);
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
    server_.Post("/api/channels/database/tables", [db_proxy](const httplib::Request& req, httplib::Response& res) {
        db_proxy("/channels/database/tables", req.body, res);
    });
    server_.Post("/api/channels/database/describe", [db_proxy](const httplib::Request& req, httplib::Response& res) {
        db_proxy("/channels/database/describe", req.body, res);
    });
    server_.Post("/api/channels/database/preview", [db_proxy](const httplib::Request& req, httplib::Response& res) {
        db_proxy("/channels/database/preview", req.body, res);
    });
    // dataframe 管理（转发给 CatalogPlugin，经由 Gateway）
    server_.Get("/api/channels/dataframe", [this](const httplib::Request&, httplib::Response& res) {
        httplib::Client client(scheduler_host_, scheduler_port_);
        client.set_connection_timeout(5);
        client.set_read_timeout(10);
        auto result = client.Get("/channels/dataframe");
        if (!result) { res.status = 503; res.set_content(R"({"error":"service unreachable"})", "application/json"); return; }
        res.status = result->status;
        res.set_content(result->body, "application/json");
    });
    server_.Post("/api/channels/dataframe/import", [this](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_file("file")) {
            res.status = 400;
            res.set_content(R"({"error":"missing multipart field 'file'"})", "application/json");
            return;
        }
        const auto file = req.get_file_value("file");
        if (file.content.size() > 10 * 1024 * 1024) {
            res.status = 400;
            res.set_content("{\"error\":\"file too large (max 10MB)\"}", "application/json");
            return;
        }

        std::filesystem::path tmp_path = std::filesystem::path(upload_dir_) /
                                         (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".csv");
        FILE* fp = fopen(tmp_path.string().c_str(), "wb");
        if (!fp) {
            res.status = 500;
            res.set_content(R"({"error":"failed to persist upload temp file"})", "application/json");
            return;
        }
        fwrite(file.content.data(), 1, file.content.size(), fp);
        fclose(fp);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("filename"); w.String(file.filename.c_str());
        w.Key("tmp_path"); w.String(tmp_path.string().c_str());
        w.EndObject();

        httplib::Client client(scheduler_host_, scheduler_port_);
        client.set_connection_timeout(5);
        client.set_read_timeout(30);
        auto result = client.Post("/channels/dataframe/import", buf.GetString(), "application/json");
        if (!result) {
            std::filesystem::remove(tmp_path);
            res.status = 503;
            res.set_content(R"({"error":"service unreachable"})", "application/json");
            return;
        }
        if (result->status != 200) {
            std::filesystem::remove(tmp_path);
        }
        res.status = result->status;
        res.set_content(result->body, "application/json");
    });
    server_.Post("/api/channels/dataframe/preview", [this](const httplib::Request& req, httplib::Response& res) {
        httplib::Client client(scheduler_host_, scheduler_port_);
        client.set_connection_timeout(5);
        client.set_read_timeout(10);
        auto result = client.Post("/channels/dataframe/preview", req.body, "application/json");
        if (!result) { res.status = 503; res.set_content(R"({"error":"service unreachable"})", "application/json"); return; }
        res.status = result->status;
        res.set_content(result->body, "application/json");
    });
    server_.Post("/api/channels/dataframe/rename", [this](const httplib::Request& req, httplib::Response& res) {
        httplib::Client client(scheduler_host_, scheduler_port_);
        client.set_connection_timeout(5);
        client.set_read_timeout(10);
        auto result = client.Post("/channels/dataframe/rename", req.body, "application/json");
        if (!result) { res.status = 503; res.set_content(R"({"error":"service unreachable"})", "application/json"); return; }
        res.status = result->status;
        res.set_content(result->body, "application/json");
    });
    server_.Post("/api/channels/dataframe/delete", [this](const httplib::Request& req, httplib::Response& res) {
        httplib::Client client(scheduler_host_, scheduler_port_);
        client.set_connection_timeout(5);
        client.set_read_timeout(10);
        auto result = client.Post("/channels/dataframe/delete", req.body, "application/json");
        if (!result) { res.status = 503; res.set_content(R"({"error":"service unreachable"})", "application/json"); return; }
        res.status = result->status;
        res.set_content(result->body, "application/json");
    });

    LOG_INFO("WebServer::Init: OK (db=%s)", db_path.c_str());
    return 0;
}

int WebServer::Start(const std::string& host, int port) {
    LOG_INFO("WebServer: listening on %s:%d", host.c_str(), port);
    if (!server_.listen(host, port)) {
        LOG_ERROR("WebServer: failed to start");
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
    cb({"GET",  "/api/channels/list",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetChannels(u, req, rsp);
        }});
    cb({"GET",  "/api/channels",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetChannels(u, req, rsp);
        }});
    cb({"GET",  "/api/operators/list",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetOperators(u, req, rsp);
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
    cb({"POST", "/api/operators/detail",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetOperatorDetail(u, req, rsp);
        }});
    cb({"POST", "/api/operators/update",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleUpdateOperator(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/submit",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleCreateTask(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/list",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetTasks(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/result",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetTaskResult(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/delete",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleDeleteTask(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/cancel",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleCancelTask(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/diagnostics",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleTaskDiagnostics(u, req, rsp);
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
    cb({"POST", "/api/channels/database/tables",
        [proxy](const std::string&, const std::string& req, std::string& rsp) {
            return proxy("/channels/database/tables", req, rsp);
        }});
    cb({"POST", "/api/channels/database/describe",
        [proxy](const std::string&, const std::string& req, std::string& rsp) {
            return proxy("/channels/database/describe", req, rsp);
        }});
    cb({"POST", "/api/channels/database/preview",
        [proxy](const std::string&, const std::string& req, std::string& rsp) {
            return proxy("/channels/database/preview", req, rsp);
        }});
    cb({"POST", "/api/channels/dataframe/preview",
        [this](const std::string&, const std::string& req, std::string& rsp) -> int32_t {
            httplib::Client client(scheduler_host_, scheduler_port_);
            client.set_connection_timeout(5);
            client.set_read_timeout(10);
            auto result = client.Post("/channels/dataframe/preview", req, "application/json");
            if (!result) { rsp = R"({"error":"service unreachable"})"; return error::UNAVAILABLE; }
            rsp = result->body;
            if (result->status == 200) return error::OK;
            if (result->status == 404) return error::NOT_FOUND;
            if (result->status == 409) return error::CONFLICT;
            if (result->status == 400) return error::BAD_REQUEST;
            return error::INTERNAL_ERROR;
        }});
    cb({"GET", "/api/channels/dataframe",
        [this](const std::string&, const std::string&, std::string& rsp) -> int32_t {
            httplib::Client client(scheduler_host_, scheduler_port_);
            client.set_connection_timeout(5);
            client.set_read_timeout(10);
            auto result = client.Get("/channels/dataframe");
            if (!result) { rsp = R"({"error":"service unreachable"})"; return error::UNAVAILABLE; }
            rsp = result->body;
            if (result->status == 200) return error::OK;
            if (result->status == 404) return error::NOT_FOUND;
            return error::INTERNAL_ERROR;
        }});
    cb({"POST", "/api/channels/dataframe/rename",
        [this](const std::string&, const std::string& req, std::string& rsp) -> int32_t {
            httplib::Client client(scheduler_host_, scheduler_port_);
            client.set_connection_timeout(5);
            client.set_read_timeout(10);
            auto result = client.Post("/channels/dataframe/rename", req, "application/json");
            if (!result) { rsp = R"({"error":"service unreachable"})"; return error::UNAVAILABLE; }
            rsp = result->body;
            if (result->status == 200) return error::OK;
            if (result->status == 404) return error::NOT_FOUND;
            if (result->status == 409) return error::CONFLICT;
            if (result->status == 400) return error::BAD_REQUEST;
            return error::INTERNAL_ERROR;
        }});
    cb({"POST", "/api/channels/dataframe/delete",
        [this](const std::string&, const std::string& req, std::string& rsp) -> int32_t {
            httplib::Client client(scheduler_host_, scheduler_port_);
            client.set_connection_timeout(5);
            client.set_read_timeout(10);
            auto result = client.Post("/channels/dataframe/delete", req, "application/json");
            if (!result) { rsp = R"({"error":"service unreachable"})"; return error::UNAVAILABLE; }
            rsp = result->body;
            if (result->status == 200) return error::OK;
            if (result->status == 404) return error::NOT_FOUND;
            if (result->status == 400) return error::BAD_REQUEST;
            return error::INTERNAL_ERROR;
        }});
}

// --- JSON 辅助 ---

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
    return ProxyGetJson(scheduler_host_, scheduler_port_, "/operators/query", &rsp);
}

int32_t WebServer::HandleUploadOperator(const std::string&, const std::string& req, std::string& rsp) {
    // 通过 JSON body 传递 base64 编码的文件内容
    // Body: {"filename":"xxx.py","content":"<base64>"}
    // 注意：multipart 上传需要 httplib 直接处理，此处简化为 JSON 上传
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("filename") || !doc.HasMember("content") ||
        !doc["filename"].IsString() || !doc["content"].IsString()) {
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

    // 上传成功后立即触发 Python Worker 重新发现 + Scheduler 刷新 Catalog，
    // 保证新算子能尽快出现在 /operators/query 列表中。
    NotifyWorkerReload();
    NotifySchedulerRefresh();

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
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/activate", req, &rsp);
}

// POST /api/operators/deactivate — Body: {"name":"catelog.opname"}
int32_t WebServer::HandleDeactivateOperator(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/deactivate", req, &rsp);
}

// POST /api/operators/detail — Body: {"name":"catelog.opname"}
int32_t WebServer::HandleGetOperatorDetail(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/detail", req, &rsp);
}

// POST /api/operators/update — Body: {"name":"catelog.opname","description":"...","content":"..."}
int32_t WebServer::HandleUpdateOperator(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/update", req, &rsp);
}

int32_t WebServer::HandleGetTasks(const std::string&, const std::string& req, std::string& rsp) {
    // 透传请求体（支持分页和状态过滤参数）
    const std::string body = req.empty() ? "{}" : req;
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/list", body, &rsp);
}

int32_t WebServer::HandleCreateTask(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/submit", req, &rsp);
}

// POST /api/tasks/result — Body: {"task_id":123}
int32_t WebServer::HandleGetTaskResult(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id")) {
        rsp = R"({"error":"invalid request, expected {\"task_id\":123}"})" ;
        return error::BAD_REQUEST;
    }
    std::string task_id;
    if (doc["task_id"].IsString()) task_id = doc["task_id"].GetString();
    else if (doc["task_id"].IsInt64()) task_id = std::to_string(doc["task_id"].GetInt64());
    else {
        rsp = R"({"error":"task_id must be string or int"})";
        return error::BAD_REQUEST;
    }

    rapidjson::StringBuffer req_buf;
    rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
    req_w.StartObject();
    req_w.Key("task_id");
    req_w.String(task_id.c_str());
    req_w.EndObject();

    std::string detail_rsp;
    int32_t rc = ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/detail", req_buf.GetString(), &detail_rsp);
    if (rc != error::OK) {
        rsp = detail_rsp;
        return rc;
    }

    rapidjson::Document detail;
    detail.Parse(detail_rsp.c_str());
    if (detail.HasParseError() || !detail.IsObject() || !detail.HasMember("status") || !detail["status"].IsString()) {
        rsp = R"({"error":"invalid task detail response"})";
        return error::INTERNAL_ERROR;
    }
    std::string status = detail["status"].GetString();
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status"); w.String(status.c_str());
    if (status == "failed") {
        w.Key("error");
        if (detail.HasMember("error_message") && detail["error_message"].IsString()) w.String(detail["error_message"].GetString());
        else w.String("execution failed");
        w.Key("error_code");
        if (detail.HasMember("error_code") && detail["error_code"].IsString()) w.String(detail["error_code"].GetString());
        else w.String("");
        w.Key("error_stage");
        if (detail.HasMember("error_stage") && detail["error_stage"].IsString()) w.String(detail["error_stage"].GetString());
        else w.String("");
    } else if (status == "completed") {
        int64_t rows = 0;
        int64_t cols = 0;
        if (detail.HasMember("result_row_count") && detail["result_row_count"].IsInt64()) rows = detail["result_row_count"].GetInt64();
        if (detail.HasMember("result_col_count") && detail["result_col_count"].IsInt64()) cols = detail["result_col_count"].GetInt64();
        w.Key("rows");
        w.Int64(rows);
        w.Key("cols");
        w.Int64(cols);
        w.Key("result_target");
        if (detail.HasMember("result_target") && detail["result_target"].IsString()) w.String(detail["result_target"].GetString());
        else w.String("");
        w.Key("data");
        w.StartArray();
        w.EndArray();
    }
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t WebServer::HandleDeleteTask(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/delete", req, &rsp);
}

int32_t WebServer::HandleCancelTask(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/cancel", req, &rsp);
}

int32_t WebServer::HandleTaskDiagnostics(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/diagnostics", req, &rsp);
}

}  // namespace web
}  // namespace flowsql
