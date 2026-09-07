// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "web_server.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <common/error_code.h>
#include <common/log.h>
#include <services/web/pcap_upload_transaction.hpp>

namespace flowsql {
namespace web {

namespace {

constexpr size_t kMaxPcapUploadFieldBytes = 4096;

enum class PcapMultipartPart {
    kNone,
    kField,
    kFile,
};

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

PcapUploadError MapPcapSchedulerError(int32_t error_code) {
    switch (error_code) {
        case error::OK:
            return PcapUploadError::kOk;
        case error::BAD_REQUEST:
            return PcapUploadError::kInvalidRequest;
        case error::CONFLICT:
            return PcapUploadError::kConflict;
        case error::UNAVAILABLE:
            return PcapUploadError::kUnavailable;
        default:
            return PcapUploadError::kInternal;
    }
}

std::string BuildPcapUploadErrorJson(PcapUploadError error_code) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("error");
    writer.String(MapPcapUploadError(error_code).error);
    writer.EndObject();
    return buffer.GetString();
}

int32_t RedactPublicStreamQueryResponse(std::string* response) {
    if (!response) return error::INTERNAL_ERROR;
    const auto fail = [response]() {
        *response = R"({"error":"invalid_scheduler_response"})";
        return error::INTERNAL_ERROR;
    };

    rapidjson::Document document;
    document.Parse(response->c_str());
    if (document.HasParseError() || !document.IsObject()) return fail();

    rapidjson::Value* channels = nullptr;
    size_t channels_count = 0;
    for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
        if (std::string(member->name.GetString(), member->name.GetStringLength()) != "channels") continue;
        ++channels_count;
        if (!member->value.IsArray()) return fail();
        channels = &member->value;
    }
    if (channels_count != 1 || !channels) return fail();

    for (auto& channel : channels->GetArray()) {
        if (!channel.IsObject()) return fail();
        const rapidjson::Value* type = nullptr;
        size_t type_count = 0;
        for (auto member = channel.MemberBegin(); member != channel.MemberEnd(); ++member) {
            if (std::string(member->name.GetString(), member->name.GetStringLength()) != "type") continue;
            ++type_count;
            if (!member->value.IsString()) return fail();
            type = &member->value;
        }
        if (type_count != 1 || !type) return fail();
        if (std::string(type->GetString(), type->GetStringLength()) != "pcapfile") continue;

        for (const char* field : {"path", "option", "options", "option_json"}) {
            while (channel.HasMember(field)) channel.RemoveMember(field);
        }
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    *response = buffer.GetString();
    return error::OK;
}

void SetPcapUploadResponse(PcapUploadError error_code,
                           const std::string& success_json,
                           httplib::Response* response) {
    if (!response) return;
    const PcapUploadErrorInfo error_info = MapPcapUploadError(error_code);
    response->status = error_info.http_status;
    response->set_content(error_code == PcapUploadError::kOk
                              ? success_json
                              : BuildPcapUploadErrorJson(error_code),
                          "application/json");
}

}  // namespace

// 内嵌 schema SQL
static const char* kSchemaSql = R"(
CREATE TABLE IF NOT EXISTS channels (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    category TEXT NOT NULL,
    name TEXT NOT NULL,
    type TEXT NOT NULL DEFAULT 'dataframe',
    schema_json TEXT DEFAULT '[]',
    status TEXT NOT NULL DEFAULT 'active',
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(category, name)
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
    // 逻辑链：
    // 1) 初始化本地元数据库与上传目录；
    // 2) 配置静态资源托管、CORS 与 SPA 回退；
    // 3) 绑定 /api 路由并将控制面请求转发到 Scheduler/Task 服务；
    // 4) 统一记录启动成功日志。
    if (db_.Open(db_path) != 0) {
        LOG_ERROR("WebServer::Init: failed to open database: %s", db_path.c_str());
        return -1;
    }
    if (db_.InitSchema(kSchemaSql) != 0) {
        LOG_ERROR("WebServer::Init: failed to init schema");
        return -1;
    }
    std::string capture_store_error;
    if (managed_capture_store_.Initialize(upload_dir_, &capture_store_error) != PcapUploadError::kOk) {
        LOG_ERROR("WebServer::Init: failed to initialize capture store: %s", capture_store_error.c_str());
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
    server_.Post("/api/channels/stream/query", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp;
        int32_t rc = HandleQueryStreamChannels("", req.body.empty() ? "{}" : req.body, rsp);
        if (rc == error::OK) {
            res.status = 200;
        } else if (rc == error::BAD_REQUEST) {
            res.status = 400;
        } else if (rc == error::NOT_FOUND) {
            res.status = 404;
        } else if (rc == error::CONFLICT) {
            res.status = 409;
        } else if (rc == error::UNAVAILABLE) {
            res.status = 503;
        } else {
            res.status = 500;
        }
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/channels/stream/definitions/query", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp;
        int32_t rc = HandleQueryStreamChannelDefinitions("", req.body.empty() ? "{}" : req.body, rsp);
        if (rc == error::OK) {
            res.status = 200;
        } else if (rc == error::BAD_REQUEST) {
            res.status = 400;
        } else if (rc == error::NOT_FOUND) {
            res.status = 404;
        } else if (rc == error::CONFLICT) {
            res.status = 409;
        } else if (rc == error::UNAVAILABLE) {
            res.status = 503;
        } else {
            res.status = 500;
        }
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/channels/stream/add", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp;
        int32_t rc = HandleAddStreamChannel("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 :
                     (rc == error::CONFLICT ? 409 :
                     (rc == error::NOT_FOUND ? 404 :
                     (rc == error::UNAVAILABLE ? 503 : 400)));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/channels/pcapfile/upload",
                 [this](const httplib::Request& req,
                        httplib::Response& res,
                        const httplib::ContentReader& content_reader) {
                     HandlePcapUpload(req, res, content_reader);
                 });
    server_.Post("/api/channels/stream/modify", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp;
        int32_t rc = HandleModifyStreamChannel("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 :
                     (rc == error::CONFLICT ? 409 :
                     (rc == error::NOT_FOUND ? 404 :
                     (rc == error::UNAVAILABLE ? 503 : 400)));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/channels/stream/reset", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp;
        int32_t rc = HandleResetStreamChannel("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 :
                     (rc == error::CONFLICT ? 409 :
                     (rc == error::NOT_FOUND ? 404 :
                     (rc == error::UNAVAILABLE ? 503 : 400)));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/channels/stream/remove", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp;
        int32_t rc = HandleRemoveStreamChannel("", req.body, rsp);
        if (rc == error::OK) {
            res.status = 200;
        } else if (rc == error::BAD_REQUEST) {
            res.status = 400;
        } else if (rc == error::NOT_FOUND) {
            res.status = 404;
        } else if (rc == error::CONFLICT) {
            res.status = 409;
        } else if (rc == error::UNAVAILABLE) {
            res.status = 503;
        } else {
            res.status = 500;
        }
        res.set_content(rsp, "application/json");
    });
    server_.Get("/api/operators/list", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string type = req.has_param("type") ? req.get_param_value("type") : "python";
        std::string req_json = std::string("{\"type\":\"") + type + "\"}";
        std::string rsp;
        int32_t rc = HandleGetOperators("", req_json, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::NOT_FOUND ? 404 : 400);
        res.set_content(rsp, "application/json");
    });
    server_.Get("/api/operators", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string type = req.has_param("type") ? req.get_param_value("type") : "python";
        std::string req_json = std::string("{\"type\":\"") + type + "\"}";
        std::string rsp;
        int32_t rc = HandleGetOperators("", req_json, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::NOT_FOUND ? 404 : 400);
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/operators/upload", [this](const httplib::Request& req, httplib::Response& res) {
        std::string body = req.body;
        std::string managed_tmp_path;
        if (req.has_file("file")) {
            const auto file = req.get_file_value("file");
            const std::string type = req.has_param("type")
                                         ? req.get_param_value("type")
                                         : ((file.filename.size() >= 3 && file.filename.substr(file.filename.size() - 3) == ".so")
                                                ? "cpp"
                                                : "python");
            std::filesystem::path tmp_path = std::filesystem::path(upload_dir_) /
                                             (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                              "_" + file.filename);
            FILE* fp = fopen(tmp_path.string().c_str(), "wb");
            if (!fp) {
                res.status = 500;
                res.set_content(R"({"error":"failed to persist upload temp file"})", "application/json");
                return;
            }
            fwrite(file.content.data(), 1, file.content.size(), fp);
            fclose(fp);

            rapidjson::StringBuffer req_buf;
            rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
            req_w.StartObject();
            req_w.Key("type");
            req_w.String(type.c_str());
            req_w.Key("filename");
            req_w.String(file.filename.c_str());
            req_w.Key("tmp_path");
            req_w.String(tmp_path.string().c_str());
            req_w.EndObject();
            body = req_buf.GetString();
            managed_tmp_path = tmp_path.string();
        }

        std::string rsp;
        int32_t rc = HandleUploadOperator("", body, rsp);
        if (rc != error::OK && !managed_tmp_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(managed_tmp_path, ec);
        }
        res.status = (rc == error::OK) ? 200 : (rc == error::CONFLICT ? 409 : (rc == error::NOT_FOUND ? 404 : 400));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/operators/activate", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleActivateOperator("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::CONFLICT ? 409 : (rc == error::NOT_FOUND ? 404 : 400));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/operators/deactivate", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleDeactivateOperator("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::CONFLICT ? 409 : (rc == error::NOT_FOUND ? 404 : 400));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/operators/delete", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleDeleteOperator("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::CONFLICT ? 409 : (rc == error::NOT_FOUND ? 404 : 400));
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
    server_.Post("/api/tasks/batch/execute", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleBatchExecuteTask("", req.body, rsp);
        res.status = (rc == 0) ? 200 : 400;
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/sql/classify", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleSqlClassifyTask("", req.body, rsp);
        res.status = (rc == 0) ? 200 : 400;
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/sql/analyze", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleSqlAnalyzeTask("", req.body, rsp);
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
    server_.Post("/api/tasks/stream/execute", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleStreamExecuteTask("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::CONFLICT ? 409 : (rc == error::NOT_FOUND ? 404 : (rc == error::UNAVAILABLE ? 503 : 400)));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/stream/stop", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleStreamStopTask("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::CONFLICT ? 409 : (rc == error::NOT_FOUND ? 404 : (rc == error::UNAVAILABLE ? 503 : 400)));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/stream/status", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleStreamStatusTask("", req.body, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::CONFLICT ? 409 : (rc == error::NOT_FOUND ? 404 : (rc == error::UNAVAILABLE ? 503 : 400)));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/stream/list", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleStreamListTask("", req.body.empty() ? "{}" : req.body, rsp);
        res.status = (rc == error::OK) ? 200 : (rc == error::CONFLICT ? 409 : (rc == error::NOT_FOUND ? 404 : (rc == error::UNAVAILABLE ? 503 : 400)));
        res.set_content(rsp, "application/json");
    });
    server_.Post("/api/tasks/runtime/graph/query", [this](const httplib::Request& req, httplib::Response& res) {
        std::string rsp; int32_t rc = HandleTaskRuntimeGraph("", req.body.empty() ? "{}" : req.body, rsp);
        if (rc == error::OK) {
            res.status = 200;
        } else if (rc == error::BAD_REQUEST) {
            res.status = 400;
        } else if (rc == error::NOT_FOUND) {
            res.status = 404;
        } else if (rc == error::CONFLICT) {
            res.status = 409;
        } else if (rc == error::UNAVAILABLE) {
            res.status = 503;
        } else {
            res.status = 500;
        }
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

void WebServer::HandlePcapUpload(const httplib::Request& req,
                                 httplib::Response& res,
                                 const httplib::ContentReader& content_reader) {
    if (!req.is_multipart_form_data()) {
        SetPcapUploadResponse(PcapUploadError::kInvalidRequest, "", &res);
        return;
    }

    PcapUploadFields fields;
    PcapMultipartPart current_part = PcapMultipartPart::kNone;
    std::string current_field_name;
    std::string current_field_value;
    std::string error_message;
    PcapUploadError upload_error = PcapUploadError::kOk;
    std::unique_ptr<PcapUploadTransaction> transaction;

    auto fail = [&](PcapUploadError error_code, const std::string& message) {
        if (upload_error == PcapUploadError::kOk) {
            upload_error = error_code;
            error_message = message;
        }
        if (transaction) transaction->Rollback();
        return false;
    };
    auto finish_field = [&]() {
        if (current_part != PcapMultipartPart::kField) return true;
        if (!fields.emplace(current_field_name, std::move(current_field_value)).second) {
            return fail(PcapUploadError::kInvalidRequest, "duplicate multipart field");
        }
        current_field_name.clear();
        current_field_value.clear();
        current_part = PcapMultipartPart::kNone;
        return true;
    };

    const bool read_ok = content_reader(
        [&](const httplib::MultipartFormData& part) {
            if (!finish_field()) return false;
            if (transaction) {
                return fail(PcapUploadError::kInvalidRequest, "file part must be last");
            }
            if (part.name != "file") {
                if (!part.filename.empty()) {
                    return fail(PcapUploadError::kInvalidRequest, "unexpected file part");
                }
                current_part = PcapMultipartPart::kField;
                current_field_name = part.name;
                current_field_value.clear();
                return true;
            }

            PcapUploadRequest upload_request;
            upload_error = ParsePcapUploadFields(fields, part.filename, &upload_request, &error_message);
            if (upload_error != PcapUploadError::kOk) return false;
            transaction = std::make_unique<PcapUploadTransaction>(
                &managed_capture_store_,
                std::move(upload_request),
                pcap_upload_max_bytes_,
                [this](const std::string& scheduler_request, std::string* message) {
                    std::string scheduler_response;
                    const int32_t rc = ProxyPostJson(scheduler_host_,
                                                     scheduler_port_,
                                                     "/channels/stream/add",
                                                     scheduler_request,
                                                     &scheduler_response);
                    if (rc != error::OK && message) *message = std::move(scheduler_response);
                    return MapPcapSchedulerError(rc);
                });
            upload_error = transaction->Begin(&error_message);
            if (upload_error != PcapUploadError::kOk) return false;
            current_part = PcapMultipartPart::kFile;
            return true;
        },
        [&](const char* data, size_t size) {
            if (current_part == PcapMultipartPart::kField) {
                if (size > kMaxPcapUploadFieldBytes - current_field_value.size()) {
                    return fail(PcapUploadError::kInvalidRequest, "multipart field is too large");
                }
                current_field_value.append(data, size);
                return true;
            }
            if (current_part != PcapMultipartPart::kFile || !transaction) {
                return fail(PcapUploadError::kInvalidRequest, "multipart content has no field header");
            }
            upload_error = transaction->Write(data, size, &error_message);
            return upload_error == PcapUploadError::kOk;
        });

    if (!read_ok && upload_error == PcapUploadError::kOk) {
        fail(PcapUploadError::kInvalidRequest, "invalid multipart request");
    }
    if (upload_error == PcapUploadError::kOk && !finish_field()) {
        upload_error = PcapUploadError::kInvalidRequest;
    }
    if (upload_error == PcapUploadError::kOk && !transaction) {
        fail(PcapUploadError::kInvalidRequest, "missing multipart file");
    }

    std::string public_json;
    if (upload_error == PcapUploadError::kOk) {
        upload_error = transaction->Complete(&public_json, &error_message);
    }
    SetPcapUploadResponse(upload_error, public_json, &res);
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

void WebServer::EnumApiRoutes(std::function<void(const RouteItem&)> cb) {
    // 逻辑链：
    // 1) 声明 Web 插件本地处理的管理面 API；
    // 2) 对通道、任务、算子接口进行统一路由注册；
    // 3) 对需后端处理的接口封装 HTTP 代理，保持 Web 层轻量转发职责。
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
    cb({"POST", "/api/channels/stream/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleQueryStreamChannels(u, req, rsp);
        }});
    cb({"POST", "/api/channels/stream/definitions/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleQueryStreamChannelDefinitions(u, req, rsp);
        }});
    cb({"POST", "/api/channels/stream/add",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleAddStreamChannel(u, req, rsp);
        }});
    cb({"POST", "/api/channels/stream/modify",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleModifyStreamChannel(u, req, rsp);
        }});
    cb({"POST", "/api/channels/stream/reset",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleResetStreamChannel(u, req, rsp);
        }});
    cb({"POST", "/api/channels/stream/remove",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRemoveStreamChannel(u, req, rsp);
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
    cb({"POST", "/api/operators/delete",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleDeleteOperator(u, req, rsp);
        }});
    cb({"POST", "/api/operators/detail",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetOperatorDetail(u, req, rsp);
        }});
    cb({"POST", "/api/operators/update",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleUpdateOperator(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/batch/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleBatchExecuteTask(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/sql/classify",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleSqlClassifyTask(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/sql/analyze",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleSqlAnalyzeTask(u, req, rsp);
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
    cb({"POST", "/api/tasks/stream/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamExecuteTask(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/stream/stop",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamStopTask(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/stream/status",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamStatusTask(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/stream/list",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamListTask(u, req, rsp);
        }});
    cb({"POST", "/api/tasks/runtime/graph/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleTaskRuntimeGraph(u, req, rsp);
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
        auto rows = db_.Query("SELECT id, category, name, type, schema_json, status, created_at FROM channels");
        rsp = RowsToJson(rows);
    }
    return error::OK;
}

int32_t WebServer::HandleQueryStreamChannels(const std::string&, const std::string& req, std::string& rsp) {
    const std::string body = req.empty() ? "{}" : req;
    const int32_t rc = ProxyPostJson(scheduler_host_, scheduler_port_, "/channels/stream/query", body, &rsp);
    return rc == error::OK ? RedactPublicStreamQueryResponse(&rsp) : rc;
}

int32_t WebServer::HandleQueryStreamChannelDefinitions(const std::string&,
                                                       const std::string& req,
                                                       std::string& rsp) {
    const std::string body = req.empty() ? "{}" : req;
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/channels/stream/definitions/query", body, &rsp);
}

int32_t WebServer::HandleAddStreamChannel(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/channels/stream/add", req, &rsp);
}

int32_t WebServer::HandleModifyStreamChannel(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/channels/stream/modify", req, &rsp);
}

int32_t WebServer::HandleResetStreamChannel(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/channels/stream/reset", req, &rsp);
}

int32_t WebServer::HandleRemoveStreamChannel(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document request;
    request.Parse(req.c_str());
    if (request.HasParseError() || !request.IsObject() ||
        !request.HasMember("type") || !request["type"].IsString() || request["type"].GetStringLength() == 0 ||
        !request.HasMember("name") || !request["name"].IsString() || request["name"].GetStringLength() == 0) {
        rsp = R"({"error":"invalid_request"})";
        return error::BAD_REQUEST;
    }

    const std::string type(request["type"].GetString(), request["type"].GetStringLength());
    if (type != "pcapfile") {
        return ProxyPostJson(scheduler_host_, scheduler_port_, "/channels/stream/remove", req, &rsp);
    }
    const std::string name(request["name"].GetString(), request["name"].GetStringLength());

    std::string query_response;
    const int32_t query_rc =
        ProxyPostJson(scheduler_host_, scheduler_port_, "/channels/stream/query", "{}", &query_response);
    if (query_rc != error::OK) {
        rsp = std::move(query_response);
        return query_rc;
    }

    rapidjson::Document query;
    query.Parse(query_response.c_str());
    if (query.HasParseError() || !query.IsObject() ||
        !query.HasMember("channels") || !query["channels"].IsArray()) {
        rsp = R"({"error":"invalid_scheduler_response"})";
        return error::INTERNAL_ERROR;
    }

    size_t match_count = 0;
    std::string capture_path;
    for (const auto& channel : query["channels"].GetArray()) {
        if (!channel.IsObject() || !channel.HasMember("type") || !channel["type"].IsString() ||
            !channel.HasMember("name") || !channel["name"].IsString()) {
            rsp = R"({"error":"invalid_scheduler_response"})";
            return error::INTERNAL_ERROR;
        }
        const std::string channel_type(channel["type"].GetString(), channel["type"].GetStringLength());
        const std::string channel_name(channel["name"].GetString(), channel["name"].GetStringLength());
        if (channel_type != type || channel_name != name) continue;

        ++match_count;
        if (match_count > 1 || !channel.HasMember("option_json") || !channel["option_json"].IsObject()) {
            rsp = R"({"error":"invalid_scheduler_response"})";
            return error::INTERNAL_ERROR;
        }
        const auto& option = channel["option_json"];
        if (option.HasMember("path")) {
            if (!option["path"].IsString()) {
                rsp = R"({"error":"invalid_scheduler_response"})";
                return error::INTERNAL_ERROR;
            }
            capture_path.assign(option["path"].GetString(), option["path"].GetStringLength());
        }
    }
    if (match_count != 1) {
        rsp = R"({"error":"invalid_scheduler_response"})";
        return error::INTERNAL_ERROR;
    }

    const int32_t remove_rc =
        ProxyPostJson(scheduler_host_, scheduler_port_, "/channels/stream/remove", req, &rsp);
    if (remove_rc != error::OK || capture_path.empty() ||
        !managed_capture_store_.IsManagedPath(capture_path)) {
        return remove_rc;
    }

    std::string remove_message;
    if (managed_capture_store_.RemoveManaged(capture_path, &remove_message) != PcapUploadError::kOk) {
        rsp = R"({"error":"storage_failure"})";
        return error::INTERNAL_ERROR;
    }
    return error::OK;
}

int32_t WebServer::HandleGetOperators(const std::string&, const std::string& req, std::string& rsp) {
    const std::string body = req.empty() ? R"({"type":"python"})" : req;
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/list", body, &rsp);
}

int32_t WebServer::HandleUploadOperator(const std::string&, const std::string& req, std::string& rsp) {
    // Body(兼容): {"type":"python|cpp","filename":"...","content":"..."} 或 {"type":"...","filename":"...","tmp_path":"..."}
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("filename") || !doc["filename"].IsString()) {
        rsp = R"({"error":"invalid request, expected {\"filename\":\"...\",\"content\":\"...\"} or {\"filename\":\"...\",\"tmp_path\":\"...\"}"})";
        return error::BAD_REQUEST;
    }
    const std::string type =
        (doc.HasMember("type") && doc["type"].IsString()) ? doc["type"].GetString() : "python";
    const std::string filename = doc["filename"].GetString();

    if (filename.find('/') != std::string::npos || filename.find("..") != std::string::npos) {
        rsp = R"({"error":"invalid filename"})";
        return error::BAD_REQUEST;
    }

    std::string tmp_path;
    bool created_temp = false;
    if (doc.HasMember("tmp_path") && doc["tmp_path"].IsString()) {
        tmp_path = doc["tmp_path"].GetString();
    } else {
        if (!doc.HasMember("content") || !doc["content"].IsString()) {
            rsp = R"({"error":"missing content or tmp_path"})";
            return error::BAD_REQUEST;
        }
        const std::string content = doc["content"].GetString();
        std::filesystem::path tmp = std::filesystem::path(upload_dir_) /
                                    (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                     "_" + filename);
        FILE* fp = fopen(tmp.string().c_str(), "wb");
        if (!fp) {
            rsp = R"({"error":"failed to persist upload temp file"})";
            return error::INTERNAL_ERROR;
        }
        fwrite(content.data(), 1, content.size(), fp);
        fclose(fp);
        tmp_path = tmp.string();
        created_temp = true;
    }

    rapidjson::StringBuffer req_buf;
    rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
    req_w.StartObject();
    req_w.Key("type");
    req_w.String(type.c_str());
    req_w.Key("filename");
    req_w.String(filename.c_str());
    req_w.Key("tmp_path");
    req_w.String(tmp_path.c_str());
    req_w.EndObject();

    int32_t rc = ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/upload", req_buf.GetString(), &rsp);
    if (rc != error::OK && created_temp) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
    }
    if (rc == error::OK && type == "python") {
        NotifyWorkerReload();
        NotifySchedulerRefresh();
    }
    return rc;
}

// POST /api/operators/activate — Body: {"name":"category.opname"}
int32_t WebServer::HandleActivateOperator(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/activate", req, &rsp);
}

// POST /api/operators/deactivate — Body: {"name":"category.opname"}
int32_t WebServer::HandleDeactivateOperator(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/deactivate", req, &rsp);
}

int32_t WebServer::HandleDeleteOperator(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/delete", req, &rsp);
}

// POST /api/operators/detail — Body: {"name":"category.opname"}
int32_t WebServer::HandleGetOperatorDetail(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/detail", req, &rsp);
}

// POST /api/operators/update — Body: {"name":"category.opname","description":"...","content":"..."}
int32_t WebServer::HandleUpdateOperator(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/operators/update", req, &rsp);
}

int32_t WebServer::HandleGetTasks(const std::string&, const std::string& req, std::string& rsp) {
    // 透传请求体（支持分页和状态过滤参数）
    const std::string body = req.empty() ? "{}" : req;
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/list", body, &rsp);
}

int32_t WebServer::HandleBatchExecuteTask(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/batch/execute", req, &rsp);
}

int32_t WebServer::HandleSqlClassifyTask(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/sql/classify", req, &rsp);
}

int32_t WebServer::HandleSqlAnalyzeTask(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/sql/analyze", req, &rsp);
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

int32_t WebServer::HandleStreamExecuteTask(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/stream/execute", req, &rsp);
}

int32_t WebServer::HandleStreamStopTask(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/stream/stop", req, &rsp);
}

int32_t WebServer::HandleStreamStatusTask(const std::string&, const std::string& req, std::string& rsp) {
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/stream/status", req, &rsp);
}

int32_t WebServer::HandleStreamListTask(const std::string&, const std::string& req, std::string& rsp) {
    const std::string body = req.empty() ? "{}" : req;
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/stream/list", body, &rsp);
}

int32_t WebServer::HandleTaskRuntimeGraph(const std::string&, const std::string& req, std::string& rsp) {
    const std::string body = req.empty() ? "{}" : req;
    return ProxyPostJson(scheduler_host_, scheduler_port_, "/tasks/runtime/graph/query", body, &rsp);
}

}  // namespace web
}  // namespace flowsql
