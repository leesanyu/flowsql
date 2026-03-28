#ifndef _FLOWSQL_WEB_WEB_SERVER_H_
#define _FLOWSQL_WEB_WEB_SERVER_H_

#include <httplib.h>

#include <memory>
#include <string>

#include <common/error_code.h>
#include <framework/interfaces/irouter_handle.h>

#include "db/database.h"

namespace flowsql {
namespace web {

// Web 管理系统主服务器
// 静态文件服务（/）保留 httplib::Server
// 管理 API（/api/*）通过 IRouterHandle 声明，由 RouterAgencyPlugin 分发
class WebServer {
 public:
    WebServer();
    ~WebServer() = default;

    // 初始化：打开数据库、注册静态文件路由
    int Init(const std::string& db_path);

    // 设置 Python Worker 地址
    void SetWorkerAddress(const std::string& host, int port);

    // 设置 Scheduler 转发地址
    void SetSchedulerAddress(const std::string& host, int port);
    void SetUploadDir(const std::string& dir) { upload_dir_ = dir; }

    // 启动静态文件服务监听（阻塞）
    int Start(const std::string& host, int port);

    // 停止
    void Stop();

    Database& GetDatabase() { return db_; }

    // 声明管理 API 路由（供 WebPlugin::EnumRoutes 调用）
    void EnumApiRoutes(std::function<void(const RouteItem&)> callback);

 private:
    // 管理 API handler（fnRouterHandler 签名）
    int32_t HandleHealth(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleGetChannels(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleGetOperators(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleUploadOperator(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleActivateOperator(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDeactivateOperator(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDeleteOperator(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleGetOperatorDetail(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleUpdateOperator(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleGetTasks(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleCreateTask(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleGetTaskResult(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDeleteTask(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleCancelTask(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleTaskDiagnostics(const std::string& uri, const std::string& req, std::string& rsp);

    // 通知 Python Worker 重新加载算子
    void NotifyWorkerReload();

    // 通知 Scheduler 刷新算子列表
    void NotifySchedulerRefresh();

    httplib::Server server_;  // 仅用于静态文件服务
    Database db_;
    std::string worker_host_ = "127.0.0.1";
    int worker_port_ = 18900;
    std::string scheduler_host_ = "127.0.0.1";
    int scheduler_port_ = 18800;
    std::string upload_dir_ = "./uploads";
};

}  // namespace web
}  // namespace flowsql

#endif  // _FLOWSQL_WEB_WEB_SERVER_H_
