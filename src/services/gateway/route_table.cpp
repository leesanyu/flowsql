#include "route_table.h"

#include <cstdio>
#include <mutex>
#include <sstream>

namespace flowsql {
namespace gateway {

RouteTable::RouteTable() = default;

std::vector<std::string> RouteTable::SplitPath(const std::string& uri) {
    std::vector<std::string> segments;
    std::istringstream ss(uri);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (!seg.empty()) segments.push_back(seg);
    }
    return segments;
}

void RouteTable::Register(const std::string& prefix, const std::string& address) {
    std::unique_lock lock(mutex_);
    auto segments = SplitPath(prefix);
    TrieNode* node = &root_;
    for (const auto& seg : segments) {
        auto it = node->children.find(seg);
        if (it == node->children.end()) {
            node->children[seg] = std::make_unique<TrieNode>();
        }
        node = node->children[seg].get();
    }
    if (node->entry) {
        // 幂等更新：更新 address 和时间戳
        node->entry->address = address;
        node->entry->last_seen_ms = NowMs();
        printf("RouteTable: updated %s -> %s\n", prefix.c_str(), address.c_str());
    } else {
        node->entry = std::make_unique<RouteEntry>();
        node->entry->prefix = prefix;
        node->entry->address = address;
        node->entry->last_seen_ms = NowMs();
        printf("RouteTable: registered %s -> %s\n", prefix.c_str(), address.c_str());
    }
}

void RouteTable::Unregister(const std::string& prefix) {
    std::unique_lock lock(mutex_);
    auto segments = SplitPath(prefix);
    TrieNode* node = &root_;
    for (const auto& seg : segments) {
        auto it = node->children.find(seg);
        if (it == node->children.end()) return;
        node = it->second.get();
    }
    if (node->entry) {
        printf("RouteTable: unregistered %s\n", prefix.c_str());
        node->entry.reset();
    }
}

bool RouteTable::Match(const std::string& uri, RouteEntry* out) const {
    std::shared_lock lock(mutex_);
    auto segments = SplitPath(uri);
    const TrieNode* node = &root_;
    const RouteEntry* best = nullptr;

    // 逐段匹配，记录沿途遇到的最后一个 entry（最长前缀匹配）
    for (const auto& seg : segments) {
        auto it = node->children.find(seg);
        if (it == node->children.end()) break;
        node = it->second.get();
        if (node->entry) best = node->entry.get();
    }

    if (!best) return false;
    *out = *best;
    return true;
}

bool RouteTable::RemoveExpiredRecursive(TrieNode* node, int64_t before_ms) {
    // 先递归处理子节点
    for (auto it = node->children.begin(); it != node->children.end();) {
        bool can_delete = RemoveExpiredRecursive(it->second.get(), before_ms);
        if (can_delete) {
            it = node->children.erase(it);
        } else {
            ++it;
        }
    }
    // 清理过期 entry
    if (node->entry && node->entry->last_seen_ms < before_ms) {
        printf("RouteTable: expired %s\n", node->entry->prefix.c_str());
        node->entry.reset();
    }
    // 如果没有 entry 且没有子节点，可以被父节点删除
    return !node->entry && node->children.empty();
}

void RouteTable::RemoveExpired(int64_t before_ms) {
    std::unique_lock lock(mutex_);
    RemoveExpiredRecursive(&root_, before_ms);
}

void RouteTable::CollectAll(const TrieNode* node, std::vector<RouteEntry>& out) const {
    if (node->entry) out.push_back(*node->entry);
    for (const auto& [_, child] : node->children) {
        CollectAll(child.get(), out);
    }
}

std::vector<RouteEntry> RouteTable::GetAll() const {
    std::shared_lock lock(mutex_);
    std::vector<RouteEntry> result;
    CollectAll(&root_, result);
    return result;
}

}  // namespace gateway
}  // namespace flowsql
