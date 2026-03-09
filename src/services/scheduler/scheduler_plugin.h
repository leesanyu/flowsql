#ifndef _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_
#define _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_

#include <httplib.h>

#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include <common/iplugin.h>

#include "framework/interfaces/ibridge.h"

namespace flowsql {

class IChannel;
class IOperator;
struct SqlStatement;

namespace scheduler {

// SchedulerPlugin — SQL 执行调度插件
// 内部维护通道表和算子查找，通过 IQuerier 遍历插件接口
class SchedulerPlugin : public IPlugin {
 public:
    SchedulerPlugin() = default;
    ~SchedulerPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

 private:
    // 注册 HTTP 路由
    void RegisterRoutes();

    // HTTP 端点处理
    void HandleExecute(const httplib::Request& req, httplib::Response& res);
    void HandleGetChannels(const httplib::Request& req, httplib::Response& res);
    void HandleGetOperators(const httplib::Request& req, httplib::Response& res);
    void HandleRefreshOperators(httplib::Response& res);

    // 通道管理（原来在 PluginRegistry 里，现在内部维护）
    IChannel* FindChannel(const std::string& name);
    void RegisterChannel(const std::string& key, std::shared_ptr<IChannel> ch);

    // 算子查找（先查 C++ 静态算子，再查 IBridge Python 算子）
    std::shared_ptr<IOperator> FindOperator(const std::string& catelog, const std::string& name);

    // 执行路径：无算子的纯数据搬运
    // rows_affected: 可选的输出参数，返回受影响的行数（写入/读取的行数）
    int ExecuteTransfer(IChannel* source, IChannel* sink,
                        const std::string& source_type, const std::string& sink_type,
                        const SqlStatement& stmt, int64_t* rows_affected = nullptr,
                        std::string* error = nullptr);

    // 执行路径：有算子，自动适配通道类型
    int ExecuteWithOperator(IChannel* source, IChannel* sink, IOperator* op,
                            const std::string& source_type, const std::string& sink_type,
                            const SqlStatement& stmt, int64_t* rows_affected = nullptr,
                            std::string* error = nullptr);

    IQuerier* querier_ = nullptr;  // Load 时传入，用于查询算子等插件接口
    httplib::Server server_;
    std::thread server_thread_;

    // 通道表（Scheduler 内部管理，替代 PluginRegistry 的动态注册）
    std::unordered_map<std::string, std::shared_ptr<IChannel>> channels_;

    std::string host_ = "127.0.0.1";
    int port_ = 18803;
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_
