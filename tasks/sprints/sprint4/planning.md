# Sprint 4 规划

## Sprint 信息

- **Sprint 周期**：Sprint 4
- **开始日期**：2026-03-05
- **预计工作量**：13 天（大型 Sprint，包含配置优化）
- **Sprint 目标**：配置格式优化、实现 MySQL 驱动支持、数据库连接池基础功能、SQL 高级特性

---

## Sprint 目标

### 主要目标

1. ✅ 优化配置格式，支持嵌套对象数组，提升可读性和可维护性
2. ✅ 实现基于能力的接口设计（Capability-Based Interface），支持未来扩展列式数据库
3. ✅ 实现 MySQL 驱动支持，提供与 SQLite 一致的数据库操作能力
4. ✅ 实现数据库连接池基础功能，支持连接复用和超时回收
5. ✅ 支持 SQL 高级特性（JOIN/GROUP BY/ORDER BY），透传给数据库引擎

### 成功标准

- 配置格式优化完成，支持新旧两种格式，向后兼容 ✅
- 接口设计符合 SOLID 原则，支持 ClickHouse 扩展 ✅
- MySQL 驱动端到端测试全部通过 ✅
- 连接池功能测试通过，连接复用率 > 80% ✅
- SQL 高级特性在 MySQL 和 SQLite 上都能正确执行 ✅

---

## Story 列表

### Story 4.0: 配置格式优化

**优先级**：P0（必须完成，为 MySQL 驱动铺路）
**工作量估算**：1 天
**负责人**：待分配
**依赖**：无（独立任务，可先行实施）

**验收标准**：
- [x] 支持嵌套对象数组配置格式
- [x] 向后兼容旧的字符串拼接格式
- [x] 环境变量替换仍然有效
- [x] 配置解析测试通过

**任务分解**：

#### Task 4.0.1: 修改配置结构定义（0.3 天）✅

**文件**：`src/services/gateway/config.h`

**修改内容**：
- 新增 `DatabaseConfig` 结构体
  ```cpp
  struct DatabaseConfig {
      std::string type;
      std::string name;
      std::unordered_map<std::string, std::string> params;
  };
  ```
- 在 `ServiceConfig` 中新增 `std::vector<DatabaseConfig> databases;` 字段

#### Task 4.0.2: 修改配置解析逻辑（0.4 天）✅

**文件**：`src/services/gateway/config.cpp`

**修改内容**：
- 在 `LoadConfig()` 中解析 `plugins` 数组时，支持两种格式：
  - 旧格式：字符串（`"libflowsql_database.so:type=..."`）
  - 新格式：对象（`{name: "libflowsql_database.so", databases: [...]}`）
- 解析 `databases` 数组：
  - 提取 `type` 和 `name` 字段
  - 将其他所有字段存入 `params` map

#### Task 4.0.3: 修改插件加载逻辑（0.2 天）✅

**文件**：`src/services/gateway/service_manager.cpp`, `src/app/main.cpp`

**修改内容**：
- 在 `service_manager.cpp` 中传递数据库配置到命令行参数
- 在 `main.cpp` 中解析 `--databases` 参数
- 为每个数据库配置调用一次插件加载

#### Task 4.0.4: 更新配置文件和文档（0.1 天）✅

**文件**：
- `config/gateway.yaml` - 更新为新格式
- `config/gateway.example.yaml` - 完整示例
- `README.md` - 补充配置说明

**依赖**：
- 无（独立任务）

**风险**：
- 配置解析兼容性问题

**缓解措施**：
- 单元测试覆盖所有配置格式组合
- 提供清晰的错误提示
- 文档说明推荐使用新格式

---

### Story 4.1: MySQL 驱动支持 ✅

**优先级**：P0（必须完成）
**工作量估算**：6 天
**实际耗时**：约 3 小时（核心功能）+ 2-3 小时（问题修复）
**负责人**：Claude
**完成日期**：2026-03-06

**验收标准**：
- [x] 实现 `RowBasedDbDriverBase` 抽象基类（含 `RowBasedBatchReader/RowBasedBatchWriter`）
- [x] 实现 `MysqlDriver`（基于 libmysqlclient）
- [x] 支持预编译语句
- [x] 支持事务控制（BEGIN/COMMIT/ROLLBACK）
- [x] 端到端测试通过（连接/读取/写入/事务）
- [x] 修复 IPC 反序列化问题
- [x] 修复 MySQL INT 类型读取问题
- [x] 修复前端行数显示问题

**任务分解**：

#### Task 4.1.1: 实现 RowBasedDbDriverBase 抽象基类（3 天）✅
- [x] 定义 `RowBasedDbDriverBase` 类和钩子方法接口
  - `ExecuteQueryImpl(sql, error): void*`
  - `InferSchemaImpl(result, error): Schema`
  - `FetchRowImpl(result, builders, error): int`
  - `FreeResultImpl(result): void`
  - `ExecuteSqlImpl(sql, error): int`
  - `BeginTransactionImpl/CommitTransactionImpl/RollbackTransactionImpl`
- [x] 实现 `RowBasedBatchReader` 类
  - 创建 Arrow builders
  - 循环调用 `FetchRowImpl` 读取行
  - 构建 RecordBatch
  - IPC 序列化
- [x] 实现 `RowBasedBatchWriter` 类
  - IPC 反序列化
  - 自动建表（Arrow Schema → SQL DDL）
  - 批量 INSERT
  - 事务管理

**实现文件**：
- `src/services/database/row_based_db_driver_base.h`
- `src/services/database/row_based_db_driver_base.cpp`

#### Task 4.1.2: 实现 MysqlDriver（2 天）✅
- [x] 实现连接管理
  - `Connect(params): int`
  - `Disconnect(): int`
  - `IsConnected(): bool`
- [x] 实现 7 个钩子方法
  - `ExecuteQueryImpl`：`mysql_stmt_init` + `mysql_stmt_prepare` + `mysql_stmt_execute`
  - `InferSchemaImpl`：`mysql_stmt_result_metadata` + MySQL 类型 → Arrow 类型映射
  - `FetchRowImpl`：`mysql_stmt_fetch` + `MYSQL_BIND` 读取列值
  - `FreeResultImpl`：`mysql_stmt_close`
  - `ExecuteSqlImpl`：`mysql_query`
  - `BeginTransactionImpl/CommitTransactionImpl/RollbackTransactionImpl`
- [x] MySQL 类型映射表
  - `MYSQL_TYPE_LONGLONG` → `arrow::int64()`
  - `MYSQL_TYPE_DOUBLE` → `arrow::float64()`
  - `MYSQL_TYPE_STRING` → `arrow::utf8()`
  - 其他类型...

**实现文件**：
- `src/services/database/drivers/mysql_driver.h`
- `src/services/database/drivers/mysql_driver.cpp`

#### Task 4.1.3: 端到端测试（1 天）✅
- [x] 连接管理测试
  - 测试正常连接 ✅
  - 测试断开连接 ✅
- [x] 读取测试
  - 测试 SELECT 查询 ✅
  - 测试 WHERE 过滤 ✅
- [x] 写入测试
  - 测试 INSERT（自动建表）✅
  - 测试不同数据类型（INT/DOUBLE/STRING）✅
- [x] 事务测试
  - 测试 COMMIT ✅

**测试文件**：
- `src/tests/test_mysql_driver/test_mysql_driver.cpp`

#### Task 4.1.4: 问题修复（额外）✅
- [x] 修复 IPC 反序列化失败问题
  - 问题：`Next()` 返回值错误（0/1 语义混淆）
  - 修复：`row_based_db_driver_base.cpp:49, 83`
- [x] 修复 DatabasePlugin 重复加载问题
  - 问题：每个数据库配置调用一次 `LoadPlugin`
  - 修复：合并配置，支持 `|` 分隔符
- [x] 修复前端显示 "0 行已写入" 问题
  - 问题：`WriteFromDataFrame` 返回值未传递
  - 修复：修改返回值为 `int64_t`，传递行数统计
- [x] 修复 MySQL INT 类型读取错误
  - 问题：`buffer_type` 与实际 buffer 大小不匹配
  - 修复：统一设置为 `MYSQL_TYPE_LONGLONG`

**依赖**：
- libmysqlclient 8.0+
- Arrow 库（已集成）

**风险**：
- `void*` 类型擦除可能导致类型转换错误
- MySQL 客户端库在某些环境下不可用

**缓解措施**：
- 单元测试覆盖所有钩子方法
- MySQL 客户端库是必需依赖，编译时自动检测
- 文档说明如何安装 libmysqlclient

---

### Story 4.3: 数据库连接池基础实现 ✅

**优先级**：P1（重要）
**工作量估算**：4-5 天（经重新评估）
**实际耗时**：约 4 小时（核心实现）
**负责人**：Claude
**完成日期**：2026-03-08

**验收标准**：
- [x] DatabasePlugin 层面统一连接池管理
- [x] 支持连接复用和空闲超时回收
- [x] 支持最大连接数限制
- [x] 健康检查自动重建失效连接
- [x] 基础功能测试通过
- [x] 事务支持（Begin/Commit/Rollback 自动管理连接）
- [x] 列式数据库读写接口补充（`IArrowReader`/`IArrowWriter`）
- [ ] 连接复用率 > 80%（待性能测试验证）
- [ ] 连接获取延迟 < 1ms（待性能测试验证）

**任务分解**：

#### Task 4.3.1: 定义核心接口（0.5 天）✅
- [x] `IDbSession` 接口定义
- [x] `IResultSet` 接口定义
- [x] 删除能力检测接口 (`AsBatchReadable` 等)
- [x] 新增 `Ping()` 健康检查接口

**文件**: `src/services/database/db_session.h`

#### Task 4.3.2: 实现模板基类（1 天）✅
- [x] `RelationDbSessionBase` 模板基类（traits 模式）
- [x] `ArrowDbSessionBase` 模板基类（traits 模式）
- [x] 钩子方法定义为 `protected`

**文件**:
- `src/services/database/relation_db_session.h`
- `src/services/database/arrow_db_session.h`

#### Task 4.3.3: 实现连接池（1.5 天）✅
- [x] `ConnectionPool` 模板类
- [x] 连接池配置参数解析
- [x] 健康检查逻辑
- [x] 超时连接回收

**文件**: `src/services/database/connection_pool.h`

#### Task 4.3.4: 实现 MysqlSession（1 天）✅
- [x] `MysqlTraits` 定义
- [x] `MysqlSession` 类实现
- [x] 8 个钩子方法实现
- [x] `MysqlDriver::CreateSession()` 实现

**文件**:
- `src/services/database/drivers/mysql_driver.h/.cpp`

#### Task 4.3.5: 实现 SqliteSession（1 天）✅
- [x] `SqliteTraits` 定义
- [x] `SqliteSession` 类实现
- [x] 8 个钩子方法实现
- [x] `SqliteDriver::CreateSession()` 实现

**文件**:
- `src/services/database/drivers/sqlite_driver.h/.cpp`

#### Task 4.3.6: 适配 DatabaseChannel（0.5 天）✅
- [x] 移除 `driver_` 成员
- [x] 添加 `session_factory_` 工厂
- [x] 使用 `CreateSession` 创建 Reader/Writer
- [x] 补充列式数据库接口（`CreateArrowReader` 等）

**文件**: `src/services/database/database_channel.h/.cpp`

#### Task 4.3.7: 适配 RowBasedBatchReader/Writer（0.5 天）✅
- [x] 持有 `shared_ptr<IDbSession>`
- [x] 析构时自动释放资源

**文件**: `src/services/database/row_based_db_driver_base.h/.cpp`

#### Task 4.3.8: 单元测试（1 天）✅
- [x] 连接池基础测试
- [x] MysqlSession 测试
- [x] SqliteSession 测试
- [x] 并发安全测试

**文件**: `src/tests/test_database/test_connection_pool.cpp`

#### Task 4.3.9: 端到端测试（0.5 天）✅
- [x] 连接复用测试
- [x] 事务测试
- [x] 健康检查测试

**文件**: `src/tests/test_database/test_session_e2e.cpp`

---

### Story 4.5: SQL 高级特性 ✅

**优先级**：P1（重要）
**工作量估算**：2.5 天
**实际耗时**：约 1 小时
**负责人**：Claude
**完成日期**：2026-03-08

**验收标准**：
- [x] `SqlStatement` 新增 `sql_part` 字段
- [x] `Parse()` 正确截断 USING/WITH/INTO 提取 `sql_part`
- [x] `NormalizeFromTableName()` 正确替换 FROM 子句后的三段式/两段式表名
- [x] 数据库通道使用 `sql_part` + 表名替换透传给数据库
- [x] DataFrame 通道继续使用 `columns`/`where_clause` 等字段
- [x] 字符串字面量中的点号不被误替换
- [x] 子查询表名替换测试通过

**任务分解**：

#### Task 4.5.1: 扩展 SqlStatement 结构体（0.5 天）✅

**文件**: `src/framework/core/sql_parser.h`

**修改内容**:
- 新增 `sql_part` 字段，用于存储完整的 SQL 部分（不含 USING/WITH/INTO）

```cpp
struct SqlStatement {
    // ... 原有字段 ...
    std::string sql_part;  // 新增：完整 SQL 部分（不含 USING/WITH/INTO）
};
```

#### Task 4.5.2: 实现 Parse() 提取 sql_part（1 天）✅

**文件**: `src/framework/core/sql_parser.cpp`

**修改内容**:
- 在 `Parse()` 方法中向前扫描检测 USING/WITH/INTO 的位置
- 截断扩展语法，提取 `sql_part`

```cpp
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
```

#### Task 4.5.3: 实现 NormalizeFromTableName()（0.5 天）✅

**文件**: `src/services/scheduler/scheduler_plugin.cpp`

**新增函数**:

```cpp
// 只替换 FROM 子句后的表名（支持三段式/两段式）
// 示例：FROM sqlite.mydb.users → FROM users
std::string NormalizeFromTableName(const std::string& sql) {
    std::string result = sql;

    // 匹配 FROM 后面的表名（支持三段式/两段式/一段式）
    // 只匹配 FROM 关键字后的第一个标识符
    std::regex FROM_PATTERN(R"((\bFROM\s+)((?:[\w]+\.)*)([\w]+))");

    // 替换为：FROM + 最后一段（表名）
    result = std::regex_replace(result, FROM_PATTERN, "$1$3");

    return result;
}
```

**说明**：
- 只匹配 `FROM` 关键字后的表名，避免误伤其他地方的点号（如字符串字面量 `'John.Doe'`）
- 支持三段式、两段式、一段式表名
- 替换后统一为单表名

#### Task 4.5.4: 修改 BuildQuery() 逻辑（0.5 天）✅

**文件**: `src/services/scheduler/scheduler_plugin.cpp`

**修改内容**:
- 数据库通道：使用 `stmt.sql_part` + 表名替换
- DataFrame 通道：继续使用各子句字段

```cpp
static std::string BuildQuery(const std::string& source_name, const SqlStatement& stmt) {
    // 数据库通道：直接使用 sql_part + 表名替换
    std::string sql = stmt.sql_part;

    // 1. 替换 FROM 子句后的三段式/两段式表名为单表名
    sql = NormalizeFromTableName(sql);

    // 2. 替换主句 FROM 的 source 为实际表名
    std::string table = ExtractTableName(source_name);

    // 使用 regex 替换 FROM 后面的表名
    std::regex FROM_PATTERN(R"((\bFROM\s+)[\w\.]+)");
    sql = std::regex_replace(sql, FROM_PATTERN, "$1" + table);

    return sql;
}
```

#### Task 4.5.5: 单元测试（0.5 天）✅

**文件**: `src/tests/test_framework/main.cpp`

**测试用例**:
- 测试 `sql_part` 提取（含 GROUP BY/ORDER BY）
- 测试 `sql_part` 提取（无扩展语法）
- 测试表名替换（三段式/两段式）
- 测试子查询表名替换
- 测试 BuildQuery 集成

```cpp
// 测试 sql_part 提取（含 GROUP BY/ORDER BY）
TEST(SqlParser, SqlPartWithGroupBy) {
    SqlParser parser;
    auto stmt = parser.Parse("SELECT a, COUNT(*) FROM source GROUP BY a ORDER BY COUNT(*) DESC USING ml.predict");
    ASSERT_EQ(stmt.sql_part, "SELECT a, COUNT(*) FROM source GROUP BY a ORDER BY COUNT(*) DESC");
    ASSERT_EQ(stmt.op_catelog, "ml");
    ASSERT_EQ(stmt.op_name, "predict");
}

// 测试表名替换
TEST(SqlParser, NormalizeTableName) {
    std::string sql = "SELECT * FROM catalog.db.table WHERE id IN (SELECT id FROM other.db.table2)";
    std::string normalized = NormalizeTableName(sql);
    ASSERT_EQ(normalized, "SELECT * FROM table WHERE id IN (SELECT id FROM table2)");
}
```

#### Task 4.5.6: 端到端测试（0.5 天）✅

**文件**: `src/tests/test_framework/main.cpp`

**测试用例**:
- 数据库通道测试 - GROUP BY/ORDER BY
- 数据库通道测试 - 子查询
- DataFrame 通道测试 - WHERE 过滤
- 完整 SQL 测试 - 所有子句

```cpp
// 测试 BuildQuery 集成
{
    auto stmt = parser.Parse("SELECT a, COUNT(*) FROM catalog.db.table WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10 USING ml.train WITH epochs=100 INTO result");
    assert(stmt.error.empty());
    assert(stmt.sql_part == "SELECT a, COUNT(*) FROM source WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10");

    std::string query = BuildQuery_Test("catalog.db.table", stmt);
    assert(query == "SELECT a, COUNT(*) FROM table WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10");
}
```

**依赖**：
- Story 4.1（MySQL 驱动）

**风险**：
- 正则性能开销
- 表名替换过度

**缓解措施**：
- 仅在数据库通道使用正则，DataFrame 通道不走正则
- 使用 `\b` 单词边界，避免替换标识符中的点

---

## 依赖关系

```
Story 4.0 (配置优化) [1 天] ✅
    ↓
Story 4.1 (MySQL 驱动) ✅
    ↓
Story 4.3 (连接池) [依赖 4.1, 重构 Session 架构] ✅
    ↓
Story 4.5 (SQL 高级特性) [依赖 4.1] ✅
```

**并行开发**：
- Story 4.0 可以独立先行实施 ✅
- Story 4.3 的 Task 4.3.1/4.3.2（接口定义/模板基类）完成后，可以并行开发：
  - Task 4.3.4（MysqlSession）✅
  - Task 4.3.5（SqliteSession）✅

---

## 风险与缓解

### 风险 1：libmysqlclient 依赖

**风险描述**：MySQL 客户端库可能在某些环境下不可用。

**影响**：无法编译 MySQL 驱动。

**缓解措施**：
- MySQL 客户端库是必需依赖，编译时自动检测
- 文档说明如何安装 libmysqlclient
- 提供 Docker 镜像，预装所有依赖

### 风险 2：void* 类型擦除

**风险描述**：`void*` 失去类型安全，可能导致类型转换错误。

**影响**：运行时崩溃或数据损坏。

**缓解措施**：
- 在钩子方法文档中明确 `void*` 的实际类型
- 使用 `static_cast` 而非 `reinterpret_cast`
- 单元测试覆盖所有钩子方法
- 代码审查时重点检查类型转换

### 风险 3：连接池并发安全

**风险描述**：多线程环境下连接池可能出现竞态条件。

**影响**：连接泄漏、死锁、数据损坏。

**缓解措施**：
- 使用 `std::mutex` 保护共享数据
- 单元测试覆盖并发场景
- 压力测试验证线程安全

### 风险 4：SQL 解析器复杂度

**风险描述**：识别 SQL 高级特性可能导致解析器过于复杂。

**影响**：开发周期延长，维护成本增加。

**缓解措施**：
- 只识别关键字，不解析语法树
- 复杂 SQL 直接透传给数据库引擎
- 保持解析器简单，避免重复造轮子

---

## 测试计划

### 单元测试

| 测试文件 | 测试内容 | 预计用例数 |
|---------|---------|-----------|
| `test_sql_db_driver.cpp` | SqlDbDriver 基类逻辑 | 10 |
| `test_mysql_driver.cpp` | MysqlDriver 钩子方法 | 15 |
| `test_connection_pool.cpp` | 连接池功能 | 12 |
| `test_sql_parser_advanced.cpp` | SQL 高级特性解析 | 20 |

**总计**：57 个单元测试用例

### 集成测试

| 测试文件 | 测试内容 | 预计用例数 |
|---------|---------|-----------|
| `test_mysql_e2e.cpp` | MySQL 端到端测试 | 8 |
| `test_connection_pool_e2e.cpp` | 连接池端到端测试 | 5 |
| `test_sql_advanced_e2e.cpp` | SQL 高级特性端到端测试 | 10 |

**总计**：23 个集成测试用例

### 性能测试

| 测试文件 | 测试内容 | 性能指标 |
|---------|---------|---------|
| `benchmark_connection_pool.cpp` | 连接池性能 | 连接获取延迟 < 1ms |
| `benchmark_mysql_driver.cpp` | MySQL 驱动性能 | 吞吐量 > 100MB/s |

---

## 验收标准

### Story 4.1 验收

- [ ] 所有单元测试通过（`test_sql_db_driver` + `test_mysql_driver`）
- [ ] 所有集成测试通过（`test_mysql_e2e`）
- [ ] 代码审查通过（无 P0/P1 问题）
- [ ] 文档更新（README + design.md）

### Story 4.3 验收

- [ ] 所有单元测试通过（`test_connection_pool`）
- [ ] 所有集成测试通过（`test_connection_pool_e2e`）
- [ ] 性能测试通过（连接复用率 > 80%）
- [ ] 代码审查通过

### Story 4.5 验收

- [ ] 所有单元测试通过（`test_sql_parser_advanced`）
- [ ] 所有集成测试通过（`test_sql_advanced_e2e`）
- [ ] MySQL 和 SQLite 都能正确执行 SQL 高级特性
- [ ] 代码审查通过

---

## 交付物

### 代码

**新增文件**：
- `src/services/database/db_session.h` — IDbSession / IResultSet 接口 ✅
- `src/services/database/relation_db_session.h` — 行式数据库模板基类 ✅
- `src/services/database/arrow_db_session.h` — 列式数据库模板基类 ✅
- `src/services/database/connection_pool.h` — 连接池实现 ✅
- `src/services/database/connection_pool.h` — 连接池模板实现 ✅

**修改文件**：
- `src/framework/core/sql_parser.h` — 新增 `sql_part` 字段 ✅
- `src/framework/core/sql_parser.cpp` — 实现 `sql_part` 提取逻辑 ✅
- `src/services/gateway/config.h` — 增加 DatabaseConfig 结构 + 连接池配置 ✅
- `src/services/gateway/config.cpp` — 解析新配置格式 ✅
- `src/services/gateway/service_manager.cpp` — 传递数据库配置 ✅
- `src/app/main.cpp` — 处理 --databases 参数 ✅
- `src/services/database/drivers/mysql_driver.h/.cpp` — MysqlSession 实现 ✅
- `src/services/database/drivers/sqlite_driver.h/.cpp` — SqliteSession 实现 ✅
- `src/services/database/database_channel.h/.cpp` — 适配 Session 架构 ✅
- `src/services/database/row_based_db_driver_base.cpp` — Reader/Writer 适配 ✅
- `src/services/database/capability_interfaces.h` — 删除能力检测接口 ✅
- `src/services/database/idb_driver.h` — 新增 Ping() 接口 ✅
- `src/services/scheduler/scheduler_plugin.cpp` — 修改 `BuildQuery()` 使用 `sql_part` ✅

### 测试

- `src/tests/test_framework/main.cpp` — 包含所有框架测试（含 SQL 高级特性）✅
- `src/tests/test_database/test_connection_pool.cpp` — 连接池测试 ✅
- `src/tests/test_database/test_session_e2e.cpp` — Session 端到端测试 ✅

### 文档

- `tasks/sprints/sprint4/design.md` — 技术设计文档（已完成）
- `tasks/sprints/sprint4/planning.md` — Sprint 规划（本文档）
- `README.md` — 更新 MySQL 驱动使用说明
- `docs/commands.md` — 记录命令历史

---

## 时间线

| 日期 | 里程碑 | 交付物 |
|------|--------|--------|
| Day 1 | 配置格式优化 ✅ | config.h/.cpp, gateway.yaml, main.cpp |
| Day 2-4 | 实现 RowBasedDbDriverBase 抽象基类 ✅ | row_based_db_driver_base.h/.cpp |
| Day 3-4 | 并行：实现 MysqlDriver ✅ | mysql_driver.h/.cpp |
| Day 5 | MySQL 驱动端到端测试 ✅ | test_mysql_e2e.cpp |
| Day 6 | 定义 IDbSession 接口 ✅ | db_session.h |
| Day 7-8 | 实现模板基类 (Relation/Arrow) ✅ | relation_db_session.h, arrow_db_session.h |
| Day 9-10 | 实现连接池 ✅ | connection_pool.h |
| Day 11 | 实现 MysqlSession/SqliteSession ✅ | mysql_driver.cpp, sqlite_driver.cpp |
| Day 12 | 适配 DatabaseChannel ✅ | database_channel.h/.cpp |
| Day 13 | 连接池测试 ✅ | test_connection_pool.cpp, test_session_e2e.cpp |
| Day 14 | SQL 高级特性 ✅ | sql_parser.h/.cpp, scheduler_plugin.cpp |
| Day 15 | 代码审查和文档更新 ✅ | README.md + review.md |

---

## 下一步

1. 确认 Sprint 规划
2. 分配任务负责人
3. 开始实施 Story 4.1 Task 1.1
