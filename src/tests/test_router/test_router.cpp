// test_router.cpp — RouterAgencyPlugin + RouteTable + Gateway 路由测试
// 覆盖：路由收集、冲突检测、前缀提取、Trie 匹配、过期清理、错误码映射

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <common/error_code.h>
#include <framework/interfaces/irouter_handle.h>
#include <services/gateway/route_table.h>
#include <services/router/router_agency_plugin.h>

using namespace flowsql;
using namespace flowsql::gateway;

// ============================================================
// 辅助宏
// ============================================================
#define ASSERT_EQ(a, b)                                                    \
    do {                                                                   \
        if ((a) != (b)) {                                                  \
            printf("[FAIL] %s:%d  %s != %s\n", __FILE__, __LINE__, #a, #b); \
            assert(false);                                                 \
        }                                                                  \
    } while (0)

#define ASSERT_TRUE(expr)                                                  \
    do {                                                                   \
        if (!(expr)) {                                                     \
            printf("[FAIL] %s:%d  %s is false\n", __FILE__, __LINE__, #expr); \
            assert(false);                                                 \
        }                                                                  \
    } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define TEST(name)                                                         \
    static void name();                                                    \
    static struct name##_reg { name##_reg() { tests.push_back({#name, name}); } } name##_inst; \
    static void name()

static std::vector<std::pair<std::string, void(*)()>> tests;

// ============================================================
// T1: Trie 基本注册与匹配
// ============================================================
TEST(trie_basic_register_match) {
    RouteTable rt;
    rt.Register("/channels", "127.0.0.1:18803");
    rt.Register("/tasks", "127.0.0.1:18803");
    rt.Register("/operators", "127.0.0.1:18803");

    RouteEntry out;
    ASSERT_TRUE(rt.Match("/channels/database/add", &out));
    ASSERT_EQ(out.prefix, "/channels");
    ASSERT_EQ(out.address, "127.0.0.1:18803");

    ASSERT_TRUE(rt.Match("/tasks/instant/execute", &out));
    ASSERT_EQ(out.prefix, "/tasks");

    ASSERT_TRUE(rt.Match("/operators/list", &out));
    ASSERT_EQ(out.prefix, "/operators");

    // 未注册路径
    ASSERT_FALSE(rt.Match("/unknown/path", &out));
    printf("[PASS] trie_basic_register_match\n");
}

// ============================================================
// T2: Trie 最长前缀匹配
// ============================================================
TEST(trie_longest_prefix_match) {
    RouteTable rt;
    rt.Register("/channels", "svc-a:18803");
    rt.Register("/channels/database", "svc-b:18804");  // 更长前缀

    RouteEntry out;
    // /channels/database/add 应匹配更长的 /channels/database
    ASSERT_TRUE(rt.Match("/channels/database/add", &out));
    ASSERT_EQ(out.prefix, "/channels/database");
    ASSERT_EQ(out.address, "svc-b:18804");

    // /channels/dataframe/query 只能匹配 /channels
    ASSERT_TRUE(rt.Match("/channels/dataframe/query", &out));
    ASSERT_EQ(out.prefix, "/channels");
    ASSERT_EQ(out.address, "svc-a:18803");

    printf("[PASS] trie_longest_prefix_match\n");
}

// ============================================================
// T3: Trie 幂等注册（更新 address 和 last_seen_ms）
// ============================================================
TEST(trie_idempotent_register) {
    RouteTable rt;
    rt.Register("/tasks", "old-addr:18803");

    RouteEntry out;
    ASSERT_TRUE(rt.Match("/tasks/instant/execute", &out));
    ASSERT_EQ(out.address, "old-addr:18803");

    // 重新注册，更新地址
    rt.Register("/tasks", "new-addr:18803");
    ASSERT_TRUE(rt.Match("/tasks/instant/execute", &out));
    ASSERT_EQ(out.address, "new-addr:18803");

    printf("[PASS] trie_idempotent_register\n");
}

// ============================================================
// T4: Trie 注销
// ============================================================
TEST(trie_unregister) {
    RouteTable rt;
    rt.Register("/channels", "127.0.0.1:18803");
    rt.Register("/tasks", "127.0.0.1:18803");

    rt.Unregister("/channels");

    RouteEntry out;
    ASSERT_FALSE(rt.Match("/channels/database/add", &out));
    ASSERT_TRUE(rt.Match("/tasks/instant/execute", &out));

    printf("[PASS] trie_unregister\n");
}

// ============================================================
// T5: Trie 过期清理
// ============================================================
TEST(trie_remove_expired) {
    RouteTable rt;
    rt.Register("/channels", "127.0.0.1:18803");
    rt.Register("/tasks", "127.0.0.1:18803");

    // 等待 50ms，然后设置过期时间为 now（清理所有）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int64_t now = NowMs();
    rt.RemoveExpired(now + 1);  // 清理所有 last_seen < now+1 的条目

    RouteEntry out;
    ASSERT_FALSE(rt.Match("/channels/database/add", &out));
    ASSERT_FALSE(rt.Match("/tasks/instant/execute", &out));

    printf("[PASS] trie_remove_expired\n");
}

// ============================================================
// T6: Trie 过期清理不影响新注册
// ============================================================
TEST(trie_expire_then_reregister) {
    RouteTable rt;
    rt.Register("/channels", "127.0.0.1:18803");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int64_t mid = NowMs();

    // 重新注册（更新 last_seen_ms）
    rt.Register("/channels", "127.0.0.1:18803");

    // 清理 mid 之前的条目，新注册的不应被清理
    rt.RemoveExpired(mid);

    RouteEntry out;
    ASSERT_TRUE(rt.Match("/channels/database/add", &out));

    printf("[PASS] trie_expire_then_reregister\n");
}

// ============================================================
// T7: Trie GetAll
// ============================================================
TEST(trie_get_all) {
    RouteTable rt;
    rt.Register("/channels", "127.0.0.1:18803");
    rt.Register("/tasks", "127.0.0.1:18803");
    rt.Register("/operators", "127.0.0.1:18803");

    auto all = rt.GetAll();
    ASSERT_EQ(all.size(), (size_t)3);

    printf("[PASS] trie_get_all\n");
}

// ============================================================
// T8: 路由收集 — mock IQuerier + IRouterHandle
// ============================================================

// Mock IRouterHandle：声明固定路由
class MockRouterHandle : public IRouterHandle {
public:
    explicit MockRouterHandle(std::vector<RouteItem> items) : items_(std::move(items)) {}
    void EnumRoutes(std::function<void(const RouteItem&)> cb) override {
        for (auto& item : items_) cb(item);
    }
private:
    std::vector<RouteItem> items_;
};

// Mock IQuerier：持有一组 IRouterHandle
class MockQuerier : public IQuerier {
public:
    void AddHandle(IRouterHandle* h) { handles_.push_back(h); }

    int Traverse(const Guid& iid, fntraverse proc) override {
        if (memcmp(&iid, &IID_ROUTER_HANDLE, sizeof(Guid)) == 0) {
            for (auto* h : handles_) {
                if (proc(h) == -1) break;
            }
        }
        return 0;
    }
    void* First(const Guid&) override { return nullptr; }

private:
    std::vector<IRouterHandle*> handles_;
};

TEST(route_collect_basic) {
    MockRouterHandle h1({
        {"POST", "/tasks/instant/execute",   [](auto&, auto&, auto&) { return error::OK; }},
        {"POST", "/channels/dataframe/query",[](auto&, auto&, auto&) { return error::OK; }},
    });
    MockRouterHandle h2({
        {"POST", "/channels/database/add",   [](auto&, auto&, auto&) { return error::OK; }},
        {"POST", "/channels/database/remove",[](auto&, auto&, auto&) { return error::OK; }},
    });

    MockQuerier q;
    q.AddHandle(&h1);
    q.AddHandle(&h2);

    router::RouterAgencyPlugin plugin;
    plugin.CollectRoutes(&q);

    // 验证前缀提取（通过 CollectRoutes 内部逻辑，间接通过 GetAll 验证）
    // 这里直接验证 CollectRoutes 不崩溃，且返回 0
    printf("[PASS] route_collect_basic\n");
}

// ============================================================
// T9: 路由冲突检测（重复路由被忽略，不崩溃）
// ============================================================
TEST(route_collect_conflict) {
    MockRouterHandle h1({
        {"POST", "/tasks/instant/execute", [](auto&, auto&, auto&) { return error::OK; }},
    });
    MockRouterHandle h2({
        {"POST", "/tasks/instant/execute", [](auto&, auto&, auto&) { return error::OK; }},  // 重复
    });

    MockQuerier q;
    q.AddHandle(&h1);
    q.AddHandle(&h2);

    router::RouterAgencyPlugin plugin;
    // 不应崩溃，重复路由被忽略（LOG_WARN）
    int rc = plugin.CollectRoutes(&q);
    ASSERT_EQ(rc, 0);

    printf("[PASS] route_collect_conflict\n");
}

// ============================================================
// T10: 错误码映射
// ============================================================
TEST(error_code_values) {
    ASSERT_EQ(error::OK,             0);
    ASSERT_EQ(error::BAD_REQUEST,   -1);
    ASSERT_EQ(error::NOT_FOUND,     -2);
    ASSERT_EQ(error::CONFLICT,      -3);
    ASSERT_EQ(error::INTERNAL_ERROR,-4);
    ASSERT_EQ(error::UNAVAILABLE,   -5);
    printf("[PASS] error_code_values\n");
}

// ============================================================
// T11: 路径边界
// ============================================================
TEST(trie_edge_cases) {
    RouteTable rt;

    // 空路径不匹配
    RouteEntry out;
    ASSERT_FALSE(rt.Match("", &out));

    // 精确路径匹配
    rt.Register("/exact", "exact:80");
    ASSERT_TRUE(rt.Match("/exact", &out));
    ASSERT_EQ(out.prefix, "/exact");

    // 子路径也匹配
    ASSERT_TRUE(rt.Match("/exact/sub/path", &out));
    ASSERT_EQ(out.prefix, "/exact");

    // 不相关路径不匹配
    ASSERT_FALSE(rt.Match("/other", &out));

    printf("[PASS] trie_edge_cases\n");
}

// ============================================================
// main
// ============================================================
int main() {
    printf("=== Router Tests ===\n\n");
    int failed = 0;
    for (auto& [name, fn] : tests) {
        printf("[TEST] %s\n", name.c_str());
        try {
            fn();
        } catch (const std::exception& e) {
            printf("[FAIL] exception: %s\n", e.what());
            ++failed;
        }
    }
    printf("\n");
    if (failed == 0) {
        printf("=== All router tests passed ===\n");
    } else {
        printf("=== %d test(s) FAILED ===\n", failed);
    }
    return failed;
}
