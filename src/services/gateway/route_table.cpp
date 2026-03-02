#include "route_table.h"

#include <cstdio>
#include <mutex>

namespace flowsql {
namespace gateway {

int RouteTable::Register(const std::string& prefix, const std::string& address, const std::string& service) {
    std::unique_lock lock(mutex_);
    if (routes_.count(prefix)) {
        printf("RouteTable: prefix already exists: %s\n", prefix.c_str());
        return -1;
    }
    routes_[prefix] = {prefix, address, service};
    printf("RouteTable: registered %s -> %s (%s)\n", prefix.c_str(), address.c_str(), service.c_str());
    return 0;
}

void RouteTable::Unregister(const std::string& prefix) {
    std::unique_lock lock(mutex_);
    routes_.erase(prefix);
}

void RouteTable::UnregisterByService(const std::string& service) {
    std::unique_lock lock(mutex_);
    for (auto it = routes_.begin(); it != routes_.end();) {
        if (it->second.service == service) {
            printf("RouteTable: unregistered %s (%s)\n", it->first.c_str(), service.c_str());
            it = routes_.erase(it);
        } else {
            ++it;
        }
    }
}

const RouteEntry* RouteTable::Match(const std::string& uri) {
    std::shared_lock lock(mutex_);
    // 提取前 2 级路径作为 key
    std::string key = ExtractPrefix(uri);
    auto it = routes_.find(key);
    if (it != routes_.end()) return &it->second;

    // 回退到前 1 级路径
    size_t second_slash = key.find('/', 1);
    if (second_slash != std::string::npos) {
        std::string key1 = key.substr(0, second_slash);
        it = routes_.find(key1);
        if (it != routes_.end()) return &it->second;
    }
    return nullptr;
}

std::vector<RouteEntry> RouteTable::GetAll() {
    std::shared_lock lock(mutex_);
    std::vector<RouteEntry> result;
    result.reserve(routes_.size());
    for (auto& [_, entry] : routes_) {
        result.push_back(entry);
    }
    return result;
}

std::string RouteTable::StripPrefix(const std::string& uri, const std::string& prefix) {
    if (uri.size() >= prefix.size() && uri.compare(0, prefix.size(), prefix) == 0) {
        std::string stripped = uri.substr(prefix.size());
        if (stripped.empty() || stripped[0] != '/') stripped = "/" + stripped;
        return stripped;
    }
    return uri;
}

std::string RouteTable::ExtractPrefix(const std::string& uri) {
    // 从 URI 提取前 2 级路径："/web/api/health" → "/web/api"
    if (uri.empty() || uri[0] != '/') return uri;

    size_t first = uri.find('/', 1);
    if (first == std::string::npos) return uri;  // 只有 1 级

    size_t second = uri.find('/', first + 1);
    if (second == std::string::npos) return uri;  // 只有 2 级

    return uri.substr(0, second);  // 前 2 级
}

}  // namespace gateway
}  // namespace flowsql
