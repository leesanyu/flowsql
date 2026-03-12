# Sprint 5 设计文档

起草日期：2026-03-12
来源：code.design.md + test_design.md 合并

---

## 一、设计原则（来自 Sprint 4 retrospective）

| 教训 | 本次设计对应措施 |
|------|----------------|
| 设计后置导致三轮重构 | 接口头文件先于实现，本文档即为"接口草稿评审" |
| 测试数据未覆盖所有类型 | ClickHouseDriver 的 `WriteArrowBatches` 必须处理全部 Arrow 类型分支，测试强制覆盖类型矩阵 |
| 绕过插件层测试 | `test_clickhouse.cpp` 直接链接驱动层（单元），`test_plugin_e2e.cpp` 走 PluginLoader（集成），两层分离 |
| assert 被 NDEBUG 禁用 | 所有新增测试目标 CMakeLists.txt 必须加 `-UNDEBUG` |
| 并发测试缺失 | ClickHouseSession 并发读写测试是验收强制条件 |
| 绕过插件层直接测试驱动，失去集成意义 | 所有集成测试必须走 `PluginLoader → IDatabaseFactory → IDatabaseChannel` 完整路径 |
| SQLite 替代 ClickHouse，优先级倒置 | ClickHouse 是本 Sprint 核心，测试优先级最高；SQLite 仅用于持久化层单元测试 |

---

## 二、架构总览

Sprint 4 完成了 MySQL 驱动和连接池。本 Sprint 补充两个独立功能：

1. **Epic 5**：ClickHouse 数据库通道（纯后端驱动扩展）
2. **Epic 6**：Web 管理数据库通道（全栈，替代 gateway.yaml 静态配置）

两个 Epic 无依赖关系，可独立排期。

### 测试分层结构

```
test_clickhouse          ← 驱动层（直接使用 ClickHouseDriver/Session）
test_database            ← 插件层 E2E（PluginLoader → IDatabaseChannel）
  └── test_plugin_e2e    ← 现有，补充 ClickHouse + IDatabaseFactory 管理方法用例
test_database_manager    ← IDatabaseFactory 持久化单元测试（新增，直接链接 flowsql_database）
```

---

## 三、Epic 5：ClickHouse 数据库驱动

### 3.1 架构设计

```
IDbSession（基类，默认实现 ExecuteQueryArrow/WriteArrowBatches 返回 -1）
└── ClickHouseSession（直接继承 IDbSession，覆盖 Arrow 方法）
    ├── ExecuteQueryArrow()  ← HTTP POST + Arrow IPC Stream 解析
    └── WriteArrowBatches()  ← Arrow IPC Stream 序列化 + HTTP POST

IDbDriver
└── ClickHouseDriver（持有连接参数，无连接池——HTTP 无状态）
    └── CreateSession() → ClickHouseSession
```

**关键设计决策**：ClickHouse 是 HTTP 无状态协议，不需要连接池。`ClickHouseDriver` 只持有连接参数，每次 `CreateSession()` 创建一个新的 `ClickHouseSession`，Session 内部用 `httplib::Client` 发请求。这与 MySQL 的连接池模式完全不同，不要套用 `RelationDbSessionBase` 模板。

### 3.2 文件清单

| 文件 | 操作 |
|------|------|
| `src/services/database/drivers/clickhouse_driver.h` | **新建** |
| `src/services/database/drivers/clickhouse_driver.cpp` | **新建** |
| `src/services/database/arrow_adapters.h` | **新建**（ArrowReaderAdapter / ArrowWriterAdapter） |
| `src/services/database/database_plugin.cpp` | 修改 2 处 |
| `src/services/database/database_channel.cpp` | 修改：激活 `CreateArrowReader/CreateArrowWriter` |
| `src/tests/test_database/test_clickhouse.cpp` | **新建** |
| `src/tests/test_database/CMakeLists.txt` | 修改 |

### 3.3 接口设计（clickhouse_driver.h）

```cpp
#ifndef _FLOWSQL_SERVICES_DATABASE_DRIVERS_CLICKHOUSE_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_DRIVERS_CLICKHOUSE_DRIVER_H_

#include <httplib.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../db_session.h"
#include "../idb_driver.h"

namespace flowsql {
namespace database {

// ClickHouseDriver — ClickHouse 数据库驱动
// 基于 HTTP 接口（8123 端口），使用 httplib，零新依赖
// HTTP 无状态，不需要连接池，每次 CreateSession() 创建新 Session
class __attribute__((visibility("default"))) ClickHouseDriver : public IDbDriver {
public:
    ClickHouseDriver() = default;
    ~ClickHouseDriver() override = default;

    // IDbDriver 实现
    int Connect(const std::unordered_map<std::string, std::string>& params) override;
    int Disconnect() override { connected_ = false; return 0; }
    bool IsConnected() override { return connected_; }
    const char* DriverName() override { return "clickhouse"; }
    const char* LastError() override { return last_error_.c_str(); }
    bool Ping() override;

    // 创建 Session（每次返回新实例，HTTP 无状态）
    std::shared_ptr<IDbSession> CreateSession();

private:
    std::string host_;
    int port_ = 8123;
    std::string user_;
    std::string password_;
    std::string database_;
    bool connected_ = false;
    std::string last_error_;
};

// ClickHouseSession — ClickHouse 会话实现
// 直接继承 IDbSession，覆盖 Arrow 方法
// 同时继承 IArrowReadable + IArrowWritable，供 DatabaseChannel::CreateArrowReader/Writer 的 dynamic_cast 检查
// 不继承 RelationDbSessionBase（ClickHouse 是列式数据库，不走行式路径）
class ClickHouseSession : public IDbSession,
                          public IArrowReadable,
                          public IArrowWritable {
public:
    ClickHouseSession(const std::string& host, int port,
                      const std::string& user, const std::string& password,
                      const std::string& database);
    ~ClickHouseSession() override = default;

    // ==================== 列式接口（核心实现）====================

    // 执行 Arrow 查询：构造 "{sql} FORMAT ArrowStream"，POST，解析响应体
    int ExecuteQueryArrow(const char* sql,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                          std::string* error) override;

    // 写入 Arrow batches：序列化为 Arrow IPC Stream，POST INSERT
    int WriteArrowBatches(const char* table,
                          const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                          std::string* error) override;

    // ==================== 行式接口（DDL 等）====================

    // 执行 SQL（DDL/DML），走普通 HTTP 文本响应
    int ExecuteSql(const char* sql, std::string* error) override;

    // 健康检查：GET /?query=SELECT+1
    bool Ping() override;

    // 事务：ClickHouse 不支持，显式覆盖返回 -1（不能依赖基类默认返回 0）
    // 基类默认实现返回 0（空操作），会误导调用方认为事务已成功开启
    int BeginTransaction(std::string* error) override { if (error) *error = "ClickHouse does not support transactions"; return -1; }
    int CommitTransaction(std::string* error) override { if (error) *error = "ClickHouse does not support transactions"; return -1; }
    int RollbackTransaction(std::string* error) override { if (error) *error = "ClickHouse does not support transactions"; return -1; }

private:
    // 构造 HTTP 请求的公共参数
    httplib::Params BuildBaseParams() const;

    // 解析 Arrow IPC Stream 响应体
    int ParseArrowStream(const std::string& body,
                         std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                         std::string* error);

    // 序列化 batches 为 Arrow IPC Stream
    int SerializeArrowStream(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                             std::string* body, std::string* error);

    std::string host_;
    int port_;
    std::string user_;
    std::string password_;
    std::string database_;
};

}  // namespace database
}  // namespace flowsql

#endif
```

### 3.4 关键实现

#### Connect() / Ping()

```cpp
int ClickHouseDriver::Connect(const std::unordered_map<std::string, std::string>& params) {
    auto get = [&](const std::string& k, const std::string& def) {
        auto it = params.find(k);
        return it != params.end() ? it->second : def;
    };
    host_     = get("host", "127.0.0.1");
    port_     = std::stoi(get("port", "8123"));
    user_     = get("user", "default");
    password_ = get("password", "");
    database_ = get("database", "default");

    // Ping 返回具体错误（区分网络不可达 vs 认证失败）
    if (!Ping()) {
        // last_error_ 已在 Ping() 内部根据 HTTP 状态码设置
        return -1;
    }
    connected_ = true;
    return 0;
}

bool ClickHouseDriver::Ping() {
    httplib::Client client(host_, port_);
    client.set_connection_timeout(5);
    httplib::Headers headers = {
        {"X-ClickHouse-User", user_},
        {"X-ClickHouse-Key", password_}
    };
    auto res = client.Get("/?query=SELECT+1", headers);
    if (!res) {
        last_error_ = "ClickHouse unreachable at " + host_ + ":" + std::to_string(port_);
        return false;
    }
    if (res->status == 401 || res->status == 403) {
        last_error_ = "ClickHouse authentication failed (HTTP " + std::to_string(res->status) + "): " + res->body;
        return false;
    }
    if (res->status != 200) {
        last_error_ = "ClickHouse error (HTTP " + std::to_string(res->status) + "): " + res->body;
        return false;
    }
    return true;
}
```

#### ExecuteQueryArrow()

```cpp
int ClickHouseSession::ExecuteQueryArrow(const char* sql,
    std::vector<std::shared_ptr<arrow::RecordBatch>>* batches, std::string* error) {
    std::string full_sql = std::string(sql) + " FORMAT ArrowStream";

    httplib::Client client(host_, port_);
    client.set_connection_timeout(10);
    client.set_read_timeout(60);

    httplib::Headers headers = {
        {"X-ClickHouse-User", user_},
        {"X-ClickHouse-Key", password_}
    };

    std::string path = "/?database=" + database_;
    auto res = client.Post(path, headers, full_sql, "text/plain");

    if (!res || res->status != 200) {
        if (error) *error = res ? res->body : "Connection failed";
        return -1;
    }

    return ParseArrowStream(res->body, batches, error);
}
```

#### WriteArrowBatches()

```cpp
// 表名转义：将反引号替换为双反引号，防止 SQL 注入（与 MySQL 驱动保持一致）
static std::string QuoteIdentifier(const std::string& name) {
    std::string result = "`";
    for (char c : name) {
        if (c == '`') result += "``";  // 转义内部反引号
        else result += c;
    }
    result += "`";
    return result;
}

int ClickHouseSession::WriteArrowBatches(const char* table,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches, std::string* error) {
    // 空 batches 提前返回，不发 HTTP 请求（ClickHouse 收到只有 Schema 的 IPC Stream 行为未定义）
    if (batches.empty()) return 0;

    std::string body;
    if (SerializeArrowStream(batches, &body, error) != 0) return -1;

    std::string query = "INSERT INTO " + QuoteIdentifier(table) + " FORMAT ArrowStream";
    std::string path = "/?database=" + database_ + "&query=" + httplib::detail::encode_url(query);

    httplib::Client client(host_, port_);
    httplib::Headers headers = {
        {"X-ClickHouse-User", user_},
        {"X-ClickHouse-Key", password_}
    };

    auto res = client.Post(path, headers, body, "application/octet-stream");
    if (!res || res->status != 200) {
        if (error) *error = res ? res->body : "Connection failed";
        return -1;
    }
    return 0;
}
```

#### ParseArrowStream()

```cpp
int ClickHouseSession::ParseArrowStream(const std::string& body,
    std::vector<std::shared_ptr<arrow::RecordBatch>>* batches, std::string* error) {
    auto buffer = arrow::Buffer::FromString(body);
    auto buf_reader = std::make_shared<arrow::io::BufferReader>(buffer);

    auto reader_result = arrow::ipc::RecordBatchStreamReader::Open(buf_reader);
    if (!reader_result.ok()) {
        if (error) *error = reader_result.status().ToString();
        return -1;
    }
    auto reader = *reader_result;

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto status = reader->ReadNext(&batch);
        if (!status.ok()) {
            if (error) *error = status.ToString();
            return -1;
        }
        if (!batch) break;  // EOS
        batches->push_back(batch);
    }
    return 0;
}
```

**类型矩阵说明**：`SerializeArrowStream` 使用 `arrow::ipc::MakeStreamWriter`，Arrow 库自动处理所有类型序列化，不需要手写类型分支。这是与 MySQL `BuildRowValues()` 的根本区别——Arrow IPC 是类型安全的，不会出现 INT32 落入 else 分支的问题。

**空结果集处理**：当 ClickHouse 查询返回 0 行时（如 `SELECT * FROM t WHERE 1=0`），响应体是只有 Schema 消息、没有 RecordBatch 消息的 Arrow IPC Stream。`ParseArrowStream` 的 while 循环会直接 break，`batches` 为空，返回 0。调用方需处理 `batches->empty()` 的情况。

### 3.5 database_plugin.cpp 修改点

#### 修改 1：CreateDriver() 增加 clickhouse 分支

```cpp
std::unique_ptr<IDbDriver> DatabasePlugin::CreateDriver(const std::string& type) {
    if (type == "sqlite") return std::make_unique<SqliteDriver>();
    if (type == "mysql")  return std::make_unique<MysqlDriver>();
    if (type == "clickhouse") return std::make_unique<ClickHouseDriver>();  // 新增
    last_error_ = "unsupported database type: " + type;
    return nullptr;
}
```

#### 修改 2：session_factory lambda 增加 ClickHouseDriver 分支

```cpp
auto session_factory = [driver_ptr]() -> std::shared_ptr<IDbSession> {
    if (auto* d = dynamic_cast<SqliteDriver*>(driver_ptr))     return d->CreateSession();
    if (auto* d = dynamic_cast<MysqlDriver*>(driver_ptr))      return d->CreateSession();
    if (auto* d = dynamic_cast<ClickHouseDriver*>(driver_ptr)) return d->CreateSession();  // 新增
    return nullptr;
};
```

### 3.6 database_channel.cpp 修改点（ArrowAdapter）

E2E 测试（E2/E3/E4/E6/E7）要求 `CreateArrowReader/CreateArrowWriter` 返回 0 并正常工作。新增 `ArrowReaderAdapter` 和 `ArrowWriterAdapter`，将 `IArrowReader`/`IArrowWriter` 接口委托给 Session 的 `ExecuteQueryArrow`/`WriteArrowBatches`。

#### arrow_adapters.h（新建）

```cpp
// ArrowReaderAdapter — 将 IArrowReader 接口委托给 IDbSession::ExecuteQueryArrow
class ArrowReaderAdapter : public IArrowReader {
public:
    ArrowReaderAdapter(std::shared_ptr<IDbSession> session, const char* query)
        : session_(std::move(session)), query_(query) {}

    int ExecuteQueryArrow(const char* query,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                          std::string* error) override {
        return session_->ExecuteQueryArrow(query ? query : query_.c_str(), batches, error);
    }
    std::shared_ptr<arrow::Schema> GetSchema() override { return nullptr; }  // 懒加载
    const char* GetLastError() override { return last_error_.c_str(); }
    void Release() override { delete this; }

private:
    std::shared_ptr<IDbSession> session_;
    std::string query_;
    std::string last_error_;
};

// ArrowWriterAdapter — 将 IArrowWriter 接口委托给 IDbSession::WriteArrowBatches
class ArrowWriterAdapter : public IArrowWriter {
public:
    ArrowWriterAdapter(std::shared_ptr<IDbSession> session, const char* table)
        : session_(std::move(session)), table_(table) {}

    int WriteBatches(const char* table,
                     const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                     std::string* error) override {
        return session_->WriteArrowBatches(table ? table : table_.c_str(), batches, error);
    }
    const char* GetLastError() override { return last_error_.c_str(); }
    void Release() override { delete this; }

private:
    std::shared_ptr<IDbSession> session_;
    std::string table_;
    std::string last_error_;
};
```

#### database_channel.cpp 修改

```cpp
int DatabaseChannel::CreateArrowReader(const char* query, IArrowReader** reader) {
    if (!opened_ || !session_factory_) { *reader = nullptr; return -1; }
    auto session = session_factory_();
    if (!session) { *reader = nullptr; return -1; }
    // dynamic_cast 检查 IArrowReadable 能力接口
    auto* arrow_readable = dynamic_cast<IArrowReadable*>(session.get());
    if (!arrow_readable) { *reader = nullptr; return -1; }
    *reader = new ArrowReaderAdapter(std::move(session), query);
    return 0;
}

int DatabaseChannel::CreateArrowWriter(const char* table, IArrowWriter** writer) {
    if (!opened_ || !session_factory_) { *writer = nullptr; return -1; }
    auto session = session_factory_();
    if (!session) { *writer = nullptr; return -1; }
    auto* arrow_writable = dynamic_cast<IArrowWritable*>(session.get());
    if (!arrow_writable) { *writer = nullptr; return -1; }
    *writer = new ArrowWriterAdapter(std::move(session), table);
    return 0;
}
```

**注意**：`ClickHouseSession` 需要同时继承 `IArrowReadable` 和 `IArrowWritable`（来自 `capability_interfaces.h`），以便 `dynamic_cast` 检查通过。MySQL/SQLite Session 不继承这两个接口，`CreateArrowReader/Writer` 对它们仍返回 -1（E5 测试验证此边界）。

### 3.7 CMakeLists.txt 修改

```cmake
# 在 src/tests/test_database/CMakeLists.txt 末尾追加
add_executable(test_clickhouse test_clickhouse.cpp)
target_include_directories(test_clickhouse PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(test_clickhouse arrow rapidjson)
add_dependencies(test_clickhouse flowsql_common flowsql_database)
target_link_libraries(test_clickhouse flowsql_common flowsql_database)
target_compile_options(test_clickhouse PRIVATE -UNDEBUG)
set_target_properties(test_clickhouse PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
```

### 3.8 Story 分解

**Story 5.1：ClickHouseDriver 核心实现**
- `ClickHouseDriver : public IDbDriver`（连接参数管理，Ping 健康检查）
- `ClickHouseSession : public IDbSession`（ExecuteQueryArrow、WriteArrowBatches、ExecuteSql）

**Story 5.2：DatabasePlugin 集成**
- `CreateDriver()` 增加 clickhouse 分支
- session_factory lambda 增加 ClickHouseDriver 分支

**Story 5.3：测试**
- `test_clickhouse.cpp`：连接成功/失败、DDL、写入、读取、Arrow 类型矩阵（INT32/INT64/FLOAT32/FLOAT64/STRING/BOOLEAN）
- 跳过机制：`IsClickHouseAvailable()` 检查，不可用时 SKIP
- 并发读写测试
- `test_empty_result_set`：查询返回 0 行，`ExecuteQueryArrow` 返回 0，`batches` 为空
- `test_quote_identifier_injection`：含反引号、含分号的表名写入，验证不产生 SQL 注入

### 3.9 测试设计

#### 3.9.1 驱动层测试（test_clickhouse.cpp）

参照 `test_mysql.cpp` 结构，每个测试自建自清，时间戳后缀唯一表名，ClickHouse 不可达时全部 SKIP。

**跳过机制**：

```cpp
static bool IsClickHouseAvailable() {
    ClickHouseDriver driver;
    auto params = GetClickHouseParams();
    return driver.Connect(params) == 0 && driver.Ping();
}
```

**用例清单（T1-T16）**：

| 编号 | 函数名 | 类型 | 验证目标 |
|------|--------|------|---------|
| T1 | `test_connect_disconnect` | 正常路径 | Connect 成功，IsConnected=true，Ping=true，Disconnect 后 IsConnected=false |
| T2 | `test_connect_wrong_password` | 错误路径 | 错误密码 Connect 返回 -1，LastError 包含 HTTP 401/403 状态码 |
| T3 | `test_connect_unreachable` | 错误路径 | 不可达主机 Connect 返回 -1，LastError 包含连接信息 |
| T4 | `test_ddl` | 正常路径 | ExecuteSql 执行 CREATE TABLE / DROP TABLE，返回 0 |
| T5 | `test_execute_query_arrow` | 核心路径 | ExecuteQueryArrow 返回正确 RecordBatch，列数/行数/列名正确 |
| T6 | `test_write_arrow_batches` | 核心路径 | WriteArrowBatches 写入成功，回读行数一致 |
| T7 | **`test_arrow_type_matrix`** | **类型覆盖** | 写入包含 INT32/INT64/FLOAT32/FLOAT64/STRING/BOOLEAN 的 batch，回读后每列类型和值均正确 |
| T8 | `test_large_batch_write` | 性能边界 | 写入 10000 行，验证行数正确 |
| T9 | `test_transaction_not_supported` | 错误路径 | BeginTransaction/Commit/Rollback 均返回 -1 |
| T10 | `test_error_nonexistent_table` | 错误路径 | 查询不存在的表，ExecuteQueryArrow 返回 -1，LastError 包含错误信息 |
| T11 | `test_error_syntax` | 错误路径 | 执行语法错误 SQL，返回 -1，LastError 包含 ClickHouse 错误响应 |
| T12 | `test_concurrent_sessions` | **并发** | 8 线程各自独立建立 Session，并发读取同一张表，每线程结果一致，无 crash |
| T13 | `test_concurrent_writers` | **并发** | 6 线程各自独立建立 Session，并发写入不同表，各表行数精确 |
| T14 | `test_quote_identifier_injection` | 安全 | 含反引号、含分号的表名被正确转义，不产生 SQL 注入 |
| T15 | `test_empty_result_set` | 边界 | 查询返回 0 行时，ExecuteQueryArrow 返回 0，batches 为空 |
| T16 | `test_write_empty_batches` | 边界 | WriteArrowBatches 传入空 batches，返回 0，不发 HTTP 请求 |

**T7 Arrow 类型矩阵（强制）**：

```cpp
auto schema = arrow::schema({
    arrow::field("c_int32",   arrow::int32()),
    arrow::field("c_int64",   arrow::int64()),
    arrow::field("c_float32", arrow::float32()),
    arrow::field("c_float64", arrow::float64()),
    arrow::field("c_string",  arrow::utf8()),
    arrow::field("c_bool",    arrow::boolean()),
});
```

回读后逐列逐行验证值，不允许只验证行数。

#### 3.9.2 插件层 E2E 测试（test_plugin_e2e.cpp 补充）

必须走完整生产路径（PluginLoader → IDatabaseFactory → IDatabaseChannel）。

**新增用例（E1-E7）**：

| 编号 | 函数名 | 验证目标 |
|------|--------|---------|
| E1 | `test_clickhouse_connect` | `factory->Get("clickhouse", "ch1")` 返回有效通道，`IsConnected()=true` |
| E2 | `test_clickhouse_create_arrow_reader` | `CreateArrowReader()` 返回 0，读取 RecordBatch 列数/行数正确 |
| E3 | `test_clickhouse_create_arrow_writer` | `CreateArrowWriter()` 返回 0，写入后回读行数一致 |
| E4 | `test_clickhouse_arrow_type_matrix` | 通过插件层写入/读取全类型矩阵，验证类型和值 |
| E5 | `test_clickhouse_create_reader_unsupported` | ClickHouse 通道调用 `CreateReader()`（行式接口）应返回 -1 |
| E6 | `test_concurrent_arrow_readers` | 8 线程同时对同一 `IDatabaseChannel` 调用 `CreateArrowReader()`，无 crash，结果一致 |
| E7 | `test_concurrent_arrow_writers` | 6 线程同时对同一 `IDatabaseChannel` 调用 `CreateArrowWriter()`（不同表），各表行数精确 |

**E5 的意义**：验证行式/列式接口的边界隔离——ClickHouse 通道不应响应行式接口，MySQL 通道不应响应列式接口。

### 3.10 验收标准

1. `DatabasePlugin::Get("clickhouse", "ch1")` 返回有效通道
2. `SELECT * FROM table` 查询返回正确 Arrow RecordBatch
3. `INSERT INTO table FORMAT ArrowStream` 写入成功，行数一致
4. 测试数据覆盖 Arrow 类型矩阵：INT32/INT64/FLOAT/DOUBLE/STRING/BOOLEAN
5. ClickHouse 不可达时 `Connect()` 返回 -1，`LastError()` 有意义
6. 现有 SQLite/MySQL 测试不受影响
7. 并发读写无 crash、无数据串扰

---

## 四、Epic 6：Web 数据库通道管理

### 4.1 架构设计

```
DatabasePlugin（配置权威方）
├── 持久化：config/flowsql.yml（channels.database_channels 节点）
├── Start() 时加载 flowsql.yml 的 channels.database_channels
└── IDatabaseFactory 扩展（合并管理方法，不新增接口）
    ├── AddChannel(config_str)
    ├── RemoveChannel(type, name)
    ├── UpdateChannel(config_str)
    └── List(callback)  ← 扩展现有 List，增加 config_json 参数

SchedulerPlugin（中间层）
└── 注册 HTTP 端点，通过 IQuerier 找 IDatabaseFactory 转发

WebServer（操作入口）
└── CRUD API，通过 Gateway 转发到 Scheduler
```

**持久化方案选择 YAML 的理由**：
- 不依赖 SQLite，避免"用 SQLite 管理 SQLite 通道"的职责混淆
- YAML 文件人类可读，运维可直接编辑，与 `gateway.yaml` 风格一致
- 项目已有 `yaml-cpp` 依赖，零新依赖
- 重启不丢失（不同于 `:memory:` 模式）
- 采用通用配置文件 `flowsql.yml`，多层结构，未来其他配置项统一放入，导出迁移只需一个文件

### 4.2 文件清单

| 文件 | 操作 |
|------|------|
| `src/framework/interfaces/idatabase_factory.h` | 修改：`IDatabaseFactory` 合并管理方法，扩展 `List` 签名 |
| `src/services/database/database_plugin.h` | 修改：实现新增方法，新增 YAML 持久化成员 |
| `src/services/database/database_plugin.cpp` | 修改：Start() 加载 YAML，实现 Add/Remove/Update，密码加解密 |
| `src/services/database/plugin_register.cpp` | 无需修改（IID 不变，仍注册 IID_DATABASE_FACTORY） |
| `src/services/scheduler/scheduler_plugin.h` | 修改：新增 Handler 声明 |
| `src/services/scheduler/scheduler_plugin.cpp` | 修改：注册 4 个新路由 |
| `src/services/web/web_server.h` | 修改：新增 Handler 声明 |
| `src/services/web/web_server.cpp` | 修改：实现 CRUD Handler |
| `src/frontend/src/views/Channels.vue` | 修改：新增数据库通道 UI |
| `src/frontend/src/api/index.js` | 修改：新增 API 调用 |
| `config/flowsql.yml` | **新建**（初始为空，运行时写入） |
| `config/gateway.yaml` | 修改：删除 databases 数组，DatabasePlugin option 改为 `config_file=config/flowsql.yml` |
| `config/gateway.example.yaml` | 修改：同步 |
| `src/services/gateway/service_manager.cpp` | 修改：删除 --databases 参数拼接 |
| `src/tests/test_database/test_database_manager.cpp` | **新建** |

### 4.3 IDatabaseFactory 接口扩展

**决策：不新增 IDatabaseManager 接口，直接扩展 IDatabaseFactory。**

理由：项目只有一个实现（DatabasePlugin），合并后 Scheduler 一次 `Traverse(IID_DATABASE_FACTORY)` 即可拿到所有能力，`plugin_register.cpp` 无需修改。

```cpp
// src/framework/interfaces/idatabase_factory.h
interface IDatabaseFactory {
    // ==================== 现有方法（不变）====================
    virtual IDatabaseChannel* Get(const char* type, const char* name) = 0;
    virtual IDatabaseChannel* GetWithContext(const char* type, const char* name,
                                             const char* user_context) { return Get(type, name); }
    virtual int Release(const char* type, const char* name) = 0;
    virtual const char* LastError() = 0;

    // ==================== List 扩展（签名变更）====================
    // 原签名：void List(std::function<void(const char* type, const char* name)> callback)
    // 新签名：增加 config_json 参数（密码脱敏），兼容旧调用方（config_json 可忽略）
    virtual void List(std::function<void(const char* type, const char* name,
                                         const char* config_json)> callback) = 0;

    // ==================== 新增：动态管理方法（Epic 6）====================

    // 运行时新增通道（写 YAML，加入 configs_，懒加载连接）
    // config_str 格式：type=xxx;name=xxx;...
    // type+name 已存在时返回 -1（报错，请用 UpdateChannel）
    virtual int AddChannel(const char* config_str) { return -1; }

    // 运行时删除通道（关闭连接，从 YAML 删除，从 configs_ 移除）
    virtual int RemoveChannel(const char* type, const char* name) { return -1; }

    // 运行时更新通道配置（原子覆盖写 YAML，不走 Remove+Add）
    virtual int UpdateChannel(const char* config_str) { return -1; }
};
```

**注意**：`AddChannel/RemoveChannel/UpdateChannel` 提供默认实现（返回 -1），不强迫只读实现覆盖。`List` 签名变更需要同步更新所有调用方（Scheduler 中的 `HandleListChannels`）。

### 4.4 flowsql.yml 格式

```yaml
# config/flowsql.yml — FlowSQL 运行时配置（持久化，可导出迁移）
# 此文件由 DatabasePlugin 管理，勿手动编辑 password 字段

channels:
  database_channels:
    - type: mysql
      name: mysqldb
      host: 127.0.0.1
      port: "3306"
      user: flowsql_user
      password: "ENC:base64encodedciphertext"
      database: flowsql_db
      charset: utf8mb4
    - type: clickhouse
      name: ch1
      host: 127.0.0.1
      port: "8123"
      user: flowsql_user
      password: "ENC:base64encodedciphertext"
      database: flowsql_db

# 未来扩展（当前不实现）：
# channels:
#   stream_channels: ...
# operators:
#   custom_operators: ...
```

**写入策略**：
1. 先 `YAML::LoadFile` 读取现有文件（文件不存在则从空节点开始）
2. 仅更新 `root["channels"]["database_channels"]` 节点
3. 写回文件（覆盖），其他顶层节点（`operators` 等）保持不变

### 4.5 database_plugin.h/cpp 关键实现

#### database_plugin.h 新增成员

```cpp
class DatabasePlugin : public IPlugin, public IDatabaseFactory {
public:
    // IDatabaseFactory 新增方法实现
    void List(std::function<void(const char*, const char*, const char*)> cb) override;
    int AddChannel(const char* config_str) override;
    int RemoveChannel(const char* type, const char* name) override;
    int UpdateChannel(const char* config_str) override;

    // IPlugin::Start() 覆盖（加载 YAML 持久化配置）
    int Start() override;

private:
    // Epic 6 新增：YAML 持久化
    std::string config_file_;  // flowsql.yml 路径，从 Option("config_file=...") 解析

    // 密码加解密（AES-256-GCM，OpenSSL）
    std::string EncryptPassword(const std::string& plain);
    std::string DecryptPassword(const std::string& cipher);

    // 从 flowsql.yml 加载 channels.database_channels 节点
    int LoadFromYaml();

    // 将 configs_ 写回 flowsql.yml 的 channels.database_channels 节点
    // 注意：只更新该节点，保留文件中其他顶层节点（operators 等）
    int SaveToYaml();
};
```

#### Option() / Start() / LoadFromYaml() / SaveToYaml()

```cpp
int DatabasePlugin::Option(const char* arg) {
    if (!arg || !*arg) return 0;
    std::string s(arg);
    if (s.find("config_file=") == 0) {
        config_file_ = s.substr(12);
        return 0;
    }
    // 原有逻辑：按 | 分隔多个数据库配置
    // ...
}

int DatabasePlugin::Start() {
    if (config_file_.empty()) return 0;
    return LoadFromYaml();  // 文件不存在时返回 0（首次启动）
}

int DatabasePlugin::LoadFromYaml() {
    if (access(config_file_.c_str(), F_OK) != 0) return 0;  // 首次启动，文件不存在

    YAML::Node root = YAML::LoadFile(config_file_);
    auto db_channels = root["channels"]["database_channels"];
    if (!db_channels) return 0;

    for (const auto& ch : db_channels) {
        std::string config_str = "type=" + ch["type"].as<std::string>()
                               + ";name=" + ch["name"].as<std::string>();
        for (const auto& kv : ch) {
            std::string key = kv.first.as<std::string>();
            if (key == "type" || key == "name") continue;
            std::string val = kv.second.as<std::string>();
            if (key == "password") val = DecryptPassword(val);
            config_str += ";" + key + "=" + val;
        }
        ParseSingleConfig(config_str.c_str());
    }
    return 0;
}

int DatabasePlugin::SaveToYaml() {
    // 先读取现有文件，保留其他顶层节点
    YAML::Node root;
    if (access(config_file_.c_str(), F_OK) == 0) {
        root = YAML::LoadFile(config_file_);
    }

    // 重建 channels.database_channels 节点
    YAML::Node db_channels;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, params] : configs_) {
            YAML::Node ch;
            for (const auto& [k, v] : params) {
                ch[k] = (k == "password") ? EncryptPassword(v) : v;
            }
            db_channels.push_back(ch);
        }
    }
    root["channels"]["database_channels"] = db_channels;

    std::ofstream fout(config_file_);
    fout << root;
    return fout.good() ? 0 : -1;
}
```

#### 密码加解密

```cpp
// 使用 OpenSSL AES-256-GCM
// 密钥从环境变量 FLOWSQL_SECRET_KEY 读取（32字节hex），未设置时使用内置开发密钥
static const char* kDevKey = "flowsql_dev_key_0000000000000000";

// EncryptPassword 返回 "ENC:" + base64(iv + tag + ciphertext)
// DecryptPassword 检查 "ENC:" 前缀，base64 解码，AES-256-GCM 解密
```

### 4.6 Scheduler 新增端点

```cpp
// scheduler_plugin.cpp RegisterRoutes() 新增
server_.Get("/db-channels", HandleListDbChannels);
server_.Post("/db-channels/add", HandleAddDbChannel);
server_.Post("/db-channels/remove", HandleRemoveDbChannel);
server_.Post("/db-channels/update", HandleUpdateDbChannel);

// Handler 实现模式（以 Add 为例）
void SchedulerPlugin::HandleAddDbChannel(const httplib::Request& req, httplib::Response& res) {
    // 1. 解析 JSON body: {"config": "type=mysql;name=mydb;..."}
    // 2. 通过 IQuerier 找 IDatabaseFactory（合并后只需一个 IID）
    IDatabaseFactory* factory = nullptr;
    querier_->Traverse(IID_DATABASE_FACTORY, [&](void* p) -> int {
        factory = static_cast<IDatabaseFactory*>(p);
        return -1;
    });
    if (!factory) { res.status = 503; return; }
    // 3. 调用 AddChannel
    if (factory->AddChannel(config.c_str()) != 0) { res.status = 400; return; }
    res.status = 200; res.body = R"({"ok":true})";
}
```

### 4.7 gateway.yaml 变更

```yaml
# 变更前
- name: libflowsql_database.so
  databases:
    - type: sqlite
      name: testdb
      path: ":memory:"

# 变更后
- name: libflowsql_database.so
  option: "config_file=config/flowsql.yml"
```

`service_manager.cpp` 删除 `--databases` 参数拼接块，改为传递 `option` 参数：

```cpp
// 删除：if (!svc.databases.empty()) { ... }
// 新增：
if (!svc.option.empty()) {
    arg_strings.push_back("--option");
    arg_strings.push_back(svc.option);
}
```

### 4.8 Story 分解

**Story 6.1：DatabasePlugin 持久化与动态管理**
- `IDatabaseFactory` 扩展（合并管理方法，不新增接口，`plugin_register.cpp` 无需修改）
- `Option()` 解析 `config_file`；`Start()` 加载 `flowsql.yml` 的 `channels.database_channels` 节点
- `AddChannel`：type+name 已存在返回 -1（报错，不覆盖）
- `RemoveChannel`：关闭连接 + 从内存移除 + `SaveToYaml()`
- `UpdateChannel`：更新内存 + `SaveToYaml()`（只更新 `channels.database_channels`，保留其他节点）
- `List`：扩展签名，增加 config_json 参数，密码字段脱敏为 `"****"`
- 密码 AES-256-GCM 加解密
- `config_file` 为空时：`Start()` 直接返回 0，`AddChannel` 返回 -1

**Story 6.2：Scheduler 新增管理端点**
- 注册 4 个路由，通过 `IQuerier` 找 `IDatabaseFactory` 转发

**Story 6.3：Web 服务 CRUD API**
- `kSchemaSql` 新增 `db_channels` 缓存表
- 实现 4 个 Handler，`NotifyDatabasePlugin()` 通过 Gateway 转发

**Story 6.4：废弃 gateway.yaml 静态配置**
- 删除 `databases:` 数组，改为 `option: "config_file=config/flowsql.yml"`
- `service_manager.cpp` 不再拼接 `--databases`
- `config/flowsql.yml` 新建（初始为空）
- `config.h` 中 `ServiceConfig::databases` 字段可保留（向后兼容），但 `service_manager.cpp` 不再使用

**Story 6.5：前端通道管理 UI**
- `Channels.vue` 新增"新增数据库通道"对话框
- 类型选择（SQLite/MySQL/ClickHouse），动态显示对应字段
- 密码字段 `type="password"`，展示时显示 `****`

**Story 6.6：端到端测试**
- `test_database_manager.cpp`（9 个用例，M1-M9）
- `test_plugin_e2e.cpp` 补充（P1-P3）
- Scheduler 端点测试（S1-S7）

### 4.9 测试设计

#### 4.9.1 IDatabaseFactory 持久化单元测试（test_database_manager.cpp）

直接链接 `flowsql_database`（不走 PluginLoader），测试对象是持久化逻辑本身。每个测试用例使用独立的临时 `flowsql.yml` 文件（时间戳后缀命名），测试结束后删除，避免用例间状态污染。

**用例清单（M1-M9）**：

| 编号 | 函数名 | 验证目标 |
|------|--------|---------|
| M1 | `test_add_channel_persists` | AddChannel 后读取 `flowsql.yml`，`channels.database_channels` 节点下有对应记录，type/name 正确 |
| M2 | `test_remove_channel_deletes` | RemoveChannel 后读取 `flowsql.yml`，对应记录不再存在 |
| M3 | `test_update_channel` | UpdateChannel 后读取 `flowsql.yml`，配置已更新；其他节点（非 `database_channels`）保持不变 |
| M4 | `test_list_channels_password_masked` | `List()` 回调中 `config_json` 的 password 字段为 `"****"`，不暴露明文 |
| M5 | `test_password_encrypted_in_yaml` | 直接读取 `flowsql.yml` 文件内容，断言 `password` 字段含 `ENC:` 前缀，不含原始密码字符串 |
| M6 | `test_restart_recovery` | Stop 后重新 Start（重新加载 `flowsql.yml`），已保存的通道自动恢复到 configs_ |
| M7 | `test_add_duplicate_channel` | 重复 AddChannel 同 type+name，返回 -1（不覆盖，需用 UpdateChannel） |
| M8 | `test_add_channel_invalid_config` | 缺少必要字段（如无 host）的配置，AddChannel 返回 -1，`flowsql.yml` 不被修改 |
| M9 | `test_concurrent_add_remove` | 多线程并发 AddChannel/RemoveChannel，最终状态与 `flowsql.yml` 文件内容一致，无 crash |

**M3 的特殊要求**：测试需预先在 `flowsql.yml` 写入一个无关节点，UpdateChannel 后验证该节点仍然存在。

#### 4.9.2 插件层补充测试（test_plugin_e2e.cpp，P1-P3）

通过 `IQuerier` 查找 `IID_DATABASE_FACTORY`，直接调用管理方法。

| 编号 | 函数名 | 验证目标 |
|------|--------|---------|
| P1 | `test_add_channel_then_get` | AddChannel 后立即 `factory->Get()` 成功，IsConnected=true |
| P2 | `test_remove_channel_then_get` | RemoveChannel 后 `factory->Get()` 返回 nullptr |
| P3 | `test_add_channel_then_query` | AddChannel 后立即执行 SQL 查询，返回正确结果（验证"无需重启立即可用"） |

#### 4.9.3 Scheduler 端点测试（S1-S7）

| 编号 | 端点 | 测试场景 | 验证目标 |
|------|------|---------|---------|
| S1 | `GET /db-channels` | 正常 | 返回 200，JSON 数组，密码字段为 `****` |
| S2 | `POST /db-channels/add` | 正常 | 返回 200，后续 GET 列表中出现新通道 |
| S3 | `POST /db-channels/add` | 缺少必要字段 | 返回 400，body 包含错误描述 |
| S4 | `POST /db-channels/remove` | 正常 | 返回 200，后续 GET 列表中不再出现 |
| S5 | `POST /db-channels/remove` | 不存在的通道 | 返回 404 或 400，不 crash |
| S6 | `POST /db-channels/update` | 正常 | 返回 200，后续 GET 列表中配置已更新 |
| S7 | 任意端点 | DatabasePlugin 未加载 | 返回 503 |

#### 4.9.4 Web API 测试（W1-W5）

| 编号 | 端点 | 测试场景 | 验证目标 |
|------|------|---------|---------|
| W1 | `GET /api/db-channels` | 正常 | 返回 200，密码字段为 `****` |
| W2 | `POST /api/db-channels` | 正常 | 返回 201，通道立即可用 |
| W3 | `POST /api/db-channels` | 缺少 type/name | 返回 400 |
| W4 | `PUT /api/db-channels/:id` | 密码字段留空 | 不修改原密码（验证"留空不修改"语义） |
| W5 | `DELETE /api/db-channels/:id` | 正常 | 返回 200，通道不再可用 |

前端测试（Story 6.5）以手动验收为主：
- [ ] 新增 SQLite/MySQL/ClickHouse 三种类型通道，表单字段动态切换正确
- [ ] 密码字段 `type="password"`，列表展示 `****`
- [ ] 编辑时密码留空，提交后原密码不变
- [ ] 删除时弹出二次确认，取消不删除
- [ ] 新增通道后不刷新页面，列表自动更新

### 4.10 验收标准

1. Web UI 新增通道后，无需重启，立即可在 SQL 中使用
2. 删除通道后，后续 SQL 返回 "channel not found"
3. Scheduler 重启后，从 `config/flowsql.yml` 的 `channels.database_channels` 节点加载，已保存通道自动恢复
4. `gateway.yaml` 中不再有 `databases:` 配置项
5. 密码在前端展示时脱敏，`flowsql.yml` 中 AES-256-GCM 加密存储（`ENC:` 前缀）
6. Web 服务未启动时，DatabasePlugin 正常运行

---

## 五、测试策略

### 5.1 测试分层结构

| 文件 | 类型 | 链接方式 | `-UNDEBUG` |
|------|------|---------|-----------|
| `test_clickhouse.cpp` | 驱动层单元测试 | 直接链接 `flowsql_database` | ✅ 必须 |
| `test_database_manager.cpp` | `IDatabaseFactory` 持久化单元测试 | 直接链接 `flowsql_database` | ✅ 必须 |
| `test_plugin_e2e.cpp`（补充） | 插件层 E2E | 仅链接 `flowsql_common`，运行时 dlopen | ✅ 已有 |

**注意**：`test_plugin_e2e.cpp` 不能直接链接 `flowsql_database`（double-loading 问题，Sprint 4 P2 根因）。

### 5.2 测试数据规范

#### 表名唯一化

```cpp
auto ts = std::chrono::system_clock::now().time_since_epoch().count();
std::string suffix = std::to_string(ts % 1000000);
std::string table = "test_ch_" + suffix;
```

#### Arrow 类型矩阵（强制）

凡是测试写入路径的用例，schema 必须包含以下所有类型：

| Arrow 类型 | ClickHouse 对应类型 | 测试值示例 |
|-----------|-------------------|----------|
| `int32()` | Int32 | 42 |
| `int64()` | Int64 | 9999999999 |
| `float32()` | Float32 | 3.14f |
| `float64()` | Float64 | 2.718281828 |
| `utf8()` | String | "hello" |
| `boolean()` | UInt8（ClickHouse 无原生 Bool） | true/false |

#### 错误路径覆盖

每个功能点必须有对应的错误路径测试：
- 连接失败（错误密码、不可达主机）
- 查询失败（不存在的表、语法错误）
- 写入失败（类型不匹配、表不存在）

### 5.3 回归测试要求

每个 Story 完成后必须运行全量回归：

```bash
./test_connection_pool   # 7 个用例
./test_sqlite            # 15 个用例
./test_mysql             # 19 个用例
./test_clickhouse        # 16 个用例（新增，T1-T16）
./test_database          # 16+ 个用例（含新增 E2E）
./test_database_manager  # 9 个用例（新增，验证 flowsql.yml 持久化）
```

**回归失败即阻塞当前 Story 验收，不允许带着失败的回归测试继续推进。**

---

## 六、验证方案

### Epic 5 验证

```bash
# 1. 编译
cmake -B build src && cmake --build build -j$(nproc)

# 2. 启动 ClickHouse（已有 docker-compose）
docker compose -f config/docker-compose-clickhouse.yml up -d

# 3. 运行驱动层测试
./build/output/test_clickhouse

# 4. 运行插件层 E2E
cd build/output && ./test_database

# 5. 回归测试
./test_connection_pool && ./test_sqlite && ./test_mysql
```

### Epic 6 验证

```bash
# 1. 启动完整服务
cd build/output && LD_LIBRARY_PATH=. ./flowsql --config ../../config/gateway.yaml

# 2. 验证 Scheduler 端点
curl -X POST http://localhost:18800/scheduler/db-channels/add \
  -H "Content-Type: application/json" \
  -d '{"config":"type=sqlite;name=test;path=:memory:"}'

curl http://localhost:18800/scheduler/db-channels

# 3. 验证重启恢复
kill $(pgrep flowsql) && sleep 2
cd build/output && LD_LIBRARY_PATH=. ./flowsql --config ../../config/gateway.yaml
curl http://localhost:18800/scheduler/db-channels  # 应看到之前添加的通道
```
