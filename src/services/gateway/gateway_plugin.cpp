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
    // 路由 TTL = heartbeat_interval_s * heartbeat_timeout_count（默认 5*3=15s）
    route_ttl_s_ = config_.heartbeat_interval_s * config_.heartbeat_timeout_count;
    if (route_ttl_s_ <= 0) route_ttl_s_ = 30;
    return 0;
}

int GatewayPlugin::Load(IQuerier* /* querier */) {
    printf("GatewayPlugin::Load: gateway=%s:%d, route_ttl=%ds\n",
           config_.host.c_str(), config_.port, route_ttl_s_);
    return 0;
}

int GatewayPlugin::Unload() {
    return 0;
}

int GatewayPlugin::Start() {
    running_ = true;

    // 启动 HTTP 服务线程
    http_thread_ = std::thread(&GatewayPlugin::HttpThread, this);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 启动过期路由清理线程
    cleanup_thread_ = std::thread(&GatewayPlugin::CleanupThread, this);

    printf("GatewayPlugin::Start: gateway running on %s:%d\n",
           config_.host.c_str(), config_.port);
    return 0;
}

int GatewayPlugin::Stop() {
    running_ = false;
    server_.stop();

    if (cleanup_thread_.joinable()) cleanup_thread_.join();
    if (http_thread_.joinable()) http_thread_.join();

    printf("GatewayPlugin::Stop: done\n");
    return 0;
}

void GatewayPlugin::HttpThread() {
    // Gateway 管理 API（精确路由，优先于通配符）
    server_.Post("/gateway/register", [this](const httplib::Request& req, httplib::Response& res) {
        HandleRegister(req, res);
    });
    server_.Post("/gateway/unregister", [this](const httplib::Request& req, httplib::Response& res) {
        HandleUnregister(req, res);
    });
    server_.Get("/gateway/routes", [this](const httplib::Request& req, httplib::Response& res) {
        HandleRoutes(req, res);
    });

    // CORS 预检
    server_.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    // 通配符路由 — 转发到后端服务（转发完整 URI，不剥离前缀）
    auto forward = [this](const httplib::Request& req, httplib::Response& res) {
        HandleForward(req, res);
    };
    server_.Get(R"(/.*)", forward);
    server_.Post(R"(/.*)", forward);
    server_.Put(R"(/.*)", forward);
    server_.Delete(R"(/.*)", forward);

    printf("GatewayPlugin: HTTP listening on %s:%d\n", config_.host.c_str(), config_.port);
    server_.listen(config_.host, config_.port);
}

void GatewayPlugin::CleanupThread() {
    // 每隔 route_ttl_s_ / 2 秒检查一次过期路由
    int check_interval_ms = (route_ttl_s_ * 1000) / 2;
    if (check_interval_ms < 1000) check_interval_ms = 1000;

    while (running_) {
        // 分段 sleep，便于快速响应 Stop()
        for (int i = 0; i < check_interval_ms / 100 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!running_) break;

        int64_t expire_before = NowMs() - (int64_t)route_ttl_s_ * 1000;
        route_table_.RemoveExpired(expire_before);
    }
}

// --- Gateway 管理 API ---

void GatewayPlugin::HandleRegister(const httplib::Request& req, httplib::Response& res) {
    rapidjson::Document doc;
    doc.Parse(req.body.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("prefix") || !doc.HasMember("address")) {
        res.status = 400;
        res.set_content(R"({"error":"invalid request, expected {\"prefix\":\"...\",\"address\":\"...\"}"})",
                        "application/json");
        return;
    }

    std::string prefix = doc["prefix"].GetString();
    std::string address = doc["address"].GetString();

    if (prefix.empty() || prefix[0] != '/') {
        res.status = 400;
        res.set_content(R"({"error":"prefix must start with /"})", "application/json");
        return;
    }

    route_table_.Register(prefix, address);
    res.set_content(R"({"status":"ok"})", "application/json");
}

void GatewayPlugin::HandleUnregister(const httplib::Request& req, httplib::Response& res) {
    rapidjson::Document doc;
    doc.Parse(req.body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("prefix")) {
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
        w.Key("prefix");   w.String(r.prefix.c_str());
        w.Key("address");  w.String(r.address.c_str());
        w.Key("last_seen_ms"); w.Int64(r.last_seen_ms);
        w.EndObject();
    }
    w.EndArray();
    res.set_content(buf.GetString(), "application/json");
}

// --- 路由转发（转发完整 URI，不剥离前缀）---

void GatewayPlugin::HandleForward(const httplib::Request& req, httplib::Response& res) {
    RouteEntry route;
    if (!route_table_.Match(req.path, &route)) {
        res.status = 404;
        res.set_content(
            R"({"error":"no route matched","path":")" + req.path + R"("})",
            "application/json");
        return;
    }

    // 解析目标地址
    std::string host;
    int port = 80;
    size_t colon = route.address.find(':');
    if (colon != std::string::npos) {
        host = route.address.substr(0, colon);
        port = std::stoi(route.address.substr(colon + 1));
    } else {
        host = route.address;
    }

    // 转发完整 URI（含查询参数）
    std::string target = req.path;
    if (!req.params.empty()) {
        // 重建 query string
        bool first = true;
        for (const auto& [k, v] : req.params) {
            target += (first ? "?" : "&");
            target += k + "=" + v;
            first = false;
        }
    }

    httplib::Client client(host, port);
    client.set_connection_timeout(5);
    client.set_read_timeout(30);

    std::string content_type = req.get_header_value("Content-Type");
    httplib::Result result(nullptr, httplib::Error::Unknown);

    if (req.method == "GET") {
        result = client.Get(target);
    } else if (req.method == "POST") {
        result = client.Post(target, req.body, content_type.empty() ? "application/json" : content_type);
    } else if (req.method == "DELETE") {
        result = client.Delete(target);
    } else if (req.method == "PUT") {
        result = client.Put(target, req.body, content_type.empty() ? "application/json" : content_type);
    }

    if (!result) {
        res.status = 502;
        res.set_content(
            R"({"error":"upstream unreachable","address":")" + route.address + R"("})",
            "application/json");
        return;
    }

    res.status = result->status;
    res.body = result->body;
    for (const auto& [key, val] : result->headers) {
        res.set_header(key, val);
    }
}

}  // namespace gateway
}  // namespace flowsql
