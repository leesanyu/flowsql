# Story 6.7：数据库错误信息透传架构设计（v2）

## 1. 背景与问题

当前错误处理存在两种模式并存：

| 模式 | 使用位置 |
|------|---------|
| `GetLastError()` | `IBatchReader`, `IBatchWriter`, `IArrowReader`, `IArrowWriter` |
| `std::string* error` 参数 | `IDbSession`, `IDatabaseChannel`, `IArrowReadable`, `IArrowWritable`, `ITransactional` |

此外，`IBatchReadable::CreateReader` 和 `IBatchWritable::CreateWriter` **完全没有错误机制**，
导致 `RelationDbSessionBase::CreateReader` 内部捕获的数据库错误被丢弃。

### 错误丢失路径

```
SchedulerPlugin::HandleExecute
  → ChannelAdapter::ReadToDataFrame(db, query, df_out, error)
    → IDatabaseChannel::CreateReader(query, reader)          ← 无错误机制
      → DatabaseChannel → IBatchReadable::CreateReader       ← 无错误机制
        → RelationDbSessionBase::CreateReader                ← 捕获 error 但丢弃
          → ExecuteQuery(sql, &raw_result, &error)           ← 有 error*，已填充具体原因
```

## 2. 方案选型

| 方案 | 描述 | 优点 | 缺点 |
|------|------|------|------|
| A | `CreateReader(query, reader, error*)` 参数扩展 | 与现有 error* 模式一致 | 两种错误模式继续并存 |
| **B（采用）** | **统一 `GetLastError()` 模式** | **风格统一、API 简洁** | **改动面较大（~20 文件）** |
| C | 返回 `Result<T, Error>` 包装类型 | 最现代 C++ 风格 | 破坏所有现有调用模式 |

**选择方案 B**：所有有状态对象统一通过 `GetLastError()` 暴露错误，移除 `std::string* error` 参数。

## 3. 分层设计

### 3.1 Layer 1 — IDbSession（错误存储层）

`IDbSession` 新增 `last_error_` 成员和 `GetLastError()`，所有公共方法移除 error 参数：

```cpp
class IDbSession : public std::enable_shared_from_this<IDbSession> {
public:
    virtual const char* GetLastError() { return last_error_.c_str(); }

    virtual int ExecuteQuery(const char* sql, IResultSet** result) {
        last_error_ = "ExecuteQuery not supported";
        return -1;
    }
    virtual int ExecuteSql(const char* sql) {
        last_error_ = "ExecuteSql not supported";
        return -1;
    }
    virtual int ExecuteQueryArrow(const char* sql,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) {
        last_error_ = "ExecuteQueryArrow not supported";
        return -1;
    }
    virtual int WriteArrowBatches(const char* table,
                                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
        last_error_ = "WriteArrowBatches not supported";
        return -1;
    }
    virtual int BeginTransaction() { return 0; }   // 默认空实现（不支持事务的数据库）
    virtual int CommitTransaction() { return 0; }
    virtual int RollbackTransaction() { return 0; }

protected:
    std::string last_error_;  // 所有子类共享的错误存储
};
```

内部钩子方法（`PrepareStatement`、`ExecuteStatement`、`GetResultMetadata` 等）**保持 `std::string* error` 参数不变**——它们是 protected 实现细节，直接写入 `&last_error_`。

### 3.2 Layer 2 — 能力接口（capability_interfaces.h）

移除 error 参数，新增 `GetLastError()`：

```cpp
interface IBatchReadable {
    virtual ~IBatchReadable() = default;
    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;
    virtual const char* GetLastError() = 0;  // 新增
};

interface IBatchWritable {
    virtual ~IBatchWritable() = default;
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;
    virtual const char* GetLastError() = 0;  // 新增
};

interface IArrowReadable {
    virtual ~IArrowReadable() = default;
    virtual int ExecuteQueryArrow(const char* sql,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) = 0;  // 移除 error
    virtual const char* GetLastError() = 0;  // 新增
};

interface IArrowWritable {
    virtual ~IArrowWritable() = default;
    virtual int WriteArrowBatches(const char* table,
                                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) = 0;  // 移除 error
    virtual const char* GetLastError() = 0;  // 新增
};

interface ITransactional {
    virtual ~ITransactional() = default;
    virtual int BeginTransaction() = 0;     // 移除 error
    virtual int CommitTransaction() = 0;    // 移除 error
    virtual int RollbackTransaction() = 0;  // 移除 error
    virtual const char* GetLastError() = 0; // 新增
};
```

### 3.3 Layer 3 — 模板基类

**RelationDbSessionBase** — 多继承 `GetLastError()` 的合并：

```cpp
template<typename Traits>
class RelationDbSessionBase : public IDbSession,
                               public IBatchReadable,
                               public IBatchWritable {
public:
    // 单一 override 同时满足 IDbSession + IBatchReadable + IBatchWritable 的 GetLastError()
    const char* GetLastError() override { return last_error_.c_str(); }

    int ExecuteSql(const char* sql) override {
        last_error_.clear();
        auto stmt = PrepareStatement(conn_, sql, &last_error_);  // 内部钩子仍用 &last_error_
        if (!stmt) return -1;
        int ret = ExecuteStatement(stmt, &last_error_);
        int affected_rows = GetAffectedRows(stmt);
        FreeStatement(stmt);
        return (ret == 0) ? affected_rows : -1;
    }

    int CreateReader(const char* query, IBatchReader** reader) override {
        last_error_.clear();
        IResultSet* raw_result = nullptr;
        if (ExecuteQuery(query, &raw_result) != 0) return -1;
        std::unique_ptr<IResultSet> result(raw_result);
        auto schema = InferSchema(result.get(), &last_error_);
        if (!schema) return -1;
        *reader = CreateBatchReader(result.release(), schema);
        return (*reader) ? 0 : -1;
    }

    int BeginTransaction() override {
        last_error_.clear();
        if (in_transaction_) { last_error_ = "Transaction already in progress"; return -1; }
        int ret = ExecuteSql("BEGIN");
        if (ret != -1) in_transaction_ = true;
        return (ret != -1) ? 0 : -1;
    }
    // CommitTransaction / RollbackTransaction 同理
};
```

**ArrowDbSessionBase** — 同样合并 `GetLastError()`：

```cpp
template<typename Traits>
class ArrowDbSessionBase : public IDbSession,
                            public IArrowReadable,
                            public IArrowWritable {
public:
    const char* GetLastError() override { return last_error_.c_str(); }

    int ExecuteQueryArrow(const char* sql,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) override {
        last_error_.clear();
        return FetchArrowBatches(conn_, sql, batches, &last_error_);  // 内部钩子仍用 &last_error_
    }
    int WriteArrowBatches(const char* table,
                          const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) override {
        last_error_.clear();
        return WriteBatches(conn_, table, batches, &last_error_);
    }
};
```

### 3.4 Layer 4 — IDatabaseChannel（公共接口）

```cpp
interface IDatabaseChannel : public IChannel {
    virtual const char* GetLastError() = 0;  // 新增

    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;
    virtual int CreateArrowReader(const char* query, IArrowReader** reader) = 0;
    virtual int CreateArrowWriter(const char* table, IArrowWriter** writer) = 0;
    virtual int ExecuteQueryArrow(const char* query,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) = 0;  // 移除 error
    virtual int WriteArrowBatches(const char* table,
                                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) = 0;  // 移除 error
    virtual int ExecuteSql(const char* sql) = 0;  // 移除 error
    virtual bool IsConnected() = 0;
};
```

### 3.5 Layer 5 — DatabaseChannel（实现层）

新增 `last_error_` 成员，各方法在失败时设置错误，session 级错误需在 session 销毁前拷贝：

```cpp
class DatabaseChannel : public IDatabaseChannel {
public:
    const char* GetLastError() override { return last_error_.c_str(); }

    int CreateReader(const char* query, IBatchReader** reader) override {
        last_error_.clear();
        if (!opened_) { last_error_ = "channel not opened"; return -1; }
        auto session = session_factory_();
        if (!session) { last_error_ = "failed to create session"; return -1; }

        auto* batch_readable = dynamic_cast<IBatchReadable*>(session.get());
        if (!batch_readable) {
            last_error_ = "session does not support batch reading (" + type_ + "." + name_ + ")";
            return -1;
        }

        int ret = batch_readable->CreateReader(query, reader);
        if (ret != 0) {
            // 关键：session 是局部变量，必须在销毁前拷贝错误
            last_error_ = batch_readable->GetLastError();
        }
        return ret;
    }

    int ExecuteSql(const char* sql) override {
        last_error_.clear();
        if (!opened_ || !session_factory_) { last_error_ = "channel not opened"; return -1; }
        auto session = session_factory_();
        if (!session) { last_error_ = "failed to create session"; return -1; }
        int ret = session->ExecuteSql(sql);
        if (ret < 0) last_error_ = session->GetLastError();
        return ret;
    }

    // ExecuteQueryArrow / WriteArrowBatches 同理

private:
    std::string last_error_;
};
```

### 3.6 Layer 6 — 适配器

**ArrowReaderAdapter / ArrowWriterAdapter**（arrow_adapters.h）：

```cpp
class ArrowReaderAdapter : public IArrowReader {
    int ExecuteQueryArrow(const char* query,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) override {
        int ret = session_->ExecuteQueryArrow(query ? query : query_.c_str(), batches);
        if (ret != 0) last_error_ = session_->GetLastError();
        return ret;
    }
};
```

**RelationBatchWriterBase**（relation_adapters.h）：

```cpp
// 事务调用改为无 error 参数
if (session_->BeginTransaction() != 0) {
    last_error_ = std::string("BeginTransaction failed: ") + session_->GetLastError();
    return -1;
}
```

### 3.7 边界层 — ChannelAdapter（保持 `std::string* error` 输出）

`ChannelAdapter` 是 static 工具类，无状态，无法持有 `GetLastError()`。作为自然边界，从 channel 提取错误：

```cpp
int ChannelAdapter::ReadToDataFrame(IDatabaseChannel* db, const char* query,
                                     IDataFrameChannel* df_out, std::string* error) {
    IBatchReader* reader = nullptr;
    if (db->CreateReader(query, &reader) != 0 || !reader) {
        if (error) {
            const char* detail = db->GetLastError();
            *error = (detail && detail[0]) ? detail
                   : "CreateReader failed for query: " + std::string(query ? query : "");
        }
        return -1;
    }
    // reader->Next() 失败时：
    if (error) *error = reader->GetLastError();
    // ...
}
```

`SchedulerPlugin` 不需要修改——它调用 `ChannelAdapter`，error 自然透传上来。

## 4. 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `framework/interfaces/idatabase_channel.h` | `IDatabaseChannel` 新增 `GetLastError()`，移除 error 参数；`IArrowReader`/`IArrowWriter` 移除 error 参数 |
| `services/database/capability_interfaces.h` | `IBatchReadable`/`IBatchWritable` 新增 `GetLastError()`；`IArrowReadable`/`IArrowWritable`/`ITransactional` 移除 error 参数，新增 `GetLastError()` |
| `services/database/db_session.h` | `IDbSession` 新增 `last_error_` + `GetLastError()`，所有方法移除 error 参数 |
| `services/database/relation_db_session.h` | 适配新签名，公共方法使用 `last_error_`，override `GetLastError()` |
| `services/database/arrow_db_session.h` | 适配新签名，override `GetLastError()` |
| `services/database/database_channel.h/.cpp` | 新增 `last_error_`，所有方法适配新签名 |
| `services/database/arrow_adapters.h` | 移除 error 参数，从 session 拷贝错误 |
| `services/database/relation_adapters.h` | 事务调用适配新签名 |
| `framework/core/channel_adapter.h/.cpp` | 使用 `db->GetLastError()` / `reader->GetLastError()` |
| `services/database/drivers/sqlite_driver.h/.cpp` | 适配 IDbSession 新签名 |
| `services/database/drivers/mysql_driver.h/.cpp` | 适配 IDbSession 新签名 |
| `services/database/drivers/clickhouse_driver.h/.cpp` | 适配 IDbSession 新签名，override `GetLastError()` |
| `tests/test_database/test_sqlite.cpp` | 适配新签名 + 新增错误透传测试 |
| `tests/test_database/test_mysql.cpp` | 适配新签名 + 新增错误透传测试 |
| `tests/test_database/test_clickhouse.cpp` | 适配新签名 |
| `tests/test_database/test_plugin_e2e.cpp` | 新增 E2E 错误透传测试 |
| `tests/test_database/test_connection_pool.cpp` | `ConnectionPool::Acquire` 使用独立 error 参数，不涉及 IDbSession，**无需改动** |

## 5. 多继承 `GetLastError()` 合并说明

C++ 中，当派生类从多个基类继承同签名虚函数时，单一 override 同时满足所有基类：

```
RelationDbSessionBase
  ├── IDbSession::GetLastError()        ─┐
  ├── IBatchReadable::GetLastError()     ├── 单一 override 满足全部
  └── IBatchWritable::GetLastError()    ─┘

ArrowDbSessionBase
  ├── IDbSession::GetLastError()        ─┐
  ├── IArrowReadable::GetLastError()     ├── 单一 override 满足全部
  └── IArrowWritable::GetLastError()    ─┘
```

`last_error_` 定义在 `IDbSession` 中（protected），所有子类共享同一存储。

## 6. 线程安全分析

**结论：不存在线程安全问题。**

1. **Session 单 owner**：`DatabaseChannel` 每次调用通过 `session_factory_()` 创建新 Session，不跨线程共享
2. **Reader/Writer 单 owner**：创建后由调用方独占使用，生命周期 `Create → Use → Close → Release`
3. **Channel 单 owner**：每个 channel 对应一个数据库连接，不跨线程共享
4. **`last_error_` 写读时序**：方法内写入 → 方法返回 → 调用方读取，天然串行

## 7. 测试计划

### 修改现有测试

所有使用 `std::string* error` 参数的测试调用点需改为调用后通过 `->GetLastError()` 获取错误。

| 测试文件 | 受影响调用点数 | 涉及方法 |
|----------|--------------|---------|
| `test_sqlite.cpp` | ~40 | `ExecuteSql`, `ExecuteQuery`, `BeginTransaction`, `CommitTransaction`, `RollbackTransaction` |
| `test_mysql.cpp` | ~50 | 同上 |
| `test_clickhouse.cpp` | ~60 | `ExecuteSql`, `ExecuteQueryArrow`, `WriteArrowBatches`, `BeginTransaction`, `CommitTransaction`, `RollbackTransaction` |
| `test_plugin_e2e.cpp` | ~25 | `ExecuteQueryArrow`, `WriteArrowBatches`, `WriteBatches` |
| `test_connection_pool.cpp` | 1 | `ConnectionPool::Acquire`（不涉及 IDbSession，无需改动） |

典型改动模式：

```cpp
// 改动前
std::string error;
int rc = session->ExecuteSql("CREATE TABLE t1 (...)", &error);
assert(rc == 0);

// 改动后
int rc = session->ExecuteSql("CREATE TABLE t1 (...)");
assert(rc == 0);
// 需要检查错误时：
// assert(std::string(session->GetLastError()).find("expected") != std::string::npos);
```

### 新增测试

- **test_sqlite.cpp**：`CreateReader` 错误透传（表不存在 → `session->GetLastError()` 包含 "no such table"）
- **test_mysql.cpp**：`CreateReader` 错误透传（表不存在 → `session->GetLastError()` 非空）
- **test_plugin_e2e.cpp**：通过 `IDatabaseChannel` 查询不存在的表，断言 `db->GetLastError()` 包含具体原因

## 8. 实现注意事项

### 8.1 session 销毁前必须拷贝错误

`DatabaseChannel` 中 session 是局部 `shared_ptr`，方法返回后可能销毁。必须在 session 存活时拷贝错误到 `last_error_`：

```cpp
int ret = batch_readable->CreateReader(query, reader);
if (ret != 0) last_error_ = batch_readable->GetLastError();  // 必须在 session 销毁前
return ret;
```

### 8.2 成功路径清空 `last_error_`

每个公共方法入口处 `last_error_.clear()`，确保成功调用后 `GetLastError()` 返回空字符串。

### 8.3 ClickHouse 特殊处理

`ClickHouseSession` 不实现 `IBatchReadable`，`DatabaseChannel::CreateReader` 在 `dynamic_cast` 处失败，`last_error_` 填充明确提示：
```
"session does not support batch reading (clickhouse.ch1)"
```
