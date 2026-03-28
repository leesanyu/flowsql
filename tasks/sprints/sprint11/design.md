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
- 命名收敛：插件宿主命名为 **BinAddonHostPlugin**（`binaddon`），强调“管理外部编译式 addon”，避免与算子插件本体混淆，且不绑定 C++ 语言
- URI 统一：不新增类型化路由；`/api/operators/list` 按 `type` 返回对应列表，`/api/operators/detail` 负责单项详情
- 内部列表 URI 统一为 `/operators/list`（不保留 `/operators/query`）
- 上传分层：WebPlugin 仅做上传入口与转发，不在 Web 层执行算子业务逻辑
- C++ 插件摘要元数据由 `/operators/list` 一次性返回（避免前端 N+1 请求）
- `/operators/detail` 主要承载 Python 深度信息（如代码内容）；C++ 详情接口保留用于兼容与单项核查

**术语与标识约束（本 Story 强制）**：
- `type`：算子类型，枚举固定为 `builtin | python | cpp`
- `category`：算子分类
- `name`：算子名
- `operator_id = category.name`（SQL/执行路径保持两段式）
- 唯一性：`operator_catalog` 以 `(category, name)` 为唯一键；`type` 不参与唯一索引
- 保留算子分类：`category=builtin` 保留给内置算子
- 一致性：`type=builtin => category=builtin`，且 `category=builtin => type=builtin`

**命名统一约束（`category`）**：
- DB：`operator_catalog` 列名统一为 `category`，唯一索引为 `(category, name)`
- API：请求与响应统一使用 `category`，不再接受旧字段名
- 内部代码：统一使用 `Category()/category`，不保留别名适配层
- 排除项：`CatalogPlugin` 插件名及既有 URI 中若出现 `catalog` 且表示插件/服务语义时，保持不变，不在本次字段重命名范围内

**Web 展示需求（Story 12.2 补充）**：
- 插件 so 文件名（`so_file`）
- ABI 版本（`abi_version`）
- 大小（字节数，`size_bytes`）
- SHA256 指纹（`sha256`）
- 插件算子总数（`operator_count`）
- 同插件算子名称列表（`operators`）
- 前端展示策略：按当前 `type` 调一次 `/api/operators/list?type=...` 渲染；禁止按行批量调用 `/api/operators/detail`（N+1）

---

### 架构设计

```
CatalogPlugin（统一 /operators/* 入口）
  │
  ├── /operators/upload：按 type 分发上传处理（Web 仅转发 tmp_path）
  ├── /operators/activate：type=builtin/python 按 operator_id；type=cpp 按 plugin_id
  ├── /operators/deactivate：type=builtin/python 按 operator_id；type=cpp 按 plugin_id
  ├── /operators/list：按 type 返回对应列表（cpp 为插件列表，builtin/python 为算子列表）
  ├── /operators/delete：type=cpp 按 plugin_id 删除插件文件与元数据
  └── /operators/detail：单算子详情；Python 为主要消费方，C++ 保留
  
BinAddonHostPlugin（IPlugin，二进制 Addon 宿主管理器）
  │
  ├── 激活：plugin_id -> path -> dlopen -> ABI 校验 -> 枚举算子 -> 注册到 IOperatorRegistry（key="category.name"）
  ├── 去激活：标记 pending_unload → 检查 active_count → 拒绝 or 注销工厂 + 移除 map
  │           （dlclose 由 LoadedSo 析构函数负责，shared_ptr 引用计数归零时自动触发）
  │
  └── loaded_sos_: unordered_map<path, shared_ptr<LoadedSo>>
                      ├── handle（dlopen 返回，析构时 dlclose）
                      ├── pending_unload（atomic<bool>，标记后工厂函数拒绝创建新实例）
                      ├── active_count（atomic<int>，存活的 BinAddonOperatorProxy 实例数）
                      ├── operator_keys（已注册的 "category.name" 列表）
                      └── plugin_meta（so_file/abi_version/size_bytes/sha256/operator_count/operators）
```

**plugin_id 与上传态管理**：
- `plugin_id` 由业务层在上传时生成（建议直接使用完整 `sha256`，64 位小写 hex，稳定且全局唯一）
- BinAddonHostPlugin 维护上传态清单 `uploaded_plugins_`（`plugin_id -> path + so_file + size_bytes + sha256 + status`）
- `POST /operators/activate|deactivate` 的 `type=="cpp"` 分支只接受 `plugin_id`，不接受前端直接传文件路径
- `ActivateByPath/DeactivateByPath` 仅作为内部实现细节，对外由 `plugin_id -> path` 映射后调用
- 重复上传策略：当 `sha256` 已存在时，upload 返回冲突（409），要求先执行 `/operators/delete` 删除旧插件后再上传

**`plugin_id` 持久化（本 Story 必做）**：
- 新增持久化表：`operator_plugin_store`
- 推荐字段：
  - `plugin_id TEXT PRIMARY KEY`
  - `type TEXT NOT NULL`（固定 `cpp`）
  - `so_file TEXT NOT NULL`
  - `file_path TEXT NOT NULL`
  - `size_bytes INTEGER NOT NULL`
  - `sha256 TEXT NOT NULL`
  - `status TEXT NOT NULL`（`uploaded|activated|deactivated|broken`）
  - `last_error TEXT NOT NULL DEFAULT ''`
  - `created_at/updated_at DATETIME`
- 唯一性说明：当前策略 `plugin_id == sha256` 时，`plugin_id` 主键已天然唯一；`UNIQUE(sha256)` 可作为防御性冗余约束保留
- 启动恢复：服务启动时重建内存 `uploaded_plugins_` 索引；仅在启动阶段对 `status=activated` 的插件自动重激活一次；若 `file_path` 丢失则置 `status=broken` 并记录 `last_error`
- 自动重试约束：不做后台循环自动重激活；启动自动重激活失败后立即置 `status=broken`，后续仅允许用户手动触发激活重试
- 状态流转：`uploaded -> activated -> deactivated`；任一阶段发现文件损坏/缺失/激活失败进入 `broken`；`broken` 仅可通过用户手动激活重试进入 `activated`

**上传预处理（upload 阶段）**：
- C++ upload 成功不等于激活：upload 阶段只做落盘 + 基础元数据采集（`plugin_id/so_file/file_path/size_bytes/sha256`）
- upload 阶段不加载 .so，不进行 ABI 校验，不探测 `(category,name)` 清单
- upload 后仅写入 `operator_plugin_store`（`status=uploaded`），不写入 `operator_catalog`
- 激活阶段负责加载 .so、校验 ABI、探测算子清单、冲突检测、注册与目录写入

**数据模型补充（本 Story 必做）**：
- `operator_catalog` 需新增可空字段 `plugin_id`（builtin/python 为 `NULL`，cpp 为非空）
- 约束建议：
  - `type='cpp' => plugin_id IS NOT NULL`
  - `type IN ('builtin','python') => plugin_id IS NULL`
  - `FOREIGN KEY (plugin_id) REFERENCES operator_plugin_store(plugin_id)`（按现有 DB 能力可降级为应用层校验）
- 简化事务边界：采用“两阶段顺序”而非跨表长事务
  - 阶段 1（upload）：先写 `operator_plugin_store`
  - 阶段 2（activate）：再注册 registry 并写 `operator_catalog`
  - 任一失败均回写 `last_error`，并保持非激活态

**active_count 语义**：记录当前存活的 `BinAddonOperatorProxy` 实例数，即"正在被任务持有、尚未析构的算子实例数"。
每次 `Create()` 返回一个 `BinAddonOperatorProxy`，构造时 `active_count++`，析构时 `active_count--`。
线程池并发时同一算子类型可有多个实例（`active_count > 1`），各任务持有独立实例互不干扰。
去激活时 `active_count > 0` 意味着有任务正在使用该 .so 的代码段，此时 dlclose 会导致崩溃，故拒绝。

**算子查找路径**：SchedulerPlugin::FindOperator 现有逻辑：
1. 遍历 IID_OPERATOR（静态注册算子）
2. IBridge（Python 算子）
3. IOperatorRegistry::Create（仅 category=="builtin"）

C++ 动态算子通过**扩展第 3 步**支持：去掉 `category=="builtin"` 限制，改为对所有 category 查询
IOperatorRegistry。注册时 key 使用 `"category.name"` 格式（如 `"mylib.encrypt"`）；若与现有
`operator_catalog` 唯一键 `(category, name)` 冲突则激活失败并回滚。

---

### 从当前实现迁移到独立 `binaddon` 插件库（实施步骤）

> 当前代码状态：C++ 插件生命周期能力已可用，但实现主要内聚在 `CatalogPlugin`。本节定义如何回到“独立宿主插件库 + Catalog 统一入口分发”的目标架构。

**阶段 1：能力拆分（保持外部契约不变）**
1. 新建 `src/services/binaddon/`，承载 `dlopen/dlsym`、生命周期状态、并发控制、元数据聚合。
2. 保持外部 API 不变：`/api/operators/*` 与 `/operators/*` 继续使用 `type=cpp + plugin_id` 契约。
3. `CatalogPlugin` 仅保留分发：`type=cpp` 的 upload/activate/deactivate/delete/detail 全部委派 `BinAddonHostPlugin`。

**阶段 2：构建与部署对齐**
1. 主工程新增 `add_subdirectory(services/binaddon)`，生成 `libflowsql_binaddon.so`。
2. `deploy-single.yaml` 与 `deploy-multi.yaml` 的 scheduler 插件列表显式加载 `libflowsql_binaddon.so`。
3. 清理旧命名路径（`cpp_operator` 目录/target/注释）避免双语义并存。

**阶段 3：回归与验收**
1. 架构一致性验收：目录、target、deploy 清单与设计逐项一致。
2. 行为回归：upload/activate/deactivate/delete/list/detail 全链路通过，前端交互不变。
3. 启动恢复回归：`status=activated` 插件仅在启动阶段自动重激活；失败进入 `broken` 且 `last_error` 可见。
4. 测试门槛：`test_binaddon` 必须通过 PluginLoader 实际加载 `libflowsql_binaddon.so`，禁止只测类级单测。

---

### 新增文件

```
src/services/binaddon/
├── binaddon_host_plugin.h    # BinAddonHostPlugin + LoadedSo 声明
├── binaddon_host_plugin.cpp  # 实现
├── binaddon_operator_proxy.h     # BinAddonOperatorProxy（header-only）
└── CMakeLists.txt           # 编译为 libflowsql_binaddon.so

samples/cpp_operator/
├── CMakeLists.txt           # 独立编译模板（不依赖主工程构建图，依赖 SDK 头）
├── include/
│   └── flowsql_operator_sdk.h  # 随 SDK 分发的头文件（直接 include 主程序接口）
├── src/
│   └── sample_operators.cpp    # 示例：ColumnStatsOperator（count/min/max/mean）
└── README.md

src/tests/test_binaddon/
├── test_binaddon.cpp    # 测试
├── fixture_operator.cpp     # 测试用 fixture .so 源码（含阻塞点）
├── fixture_bad_abi.cpp      # ABI 版本不匹配的 fixture .so
└── CMakeLists.txt
```

**修改文件**：

```
src/framework/interfaces/ioperator_registry.h  # 新增 RemoveFactory() 纯虚方法
src/framework/interfaces/ioperator_catalog.h   # 新增 SetActive() 纯虚方法
src/services/catalog/catalog_plugin.h/cpp      # 实现 RemoveFactory/SetActive；内置算子双 key 注册
src/services/scheduler/scheduler_plugin.cpp    # FindOperator 扩展支持非 builtin category
src/CMakeLists.txt                             # 添加 add_subdirectory(services/binaddon)
config/deploy-single.yaml                      # 插件列表添加 libflowsql_binaddon.so
src/services/web/web_server.cpp                # 复用统一 /api/operators/* 路由，不新增 /api/operators/cpp/*
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
//   common/typedef.h（interface 宏）
//   common/span.h（Span<T>）
#pragma once
#include <common/typedef.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ichannel.h>
#include <common/span.h>

// 当前 ABI 版本，主程序加载时校验
#define FLOWSQL_ABI_VERSION 1

// 导出可见性：配合 -fvisibility=hidden，确保 dlsym 可见
#if defined(_WIN32)
#define FLOWSQL_SDK_EXPORT __declspec(dllexport)
#else
#define FLOWSQL_SDK_EXPORT __attribute__((visibility("default")))
#endif

// 插件必须导出的 4 个 C 函数（extern "C" 保证无 name mangling）
extern "C" {
    FLOWSQL_SDK_EXPORT int flowsql_abi_version();          // 返回 FLOWSQL_ABI_VERSION
    FLOWSQL_SDK_EXPORT int flowsql_operator_count();       // 返回此 .so 包含的算子数量
    FLOWSQL_SDK_EXPORT flowsql::IOperator* flowsql_create_operator(int index);   // 按索引创建实例
    FLOWSQL_SDK_EXPORT void flowsql_destroy_operator(flowsql::IOperator* op);    // 销毁实例
}
```

> **vtable 一致性保证**：SDK 头文件直接复用主程序的 `ioperator.h`，`IOperator` 只有一份定义，
> 虚函数表布局由同一份头文件决定，消除两份定义导致的 vtable 偏移错位风险。

> **异常隔离**：`flowsql_create_operator` 内部必须 `try/catch(...)` 所有异常并返回 nullptr，
> 禁止异常跨 .so 边界传播（SEI CERT ERR59-CPP）。

#### 2. IOperatorRegistry 扩展：RemoveFactory

```cpp
// ioperator_registry.h 新增
virtual int RemoveFactory(const char* name) = 0;  // 注销算子工厂；不存在时返回 -1
```

CatalogPlugin 实现：加锁从 `op_factories_` map 中 erase，返回 0（成功）或 -1（不存在）。

#### 3. IOperatorCatalog 扩展：SetActive

```cpp
// ioperator_catalog.h 新增
virtual int SetActive(const std::string& category,
                      const std::string& name,
                      bool active) = 0;
```

CatalogPlugin 实现：包装现有私有方法 `SetOperatorActive(category, name, active ? 1 : 0)`。

#### 4. LoadedSo 与生命周期管理

```cpp
// binaddon_host_plugin.h 内
struct LoadedSo {
    void*  handle = nullptr;
    std::string path;
    std::string so_file;   // 文件名（basename）
    int abi_version = 0;
    uint64_t size_bytes = 0;
    std::string sha256;    // 小写 hex，64 字符
    std::atomic<int>  active_count{0};    // 存活的 BinAddonOperatorProxy 实例数
    std::atomic<bool> pending_unload{false};  // 标记后工厂函数拒绝创建新实例
    int        (*count_fn)()  = nullptr;
    IOperator* (*create_fn)(int) = nullptr;
    void       (*destroy_fn)(IOperator*) = nullptr;
    std::vector<std::string> operator_keys;  // "category.name" 格式
    std::vector<std::string> operators;      // 算子名称列表（仅 name，不含 category）

    ~LoadedSo() {
        // dlclose 在此处调用，保证所有 BinAddonOperatorProxy 析构后才执行
        // （shared_ptr 引用计数归零时触发）
        if (handle) { dlclose(handle); handle = nullptr; }
    }
};
```

```cpp
// binaddon_host_plugin.h 内
struct UploadedPluginMeta {
    std::string plugin_id;   // 固定等于 sha256
    std::string so_file;
    std::string file_path;
    uint64_t size_bytes = 0;
    std::string sha256;
    std::string status;      // uploaded|activated|deactivated|broken
    std::string last_error;
};
```

**生命周期说明**：
- `loaded_sos_` 持有 `shared_ptr<LoadedSo>`（引用计数 = 1）
- 每个 `BinAddonOperatorProxy` 也持有 `shared_ptr<LoadedSo>`（引用计数 += 1）
- `DeactivateByPath` 从 map 移除后引用计数 -= 1；所有 Proxy 析构后引用计数归零
- 引用计数归零时 `~LoadedSo()` 自动调用 `dlclose`，此时 `destroy_fn` 已不再被任何 Proxy 调用

**插件元数据采集策略**：
- `so_file`：由规范化后的 `path` 取 basename
- `abi_version`：仅在 activate 成功后由 `flowsql_abi_version()` 采集并缓存
- `size_bytes`：由 `stat(path)` 获取
- `sha256`：upload 时读取文件内容计算并缓存（建议 `OpenSSL::SHA256`）；activate 只做读取与校验
- `operator_count`：仅在 activate 成功后取 `operators.size()`
- `operators`：仅在 activate 成功后读取每个算子的 `Name()` 形成列表
- 一致性约束：`operator_count` 必须等于 `operators.size()`，且 `operators` 仅包含当前 .so 的算子名

> 性能约束：SHA256 仅在 upload 时计算；activate/detail 直接读取缓存值，不重复读文件。

#### 5. BinAddonOperatorProxy

```cpp
// binaddon_operator_proxy.h — header-only
// 注意：此文件只在 binaddon_host_plugin.cpp 中使用，不暴露给其他插件
class BinAddonOperatorProxy : public IOperator {
public:
    BinAddonOperatorProxy(IOperator* impl, std::shared_ptr<LoadedSo> so)
        : impl_(impl), so_(so) {
        so_->active_count.fetch_add(1, std::memory_order_acq_rel);
    }

    ~BinAddonOperatorProxy() override {
        so_->destroy_fn(impl_);  // 在 .so 内部 delete，保证 allocator 一致
        impl_ = nullptr;
        so_->active_count.fetch_sub(1, std::memory_order_acq_rel);
        // so_ shared_ptr 在此析构，引用计数 -1；若归零则触发 ~LoadedSo() → dlclose
    }

    std::string Category()     override { return impl_->Category(); }
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

#### 6. BinAddonHostPlugin

```cpp
class BinAddonHostPlugin : public IPlugin {
public:
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override { return 0; }
    int Stop()  override { return 0; }

    // 供 CatalogPlugin 的统一 URI 入口调用
    int ActivateByPath(const std::string& path, std::string& rsp);
    int DeactivateByPath(const std::string& path, std::string& rsp);
    int DeleteByPath(const std::string& path, std::string& rsp);
    int GetPluginMetaByPluginId(const std::string& plugin_id, std::string& rsp);

private:
    IOperatorRegistry* registry_ = nullptr;
    IOperatorCatalog*  catalog_  = nullptr;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<LoadedSo>> loaded_sos_;
    std::unordered_map<std::string, UploadedPluginMeta> uploaded_plugins_;  // key=plugin_id
};
```

**ActivateByPath 流程**：

```
1. 输入 path（由上层根据 `plugin_id` 解析得到，不直接来自前端）；realpath() 规范化路径，防止路径穿越
2. 检查文件存在且可读；可选：校验是否在 allowed_dir 白名单内
3. 加锁（mu_），检查 path 是否已加载（重复激活返回 409）
4. 解锁，在锁外执行 dlopen(path, RTLD_NOW | RTLD_LOCAL)
   （dlopen 可能耗时，不持锁避免阻塞其他 HTTP 请求）
5. dlsym 获取 4 个导出函数，任一缺失则 dlclose + 返回错误
6. 校验 flowsql_abi_version() == FLOWSQL_ABI_VERSION，不匹配则 dlclose + 返回错误
7. 探测阶段（不注册）：循环 flowsql_operator_count()，对每个 index：
   a. flowsql_create_operator(index) 探测元数据（Category/Name）
   b. key = category + "." + name；收集到 keys 列表
   c. 记录 name 到 operators 列表（供 list/detail 的插件元数据展示）
   d. flowsql_destroy_operator 销毁探测实例
   e. 任一步骤失败 → dlclose + 返回错误（此阶段无注册，无需回滚）
8. 冲突检测：若任一 `(category,name)` 与现有 `operator_catalog` 冲突（含 builtin/python/cpp），则 dlclose + 标记失败 + 返回错误
9. 采集插件元数据（不注册）：
   a. so_file = basename(path)
   b. abi_version = flowsql_abi_version()
   c. size_bytes = stat(path).st_size
   d. sha256 = 读取 upload 阶段缓存值（可选：与当前文件重算值比对，不一致则拒绝激活）
10. 加锁（mu_），再次检查 path 是否已加载（防止并发重复激活）
11. 注册阶段（全量成功或全量回滚）：
   a. 逐一向 IOperatorRegistry 注册工厂函数（key 为 "category.name"）
   b. 任一 Register 失败 → 对已注册的 key 逐一 RemoveFactory 回滚 → dlclose + 返回错误
12. 向 IOperatorCatalog::UpsertBatch 写入元数据（type="cpp"，active=1，带 plugin_id）
    失败时：逐一 RemoveFactory 回滚 → dlclose + 返回错误
13. 将 operators/operator_count 与插件元数据写入 LoadedSo
14. 保存 shared_ptr<LoadedSo> 到 loaded_sos_
15. 解锁，返回 200 + 已注册算子列表
```

> **失败回滚保证**：探测阶段（步骤 7-9）失败不产生副作用；注册阶段（步骤 11-12）失败时
> 对已注册的工厂函数全量回滚，确保不留脏状态。
> **失败可观测性**：若激活失败（ABI 不匹配、符号缺失、算子重名冲突等），需写入 `operator_plugin_store.last_error`，并将 `status=broken`、`active=0`，供 `/operators/list` 查询展示。

**UploadByPath 流程**（upload 阶段，预处理）：

```
1. 输入 tmp_path + filename（来自 CatalogPlugin /operators/upload 分发）
2. 业务层将 tmp 文件原子移动到持久目录（如 `./uploads/binaddon/`）
3. 计算 sha256，生成 plugin_id（固定等于 sha256）；读取 size_bytes
4. 若 plugin_id 已存在：
   a. 返回 409（already exists）
   b. 响应提示“请先 delete 旧插件，再 upload 新插件”
5. 写入 operator_plugin_store（status=uploaded，last_error 置空）
6. upload 阶段不执行 dlopen/dlsym，不写入 operator_catalog
7. 返回 upload 响应（plugin_id/so_file/size_bytes/sha256/status）
```

**DeactivateByPath 流程**：

```
1. 输入 path（由上层根据 `plugin_id` 解析得到）
2. 加锁（mu_），查找 LoadedSo，不存在返回 404
3. 设置 so->pending_unload.store(true)（阻止工厂函数创建新实例）
4. 检查 active_count：
   - active_count > 0 → 重置 pending_unload.store(false)，解锁，返回 409
5. 从 IOperatorRegistry 逐一 RemoveFactory(key)
6. 从 IOperatorCatalog 逐一 SetActive(category, name, false)
7. 从 loaded_sos_ 移除（shared_ptr 引用计数 -1）
   - 若此时无存活 Proxy（active_count==0），引用计数归零，~LoadedSo() 自动 dlclose
8. 解锁，返回 200
```

**DeleteByPath 流程**：

```
1. 输入 path（由上层根据 `plugin_id` 解析得到）
2. 若插件仍在 loaded_sos_（已激活或正在使用）则返回 409
3. 删除 operator_catalog 中该 plugin_id 对应的全部记录（仅 cpp）
4. 删除 operator_plugin_store 中该 plugin_id 记录
5. 删除磁盘文件（不存在时记告警但不阻塞删除元数据）
6. 返回 200
```

**GetPluginMetaByPluginId 流程**（供 `/operators/detail` 在 `type=="cpp"` 时复用）：

```
1. 输入 plugin_id
2. 加锁（mu_），在 `uploaded_plugins_` / `loaded_sos_` 中定位插件元数据
3. 组装插件元数据：
   - so_file
   - abi_version
   - size_bytes
   - sha256
   - operator_count
   - operators
4. 返回给 CatalogPlugin，由 `/operators/detail` 拼装到单算子详情响应中
```

> **并发安全说明**：
> - `pending_unload` 是 `atomic<bool>`，工厂 lambda 在 `CatalogPlugin::mu_` 下读取，
>   `DeactivateByPath` 在 `BinAddonHostPlugin::mu_` 下写入，两把锁不同但 atomic 保证无数据竞争。
> - 步骤 3 设置 `pending_unload=true` 后，新的 `Create()` 调用返回 nullptr，不再增加 `active_count`。
> - 步骤 4 检查 `active_count` 时，已在途的 `Work()` 调用仍持有 Proxy，`active_count > 0` 触发拒绝。
> - `dlclose` 由 `~LoadedSo()` 负责，时机由 `shared_ptr` 引用计数保证，不存在 destroy_fn 悬空问题。

#### 7. SchedulerPlugin 修改

`FindOperator` 第 3 步扩展支持非 builtin category：

```cpp
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

CatalogPlugin::Load() 中内置算子同时注册两个 key：

```cpp
Register("passthrough",         []() -> IOperator* { return new PassthroughOperator(); });
Register("builtin.passthrough", []() -> IOperator* { return new PassthroughOperator(); });
// concat / hstack 同理
```

#### 8. HTTP API

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/operators/upload` | 统一上传入口：`{"type":"python|cpp","filename":"...","tmp_path":"..."}` |
| POST | `/operators/activate` | 统一激活入口：builtin/python 用 `{"type":"builtin|python","name":"category.op"}`；cpp 用 `{"type":"cpp","plugin_id":"..."}` |
| POST | `/operators/deactivate` | 统一去激活入口：builtin/python 用 `{"type":"builtin|python","name":"category.op"}`；cpp 用 `{"type":"cpp","plugin_id":"..."}` |
| POST | `/operators/delete` | 插件删除入口：cpp 用 `{"type":"cpp","plugin_id":"..."}`，删除文件与元数据 |
| POST | `/operators/list` | 类型化列表（对应 Web `GET /api/operators/list?type=...` 转发）：请求体携带 `type`，`type=cpp` 返回插件列表，`type=builtin|python` 返回算子列表 |
| POST | `/operators/detail` | 单项详情（对应 Web `POST /api/operators/detail`）：`type=builtin|python` 用 `{"type":"...","name":"category.op"}`；`type=cpp` 用 `{"type":"cpp","plugin_id":"..."}` |

响应风格与 Python 算子 API 保持一致（`{"code":0,"message":"ok","data":{...}}`）。

**请求体契约（实现约束）**：
- `POST /operators/upload`
  - 必填：`type`、`filename`、`tmp_path`
  - 限定：`type in {"python","cpp"}`
- `POST /operators/activate`
  - `type in {"builtin","python"}`：必填 `name`（`category.name`）
  - `type=="cpp"`：必填 `plugin_id`（64 位小写 hex）
- `POST /operators/deactivate`
  - 与 activate 相同
- `POST /operators/delete`
  - 限定：`type=="cpp"` 且必填 `plugin_id`
- `POST /operators/detail`
  - `type in {"builtin","python"}`：必填 `name`
  - `type=="cpp"`：必填 `plugin_id`
- `POST /operators/list`
  - 必填请求体：`{"type":"builtin|python|cpp"}`
  - 响应按 `type` 变化：`cpp` 为插件维度；`builtin/python` 为算子维度
- 校验失败统一返回 400，错误消息包含缺失字段名或格式错误字段名

**Web 上传分层与调用链（重构约束）**：
1. 前端上传文件到 Web（multipart），Web 只负责落临时文件（如 `./uploads/tmp/...`）
2. Web 组装统一业务请求并转发到 CatalogPlugin：`POST /operators/upload`（仅透传 `type/filename/tmp_path`）
3. Web 处理列表查询：`GET /api/operators/list?type=...` 转换为内部 `POST /operators/list`，并将 `type` 写入 JSON body
4. CatalogPlugin 按 `type` 分发：
   - `python`：委派 BridgePlugin 完成落盘、reload、catalog 刷新
   - `cpp`：委派 BinAddonHostPlugin 完成落盘、sha256/plugin_id 与上传登记（不加载 so）
5. Web 不允许直接创建业务目录（如 python operators 目录）、不允许执行插件探测/激活业务逻辑

> 约束目标：`WebServer::HandleUploadOperator` 仅做 ingress/转发，不承载算子业务语义。

**CatalogPlugin 统一分发规则**：

1. `POST /operators/upload`：按 `type` 分发上传处理；Web 仅提供 `tmp_path`，业务插件负责最终落盘与元数据登记  
2. `POST /operators/activate`：`type=="cpp"` 时先以 `plugin_id` 查询内部插件记录并取 path，再调用 `BinAddonHostPlugin::ActivateByPath`；其余类型按 `name` 走现有激活逻辑  
3. `POST /operators/deactivate`：`type=="cpp"` 时先以 `plugin_id` 查询内部插件记录并取 path，再调用 `BinAddonHostPlugin::DeactivateByPath`；其余类型按 `name` 走现有去激活逻辑  
4. `POST /operators/delete`：`type=="cpp"` 时先以 `plugin_id` 查询 path，再调用 `BinAddonHostPlugin::DeleteByPath`，清理文件与元数据  
5. `POST /operators/list`：按请求体 `type` 返回对应列表：
   - `type=="cpp"`：返回插件列表（每插件一行），携带 `plugin` 摘要（`so_file/size_bytes/sha256/status/abi_version/operator_count/operators/last_error`，未激活时 `abi_version/operator_count/operators` 置空）
   - `type in {"builtin","python"}`：返回算子列表（维持既有结构）
6. `POST /operators/detail`：默认用于 Python 算子深度信息（如代码内容）；`type=="cpp"` 时按需返回插件详情，接口语义保留

**上传后可见规则（本 Story 必做）**：
1. Python 上传成功后：由业务层完成落盘 + Worker reload + Catalog refresh，确保 `/operators/list` 可见且 `active=0`
2. C++ 上传成功后：写入 `operator_plugin_store`，并在 `POST /operators/list`（`type=cpp`）产出插件记录（`active=0`，算子字段待激活后补齐）
3. C++ 未激活展示语义：
   - 必填：`plugin_id/so_file/size_bytes/sha256/status/last_error`
   - 置空：`abi_version/operator_count/operators`
4. C++ 激活成功后：补齐 `abi_version/operator_count/operators`，并将状态更新为 `activated`
5. C++ 去激活后：保留 `operator_catalog` 记录（`active=0`），不删除上传态记录，支持按 `plugin_id` 再激活
6. C++ 激活失败后：写入 `last_error`，置 `status=broken` 且 `active=0`，失败原因可在 `POST /operators/list`（`type=cpp`）查询

**C++ upload 响应契约（补充）**：
- 返回 `plugin_id`（=sha256）、`so_file`、`size_bytes`、`sha256`、`status`
- 当命中重复上传冲突时，返回 409 与已存在 `plugin_id`

**`POST /operators/list`（`type=cpp`）返回模型（C++）**：
- 返回插件列表语义：每个 C++ 插件一行（不是“每个算子一行”）
- 同一插件导出的多个算子放在 `plugin.operators` 数组中
- 前端按 `type` 分页/分类后处理，允许与 `builtin/python` 不同的数据结构

> 分层约束：以上动作均由业务插件执行；WebPlugin 仅负责接收上传和转发 `tmp_path`。

`POST /operators/list`（请求体 `{"type":"cpp"}`）响应示例：

```json
{
  "operators": [
    {
      "type": "cpp",
      "active": 0,
      "plugin_id": "f6f4b0c8d5b7d7a50d39d3a67a8c7e675975f645dd5bb58b2f9b2585fa1313d4",
      "plugin": {
        "so_file": "libsample_cpp_operator.so",
        "size_bytes": 24576,
        "sha256": "f6f4b0c8d5b7d7a50d39d3a67a8c7e675975f645dd5bb58b2f9b2585fa1313d4",
        "status": "uploaded",
        "abi_version": null,
        "operator_count": null,
        "operators": null,
        "last_error": ""
      }
    }
  ]
}
```

`POST /operators/list`（`type=cpp`）字段契约（列表项）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 算子类型：`builtin|python|cpp` |
| `active` | int/bool | 激活状态（0/1） |
| `plugin_id` | string | 插件唯一标识（固定等于 sha256） |
| `plugin.so_file` | string | 插件文件名 |
| `plugin.size_bytes` | int64 | 插件文件字节数 |
| `plugin.sha256` | string | 64 位小写 hex |
| `plugin.status` | string | `uploaded|activated|deactivated|broken` |
| `plugin.abi_version` | int/null | 已激活时返回；未激活为空 |
| `plugin.operator_count` | int/null | 已激活时返回；未激活为空 |
| `plugin.operators` | array<string>/null | 已激活时返回；未激活为空 |
| `plugin.last_error` | string | 最近一次激活失败原因；无错误时为空字符串 |

> 一致性约束：
> - `plugin_id == plugin.sha256`（本 Story 固定策略，避免多重 ID 语义）。
> - C++ 激活态下 `plugin.operator_count == plugin.operators.length`。
> - `POST /operators/list`（`type=cpp`）中同一 `plugin_id` 只允许出现一行记录。

`POST /operators/detail`（`type=cpp`，按 `plugin_id`）响应示例：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "type": "cpp",
    "plugin_id": "f6f4b0c8d5b7d7a50d39d3a67a8c7e675975f645dd5bb58b2f9b2585fa1313d4",
    "source": "libsample_cpp_operator.so",
    "active": 1,
    "plugin": {
      "path": "/opt/flowsql/plugins/libsample_cpp_operator.so",
      "so_file": "libsample_cpp_operator.so",
      "abi_version": 1,
      "size_bytes": 24576,
      "sha256": "f6f4b0c8d5b7d7a50d39d3a67a8c7e675975f645dd5bb58b2f9b2585fa1313d4",
      "operator_count": 2,
      "operators": ["column_stats", "zscore_norm"]
    }
  }
}
```

**Web 展示字段映射（Story 12.2）**：

| 字段 | 来源 | 展示说明 |
|------|------|----------|
| `so_file` | `/operators/list.operators[i].plugin.so_file` | 插件文件名 |
| `abi_version` | `/operators/list.operators[i].plugin.abi_version` | ABI 版本号（未激活时为空） |
| `size_bytes` | `/operators/list.operators[i].plugin.size_bytes` | 文件字节数（原值显示） |
| `sha256` | `/operators/list.operators[i].plugin.sha256` | 64 位小写 hex 指纹 |
| `status` | `/operators/list.operators[i].plugin.status` | 插件状态（uploaded/activated/deactivated/broken） |
| `operator_count` | `/operators/list.operators[i].plugin.operator_count` | 同插件算子总数（未激活时为空） |
| `operators` | `/operators/list.operators[i].plugin.operators` | 同插件算子名称列表（未激活时为空） |
| `last_error` | `/operators/list.operators[i].plugin.last_error` | 最近一次激活失败原因（无错误时为空） |

**Web 展示分层规范（本 Story 强制）**：
- 必显字段（C++ 列表主表）：
  - `so_file`
  - `size_bytes`
  - `sha256`
  - `status`
- 条件显示字段：
  - `last_error`（当 `status=="broken"` 时必须显示；其他状态可为空或隐藏）
  - `abi_version`（仅 `status=="activated"` 时显示）
  - `operator_count`（仅 `status=="activated"` 时显示）
  - `operators`（仅 `status=="activated"` 时显示）
- 非主展示字段（不占主表列，供交互/调试）：
  - `plugin_id`（建议放“复制ID”按钮或展开区）
  - `active`（与 `status` 语义重叠，不单独主展示）

> 前端与后端统一契约：`operator_count == operators.length`（激活态），不一致视为后端数据错误。
> 前端默认仅调用一次 `/api/operators/list` 渲染；`/api/operators/detail` 仅在用户进入单算子深度查看时调用。

---

### 实现前清单

| 接口 | 状态机 | 测试断言 |
|------|--------|----------|
| `POST /operators/upload`（`type=cpp`） | 成功后 `status=uploaded`，不触发 dlopen，不写 `operator_catalog` | 上传后 `operator_plugin_store` 有记录；`/operators/list`（`type=cpp`）可见一条插件记录，`abi_version/operator_count/operators` 为空 |
| `POST /operators/activate`（`type=cpp`） | 成功后 `status=activated`、`active=1`；失败后 `status=broken`、`active=0` 且写 `last_error` | ABI 不匹配/符号缺失/重名冲突均返回失败且可在 list 中看到 `last_error` |
| `POST /operators/deactivate`（`type=cpp`） | `active_count>0` 返回 409；成功后 `status=deactivated`、`active=0` | 去激活后无法再 Create；并发下无 crash，pending_unload 回滚正确 |
| `POST /operators/delete`（`type=cpp`） | 仅未激活可删；成功后清理 store+catalog+文件 | 激活态 delete 返回 409；delete 成功后 list 中不再出现该 `plugin_id` |
| `POST /operators/list`（`type=cpp`） | 每插件一行；同一 `plugin_id` 仅一行 | `plugin.operator_count == plugin.operators.length`（激活态）；字段含 `status/last_error` |
| `POST /operators/list`（`type=builtin|python`） | 维持既有算子列表语义 | 与现网行为一致，无 cpp 字段污染 |
| `POST /operators/detail`（`type=cpp`, `plugin_id`） | 按插件返回详情，不按算子名路由 | detail 返回与 list 公共字段一致，且包含插件路径/元数据 |
| 启动恢复流程 | 仅启动阶段对 `status=activated` 自动重激活一次；失败转 `broken`，不自动重试 | 重启后激活态插件可恢复；失败插件不被后台循环重试，仅可手动激活 |
| 重复上传（同 `sha256`） | 返回 409，要求先 delete 再 upload | 响应包含已存在 `plugin_id`；不会覆盖旧记录 |

> 执行顺序建议：先完成接口契约与数据落库，再实现状态机分支，最后补齐断言级测试。

---

### Sample 工程设计

`samples/cpp_operator/` 是独立 CMake 工程（不依赖主工程构建图，但依赖 FlowSQL SDK 头文件）：

```cmake
# samples/cpp_operator/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(sample_cpp_operator VERSION 1.0.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FlowSQL SDK 头文件根路径（通常指向 src/，含 ioperator.h / ichannel.h / common/typedef.h）
set(FLOWSQL_SRC_INCLUDE "" CACHE PATH "FlowSQL src/ 目录路径")
if(NOT FLOWSQL_SRC_INCLUDE)
    message(FATAL_ERROR "请设置 FLOWSQL_SRC_INCLUDE，例如：cmake -DFLOWSQL_SRC_INCLUDE=/path/to/flowsql/src")
endif()

# 编译器版本下限（与主程序 ABI 约束一致）
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
    message(FATAL_ERROR "GCC >= 11 is required")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "Clang >= 14 is required")
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
src/tests/test_binaddon/
├── test_binaddon.cpp    # 测试主文件（通过 PluginLoader 加载插件，不直接链接）
├── fixture_operator.cpp     # 正常 fixture .so（含可控阻塞点，通过 Configure 注入）
├── fixture_bad_abi.cpp      # ABI 版本不匹配 fixture .so
└── CMakeLists.txt           # target_compile_options PRIVATE -UNDEBUG
```

**测试路径原则**（L9）：集成测试必须走完整生产路径。
测试通过 `PluginLoader` 加载 `libflowsql_catalog.so` 和 `libflowsql_binaddon.so`，
不直接链接这两个 .so，避免 double-loading 导致堆损坏。

**fixture .so 构建方式**：

```cmake
# 正常 fixture（flowsql_abi_version 返回正确版本）
add_library(fixture_operator SHARED fixture_operator.cpp)
target_include_directories(fixture_operator PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(fixture_operator PRIVATE -fvisibility=hidden)

# ABI 不匹配 fixture（flowsql_abi_version 返回 999）
add_library(fixture_bad_abi SHARED fixture_bad_abi.cpp)
target_include_directories(fixture_bad_abi PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(fixture_bad_abi PRIVATE -fvisibility=hidden)

# 测试主程序：只依赖 flowsql_common，通过 PluginLoader 动态加载其他插件
add_executable(test_binaddon test_binaddon.cpp)
target_include_directories(test_binaddon PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(test_binaddon arrow rapidjson sqlite)
add_dependencies(test_binaddon flowsql_common flowsql_catalog flowsql_binaddon
                 fixture_operator fixture_bad_abi)
target_link_libraries(test_binaddon flowsql_common)  # 不直接链接 catalog/binaddon
target_compile_options(test_binaddon PRIVATE -UNDEBUG)
target_compile_definitions(test_binaddon PRIVATE
    FIXTURE_SO_PATH="$<TARGET_FILE:fixture_operator>"
    FIXTURE_BAD_ABI_SO_PATH="$<TARGET_FILE:fixture_bad_abi>"
)
```

**fixture_operator.cpp 关键设计**（通过 Configure 控制阻塞，避免跨 .so 全局变量注入问题）：

```cpp
// RTLD_LOCAL + -fvisibility=hidden 下全局变量不可跨 .so 访问，
// 改用 Configure("block","1") / Configure("unblock","1") 控制阻塞行为
class BlockingOperator : public flowsql::IOperator {
    std::mutex mu_;
    std::condition_variable cv_;
    bool blocked_ = false;

    int Work(IChannel* in, IChannel* out) override {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this]{ return !blocked_; });  // 阻塞直到 unblock
        return 0;
    }
    int Configure(const char* key, const char* value) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (std::string(key) == "block")   { blocked_ = true;  return 0; }
        if (std::string(key) == "unblock") { blocked_ = false; cv_.notify_all(); return 0; }
        return -1;
    }
    // ...
};
```

**测试用例**（走完整插件路径：PluginLoader → BinAddonHostPlugin → IOperatorRegistry）：

1. upload 合法 .so → 仅写入 `operator_plugin_store`（status=uploaded），不触发 dlopen
2. 激活合法 .so → 算子以 "category.name" 注册到 IOperatorRegistry
3. 激活不存在 plugin_id/path → 返回错误，registry 无变化
4. 激活 ABI 版本不匹配 .so → 返回错误，`plugin.last_error` 可查询，状态回到非激活
5. 重复激活同一 .so → 返回 409
6. 去激活已激活 .so → 算子从 registry 注销，catalog 标记 active=0，dlclose 已调用
7. 去激活不存在 .so → 返回 404
8. 去激活时 active_count > 0 → 返回 409，pending_unload 回滚，算子仍可用
   （通过 Configure("block","1") 阻塞 Work()，确保 active_count > 0 时发起 deactivate，测试确定性）
9. 激活后通过 SchedulerPlugin 执行 Work()（E2E 路径，验证 FindOperator 扩展）
10. 去激活后 Create("category.name") 返回 nullptr
11. 并发竞态：多线程同时 Create + 单线程 Deactivate，验证无 crash、无数据竞争
12. 内置算子向后兼容：builtin.passthrough 和 passthrough 两种 key 均可查到
13. dlclose 安全性：去激活后所有 Proxy 析构，验证 ~LoadedSo() 调用 dlclose 且无 use-after-free
14. 激活失败回滚：注入 Register 或 UpsertBatch 失败，验证已注册工厂函数被全量回滚、registry 不留脏 key
15. 同名冲突校验：与 builtin/python/cpp 已有 `(category,name)` 冲突时激活失败，`plugin.last_error` 可查询，状态回到非激活
16. 列表元数据校验：`/operators/list`（`type=="cpp"`）返回 `plugin_id/plugin.so_file/plugin.size_bytes/plugin.sha256/plugin.status/plugin.last_error`；
    未激活时 `plugin.abi_version/plugin.operator_count/plugin.operators` 为空
17. 激活后列表元数据校验：`/operators/list` 中 `plugin.operator_count == plugin.operators.size()`，且均属于对应插件
18. 详情接口保留校验：`/operators/detail`（`type=="cpp"`）按需返回插件详情，字段与 `/operators/list` 公共字段一致
19. 持久化恢复校验：重启后自动重激活 `status=activated` 的插件；失败时写回 `plugin.last_error` 且置 `status=broken`
20. 自动重试约束校验：自动重激活仅在启动阶段执行一次，不存在后台循环重试；`status=broken` 插件不会被自动重试
21. 重复上传冲突校验：同一文件重复 upload 返回 409，提示先 delete 旧插件再 upload
22. C++ 列表结构校验：`POST /operators/list`（`type=cpp`）中同一 `plugin_id` 仅出现一条插件记录
23. 删除校验：`/operators/delete` 删除未激活插件后，文件与元数据清理成功；激活态删除返回 409
