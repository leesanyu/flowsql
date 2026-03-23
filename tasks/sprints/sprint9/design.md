# Sprint 9 设计文档

本文合并 Sprint 9 的两部分设计：
1. Epic 10：算子目录与状态下沉 CatalogPlugin 重构
2. Epic 11：Pipeline MVP（串行 + 异步任务）

---

## Part A：Epic 10 设计

# 设计文档：算子目录与状态下沉 CatalogPlugin 重构

日期：2026-03-21
更新：2026-03-22（审查修订）
作者：FlowSQL 开发组

## 1. 背景与问题

当前算子目录相关职责存在跨层混杂：

1. `WebServer` 维护本地 `operators` 表（SQLite），承担了算子目录摘要信息与激活状态持久化。
2. `Scheduler` 通过 `/operators/native/query` 返回运行时算子列表（C++ + Python），形成另一套“实时视图”。
3. `BridgePlugin` 仅负责 Python 算子发现与执行，不承担目录状态管理。

导致的问题：

1. **状态真相分裂**：Web 本地表、Scheduler 运行时、Bridge 发现结果不一致。
2. **边界不清**：Web 层承担了不属于展示层的目录持久化职责。
3. **查询异常**：列表可见但详情不存在（`operator not found`）等一致性问题。
4. **扩展困难**：未来新增算子类型时，Web/Bridge/Scheduler 都要改目录逻辑。

## 2. 重构目标

本次重构目标是将“算子目录 + 激活状态”统一收敛到 `CatalogPlugin`。

1. `CatalogPlugin` 成为算子目录与状态唯一来源（Single Source of Truth）。
2. `WebServer` 从目录持久化职责中退出，仅做 API 网关/转发。
3. `BridgePlugin` 将 Python 发现结果同步到 Catalog（upsert），不单独持有目录状态。
4. `Scheduler` 执行前只以 Catalog 激活状态作为准入依据。
5. 采用一次性切换方式完成重构，不保留旧实现并行路径。

## 3. 非目标（本次不做）

1. 不调整 SQL 语法（`USING catelog.name` 继续保留）。
2. 不改变 Python 算子执行协议（Arrow IPC + `/operators/python/work/...` 保持）。
3. 不引入额外的兼容层（例如双写、回退开关、多数据源并存）。

## 4. 目标架构

### 4.1 职责划分

1. `CatalogPlugin`
   - 维护算子目录元数据：`catelog/name/type/source/position/description/editable/...`
   - 维护状态字段：`active`, `updated_at`
   - 提供统一接口：列表、详情、激活、去激活、更新
   - 暴露 `IOperatorCatalog` 进程内接口供 Scheduler/Bridge 直接调用

2. `BridgePlugin`
   - 继续负责 Python 算子发现/刷新/执行
   - 在 `Start/Refresh` 后通过 `IOperatorCatalog` 接口（IQuerier，**不走 HTTP**）进行 **upsert**
   - 不持久化激活状态，仅同步算子静态元信息

3. `Scheduler`
   - 统一执行层
   - `Start()` 时通过 `IOperatorCatalog` 接口注册 C++ 算子
   - 执行前通过 `IOperatorCatalog::QueryStatus()` 校验算子状态
   - 不承担算子目录查询接口

4. `WebServer`
   - 只转发（proxy）Catalog / Scheduler API
   - 不再直接写 `operators` 表

### 4.2 C++ 算子注册机制

C++ 算子（`type=cpp`）由 `SchedulerPlugin` 在 `Start()` 时通过 `IOperatorCatalog` 接口批量注册到 Catalog：

1. Scheduler 在 `Start()` 中通过 `IQuerier::First(IID_OPERATOR_CATALOG)` 获取 Catalog 接口
2. 调用 `UpsertBatch()` 将所有已加载的 C++ 算子元信息写入 `operator_catalog`
3. upsert 语义与 Python 算子一致：新记录 `active=0`，已有记录不覆盖 `active`

**运行时动态注册（未来扩展，本 Sprint 不实现）**：

未来支持算子插件热加载时，新算子 .so 加载后触发 `IOperatorCatalog::UpsertBatch()` 完成注册，与 Python 算子刷新路径对称。本 Sprint 仅实现启动时静态注册。

### 4.3 数据流（目标）

1. Python Worker 发现算子 -> Bridge `DiscoverOperators`
2. Bridge 通过 `IOperatorCatalog` 接口（IQuerier）调 Catalog `UpsertBatch` 同步元信息
3. Scheduler `Start()` 时通过 `IOperatorCatalog` 接口注册 C++ 算子
4. 前端查询算子列表/详情 -> Web 转发 Catalog
5. 用户激活/去激活 -> Web 转发 Catalog -> Catalog 持久化状态
6. Scheduler 执行 SQL -> 通过 `IOperatorCatalog::QueryStatus()` 校验状态 -> 执行算子

## 5. 接口设计

> 以下为建议接口，最终 URI 与参数可按团队命名规范微调。

### 5.1 CatalogPlugin 新增/扩展接口

1. `POST /operators/upsert_batch`
   - 入参：
     - `operators: [{catelog,name,description,position,type,source}]`
     - `type`：`cpp` / `python` / `builtin`
     - `source`：`scheduler`（C++ 算子）/ `bridge`（Python 算子）/ `system`
   - 语义：按 `catelog+name` upsert；新记录 `active` 初始值为 `0`（需用户手动激活）；已有记录不覆盖 `active`（保留用户状态）
   - **注意**：此接口仅供 HTTP 外部调用（如测试/运维）；进程内调用走 `IOperatorCatalog::UpsertBatch()`

2. `GET /operators/query`
   - 返回统一目录视图（含激活状态，C++ + Python 算子均包含）

3. `POST /operators/detail`
   - 入参：`{"name":"catelog.opname"}`
   - 返回目录详情（必要时携带 `content` 代理字段或来源指针）

4. `POST /operators/activate`
5. `POST /operators/deactivate`
6. `POST /operators/update`

### 5.2 IOperatorCatalog 进程内接口（Scheduler / Bridge 调用）

`CatalogPlugin` 暴露 `IOperatorCatalog` 纯虚接口，Scheduler 和 Bridge 均通过 `IQuerier::First(IID_OPERATOR_CATALOG)` 获取，**不走 HTTP**。

```cpp
// 算子准入状态枚举，语义明确区分"未注册"与"已去激活"
enum class OperatorStatus {
    kNotFound    = 0,  // 未注册到 Catalog
    kDeactivated = 1,  // 已注册但 active=0
    kActive      = 2,  // 已注册且 active=1
};

struct OperatorMeta {
    std::string catelog;
    std::string name;
    std::string type;        // cpp / python / builtin
    std::string source;      // scheduler / bridge / system
    std::string description;
    std::string position;
};

// 批量 upsert 结果，用于调用方日志与计数
struct UpsertResult {
    int32_t success_count = 0;
    int32_t failed_count  = 0;
    std::string error_message;  // 首条失败原因，便于日志定位
};

interface IOperatorCatalog {
    // 准入校验：返回枚举，调用方按枚举值映射错误码
    virtual OperatorStatus QueryStatus(
        const std::string& catelog,
        const std::string& name) = 0;

    // 批量 upsert：Scheduler(C++) 和 Bridge(Python) 均调此接口
    // 语义：新记录 active=0；已有记录不覆盖 active
    // 调用方必须检查 failed_count > 0 并打 ERROR 日志；注册失败不阻断启动
    virtual UpsertResult UpsertBatch(
        const std::vector<OperatorMeta>& operators) = 0;
};
```

Scheduler 执行前校验映射：

| `QueryStatus` 返回值 | 错误响应 |
|---|---|
| `kNotFound` | `operator not found` |
| `kDeactivated` | `operator is deactivated` |
| `kActive` | 放行执行 |

去激活并发语义：

1. `deactivate` 仅影响新任务准入，不中断已进入 `running` 的任务
2. 任务在 `pending -> running` 时固化算子使用快照（至少记录算子名与校验时刻）
3. 运行中任务继续执行到终态；后续新任务命中去激活算子返回 `operator is deactivated`

### 5.3 Web API 对外契约

Web 对前端统一提供：

1. `GET /api/operators`
2. `POST /api/operators/detail`
3. `POST /api/operators/activate`
4. `POST /api/operators/deactivate`
5. `POST /api/operators/update`

内部管理 URI 对应关系（Web 仅做代理，不再访问本地 `operators` 表）：

| 前端 URI | 方法 | 内部管理 URI | 目标服务 | 说明 |
|---|---|---|---|---|
| `/api/operators` | GET | `/operators/query` | Catalog | 统一目录查询入口 |
| `/api/operators/detail` | POST | `/operators/detail` | Catalog | 查询目录详情与状态；Python 详情由 Catalog/Bridge 协同提供 |
| `/api/operators/activate` | POST | `/operators/activate` | Catalog | 激活状态持久化唯一入口 |
| `/api/operators/deactivate` | POST | `/operators/deactivate` | Catalog | 去激活状态持久化唯一入口 |
| `/api/operators/update` | POST | `/operators/update` | Catalog | 更新算子元数据（描述、位置等） |

约束：

1. Web 不得直接读写 `operators` 本地表。
2. 除统一列表查询外，状态与详情相关操作均以 Catalog 为准。

## 6. 存储模型建议（Catalog）

建议在 Catalog 内维护 `operator_catalog`：

1. `catelog` TEXT
2. `name` TEXT
3. `type` TEXT (`cpp` / `python` / `builtin` ...)
4. `description` TEXT
5. `position` TEXT
6. `source` TEXT (`scheduler` / `bridge` / `system`)
7. `active` INTEGER
8. `editable` INTEGER
9. `content_ref` TEXT（可选：源码来源引用）
10. `created_at` / `updated_at`

唯一键：`(lower(catelog), lower(name))`

## 7. 重构实施方案（直接切换）

前提声明（构件阶段）：

1. 本 Sprint 处于软件构件阶段，不做历史兼容，不保留旧 `operators` 表存量状态。
2. 切换时以 Catalog 当前回灌结果为唯一目录状态，不执行旧表到 Catalog 的迁移脚本。

1. Catalog 实现目录接口与表结构（列表/详情/激活/去激活/更新/upsert），同步实现 `IOperatorCatalog` 接口供 Scheduler/Bridge 通过 IQuerier 调用。
2. Bridge 增加 `UpsertBatch` 同步（`Start/Refresh` 后通过 `IOperatorCatalog` 接口回灌 Python 算子元信息，**不走 HTTP**）。
3. Scheduler 在 `Start()` 时通过 `IOperatorCatalog::UpsertBatch()` 注册 C++ 算子；执行前状态校验改为调用 `IOperatorCatalog::QueryStatus()`。
4. Web `/api/operators*` 全部改为转发 Catalog/Scheduler，不再读写本地 `operators` 表。
5. 切换完成并验证无旧路径引用后，删除 `/operators/native/query` 接口及其所有调用方引用。
6. 删除 Web 侧 `operators` 表及相关 SQL、数据访问代码。
7. 删除历史兼容代码、注释与无效配置项，同步更新设计/计划/测试文档与用例。

验收：算子目录状态唯一来源为 Catalog；前端列表/详情/激活与执行链路均基于 Catalog + Bridge；仅保留 Catalog 的 `/operators/query`。

## 8. 风险与应对

1. **直接切换期间出现状态缺失**
   - 应对：先完成 Bridge -> Catalog 全量同步，再切 Web/Scheduler 读路径；切换前执行全量回灌。

2. **Catalog 不可用导致前端不可用**
   - 应对：加强 Catalog 启动与健康检查；切换前完成稳定性压测。

3. **Bridge 批量同步覆盖用户状态**
   - 应对：upsert 明确“禁止覆盖 active”

4. **执行链路回归风险**
   - 应对：增加 e2e 用例覆盖 `active` 准入与详情一致性

## 9. 测试计划

### 9.1 单元测试

1. Catalog upsert 语义（不覆盖 active）
2. Catalog activate/deactivate 幂等
3. Bridge 同步去重与静态元信息更新（通过 IOperatorCatalog 接口，不走 HTTP）
4. `/operators/query` 字段映射单测（Catalog 记录 -> API 响应）
5. `source/type` 枚举值与默认值校验（脏数据防御）
6. `QueryStatus` 返回值：kNotFound / kDeactivated / kActive 三种场景
7. Scheduler `Start()` 后 C++ 算子在 Catalog 中可查（`/operators/query` 包含 C++ 条目）

### 9.2 集成测试

1. Bridge 刷新后 Catalog 列表可见 Python 算子
2. Scheduler 启动后 Catalog 列表可见 C++ 算子
3. Web 列表/详情与 Catalog 一致（C++ + Python 均包含）
4. 激活后可执行，去激活后执行拒绝（`kDeactivated` 错误码）
5. 未注册算子执行拒绝（`kNotFound` 错误码）
6. `/operators/query` 返回 C++ + Python 目录条目且字段完整
7. Bridge 全量回灌后，Catalog 中 Python 算子目录完整可查
8. Python 算子刷新后不误清理用户配置字段（如 `active`）

### 9.3 回归测试

1. `/operators/query` 返回统一算子目录视图（C++ + Python）
2. 前端 Operators 页面操作链路不变
3. 现有 SQL 执行链路不回归
4. 空目录/部分桥接异常时，列表与详情错误码语义稳定
5. 历史 `/operators/native/query` 调用已全部移除，不再被任何入口引用

### 9.4 并发与幂等测试

1. 并发 activate/deactivate 同一算子，最终状态可预测且无中间脏写。
2. Bridge `upsert_batch` 与用户 `deactivate` 并发时，`active` 不被覆盖。
3. 重复执行 `upsert_batch` 不产生重复记录（唯一键稳定）。
4. 并发详情查询与状态变更不会返回结构不完整响应。
5. 算子 `deactivate` 与运行中任务并发时：已 `running` 任务不中断，新任务被准入拒绝。

### 9.5 契约与性能测试

1. `/operators/query` contract test：必填字段、可选字段、枚举值、空值语义。
2. C++/Python 两类算子差异字段 contract test（如 `content_ref`、`source`）。
3. 规模压测：1k/5k 算子下列表与详情查询的 P95/P99 延迟。
4. Operators 页面端到端耗时回归（首屏、详情弹窗、状态切换）。
5. 失败注入：Catalog 不可用、Bridge 延迟/失败时的错误码与超时行为。

## 10. 实施顺序建议

1. 先做 Catalog 接口和存储模型
2. 再做 Bridge -> Catalog 同步
3. 切 Catalog `/operators/query` 与 Scheduler 准入校验
4. 再切 Web 读写路径并删除旧表逻辑

## 11. 里程碑与交付件

1. M1：Catalog API 与表结构上线
   - 交付门槛：单元测试通过，`operator_catalog` 唯一键与索引生效，基础 CRUD 可用。
2. M2：Bridge 同步上线（目录自动更新）
   - 交付门槛：Bridge 全量回灌与 Catalog 数量一致；重复同步无重复记录；`active` 不被覆盖。
3. M3：Catalog `/operators/query` + Scheduler 准入切 Catalog
   - 交付门槛：执行前状态校验全部走 Catalog；`/operators/native/query` 代码与调用引用清零。
4. M4：Web 切 Catalog + 删除旧表与旧路径
   - 交付门槛：Web 不再读写 `operators` 表；端到端操作链路通过；旧表与旧 SQL 清理完成。
5. 发布判定（Go/No-Go）
   - `/operators/query` 成功率 >= 99.9%，错误率 <= 0.1%（压测与联调环境）
   - 1k/5k 算子规模下列表与详情查询 P95 达标（阈值按环境基线固化到测试配置）
   - 关键回归与契约测试通过率 100%，无 blocker 级缺陷

## 12. 结论

这是一次必要的中等规模重构。核心价值不在“新增功能”，而在于建立清晰边界和单一状态真相，解决当前算子列表/详情/激活状态分裂问题，并为后续算子类型扩展提供稳定基础。

---

## Part B：Epic 11 设计

# 设计文档：Epic 11 MVP（Pipeline 串行 + 异步任务）

> 状态：待评审
> 创建：2026-03-21
> 更新：2026-03-21
> 适用迭代：Sprint 9

---

## 1. 目标与范围（仅 MVP）

本设计仅覆盖可交付 MVP，目标是“可开发、可测试、可上线”，不讨论增强能力。

MVP 交付目标：

1. 支持串行 Pipeline：`USING op1 THEN op2`。
2. 支持异步任务闭环：提交、列表、详情、删除。
3. 任务状态统一由 `TaskPlugin` 持久化。
4. Web 任务页支持操作：`查看结果`、`删除`。

MVP 明确不做：

1. 不做 Pipeline 并行执行。
2. 不做重试策略与复杂容错策略。
3. 不做 WebSocket 推送（先轮询）。
4. 不做完整结果数据集持久化。
5. 不做运行中任务取消（仅保留 `cancelled` 状态枚举用于前向兼容）。
6. 不做超时控制（仅保留 `timeout` 状态枚举用于前向兼容，MVP 不触发此状态）。

---

## 2. 术语与职责边界

组件职责：

1. `SchedulerPlugin`
   - 解析 SQL 与 Pipeline 算子链。
   - 创建异步任务并入队执行。
   - 推进状态并写入 TaskPlugin。

2. `TaskPlugin`
   - 任务元数据、状态、错误、结果摘要的唯一存储。
   - 提供列表、详情、删除查询能力。

3. `BridgePlugin`
   - Python 算子执行容器。
   - 返回执行结果/错误，不直接写任务状态。

4. `WebServer`
   - 代理任务 API。
   - 只做展示层逻辑，不持久化任务数据。

单一真相：

1. 任务状态真相：`TaskPlugin`
2. 算子目录真相：`CatalogPlugin`
3. 执行控制真相：`SchedulerPlugin`

---

## 3. MVP 功能需求（可验收）

### 3.1 Story 11.1 Pipeline 串行

1. 支持 `USING op1 THEN op2` 语法。
2. 按左到右串行执行。
3. 任一步失败，任务整体失败。
4. 失败时记录失败算子名（`error_stage`）。

### 3.2 Story 11.2 异步任务

1. 支持任务提交：返回 `task_id`。
2. 支持任务列表分页查询。
3. 支持任务详情查询（包含错误信息或结果摘要）。
4. 支持历史任务删除（仅删除任务记录）。
5. 任务状态包含：`pending/running/completed/failed/cancelled/timeout`。

### 3.3 Web 交互语义

1. 操作列包含：`查看结果`、`删除`。
2. `查看结果`：
   - `failed` 展示：`error_code/error_message/error_stage`
   - `completed` 展示：`result_row_count/result_target`
   - 其他状态展示状态说明，不展示结果。
3. `删除`：
   - 二次确认。
   - 删除后列表刷新。
   - 提示“仅删除任务记录，不删除业务数据”。

---

## 4. 任务状态机与并发规则

状态集合：`pending/running/completed/failed/cancelled/timeout`

允许流转：

1. `pending -> running`
2. `running -> completed`
3. `running -> failed`
4. `running -> timeout`

MVP 规则：

1. `cancelled` 仅保留状态定义，不开放外部取消接口。
2. `timeout` 仅保留状态定义，MVP 不触发此状态（超时控制为后续增强项）。
3. 终态（`completed/failed/cancelled/timeout`）不可再变更。
4. 相同状态重复写入幂等（返回成功，不重复写事件）。
5. 非法流转返回 `conflict`。

并发约束：

1. 状态更新按 `task_id` 串行化（TaskPlugin 内部按 task_id 加锁或事务条件更新）。
2. 使用”条件更新”防止乱序覆盖：
   - 示例：仅当当前状态为 `pending` 时允许更新为 `running`。

### 4.1 进程重启孤儿任务清理

进程重启后，内存队列中的 `pending` 任务丢失，`running` 任务无法继续执行，这两类任务会永久卡在非终态。

**MVP 处理策略**：TaskPlugin 在 `Start()` 时执行一次孤儿任务清理：

```sql
-- 将所有非终态任务强制置为 failed
UPDATE task_runs
SET status = 'failed',
    error_code = 'PROCESS_RESTART',
    error_message = 'Task interrupted by process restart',
    finished_at = CURRENT_TIMESTAMP,
    updated_at = CURRENT_TIMESTAMP
WHERE status IN ('pending', 'running');
```

同时为每条被清理的任务写入 `task_events` 记录（`from_status=原状态, to_status=failed, message=PROCESS_RESTART`）。

**已知限制**：`request_sql` 截断存储 4KB，仅用于展示，不保证可执行，不支持自动重试。

---

## 5. 数据模型（TaskPlugin）

MVP 建议两张表：主表 + 事件表。

### 5.1 主表 `task_runs`

字段：

1. `task_id` TEXT PRIMARY KEY
2. `request_sql` TEXT NOT NULL
3. `status` TEXT NOT NULL
4. `created_at` DATETIME NOT NULL
5. `started_at` DATETIME NULL
6. `finished_at` DATETIME NULL
7. `duration_ms` INTEGER NULL
8. `result_row_count` INTEGER NULL
9. `result_target` TEXT NULL
10. `error_code` TEXT NULL
11. `error_message` TEXT NULL
12. `error_stage` TEXT NULL
13. `updated_at` DATETIME NOT NULL

索引：

1. `idx_task_runs_status_created_at(status, created_at desc)`
2. `idx_task_runs_created_at(created_at desc)`

### 5.2 事件表 `task_events`（MVP 推荐）

字段：

1. `id` INTEGER PRIMARY KEY AUTOINCREMENT
2. `task_id` TEXT NOT NULL
3. `from_status` TEXT NULL
4. `to_status` TEXT NOT NULL
5. `event_time` DATETIME NOT NULL
6. `message` TEXT NULL

用途：

1. 追踪状态流转。
2. 支撑故障诊断与测试断言。

### 5.3 删除语义

1. 仅允许删除终态任务（`completed/failed/timeout/cancelled`）。
2. `pending/running` 删除返回 `conflict`。
3. 删除 `task_runs` 与关联 `task_events`。
4. 不触达业务数据（输出表/输出通道）。

---

## 6. 接口契约（MVP）

路由层级：

1. 对外：`/api/tasks/*`（Web 对前端）
2. 内部：`/tasks/*`（Web -> Scheduler/TaskPlugin）

方法约束边界：

1. `/api/tasks/*` 为前端 Web 服务对外契约，可按 REST 语义使用 `GET`（例如列表查询）或 `POST`。
2. `/tasks/*` 为内部管理通道，统一采用 `POST + JSON body`（不使用 `GET + query string`）。

### 6.1 提交任务

`POST /api/tasks/submit`

请求：

```json
{
  "sql": "select * from mysql.prod.orders using clean then enrich into dataframe.r1",
  "mode": "async"
}
```

`mode` 字段（可选，默认 `async`）：
- `async`：异步执行，立即返回 `task_id`，通过轮询查询状态。
- `sync`：同步执行，阻塞直到完成，适用于简单 SQL 的快速验证。生产环境建议使用 `async`。

响应：

```json
{
  "task_id": "tsk_20260321_000001",
  "status": "pending"
}
```

校验：

1. `sql` 不能为空。
2. 非法 SQL 返回 `bad_request`。

### 6.2 任务列表

`GET /api/tasks/list?page=1&page_size=20&status=completed`

响应：

```json
{
  "total": 128,
  "items": [
    {
      "task_id": "tsk_20260321_000001",
      "status": "completed",
      "created_at": "2026-03-21T16:00:00Z",
      "duration_ms": 932,
      "result_row_count": 1042,
      "result_target": "dataframe.r1"
    }
  ]
}
```

### 6.3 任务详情

`POST /api/tasks/detail`

请求：

```json
{ "task_id": "tsk_20260321_000001" }
```

响应（failed）：

```json
{
  "task_id": "tsk_20260321_000001",
  "status": "failed",
  "error_code": "OP_EXEC_FAIL",
  "error_message": "python operator crashed",
  "error_stage": "enrich"
}
```

响应（completed）：

```json
{
  "task_id": "tsk_20260321_000002",
  "status": "completed",
  "result_row_count": 1042,
  "result_target": "mysql.prod.tbl_orders_enriched"
}
```

### 6.4 删除任务

`POST /api/tasks/delete`

请求：

```json
{ "task_id": "tsk_20260321_000001" }
```

响应：

```json
{ "ok": true }
```

失败语义：

1. 不存在 -> `not_found`
2. 非终态 -> `conflict`

### 6.5 内部状态更新（Scheduler -> TaskPlugin）

Scheduler 通过 IQuerier 查找 `ITaskStore` 接口直接调用，**不走 HTTP**。

`ITaskStore::UpdateState()` 调用参数：

```cpp
struct TaskStateUpdate {
    std::string task_id;
    std::string to_status;
    std::string error_code;    // 可选
    std::string error_message; // 可选
    std::string error_stage;   // 可选
    int64_t result_row_count;  // 可选，completed 时填写
    std::string result_target; // 可选，completed 时填写
};
```

语义：

1. 执行状态机合法性校验。
2. 更新主表。
3. 写入事件表。

### 6.6 内部通道契约（Web -> TaskPlugin，POST + JSON body）

Web 收到前端请求后，透传到内部 `/tasks/*`。以下为内部通道的最小 schema，实现时以此为基准，不得随意增减字段。

| 方法 | 内部 Path | 请求体 | 响应体 | 错误码 |
|------|-----------|--------|--------|--------|
| POST | `/tasks/submit` | `{"sql":"..."}` | `{"task_id":"...","status":"pending"}` | `bad_request`（sql 为空或非法） |
| POST | `/tasks/list` | `{"page":1,"page_size":20,"status":"completed"}` | `{"total":N,"items":[...]}` | — |
| POST | `/tasks/detail` | `{"task_id":"..."}` | 见下方 | `not_found` |
| POST | `/tasks/delete` | `{"task_id":"..."}` | `{"ok":true}` | `not_found`、`conflict`（非终态） |

`/tasks/list` 响应 items 字段（每条）：

```json
{
  "task_id": "tsk_20260321_000001",
  "status": "completed",
  "created_at": "2026-03-21T16:00:00Z",
  "duration_ms": 932,
  "result_row_count": 1042,
  "result_target": "dataframe.r1"
}
```

`/tasks/detail` 响应（failed）：

```json
{
  "task_id": "tsk_20260321_000001",
  "status": "failed",
  "created_at": "2026-03-21T16:00:00Z",
  "started_at": "2026-03-21T16:00:01Z",
  "finished_at": "2026-03-21T16:00:03Z",
  "duration_ms": 2100,
  "error_code": "OP_EXEC_FAIL",
  "error_message": "python operator crashed",
  "error_stage": "enrich"
}
```

`/tasks/detail` 响应（completed）：

```json
{
  "task_id": "tsk_20260321_000002",
  "status": "completed",
  "created_at": "2026-03-21T16:00:00Z",
  "started_at": "2026-03-21T16:00:01Z",
  "finished_at": "2026-03-21T16:00:02Z",
  "duration_ms": 932,
  "result_row_count": 1042,
  "result_target": "mysql.prod.tbl_orders_enriched"
}
```

约束：

1. `status` 过滤为可选参数，缺省时返回全部状态。
2. `page` / `page_size` 缺省值：`page=1`，`page_size=20`。
3. 内部通道不对前端直接暴露，Web 层负责参数校验后再透传。

---

## 7. 执行时序（开发必读）

### 7.1 成功路径

1. Web 调用 `submit`。
2. Scheduler 生成 `task_id`，写 `pending` 到 TaskPlugin。
3. Scheduler 将任务入队。
4. Worker 拉取任务，写 `running`。
5. 执行 SQL + `THEN` 算子链。
6. 成功后写 `completed + result_row_count + result_target`。

### 7.2 失败路径

1. 在第 N 个算子失败。
2. Scheduler 捕获异常，提取 `error_code/error_message/error_stage`。
3. 写 `failed` 状态到 TaskPlugin。
4. Web 详情页展示失败信息。

### 7.3 删除路径

1. Web 发起删除。
2. TaskPlugin 校验任务是否终态。
3. 删除记录并返回成功。

---

## 8. 代码落地建议（MVP）

### 8.1 Pipeline 串行数据传递机制

`USING op1 THEN op2` 拆解为多个 Pipeline 步骤，中间用临时内存 DataFrame 通道衔接：

```
step 1: actual_source → [op1] → tmp_channel_1
step 2: tmp_channel_1 → [op2] → actual_sink
```

实现要点：
1. `SqlStatement` 增加 `op_chain: vector<pair<string,string>>`（catelog+name 列表），替换原有 `op_catelog/op_name` 单字段
2. SQL 解析器在 `USING` 后支持 `THEN` 分隔的多算子：`USING cat.op1 THEN cat.op2`
3. `ExecuteWithOperator` 改为循环：每步创建临时 DataFrame 通道，最后一步直接用 `actual_sink`，执行完成后释放中间通道
4. 单算子场景（`op_chain` 长度为 1）行为与现有逻辑完全一致，无回归风险

**失败路径资源清理**：

任意步骤失败时，必须释放所有已创建的临时通道，不得泄漏：

```
// 伪代码
vector<TmpChannel*> tmp_channels;
try {
    for each step:
        create tmp_channel, push to tmp_channels
        execute step
} catch (...) {
    for each tmp_channel in tmp_channels:
        tmp_channel->Close()
        destroy tmp_channel
    throw;
}
// 成功路径同样在 finally 块释放
```

**`result_row_count` 语义**：记录最后一步写入 `actual_sink` 的行数，中间步骤的行数不计入。

### 8.2 新增接口

建议新增接口：

1. `src/framework/interfaces/ioperator_catalog.h`
   - `QueryStatus(catelog, name) -> OperatorStatus`（枚举：kNotFound/kDeactivated/kActive）
   - `UpsertBatch(operators) -> UpsertResult`（含 success_count/failed_count/error_message）
   - 调用方检查 `failed_count > 0` 打 ERROR 日志，不阻断启动
   - Scheduler 和 Bridge 均通过 IQuerier 查找此接口

2. `src/framework/interfaces/itask_store.h`
   - `CreateTask`
   - `UpdateState`
   - `ListTasks`
   - `GetTask`
   - `DeleteTask`

### 8.3 新增模块

建议新增模块：

1. `src/services/task/`
   - `task_plugin.h/.cpp`
   - `plugin_register.cpp`
   - `CMakeLists.txt`

### 8.4 改造模块

建议改造模块：

1. `src/services/scheduler/`
   - 增加异步任务队列与 worker（MVP 单线程 worker，并发度=1）
   - 增加 pipeline then 执行路径（SqlStatement op_chain + 临时通道链，含失败清理）
   - 接入 `ITaskStore`（通过 IQuerier）
   - 接入 `IOperatorCatalog`（通过 IQuerier）做准入校验与 C++ 算子注册

2. `src/services/catalog/`
   - 实现 `IOperatorCatalog` 接口（`QueryStatus` + `UpsertBatch`）
   - 新增 `operator_catalog` 表与 CRUD

3. `src/services/web/`
   - 增加 `/api/tasks/*` 代理接口

4. `src/frontend/src/views/`
   - 新增/改造任务列表页面，落地查看结果/删除（轮询间隔建议 3s）

### 8.5 task_id 生成策略

使用时间序列号：`tsk_{yyyyMMddHHmmss}_{seq}`。

`seq` 在进程启动时从数据库初始化：

```sql
-- tsk_ (4) + yyyyMMddHHmmss (14) + _ (1) = 19 字符前缀，seq 从第 20 位开始
SELECT COALESCE(MAX(CAST(SUBSTR(task_id, 20) AS INTEGER)), 0) + 1
FROM task_runs
WHERE task_id LIKE 'tsk_%';
```

之后用 `std::atomic<uint64_t>` 递增，保证单进程内唯一。重启后从数据库最大值续接，避免跨重启冲突。

---

## 9. 可观测性与运营指标（MVP）

最小指标：

1. `tasks_submitted_total`
2. `tasks_completed_total`
3. `tasks_failed_total`
4. `task_duration_ms`（直方图）
5. `task_queue_depth`

日志要求：

1. 每次状态变更必须打结构化日志：`task_id/from/to/error_code`。
2. 失败日志必须包含 `error_stage`。

---

## 10. 测试矩阵（MVP）

> **L21 约束**：以下所有测试用例必须落地为自动化测试代码，不得仅停留在文档中。每个 Story 的验收标准包含"测试用例已实现并通过"。

### 10.1 单元测试

1. `THEN` 语法解析：单算子、双算子、非法语法。
2. 状态机：合法流转、非法流转、终态保护。
3. 幂等：重复写入同状态。
4. 删除：非终态删除冲突，终态删除成功。
5. 孤儿任务清理：TaskPlugin `Start()` 后 `pending/running` 任务被置为 `failed`，终态任务不受影响。
6. Pipeline 失败清理：双算子链第 1 步失败时，已创建的临时通道被释放（无资源泄漏）。

### 10.2 集成测试

1. 提交异步任务 -> `pending/running/completed` 全链路。
2. 双算子链第 2 步失败 -> `failed + error_stage=op2`。
3. completed 详情仅返回摘要（`result_row_count` 为最后一步写入 sink 的行数），不返回数据集。
4. failed 详情返回错误三元组。
5. 删除任务后列表不可见且业务数据仍存在。
6. 进程重启后孤儿任务状态为 `failed`，`error_code=PROCESS_RESTART`。

### 10.3 回归测试

1. 现有同步执行路径不回归。
2. Catalog/Bridge 现有接口不回归。
3. Web 任务页操作可用（查看结果/删除）。

---

## 11. 实施计划（Sprint 9）

依赖说明：

1. Story 11.2 对 Story 11.1 为工程排期依赖（优先收敛 Pipeline 串行链路，再联调异步提交与状态追踪），并非强技术阻塞依赖。

1. 第 1-2 天：`itask_store` + TaskPlugin 表结构与 CRUD。
2. 第 3-4 天：Scheduler 异步队列 + 状态上报 + `THEN` 串行执行。
3. 第 5 天：Web API 代理与前端任务页最小交互。
4. 第 6 天：补齐测试与回归。

---

## 12. 开放问题（已决策）

1. `task_id` 生成策略：**时间序列号** `tsk_{yyyyMMddHHmmss}_{seq}`，seq 启动时从数据库 `MAX(seq)+1` 初始化，之后原子递增，避免跨重启冲突。
2. `request_sql` 是否需要脱敏存储：**存储截断 4KB**，不做脱敏；截断后仅用于展示，不保证可执行。
3. 删除不存在任务时返回 `not_found` 还是幂等成功：**返回 `not_found`**。
4. 内部状态更新走 HTTP 还是接口：**走 ITaskStore 接口**（IQuerier 查找），不走 HTTP。
5. Scheduler 准入校验走 HTTP 还是接口：**走 IOperatorCatalog 接口**（IQuerier 查找），不走 HTTP。
6. Bridge 同步 Catalog 走 HTTP 还是接口：**走 IOperatorCatalog 接口**（IQuerier 查找），不走 HTTP，与 Scheduler 保持一致。
7. Pipeline 数据传递机制：**临时 DataFrame 通道链**，每步创建内存通道，执行后释放；失败时必须清理所有已创建的临时通道。
8. `async` 字段：**保留 `mode` 参数**（`sync`/`async`，默认 `async`）。`sync` 用于简单 SQL 的快速验证，阻塞执行并直接返回结果；`async` 为默认值，适用于生产任务。同步执行不走 worker 队列，直接在请求线程执行。
9. `timeout` 状态触发：**MVP 不触发**，仅保留枚举定义用于前向兼容。
10. upsert 新算子 `active` 初始值：**默认 0**，需用户手动激活。
11. `result_row_count` 语义：**最后一步写入 actual_sink 的行数**，中间步骤不计入。
12. 进程重启孤儿任务处理：**TaskPlugin `Start()` 时清理**，将 `pending/running` 强制置为 `failed`，`error_code=PROCESS_RESTART`。
13. Scheduler worker 并发度：**MVP 单线程**，并发度=1，后续增强项。
14. 前端任务列表轮询间隔：**3 秒**。
15. C++ 算子注册时机：**Scheduler `Start()` 时**通过 `IOperatorCatalog::UpsertBatch()` 注册；运行时动态注册为未来增强项。
