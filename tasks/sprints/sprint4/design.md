# Sprint 4 技术设计文档

## 概述

Sprint 4 实现 Epic 4 的核心功能：配置格式优化、MySQL 驱动支持、数据库连接池基础实现、SQL 高级特性。

**核心技术方案**：
1. **配置格式优化**：采用嵌套对象数组配置格式，提升可读性和可维护性，向后兼容旧格式
2. **能力接口设计**：采用基于能力的接口设计（Capability-Based Interface），`IDbDriver` 作为基础接口，能力接口按需组合，支持未来扩展列式数据库（ClickHouse）

---

## 整体架构

### 接口层次结构

```
IDbDriver (基础接口 - 所有驱动必须实现)
    ├── Connect/Disconnect/IsConnected
    ├── DriverName/LastError
    └── 不包含数据读写方法

能力接口 (Capability Interfaces - 按需实现)
├── IBatchReadable (批量读取)
│   └── CreateReader(query, reader)
├── IBatchWritable (批量写入)
│   └── CreateWriter(table, writer)
├── IArrowReadable (Arrow 原生读取 - 未来扩展)
│   └── ExecuteQueryArrow(sql, batches)
├── IArrowWritable (Arrow 原生写入 - 未来扩展)
│   └── WriteArrowBatches(table, batches)
└── ITransactional (事务支持)
    ├── BeginTransaction()
    ├── CommitTransaction()
    └── RollbackTransaction()

辅助基类 (可选 - 减少重复代码)
└── RowBasedDbDriverBase
    ├── 实现 IDbDriver + IBatchReadable + IBatchWritable + ITransactional
    ├── 提供模板方法实现
    └── 子类只需实现 5 个钩子方法

具体驱动 (按需组合能力)
├── MysqlDriver: RowBasedDbDriverBase
├── SqliteDriver: RowBasedDbDriverBase
├── PostgresDriver: RowBasedDbDriverBase (未来扩展)
└── ClickhouseDriver: IDbDriver + IArrowReadable + IArrowWritable (未来扩展)
```

### 类图

```
┌──────────────────────────────────────────────────────────────┐
│                        IDbDriver                             │
│  + Connect(params): int                                      │
│  + Disconnect(): int                                         │
│  + IsConnected(): bool                                       │
│  + DriverName(): const char*                                 │
│  + LastError(): const char*                                  │
└──────────────────────────────────────────────────────────────┘
                            △
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
┌───────┴──────────┐ ┌──────┴────────┐ ┌───────┴──────┐
│ IBatchReadable   │ │ IBatchWritable│ │ITransactional│
│ +CreateReader()  │ │ +CreateWriter()│ │+BeginTrans() │
└──────────────────┘ └───────────────┘ └──────────────┘
        △                   △                   △
        │                   │                   │
        └───────────────────┴───────────────────┘
                            │
        ┌───────────────────┴──────────────────┐
        │    RowBasedDbDriverBase              │
        │  # ExecuteQueryImpl()                │
        │  # InferSchemaImpl()                 │
        │  # FetchRowImpl()                    │
        │  # FreeResultImpl()                  │
        │  # ExecuteSqlImpl()                  │
        └──────────────────────────────────────┘
                            △
                            │
                ┌───────────┴───────────┐
                │                       │
    ┌───────────┴──────────┐  ┌────────┴──────────┐
    │   SqliteDriver       │  │   MysqlDriver     │
    │  - db_: sqlite3*     │  │  - conn_: MYSQL*  │
    └──────────────────────┘  └───────────────────┘
```

---

## Story 4.0: 配置格式优化

### 设计要点

**问题**：旧配置格式使用字符串拼接，冗余且难以阅读。

```yaml
# 旧格式（有问题）
plugins:
  - "libflowsql_database.so:type=sqlite;name=testdb;path=:memory:"
  - "libflowsql_database.so:type=mysql;name=userdb;host=localhost;port=3306;..."
```

**解决方案**：采用嵌套对象数组配置格式。

```yaml
# 新格式（推荐）
plugins:
  - name: libflowsql_database.so
    databases:
      - type: sqlite
        name: testdb
        path: ":memory:"
      - type: mysql
        name: userdb
        host: localhost
        port: 3306
        user: root
        password: secret
        database: users
```

**向后兼容**：保留对旧格式的支持。

### 关键实现

**配置结构体**（config.h）：
```cpp
struct DatabaseConfig {
    std::string type;                                      // 数据库类型
    std::string name;                                      // 实例名称
    std::unordered_map<std::string, std::string> params;  // 其他参数
};

struct ServiceConfig {
    std::vector<std::string> plugins;           // 插件列表（向后兼容）
    std::vector<DatabaseConfig> databases;      // 数据库配置列表
};
```

**插件加载**（main.cpp）：
- 使用 `|` 分隔多个数据库配置
- 在 `DatabasePlugin::Option()` 中解析多个配置

---

## Story 4.1: MySQL 驱动支持

### 设计要点

**目标**：实现 MySQL 驱动，提供与 SQLite 一致的数据库操作能力。

**能力接口设计**：
- `IDbDriver`：基础接口（连接管理、元数据）
- `IBatchReadable`：批量读取能力
- `IBatchWritable`：批量写入能力
- `ITransactional`：事务支持

**模板方法模式**：
- `RowBasedDbDriverBase` 实现所有能力接口
- 子类（MysqlDriver/SqliteDriver）只需实现 5 个钩子方法

### 关键实现

**RowBasedDbDriverBase 钩子方法**：
```cpp
class RowBasedDbDriverBase {
protected:
    virtual void* ExecuteQueryImpl(const char* sql, std::string* error) = 0;
    virtual std::shared_ptr<arrow::Schema> InferSchemaImpl(void* result, std::string* error) = 0;
    virtual int FetchRowImpl(void* result,
                            const std::vector<std::unique_ptr<arrow::ArrayBuilder>>& builders,
                            std::string* error) = 0;
    virtual void FreeResultImpl(void* result) = 0;
    virtual int ExecuteSqlImpl(const char* sql, std::string* error) = 0;
};
```

**MysqlDriver 实现**：
- 使用 MySQL C API (`mysql.h`)
- 支持 prepared statements 和类型绑定
- MySQL 类型到 Arrow 类型的映射

**配置参数**：
| 参数 | 必需 | 说明 |
|------|------|------|
| type | ✅ | 固定为 `mysql` |
| name | ✅ | 数据库实例名 |
| host | ✅ | 服务器地址 |
| port | ✅ | 端口 |
| user | ✅ | 用户名 |
| password | ✅ | 密码 |
| database | ✅ | 数据库名 |
| charset | ❌ | 字符集（默认 utf8mb4）|

---

## Story 4.3: 数据库连接池基础实现

### 设计方案（方案 C）

**方案 C**：在每种数据库的 Driver 内部实现连接池

- MysqlDriver 内部管理多个 MYSQL* 连接
- SqliteDriver 内部管理多个 sqlite3* 连接
- 每个 Driver 自己负责连接的创建、健康检查、回收

**优点**：
1. 逻辑简单，修改范围小
2. 每个 Driver 按自己的方式管理连接
3. 不影响 RowBasedDbDriverBase 和 DatabaseChannel

### 架构

```
DatabasePlugin
  └── channels_["mysql.userdb"] → DatabaseChannel
                                     └── MysqlDriver
                                           └── 连接池
                                                 ├── idle_conns_ [conn1, conn2, ...]
                                                 └── active_conns_ {conn3, conn4}
```

### 连接池生命周期

#### 1. 创建连接池

**时机**：DatabasePlugin 初始化时，预创建 min_idle 个连接

```
DatabasePlugin::Load()
    │
    ▼
MysqlDriver::SetPoolConfig(config)  ← 设置配置
    │
    ▼
MysqlDriver::PreCreateConnections() ← 预创建 min_idle 个连接
```

```cpp
void MysqlDriver::PreCreateConnections() {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    for (int i = 0; i < pool_config_.min_idle; ++i) {
        auto conn = CreateConnection(params_, &last_error_);
        if (conn) {
            idle_conns_.push(conn);
            ++total_connections_;
        }
    }
}
```

#### 2. 获取连接

**时机**：CreateReader / CreateWriter 时

```
DatabaseChannel::CreateReader(query)
    │
    ▼
RowBasedDbDriverBase::CreateReader(query)
    │
    ▼
MysqlDriver::AcquireConnection()  ← 从池获取
    │
    ├──[池有空闲]──> 返回连接
    │
    └──[池无空闲]──> 检查 total < max → 创建新连接 → 返回
                    │
                    └── 达到上限 → 返回错误
    │
    ▼
ExecuteQueryImpl(conn, sql)  ← 使用传入的连接执行
    │
    ▼
RowBasedBatchReader(conn, result, schema)  ← Reader 持有连接
```

```cpp
// RowBasedDbDriverBase::CreateReader
int RowBasedDbDriverBase::CreateReader(const char* query, IBatchReader** reader) {
    // 1. 获取连接
    auto conn = AcquireConnection(params_, &last_error_);
    if (!conn) return -1;

    // 2. 执行查询（传入连接）
    auto result = ExecuteQueryImpl(conn, query, &last_error_);
    if (!result) {
        ReleaseConnection(conn);
        return -1;
    }

    // 3. 获取 Schema
    auto schema = InferSchemaImpl(result, &last_error_);
    if (!schema) {
        FreeResultImpl(result);
        ReleaseConnection(conn);
        return -1;
    }

    // 4. 创建 Reader，传入连接
    *reader = new RowBasedBatchReader(this, conn, result, std::move(schema));
    return 0;
}
```

#### 3. 释放连接

**时机**：Reader / Writer 析构时

```
~RowBasedBatchReader()
    │
    ▼
FreeResultImpl(result)  ← 释放查询结果
    │
    ▼
ReleaseConnection(conn)  ← 归还连接到池
```

```cpp
// RowBasedBatchReader
class RowBasedBatchReader : public IBatchReader {
public:
    RowBasedBatchReader(RowBasedDbDriverBase* driver, void* conn, void* result,
                        std::shared_ptr<arrow::Schema> schema)
        : driver_(driver), conn_(conn), result_(result), schema_(std::move(schema)) {}

    ~RowBasedBatchReader() override {
        if (result_ && driver_) {
            driver_->FreeResultImpl(result_);  // 释放结果集
        }
        if (conn_ && driver_) {
            driver_->ReleaseConnection(conn_);  // 归还连接
        }
    }

private:
    RowBasedDbDriverBase* driver_;
    void* conn_;      // 持有连接
    void* result_;
    std::shared_ptr<arrow::Schema> schema_;
};
```

### 完整流程图

```
                    创建连接池
                         │
                         ▼
    ┌────────────────────────────────────────┐
    │  DatabasePlugin::Load()               │
    │    → MysqlDriver::SetPoolConfig()     │
    │    → PreCreateConnections()           │
    │       → 创建 min_idle 个连接          │
    └────────────────────────────────────────┘
                         │
                         ▼
    ┌────────────────────────────────────────┐
    │  CreateReader(query)                   │
    │    → AcquireConnection()              │
    │       ├─ 从 idle_ 获取              │
    │       ├─ 健康检查 mysql_ping()       │
    │       └─ 或创建新连接                │
    │    → ExecuteQueryImpl(conn, sql)     │
    │    → new RowBasedBatchReader(conn...) │
    └────────────────────────────────────────┘
                         │
                         ▼
    ┌────────────────────────────────────────┐
    │  ~RowBasedBatchReader()                │
    │    → FreeResultImpl(result)           │
    │    → ReleaseConnection(conn)          │
    │       ├─ 加入 idle_ 队列             │
    │       └─ 或关闭（超过 max_connections）│
    └────────────────────────────────────────┘
```

### MysqlDriver 连接池设计

```cpp
// mysql_driver.h 新增

class MysqlDriver : public RowBasedDbDriverBase {
public:
    // 连接池配置
    struct PoolConfig {
        int max_connections = 10;      // 最大连接数
        int idle_timeout_sec = 300;    // 空闲超时（秒）
        int min_idle = 1;              // 最小空闲连接数
    };

    void SetPoolConfig(const PoolConfig& config) { pool_config_ = config; }
    void PreCreateConnections();

    // 连接池方法
    MYSQL* AcquireConnection(const std::unordered_map<std::string, std::string>& params,
                              std::string* error);
    void ReleaseConnection(MYSQL* conn);
    void ClosePool();

private:
    MYSQL* CreateConnection(const std::unordered_map<std::string, std::string>& params,
                             std::string* error);
    bool IsConnectionAlive(MYSQL* conn);

    // 连接参数（用于创建新连接）
    std::string host_;
    int port_ = 3306;
    std::string user_;
    std::string password_;
    std::string database_;
    std::string charset_ = "utf8mb4";
    int timeout_ = 10;

    // 连接池配置
    PoolConfig pool_config_;
    std::queue<MYSQL*> idle_conns_;           // 空闲连接队列
    std::unordered_set<MYSQL*> active_conns_;  // 活跃连接集合
    std::mutex pool_mutex_;
    std::atomic<int> total_connections_{0};
};
```

### AcquireConnection 实现

```cpp
MYSQL* MysqlDriver::AcquireConnection(const auto& params, std::string* error) {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    // 1. 尝试从空闲队列获取
    while (!idle_conns_.empty()) {
        auto conn = idle_conns_.front();
        idle_conns_.pop();

        // 健康检查
        if (IsConnectionAlive(conn)) {
            active_conns_.insert(conn);
            return conn;
        } else {
            // 连接失效，关闭
            mysql_close(conn);
            --total_connections_;
        }
    }

    // 2. 创建新连接（未达上限）
    if (total_connections_ < pool_config_.max_connections) {
        auto conn = CreateConnection(params, error);
        if (conn) {
            active_conns_.insert(conn);
            ++total_connections_;
            return conn;
        }
    }

    // 3. 达到上限
    if (error) *error = "connection pool exhausted";
    return nullptr;
}
```

### ReleaseConnection 实现

```cpp
void MysqlDriver::ReleaseConnection(MYSQL* conn) {
    if (!conn) return;

    std::lock_guard<std::mutex> lock(pool_mutex_);

    // 从活跃集合移除
    active_conns_.erase(conn);

    // 超过最大连接数则关闭，否则放入空闲队列
    if (total_connections_ > pool_config_.max_connections) {
        mysql_close(conn);
        --total_connections_;
    } else {
        idle_conns_.push(conn);
    }
}
```

### 健康检查

```cpp
bool MysqlDriver::IsConnectionAlive(MYSQL* conn) {
    if (!conn) return false;
    return mysql_ping(conn) == 0;
}
```

### 关闭连接池

```cpp
void MysqlDriver::ClosePool() {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    // 关闭所有空闲连接
    while (!idle_conns_.empty()) {
        mysql_close(idle_conns_.front());
        idle_conns_.pop();
    }

    // 关闭所有活跃连接
    for (auto conn : active_conns_) {
        mysql_close(conn);
    }
    active_conns_.clear();

    total_connections_ = 0;
}
```

### RowBasedDbDriverBase 修改

```cpp
class RowBasedDbDriverBase : public IDbDriver,
                              public IBatchReadable,
                              public IBatchWritable,
                              public ITransactional {
public:
    // IBatchReadable 实现（修改）
    int CreateReader(const char* query, IBatchReader** reader) override;

    // IBatchWritable 实现（修改）
    int CreateWriter(const char* table, IBatchWriter** writer) override;

    // 连接池方法（供 Reader/Writer 调用）
    virtual void* AcquireConnection(const std::unordered_map<std::string, std::string>& params,
                                   std::string* error) = 0;
    virtual void ReleaseConnection(void* conn) = 0;

protected:
    // 钩子方法（修改：传入连接参数）
    virtual void* ExecuteQueryImpl(void* conn, const char* sql, std::string* error) = 0;
    virtual void FreeResultImpl(void* result) = 0;

    std::string last_error_;
};
```

### 配置参数

在 `DatabaseConfig` 中添加：
```cpp
struct DatabaseConfig {
    std::string type;
    std::string name;
    std::unordered_map<std::string, std::string> params;

    // 连接池配置（可选）
    int pool_max_connections = 10;
    int pool_idle_timeout_sec = 300;
    int pool_min_idle = 1;
};
```

在 `gateway.yaml` 中：
```yaml
plugins:
  - name: libflowsql_database.so
    databases:
      - type: mysql
        name: userdb
        host: localhost
        port: 3306
        user: root
        password: secret
        database: users
        pool_max_connections: 20
        pool_idle_timeout_sec: 600
```

### 线程安全性

- `std::mutex` 保护 idle_ 和 active_ 集合
- `std::lock_guard` RAII 锁管理
- `std::atomic` 计数 total_connections_
- 每次查询从池中获取连接，使用完归还

### 注意事项

1. **当前连接切换**：每次 Connect() 可能获取不同连接，同一 Driver 实例可并发使用
2. **事务支持**：如果使用连接池，事务边界需要明确（ BEGIN → COMMIT/ROLLBACK → 归还连接）
3. **健康检查**：每次获取连接时用 mysql_ping 检查，发现失效立即重建

### 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `src/services/database/drivers/mysql_driver.h/.cpp` | 添加连接池管理：AcquireConnection/ReleaseConnection/PreCreateConnections |
| `src/services/database/drivers/sqlite_driver.h/.cpp` | 添加连接池管理 |
| `src/services/database/row_based_db_driver_base.h/.cpp` | 修改 CreateReader/CreateWriter 签名，调用 AcquireConnection |
| `src/services/database/row_based_db_driver_base.cpp` | 修改 RowBasedBatchReader/Writer 持有连接 |
| `src/services/gateway/config.h` | 添加连接池配置字段 |
| `src/services/gateway/config.cpp` | 解析连接池配置 |
| `src/app/main.cpp` | 传递连接池配置参数 |

---

## Story 4.5: SQL 高级特性

### 设计要点

**目标**：支持 SQL 高级特性（JOIN/GROUP BY/ORDER BY），透传给数据库引擎。

**支持的关键字**：
- JOIN：`INNER JOIN`, `LEFT JOIN`, `RIGHT JOIN`, `FULL JOIN`, `ON`
- GROUP BY：`GROUP BY`, `HAVING`
- ORDER BY：`ORDER BY`, `ASC`, `DESC`
- 子查询：`SELECT ... WHERE ... IN (SELECT ...)`
- 集合操作：`UNION`, `INTERSECT`, `EXCEPT`

### 关键实现

**SqlParser 扩展**：
- 识别关键字并构建 AST 节点
- 透传 SQL 给数据库引擎（不做本地执行）

**架构**：
```
SqlParser → AST → SQL 改写（可选） → 数据库引擎执行
```

---

## 附录

### 关键文件

**新增文件**：
- `src/services/database/idb_driver.h` — IDbDriver 基础接口
- `src/services/database/capability_interfaces.h` — 能力接口定义
- `src/services/database/row_based_db_driver_base.h/.cpp` — 辅助基类
- `src/services/database/drivers/mysql_driver.h/.cpp` — MysqlDriver 实现
- `src/services/database/connection_pool.h/.cpp` — ConnectionPool 实现
- `config/gateway.example.yaml` — 完整配置示例

**修改文件**：
- `src/services/gateway/config.h` — 增加 DatabaseConfig 结构 + 连接池配置
- `src/services/gateway/config.cpp` — 解析新配置格式
- `src/services/gateway/service_manager.cpp` — 传递数据库配置
- `src/app/main.cpp` — 处理 --databases 参数
- `config/gateway.yaml` — 更新为新格式
- `src/services/database/row_based_db_driver_base.h/.cpp` — 添加连接池支持
- `src/services/database/drivers/sqlite_driver.h/.cpp` — 重构为继承 RowBasedDbDriverBase
- `src/services/database/database_plugin.h/.cpp` — 返回类型改为 IDbDriver*
- `src/services/scheduler/scheduler_plugin.cpp` — 增加能力检测逻辑
- `src/framework/core/sql_parser.h/.cpp` — SQL 高级特性支持

### 风险与缓解

**风险 1：void* 类型擦除**

`RowBasedDbDriverBase` 中使用 `void*` 失去类型安全。

- 缓解：在钩子方法文档中明确 `void*` 的实际类型
- 缓解：使用 `static_cast` 而非 `reinterpret_cast`

**风险 2：dynamic_cast 性能开销**

Scheduler 中频繁使用 `dynamic_cast` 检测能力。

- 缓解：能力检测结果可以缓存
- 缓解：性能测试验证开销可接受

**风险 3：连接泄漏**

请求异常导致连接未归还。

- 缓解：使用 RAII 包装器 `ConnectionGuard` 确保释放

### 设计优势

1. **符合接口隔离原则（ISP）**：每个驱动只实现它需要的能力
2. **扩展性强**：新增驱动可以自由组合能力
3. **性能优化**：Scheduler 优先使用高性能接口
4. **类型安全**：通过 `dynamic_cast` 安全检测能力

---

## 参考资料

- [MySQL C API Documentation](https://dev.mysql.com/doc/c-api/8.0/en/)
- [Apache Arrow IPC Format](https://arrow.apache.org/docs/format/Columnar.html#ipc-streaming-format)
- [Interface Segregation Principle](https://en.wikipedia.org/wiki/Interface_segregation_principle)
- [ClickHouse Arrow Format](https://clickhouse.com/docs/en/interfaces/formats#arrow)
