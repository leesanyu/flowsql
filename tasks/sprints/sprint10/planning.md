# Sprint 10 规划

## Sprint 信息

- **Sprint 周期**：Sprint 10
- **开始日期**：2026-03-23
- **预计工作量**：7 天
- **Sprint 目标**：交付 Epic 11 增强能力——多 SQL 任务、多输入算子、内置合并算子、线程池、任务取消/超时、前端轮询感知、结构化诊断、保留策略

---

## Sprint 目标

### 主要目标

1. **Story 11.3**：IOperator 多输入接口扩展 + SQL 多源语法 + 内置 concat/hstack 算子 + 多 SQL 任务
2. **Story 11.4**：单 worker → 线程池 + 任务取消/超时 + 前端轮询感知 + 结构化诊断 + 保留策略

### 成功标准

- [x] `FROM ch1, ch2 USING builtin.concat INTO out` 端到端执行成功
- [x] 多 SQL 任务中间 dataframe 通道在任务结束后自动清理
- [x] 线程池并发执行多个任务，worker_threads 可配置
- [x] pending/running 任务支持取消，running 任务在下一条 SQL 前生效
- [x] 超时任务自动标记为 timeout 状态
- [x] 前端任务列表轮询间隔 2s，任务进入终态后自动停止轮询
- [x] 任务完成后可查询结构化诊断信息（每条 SQL 耗时/行数/算子链）
- [x] 保留策略按时间和数量双维度可配置，任务进入终态后自动触发清理
- [x] 所有现有测试回归通过

---

## 设计文档

- [Sprint 10 统一设计文档](design.md)

---

## Story 列表

### Story 11.3：多算子 Pipeline（增强）

**优先级**：P1
**工作量估算**：3 天
**依赖**：无

**验收标准**：
- [x] `common/span.h` 新增 `Span<T>` 轻量模板（指针+长度，C++17 兼容）
- [x] `IOperator` 新增多输入 `Work(Span<IChannel*>, IChannel*)` 默认实现（降级到 inputs[0]）
- [x] `SqlStatement` 新增 `sources` 字段，`FROM ch1, ch2` 语法解析正确
- [x] `SchedulerPlugin` 多源分支：查找所有源通道，调用算子多输入 Work
- [x] `builtin.concat`：多 DataFrame 按行合并，schema 不兼容时返回明确错误
- [x] `builtin.hstack`：多 DataFrame 按列合并，行数不一致时返回明确错误
- [x] `CatalogPlugin::Load()` 注册 concat/hstack
- [x] 多 SQL 任务：`{"sqls":["sql1","sql2"]}` 提交，顺序执行，任一失败则整体 failed
- [x] 任务内中间 `dataframe.*` 通道任务结束后自动清理
- [x] 端到端测试通过（多源合并、多 SQL 任务、中间通道清理）

**任务分解**：
- [x] T1：`src/common/span.h` — 新增 `Span<T>`
- [x] T2：`src/framework/interfaces/ioperator.h` — 新增多输入 Work 默认实现
- [x] T3：`src/framework/core/sql_parser.h/cpp` — `sources` 字段 + FROM 多源解析
- [x] T4：`src/services/scheduler/scheduler_plugin.cpp` — 统一算子执行入口为 `ExecuteWithOperatorChain(Span<IChannel*>)`；Stage0 用原始 inputs，后续 stage 用单元素 Span；多源无算子报错；V1 仅支持 `dataframe.*` 多源且禁止 `WHERE`
- [x] T5：`src/services/catalog/builtin/concat_operator.h/cpp` — 按行合并算子
- [x] T6：`src/services/catalog/builtin/hstack_operator.h/cpp` — 按列合并算子
- [x] T7：`src/services/catalog/catalog_plugin.cpp` + `CMakeLists.txt` — 注册 concat/hstack；`file(GLOB DIR_SRCS *.cpp)` 改为 `GLOB_RECURSE` 以收录 builtin/*.cpp
- [x] T8：`src/services/task/task_plugin.cpp` — 多 SQL 任务提交/执行（读 sqls_json）/中间通道清理（按 dest 集合，排除最终输出）
- [x] T9：测试扩展（test_scheduler_e2e + test_task）
  - 测试必须走完整插件路径（PluginLoader → CatalogPlugin → IOperatorRegistry），不直接 `new ConcatOperator()`
  - 多 SQL 任务：两条 SQL 共享中间 dataframe 通道，任务结束后通道自动清理
  - 多 SQL 向后兼容：旧格式 `{"sql":"..."}` 提交，验证正常执行
  - concat 算子：覆盖 INT32/INT64/FLOAT/DOUBLE/STRING/BOOL 各类型列的按行合并；schema 不兼容时返回明确错误
  - hstack 算子：覆盖多类型列的按列合并；行数不一致时返回明确错误
  - Span<T>：空 Span 的 empty() 返回 true；越界访问触发 assert（需 -UNDEBUG 编译）

---

### Story 11.4：异步任务执行（增强）

**优先级**：P1
**工作量估算**：4 天
**依赖**：Story 11.3（多 SQL 任务基础）

**验收标准**：
- [x] `worker_threads=N` option 可配置线程池大小（默认 4）
- [x] 多 worker 并发执行，DB 操作线程安全
- [x] `POST /tasks/cancel` 支持取消 pending/running 任务
- [x] `timeout_s` 提交参数支持任务级超时，超时后状态变为 timeout
- [x] 前端轮询间隔 2s，任务进入终态后自动停止轮询
- [x] `POST /tasks/diagnostics` 返回结构化诊断（每条 SQL 耗时/行数/算子链）
- [x] `retention_days=N;retention_max_count=M` option 配置保留策略
- [x] 任务进入终态后自动触发清理，两种策略同时启用取更严格条件
- [x] 端到端测试通过（线程池并发、取消、超时、诊断、保留策略）

**任务分解**：
- [x] T10：`task_plugin.h/cpp` — 线程池改造（`workers_` vector，`worker_threads_` option，`sqlite3_open` 改为 `sqlite3_open_v2` + `SQLITE_OPEN_FULLMUTEX`，`timeout_thread_` 在 Start() 中启动）
- [x] T11：`task_plugin.h/cpp` — 取消机制（`HandleCancel` 路由，DB `cancel_requested` 字段，执行检查点）
- [x] T12：`task_plugin.h/cpp` — 超时机制（`timeout_thread_`，`timeout_s` DB 字段，扫描逻辑）
- [x] T13：`task_plugin.h/cpp` — 诊断（`task_diagnostics` 表，`WriteDiagnostic()`，`HandleDiagnostics` 路由 `POST /tasks/diagnostics`）
- [x] T14：`src/services/web/web_server.cpp` — 新增 `/api/tasks/cancel`、`/api/tasks/diagnostics` 代理路由（EnumRoutes + ProxyPostJson）
- [x] T15：`src/frontend/src/views/Tasks.vue` + `src/frontend/src/api/index.js` — 轮询间隔从 3s 调整为 2s，终态自动停止；新增 cancelTask/getTaskDiagnostics API 封装
- [x] T16：`task_plugin.h/cpp` — 保留策略（`retention_days/max_count` option，`RunRetentionCleanup()`）
- [x] T17：测试扩展（test_task）
  - 线程池并发：同时提交 N 个任务，验证：① 无 crash；② 两个任务写不同 dataframe 通道无数据串扰；③ Stop() 后 workers_ 为空、无悬挂线程（资源无泄漏）
  - 取消 pending 任务：提交后立即取消（任务尚未被 worker 取走），验证状态为 cancelled
  - 取消 running 任务：提交多 SQL 任务（第一条 SQL 使用 passthrough 处理足够大的 dataframe 保证执行时间 > 200ms），轮询到 status=running 后发取消请求，验证最终状态为 cancelled（而非 completed）
  - 超时：设置 timeout_s=1，提交慢任务，等待约 2s，验证状态变为 timeout（而非 cancelled，区分 cancel_requested=1 和 2）
  - 诊断：完成多 SQL 任务后查询 diagnostics，验证每条 SQL 的 duration_ms/source_rows/sink_rows/operator_chain 字段正确
  - CASCADE DELETE：完成任务后触发保留策略删除该任务，验证 task_diagnostics 中对应记录同步删除
  - 保留策略：插入超量任务，验证清理后数量符合配置
---

## 实施顺序

```
Day 1-3（Story 11.3）：
  T1 → T2 → T3 → T4 → T5 → T6 → T7 → T8 → T9

Day 4-7（Story 11.4）：
  T10 → T11 → T12 → T13 → T14 → T15 → T16 → T17
```

---

## 风险与缓解

| 风险 | 可能性 | 缓解措施 |
|------|--------|---------|
| 线程池多 worker 并发写 SQLite 死锁 | 中 | 所有 sqlite3 调用不在 mu_ 持有期间执行，WAL 模式已启用 |
| concat/hstack schema 检查逻辑复杂 | 低 | 复用 Arrow 的 schema 比较 API，不自己实现 |
| 多 SQL 任务中间通道清理遗漏 | 低 | 按 SQL `dest` 精确收集中间通道（排除最后一条 SQL 的 `dest`），任务结束后按集合清理 |
| ALTER TABLE 向后兼容旧 DB 文件 | 低 | 用 `PRAGMA table_info(tasks)` 检查列是否存在后再 ALTER，不依赖 `IF NOT EXISTS` 语法（SQLite 3.37+ 才支持） |

---

## 配置变更（deploy-single.yaml）

```yaml
- name: libflowsql_task.so
  option: "db_path=./meta/flowsql_meta.db;worker_threads=4;retention_days=7;retention_max_count=1000"
```
