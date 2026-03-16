#include "web_plugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
        else if (key == "gateway") {
            // 格式：host:port，内部服务转发目标
            size_t colon = val.find(':');
            if (colon != std::string::npos) {
                gateway_host_ = val.substr(0, colon);
                gateway_port_ = std::stoi(val.substr(colon + 1));
            }
        }

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }

    return 0;
}

int WebPlugin::Load(IQuerier* /* querier */) {
    printf("WebPlugin::Load: host=%s, port=%d, db=%s\n", host_.c_str(), port_, db_path_.c_str());
    return 0;
}

int WebPlugin::Unload() {
    return 0;
}

int WebPlugin::Start() {
    server_.SetWorkerAddress(worker_host_, worker_port_);
    // 内部服务转发走 Gateway，与 worker 地址分开
    server_.SetSchedulerAddress(gateway_host_, gateway_port_);

    if (server_.Init(db_path_) != 0) {
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

void WebPlugin::EnumRoutes(std::function<void(const RouteItem&)> callback) {
    server_.EnumApiRoutes(callback);
}

}  // namespace web
}  // namespace flowsql
