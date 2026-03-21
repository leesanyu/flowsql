#ifndef _FLOWSQL_WEB_WEB_PLUGIN_H_
#define _FLOWSQL_WEB_WEB_PLUGIN_H_

#include <string>
#include <thread>

#include <common/iplugin.h>
#include <framework/interfaces/irouter_handle.h>

#include "web_server.h"

namespace flowsql {
namespace web {

// WebPlugin — Web 管理系统插件
// 静态文件服务（/）保留 httplib::Server，管理 API（/api/*）通过 IRouterHandle 声明
class WebPlugin : public IPlugin, public IRouterHandle {
 public:
    WebPlugin() = default;
    ~WebPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // IRouterHandle — 声明 /api/* 管理路由
    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;

 private:
    WebServer server_;
    std::thread server_thread_;

    std::string host_ = "127.0.0.1";
    int port_ = 8081;
    std::string db_path_ = "/tmp/flowsql.db";
    std::string worker_host_ = "127.0.0.1";
    int worker_port_ = 18900;
    std::string gateway_host_ = "127.0.0.1";
    int gateway_port_ = 18800;  // 内部服务转发目标（Gateway）
    std::string upload_dir_ = "./uploads";
};

}  // namespace web
}  // namespace flowsql

#endif  // _FLOWSQL_WEB_WEB_PLUGIN_H_
