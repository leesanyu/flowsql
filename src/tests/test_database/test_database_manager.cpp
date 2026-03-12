// test_database_manager.cpp — DatabasePlugin 持久化与动态管理单元测试
//
// 直接链接 flowsql_database，不走 PluginLoader
// 测试 AddChannel / RemoveChannel / UpdateChannel / LoadFromYaml / SaveToYaml
//
// 环境要求：可写的临时目录（/tmp）
//
#include <cassert>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <services/database/database_plugin.h>

using namespace flowsql;
using namespace flowsql::database;

static int g_passed = 0;

// 生成唯一临时文件路径
static std::string TmpYaml(const char* suffix) {
    return std::string("/tmp/flowsql_test_") + suffix + ".yml";
}

// 删除文件（忽略错误）
static void RemoveFile(const std::string& path) {
    remove(path.c_str());
}

// ============================================================
// M1: config_file 为空时 AddChannel 返回 -1
// ============================================================
void test_add_channel_no_config_file() {
    printf("[TEST] M1: AddChannel without config_file returns -1...\n");

    DatabasePlugin plugin;
    plugin.Load(nullptr);
    plugin.Start();

    int rc = plugin.AddChannel("type=sqlite;name=testdb;path=:memory:");
    assert(rc == -1);
    assert(strlen(plugin.LastError()) > 0);
    printf("  Correctly returned -1: %s\n", plugin.LastError());

    g_passed++;
    printf("[PASS] M1\n");
}

// ============================================================
// M2: AddChannel 成功写入 YAML
// ============================================================
void test_add_channel_persists() {
    printf("[TEST] M2: AddChannel persists to YAML...\n");

    std::string yml = TmpYaml("m2");
    RemoveFile(yml);

    DatabasePlugin plugin;
    plugin.Option(("config_file=" + yml).c_str());
    plugin.Load(nullptr);
    plugin.Start();

    int rc = plugin.AddChannel("type=sqlite;name=mydb;path=:memory:");
    assert(rc == 0);

    // 验证文件存在且包含 mydb
    std::ifstream f(yml);
    assert(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)), {});
    assert(content.find("mydb") != std::string::npos);
    assert(content.find("sqlite") != std::string::npos);
    printf("  YAML written, contains 'mydb' and 'sqlite'\n");

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M2\n");
}

// ============================================================
// M3: AddChannel 重复添加返回 -1
// ============================================================
void test_add_channel_duplicate() {
    printf("[TEST] M3: AddChannel duplicate returns -1...\n");

    std::string yml = TmpYaml("m3");
    RemoveFile(yml);

    DatabasePlugin plugin;
    plugin.Option(("config_file=" + yml).c_str());
    plugin.Load(nullptr);
    plugin.Start();

    assert(plugin.AddChannel("type=sqlite;name=mydb;path=:memory:") == 0);
    int rc = plugin.AddChannel("type=sqlite;name=mydb;path=/other.db");
    assert(rc == -1);
    assert(strlen(plugin.LastError()) > 0);
    printf("  Duplicate correctly rejected: %s\n", plugin.LastError());

    // 验证 YAML 中只有一条记录（第一次添加的）
    std::ifstream f(yml);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    assert(content.find(":memory:") != std::string::npos);
    assert(content.find("/other.db") == std::string::npos);
    printf("  YAML still contains only original entry\n");

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M3\n");
}

// ============================================================
// M4: RemoveChannel 成功，YAML 中不再有该通道
// ============================================================
void test_remove_channel() {
    printf("[TEST] M4: RemoveChannel removes from YAML...\n");

    std::string yml = TmpYaml("m4");
    RemoveFile(yml);

    DatabasePlugin plugin;
    plugin.Option(("config_file=" + yml).c_str());
    plugin.Load(nullptr);
    plugin.Start();

    assert(plugin.AddChannel("type=sqlite;name=mydb;path=:memory:") == 0);
    assert(plugin.RemoveChannel("sqlite", "mydb") == 0);

    // 验证 List 为空
    int count = 0;
    plugin.List([&](const char*, const char*, const char*) { ++count; });
    assert(count == 0);

    // 验证 YAML 中不再有 mydb
    std::ifstream f(yml);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    assert(content.find("mydb") == std::string::npos);
    printf("  Channel removed from memory and YAML\n");

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M4\n");
}

// ============================================================
// M5: RemoveChannel 不存在时返回 -1
// ============================================================
void test_remove_channel_not_found() {
    printf("[TEST] M5: RemoveChannel not found returns -1...\n");

    std::string yml = TmpYaml("m5");
    RemoveFile(yml);

    DatabasePlugin plugin;
    plugin.Option(("config_file=" + yml).c_str());
    plugin.Load(nullptr);
    plugin.Start();

    int rc = plugin.RemoveChannel("sqlite", "nonexistent");
    assert(rc == -1);
    printf("  Correctly returned -1: %s\n", plugin.LastError());

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M5\n");
}

// ============================================================
// M6: UpdateChannel 成功更新配置
// ============================================================
void test_update_channel() {
    printf("[TEST] M6: UpdateChannel updates config...\n");

    std::string yml = TmpYaml("m6");
    RemoveFile(yml);

    // 预先写入一个无关顶层节点，验证 UpdateChannel 不破坏它
    {
        std::ofstream pre(yml);
        pre << "other_section:\n  key: preserved_value\n";
    }

    DatabasePlugin plugin;
    plugin.Option(("config_file=" + yml).c_str());
    plugin.Load(nullptr);
    plugin.Start();

    assert(plugin.AddChannel("type=sqlite;name=mydb;path=:memory:") == 0);
    assert(plugin.UpdateChannel("type=sqlite;name=mydb;path=/new/path.db") == 0);

    // 验证 YAML 包含新路径，不含旧路径
    std::ifstream f(yml);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    assert(content.find("/new/path.db") != std::string::npos);
    assert(content.find(":memory:") == std::string::npos);
    // 验证无关节点仍然存在
    assert(content.find("preserved_value") != std::string::npos);
    printf("  Updated path found, old path gone, other_section preserved\n");

    // 验证内存中配置也已更新（通过 List 回调）
    bool found_new = false;
    plugin.List([&](const char*, const char*, const char* config_json) {
        if (config_json && std::string(config_json).find("/new/path.db") != std::string::npos)
            found_new = true;
    });
    assert(found_new);
    printf("  In-memory config also updated\n");

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M6\n");
}

// ============================================================
// M7: UpdateChannel 不存在时返回 -1
// ============================================================
void test_update_channel_not_found() {
    printf("[TEST] M7: UpdateChannel not found returns -1...\n");

    std::string yml = TmpYaml("m7");
    RemoveFile(yml);

    DatabasePlugin plugin;
    plugin.Option(("config_file=" + yml).c_str());
    plugin.Load(nullptr);
    plugin.Start();

    int rc = plugin.UpdateChannel("type=sqlite;name=nonexistent;path=:memory:");
    assert(rc == -1);
    printf("  Correctly returned -1: %s\n", plugin.LastError());

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M7\n");
}

// ============================================================
// M8: LoadFromYaml 重启恢复（模拟重启：新建 plugin 实例加载同一 YAML）
// ============================================================
void test_restart_recovery() {
    printf("[TEST] M8: restart recovery from YAML...\n");

    std::string yml = TmpYaml("m8");
    RemoveFile(yml);

    // 第一个实例：添加通道
    {
        DatabasePlugin plugin;
        plugin.Option(("config_file=" + yml).c_str());
        plugin.Load(nullptr);
        plugin.Start();
        assert(plugin.AddChannel("type=sqlite;name=db1;path=:memory:") == 0);
        assert(plugin.AddChannel("type=sqlite;name=db2;path=:memory:") == 0);
    }

    // 第二个实例：从 YAML 恢复
    {
        DatabasePlugin plugin2;
        plugin2.Option(("config_file=" + yml).c_str());
        plugin2.Load(nullptr);
        plugin2.Start();

        int count = 0;
        plugin2.List([&](const char* type, const char* name, const char*) {
            printf("  Recovered: %s.%s\n", type, name);
            ++count;
        });
        assert(count == 2);
        printf("  Recovered %d channels after restart\n", count);
    }

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M8\n");
}

// ============================================================
// M9: List 密码脱敏
// ============================================================
void test_list_password_masked() {
    printf("[TEST] M9: List masks password...\n");

    std::string yml = TmpYaml("m9");
    RemoveFile(yml);

    DatabasePlugin plugin;
    plugin.Option(("config_file=" + yml).c_str());
    plugin.Load(nullptr);
    plugin.Start();

    assert(plugin.AddChannel("type=sqlite;name=mydb;path=:memory:;password=secret123") == 0);

    bool found = false;
    plugin.List([&](const char*, const char*, const char* config_json) {
        if (!config_json) return;
        std::string j(config_json);
        // 密码不应出现明文
        assert(j.find("secret123") == std::string::npos);
        // 应出现脱敏标记
        assert(j.find("****") != std::string::npos);
        printf("  config_json: %s\n", config_json);
        found = true;
    });
    assert(found);

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M9\n");
}

// ============================================================
// M5（补充）: YAML 文件中密码以 ENC: 前缀加密存储
// ============================================================
void test_password_encrypted_in_yaml() {
    printf("[TEST] M5-ext: password encrypted in YAML with ENC: prefix...\n");

    std::string yml = TmpYaml("m5ext");
    RemoveFile(yml);

    DatabasePlugin plugin;
    plugin.Option(("config_file=" + yml).c_str());
    plugin.Load(nullptr);
    plugin.Start();

    assert(plugin.AddChannel("type=sqlite;name=mydb;path=:memory:;password=secret123") == 0);

    // 直接读取 YAML 文件内容
    std::ifstream f(yml);
    assert(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)), {});

    // 原始密码不应出现在文件中
    assert(content.find("secret123") == std::string::npos);
    // 应有 ENC: 前缀（AES-256-GCM 加密）
    assert(content.find("ENC:") != std::string::npos);
    printf("  YAML password field: ENC: prefix found, plaintext absent\n");

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M5-ext: password encrypted in YAML\n");
}

// ============================================================
// M8（补充）: AddChannel 缺少必要字段返回 -1，YAML 不被修改
// ============================================================
void test_add_channel_invalid_config() {
    printf("[TEST] M8-ext: AddChannel invalid config returns -1, YAML unchanged...\n");

    std::string yml = TmpYaml("m8ext");
    RemoveFile(yml);

    DatabasePlugin plugin;
    plugin.Option(("config_file=" + yml).c_str());
    plugin.Load(nullptr);
    plugin.Start();

    // 先添加一个合法通道，建立基准 YAML
    assert(plugin.AddChannel("type=sqlite;name=baseline;path=:memory:") == 0);
    std::ifstream f1(yml);
    std::string before((std::istreambuf_iterator<char>(f1)), {});

    // 缺少 name 字段
    int rc = plugin.AddChannel("type=sqlite;path=:memory:");
    assert(rc == -1);
    assert(strlen(plugin.LastError()) > 0);
    printf("  Missing name: %s\n", plugin.LastError());

    // 缺少 type 字段
    rc = plugin.AddChannel("name=mydb;path=:memory:");
    assert(rc == -1);
    printf("  Missing type: %s\n", plugin.LastError());

    // YAML 内容不应被修改（与添加非法配置前一致）
    std::ifstream f2(yml);
    std::string after((std::istreambuf_iterator<char>(f2)), {});
    assert(before == after);
    printf("  YAML unchanged after invalid AddChannel\n");

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M8-ext: AddChannel invalid config\n");
}

// ============================================================
// M9（补充）: 多线程并发 AddChannel/RemoveChannel，最终状态一致
// ============================================================
void test_concurrent_add_remove() {
    printf("[TEST] M9-ext: concurrent AddChannel/RemoveChannel...\n");

    std::string yml = TmpYaml("m9ext");
    RemoveFile(yml);

    DatabasePlugin plugin;
    plugin.Option(("config_file=" + yml).c_str());
    plugin.Load(nullptr);
    plugin.Start();

    const int N = 10;
    std::atomic<int> add_ok{0}, remove_ok{0};
    std::vector<std::thread> threads;

    // N 个线程各自 Add 一个唯一通道
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&plugin, &add_ok, i]() {
            std::string cfg = "type=sqlite;name=ch" + std::to_string(i) + ";path=:memory:";
            if (plugin.AddChannel(cfg.c_str()) == 0) add_ok++;
        });
    }
    for (auto& t : threads) t.join();
    threads.clear();

    assert(add_ok == N);
    printf("  %d channels added concurrently\n", add_ok.load());

    // N 个线程各自 Remove 自己的通道
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&plugin, &remove_ok, i]() {
            std::string name = "ch" + std::to_string(i);
            if (plugin.RemoveChannel("sqlite", name.c_str()) == 0) remove_ok++;
        });
    }
    for (auto& t : threads) t.join();

    assert(remove_ok == N);
    printf("  %d channels removed concurrently\n", remove_ok.load());

    // 验证内存和 YAML 最终状态一致：均为空
    int count = 0;
    plugin.List([&](const char*, const char*, const char*) { ++count; });
    assert(count == 0);

    std::ifstream f(yml);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    // database_channels 节点应为空（无 ch0~ch9）
    for (int i = 0; i < N; ++i) {
        assert(content.find("ch" + std::to_string(i)) == std::string::npos);
    }
    printf("  Memory and YAML both empty after concurrent remove\n");

    RemoveFile(yml);
    g_passed++;
    printf("[PASS] M9-ext: concurrent AddChannel/RemoveChannel\n");
}

// ============================================================
// main
// ============================================================
int main() {
    printf("=== DatabasePlugin Manager Tests ===\n\n");

    test_add_channel_no_config_file();
    test_add_channel_persists();
    test_add_channel_duplicate();
    test_remove_channel();
    test_remove_channel_not_found();
    test_update_channel();
    test_update_channel_not_found();
    test_restart_recovery();
    test_list_password_masked();
    test_password_encrypted_in_yaml();
    test_add_channel_invalid_config();
    test_concurrent_add_remove();

    printf("\n=== Results: %d/12 passed ===\n", g_passed);
    return (g_passed == 12) ? 0 : 1;
}
