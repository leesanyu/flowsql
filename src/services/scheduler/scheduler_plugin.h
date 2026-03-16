#ifndef _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_
#define _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

#include <common/error_code.h>
#include <common/iplugin.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/ibridge.h>

namespace flowsql {

class IChannel;
class IOperator;
struct SqlStatement;

namespace scheduler {

// SchedulerPlugin — SQL 执行调度插件
// 通过 IRouterHandle 声明路由，对 HTTP 完全无感知
class SchedulerPlugin : public IPlugin, public IRouterHandle {
 public:
    SchedulerPlugin() = default;
    ~SchedulerPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // IRouterHandle — 声明路由
    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;

 private:
    // 路由处理（fnRouterHandler 签名）
    int32_t HandleExecute(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleGetChannels(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleGetOperators(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleRefreshOperators(const std::string& uri, const std::string& req, std::string& rsp);

    // 通道管理
    IChannel* FindChannel(const std::string& name);
    void RegisterChannel(const std::string& key, std::shared_ptr<IChannel> ch);

    // 算子查找
    std::shared_ptr<IOperator> FindOperator(const std::string& catelog, const std::string& name);

    // 执行路径
    int ExecuteTransfer(IChannel* source, IChannel* sink,
                        const std::string& source_type, const std::string& sink_type,
                        const SqlStatement& stmt, int64_t* rows_affected = nullptr,
                        std::string* error = nullptr);

    int ExecuteWithOperator(IChannel* source, IChannel* sink, IOperator* op,
                            const std::string& source_type, const std::string& sink_type,
                            const SqlStatement& stmt, int64_t* rows_affected = nullptr,
                            std::string* error = nullptr);

    IQuerier* querier_ = nullptr;

    // 通道表
    std::unordered_map<std::string, std::shared_ptr<IChannel>> channels_;

    std::string host_ = "127.0.0.1";
    int port_ = 18803;

    // 用于生成唯一临时通道名
    std::atomic<uint64_t> tmp_channel_seq_{0};
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_
