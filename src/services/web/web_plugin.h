#ifndef _FLOWSQL_WEB_WEB_PLUGIN_H_
#define _FLOWSQL_WEB_WEB_PLUGIN_H_

#include <string>
#include <thread>

#include <common/loader.hpp>

#include "web_server.h"

namespace flowsql {
namespace web {

// WebPlugin — Web 管理系统插件
// 将 WebServer 包装为 IPlugin，支持 Gateway 架构动态加载
class WebPlugin : public IPlugin {
 public:
    WebPlugin() = default;
    ~WebPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load() override;
    int Unload() override;
    int Start() override;
    int Stop() override;

 private:
    WebServer server_;
    std::thread server_thread_;

    std::string host_ = "127.0.0.1";
    int port_ = 8081;
    std::string db_path_ = "/tmp/flowsql.db";
    std::string worker_host_ = "127.0.0.1";
    int worker_port_ = 18900;
};

}  // namespace web
}  // namespace flowsql

#endif  // _FLOWSQL_WEB_WEB_PLUGIN_H_
