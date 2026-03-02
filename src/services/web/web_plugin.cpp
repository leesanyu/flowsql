#include "web_plugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "framework/core/plugin_registry.h"

namespace flowsql {
namespace web {

int WebPlugin::Option(const char* arg) {
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
        else if (key == "db_path") db_path_ = val;
        else if (key == "worker_host") worker_host_ = val;
        else if (key == "worker_port") worker_port_ = std::stoi(val);

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }

    // Gateway 模式下，从环境变量获取 Gateway 地址作为 Worker 转发目标
    const char* gw = std::getenv("FLOWSQL_GATEWAY_ADDR");
    if (gw) {
        // 通过 Gateway 转发到 pyworker，使用 Gateway 地址
        std::string gw_addr(gw);
        size_t colon = gw_addr.find(':');
        if (colon != std::string::npos) {
            worker_host_ = gw_addr.substr(0, colon);
            worker_port_ = std::stoi(gw_addr.substr(colon + 1));
        }
    }

    return 0;
}

int WebPlugin::Load() {
    printf("WebPlugin::Load: host=%s, port=%d, db=%s\n", host_.c_str(), port_, db_path_.c_str());
    return 0;
}

int WebPlugin::Unload() {
    return 0;
}

int WebPlugin::Start() {
    auto* registry = PluginRegistry::Instance();

    // 初始化 WebServer
    server_.SetWorkerAddress(worker_host_, worker_port_);

    // Gateway 模式下，Scheduler 转发地址与 Worker 相同（都通过 Gateway 路由）
    server_.SetSchedulerAddress(worker_host_, worker_port_);

    if (server_.Init(db_path_, registry) != 0) {
        printf("WebPlugin::Start: failed to init WebServer\n");
        return -1;
    }

    // 在独立线程中启动 HTTP 监听（阻塞调用）
    server_thread_ = std::thread([this]() {
        printf("WebPlugin: listening on %s:%d\n", host_.c_str(), port_);
        server_.Start(host_, port_);
    });

    return 0;
}

int WebPlugin::Stop() {
    server_.Stop();
    if (server_thread_.joinable()) server_thread_.join();
    printf("WebPlugin::Stop: done\n");
    return 0;
}

}  // namespace web
}  // namespace flowsql
