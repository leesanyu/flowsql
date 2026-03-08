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

## Story 4.3: 数据库连接池基础实现 ✅

### 最终方案：IDbSession 完整封装模式

**核心设计**：`IDbSession = 连接 + 操作`

Session 不仅管理连接生命周期，还封装所有底层数据库操作。连接由 Session 的析构函数自动归还，无需显式调用 Detach。

**关键设计决策**：

| 决策点 | 方案 | 说明 |
|--------|------|------|
| `CreateSession()` 返回类型 | `std::shared_ptr<IDbSession>` | Reader/Writer 持有 Session，引用计数管理生命周期 |
| 钩子方法访问权限 | `protected` | 钩子方法是框架内部调用，不是 public API |
| `FreeResult` 设计 | 删除 `IDbSession::FreeResult(void*)` | 由 `RowBasedBatchReader` 析构时直接调用驱动的 `FreeResultImpl` |
| 能力检测接口 | 删除 `AsBatchReadable()` 等 | 统一使用 `dynamic_cast` 检测能力 |
| 模板参数优化 | 使用 traits 模式 | 封装 4 个模板参数，类型签名更清晰 |
| 连接健康检查 | 新增 `Ping()` 接口 | 获取连接时检查有效性，自动重建失效连接 |
| 列式数据库支持 | `IDbSession` 提供默认实现 | 行式/列式数据库按需 override |

### 接口层次结构

```
IDbDriver (基础接口)
    │
    ├─ IBatchReadable / IBatchWritable (行式能力)
    ├─ IArrowReadable / IArrowWritable (列式能力)
    └─ ITransactional (事务能力)
            │
            ▼
        IDbSession (新增接口 = 连接 + 操作)
                │
        ┌───────┴────────┐
        ▼                ▼
┌───────────────────┐  ┌───────────────────┐
│ RelationDbSession │  │  ArrowDbSession   │
│ (行式数据库基类)   │  │ (列式数据库基类)   │
└───────────────────┘  └───────────────────┘
```

**列式数据库支持说明**：
- `IDbSession` 中的 `ExecuteQuery`/`ExecuteSql` 提供默认实现（返回 -1 表示不支持）
- 列式数据库 Session 可 override 这些方法，或直接使用 `IArrowReadable/IArrowWritable` 接口
- `RelationDbSessionBase` 实现行式数据库通用逻辑（基于行的读取/写入）
- `ArrowDbSessionBase` 实现列式数据库通用逻辑（基于 Arrow Batch 的读取/写入）

#### 1. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│  接口层 (capability_interfaces.h)                           │
├─────────────────────────────────────────────────────────────┤
│  IDbDriver (基础驱动接口)                                    │
│  IBatchReadable / IBatchWritable (行式能力)                  │
│  IArrowReadable / IArrowWritable (列式能力)                  │
│  ITransactional (事务能力)                                   │
│  IDbSession (会话接口 = 连接 + 操作)                         │
└─────────────────────────────────────────────────────────────┘
                            │
        ┌───────────────────┴───────────────────┐
        ▼                                       ▼
┌───────────────────┐               ┌───────────────────┐
│ 行式数据库          │               │ 列式数据库          │
│ (MySQL/SQLite)    │               │ (ClickHouse)      │
├───────────────────┤               ├───────────────────┤
│ RelationDbSession │               │ ArrowDbSession    │
│ <Conn,Stmt,Res>   │               │ <Conn,Batch>      │
│ (模板基类)        │               │ (模板基类)        │
└───────────────────┘               └───────────────────┘
        │                                       │
    ┌───┴───┐                           ┌───┴───┐
    ▼       ▼                           ▼       ▼
Mysql  Sqlite                        ClickHouse  ...
Session Session                      Session
```

### 实现状态

#### ✅ 已完成功能

1. **核心接口定义** (`db_session.h`)
   - `IDbSession` - 数据库会话接口
   - `IResultSet` - 结果集接口

2. **模板基类** (`relation_db_session.h`, `arrow_db_session.h`)
   - `RelationDbSessionBase<Traits>` - 行式数据库模板基类
   - `ArrowDbSessionBase<Traits>` - 列式数据库模板基类

3. **连接池** (`connection_pool.h`)
   - `ConnectionPool<T>` - 模板连接池
   - 支持最大连接数、空闲超时、健康检查

4. **具体 Session 实现**
   - `MysqlSession` - MySQL 会话实现
   - `SqliteSession` - SQLite 会话实现

5. **DatabaseChannel 适配**
   - 使用 `SessionFactory` 创建 Session
   - 补充列式数据库接口 (`CreateArrowReader` 等)

6. **单元测试**
   - `test_connection_pool.cpp` - 连接池功能测试
   - `test_session_e2e.cpp` - Session 端到端测试

#### 列式数据库接口补充

在 `idatabase_channel.h` 中新增列式数据库读写接口：

```cpp
// IArrowReader — 列式读取器（Arrow 原生）
interface IArrowReader {
    virtual int ExecuteQueryArrow(const char* query,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                                  std::string* error) = 0;
    virtual std::shared_ptr<arrow::Schema> GetSchema() = 0;
    virtual const char* GetLastError() = 0;
    virtual void Release() = 0;
};

// IArrowWriter — 列式写入器（Arrow 原生）
interface IArrowWriter {
    virtual int WriteBatches(const char* table,
                            const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                            std::string* error) = 0;
    virtual const char* GetLastError() = 0;
    virtual void Release() = 0;
};

// IDatabaseChannel 新增方法
interface IDatabaseChannel : public IChannel {
    // 行式数据库接口
    int CreateReader(const char* query, IBatchReader** reader) = 0;
    int CreateWriter(const char* table, IBatchWriter** writer) = 0;

    // 列式数据库接口
    int CreateArrowReader(const char* query, IArrowReader** reader) = 0;
    int CreateArrowWriter(const char* table, IArrowWriter** writer) = 0;
    int ExecuteQueryArrow(const char* query,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                          std::string* error) = 0;
    int WriteArrowBatches(const char* table,
                          const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                          std::string* error) = 0;

    bool IsConnected() = 0;
};
```

### 连接池配置参数

在 `DatabaseConfig` 中新增连接池配置参数：

```cpp
struct ConnectionPoolConfig {
    int max_connections = 10;           // 最大连接数
    int min_connections = 0;            // 最小空闲连接数（预热）
    int idle_timeout_seconds = 300;     // 空闲超时（秒），超时连接自动回收
    int health_check_interval_seconds = 60;  // 健康检查间隔（秒）
};

struct DatabaseConfig {
    std::string type;
    std::string name;
    std::unordered_map<std::string, std::string> params;
    ConnectionPoolConfig pool;  // 连接池配置
};
```

**配置示例**（gateway.yaml）：

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
        # 连接池配置
        pool:
          max_connections: 20
          idle_timeout_seconds: 300
```

---

### 修改文件清单

| 文件 | 修改内容 | 状态 |
|------|----------|------|
| `src/services/database/db_session.h` | 新增 `IDbSession` / `IResultSet` 接口 | ✅ |
| `src/services/database/relation_db_session.h` | 新增行式数据库模板基类（traits 模式） | ✅ |
| `src/services/database/arrow_db_session.h` | 新增列式数据库模板基类（traits 模式） | ✅ |
| `src/services/database/connection_pool.h` | 新增连接池模板类 | ✅ |
| `src/services/database/drivers/mysql_driver.h` | 新增 `MysqlSession` 类 + 连接池成员 | ✅ |
| `src/services/database/drivers/mysql_driver.cpp` | 实现 `MysqlSession` 钩子方法 | ✅ |
| `src/services/database/drivers/sqlite_driver.h` | 新增 `SqliteSession` 类 + 连接池成员 | ✅ |
| `src/services/database/drivers/sqlite_driver.cpp` | 实现 `SqliteSession` 钩子方法 | ✅ |
| `src/services/gateway/config.h` | 新增 `ConnectionPoolConfig` 结构体 | ✅ |
| `src/services/database/idb_driver.h` | 新增 `Ping()` 健康检查接口 | ✅ |
| `src/services/database/row_based_db_driver_base.h` | 改为抽象基类定义 | ✅ |
| `src/services/database/row_based_db_driver_base.cpp` | 通用 Reader/Writer 实现 | ✅ |
| `src/services/database/database_channel.h` | 使用 `SessionFactory` + 列式接口 | ✅ |
| `src/services/database/database_channel.cpp` | 使用 `CreateSession` 创建 Reader/Writer | ✅ |
| `src/services/database/database_plugin.h` | 新增 `driver_storage_` 成员 | ✅ |
| `src/services/database/database_plugin.cpp` | 适配 Session 架构 | ✅ |
| `src/framework/interfaces/idatabase_channel.h` | 新增 `IArrowReader`/`IArrowWriter` 接口 | ✅ |
| `src/tests/test_database/test_connection_pool.cpp` | 连接池单元测试 | ✅ |
| `src/tests/test_database/test_session_e2e.cpp` | Session 端到端测试 | ✅ |

### 注意事项

1. **Session 生命周期**：由 `shared_ptr` 管理，确保连接正确归还
2. **Reader/Writer 持有 Session**：使用 `shared_ptr<IDbSession>` 确保 Session 存活到 Reader/Writer 销毁
3. **连接池配置**：在 `Connect()` 时解析连接池参数，`CreateSession()` 时从池获取连接
4. **ClickHouse 扩展**：使用 `ArrowDbSessionBase` 模板基类，实现 Arrow 原生读写

---

## Story 4.5: SQL 高级特性

### 设计要点

**目标**：支持 SQL 高级特性（GROUP BY/ORDER BY/LIMIT 等），透传给数据库引擎执行。

**核心设计原则**：
1. **分通道类型处理**：数据库通道和 DataFrame 通道采用不同的处理策略
2. **简化实现**：USING/WITH/INTO 都在 SQL 末尾，直接截断即可
3. **保持兼容**：原有的 `where_clause` 等字段保留，供 DataFrame 通道使用

### 最终方案：分通道类型处理

#### 数据库通道（database channel）

```
用户 SQL: SELECT a, b FROM source WHERE x>1 GROUP BY a ORDER BY b USING ml.predict

处理：
1. 截断 USING/WITH/INTO → SELECT a, b FROM source WHERE x>1 GROUP BY a ORDER BY b
2. 替换表名 → SELECT a, b FROM actual_table WHERE x>1 GROUP BY a ORDER BY b
3. 直接交给数据库执行
```

#### DataFrame 通道（dataframe channel）

```
用户 SQL: SELECT a, b FROM source WHERE x>1 GROUP BY a USING ml.predict

处理：
1. 解析各子句：columns=[a,b], where_clause="x>1"
2. DataFrame 在内存中执行：读取数据 → WHERE 过滤
3. 结果传递给下一个操作
```

**注意**：DataFrame 通道的高级特性（GROUP BY/ORDER BY 等）由 DataFrame 库自身处理，SqlParser 只负责透传 `sql_part` 给数据库通道。

### 关键实现

#### 1. SqlStatement 结构扩展

```cpp
struct SqlStatement {
    std::string source;                                    // FROM 后的源通道名
    std::string op_catelog;                                // USING 后的 operator catalog
    std::string op_name;                                   // USING 后的 operator name
    std::unordered_map<std::string, std::string> with_params;  // WITH 参数
    std::string dest;                                      // INTO 后的目标通道名
    std::vector<std::string> columns;                      // SELECT 后的列名
    std::string where_clause;                              // WHERE 子句（原有）

    // 新增：完整 SQL 部分（不含 USING/WITH/INTO）
    // 数据库通道直接使用这个，不再拼接
    std::string sql_part;

    std::string error;                                     // 解析错误
};
```

**关键说明**：
- `sql_part` = `SELECT ... FROM ... WHERE ... GROUP BY ... ORDER BY ...`（不含 USING/WITH/INTO）
- DataFrame 通道继续使用 `columns`、`where_clause` 等字段进行内存处理
- 数据库通道直接使用 `sql_part` + 表名替换透传给数据库

#### 2. SqlParser 解析逻辑扩展

**文件**: `src/framework/core/sql_parser.cpp`

在 `Parse()` 方法中：

```cpp
SqlStatement SqlParser::Parse(const std::string& sql) {
    SqlStatement stmt;

    // ... 原有 SELECT/FROM/WHERE 解析 ...

    // 依次检测 USING/WITH/INTO，记录它们的位置
    size_t extension_start = std::string::npos;

    // 向前扫描检测扩展语法的起始位置
    for (size_t i = 0; i < sql.size(); ++i) {
        if (MatchKeywordAt(sql, i, "USING") ||
            MatchKeywordAt(sql, i, "WITH") ||
            MatchKeywordAt(sql, i, "INTO")) {
            extension_start = i;
            break;
        }
    }

    // sql_part = 原始 SQL 去掉 USING/WITH/INTO 部分
    if (extension_start != std::string::npos) {
        stmt.sql_part = TrimWhitespace(sql.substr(0, extension_start));
    } else {
        stmt.sql_part = TrimWhitespace(sql);
    }

    // 继续解析 USING/WITH/INTO（原有逻辑保持不变）
    // ...

    return stmt;
}
```

#### 3. NormalizeFromTableName() 函数

**文件**: `src/services/scheduler/scheduler_plugin.cpp`

```cpp
// 只替换 FROM 子句后的表名（支持三段式/两段式）
// 示例：FROM sqlite.mydb.users → FROM users
std::string NormalizeFromTableName(const std::string& sql) {
    std::string result = sql;

    // 匹配 FROM 后面的表名（支持三段式/两段式/一段式）
    // 只匹配 FROM 关键字后的第一个标识符
    std::regex FROM_PATTERN("(\\bFROM\\s+)([\\w]+\\.)?([\\w]+\\.)?([\\w]+)");

    // 替换为：FROM $4（只保留最后一段表名）
    result = std::regex_replace(result, FROM_PATTERN, "$1$4");

    return result;
}
```

**说明**：
- 只匹配 `FROM` 关键字后的表名，避免误伤其他地方的点号（如字符串字面量 `'John.Doe'`）
- 支持三段式（`catalog.database.table`）、两段式（`database.table`）、一段式（`table`）
- 替换后统一为单表名（`table`）

**测试用例**：

```cpp
// 测试三段式表名
TEST(NormalizeFromTableName, ThreePart) {
    std::string sql = "SELECT * FROM sqlite.mydb.users WHERE id > 1";
    std::string normalized = NormalizeFromTableName(sql);
    ASSERT_EQ(normalized, "SELECT * FROM users WHERE id > 1");
}

// 测试两段式表名
TEST(NormalizeFromTableName, TwoPart) {
    std::string sql = "SELECT * FROM mydb.users WHERE id > 1";
    std::string normalized = NormalizeFromTableName(sql);
    ASSERT_EQ(normalized, "SELECT * FROM users WHERE id > 1");
}

// 测试一段式表名（无需替换）
TEST(NormalizeFromTableName, OnePart) {
    std::string sql = "SELECT * FROM users WHERE id > 1";
    std::string normalized = NormalizeFromTableName(sql);
    ASSERT_EQ(normalized, "SELECT * FROM users WHERE id > 1");
}

// 测试字符串字面量中的点号（不应被替换）
TEST(NormalizeFromTableName, StringLiteral) {
    std::string sql = "SELECT * FROM users WHERE name = 'John.Doe'";
    std::string normalized = NormalizeFromTableName(sql);
    ASSERT_EQ(normalized, "SELECT * FROM users WHERE name = 'John.Doe'");
}
```

#### 4. BuildQuery 修改

**文件**: `src/services/scheduler/scheduler_plugin.cpp`

```cpp
static std::string BuildQuery(const std::string& source_name, const SqlStatement& stmt) {
    // 数据库通道：直接使用 sql_part + 表名替换
    std::string sql = stmt.sql_part;

    // 1. 替换 FROM 子句后的三段式/两段式表名为单表名
    sql = NormalizeFromTableName(sql);

    // 2. 替换主句 FROM 的 source 为实际表名
    std::string table = ExtractTableName(source_name);

    // 使用 regex 替换 FROM 后面的表名（支持子查询中的表名引用）
    std::regex FROM_PATTERN("(\\bFROM\\s+)(\\w+)");
    sql = std::regex_replace(sql, FROM_PATTERN, "$1" + table);

    return sql;
}
```

### 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `src/framework/core/sql_parser.h` | 新增 `sql_part` 字段到 `SqlStatement` |
| `src/framework/core/sql_parser.cpp` | 在 `Parse()` 方法中提取 `sql_part`（截断 USING/WITH/INTO） |
| `src/services/scheduler/scheduler_plugin.cpp` | 修改 `BuildQuery()` 使用 `sql_part` + `NormalizeTableName()` |

### 风险与缓解

| 风险 | 缓解措施 |
|------|----------|
| 正则性能开销 | 仅在数据库通道使用，DataFrame 通道不走正则 |
| 复杂 SQL 解析错误 | 保持 `where_clause` 原有逻辑，`sql_part` 作为原始 SQL 截断 |
| 表名替换过度 | 使用 `\b` 单词边界，避免替换标识符中的点 |

### 设计优势

| 优势 | 说明 |
|------|------|
| 实现简单 | 不需要复杂的 AST 解析，只需截断扩展语法 |
| 灵活性强 | 数据库支持的所有 SQL 特性都能透传 |
| 向后兼容 | 保留原有字段供 DataFrame 通道使用 |
| 子查询支持 | 正则全局替换支持子查询中的表名引用 |

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

**风险 1：模板代码膨胀**

使用 traits 模式和模板基类可能导致编译时间增加和代码膨胀。

- 缓解：模板基类代码在头文件中，编译器可以优化
- 缓解：数据库驱动类型有限（MySQL/SQLite/ClickHouse 等），影响可控

**风险 2：连接池并发安全**

多线程环境下连接池可能出现竞态条件。

- 缓解：使用 `std::mutex` 保护共享数据
- 缓解：单元测试覆盖并发场景
- 缓解：压力测试验证线程安全

**风险 3：连接泄漏**

请求异常导致连接未归还。

- 缓解：使用 RAII，`Session` 析构自动归还
- 缓解：`shared_ptr` 管理 `Session` 生命周期
- 缓解：连接池定期清理超时连接

**风险 4：健康检查开销**

频繁的健康检查可能影响性能。

- 缓解：仅在获取连接时检查，非每次操作都检查
- 缓解：可配置健康检查间隔
- 缓解：性能测试验证开销可接受

### 设计优势

| 优势 | 说明 |
|------|------|
| 符合接口隔离原则（ISP） | 每个驱动只实现它需要的能力 |
| 符合开闭原则（OCP） | 新增驱动无需修改现有代码 |
| 符合依赖倒置原则（DIP） | 依赖 `IDbSession` 抽象，而非具体实现 |
| 类型安全 | traits 模式和模板基类避免 `void*` 类型擦除 |
| 扩展性强 | 新增驱动可以自由组合能力 |
| 性能优化 | 优先使用本地接口，避免不必要的虚函数调用 |
| 连接复用 | 连接池管理连接生命周期，提高资源利用率 |
| 异常安全 | 智能指针和 RAII 确保资源正确释放 |

---

## 参考资料

- [MySQL C API Documentation](https://dev.mysql.com/doc/c-api/8.0/en/)
- [Apache Arrow IPC Format](https://arrow.apache.org/docs/format/Columnar.html#ipc-streaming-format)
- [Interface Segregation Principle](https://en.wikipedia.org/wiki/Interface_segregation_principle)
- [ClickHouse Arrow Format](https://clickhouse.com/docs/en/interfaces/formats#arrow)
