#include "gateway_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace flowsql {
namespace gateway {

int GatewayPlugin::Option(const char* arg) {
    if (!arg || strlen(arg) == 0) {
        printf("GatewayPlugin: no config file specified\n");
        return -1;
    }
    if (LoadConfig(arg, &config_) != 0) {
        return -1;
    }
    return 0;
}

int GatewayPlugin::Load() {
    printf("GatewayPlugin::Load: gateway=%s:%d, %zu services\n", config_.host.c_str(), config_.port,
           config_.services.size());
    return 0;
}

int GatewayPlugin::Unload() {
    return 0;
}

int GatewayPlugin::Start() {
    running_ = true;
    std::string gateway_addr = config_.host + ":" + std::to_string(config_.port);

    // 1. 启动 HTTP 服务线程
    http_thread_ = std::thread(&GatewayPlugin::HttpThread, this);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 2. 启动所有子服务
    service_manager_.StartAll(config_, gateway_addr);

    // 3. 启动心跳检测线程
    heartbeat_thread_ = std::thread(&GatewayPlugin::HeartbeatThread, this);

    printf("GatewayPlugin::Start: gateway running on %s\n", gateway_addr.c_str());
    return 0;
}

int GatewayPlugin::Stop() {
    running_ = false;

    // 停止子服务
    service_manager_.StopAll();

    // 停止 HTTP 服务
    server_.stop();

    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (http_thread_.joinable()) http_thread_.join();

    printf("GatewayPlugin::Stop: done\n");
    return 0;
}

void GatewayPlugin::HttpThread() {
    // Gateway 管理 API
    server_.Post("/gateway/register", [this](const httplib::Request& req, httplib::Response& res) {
        HandleRegister(req, res);
    });
    server_.Post("/gateway/unregister", [this](const httplib::Request& req, httplib::Response& res) {
        HandleUnregister(req, res);
    });
    server_.Get("/gateway/routes", [this](const httplib::Request& req, httplib::Response& res) {
        HandleRoutes(req, res);
    });
    server_.Post("/gateway/heartbeat", [this](const httplib::Request& req, httplib::Response& res) {
        HandleHeartbeat(req, res);
    });

    // 通配符路由 — 转发到子服务
    auto forward = [this](const httplib::Request& req, httplib::Response& res) {
        HandleForward(req, res);
    };
    server_.Get(R"(/.*)", forward);
    server_.Post(R"(/.*)", forward);
    server_.Put(R"(/.*)", forward);
    server_.Delete(R"(/.*)", forward);
    server_.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    printf("GatewayPlugin: HTTP listening on %s:%d\n", config_.host.c_str(), config_.port);
    server_.listen(config_.host, config_.port);
}

void GatewayPlugin::HeartbeatThread() {
    std::string gateway_addr = config_.host + ":" + std::to_string(config_.port);
    int timeout_s = config_.heartbeat_interval_s * config_.heartbeat_timeout_count;

    while (running_) {
        // 分段 sleep
        for (int i = 0; i < config_.heartbeat_interval_s * 10 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!running_) break;
        service_manager_.CheckAndRestart(timeout_s, gateway_addr);
    }
}

// --- Gateway API 处理 ---

void GatewayPlugin::HandleRegister(const httplib::Request& req, httplib::Response& res) {
    rapidjson::Document doc;
    doc.Parse(req.body.c_str());
    if (doc.HasParseError() || !doc.HasMember("prefix") || !doc.HasMember("address")) {
        res.status = 400;
        res.set_content(R"({"error":"invalid request"})", "application/json");
        return;
    }

    std::string prefix = doc["prefix"].GetString();
    std::string address = doc["address"].GetString();
    std::string service = doc.HasMember("service") ? doc["service"].GetString() : "";

    if (route_table_.Register(prefix, address, service) != 0) {
        res.status = 409;
        res.set_content(R"({"error":"prefix already registered"})", "application/json");
        return;
    }
    res.set_content(R"({"status":"ok"})", "application/json");
}

void GatewayPlugin::HandleUnregister(const httplib::Request& req, httplib::Response& res) {
    rapidjson::Document doc;
    doc.Parse(req.body.c_str());
    if (doc.HasParseError() || !doc.HasMember("prefix")) {
        res.status = 400;
        res.set_content(R"({"error":"invalid request"})", "application/json");
        return;
    }
    route_table_.Unregister(doc["prefix"].GetString());
    res.set_content(R"({"status":"ok"})", "application/json");
}

void GatewayPlugin::HandleRoutes(const httplib::Request&, httplib::Response& res) {
    auto routes = route_table_.GetAll();
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for (const auto& r : routes) {
        w.StartObject();
        w.Key("prefix"); w.String(r.prefix.c_str());
        w.Key("address"); w.String(r.address.c_str());
        w.Key("service"); w.String(r.service.c_str());
        w.EndObject();
    }
    w.EndArray();
    res.set_content(buf.GetString(), "application/json");
}

void GatewayPlugin::HandleHeartbeat(const httplib::Request& req, httplib::Response& res) {
    rapidjson::Document doc;
    doc.Parse(req.body.c_str());
    if (doc.HasParseError() || !doc.HasMember("service")) {
        res.status = 400;
        res.set_content(R"({"error":"invalid request"})", "application/json");
        return;
    }
    service_manager_.UpdateHeartbeat(doc["service"].GetString());
    res.set_content(R"({"status":"ok"})", "application/json");
}

// --- 路由转发 ---

void GatewayPlugin::HandleForward(const httplib::Request& req, httplib::Response& res) {
    const RouteEntry* route = route_table_.Match(req.path);
    if (!route) {
        res.status = 404;
        res.set_content(R"({"error":"no route matched","path":")" + req.path + R"("})", "application/json");
        return;
    }

    // 剥离前缀，构造目标路径
    std::string target_path = RouteTable::StripPrefix(req.path, route->prefix);

    // 解析目标地址
    std::string addr = route->address;
    std::string host;
    int port;
    size_t colon = addr.find(':');
    if (colon != std::string::npos) {
        host = addr.substr(0, colon);
        port = std::stoi(addr.substr(colon + 1));
    } else {
        host = addr;
        port = 80;
    }

    // 转发请求
    httplib::Client client(host, port);
    client.set_connection_timeout(5);
    client.set_read_timeout(30);

    std::string content_type = req.get_header_value("Content-Type");
    httplib::Result result(nullptr, httplib::Error::Unknown);

    if (req.method == "GET") {
        result = client.Get(target_path);
    } else if (req.method == "POST") {
        result = client.Post(target_path, req.body, content_type);
    } else if (req.method == "DELETE") {
        result = client.Delete(target_path);
    } else if (req.method == "PUT") {
        result = client.Put(target_path, req.body, content_type);
    }

    if (!result) {
        res.status = 502;
        res.set_content(R"({"error":"upstream unreachable","service":")" + route->service + R"("})",
                        "application/json");
        return;
    }

    // 回写响应
    res.status = result->status;
    res.body = result->body;
    for (const auto& [key, val] : result->headers) {
        res.set_header(key, val);
    }
}

}  // namespace gateway
}  // namespace flowsql
