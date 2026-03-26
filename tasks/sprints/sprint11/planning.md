# Sprint 11 规划

## Sprint 信息

- **Sprint 周期**：Sprint 11
- **开始日期**：2026-03-26
- **预计工作量**：待评估
- **Sprint 目标**：PostgreSQL 驱动支持 + C++ 算子插件基础能力（编译 Sample + 动态激活/去激活）

---

## Sprint 目标

### 主要目标

1. **Story 4.2**：PostgreSQL 驱动支持（基于 libpq，复用现有 Traits + RelationDbSessionBase + ConnectionPool 架构）
2. **Story 12.1**：C++ 算子插件编译工程 Sample（独立 CMake 工程，示例算子实现）
3. **Story 12.2**：C++ 算子插件动态激活与去激活（CppOperatorPlugin + HTTP API）

### 成功标准

- PostgreSQL 数据库通道可在 Web UI 中添加、预览、查询
- C++ 算子 .so 可通过 HTTP API 在运行时加载/卸载，并注册到 CatalogPlugin
- Sample 工程可独立编译，文档清晰

---

## Story 详情

### Story 4.2: PostgreSQL 驱动支持
**来源**: Epic 4 | **状态**: ✅ 已完成（2026-03-26）
**估算**: 4 天

**验收标准**:
- 实现 PostgresDriver（基于 libpq）
- 支持连接池管理（复用 ConnectionPool<PGconn*>）
- 本 Story 使用 `PQexec` 简单路径，不以 prepared statement 作为验收前置
- 支持事务控制（BEGIN/COMMIT/ROLLBACK）
- 支持 PostgreSQL 常用类型映射（INT32/INT64/FLOAT/DOUBLE/STRING/BOOL/BINARY）
- Postgres `Ping()` 使用主动探测（`SELECT 1`），可识别网络中断
- DatabasePlugin 可创建 postgres 通道（`Get("postgres", name)` 可用）
- Web 浏览接口支持 postgres（tables/describe/preview）
- Web 浏览接口支持可选 `schema`（postgres 默认 `public`）
- DatabasePlugin 通过会话工厂能力接口创建 session，不再依赖具体驱动 `dynamic_cast` 链
- Browse SQL 构造通过方言 helper/接口统一（tables/describe/quote）
- 驱动测试 + 插件层 E2E 测试通过（包含并发场景）
- 测试目标显式 `-UNDEBUG`

**任务分解**:
- [x] 定义 PostgresTraits（ConnectionType=PGconn*, StatementType, ResultType=PGresult*）
- [x] 实现 PostgresDriver（Connect/Disconnect/Ping，基于 libpq）
- [x] 实现 PostgresSession（`PQexec` 简单路径 + 模板钩子兼容）
- [x] 实现 PostgresResultSet（含 `FieldLength()`，支持 bytea 解码）
- [x] 实现 PostgresBatchWriter（BOOL/BINARY 安全写入）
- [x] 在 `relation_adapters.h` 增加 BOOL 读取分支，保证 boolean schema 可消费
- [x] 引入 `IDbSessionFactoryProvider` 能力接口并让四类驱动实现
- [x] 在 DatabasePlugin 的 `CreateDriver()` 中注册 "postgres" 类型
- [x] 将 DatabasePlugin 的 session 创建改为基于 `IDbSessionFactoryProvider`（移除具体驱动 `dynamic_cast` 链）
- [x] 在 DatabasePlugin 的 `HandleTables/HandleDescribe/QuoteTableName` 中增加 postgres 分支
- [x] 为 Browse SQL 增加统一 helper（`BuildTablesSql/BuildDescribeSql`）
- [x] 在 DatabasePlugin 中新增 SQL 字面量转义辅助函数（用于 describe 防注入）
- [x] 为 Browse API 增加可选 `schema` 参数（postgres 默认 `public`）
- [x] CMakeLists.txt 添加 libpq 依赖
- [x] 新增 `src/tests/test_database/test_postgres.cpp`（连接/事务/类型矩阵/并发）
- [x] 扩展 `src/tests/test_database/test_plugin_e2e.cpp` postgres 场景（生产路径）
- [x] 新增 `Ping()` 主动探测与 `schema` 参数测试用例
- [x] 更新 `src/tests/test_database/CMakeLists.txt`，新增 `test_postgres` 并显式 `-UNDEBUG`

---

### Story 12.1: C++ 算子插件编译工程 Sample
**来源**: Epic 12 | **状态**: 🚧 进行中
**估算**: 1 天

**验收标准**:
- 提供 `samples/cpp_operator/` 独立工程
- 实现一个示例算子（如列统计：count/min/max/mean）
- CMakeLists.txt 可独立编译出 .so
- 文档说明接口约定和编译步骤
- 编译产物可被系统正确加载执行

**任务分解**:
- [ ] 确定 C++ 算子插件的导出接口约定（工厂函数签名）
- [ ] 创建 `samples/cpp_operator/` 工程骨架
- [ ] 实现示例算子（ColumnStatsOperator）
- [ ] 编写 CMakeLists.txt（链接 flowsql 头文件，输出 .so）
- [ ] 编写 README 说明开发步骤

---

### Story 12.2: C++ 算子插件动态激活与去激活
**来源**: Epic 12 | **状态**: 🚧 进行中
**估算**: 3 天

**验收标准**:
- 新增 CppOperatorPlugin（IPlugin + IRouterHandle）
- `POST /operators/cpp/activate` — 指定 .so 路径，运行时 dlopen 加载
- `POST /operators/cpp/deactivate` — 指定算子标识，安全 dlclose 卸载
- `GET /operators/cpp/list` — 列出已激活的 C++ 算子
- 激活后算子注册到 IOperatorCatalog
- 去激活时等待执行中任务完成后再卸载
- API 风格与 Python 算子保持一致

**任务分解**:
- [ ] 定义 C++ 算子插件导出接口（ICppOperatorFactory 或工厂函数）
- [ ] 实现 CppOperatorPlugin（Load/Unload/Start/Stop + EnumRoutes）
- [ ] 实现 HandleActivate（dlopen + 工厂函数调用 + 注册到 CatalogPlugin）
- [ ] 实现 HandleDeactivate（引用计数 + 安全 dlclose）
- [ ] 实现 HandleList（遍历已加载的 C++ 算子）
- [ ] 集成到 gateway.yaml 插件列表
- [ ] 与 Sample 工程联调验证

---

## 风险与依赖

| 风险 | 缓解措施 |
|------|---------|
| libpq 在构建环境中未安装 | 提前确认 apt install libpq-dev |
| PostgreSQL bytea 文本格式与 Arrow binary 对齐风险 | 在 ResultSet 中统一 hex 解码，增加 binary round-trip 测试 |
| 会话工厂/方言抽象改造影响现有 sqlite/mysql/clickhouse 行为 | 增加三库回归（tables/describe/preview + CreateReader/CreateWriter） |
| dlclose 导致 vtable 悬空 | 去激活前确保无活跃 IOperator 引用 |
| C++ 算子 ABI 兼容性 | 限定编译器版本，接口用纯虚基类隔离 |

---

## 技术设计要点（待讨论）

- C++ 算子插件的导出函数签名（`create_operator()` / `destroy_operator()`？）
- 是否需要支持一个 .so 导出多个算子？
- 去激活的引用计数策略（立即卸载 vs 等待空闲）
- PostgreSQL schema 参数结论：本 Story 暴露可选 `schema` 请求参数；缺省 `public`
- `IDbSessionFactoryProvider` 放在 `idb_driver.h` 还是独立 capability 头文件

---

# Story 12 实现计划：C++ 算子插件

> **目标**：实现 C++ 算子插件的编译 Sample 工程（Story 12.1）和动态激活/去激活（Story 12.2）。
>
> **架构**：CppOperatorPlugin（IPlugin + IRouterHandle）管理用户 .so 的 dlopen/dlclose，
> 通过 IOperatorRegistry 注册工厂函数，SchedulerPlugin 透明获取算子实例。
>
> **技术栈**：C++17、dlopen/dlclose、std::atomic、shared_ptr 引用计数、CMake

---

## 任务 1：接口扩展 — IOperatorRegistry::Unregister + IOperatorCatalog::SetActive

**文件**：
- 修改：`src/framework/interfaces/ioperator_registry.h`
- 修改：`src/framework/interfaces/ioperator_catalog.h`
- 修改：`src/services/catalog/catalog_plugin.h`
- 修改：`src/services/catalog/catalog_plugin.cpp`

- [ ] **步骤 1**：在 `ioperator_registry.h` 的 `IOperatorRegistry` 接口末尾新增纯虚方法：

```cpp
// 注销算子；不存在时返回 -1
virtual int Unregister(const char* name) = 0;
```

- [ ] **步骤 2**：在 `ioperator_catalog.h` 的 `IOperatorCatalog` 接口末尾新增纯虚方法：

```cpp
virtual int SetActive(const std::string& catelog,
                      const std::string& name,
                      bool active) = 0;
```

- [ ] **步骤 3**：在 `catalog_plugin.h` 的 `IOperatorRegistry` 实现区声明 `Unregister`，
  在 `IOperatorCatalog` 实现区声明 `SetActive`。

- [ ] **步骤 4**：在 `catalog_plugin.cpp` 实现 `Unregister`（加锁 erase `op_factories_`）：

```cpp
int CatalogPlugin::Unregister(const char* name) {
    if (!name || !*name) return -1;
    std::lock_guard<std::mutex> lock(mu_);
    return op_factories_.erase(name) > 0 ? 0 : -1;
}
```

- [ ] **步骤 5**：在 `catalog_plugin.cpp` 实现 `SetActive`（包装现有私有方法）：

```cpp
int CatalogPlugin::SetActive(const std::string& catelog,
                              const std::string& name, bool active) {
    return SetOperatorActive(catelog, name, active ? 1 : 0);
}
```

- [ ] **步骤 6**：在 `CatalogPlugin::Load()` 中为内置算子同时注册双 key（向后兼容）：

```cpp
// 原有 key（向后兼容）
Register("passthrough", []() -> IOperator* { return new PassthroughOperator(); });
Register("concat",      []() -> IOperator* { return new ConcatOperator(); });
Register("hstack",      []() -> IOperator* { return new HstackOperator(); });
// 新增 "builtin.name" 格式 key（供 SchedulerPlugin 统一查找路径使用）
Register("builtin.passthrough", []() -> IOperator* { return new PassthroughOperator(); });
Register("builtin.concat",      []() -> IOperator* { return new ConcatOperator(); });
Register("builtin.hstack",      []() -> IOperator* { return new HstackOperator(); });
```

- [ ] **步骤 7**：编译验证（无新增测试，接口变更不破坏现有测试）：

```bash
cmake --build build --target flowsql_catalog test_builtin -j4 2>&1 | tail -20
```

预期：编译通过，`test_builtin` 运行无 FAIL。

- [ ] **步骤 8**：Commit

```bash
git add src/framework/interfaces/ioperator_registry.h \
        src/framework/interfaces/ioperator_catalog.h \
        src/services/catalog/catalog_plugin.h \
        src/services/catalog/catalog_plugin.cpp
git commit -m "feat(catalog): add Unregister/SetActive interfaces for C++ operator plugin support"
```

---

## 任务 2：SchedulerPlugin — FindOperator 扩展支持非 builtin catelog

**文件**：
- 修改：`src/services/scheduler/scheduler_plugin.cpp`（FindOperator，约第 345-350 行）

- [ ] **步骤 1**：将 FindOperator 第 3 步从仅查 builtin 改为查所有 catelog：

```cpp
// 修改前
if (IEquals(catelog, "builtin") && op_registry) {
    IOperator* op = op_registry->Create(name.c_str());
    if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
}

// 修改后
if (op_registry) {
    // 先用 "catelog.name" 格式查（C++ 动态算子 + 内置算子双 key）
    std::string key = catelog + "." + name;
    IOperator* op = op_registry->Create(key.c_str());
    if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    // 向后兼容：builtin 算子旧 key（纯 name）仍可查到
    if (IEquals(catelog, "builtin")) {
        op = op_registry->Create(name.c_str());
        if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    }
}
```

- [ ] **步骤 2**：编译并运行 scheduler E2E 测试验证向后兼容：

```bash
cmake --build build --target flowsql_scheduler test_scheduler_e2e -j4 2>&1 | tail -10
./build/output/test_scheduler_e2e
```

预期：所有现有测试通过，`builtin.passthrough` 和 `passthrough` 均可查到。

- [ ] **步骤 3**：Commit

```bash
git add src/services/scheduler/scheduler_plugin.cpp
git commit -m "feat(scheduler): extend FindOperator to support non-builtin catelog via catelog.name key"
```

---

## 任务 3：CppOperatorPlugin — 核心实现

**文件**：
- 新增：`src/services/cpp_operator/cpp_operator_plugin.h`
- 新增：`src/services/cpp_operator/cpp_operator_proxy.h`
- 新增：`src/services/cpp_operator/cpp_operator_plugin.cpp`
- 新增：`src/services/cpp_operator/CMakeLists.txt`
- 修改：`src/CMakeLists.txt`

- [ ] **步骤 1**：新增 `cpp_operator_plugin.h`，定义 `LoadedSo` 和 `CppOperatorPlugin`：

```cpp
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <dlfcn.h>

#include <common/iplugin.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ioperator_registry.h>
#include <framework/interfaces/irouter_handle.h>

namespace flowsql {
namespace cpp_operator {

struct LoadedSo {
    void*  handle = nullptr;
    std::string path;
    std::atomic<int>  active_count{0};
    std::atomic<bool> pending_unload{false};
    int        (*count_fn)()         = nullptr;
    IOperator* (*create_fn)(int)     = nullptr;
    void       (*destroy_fn)(IOperator*) = nullptr;
    std::vector<std::string> operator_keys;  // "catelog.name" 格式

    ~LoadedSo() {
        if (handle) { dlclose(handle); handle = nullptr; }
    }
};

class __attribute__((visibility("default"))) CppOperatorPlugin
    : public IPlugin, public IRouterHandle {
 public:
    int Option(const char* arg) override { return 0; }
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override { return 0; }
    int Stop()  override { return 0; }
    void EnumRoutes(std::function<void(const RouteItem&)> cb) override;

 private:
    int32_t HandleActivate(const std::string& uri,
                           const std::string& req, std::string& rsp);
    int32_t HandleDeactivate(const std::string& uri,
                             const std::string& req, std::string& rsp);
    int32_t HandleList(const std::string& uri,
                       const std::string& req, std::string& rsp);

    IOperatorRegistry* registry_ = nullptr;
    IOperatorCatalog*  catalog_  = nullptr;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<LoadedSo>> loaded_sos_;
};

}  // namespace cpp_operator
}  // namespace flowsql
```

- [ ] **步骤 2**：新增 `cpp_operator_proxy.h`（header-only，只在 cpp_operator_plugin.cpp 中 include）：

```cpp
#pragma once
#include <memory>
#include <framework/interfaces/ioperator.h>
#include <services/cpp_operator/cpp_operator_plugin.h>

namespace flowsql {
namespace cpp_operator {

// CppOperatorProxy：转发所有 IOperator 方法到用户 .so 的实现
// 构造时 active_count++，析构时调用 destroy_fn 后 active_count--
// 持有 shared_ptr<LoadedSo>，保证 dlclose 在所有 Proxy 析构后才执行
class CppOperatorProxy : public IOperator {
 public:
    CppOperatorProxy(IOperator* impl, std::shared_ptr<LoadedSo> so)
        : impl_(impl), so_(std::move(so)) {
        so_->active_count.fetch_add(1, std::memory_order_acq_rel);
    }

    ~CppOperatorProxy() override {
        so_->destroy_fn(impl_);
        impl_ = nullptr;
        so_->active_count.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::string Catelog()     override { return impl_->Catelog(); }
    std::string Name()        override { return impl_->Name(); }
    std::string Description() override { return impl_->Description(); }
    OperatorPosition Position() override { return impl_->Position(); }
    int Work(IChannel* in, IChannel* out) override {
        return impl_->Work(in, out);
    }
    int Work(Span<IChannel*> inputs, IChannel* out) override {
        return impl_->Work(inputs, out);
    }
    int Configure(const char* key, const char* value) override {
        return impl_->Configure(key, value);
    }
    std::string LastError() override { return impl_->LastError(); }

 private:
    IOperator* impl_;
    std::shared_ptr<LoadedSo> so_;
};

}  // namespace cpp_operator
}  // namespace flowsql
```

- [ ] **步骤 3**：新增 `cpp_operator_plugin.cpp`，实现 Load/Unload/EnumRoutes/HandleActivate/HandleDeactivate/HandleList：

Load() 获取 IOperatorRegistry 和 IOperatorCatalog 指针；
Unload() 遍历 loaded_sos_ 注销所有算子并清空 map；
EnumRoutes() 注册三条路由；
HandleActivate 按设计文档流程实现（dlopen 在锁外，二次检查在锁内）；
HandleDeactivate 按设计文档流程实现（pending_unload + active_count 检查）；
HandleList 返回已加载 .so 列表及其算子。

- [ ] **步骤 4**：新增 `CMakeLists.txt`：

```cmake
project(flowsql_cpp_operator)
file(GLOB DIR_SRCS *.cpp)
add_library(${PROJECT_NAME} SHARED ${DIR_SRCS})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(${PROJECT_NAME} arrow rapidjson)
add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)
set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
```

- [ ] **步骤 5**：在 `src/CMakeLists.txt` 末尾添加：

```cmake
add_subdirectory(${CMAKE_SOURCE_DIR}/services/cpp_operator ${CMAKE_BINARY_DIR}/cpp_operator)
```

- [ ] **步骤 6**：编译验证：

```bash
cmake --build build --target flowsql_cpp_operator -j4 2>&1 | tail -10
```

预期：编译通过，`build/output/libflowsql_cpp_operator.so` 生成。

- [ ] **步骤 7**：Commit

```bash
git add src/services/cpp_operator/ src/CMakeLists.txt
git commit -m "feat(cpp_operator): implement CppOperatorPlugin with dlopen/dlclose lifecycle management"
```

---

## 任务 4：WebServer — 添加 /api/operators/cpp/* 代理路由

**文件**：
- 修改：`src/services/web/web_server.cpp`

- [ ] **步骤 1**：参考现有 `/api/operators/python/*` 代理路由的实现方式，
  在 `web_server.cpp` 中添加三条 cpp 路由：

```cpp
// POST /api/operators/cpp/activate
server_.Post("/api/operators/cpp/activate", [this](const httplib::Request& req, httplib::Response& rsp) {
    std::string body;
    int32_t rc = ProxyPostJson(router_host_, router_port_, "/operators/cpp/activate", req.body, &body);
    rsp.set_content(body, "application/json");
    rsp.status = RcToHttpStatus(rc);
});
// POST /api/operators/cpp/deactivate — 同上
// GET  /api/operators/cpp/list — 使用 ProxyGetJson
```

- [ ] **步骤 2**：编译验证：

```bash
cmake --build build --target flowsql_web -j4 2>&1 | tail -5
```

- [ ] **步骤 3**：Commit

```bash
git add src/services/web/web_server.cpp
git commit -m "feat(web): add /api/operators/cpp/* proxy routes"
```

---

## 任务 5：deploy-single.yaml — 接入插件

**文件**：
- 修改：`config/deploy-single.yaml`

- [ ] **步骤 1**：在插件列表中添加 `libflowsql_cpp_operator.so`（无需 option）：

```yaml
- libflowsql_cpp_operator.so
```

- [ ] **步骤 2**：Commit

```bash
git add config/deploy-single.yaml
git commit -m "chore(config): add libflowsql_cpp_operator.so to deploy-single.yaml"
```

---

## 任务 6：测试 fixture .so 工程

**文件**：
- 新增：`src/tests/test_cpp_operator/fixture_operator.cpp`
- 新增：`src/tests/test_cpp_operator/fixture_bad_abi.cpp`

- [ ] **步骤 1**：新增 `fixture_operator.cpp`（正常 fixture，含可控阻塞点）：

```cpp
#include <atomic>
#include <future>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ichannel.h>

// 测试注入的阻塞点：Work() 执行时等待此 promise 被 set_value
std::promise<void>* g_work_block = nullptr;

class FixtureOperator : public flowsql::IOperator {
 public:
    std::string Catelog()     override { return "fixture"; }
    std::string Name()        override { return "blocking"; }
    std::string Description() override { return "test fixture"; }
    flowsql::OperatorPosition Position() override {
        return flowsql::OperatorPosition::DATA;
    }
    int Work(flowsql::IChannel* in, flowsql::IChannel* out) override {
        if (g_work_block) g_work_block->get_future().wait();
        return 0;
    }
    int Configure(const char*, const char*) override { return 0; }
};

extern "C" {
    int flowsql_abi_version()   { return 1; }
    int flowsql_operator_count() { return 1; }
    flowsql::IOperator* flowsql_create_operator(int index) {
        if (index != 0) return nullptr;
        try { return new FixtureOperator(); } catch (...) { return nullptr; }
    }
    void flowsql_destroy_operator(flowsql::IOperator* op) { delete op; }
}
```

- [ ] **步骤 2**：新增 `fixture_bad_abi.cpp`（ABI 版本不匹配）：

```cpp
#include <framework/interfaces/ioperator.h>
extern "C" {
    int flowsql_abi_version()    { return 999; }  // 故意不匹配
    int flowsql_operator_count() { return 0; }
    flowsql::IOperator* flowsql_create_operator(int) { return nullptr; }
    void flowsql_destroy_operator(flowsql::IOperator*) {}
}
```

- [ ] **步骤 3**：编译验证（CMakeLists.txt 在任务 7 中添加，此步骤先确认源码无语法错误）。

---

## 任务 7：测试实现

**文件**：
- 新增：`src/tests/test_cpp_operator/test_cpp_operator.cpp`
- 新增：`src/tests/test_cpp_operator/CMakeLists.txt`
- 修改：`src/CMakeLists.txt`（添加 test_cpp_operator subdirectory）

- [ ] **步骤 1**：新增 `CMakeLists.txt`：

```cmake
project(test_cpp_operator)

# fixture .so（正常）
add_library(fixture_operator SHARED fixture_operator.cpp)
target_include_directories(fixture_operator PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(fixture_operator PRIVATE -fvisibility=hidden)
set_target_properties(fixture_operator PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

# fixture .so（ABI 不匹配）
add_library(fixture_bad_abi SHARED fixture_bad_abi.cpp)
target_include_directories(fixture_bad_abi PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(fixture_bad_abi PRIVATE -fvisibility=hidden)
set_target_properties(fixture_bad_abi PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

# 测试主程序
add_executable(${PROJECT_NAME} test_cpp_operator.cpp)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(${PROJECT_NAME} arrow rapidjson sqlite)
add_dependencies(${PROJECT_NAME} flowsql_common flowsql_catalog flowsql_cpp_operator
                 fixture_operator fixture_bad_abi)
target_link_libraries(${PROJECT_NAME} flowsql_common flowsql_catalog flowsql_cpp_operator)
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
# 将 fixture .so 路径注入测试（避免硬编码）
target_compile_definitions(${PROJECT_NAME} PRIVATE
    FIXTURE_SO_PATH="$<TARGET_FILE:fixture_operator>"
    FIXTURE_BAD_ABI_SO_PATH="$<TARGET_FILE:fixture_bad_abi>"
)
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
```

- [ ] **步骤 2**：编写测试用例 1-4（激活/重复激活/ABI 不匹配/不存在路径）：

```cpp
// 测试框架：直接实例化 CatalogPlugin + CppOperatorPlugin，调用生命周期方法
// 参考 test_builtin.cpp 的 ASSERT_EQ/ASSERT_TRUE 宏和 MakeTempDir 模式
void test_activate_ok() { ... }
void test_activate_duplicate() { ... }
void test_activate_bad_abi() { ... }
void test_activate_not_found() { ... }
```

- [ ] **步骤 3**：运行测试 1-4，确认通过：

```bash
cmake --build build --target test_cpp_operator -j4 && ./build/output/test_cpp_operator
```

- [ ] **步骤 4**：编写测试用例 5-9（去激活/不存在/向后兼容/E2E/去激活后 Create 返回 nullptr）。

- [ ] **步骤 5**：运行测试 5-9，确认通过。

- [ ] **步骤 6**：编写测试用例 10-12（并发竞态/dlclose 安全性）：

```cpp
void test_deactivate_while_active() {
    // 通过 g_work_block 阻塞 Work()，确保 active_count > 0 时发起 deactivate
    std::promise<void> block;
    g_work_block = &block;
    std::thread worker([&]{ /* Create + Work */ });
    // 等 active_count > 0
    while (so->active_count.load() == 0) std::this_thread::yield();
    // 发起 deactivate，预期 409
    assert(HandleDeactivate(...) == 409);
    // 放行 Work
    block.set_value();
    worker.join();
    // 再次 deactivate，预期 200
    assert(HandleDeactivate(...) == 200);
}
```

- [ ] **步骤 7**：运行全部测试，确认通过：

```bash
./build/output/test_cpp_operator
```

预期：12 个测试全部 PASS，无 crash，无 ASAN 报告（如启用 AddressSanitizer）。

- [ ] **步骤 8**：在 `src/CMakeLists.txt` 添加：

```cmake
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/test_cpp_operator ${CMAKE_BINARY_DIR}/test_cpp_operator)
```

- [ ] **步骤 9**：Commit

```bash
git add src/tests/test_cpp_operator/ src/CMakeLists.txt
git commit -m "test(cpp_operator): add full test suite with fixture .so and concurrency tests"
```

---

## 任务 8：Sample 工程（Story 12.1）

**文件**：
- 新增：`samples/cpp_operator/CMakeLists.txt`
- 新增：`samples/cpp_operator/include/flowsql_operator_sdk.h`
- 新增：`samples/cpp_operator/src/sample_operators.cpp`
- 新增：`samples/cpp_operator/README.md`

- [ ] **步骤 1**：新增 `flowsql_operator_sdk.h`（直接 include 主程序接口，不重复定义）：

```cpp
#pragma once
// 直接复用主程序接口定义，保证 vtable 布局一致
#include <common/define.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ichannel.h>
#include <common/span.h>

#define FLOWSQL_ABI_VERSION 1

extern "C" {
    int flowsql_abi_version();
    int flowsql_operator_count();
    flowsql::IOperator* flowsql_create_operator(int index);
    void flowsql_destroy_operator(flowsql::IOperator* op);
}
```

- [ ] **步骤 2**：新增 `sample_operators.cpp`，实现 `ColumnStatsOperator`（读取数值列，输出 count/min/max/mean）。

- [ ] **步骤 3**：新增 `CMakeLists.txt`（独立工程，通过 `FLOWSQL_SRC_INCLUDE` 指向主程序 src/）。

- [ ] **步骤 4**：独立编译验证：

```bash
cd samples/cpp_operator
cmake -B build -DFLOWSQL_SRC_INCLUDE=/mnt/d/working/flowSQL/src
cmake --build build
ls build/libsample_cpp_operator.so
```

预期：`.so` 生成，`nm -D build/libsample_cpp_operator.so | grep flowsql_` 显示 4 个导出符号。

- [ ] **步骤 5**：Commit

```bash
git add samples/cpp_operator/
git commit -m "feat(sample): add cpp_operator sample project with ColumnStatsOperator"
```

---

## 任务 9：联调验证

- [ ] **步骤 1**：启动服务（使用 deploy-single.yaml），确认 `libflowsql_cpp_operator.so` 加载成功。

- [ ] **步骤 2**：激活 sample .so：

```bash
curl -X POST http://localhost:18803/operators/cpp/activate \
  -H "Content-Type: application/json" \
  -d '{"path":"/path/to/libsample_cpp_operator.so"}'
```

预期：返回 `{"code":0,"data":{"operators":["fixture.column_stats"]}}`。

- [ ] **步骤 3**：查询算子列表，确认 `fixture.column_stats` 出现在 `/operators/query` 结果中。

- [ ] **步骤 4**：提交一个使用该算子的任务，确认执行成功。

- [ ] **步骤 5**：去激活 .so，确认算子从列表消失，再次提交任务返回"算子不存在"错误。
