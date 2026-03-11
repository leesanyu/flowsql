#ifndef _FLOWSQL_SERVICES_DATABASE_CONNECTION_POOL_H_
#define _FLOWSQL_SERVICES_DATABASE_CONNECTION_POOL_H_

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace flowsql {
namespace database {

// 连接池配置
struct ConnectionPoolConfig {
    int max_connections = 10;                    // 最大连接数
    int min_connections = 0;                     // 最小连接数
    std::chrono::seconds idle_timeout = std::chrono::seconds(300);  // 空闲超时（5 分钟）
    std::chrono::seconds health_check_interval = std::chrono::seconds(60);  // 健康检查间隔
};

// 连接池实现
// 线程安全的连接池，支持连接复用、超时回收、健康检查
template<typename ConnectionType>
class ConnectionPool {
public:
    // 连接工厂函数
    using FactoryFunc = std::function<ConnectionType(std::string* error)>;
    // 关闭连接函数
    using CloseFunc = std::function<void(ConnectionType)>;
    // 健康检查函数
    using PingFunc = std::function<bool(ConnectionType)>;

    ConnectionPool(ConnectionPoolConfig config,
                   FactoryFunc factory,
                   CloseFunc closer,
                   PingFunc pinger)
        : config_(std::move(config)),
          factory_(std::move(factory)),
          closer_(std::move(closer)),
          pinger_(std::move(pinger)),
          total_connections_(0) {
        // 预创建最小连接数
        PrecreateConnections(config_.min_connections);
    }

    ~ConnectionPool() {
        // 关闭所有连接
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto conn : pool_) {
            closer_(conn);
        }
        pool_.clear();
        total_connections_ = 0;
    }

    // 获取连接（带健康检查）
    // 返回 true 表示成功，false 表示失败（error 会包含错误信息）
    bool Acquire(ConnectionType* conn, std::string* error) {
        while (true) {
            ConnectionType candidate{};
            bool need_ping = false;
            bool need_close = false;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pool_.empty()) {
                    // 池中没有可用连接，尝试创建新连接
                    if (total_connections_ < config_.max_connections) {
                        ConnectionType new_conn = factory_(error);
                        if (new_conn) {
                            auto now = std::chrono::steady_clock::now();
                            total_connections_++;
                            conn_info_[new_conn] = {now, now, true};
                            *conn = new_conn;
                            return true;
                        }
                        return false;
                    }
                    if (error) {
                        *error = "Connection pool exhausted (max_connections=" +
                                 std::to_string(config_.max_connections) + ")";
                    }
                    return false;
                }

                candidate = pool_.front();
                pool_.pop_front();

                auto now = std::chrono::steady_clock::now();
                auto& info = conn_info_[candidate];
                auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
                    now - info.last_used).count();

                if (idle_time > config_.idle_timeout.count()) {
                    // 超时，锁外关闭
                    conn_info_.erase(candidate);
                    total_connections_--;
                    need_close = true;
                } else {
                    auto since_check = std::chrono::duration_cast<std::chrono::seconds>(
                        now - info.last_check).count();
                    if (since_check >= config_.health_check_interval.count()) {
                        // 需要健康检查，锁外 ping
                        need_ping = true;
                    } else {
                        // 连接可用，直接返回
                        info.last_used = now;
                        info.in_use = true;
                        *conn = candidate;
                        return true;
                    }
                }
            }  // 释放锁

            if (need_close) {
                closer_(candidate);
                continue;  // 重新尝试获取
            }

            if (need_ping) {
                bool alive = pinger_(candidate);
                std::lock_guard<std::mutex> lock(mutex_);
                if (!alive) {
                    conn_info_.erase(candidate);
                    total_connections_--;
                    closer_(candidate);
                    continue;  // 重新尝试获取
                }
                auto now = std::chrono::steady_clock::now();
                auto it = conn_info_.find(candidate);
                if (it == conn_info_.end()) {
                    // 极端情况：ping 期间连接被其他路径清理
                    closer_(candidate);
                    continue;
                }
                it->second.last_check = now;
                it->second.last_used = now;
                it->second.in_use = true;
                *conn = candidate;
                return true;
            }
        }
    }

    // 归还连接（直接放回池中，超时清理在 Acquire 时进行）
    void Return(ConnectionType conn) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = conn_info_.find(conn);
        if (it == conn_info_.end()) {
            // 连接不在 map 中（可能已被清理），直接关闭避免泄漏
            closer_(conn);
            return;
        }
        it->second.in_use = false;
        it->second.last_used = std::chrono::steady_clock::now();
        pool_.push_back(conn);
    }

    // 获取当前池状态
    struct PoolStats {
        int total_connections;
        int available_connections;
        int in_use_connections;
    };

    PoolStats GetStats() {
        std::lock_guard<std::mutex> lock(mutex_);
        int available = static_cast<int>(pool_.size());
        return {
            total_connections_,
            available,
            total_connections_ - available
        };
    }

private:
    // 预创建连接
    void PrecreateConnections(int count) {
        for (int i = 0; i < count; ++i) {
            std::string error;
            ConnectionType conn = factory_(&error);
            if (conn) {
                auto now = std::chrono::steady_clock::now();
                pool_.push_back(conn);
                
                conn_info_[conn] = {now, now, false};
                total_connections_++;
            } else {
                // 预创建失败，记录日志但继续
                // TODO: 使用日志系统
            }
        }
    }

    ConnectionPoolConfig config_;
    FactoryFunc factory_;
    CloseFunc closer_;
    PingFunc pinger_;

    std::deque<ConnectionType> pool_;
    std::mutex mutex_;

    // 连接信息
    struct ConnectionInfo {
        std::chrono::steady_clock::time_point last_used;
        std::chrono::steady_clock::time_point last_check;
        bool in_use;
    };
    std::unordered_map<ConnectionType, ConnectionInfo> conn_info_;

    int total_connections_;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_CONNECTION_POOL_H_
