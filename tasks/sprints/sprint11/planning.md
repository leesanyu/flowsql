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
3. **Story 12.2**：C++ 算子插件动态激活与去激活（BinAddonHostPlugin + HTTP API）

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
**来源**: Epic 12 | **状态**: ✅ 已完成（2026-03-28）
**估算**: 1 天

**验收标准**:
- 提供 `samples/cpp_operator/` 独立工程
- 实现一个示例算子（如列统计：count/min/max/mean）
- CMakeLists.txt 可独立编译出 .so
- 文档说明接口约定和编译步骤
- 编译产物可被系统正确加载执行

**任务分解**:
- [x] 确定 C++ 算子插件的导出接口约定（工厂函数签名）
- [x] 创建 `samples/cpp_operator/` 工程骨架
- [x] 实现示例算子（ColumnStatsOperator）
- [x] 编写 CMakeLists.txt（链接 flowsql 头文件，输出 .so）
- [x] 编写 README 说明开发步骤

---

### Story 12.2: C++ 算子插件动态激活与去激活
**来源**: Epic 12 | **状态**: ✅ 已完成（2026-03-28）
**估算**: 3 天

**验收标准**:
- 新增 BinAddonHostPlugin（IPlugin，作为二进制 Addon 宿主管理器，当前服务于 `type=cpp`）
- 算子模型术语统一：`type`=算子类型（`builtin|python|cpp`），`category`=算子分类，`name`=算子名
- 唯一性统一：`(category, name)` 为算子唯一标识；`type` 为属性字段，不参与唯一索引
- 一致性约束：`type=builtin <=> category=builtin`
- `category/Category` 命名统一：DB 列、API 字段、内部接口统一收敛，不保留旧字段兼容输入或别名适配
- 重命名排除项：`CatalogPlugin` 插件名及 URI 中表示插件/服务语义的 `catalog` 字段不改
- `POST /operators/upload` 统一上传入口（Web 仅转发上传结果，业务插件负责最终处理）
- `POST /operators/activate` 统一激活入口：builtin/python 使用 `{"type":"...","name":"category.op"}`，cpp 使用 `{"type":"cpp","plugin_id":"..."}`
- `POST /operators/deactivate` 统一去激活入口：builtin/python 使用 `{"type":"...","name":"category.op"}`，cpp 使用 `{"type":"cpp","plugin_id":"..."}`
- `POST /operators/list` 为类型化列表接口（`type` 通过 body 传递；Web 层由 `/api/operators/list?type=...` 转发）
- `POST /operators/detail` 返回单项详情（builtin/python 按 `name`；cpp 按 `plugin_id`）
- 当 `type=="cpp"` 时，`/operators/list` 返回插件列表（每插件一行），并在列表项 `data.operators[i].plugin` 下返回插件摘要元数据：
  `so_file`、`status`、`abi_version`、`size_bytes`、`sha256`、`operator_count`、`operators`
- 激活失败原因可查：`data.operators[i].plugin.last_error`
- `plugin_id` 固定采用 `sha256`（64 位小写 hex）；重复上传同内容返回 409（需先 delete 旧插件）
- `/operators/list` 中 `data.operators[i].plugin.operator_count == data.operators[i].plugin.operators.size()`（激活态），
  且 `data.operators[i].plugin.operators` 仅包含对应 .so 导出的算子名
- 激活后算子注册到 IOperatorCatalog
- 去激活时检查 active_count（存活的算子实例数），大于 0 则立即拒绝，不做延迟卸载
- 重启恢复时，`status=activated` 的 C++ 插件在启动阶段自动重激活一次；失败置 `status=broken`
- 不进行后台循环自动重试；`status=broken` 仅允许用户手动激活重试
- 新增 `POST /operators/delete`：按 `plugin_id` 删除未激活插件（文件+元数据）
- Web Operators 页面通过列表接口直接展示上述 C++ 插件元数据字段（避免 N+1）
- `/operators/detail` 主要用于 Python 算子深度信息；C++ detail 接口保留用于兼容与单项核查
- API 风格与 Python 算子保持一致

**任务分解**:
- [x] 执行 `category` 命名统一迁移（DB schema、API 契约、内部命名），并移除兼容期别名适配
- [x] 定义 C++ 算子插件导出接口（extern "C" 工厂函数，4 个导出符号）
- [x] 实现 BinAddonHostPlugin（Load/Unload/Start/Stop + 对外管理方法）
- [x] 实现上传态插件登记（`plugin_id -> path`），并在激活/去激活时通过 `plugin_id` 分发
- [x] 实现 upload 预处理（不加载 so）：仅落盘 + sha256/plugin_id + `operator_plugin_store(status=uploaded)`
- [x] 新增 `operator_plugin_store` 持久化（上传态/激活态/错误态）并补充启动恢复逻辑
- [x] 实现启动自动重激活：对 `status=activated` 的插件在启动阶段自动调用激活流程
- [x] 实现激活失败状态机：失败写 `last_error` 并置 `status=broken`，阻断自动重试
- [x] 扩展 `operator_catalog`：新增 `plugin_id` 字段并增加 `type/plugin_id` 一致性约束
- [x] 实现“上传后未激活可见”：`/operators/list` 在 C++ 未激活时返回基础摘要字段，激活后补齐 ABI/算子列表字段
- [x] 实现 ActivateByPath（dlopen + 工厂函数调用 + 注册到 CatalogPlugin + 元数据采集）
- [x] 实现激活阶段检测：ABI 校验、导出算子清单探测、`(category,name)` 冲突检测；失败写入 `last_error`
- [x] 实现 DeactivateByPath（引用计数 + 安全 dlclose）
- [x] 实现 DeleteByPath（仅未激活插件可删；清理文件+catalog+plugin_store）
- [x] 实现 GetPluginMetaByPluginId（按 `plugin_id` 查询插件元数据，供 detail 复用）
- [x] 扩展 CatalogPlugin 的 `/operators/activate|deactivate|delete|detail` 统一分发逻辑
- [x] 重构 `WebServer::HandleUploadOperator`：仅负责临时文件接收与业务转发，不执行算子业务落盘逻辑
- [x] 集成到 gateway.yaml 插件列表
- [x] 前端 Operators 页面补充 C++ 插件元数据展示
- [x] 与 Sample 工程联调验证

---

## 风险与依赖

| 风险 | 缓解措施 |
|------|---------|
| libpq 在构建环境中未安装 | 提前确认 apt install libpq-dev |
| PostgreSQL bytea 文本格式与 Arrow binary 对齐风险 | 在 ResultSet 中统一 hex 解码，增加 binary round-trip 测试 |
| 会话工厂/方言抽象改造影响现有 sqlite/mysql/clickhouse 行为 | 增加三库回归（tables/describe/preview + CreateReader/CreateWriter） |
| dlclose 导致 vtable 悬空 | 去激活前确保无活跃 IOperator 引用 |
| C++ 算子 ABI 兼容性 | 限定编译器版本，接口用纯虚基类隔离 |
| 大文件 SHA256 计算开销 | 仅在 upload 时计算并缓存；activate/detail 直接读取缓存元数据 |

---

## 技术设计要点（待讨论）

- C++ 算子导出函数签名已定：`flowsql_abi_version/flowsql_operator_count/flowsql_create_operator/flowsql_destroy_operator`
- 支持一个 .so 导出多个算子；激活/去激活按 .so 为单位
- 去激活策略已定：`active_count > 0` 立即返回 409，不做延迟卸载
- PostgreSQL schema 参数结论：本 Story 暴露可选 `schema` 请求参数；缺省 `public`
- `IDbSessionFactoryProvider` 放在 `idb_driver.h` 还是独立 capability 头文件

---

# Story 12 实现计划：C++ 算子插件

> **目标**：实现 C++ 算子插件的编译 Sample 工程（Story 12.1）和动态激活/去激活（Story 12.2）。
>
> **架构**：BinAddonHostPlugin（IPlugin）管理用户 .so 的 dlopen/dlclose（二进制 Addon 宿主层），
> 通过 IOperatorRegistry 注册工厂函数，SchedulerPlugin 透明获取算子实例。
>
> **技术栈**：C++17、dlopen/dlclose、std::atomic、shared_ptr 引用计数、CMake

---

## 任务 1：接口扩展 — IOperatorRegistry::RemoveFactory + IOperatorCatalog::SetActive

**文件**：
- 修改：`src/framework/interfaces/ioperator_registry.h`
- 修改：`src/framework/interfaces/ioperator_catalog.h`
- 修改：`src/services/catalog/catalog_plugin.h`
- 修改：`src/services/catalog/catalog_plugin.cpp`

- [x] **步骤 1**：在 `ioperator_registry.h` 的 `IOperatorRegistry` 接口末尾新增纯虚方法：

```cpp
// 注销算子工厂；不存在时返回 -1
virtual int RemoveFactory(const char* name) = 0;
```

- [x] **步骤 2**：在 `ioperator_catalog.h` 的 `IOperatorCatalog` 接口末尾新增纯虚方法：

```cpp
virtual int SetActive(const std::string& category,
                      const std::string& name,
                      bool active) = 0;
```

- [x] **步骤 3**：在 `catalog_plugin.h` 的 `IOperatorRegistry` 实现区声明 `RemoveFactory`，
  在 `IOperatorCatalog` 实现区声明 `SetActive`。

- [x] **步骤 4**：在 `catalog_plugin.cpp` 实现 `RemoveFactory`（加锁 erase `op_factories_`）：

```cpp
int CatalogPlugin::RemoveFactory(const char* name) {
    if (!name || !*name) return -1;
    std::lock_guard<std::mutex> lock(mu_);
    return op_factories_.erase(name) > 0 ? 0 : -1;
}
```

- [x] **步骤 5**：在 `catalog_plugin.cpp` 实现 `SetActive`（包装现有私有方法）：

```cpp
int CatalogPlugin::SetActive(const std::string& category,
                              const std::string& name, bool active) {
    return SetOperatorActive(category, name, active ? 1 : 0);
}
```

- [x] **步骤 6**：在 `CatalogPlugin::Load()` 中为内置算子同时注册双 key（向后兼容）：

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

- [x] **步骤 7**：编译验证（无新增测试，接口变更不破坏现有测试）：

```bash
cmake --build build --target flowsql_catalog test_builtin -j4 2>&1 | tail -20
```

预期：编译通过，`test_builtin` 运行无 FAIL。

- [x] **步骤 8**：Commit

```bash
git add src/framework/interfaces/ioperator_registry.h \
        src/framework/interfaces/ioperator_catalog.h \
        src/services/catalog/catalog_plugin.h \
        src/services/catalog/catalog_plugin.cpp
git commit -m "feat(catalog): add RemoveFactory/SetActive interfaces for C++ operator plugin support"
```

---

## 任务 2：SchedulerPlugin — FindOperator 扩展支持非 builtin category

**文件**：
- 修改：`src/services/scheduler/scheduler_plugin.cpp`（FindOperator，约第 345-350 行）

- [x] **步骤 1**：将 FindOperator 第 3 步从仅查 builtin 改为查所有 category：

```cpp
// 修改前
if (IEquals(category, "builtin") && op_registry) {
    IOperator* op = op_registry->Create(name.c_str());
    if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
}

// 修改后
if (op_registry) {
    // 先用 "category.name" 格式查（C++ 动态算子 + 内置算子双 key）
    std::string key = category + "." + name;
    IOperator* op = op_registry->Create(key.c_str());
    if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    // 向后兼容：builtin 算子旧 key（纯 name）仍可查到
    if (IEquals(category, "builtin")) {
        op = op_registry->Create(name.c_str());
        if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    }
}
```

- [x] **步骤 2**：编译并运行 scheduler E2E 测试验证向后兼容：

```bash
cmake --build build --target flowsql_scheduler test_scheduler_e2e -j4 2>&1 | tail -10
./build/output/test_scheduler_e2e
```

预期：所有现有测试通过，`builtin.passthrough` 和 `passthrough` 均可查到。

- [x] **步骤 3**：Commit

```bash
git add src/services/scheduler/scheduler_plugin.cpp
git commit -m "feat(scheduler): extend FindOperator to support non-builtin category via category.name key"
```

---

## 任务 3：BinAddonHostPlugin — 核心实现

**文件**：
- 新增：`src/services/binaddon/binaddon_host_plugin.h`
- 新增：`src/services/binaddon/binaddon_operator_proxy.h`
- 新增：`src/services/binaddon/binaddon_host_plugin.cpp`
- 新增：`src/services/binaddon/CMakeLists.txt`
- 修改：`src/CMakeLists.txt`

- [x] **步骤 1**：新增 `binaddon_host_plugin.h`，定义 `LoadedSo` 和 `BinAddonHostPlugin`：

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
namespace binaddon {

struct LoadedSo {
    void*  handle = nullptr;
    std::string path;
    std::atomic<int>  active_count{0};
    std::atomic<bool> pending_unload{false};
    int        (*count_fn)()         = nullptr;
    IOperator* (*create_fn)(int)     = nullptr;
    void       (*destroy_fn)(IOperator*) = nullptr;
    std::vector<std::string> operator_keys;  // "category.name" 格式

    ~LoadedSo() {
        if (handle) { dlclose(handle); handle = nullptr; }
    }
};

class __attribute__((visibility("default"))) BinAddonHostPlugin : public IPlugin {
 public:
    int Option(const char* arg) override { return 0; }
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override { return 0; }
    int Stop()  override { return 0; }

    // 供 CatalogPlugin 统一 URI 分发调用
    int32_t ActivateByPath(const std::string& path, std::string& rsp);
    int32_t DeactivateByPath(const std::string& path, std::string& rsp);
    int32_t GetPluginMetaByPluginId(const std::string& plugin_id, std::string& rsp);

 private:
    IOperatorRegistry* registry_ = nullptr;
    IOperatorCatalog*  catalog_  = nullptr;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<LoadedSo>> loaded_sos_;
};

}  // namespace binaddon
}  // namespace flowsql
```

- [x] **步骤 2**：新增 `binaddon_operator_proxy.h`（header-only，只在 binaddon_host_plugin.cpp 中 include）：

```cpp
#pragma once
#include <memory>
#include <framework/interfaces/ioperator.h>
#include <services/binaddon/binaddon_host_plugin.h>

namespace flowsql {
namespace binaddon {

// BinAddonOperatorProxy：转发所有 IOperator 方法到用户 .so 的实现
// 构造时 active_count++，析构时调用 destroy_fn 后 active_count--
// 持有 shared_ptr<LoadedSo>，保证 dlclose 在所有 Proxy 析构后才执行
class BinAddonOperatorProxy : public IOperator {
 public:
    BinAddonOperatorProxy(IOperator* impl, std::shared_ptr<LoadedSo> so)
        : impl_(impl), so_(std::move(so)) {
        so_->active_count.fetch_add(1, std::memory_order_acq_rel);
    }

    ~BinAddonOperatorProxy() override {
        so_->destroy_fn(impl_);
        impl_ = nullptr;
        so_->active_count.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::string Category()     override { return impl_->Category(); }
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

}  // namespace binaddon
}  // namespace flowsql
```

- [x] **步骤 3**：新增 `binaddon_host_plugin.cpp`，实现 Load/Unload/ActivateByPath/DeactivateByPath/GetPluginMetaByPluginId：

Load() 获取 IOperatorRegistry 和 IOperatorCatalog 指针；
Unload() 遍历 loaded_sos_ 注销所有算子并清空 map；
ActivateByPath 按设计文档流程实现（dlopen 在锁外，二次检查在锁内）；
DeactivateByPath 按设计文档流程实现（pending_unload + active_count 检查）；
GetPluginMetaByPluginId 返回插件元数据（按 plugin_id 查询）
（so_file/abi_version/size_bytes/sha256/operator_count/operators）。

- [x] **步骤 4**：新增 `CMakeLists.txt`：

```cmake
project(flowsql_binaddon)
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

- [x] **步骤 5**：在 `src/CMakeLists.txt` 末尾添加：

```cmake
add_subdirectory(${CMAKE_SOURCE_DIR}/services/binaddon ${CMAKE_BINARY_DIR}/binaddon)
```

- [x] **步骤 6**：编译验证：

```bash
cmake --build build --target flowsql_binaddon -j4 2>&1 | tail -10
```

预期：编译通过，`build/output/libflowsql_binaddon.so` 生成。

- [x] **步骤 7**：Commit

```bash
git add src/services/binaddon/ src/CMakeLists.txt
git commit -m "feat(binaddon): implement BinAddonHostPlugin with dlopen/dlclose lifecycle management"
```

---

## 任务 4：WebServer — 统一 URI（不新增 /api/operators/cpp/*）

**文件**：
- 修改：`src/services/web/web_server.cpp`

- [x] **步骤 1**：保持既有统一路由语义，不新增类型化 URI：
  - `/api/operators/list`：按 `type` 返回对应列表（cpp 为插件列表，builtin/python 为算子列表）
  - `/api/operators/detail`：单算子详情（主要用于 Python；C++ 按需查询）
  - `/api/operators/activate`、`/api/operators/deactivate`：统一激活/去激活入口
  - `/api/operators/delete`：统一删除入口（仅 cpp 插件）

```cpp
// 无新增 /api/operators/cpp/* 路由；复用既有 /api/operators/* 统一入口
// GET  /api/operators/list?type=... -> POST /operators/list with body {"type":"..."}
// POST /api/operators/activate   -> /operators/activate
// POST /api/operators/deactivate -> /operators/deactivate
// POST /api/operators/delete     -> /operators/delete
// POST /api/operators/detail     -> /operators/detail
```

- [x] **步骤 2**：编译验证：

```bash
cmake --build build --target flowsql_web -j4 2>&1 | tail -5
```

- [x] **步骤 3**：Commit

```bash
git add src/services/web/web_server.cpp
git commit -m "refactor(web): keep unified /api/operators/* routes for cpp operator lifecycle"
```

---

## 任务 4.5：Frontend — C++ 插件元数据展示

**文件**：
- 修改：`src/frontend/src/views/Operators.vue`
- 可选修改：`src/frontend/src/api/index.js`（如需新增 API 封装）

- [x] **步骤 1**：在 Operators 页面复用统一接口：
  - `GET /api/operators/list?type=<builtin|python|cpp>` 渲染对应类型列表
  - `POST /api/operators/detail` 仅在用户进入单算子深度查看时调用（主要用于 Python）

- [x] **步骤 2**：表格字段按分层展示：
  - 必显：`so_file`、`size_bytes`、`sha256`、`status`
  - 条件显示：`last_error`（仅 broken）、`abi_version/operator_count/operators`（仅 activated）
  - 非主展示：`plugin_id`、`active`（放详情或展开区）

- [x] **步骤 3**：兼容空列表与错误态展示（无算子、列表请求失败）；禁止按行批量调用 detail（N+1）。

- [x] **步骤 4**：支持插件删除操作（仅 cpp，调用 `/api/operators/delete`，删除前需确认未激活）。

- [x] **步骤 5**：Commit

```bash
git add src/frontend/src/views/Operators.vue src/frontend/src/api/index.js
git commit -m "feat(frontend): show cpp plugin metadata in operators page"
```

---

## 任务 5：deploy-single.yaml — 接入插件

**文件**：
- 修改：`config/deploy-single.yaml`

- [x] **步骤 1**：在插件列表中添加 `libflowsql_binaddon.so`（无需 option）：

```yaml
- libflowsql_binaddon.so
```

- [x] **步骤 2**：Commit

```bash
git add config/deploy-single.yaml
git commit -m "chore(config): add libflowsql_binaddon.so to deploy-single.yaml"
```

---

## 任务 6：测试 fixture .so 工程

**文件**：
- 新增：`src/tests/test_binaddon/fixture_operator.cpp`
- 新增：`src/tests/test_binaddon/fixture_bad_abi.cpp`

- [x] **步骤 1**：新增 `fixture_operator.cpp`（正常 fixture，含可控阻塞点）：

```cpp
#include <condition_variable>
#include <mutex>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ichannel.h>

class FixtureOperator : public flowsql::IOperator {
 public:
    std::string Category()     override { return "fixture"; }
    std::string Name()        override { return "blocking"; }
    std::string Description() override { return "test fixture"; }
    flowsql::OperatorPosition Position() override {
        return flowsql::OperatorPosition::DATA;
    }
    int Configure(const char* key, const char* value) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (std::string(key) == "block" && std::string(value) == "1") {
            blocked_ = true;
            return 0;
        }
        if (std::string(key) == "unblock" && std::string(value) == "1") {
            blocked_ = false;
            cv_.notify_all();
            return 0;
        }
        return -1;
    }
    int Work(flowsql::IChannel* in, flowsql::IChannel* out) override {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !blocked_; });
        return 0;
    }
 private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool blocked_ = false;
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

- [x] **步骤 2**：新增 `fixture_bad_abi.cpp`（ABI 版本不匹配）：

```cpp
#include <framework/interfaces/ioperator.h>
extern "C" {
    int flowsql_abi_version()    { return 999; }  // 故意不匹配
    int flowsql_operator_count() { return 0; }
    flowsql::IOperator* flowsql_create_operator(int) { return nullptr; }
    void flowsql_destroy_operator(flowsql::IOperator*) {}
}
```

- [x] **步骤 3**：编译验证（CMakeLists.txt 在任务 7 中添加，此步骤先确认源码无语法错误）。

---

## 任务 7：测试实现

**文件**：
- 新增：`src/tests/test_binaddon/test_binaddon.cpp`
- 新增：`src/tests/test_binaddon/CMakeLists.txt`
- 修改：`src/CMakeLists.txt`（添加 test_binaddon subdirectory）

- [x] **步骤 1**：新增 `CMakeLists.txt`：

```cmake
project(test_binaddon)

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
add_executable(${PROJECT_NAME} test_binaddon.cpp)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(${PROJECT_NAME} arrow rapidjson sqlite)
add_dependencies(${PROJECT_NAME} flowsql_common flowsql_catalog flowsql_binaddon
                 fixture_operator fixture_bad_abi)
target_link_libraries(${PROJECT_NAME} flowsql_common)  # 不直接链接插件 .so，通过 PluginLoader 动态加载
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
# 将 fixture .so 路径注入测试（避免硬编码）
target_compile_definitions(${PROJECT_NAME} PRIVATE
    FIXTURE_SO_PATH="$<TARGET_FILE:fixture_operator>"
    FIXTURE_BAD_ABI_SO_PATH="$<TARGET_FILE:fixture_bad_abi>"
)
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
```

- [x] **步骤 2**：编写测试用例 1-7（激活/重复激活/ABI 不匹配/不存在路径/失败回滚/detail 元数据）：

```cpp
// 测试框架：通过 PluginLoader 加载 libflowsql_catalog.so + libflowsql_binaddon.so，
// 走完整生产路径（不直接实例化插件类，不直接链接插件 .so）
// 参考 test_builtin.cpp 的 ASSERT_EQ/ASSERT_TRUE 宏和 MakeTempDir 模式
void test_activate_ok() { ... }
void test_activate_duplicate() { ... }
void test_activate_bad_abi() { ... }
void test_activate_not_found() { ... }
void test_activate_rollback_on_register_fail() { ... }
void test_activate_rollback_on_upsert_fail() { ... }
void test_detail_contains_cpp_plugin_metadata() { ... }  // 校验 so_file/abi_version/size_bytes/sha256/operator_count/operators + count 一致性
```

- [x] **步骤 3**：运行测试 1-7，确认通过：

```bash
cmake --build build --target test_binaddon -j4 && ./build/output/test_binaddon
```

- [x] **步骤 4**：编写测试用例 8-12（去激活/不存在/向后兼容/E2E/去激活后 Create 返回 nullptr），
  并校验 detail 中同插件算子列表与 operator_count 一致。

- [x] **步骤 5**：运行测试 8-12，确认通过。

- [x] **步骤 6**：编写测试用例 13-14（并发竞态/dlclose 安全性）：

```cpp
void test_deactivate_while_active() {
    // 通过 Configure("block","1") 阻塞 Work()（避免跨 RTLD_LOCAL 边界访问全局变量）
    auto* op = registry->Create("fixture.blocking");
    op->Configure("block", "1");  // 设置阻塞模式
    std::thread worker([op]{ op->Work(in, out); });
    // 等 active_count > 0
    while (so->active_count.load() == 0) std::this_thread::yield();
    // 发起 deactivate，预期 409
    assert(DeactivateByPath(...) == 409);
    // 放行 Work
    op->Configure("unblock", "1");
    worker.join();
    // 再次 deactivate，预期 200
    assert(DeactivateByPath(...) == 200);
}
```

- [x] **步骤 7**：运行全部测试，确认通过：

```bash
./build/output/test_binaddon
```

预期：14 个测试全部 PASS，无 crash，无 ASAN 报告（如启用 AddressSanitizer）。

- [x] **步骤 8**：在 `src/CMakeLists.txt` 添加：

```cmake
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/test_binaddon ${CMAKE_BINARY_DIR}/test_binaddon)
```

- [x] **步骤 9**：Commit

```bash
git add src/tests/test_binaddon/ src/CMakeLists.txt
git commit -m "test(binaddon): add full test suite with fixture .so and concurrency tests"
```

---

## 任务 8：Sample 工程（Story 12.1）

**文件**：
- 新增：`samples/cpp_operator/CMakeLists.txt`
- 新增：`samples/cpp_operator/include/flowsql_operator_sdk.h`
- 新增：`samples/cpp_operator/src/sample_operators.cpp`
- 新增：`samples/cpp_operator/README.md`

- [x] **步骤 1**：新增 `flowsql_operator_sdk.h`（直接 include 主程序接口，不重复定义）：

```cpp
#pragma once
// 直接复用主程序接口定义，保证 vtable 布局一致
#include <common/typedef.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ichannel.h>
#include <common/span.h>

#define FLOWSQL_ABI_VERSION 1

#if defined(_WIN32)
#define FLOWSQL_SDK_EXPORT __declspec(dllexport)
#else
#define FLOWSQL_SDK_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {
    FLOWSQL_SDK_EXPORT int flowsql_abi_version();
    FLOWSQL_SDK_EXPORT int flowsql_operator_count();
    FLOWSQL_SDK_EXPORT flowsql::IOperator* flowsql_create_operator(int index);
    FLOWSQL_SDK_EXPORT void flowsql_destroy_operator(flowsql::IOperator* op);
}
```

- [x] **步骤 2**：新增 `sample_operators.cpp`，实现 `ColumnStatsOperator`（读取数值列，输出 count/min/max/mean）。

- [x] **步骤 3**：新增 `CMakeLists.txt`（独立构建，通过 `FLOWSQL_SRC_INCLUDE` 指向 SDK 头目录，含编译器版本下限检查）。

- [x] **步骤 4**：独立编译验证：

```bash
cd samples/cpp_operator
cmake -B build -DFLOWSQL_SRC_INCLUDE=/mnt/d/working/flowSQL/src
cmake --build build
ls build/libsample_cpp_operator.so
```

预期：`.so` 生成，`nm -D build/libsample_cpp_operator.so | grep flowsql_` 显示 4 个导出符号。

- [x] **步骤 5**：Commit

```bash
git add samples/cpp_operator/
git commit -m "feat(sample): add cpp_operator sample project with ColumnStatsOperator"
```

---

## 任务 9：联调验证

- [x] **步骤 1**：启动服务（使用 deploy-single.yaml），确认 `libflowsql_binaddon.so` 加载成功。

- [x] **步骤 2**：先上传 sample .so（获取 `plugin_id`），再按 `plugin_id` 激活：

```bash
curl -X POST http://localhost:18803/operators/upload \
  -H "Content-Type: application/json" \
  -d '{"type":"cpp","filename":"libsample_cpp_operator.so","tmp_path":"/path/to/libsample_cpp_operator.so"}'

curl -X POST http://localhost:18803/operators/activate \
  -H "Content-Type: application/json" \
  -d '{"type":"cpp","plugin_id":"<from-upload-response>"}'
```

预期：upload 返回包含 `plugin_id`；activate 返回 `{"code":0,"data":{"operators":["fixture.column_stats"]}}`。

- [x] **步骤 3**：查询 `POST /operators/list`（`{"type":"cpp"}`），确认该插件一行记录存在，且 `plugin.operators` 包含 `column_stats`。

```bash
# 内部 URI（body 传 type）
curl -X POST http://localhost:18803/operators/list \
  -H "Content-Type: application/json" \
  -d '{"type":"cpp"}'

# Web API（前端调用）
curl "http://localhost:18803/api/operators/list?type=cpp"
```

预期：返回中该 `plugin_id` 仅一条记录；激活后 `plugin.operators` 包含 `column_stats`。

- [x] **步骤 4**：提交一个使用该算子的任务，确认执行成功。

- [x] **步骤 5**：按 `plugin_id` 去激活 .so，确认算子在列表中仍可见但 `active=0`，再次提交任务返回"算子未激活/不可用"错误。

```bash
curl -X POST http://localhost:18803/operators/deactivate \
  -H "Content-Type: application/json" \
  -d '{"type":"cpp","plugin_id":"<from-upload-response>"}'

curl -X POST http://localhost:18803/operators/list \
  -H "Content-Type: application/json" \
  -d '{"type":"cpp"}'
```

- [x] **步骤 6**：按 `plugin_id` 调用 `/operators/delete`，确认插件文件和元数据均被清理，列表中不再显示该插件。

```bash
curl -X POST http://localhost:18803/operators/delete \
  -H "Content-Type: application/json" \
  -d '{"type":"cpp","plugin_id":"<from-upload-response>"}'

curl -X POST http://localhost:18803/operators/list \
  -H "Content-Type: application/json" \
  -d '{"type":"cpp"}'
```

预期：delete 返回成功；list 中不再出现该 `plugin_id`。
