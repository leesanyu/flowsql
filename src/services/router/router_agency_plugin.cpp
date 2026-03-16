#include "router_agency_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <common/error_code.h>
#include <common/log.h>

namespace flowsql {
namespace router {

// --- IPlugin ---

int RouterAgencyPlugin::Option(const char* arg) {
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

        if (key == "host") {
            host_ = val;
        } else if (key == "port") {
            port_ = std::stoi(val);
        } else if (key == "gateway") {
            // 格式：host:port
            size_t colon = val.find(':');
            if (colon != std::string::npos) {
                gateway_host_ = val.substr(0, colon);
                gateway_port_ = std::stoi(val.substr(colon + 1));
            } else {
                gateway_host_ = val;
            }
        } else if (key == "keepalive_interval_s") {
            keepalive_interval_s_ = std::stoi(val);
        }

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int RouterAgencyPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    LOG_INFO("RouterAgencyPlugin::Load: host=%s, port=%d, gateway=%s:%d",
             host_.c_str(), port_, gateway_host_.c_str(), gateway_port_);
    return 0;
}

int RouterAgencyPlugin::Unload() {
    return 0;
}

int RouterAgencyPlugin::Start() {
    // 1. 收集所有业务插件声明的路由
    CollectRoutes(querier_);
    LOG_INFO("RouterAgencyPlugin::Start: collected %zu routes, %zu prefixes",
             route_table_.size(), prefixes_.size());

    // 2. 启动 HTTP 服务线程
    running_ = true;
    http_thread_ = std::thread(&RouterAgencyPlugin::HttpThread, this);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 3. 立即同步注册一次，消除首次注册窗口期
    if (!gateway_host_.empty()) {
        RegisterOnce();
        // 4. 启动 KeepAlive 线程（定期续期 + 故障恢复）
        keepalive_thread_ = std::thread(&RouterAgencyPlugin::KeepAliveThread, this);
    }

    return 0;
}

int RouterAgencyPlugin::Stop() {
    running_ = false;

    // 优雅关闭：主动注销路由，让 Gateway 立即感知
    if (!gateway_host_.empty()) {
        UnregisterOnce();
    }

    server_.stop();

    if (keepalive_thread_.joinable()) keepalive_thread_.join();
    if (http_thread_.joinable()) http_thread_.join();

    LOG_INFO("RouterAgencyPlugin::Stop: done");
    return 0;
}

// --- 路由收集 ---

int RouterAgencyPlugin::CollectRoutes(IQuerier* querier) {
    if (!querier) return 0;

    querier->Traverse(IID_ROUTER_HANDLE, [&](void* p) -> int {
        auto* handle = static_cast<IRouterHandle*>(p);
        handle->EnumRoutes([&](const RouteItem& item) {
            std::string key = item.method + ":" + item.uri;
            if (route_table_.count(key)) {
                LOG_WARN("RouterAgencyPlugin: duplicate route %s, ignored", key.c_str());
            } else {
                route_table_[key] = item.handler;
                LOG_INFO("RouterAgencyPlugin: registered route %s", key.c_str());
            }
        });
        return 0;
    });

    // 从路由表提取一级前缀（用于 Gateway 注册）
    for (const auto& [key, _] : route_table_) {
        auto colon = key.find(':');
        if (colon == std::string::npos) continue;
        std::string uri = key.substr(colon + 1);
        if (uri.empty() || uri[0] != '/') continue;
        // 取第一个路径段作为前缀，如 "/channels/database/add" → "/channels"
        size_t second_slash = uri.find('/', 1);
        std::string prefix = (second_slash != std::string::npos)
                                 ? uri.substr(0, second_slash)
                                 : uri;
        prefixes_.insert(prefix);
    }

    return 0;
}

// --- HTTP 服务 ---

void RouterAgencyPlugin::HttpThread() {
    // 请求体大小限制：1MB
    server_.set_payload_max_length(1 * 1024 * 1024);

    // catch-all：所有方法统一走 Dispatch
    auto dispatch = [this](const httplib::Request& req, httplib::Response& res) {
        Dispatch(req, res);
    };
    server_.Get(R"(/.*)", dispatch);
    server_.Post(R"(/.*)", dispatch);
    server_.Put(R"(/.*)", dispatch);
    server_.Delete(R"(/.*)", dispatch);

    // CORS 预检
    server_.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, GET, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    LOG_INFO("RouterAgencyPlugin: HTTP listening on %s:%d", host_.c_str(), port_);
    server_.listen(host_, port_);
}

void RouterAgencyPlugin::Dispatch(const httplib::Request& req, httplib::Response& res) {
    // CORS 统一处理
    res.set_header("Access-Control-Allow-Origin", "*");

    std::string key = req.method + ":" + req.path;
    auto it = route_table_.find(key);
    if (it == route_table_.end()) {
        res.status = 404;
        res.set_content(R"({"error":"route not found"})", "application/json");
        return;
    }

    std::string rsp_json;
    int32_t rc = error::INTERNAL_ERROR;
    try {
        rc = it->second(req.path, req.body, rsp_json);
    } catch (const std::exception& e) {
        LOG_ERROR("RouterAgencyPlugin: handler exception: %s", e.what());
        rsp_json = R"({"error":"handler exception"})";
        rc = error::INTERNAL_ERROR;
    } catch (...) {
        rsp_json = R"({"error":"unknown exception"})";
        rc = error::INTERNAL_ERROR;
    }

    // handler 忘记设置 rsp_json 时兜底
    if (rsp_json.empty()) {
        rsp_json = (rc == error::OK) ? R"({"ok":true})" : R"({"error":"internal error"})";
    }

    res.status = HttpStatus(rc);
    res.set_content(rsp_json, "application/json");
}

int RouterAgencyPlugin::HttpStatus(int32_t rc) {
    switch (rc) {
        case error::OK:             return 200;
        case error::BAD_REQUEST:    return 400;
        case error::NOT_FOUND:      return 404;
        case error::CONFLICT:       return 409;
        case error::UNAVAILABLE:    return 503;
        case error::INTERNAL_ERROR: return 500;
        default:                    return (rc >= 0) ? 200 : 500;
    }
}

// --- Gateway 注册 ---

void RouterAgencyPlugin::RegisterOnce() {
    std::string local_addr = host_ + ":" + std::to_string(port_);
    for (const auto& prefix : prefixes_) {
        httplib::Client cli(gateway_host_, gateway_port_);
        cli.set_connection_timeout(2);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("prefix");  w.String(prefix.c_str());
        w.Key("address"); w.String(local_addr.c_str());
        w.EndObject();

        auto result = cli.Post("/gateway/register", buf.GetString(), "application/json");
        if (!result) {
            LOG_WARN("RouterAgencyPlugin: failed to register prefix %s (gateway unreachable)",
                     prefix.c_str());
        } else {
            LOG_INFO("RouterAgencyPlugin: registered prefix %s -> %s",
                     prefix.c_str(), local_addr.c_str());
        }
    }
}

void RouterAgencyPlugin::UnregisterOnce() {
    std::string local_addr = host_ + ":" + std::to_string(port_);
    for (const auto& prefix : prefixes_) {
        httplib::Client cli(gateway_host_, gateway_port_);
        cli.set_connection_timeout(2);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("prefix");  w.String(prefix.c_str());
        w.Key("address"); w.String(local_addr.c_str());
        w.EndObject();

        cli.Post("/gateway/unregister", buf.GetString(), "application/json");
    }
}

void RouterAgencyPlugin::KeepAliveThread() {
    while (running_) {
        // 分段 sleep，便于快速响应 Stop()
        for (int i = 0; i < keepalive_interval_s_ * 10 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!running_) break;
        RegisterOnce();
    }
}

}  // namespace router
}  // namespace flowsql
