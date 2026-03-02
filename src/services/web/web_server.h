#ifndef _FLOWSQL_WEB_WEB_SERVER_H_
#define _FLOWSQL_WEB_WEB_SERVER_H_

#include <httplib.h>

#include <memory>
#include <string>

#include "db/database.h"

namespace flowsql {

class PluginRegistry;

namespace web {

// Web 管理系统主服务器
class WebServer {
 public:
    WebServer();
    ~WebServer() = default;

    // 初始化：打开数据库、注册路由、同步插件信息
    int Init(const std::string& db_path, PluginRegistry* registry);

    // 设置 Python Worker 地址（用于算子激活/停用时通知 Worker 重载）
    void SetWorkerAddress(const std::string& host, int port);

    // 设置 Scheduler 转发地址（通过 Gateway 转发 SQL 执行请求）
    void SetSchedulerAddress(const std::string& host, int port);

    // 启动监听（阻塞）
    int Start(const std::string& host, int port);

    // 停止
    void Stop();

    Database& GetDatabase() { return db_; }
    PluginRegistry* GetRegistry() { return registry_; }

 private:
    // 注册所有 API 路由
    void RegisterRoutes();

    // 同步 PluginRegistry 中的通道和算子信息到 SQLite
    void SyncRegistryToDb();

    // API 处理函数
    void HandleHealth(const httplib::Request& req, httplib::Response& res);
    void HandleGetChannels(const httplib::Request& req, httplib::Response& res);
    void HandleGetOperators(const httplib::Request& req, httplib::Response& res);
    void HandleUploadOperator(const httplib::Request& req, httplib::Response& res);
    void HandleActivateOperator(const httplib::Request& req, httplib::Response& res);
    void HandleDeactivateOperator(const httplib::Request& req, httplib::Response& res);
    void HandleGetTasks(const httplib::Request& req, httplib::Response& res);
    void HandleCreateTask(const httplib::Request& req, httplib::Response& res);
    void HandleGetTaskResult(const httplib::Request& req, httplib::Response& res);

    // 通知 Python Worker 重新加载算子，并同步 PluginRegistry 到数据库
    void NotifyWorkerReload();

    httplib::Server server_;
    Database db_;
    PluginRegistry* registry_ = nullptr;
    std::string worker_host_ = "127.0.0.1";
    int worker_port_ = 18900;
    std::string scheduler_host_ = "127.0.0.1";
    int scheduler_port_ = 18800;  // 默认指向 Gateway
};

}  // namespace web
}  // namespace flowsql

#endif  // _FLOWSQL_WEB_WEB_SERVER_H_
