#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IROUTER_HANDLE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IROUTER_HANDLE_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>
#include <string>

namespace flowsql {

// 路由处理函数：纯业务逻辑，不感知 HTTP
// 参数：uri（完整路径）、req_json（请求体 JSON）、rsp_json（响应体 JSON，输出）
// 返回：error::OK 成功，其他值见 error_code.h
typedef std::function<int32_t(const std::string& uri,
                               const std::string& req_json,
                               std::string& rsp_json)>
    fnRouterHandler;

// 路由条目
struct RouteItem {
    std::string method;   // "GET" / "POST" / "PUT" / "DELETE"
    std::string uri;      // 完整路径，如 "/tasks/instant/execute"
    fnRouterHandler handler;
};

// {0xa1b2c3d4-e5f6-7890-abcd-ef1234567890}
const Guid IID_ROUTER_HANDLE = {
    0xa1b2c3d4, 0xe5f6, 0x7890, {0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90}};

// 业务插件实现此接口，声明自己提供的路由
// RouterAgencyPlugin 在 Start() 时通过 Traverse(IID_ROUTER_HANDLE) 收集所有路由
interface IRouterHandle {
    virtual ~IRouterHandle() = default;
    virtual void EnumRoutes(std::function<void(const RouteItem&)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IROUTER_HANDLE_H_
