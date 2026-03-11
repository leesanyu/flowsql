#include <cassert>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include <services/database/connection_pool.h>

using namespace flowsql::database;

// ============================================================
// Test 1: 连接池基础功能 - 获取和归还
// ============================================================
void test_pool_acquire_return() {
    printf("[TEST] Connection pool: acquire and return...\n");

    int connection_counter = 0;

    // 简单的模拟连接（整数 ID）
    auto factory = [&connection_counter](std::string* error) -> int {
        return ++connection_counter;
    };

    auto closer = [](int conn) {
        printf("  Closing connection %d\n", conn);
    };

    auto pinger = [](int conn) -> bool {
        return true;  // 总是健康
    };

    ConnectionPoolConfig config;
    config.max_connections = 5;
    config.min_connections = 0;
    config.idle_timeout = std::chrono::seconds(300);
    config.health_check_interval = std::chrono::seconds(60);

    ConnectionPool<int> pool(config, factory, closer, pinger);

    // 获取连接
    int conn1;
    bool ok = pool.Acquire(&conn1, nullptr);
    assert(ok);
    assert(conn1 == 1);
    printf("  Acquired connection %d\n", conn1);

    // 归还连接
    pool.Return(conn1);
    printf("  Returned connection %d\n", conn1);

    // 再次获取应复用同一个连接
    int conn2;
    ok = pool.Acquire(&conn2, nullptr);
    assert(ok);
    assert(conn2 == conn1);  // 复用
    printf("  Reused connection %d\n", conn2);

    pool.Return(conn2);

    printf("[PASS] Connection pool: acquire and return\n");
}

// ============================================================
// Test 2: 连接池最大连接数限制
// ============================================================
void test_pool_max_connections() {
    printf("[TEST] Connection pool: max connections limit...\n");

    ConnectionPoolConfig config;
    config.max_connections = 3;
    config.min_connections = 0;
    config.idle_timeout = std::chrono::seconds(300);
    config.health_check_interval = std::chrono::seconds(60);

    int create_count = 0;
    auto factory = [&create_count](std::string* error) -> int {
        return ++create_count;
    };

    auto closer = [](int conn) {};
    auto pinger = [](int conn) -> bool { return true; };

    ConnectionPool<int> pool(config, factory, closer, pinger);

    // 获取 3 个连接（达到最大值）
    int conn1, conn2, conn3;
    assert(pool.Acquire(&conn1, nullptr));
    assert(pool.Acquire(&conn2, nullptr));
    assert(pool.Acquire(&conn3, nullptr));
    printf("  Acquired 3 connections: %d, %d, %d\n", conn1, conn2, conn3);

    // 尝试获取第 4 个连接（应失败）
    int conn4;
    std::string error;
    bool ok = pool.Acquire(&conn4, &error);
    assert(!ok);
    assert(!error.empty());
    printf("  4th connection rejected: %s\n", error.c_str());

    // 归还一个连接后再获取应成功
    pool.Return(conn1);
    int conn5;
    ok = pool.Acquire(&conn5, nullptr);
    assert(ok);
    printf("  After return, acquired connection %d\n", conn5);

    // 清理
    pool.Return(conn2);
    pool.Return(conn3);
    pool.Return(conn5);

    printf("[PASS] Connection pool: max connections limit\n");
}

// ============================================================
// Test 3: 连接池空闲超时回收
// ============================================================
void test_pool_idle_timeout() {
    printf("[TEST] Connection pool: idle timeout...\n");

    ConnectionPoolConfig config;
    config.max_connections = 5;
    config.min_connections = 0;
    // idle_timeout=1s，需等待 >1s 才能触发（duration_cast<seconds> 截断）
    // 实际等待 2100ms，确保 idle_time=2 > 1
    config.idle_timeout = std::chrono::seconds(1);
    config.health_check_interval = std::chrono::seconds(60);

    int create_count = 0;
    int close_count = 0;

    auto factory = [&create_count](std::string* error) -> int {
        return ++create_count;
    };

    auto closer = [&close_count](int conn) {
        close_count++;
        printf("  Closed connection %d\n", conn);
    };

    auto pinger = [](int conn) -> bool { return true; };

    ConnectionPool<int> pool(config, factory, closer, pinger);

    // 获取并归还连接
    int conn1;
    assert(pool.Acquire(&conn1, nullptr));
    pool.Return(conn1);
    printf("  Acquired and returned connection %d\n", conn1);

    // 等待超时：idle_timeout=1s，duration_cast<seconds> 截断，需等 2100ms 使 idle_time=2 > 1
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    // 再次获取，应创建新连接（旧连接已超时）
    int conn2;
    assert(pool.Acquire(&conn2, nullptr));
    assert(conn2 != conn1);  // 新连接
    printf("  Got new connection %d (old was %d)\n", conn2, conn1);

    pool.Return(conn2);

    // 验证关闭计数
    assert(close_count >= 1);
    printf("  Connections closed: %d\n", close_count);

    printf("[PASS] Connection pool: idle timeout\n");
}

// ============================================================
// Test 4: 连接池健康检查
// ============================================================
void test_pool_health_check() {
    printf("[TEST] Connection pool: health check...\n");

    ConnectionPoolConfig config;
    config.max_connections = 5;
    config.min_connections = 0;
    config.idle_timeout = std::chrono::seconds(300);
    // health_check_interval=1s，duration_cast<seconds> 用 >=，等 1100ms 即可触发
    config.health_check_interval = std::chrono::seconds(1);

    int create_count = 0;
    int close_count = 0;
    int ping_fail_count = 0;

    auto factory = [&create_count](std::string* error) -> int {
        return ++create_count;
    };

    auto closer = [&close_count](int conn) {
        close_count++;
    };

    int check_count = 0;
    auto pinger = [&ping_fail_count, &check_count](int conn) -> bool {
        // 模拟连接失效：第 1 次检查就失败
        check_count++;
        ping_fail_count++;
        return false;  // 健康检查失败，连接被丢弃
    };

    ConnectionPool<int> pool(config, factory, closer, pinger);

    // 获取并归还连接
    int conn1;
    assert(pool.Acquire(&conn1, nullptr));
    pool.Return(conn1);

    // 等待健康检查间隔：health_check_interval=1s，since_check >= 1s 即触发，等 1100ms
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // 再次获取，应创建新连接（健康检查失败）
    int conn2;
    assert(pool.Acquire(&conn2, nullptr));
    printf("  Got connection %d (old was %d, ping_fail=%d)\n", conn2, conn1, ping_fail_count);

    pool.Return(conn2);

    assert(ping_fail_count >= 1);
    assert(close_count >= 1);

    printf("[PASS] Connection pool: health check\n");
}

// ============================================================
// Test 5: 连接池统计信息
// ============================================================
void test_pool_stats() {
    printf("[TEST] Connection pool: statistics...\n");

    ConnectionPoolConfig config;
    config.max_connections = 5;
    config.min_connections = 0;
    config.idle_timeout = std::chrono::seconds(300);
    config.health_check_interval = std::chrono::seconds(60);

    int create_count = 0;
    auto factory = [&create_count](std::string* error) -> int {
        return ++create_count;
    };
    auto closer = [](int conn) {};
    auto pinger = [](int conn) -> bool { return true; };

    ConnectionPool<int> pool(config, factory, closer, pinger);

    // 初始状态
    auto stats = pool.GetStats();
    assert(stats.total_connections == 0);
    assert(stats.available_connections == 0);
    assert(stats.in_use_connections == 0);
    printf("  Initial: total=%d, available=%d, in_use=%d\n",
           stats.total_connections, stats.available_connections, stats.in_use_connections);

    // 获取一个连接
    int conn1;
    pool.Acquire(&conn1, nullptr);
    stats = pool.GetStats();
    assert(stats.total_connections == 1);
    assert(stats.in_use_connections == 1);
    printf("  After acquire: total=%d, in_use=%d\n",
           stats.total_connections, stats.in_use_connections);

    // 归还连接
    pool.Return(conn1);
    stats = pool.GetStats();
    assert(stats.total_connections == 1);
    assert(stats.available_connections == 1);
    assert(stats.in_use_connections == 0);
    printf("  After return: total=%d, available=%d\n",
           stats.total_connections, stats.available_connections);

    printf("[PASS] Connection pool: statistics\n");
}

// ============================================================
// Test 6: 并发安全测试
// ============================================================
void test_pool_concurrency() {
    printf("[TEST] Connection pool: concurrency...\n");

    ConnectionPoolConfig config;
    config.max_connections = 10;
    config.min_connections = 0;
    config.idle_timeout = std::chrono::seconds(300);
    config.health_check_interval = std::chrono::seconds(60);

    std::mutex mtx;
    std::vector<int> connections_used;

    auto factory = [&mtx](std::string* error) -> int {
        static int counter = 0;
        std::lock_guard<std::mutex> lock(mtx);
        return ++counter;
    };

    auto closer = [](int conn) {};
    auto pinger = [](int conn) -> bool { return true; };

    ConnectionPool<int> pool(config, factory, closer, pinger);

    std::atomic<int> peak_in_use{0};
    std::atomic<int> current_in_use{0};

    // 多线程并发获取和归还
    std::vector<std::thread> threads;
    for (int i = 0; i < 20; ++i) {
        threads.emplace_back([&pool, &mtx, &connections_used,
                              &peak_in_use, &current_in_use]() {
            int conn;
            if (pool.Acquire(&conn, nullptr)) {
                int cur = ++current_in_use;
                // 记录峰值并发数
                int expected = peak_in_use.load();
                while (cur > expected &&
                       !peak_in_use.compare_exchange_weak(expected, cur)) {}

                std::this_thread::sleep_for(std::chrono::milliseconds(10));

                --current_in_use;
                pool.Return(conn);
                std::lock_guard<std::mutex> lock(mtx);
                connections_used.push_back(conn);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    printf("  Concurrent operations: %zu succeeded, peak_in_use=%d (max=%d)\n",
           connections_used.size(), peak_in_use.load(), config.max_connections);

    // ① 至少有连接被使用
    assert(connections_used.size() > 0);
    // ② 峰值并发数不超过 max_connections
    assert(peak_in_use.load() <= config.max_connections);
    // ③ 所有线程结束后无连接泄漏（in_use 归零）
    assert(current_in_use.load() == 0);
    // ④ 连接池统计一致
    auto stats = pool.GetStats();
    assert(stats.in_use_connections == 0);

    printf("[PASS] Connection pool: concurrency\n");
}

// ============================================================
// main
// ============================================================
int main() {
    printf("=== FlowSQL Connection Pool Tests ===\n\n");

    test_pool_acquire_return();
    test_pool_max_connections();
    test_pool_idle_timeout();
    test_pool_health_check();
    test_pool_stats();
    test_pool_concurrency();

    printf("\n=== All connection pool tests passed ===\n");
    return 0;
}
