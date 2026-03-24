# Sprint 10 设计文档

## 概述

Sprint 10 交付 Epic 11 的增强能力，分两个 Story：
- **Story 11.3**：多输入算子接口 + SQL 多源语法 + 内置合并算子 + 多 SQL 任务
- **Story 11.4**：线程池 + 取消/超时 + 前端轮询感知 + 结构化诊断 + 保留策略

---

## Part A：Story 11.3 设计

### A.1 Span<T> 轻量模板

C++17 无 `std::span`，在 `src/common/span.h` 新增：

```cpp
namespace flowsql {

// 轻量只读视图，不拥有数据，零拷贝
template <typename T>
struct Span {
    T* data = nullptr;
    size_t size = 0;

    Span() = default;
    Span(T* d, size_t s) : data(d), size(s) {}
    // 从单个元素构造（size=1），调用方无需手动取地址+传长度
    explicit Span(T& single) : data(&single), size(1) {}
    // 从 std::vector 隐式构造
    Span(std::vector<T>& v) : data(v.data()), size(v.size()) {}

    bool empty() const { return size == 0; }
    T& operator[](size_t i) { assert(i < size); return data[i]; }
    const T& operator[](size_t i) const { assert(i < size); return data[i]; }
};

}  // namespace flowsql
```

### A.2 IOperator 多输入接口

`src/framework/interfaces/ioperator.h` 新增：

```cpp
#include <common/span.h>

interface IOperator {
    // ... 现有接口不变 ...
    virtual int Work(IChannel* in, IChannel* out) = 0;

    // 多输入版本：默认降级到 inputs[0]，多输入算子按需覆盖
    virtual int Work(Span<IChannel*> inputs, IChannel* out) {
        if (inputs.empty()) return -1;
        return Work(inputs[0], out);
    }
};
```

**调用约定**：
- Scheduler 统一调多输入版本，单源时传 `Span<IChannel*>(*ch)`（单元素构造），多源时传 `Span<IChannel*>(vec)`
- 现有所有算子无需修改，自动通过默认实现降级到 `inputs[0]`
- `ExecuteWithOperatorChain` 的 source 参数统一改为 `Span<IChannel*>`，不再区分单源/多源分支

### A.3 SQL 多源语法

#### SqlStatement 变更

`src/framework/core/sql_parser.h`：

```cpp
struct SqlStatement {
    std::string source;                    // 保留（向后兼容，= sources[0]）
    std::vector<std::string> sources;      // 新增：FROM 后所有源（含 source）
    // ... 其余字段不变 ...
};
```

#### 解析逻辑变更

`sql_parser.cpp` 中 FROM 解析（`MatchChar` 不存在，直接操作 `pos_`）：

```cpp
// 原来：stmt.source = ReadIdentifier();
// 改为：
stmt.sources.clear();
stmt.sources.push_back(ReadIdentifier());
if (stmt.sources[0].empty()) {
    stmt.error = "expected source channel name after FROM";
    return stmt;
}
SkipWhitespace();
while (pos_ < end_ && *pos_ == ',') {
    ++pos_;  // 跳过逗号
    SkipWhitespace();
    std::string s = ReadIdentifier();
    if (s.empty()) {
        stmt.error = "expected source channel name after ','";
        return stmt;
    }
    stmt.sources.push_back(std::move(s));
    SkipWhitespace();
}
stmt.source = stmt.sources[0];  // 向后兼容
```

#### 语法示例

```sql
-- 单源（现有，不变）
SELECT * FROM dataframe.d1 USING builtin.passthrough INTO dataframe.out

-- 多源（新增）
SELECT * FROM dataframe.d1, dataframe.d2 USING builtin.concat INTO dataframe.merged
SELECT * FROM dataframe.d1, dataframe.d2, dataframe.d3 USING builtin.hstack INTO dataframe.wide
```

### A.4 Scheduler 单源/多源统一执行

#### “首算子”语义（关键约定）

“首算子”定义在**单条 SQL 语句级**，即该语句 `operators[0]`。  
Task（多 SQL）级别不存在唯一“首算子”。

示例（同一任务的三条 SQL）：

```sql
SELECT * FROM dataframe.s1 USING op1 INTO dataframe.d1
SELECT * FROM dataframe.s2 USING op2 INTO dataframe.d2
SELECT * FROM dataframe.d1, dataframe.d2 USING builtin.concat INTO dataframe.dd
```

- 第 1 条 SQL 的首算子是 `op1`
- 第 2 条 SQL 的首算子是 `op2`
- 第 3 条 SQL 的首算子是 `builtin.concat`

#### 统一执行模型

`ExecuteWithOperatorChain` 统一接收 `Span<IChannel*>` 输入，单源/多源走同一入口，不再维护两套算子调用路径：

```cpp
int SchedulerPlugin::ExecuteWithOperatorChain(
    Span<IChannel*> inputs,    // 单源=1个元素，多源=>多个元素
    IChannel* sink,
    const std::vector<IOperator*>& ops,
    const SqlStatement& stmt,
    int64_t* rows_affected,
    std::string* error);
```

执行规则：
1. 无算子（`!stmt.HasOperator()`）继续走现有 `ExecuteTransfer`，且仅允许单源；多源无算子直接报错。  
2. 有算子时统一调用多输入接口：  
   - Stage 0：`ops[0]->Work(inputs, stage0_sink)`  
   - Stage i（i > 0）：`ops[i]->Work(Span<IChannel*>(prev_stage_out), stage_i_sink)`（单元素 Span）  
3. 单源 SQL 只是多输入模型的特例（`inputs.size()==1`）。

`HandleExecute()` 统一构造 `Span` 传入，并在入口做约束校验：

```cpp
// 查找所有源通道（单源/多源统一处理）
std::vector<IChannel*> input_channels;
for (const auto& name : stmt.sources) {
    IChannel* ch = FindChannel(name);
    if (!ch) { /* 返回错误 */ }
    input_channels.push_back(ch);
}

// 多源必须有算子，否则无法确定合并语义
if (stmt.sources.size() > 1 && !stmt.HasOperator()) {
    rsp = MakeErrorJson("multi-source FROM requires USING operator");
    return error::BAD_REQUEST;
}

// V1 约束：多源仅支持 dataframe.*，且不支持 WHERE
if (stmt.sources.size() > 1) {
    for (const auto& s : stmt.sources) {
        if (!StartsWithIgnoreCase(s, "dataframe.")) {
            rsp = MakeErrorJson("multi-source FROM only supports dataframe.* in Sprint 10");
            return error::BAD_REQUEST;
        }
    }
    if (!stmt.where_clause.empty()) {
        rsp = MakeErrorJson("multi-source FROM does not support WHERE in Sprint 10");
        return error::BAD_REQUEST;
    }
}

rc = ExecuteWithOperatorChain(Span<IChannel*>(input_channels), sink, op_chain,
                              stmt, &rows, &error);
```

### A.5 内置合并算子

#### ConcatOperator（按行合并）

`src/services/catalog/builtin/concat_operator.h/cpp`：

```cpp
class ConcatOperator : public IOperator {
    std::string Catelog() override { return "builtin"; }
    std::string Name() override { return "concat"; }
    std::string Description() override { return "Concatenate multiple DataFrames by rows"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel* in, IChannel* out) override { return -1; }  // 不支持单输入
    int Work(Span<IChannel*> inputs, IChannel* out) override;
    int Configure(const char* key, const char* value) override { return 0; }
};
```

`Work` 实现逻辑：
1. 读取所有输入 DataFrame（`dynamic_cast<IDataFrameChannel*>`）
2. 检查 schema 兼容：所有输入列名和类型必须一致
3. 使用 `arrow::ConcatenateTables()` 或逐行追加合并
4. 写入 out 通道

#### HstackOperator（按列合并）

类似 ConcatOperator，`Work` 实现逻辑：
1. 读取所有输入 DataFrame
2. 检查行数一致
3. 按列合并（将所有 RecordBatch 的列合并为一个宽表）
4. 写入 out 通道

#### CatalogPlugin 注册

`catalog_plugin.cpp` 的 `Load()` 中：

```cpp
Register("concat", []() -> IOperator* { return new ConcatOperator(); });
Register("hstack", []() -> IOperator* { return new HstackOperator(); });
```

### A.6 多 SQL 任务

#### 提交接口变更

`POST /tasks/submit` 请求格式（向后兼容）：

```json
// 新格式（推荐）
{
  "sqls": ["SELECT * FROM ch1 INTO dataframe.tmp", "SELECT * FROM dataframe.tmp USING op INTO sqlite.db.out"],
  "timeout_s": 30
}

// 旧格式（兼容）
{
  "sql": "SELECT * FROM ch1 INTO dataframe.tmp"
}
```

内部统一转为 `std::vector<std::string> sqls`，存储时：
- `sqls_json`（新增字段）：完整 JSON 数组，不截断，执行时从此字段解析
- `request_sql`（现有字段）：保留为摘要，取第一条 SQL 截断到 200 字符，供列表展示

#### tasks 表新增字段

```sql
ALTER TABLE tasks ADD COLUMN sqls_json TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN sql_count INTEGER NOT NULL DEFAULT 1;
ALTER TABLE tasks ADD COLUMN current_sql_index INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN timeout_s INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN cancel_requested INTEGER NOT NULL DEFAULT 0;
```

向后兼容：`EnsureSchema()` 中用 `PRAGMA table_info(tasks)` 检查列是否存在，再决定是否执行 `ALTER TABLE ADD COLUMN`。**不依赖 `IF NOT EXISTS` 语法**（SQLite 3.37+ 才支持，不能假设版本）。

旧任务（`sqls_json` 为空）执行时回退到 `request_sql` 字段，作为单条 SQL 执行。

#### ExecuteOneTask 多 SQL 循环

```cpp
int TaskPlugin::ExecuteOneTask(const std::string& task_id) {
    // 1. 从 sqls_json 字段解析 sqls vector
    // 2. 收集中间通道集合：所有 SQL 的 dest，排除最后一条 SQL 的 dest（最终输出需保留）
    std::set<std::string> intermediate_channels;
    for (int i = 0; i + 1 < sqls.size(); i++) {
        SqlParser p; auto s = p.Parse(sqls[i]);
        if (!s.dest.empty()) intermediate_channels.insert(s.dest);
    }
    // 3. 循环执行每条 SQL：
    for (int i = 0; i < sqls.size(); i++) {
        // 检查 cancel flag
        if (IsCancelRequested(task_id)) { ... }
        // 更新 current_sql_index
        UpdateCurrentSqlIndex(task_id, i);
        // 执行 SQL
        int rc = ExecuteSingleSql(task_id, sqls[i], &rsp);
        if (rc != 0) {
            UpdateStatus(task_id, kFailed, ...);
            CleanupIntermediateChannels(intermediate_channels);
            return 0;
        }
    }
    // 4. 全部成功
    UpdateStatus(task_id, kCompleted, ...);
    CleanupIntermediateChannels(intermediate_channels);
    RunRetentionCleanup();
    return 0;
}
```

#### 中间通道清理

**清理策略**：只删除本任务明确创建的中间通道（非最后一条 SQL 的 dest），不依赖时序快照，不影响并发任务和最终输出。

**进程归属说明**：TaskPlugin、SchedulerPlugin、CatalogPlugin 在两种部署模式下均位于同一进程（deploy-single.yaml 的 `all` 服务，deploy-multi.yaml 的 `scheduler` 服务），进程内 IQuerier 调用成立。

```cpp
void TaskPlugin::CleanupIntermediateChannels(const std::set<std::string>& channels) {
    auto* registry = static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY));
    if (!registry) return;
    for (const auto& name : channels) {
        // name 是完整通道名（如 "dataframe.tmp"），IChannelRegistry 按不含前缀的名字注册
        // 需截取 "dataframe." 后的部分
        const std::string prefix = "dataframe.";
        if (name.rfind(prefix, 0) == 0) {
            registry->Unregister(name.substr(prefix.size()).c_str());
        }
    }
}
```

---

## Part B：Story 11.4 设计

### B.1 线程池改造

#### 锁机制原则（延续现有设计）

现有 TaskPlugin 的锁设计已经是高效的分层模型，Sprint 10 必须延续：

| 数据 | 保护方式 | 原因 |
|------|---------|------|
| `queue_`（任务队列） | `mu_` + `cv_` | 内存数据结构，需要互斥 |
| SQLite DB 操作 | **不持锁** | SQLite 以 serialized 模式编译（`sqlite3_threadsafe()=1`），单连接多线程访问由 SQLite 内部互斥保证，无需应用层加锁；WAL 模式额外保证多进程并发安全。实现时将 `sqlite3_open()` 改为 `sqlite3_open_v2(..., SQLITE_OPEN_FULLMUTEX)` 以显式声明意图，与 `sqlite_driver.cpp` 保持一致 |

取消信号通过 DB 的 `cancel_requested` 字段传递，不引入额外内存数据结构。

#### task_plugin.h 变更

```cpp
// 替换
std::thread worker_;
// 为
std::vector<std::thread> workers_;
int worker_threads_ = 4;
std::thread timeout_thread_;
int retention_days_ = 7;
int retention_max_count_ = 1000;
// 注：不引入 cancel_flags_，取消信号通过 DB cancel_requested 字段传递
```

#### Start()/Stop() 变更

```cpp
int TaskPlugin::Start() {
    running_.store(true);
    int n = disable_worker_ ? 0 : worker_threads_;
    for (int i = 0; i < n; i++) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
    timeout_thread_ = std::thread([this] { TimeoutLoop(); });  // 启动超时扫描线程
    return 0;
}

int TaskPlugin::Stop() {
    running_.store(false);
    cv_.notify_all();  // 唤醒所有 worker
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    return 0;
}
```

### B.2 取消机制

**不引入 `cancel_flags_` 内存结构**，直接以 DB 的 `cancel_requested` 字段作为取消信号，与现有"DB 是状态真相源"原则一致。

#### cancel_requested 枚举值

`cancel_requested` 字段使用整数枚举，区分取消来源，避免 worker 与 timeout_thread_ 的状态竞态：

| 值 | 含义 | 写入方 | worker 响应 |
|----|------|--------|------------|
| 0 | 无取消 | — | 继续执行 |
| 1 | 用户主动取消 | HandleCancel | 写终态 `cancelled` |
| 2 | 超时自动取消 | TimeoutLoop | 写终态 `timeout` |

`timeout_thread_` 直接写 `cancel_requested=2`，不写 status；worker 检查点读到 2 时写 `status=timeout`。两条路径写入不同的终态，无竞态。

#### HandleCancel 路由

```
POST /tasks/cancel
Body: {"task_id": "xxx"}
```

逻辑：
1. 查询任务状态（读 DB）
2. 若 `pending`：在 `mu_` 下尝试从 `queue_` 移除
   - 移除成功：写 DB 状态为 `cancelled`，返回 ok
   - 移除失败（任务已被 worker 取走）：重新读 DB 状态，若已变为 `running` 则走步骤 3；否则返回错误
3. 若 `running`：写 DB `cancel_requested=1`，worker 下次检查时自行停止并写 `cancelled`
4. 其他状态（终态）：返回错误

> **P4 说明**：步骤 2 的"移除失败后重读"处理了 pending→running 的 TOCTOU 竞态。极端情况下（重读时任务已完成），用户收到错误响应，任务以 completed/failed 结束，无数据损坏，可接受。

#### ExecuteOneTask 检查点

`TaskRecord` 当前没有 `cancel_requested` 字段，需单独查 DB 列（新增字段后 `GetTask` 会填充，或直接执行轻量 SELECT）：

```cpp
// 每条 SQL 执行前，单独读 cancel_requested 列
int cancel_val = 0;
GetCancelRequested(task_id, &cancel_val);  // SELECT cancel_requested FROM tasks WHERE task_id=?
if (cancel_val > 0) {
    CleanupIntermediateChannels(intermediate_channels);
    // cancel_requested=1 → cancelled，cancel_requested=2 → timeout
    TaskStatus final_status = (cancel_val == 2) ? kTimeout : kCancelled;
    UpdateStatus(task_id, final_status, ...);
    return 0;
}
```

> `TaskRecord` 需同步新增 `cancel_requested` 字段，或 `GetTask` 填充该字段，两种方式均可，实现时选其一保持一致。

**取消响应延迟** = 当前正在执行的 SQL 完成时间（SQL 语句间粒度，符合设计预期）。

### B.3 超时机制

#### timeout_thread_ 逻辑

```cpp
void TaskPlugin::TimeoutLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // 查询所有 running 且 timeout_s > 0 的任务
        // 检查 started_at + timeout_s < now
        // 超时则写 cancel_requested=2（区别于用户取消的 1），worker 检查点读到 2 后写 status=timeout
        CheckAndTimeoutTasks();
    }
}
```

#### 提交时传入 timeout_s

`HandleSubmit()` 解析 `timeout_s` 字段（默认 0 = 不超时），写入 DB。

### B.4 结构化诊断

#### task_diagnostics 表

```sql
CREATE TABLE IF NOT EXISTS task_diagnostics (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT NOT NULL,
    sql_index INTEGER NOT NULL,
    sql_text TEXT NOT NULL DEFAULT '',
    duration_ms INTEGER NOT NULL DEFAULT 0,
    source_rows INTEGER NOT NULL DEFAULT 0,
    sink_rows INTEGER NOT NULL DEFAULT 0,
    operator_chain TEXT NOT NULL DEFAULT '',  -- "op1,op2" 逗号分隔
    error_message TEXT NOT NULL DEFAULT '',
    FOREIGN KEY(task_id) REFERENCES tasks(task_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_task_diagnostics_task_id ON task_diagnostics(task_id);
```

注意：`EnsureDb()` 中需执行 `PRAGMA foreign_keys = ON`，否则 CASCADE DELETE 不生效。TaskPlugin 使用单连接持有（不重连），`EnsureDb()` 在 `Load()` 时调用一次即可，PRAGMA 对该连接全程有效。

#### WriteDiagnostic

每条 SQL 执行后调用：

```cpp
int TaskPlugin::WriteDiagnostic(const std::string& task_id, int sql_index,
                                 const std::string& sql_text, int64_t duration_ms,
                                 int64_t source_rows, int64_t sink_rows,
                                 const std::string& op_chain,
                                 const std::string& error_message);
```

#### HandleDiagnostics 路由

```
POST /tasks/diagnostics
Body: {"task_id": "xxx"}
Response:
{
  "task_id": "xxx",
  "diagnostics": [
    {
      "sql_index": 0,
      "sql_text": "SELECT * FROM ...",
      "duration_ms": 123,
      "source_rows": 1000,
      "sink_rows": 1000,
      "operator_chain": "builtin.passthrough",
      "error_message": ""
    }
  ]
}
```

### B.5 任务状态实时感知

#### 方案选择

SSE 需要跨进程推送（WebPlugin 在 web 进程，TaskPlugin 在 scheduler 进程），现有架构无进程间共享内存，RouterAgencyPlugin 的 `fnRouterHandler` 也是同步模型，无法支持长连接流式响应。引入独立 SSE 端口会破坏架构一致性，代价高于收益。

**决策：前端轮询**，复用现有 `POST /api/tasks/list`（getTasks）接口，每 2 秒刷新任务列表，任务进入终态后停止轮询。

#### 前端改动

`src/frontend/src/views/Tasks.vue` 中已有 3 秒全局轮询逻辑（`setInterval(loadTasks, 3000)`），调整为：
- 轮询间隔从 3s 改为 2s
- 任务进入终态（completed/failed/cancelled/timeout）后自动停止该任务的轮询（当前所有任务均为终态时暂停定时器）

`src/frontend/src/api/index.js` 新增 cancel 和 diagnostics 封装：

```js
cancelTask: (id) => api.post('/api/tasks/cancel', { task_id: id }),
getTaskDiagnostics: (id) => api.post('/api/tasks/diagnostics', { task_id: id }),
```

### B.6 保留策略

#### Option 配置

```
retention_days=7;retention_max_count=1000
```

- `retention_days=0`：禁用按时间清理
- `retention_max_count=0`：禁用按数量清理

#### RunRetentionCleanup

```cpp
void TaskPlugin::RunRetentionCleanup() {
    // 不持锁，直接执行 SQLite（WAL 模式保证并发安全）

    // 按时间清理
    if (retention_days_ > 0) {
        std::string sql = "DELETE FROM tasks WHERE status IN "
            "('completed','failed','cancelled','timeout') "
            "AND finished_at < datetime('now', '-" + std::to_string(retention_days_) + " days');";
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }

    // 按数量清理
    if (retention_max_count_ > 0) {
        std::string sql =
            "DELETE FROM tasks WHERE task_id IN ("
            "  SELECT task_id FROM tasks"
            "  WHERE status IN ('completed','failed','cancelled','timeout')"
            "  ORDER BY finished_at ASC"
            "  LIMIT MAX(0, (SELECT COUNT(1) FROM tasks"
            "    WHERE status IN ('completed','failed','cancelled','timeout')) - " +
            std::to_string(retention_max_count_) + "));";
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }
}
```

触发时机：每次任务进入终态（`UpdateStatus` 写入 terminal status 后）调用。延续现有模式，**不持锁执行 DELETE SQL**，直接调用 SQLite（WAL 模式保证并发安全），不阻塞其他 worker 的 DB 操作。

### B.7 Stop() 顺序

```cpp
int TaskPlugin::Stop() {
    running_.store(false);
    cv_.notify_all();           // 唤醒所有 worker
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    if (timeout_thread_.joinable()) timeout_thread_.join();  // 最后 join timeout 线程
    return 0;
}
```

`timeout_thread_` 必须在 workers 全部退出后再 join，确保不会访问已销毁的 DB 连接。

---

## 接口变更汇总

### 新增 HTTP 端点

| 层 | 方法 | 路径 | 说明 |
|----|------|------|------|
| TaskPlugin（内部） | POST | `/tasks/cancel` | 取消任务 |
| TaskPlugin（内部） | POST | `/tasks/diagnostics` | 查询诊断信息 |
| WebPlugin（对外 8081） | POST | `/api/tasks/cancel` | 代理到 TaskPlugin `/tasks/cancel` |
| WebPlugin（对外 8081） | POST | `/api/tasks/diagnostics` | 代理到 TaskPlugin `/tasks/diagnostics` |

WebPlugin 新增两条 `EnumRoutes()` 注册 + `HandleCancelTask()`/`HandleDiagnostics()` 实现（均为 `ProxyPostJson` 透传，与现有 submit/list/result/delete 模式一致）。

### 修改 HTTP 端点

| 方法 | 路径 | 变更 |
|------|------|------|
| POST | `/tasks/submit` | 新增 `sqls` 数组字段和 `timeout_s` 字段 |

### 新增 SQL 语法

```sql
-- 多源输入（新增）
SELECT * FROM ch1, ch2 USING builtin.concat INTO out
SELECT * FROM ch1, ch2, ch3 USING builtin.hstack INTO out

-- 多 SQL 任务（通过 API 提交，不是 SQL 语法变更）
{"sqls": ["sql1", "sql2"]}
```

---

## 关键文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `src/common/span.h` | 新增 | Span<T> 轻量模板 |
| `src/framework/interfaces/ioperator.h` | 修改 | 新增多输入 Work 默认实现 |
| `src/framework/core/sql_parser.h` | 修改 | SqlStatement.sources 字段 |
| `src/framework/core/sql_parser.cpp` | 修改 | FROM 多源解析 |
| `src/services/scheduler/scheduler_plugin.cpp` | 修改 | ExecuteWithOperatorChain source 改为 Span<IChannel*> |
| `src/services/catalog/builtin/concat_operator.h/cpp` | 新增 | 按行合并算子 |
| `src/services/catalog/builtin/hstack_operator.h/cpp` | 新增 | 按列合并算子 |
| `src/services/catalog/CMakeLists.txt` | 修改 | `file(GLOB DIR_SRCS *.cpp)` 改为 `GLOB_RECURSE` 以收录 builtin/*.cpp |
| `src/services/catalog/catalog_plugin.cpp` | 修改 | 注册 concat/hstack |
| `src/services/task/task_plugin.h` | 修改 | 线程池、取消、超时等新成员 |
| `src/services/task/task_plugin.cpp` | 修改 | 最大改动量 |
| `src/services/web/web_server.cpp` | 修改 | 新增 /api/tasks/cancel、/api/tasks/diagnostics 代理路由 |
| `src/frontend/src/views/Tasks.vue` | 修改 | 前端轮询：2s 间隔，终态自动停止 |
| `src/frontend/src/api/index.js` | 修改 | 新增 cancelTask、getTaskDiagnostics 封装 |
| `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp` | 扩展 | 11.3 测试 |
| `src/tests/test_task/test_task.cpp` | 扩展 | 11.4 测试 |
| `config/deploy-single.yaml` | 修改 | TaskPlugin option 新增配置项 |
