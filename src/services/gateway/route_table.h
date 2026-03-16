#ifndef _FLOWSQL_GATEWAY_ROUTE_TABLE_H_
#define _FLOWSQL_GATEWAY_ROUTE_TABLE_H_

#include <chrono>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace gateway {

struct RouteEntry {
    std::string prefix;       // 路由前缀，如 "/channels"
    std::string address;      // 目标地址，如 "127.0.0.1:18803"
    int64_t last_seen_ms = 0; // KeepAlive 每次注册时更新（毫秒时间戳）
};

// Trie 节点
struct TrieNode {
    std::unordered_map<std::string, std::unique_ptr<TrieNode>> children;
    std::unique_ptr<RouteEntry> entry;  // 非空表示此节点是一个注册的前缀终点
};

// 路由表 — 字典树（Trie）实现，按路径段逐级匹配，线程安全
// 匹配语义：最长前缀匹配（longest prefix match）
class RouteTable {
 public:
    RouteTable();

    // 注册路由前缀（幂等：已存在则更新 address 和 last_seen_ms）
    void Register(const std::string& prefix, const std::string& address);

    // 注销指定前缀
    void Unregister(const std::string& prefix);

    // 匹配路由：按路径段逐级查找最长匹配
    // 返回匹配到的 RouteEntry 指针（调用方不持有所有权，仅在锁内有效）
    // 注意：返回值为拷贝，调用方安全持有
    bool Match(const std::string& uri, RouteEntry* out) const;

    // 移除 last_seen_ms < before_ms 的过期条目
    void RemoveExpired(int64_t before_ms);

    // 查询所有路由（用于 /gateway/routes 端点）
    std::vector<RouteEntry> GetAll() const;

 private:
    // 将 URI 按 '/' 分割为路径段（忽略空段）
    static std::vector<std::string> SplitPath(const std::string& uri);

    // 递归移除过期节点，返回该节点是否可以被删除（无子节点且无 entry）
    bool RemoveExpiredRecursive(TrieNode* node, int64_t before_ms);

    // 递归收集所有 entry
    void CollectAll(const TrieNode* node, std::vector<RouteEntry>& out) const;

    mutable std::shared_mutex mutex_;
    TrieNode root_;
};

// 获取当前毫秒时间戳
inline int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace gateway
}  // namespace flowsql

#endif  // _FLOWSQL_GATEWAY_ROUTE_TABLE_H_
