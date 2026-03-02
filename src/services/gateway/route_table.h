#ifndef _FLOWSQL_GATEWAY_ROUTE_TABLE_H_
#define _FLOWSQL_GATEWAY_ROUTE_TABLE_H_

#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace gateway {

struct RouteEntry {
    std::string prefix;   // 路由前缀，如 "/web/api"
    std::string address;  // 目标地址，如 "127.0.0.1:18802"
    std::string service;  // 所属服务名，如 "web"
};

// 路由表 — 前 2 级 URI 精确匹配，线程安全
class RouteTable {
 public:
    // 注册路由（前缀不能重复）
    int Register(const std::string& prefix, const std::string& address, const std::string& service);

    // 注销指定前缀
    void Unregister(const std::string& prefix);

    // 注销某服务的所有路由
    void UnregisterByService(const std::string& service);

    // 匹配路由：从 URI 提取前 2 级路径精确查找
    const RouteEntry* Match(const std::string& uri);

    // 查询所有路由
    std::vector<RouteEntry> GetAll();

    // 从 URI 中剥离匹配的前缀
    static std::string StripPrefix(const std::string& uri, const std::string& prefix);

    // 从 URI 提取前 2 级路径作为匹配 key
    static std::string ExtractPrefix(const std::string& uri);

 private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, RouteEntry> routes_;  // key = prefix
};

}  // namespace gateway
}  // namespace flowsql

#endif  // _FLOWSQL_GATEWAY_ROUTE_TABLE_H_
