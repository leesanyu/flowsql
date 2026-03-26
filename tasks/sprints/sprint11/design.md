# Sprint 11 设计文档

## Story 4.2: PostgreSQL 驱动支持

### 范围与关键决策

- 本 Story 目标：补齐 PostgreSQL 行式通道能力，走现有 `Traits + RelationDbSessionBase + ConnectionPool` 架构，保证可在 Web/UI 与插件层完整使用。
- 本 Story **不实现** `PQprepare + PQexecPrepared` 预编译执行路径，统一采用 `PQexec` 简单路径（与当前 MySQL 驱动策略一致）。`PostgresTraits` 与钩子保留，但仅作为模板接口兼容，不作为验收项。
- 预编译语句支持单独作为后续 Story（建议 Story 4.2.x），避免本 Story 验收口径与实现策略冲突。

---

### MySQL 对比补充（差异与共性）

#### A. 差异点补充（本 Story 必做）

1. 连接健康检查语义对齐：
- MySQL 的 `mysql_ping()` 是主动探测；PostgreSQL 若仅用 `PQstatus()` 无法覆盖网络中断场景。
- PostgresDriver `Ping()` 要求执行轻量 SQL（`SELECT 1`）做主动探测，确保与 MySQL 可用性语义一致。

2. schema 维度显式支持：
- PostgreSQL 的 `tables/describe` 依赖 `schema`，MySQL 主要依赖当前 database。
- Browse API 请求体新增可选 `schema` 字段；postgres 默认 `public`，mysql/sqlite/clickhouse 忽略该参数。

3. 受影响行数语义明确：
- `ExecuteSql()` 返回值统一为受影响行数，失败返回 `-1`。
- PostgreSQL 用 `PQcmdTuples()` 解析（空串按 0 处理），与 MySQL `mysql_affected_rows()` 语义对齐。

4. 精度边界声明：
- `numeric/decimal -> arrow::float64` 为当前实现策略，文档明确是有损映射。
- 后续如需无损精度，单独追加 decimal 能力 Story，不在本 Story 内展开。

#### B. 共性抽象补充（本 Story 内落地最小可用版）

1. 会话工厂抽象：
- 新增 `IDbSessionFactoryProvider`（能力接口）：`CreateSession()`
- `SqliteDriver/MysqlDriver/PostgresDriver/ClickHouseDriver` 实现该接口。
- `DatabasePlugin::Get()` 不再按具体驱动 `dynamic_cast<SqliteDriver/MysqlDriver/...>` 链判断，只对 `IDbSessionFactoryProvider` 做一次能力判断。

2. Browse SQL 方言抽象：
- 新增最小方言接口（或等价 helper）统一三类逻辑：
  - `QuoteIdentifier(table, db_type)`
  - `BuildTablesSql(db_type, schema)`
  - `BuildDescribeSql(db_type, schema, table)`
- `HandleTables/HandleDescribe` 仅做参数解析与调用，不内联散落 SQL 字符串。

3. 已有共性复用保持不变：
- 继续复用 `RelationDbSessionBase + RelationBatchReader/Writer + ConnectionPool`，避免重复实现生命周期、事务封装与 Arrow 适配逻辑。

---

### 架构改动

#### 新增文件

```text
src/services/database/drivers/
├── postgres_driver.h
└── postgres_driver.cpp

src/tests/test_database/
└── test_postgres.cpp
```

#### 修改文件

```text
src/services/database/
├── database_plugin.cpp         # CreateDriver/session_factory/tables/describe/quote
├── CMakeLists.txt              # 增加 libpq 依赖
└── relation_adapters.h         # RelationBatchReader 增加 BOOL 读取分支

src/tests/test_database/
├── CMakeLists.txt              # 增加 test_postgres 目标，显式 -UNDEBUG
└── test_plugin_e2e.cpp         # 增补 postgres 插件层 E2E 场景
```

---

### 详细设计

#### 1. PostgresTraits

```cpp
struct PostgresTraits {
    using ConnectionType = PGconn*;
    using StatementType  = std::string;  // 占位：当前不走 prepared statement
    using ResultType     = PGresult*;
    using BindType       = void*;
};
```

#### 2. PostgresDriver

```cpp
class PostgresDriver : public IDbDriver {
public:
    int Connect(const std::unordered_map<std::string, std::string>& params) override;
    int Disconnect() override;
    bool IsConnected() override { return pool_ != nullptr; }
    const char* DriverName() override { return "postgres"; }
    const char* LastError() override { return last_error_.c_str(); }
    bool Ping() override;

    std::shared_ptr<IDbSession> CreateSession();
    void ReturnToPool(PGconn* conn);

private:
    std::unique_ptr<ConnectionPool<PGconn*>> pool_;
    std::string host_, user_, password_, database_;
    int port_ = 5432;
    int timeout_ = 10;
    std::string last_error_;
};
```

实现要点：
- 连接串：`host=... port=... user=... password=... dbname=... connect_timeout=...`
- 工厂函数：`PQconnectdb()`，成功条件 `PQstatus(conn) == CONNECTION_OK`
- 关闭函数：`PQfinish(conn)`
- 探测连接：`pool_->Acquire()` 一次，失败则 `Connect()` 直接返回 `-1`
- `CreateSession()` 从连接池取连接，失败回填 `last_error_`
- `Ping()` 走主动探测 SQL（`SELECT 1`），网络断开/鉴权失败都应返回 `false` 并更新 `last_error_`

#### 3. PostgresResultSet

```cpp
class PostgresResultSet : public IResultSet {
public:
    PostgresResultSet(PGresult* result, std::function<void(PGresult*)> free_func);
    ~PostgresResultSet() override;

    int FieldCount() override;
    const char* FieldName(int index) const override;
    int FieldType(int index) const override;     // OID
    int FieldLength(int index) override;         // PQfsize（变长返回 -1 时转 0）
    bool HasNext() override;
    bool Next() override;
    int GetInt(int index, int* value) override;
    int GetInt64(int index, int64_t* value) override;
    int GetDouble(int index, double* value) override;
    int GetString(int index, const char** value, size_t* len) override;
    bool IsNull(int index) override;
};
```

实现细节：
- 游标使用 `current_row_`（`PQntuples` 总行数）
- 数值读取使用 `strtol/strtoll/strtod`
- `bytea` 读取：`GetString()` 发现列 OID=17 时，将 `\xDEADBEEF` hex 文本先解码为字节数组缓存，再返回字节指针与长度
- 避免把 `bytea` 当普通字符串传给 Arrow `binary`

#### 4. PostgresSession

继承 `RelationDbSessionBase<PostgresTraits>`，覆盖 `ExecuteQuery/ExecuteSql` 使用 `PQexec`。

```cpp
class PostgresSession : public RelationDbSessionBase<PostgresTraits> {
public:
    PostgresSession(PostgresDriver* driver, PGconn* conn);
    ~PostgresSession() override;

    int ExecuteQuery(const char* sql, IResultSet** result) override; // PQexec + PGRES_TUPLES_OK
    int ExecuteSql(const char* sql) override;                         // PQexec + PGRES_COMMAND_OK

protected:
    // 仅为模板接口兼容：返回“not implemented”错误，不进入本 Story 验收
    std::string PrepareStatement(PGconn* conn, const char* sql, std::string* error) override;
    int ExecuteStatement(std::string stmt_name, std::string* error) override;
    PGresult* GetResultMetadata(std::string stmt_name, std::string* error) override;

    int GetAffectedRows(std::string stmt_name) override;
    std::string GetDriverError(PGconn* conn) override;
    void FreeResult(PGresult* result) override;
    void FreeStatement(std::string stmt_name) override;
    bool PingImpl(PGconn* conn) override;
    void ReturnConnection(PGconn* conn) override;

    IResultSet* CreateResultSet(PGresult* result,
                                std::function<void(PGresult*)> free_func) override;
    IBatchReader* CreateBatchReader(IResultSet* result,
                                    std::shared_ptr<arrow::Schema> schema) override;
    IBatchWriter* CreateBatchWriter(const char* table) override;
    std::shared_ptr<arrow::Schema> InferSchema(IResultSet* result, std::string* error) override;
};
```

`InferSchema` OID 映射：

| OID | PostgreSQL 类型 | Arrow 类型 |
|-----|----------------|-----------|
| 16  | bool           | boolean   |
| 21  | int2           | int32     |
| 23  | int4           | int32     |
| 20  | int8           | int64     |
| 700 | float4         | float32   |
| 701 | float8         | float64   |
| 1700| numeric        | float64   |
| 17  | bytea          | binary    |
| 其他 | text/varchar 等 | utf8     |

#### 5. PostgresBatchWriter

- 标识符：双引号包裹，内部 `"` 转义为 `""`
- DDL：`CREATE TABLE IF NOT EXISTS`
- 插入：分块多值 `INSERT`（每批 1000 行）
- 值转义规则：字符串 `'` 转义为 `''`；布尔写 `TRUE/FALSE`；二进制写 `decode('<HEX>', 'hex')`

注意：`RelationBatchWriterBase::BuildRowValues` 当前不覆盖 `BOOL/BINARY`，`PostgresBatchWriter` 需自定义逐列拼接逻辑，不能直接复用默认实现。

#### 6. relation_adapters.h 补丁（BOOL 读取）

为保证 `InferSchema()` 映射 `arrow::boolean()` 可实际消费，`RelationBatchReader::Next()` 新增 `arrow::Type::BOOL` 分支：
- 读取策略：优先 `GetString()`，接受 `t/f/true/false/1/0`；失败时回退 `GetInt()`
- 写入 `arrow::BooleanBuilder`

#### 7. database_plugin.cpp 集成改动

`CreateDriver()` 新增：

```cpp
if (type == "postgres") {
    return std::make_unique<PostgresDriver>();
}
```

`Get()` 内 `session_factory` 改为能力接口抽象：

```cpp
auto* provider = dynamic_cast<IDbSessionFactoryProvider*>(driver_ptr);
if (!provider) { /* set error and return nullptr */ }
auto session_factory = [provider]() { return provider->CreateSession(); };
```

`QuoteTableName()` 新增 postgres 分支（双引号转义）：

```cpp
if (db_type == "postgres") { /* "...""..." */ }
```

新增 `QuoteSqlLiteral()`（单引号字符串字面量转义）：
- `abc'd` -> `'abc''d'`

`HandleTables()` postgres 分支：

```cpp
sql = "SELECT tablename FROM pg_tables "
      "WHERE schemaname='public' ORDER BY tablename";
```

`HandleDescribe()` postgres 分支（防注入）：

```cpp
// 本 Story 即支持从请求体读取 schema，缺省为 public
std::string schema = req_schema.empty() ? "public" : req_schema;
sql = "SELECT column_name, data_type, is_nullable "
      "FROM information_schema.columns "
      "WHERE table_schema=" + QuoteSqlLiteral(schema) +
      " AND table_name=" + QuoteSqlLiteral(table) +
      " ORDER BY ordinal_position";
```

Browse API 参数补充：
- `schema` 可选；postgres 缺省 `public`
- 非 postgres 类型忽略 `schema`

会话工厂能力接口：

```cpp
interface IDbSessionFactoryProvider {
    virtual ~IDbSessionFactoryProvider() = default;
    virtual std::shared_ptr<IDbSession> CreateSession() = 0;
};
```

Browse SQL 方言抽象（最小实现可为 static helper）：

```cpp
std::string BuildTablesSql(const std::string& db_type, const std::string& schema);
std::string BuildDescribeSql(const std::string& db_type,
                             const std::string& schema,
                             const std::string& table);
```

#### 8. CMake 依赖

`src/services/database/CMakeLists.txt` 增加 libpq 检测并链接，作为 database 服务必需依赖：

```cmake
find_path(PQ_INCLUDE_DIR libpq-fe.h PATHS /usr/include /usr/include/postgresql /usr/local/include)
find_library(PQ_LIBRARY pq PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)

if(PQ_INCLUDE_DIR AND PQ_LIBRARY)
    target_include_directories(${PROJECT_NAME} PRIVATE ${PQ_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} ${PQ_LIBRARY})
else()
    message(FATAL_ERROR "PostgreSQL client library not found. Please install libpq-dev")
endif()
```

---

### 测试设计

#### A. 驱动测试：`src/tests/test_database/test_postgres.cpp`

环境变量（与现有测试风格一致）：
- `PG_HOST`（默认 `127.0.0.1`）
- `PG_PORT`（默认 `5432`）
- `PG_USER`
- `PG_PASSWORD`
- `PG_DATABASE`

不可达时行为：打印 `[SKIP]` 并返回 0。

测试矩阵：
1. `Connect()` 正常连接
2. `Connect()` 错误密码失败，`LastError` 非空
3. `ExecuteQuery("SELECT 1")`
4. `ExecuteSql()` DDL/DML
5. 事务 `BEGIN/COMMIT`
6. 事务 `BEGIN/ROLLBACK`
7. `CreateReader()` Arrow 读取
8. `CreateWriter()` Arrow 写入
9. 类型矩阵（INT32/INT64/FLOAT/DOUBLE/STRING/BOOL/BINARY）
10. 并发读（多线程）
11. 并发写（多线程，不同表，校验无串扰）
12. 标识符/字符串转义场景（引号、分号）
13. `Ping()` 主动探测场景（网络不可达/鉴权失败/正常）
14. `schema` 参数场景（默认 `public` + 指定 schema）

#### B. 插件层 E2E：扩展 `src/tests/test_database/test_plugin_e2e.cpp`

新增 postgres 场景，必须走生产路径：
- `PluginLoader -> IDatabaseFactory -> IDatabaseChannel`
- 覆盖 `Get("postgres", "...")`、`CreateReader`、`CreateWriter`
- 覆盖 `/channels/database/tables`、`/channels/database/describe` 路由在 postgres 下的行为

#### C. 测试构建接入

文件：`src/tests/test_database/CMakeLists.txt`
- 新增 `add_executable(test_postgres test_postgres.cpp)`
- 依赖：`flowsql_common`、`flowsql_database`、`arrow rapidjson`
- 强制 `target_compile_options(test_postgres PRIVATE -UNDEBUG)`

---

### 配置格式

```text
type=postgres;name=mydb;host=localhost;port=5432;user=postgres;password=secret;database=testdb
```

可选参数：
- `timeout`（默认 10 秒）

---

### 风险与回退

- 风险：libpq 未安装导致构建失败
- 缓解：在开发环境文档中增加 `libpq-dev` 依赖说明，并在 CI 镜像预装
- 风险：`bytea` 文本格式解析不一致
- 缓解：统一按 hex 输出解析（`\x...`），测试覆盖二进制 round-trip
- 风险：会话工厂/方言 helper 抽象改造影响现有三库行为
- 缓解：对 sqlite/mysql/clickhouse 执行 tables/describe/preview + CreateReader/CreateWriter 回归

---

## Story 12.1 & 12.2: C++ 算子插件

### 背景

FlowSQL 已支持内置 C++ 算子（passthrough/concat/hstack）和 Python 动态算子（通过 BridgePlugin 代理）。
C++ 算子插件扩展允许用户将自定义 C++ 算子编译为独立 .so，在运行时通过 HTTP API 动态加载/卸载，
无需重启服务。

**设计决策**：
- 接口方案：纯虚基类 + `extern "C"` 工厂函数，与现有 IPlugin 架构一致
- 单 .so 支持多算子（成对算子如加解密放同一 .so 更易管理）
- 激活/去激活以 .so 为单位
- 去激活时有活跃引用则立即拒绝（不做延迟卸载）

---

### 架构设计

```
CppOperatorPlugin（IPlugin + IRouterHandle）
  │
  ├── 激活：dlopen → ABI 校验 → 枚举算子 → 注册到 IOperatorRegistry（key="catelog.name"）
  ├── 去激活：标记 pending_unload → 检查 active_count → 拒绝 or 注销工厂 + 移除 map
  │           （dlclose 由 LoadedSo 析构函数负责，shared_ptr 引用计数归零时自动触发）
  │
  └── loaded_sos_: unordered_map<path, shared_ptr<LoadedSo>>
                      ├── handle（dlopen 返回，析构时 dlclose）
                      ├── pending_unload（atomic<bool>，标记后工厂函数拒绝创建新实例）
                      ├── active_count（atomic<int>，存活的 CppOperatorProxy 实例数）
                      └── operator_keys（已注册的 "catelog.name" 列表）
```

**active_count 语义**：记录当前存活的 `CppOperatorProxy` 实例数，即"正在被任务持有、尚未析构的算子实例数"。
每次 `Create()` 返回一个 `CppOperatorProxy`，构造时 `active_count++`，析构时 `active_count--`。
线程池并发时同一算子类型可有多个实例（`active_count > 1`），各任务持有独立实例互不干扰。
去激活时 `active_count > 0` 意味着有任务正在使用该 .so 的代码段，此时 dlclose 会导致崩溃，故拒绝。

**算子查找路径**：SchedulerPlugin::FindOperator 现有逻辑：
1. 遍历 IID_OPERATOR（静态注册算子）
2. IBridge（Python 算子）
3. IOperatorRegistry::Create（仅 catelog=="builtin"）

C++ 动态算子通过**扩展第 3 步**支持：去掉 `catelog=="builtin"` 限制，改为对所有 catelog 查询
IOperatorRegistry。注册时 key 使用 `"catelog.name"` 格式（如 `"mylib.encrypt"`），避免多 catelog
重名冲突。

---

### 新增文件

```
src/services/cpp_operator/
├── cpp_operator_plugin.h    # CppOperatorPlugin + LoadedSo 声明
├── cpp_operator_plugin.cpp  # 实现
├── cpp_operator_proxy.h     # CppOperatorProxy（header-only）
└── CMakeLists.txt           # 编译为 libflowsql_cpp_operator.so

samples/cpp_operator/
├── CMakeLists.txt           # 独立编译模板（不依赖 FlowSQL 构建系统）
├── include/
│   └── flowsql_operator_sdk.h  # 随 SDK 分发的头文件（直接 include 主程序接口）
├── src/
│   └── sample_operators.cpp    # 示例：ColumnStatsOperator（count/min/max/mean）
└── README.md

src/tests/test_cpp_operator/
├── test_cpp_operator.cpp    # 测试
├── fixture_operator.cpp     # 测试用 fixture .so 源码（含阻塞点）
├── fixture_bad_abi.cpp      # ABI 版本不匹配的 fixture .so
└── CMakeLists.txt
```

**修改文件**：

```
src/framework/interfaces/ioperator_registry.h  # 新增 Unregister() 纯虚方法
src/framework/interfaces/ioperator_catalog.h   # 新增 SetActive() 纯虚方法
src/services/catalog/catalog_plugin.h/cpp      # 实现 Unregister/SetActive；内置算子双 key 注册
src/services/scheduler/scheduler_plugin.cpp    # FindOperator 扩展支持非 builtin catelog
src/CMakeLists.txt                             # 添加 add_subdirectory(services/cpp_operator)
config/deploy-single.yaml                      # 插件列表添加 libflowsql_cpp_operator.so
src/services/web/web_server.cpp                # 添加 /api/operators/cpp/* 代理路由
```

---

### 详细设计

#### 1. 插件导出接口约定（flowsql_operator_sdk.h）

SDK 头文件**不重复定义** `IOperator`，直接 include 主程序接口头文件，保证 vtable 布局与主程序完全一致：

```cpp
// flowsql_operator_sdk.h — 插件开发者 include 此文件
// 发布时从主程序 src/ 目录导出以下头文件：
//   framework/interfaces/ioperator.h
//   framework/interfaces/ichannel.h
//   common/define.h（interface 宏）
//   common/span.h（Span<T>）
#pragma once
#include <common/define.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ichannel.h>

// 当前 ABI 版本，主程序加载时校验
#define FLOWSQL_ABI_VERSION 1

// 插件必须导出的 4 个 C 函数（extern "C" 保证无 name mangling）
extern "C" {
    int flowsql_abi_version();          // 返回 FLOWSQL_ABI_VERSION
    int flowsql_operator_count();       // 返回此 .so 包含的算子数量
    flowsql::IOperator* flowsql_create_operator(int index);   // 按索引创建实例
    void flowsql_destroy_operator(flowsql::IOperator* op);    // 销毁实例
}
```

> **vtable 一致性保证**：SDK 头文件直接复用主程序的 `ioperator.h`，`IOperator` 只有一份定义，
> 虚函数表布局由同一份头文件决定，消除两份定义导致的 vtable 偏移错位风险。

> **异常隔离**：`flowsql_create_operator` 内部必须 `try/catch(...)` 所有异常并返回 nullptr，
> 禁止异常跨 .so 边界传播（SEI CERT ERR59-CPP）。

#### 2. IOperatorRegistry 扩展：Unregister

```cpp
// ioperator_registry.h 新增
virtual int Unregister(const char* name) = 0;  // 注销算子；不存在时返回 -1
```

CatalogPlugin 实现：加锁从 `op_factories_` map 中 erase，返回 0（成功）或 -1（不存在）。

#### 3. IOperatorCatalog 扩展：SetActive

```cpp
// ioperator_catalog.h 新增
virtual int SetActive(const std::string& catelog,
                      const std::string& name,
                      bool active) = 0;
```

CatalogPlugin 实现：包装现有私有方法 `SetOperatorActive(catelog, name, active ? 1 : 0)`。

#### 4. LoadedSo 与生命周期管理

```cpp
// cpp_operator_plugin.h 内
struct LoadedSo {
    void*  handle = nullptr;
    std::string path;
    std::atomic<int>  active_count{0};    // 存活的 CppOperatorProxy 实例数
    std::atomic<bool> pending_unload{false};  // 标记后工厂函数拒绝创建新实例
    int        (*count_fn)()  = nullptr;
    IOperator* (*create_fn)(int) = nullptr;
    void       (*destroy_fn)(IOperator*) = nullptr;
    std::vector<std::string> operator_keys;  // "catelog.name" 格式

    ~LoadedSo() {
        // dlclose 在此处调用，保证所有 CppOperatorProxy 析构后才执行
        // （shared_ptr 引用计数归零时触发）
        if (handle) { dlclose(handle); handle = nullptr; }
    }
};
```

**生命周期说明**：
- `loaded_sos_` 持有 `shared_ptr<LoadedSo>`（引用计数 = 1）
- 每个 `CppOperatorProxy` 也持有 `shared_ptr<LoadedSo>`（引用计数 += 1）
- `HandleDeactivate` 从 map 移除后引用计数 -= 1；所有 Proxy 析构后引用计数归零
- 引用计数归零时 `~LoadedSo()` 自动调用 `dlclose`，此时 `destroy_fn` 已不再被任何 Proxy 调用

#### 5. CppOperatorProxy

```cpp
// cpp_operator_proxy.h — header-only
// 注意：此文件只在 cpp_operator_plugin.cpp 中使用，不暴露给其他插件
class CppOperatorProxy : public IOperator {
public:
    CppOperatorProxy(IOperator* impl, std::shared_ptr<LoadedSo> so)
        : impl_(impl), so_(so) {
        so_->active_count.fetch_add(1, std::memory_order_acq_rel);
    }

    ~CppOperatorProxy() override {
        so_->destroy_fn(impl_);  // 在 .so 内部 delete，保证 allocator 一致
        impl_ = nullptr;
        so_->active_count.fetch_sub(1, std::memory_order_acq_rel);
        // so_ shared_ptr 在此析构，引用计数 -1；若归零则触发 ~LoadedSo() → dlclose
    }

    std::string Catelog()     override { return impl_->Catelog(); }
    std::string Name()        override { return impl_->Name(); }
    std::string Description() override { return impl_->Description(); }
    OperatorPosition Position() override { return impl_->Position(); }
    int Work(IChannel* in, IChannel* out) override { return impl_->Work(in, out); }
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
```

#### 6. CppOperatorPlugin

```cpp
class CppOperatorPlugin : public IPlugin, public IRouterHandle {
public:
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override { return 0; }
    int Stop()  override { return 0; }
    void EnumRoutes(fnRouteRegister reg) override;

private:
    int HandleActivate(const std::string& req, std::string& rsp);
    int HandleDeactivate(const std::string& req, std::string& rsp);
    int HandleList(const std::string& req, std::string& rsp);

    IOperatorRegistry* registry_ = nullptr;
    IOperatorCatalog*  catalog_  = nullptr;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<LoadedSo>> loaded_sos_;
};
```

**HandleActivate 流程**：

```
1. 解析 path（JSON body）
2. 加锁（mu_），检查 path 是否已加载（重复激活返回 409）
3. 解锁，在锁外执行 dlopen(path, RTLD_NOW | RTLD_LOCAL)
   （dlopen 可能耗时，不持锁避免阻塞其他 HTTP 请求）
4. dlsym 获取 4 个导出函数，任一缺失则 dlclose + 返回错误
5. 校验 flowsql_abi_version() == FLOWSQL_ABI_VERSION，不匹配则 dlclose + 返回错误
6. 循环 flowsql_operator_count()，对每个 index：
   a. flowsql_create_operator(index) 探测元数据（Catelog/Name）
   b. key = catelog + "." + name
   c. flowsql_destroy_operator 销毁探测实例
7. 加锁（mu_），再次检查 path 是否已加载（防止并发重复激活）
8. 向 IOperatorRegistry 注册工厂函数（key 为 "catelog.name"）：
   工厂函数捕获 shared_ptr<LoadedSo>，检查 pending_unload，
   若为 true 返回 nullptr，否则 new CppOperatorProxy(create_fn(index), so)
9. 向 IOperatorCatalog::UpsertBatch 写入元数据（type="cpp"，active=1）
10. 保存 shared_ptr<LoadedSo> 到 loaded_sos_
11. 解锁，返回 200 + 已注册算子列表
```

**HandleDeactivate 流程**：

```
1. 解析 path
2. 加锁（mu_），查找 LoadedSo，不存在返回 404
3. 设置 so->pending_unload.store(true)（阻止工厂函数创建新实例）
4. 检查 active_count：
   - active_count > 0 → 重置 pending_unload.store(false)，解锁，返回 409
5. 从 IOperatorRegistry 逐一 Unregister(key)
6. 从 IOperatorCatalog 逐一 SetActive(catelog, name, false)
7. 从 loaded_sos_ 移除（shared_ptr 引用计数 -1）
   - 若此时无存活 Proxy（active_count==0），引用计数归零，~LoadedSo() 自动 dlclose
8. 解锁，返回 200
```

> **并发安全说明**：
> - `pending_unload` 是 `atomic<bool>`，工厂 lambda 在 `CatalogPlugin::mu_` 下读取，
>   `HandleDeactivate` 在 `CppOperatorPlugin::mu_` 下写入，两把锁不同但 atomic 保证无数据竞争。
> - 步骤 3 设置 `pending_unload=true` 后，新的 `Create()` 调用返回 nullptr，不再增加 `active_count`。
> - 步骤 4 检查 `active_count` 时，已在途的 `Work()` 调用仍持有 Proxy，`active_count > 0` 触发拒绝。
> - `dlclose` 由 `~LoadedSo()` 负责，时机由 `shared_ptr` 引用计数保证，不存在 destroy_fn 悬空问题。

#### 7. SchedulerPlugin 修改

`FindOperator` 第 3 步扩展支持非 builtin catelog：

```cpp
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

CatalogPlugin::Load() 中内置算子同时注册两个 key：

```cpp
Register("passthrough",         []() -> IOperator* { return new PassthroughOperator(); });
Register("builtin.passthrough", []() -> IOperator* { return new PassthroughOperator(); });
// concat / hstack 同理
```

#### 8. HTTP API

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/operators/cpp/activate` | body: `{"path":"/abs/path/to/lib.so"}` |
| POST | `/operators/cpp/deactivate` | body: `{"path":"/abs/path/to/lib.so"}` |
| GET  | `/operators/cpp/list` | 列出已激活的 .so 及其算子列表 |

响应风格与 Python 算子 API 保持一致（`{"code":0,"message":"ok","data":{...}}`）。

---

### Sample 工程设计

`samples/cpp_operator/` 是独立 CMake 工程，不依赖 FlowSQL 构建系统：

```cmake
# samples/cpp_operator/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(sample_cpp_operator VERSION 1.0.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FlowSQL 源码 include 路径（含 ioperator.h / ichannel.h / common/define.h）
set(FLOWSQL_SRC_INCLUDE "" CACHE PATH "FlowSQL src/ 目录路径")
if(NOT FLOWSQL_SRC_INCLUDE)
    message(FATAL_ERROR "请设置 FLOWSQL_SRC_INCLUDE，例如：cmake -DFLOWSQL_SRC_INCLUDE=/path/to/flowsql/src")
endif()

add_library(sample_cpp_operator SHARED src/sample_operators.cpp)
target_include_directories(sample_cpp_operator PRIVATE
    ${FLOWSQL_SRC_INCLUDE}
    include   # flowsql_operator_sdk.h 所在目录
)

# 必须：隐藏所有符号，只暴露 flowsql_* 导出函数
target_compile_options(sample_cpp_operator PRIVATE -fvisibility=hidden)

set_target_properties(sample_cpp_operator PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "sample_cpp_operator"
)
```

示例算子 `ColumnStatsOperator`：读取输入通道的数值列，计算 count/min/max/mean，写入输出通道。

---

### 安全约束（编译要求文档化）

插件开发者须遵守：

1. **编译器版本**：与主程序相同（GCC 11+ 或 Clang 14+），避免 vtable ABI 不兼容
2. **符号隐藏**：必须使用 `-fvisibility=hidden`，避免符号污染全局符号表
3. **异常隔离**：`flowsql_create_operator` 内部 catch 所有异常，不跨边界传播
4. **内存所有权**：`flowsql_create_operator` 分配的对象必须由 `flowsql_destroy_operator` 销毁
5. **线程安全**：`Work()` 可能被多线程并发调用，实现须线程安全或无状态
6. **第三方库约束**：插件不得依赖与主程序版本不同的第三方库；优先只依赖 SDK 头文件和标准库

---

### 测试方案

```
src/tests/test_cpp_operator/
├── test_cpp_operator.cpp    # 测试主文件
├── fixture_operator.cpp     # 正常 fixture .so（含可控阻塞点）
├── fixture_bad_abi.cpp      # ABI 版本不匹配 fixture .so
└── CMakeLists.txt           # target_compile_options PRIVATE -UNDEBUG
```

**fixture .so 构建方式**：

```cmake
# 正常 fixture（flowsql_abi_version 返回正确版本）
add_library(fixture_operator SHARED fixture_operator.cpp)
target_include_directories(fixture_operator PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(fixture_operator PRIVATE -fvisibility=hidden)

# ABI 不匹配 fixture（flowsql_abi_version 返回 999）
add_library(fixture_bad_abi SHARED fixture_bad_abi.cpp)
target_compile_options(fixture_bad_abi PRIVATE -fvisibility=hidden)

# 将 fixture .so 路径注入测试可执行文件（避免硬编码）
target_compile_definitions(test_cpp_operator PRIVATE
    FIXTURE_SO_PATH="$<TARGET_FILE:fixture_operator>"
    FIXTURE_BAD_ABI_SO_PATH="$<TARGET_FILE:fixture_bad_abi>"
)
add_dependencies(test_cpp_operator fixture_operator fixture_bad_abi)
```

**fixture_operator.cpp 关键设计**（支持并发竞态测试）：

```cpp
// 全局阻塞点，测试注入
std::promise<void>* g_work_block = nullptr;

class BlockingOperator : public flowsql::IOperator {
    int Work(IChannel* in, IChannel* out) override {
        if (g_work_block) g_work_block->get_future().wait();  // 可控阻塞
        return 0;
    }
    // ...
};
```

**测试用例**（走完整插件路径：PluginLoader → CppOperatorPlugin → IOperatorRegistry）：

1. 激活合法 .so → 算子以 "catelog.name" 注册到 IOperatorRegistry
2. 激活不存在路径 → 返回错误，registry 无变化
3. 激活 ABI 版本不匹配 .so → 返回错误，dlclose 已调用（验证无内存泄漏）
4. 重复激活同一 .so → 返回 409
5. 去激活已激活 .so → 算子从 registry 注销，catalog 标记 active=0，dlclose 已调用
6. 去激活不存在 .so → 返回 404
7. 去激活时 active_count > 0 → 返回 409，pending_unload 回滚，算子仍可用
   （通过 g_work_block 阻塞 Work()，确保 active_count > 0 时发起 deactivate，测试确定性）
8. 激活后通过 SchedulerPlugin 执行 Work()（E2E 路径，验证 FindOperator 扩展）
9. 去激活后 Create("catelog.name") 返回 nullptr
10. 并发竞态：多线程同时 Create + 单线程 Deactivate，验证无 crash、无数据竞争
11. 内置算子向后兼容：builtin.passthrough 和 passthrough 两种 key 均可查到
12. dlclose 安全性：去激活后所有 Proxy 析构，验证 ~LoadedSo() 调用 dlclose 且无 use-after-free
