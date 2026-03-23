# Sprint 9 规划

## Sprint 信息

- **Sprint 周期**：Sprint 9
- **开始日期**：2026-03-21
- **结束日期**：待定
- **预计工作量**：10-12 天
- **Sprint 目标**：完成 Epic 10（算子目录与状态下沉 CatalogPlugin）并启动 Epic 11（Pipeline 增强与异步任务）的核心能力落地

---

## Sprint 目标

### 主要目标

1. **状态收敛**：将算子目录与激活状态统一下沉到 `CatalogPlugin`，消除 Web/Bridge/Scheduler 状态分裂
2. **路径切换**：统一算子查询入口为 `/operators/query`，移除 `/operators/native/query`
3. **职责清理**：Web 退出算子状态持久化职责，Bridge 负责 Python 算子目录回灌 Catalog
4. **能力启动**：交付 Epic 11 的第一版可运行能力（多算子 Pipeline + 异步任务基础框架）

### 成功标准

- [ ] Catalog 成为算子目录与状态唯一真相源（list/detail/activate/deactivate/update/upsert）
- [ ] Bridge `Start/Refresh` 后可批量同步 Python 算子到 Catalog，且不覆盖 `active`
- [ ] Scheduler 执行前状态校验全部走 Catalog
- [ ] `/operators/query` 成为唯一统一查询入口，旧路径与调用引用清零
- [ ] Web `/api/operators*` 仅做代理，不再读写本地 `operators` 表
- [ ] Epic 10 关键回归与契约测试通过，且无 blocker 级缺陷
- [ ] Epic 11 至少完成可演示的 MVP 闭环（仅 Story 11.1/11.2）

---

## 设计文档

- [Sprint 9 统一设计文档](design.md)

---

## Story 列表

### Epic 10：算子目录与状态下沉 CatalogPlugin（架构收敛）

### Story 10.1：CatalogPlugin 成为算子目录唯一来源

**优先级**：P0
**工作量估算**：2 天
**依赖**：无
**状态**：🚧 准备开始

**验收标准**：
- [ ] Catalog 提供统一目录接口（列表/详情/激活/去激活/更新/upsert_batch）
- [ ] `operator_catalog` 存储结构与唯一键生效
- [ ] `IOperatorCatalog` 进程内接口实现（`QueryStatus` + `UpsertBatch`）
- [ ] 以上接口均有自动化单元测试并通过

### Story 10.2：BridgePlugin 同步 Python 算子到 Catalog

**优先级**：P0
**工作量估算**：1.5 天
**依赖**：Story 10.1
**状态**：✅ 已完成

**验收标准**：
- [x] Bridge 在 `Start/Refresh` 后通过 `IOperatorCatalog` 接口（IQuerier，不走 HTTP）执行 `UpsertBatch`
- [x] upsert 不覆盖用户状态字段（`active`）
- [x] 正确同步算子静态元信息（name/type/source/description/position）
- [x] 以上行为有自动化测试并通过

### Story 10.3：Web/Scheduler 读写路径切换到 Catalog

**优先级**：P0
**工作量估算**：2.5 天
**依赖**：Story 10.1, Story 10.2
**状态**：✅ 已完成

**验收标准**：
- [x] Web `/api/operators`、`/api/operators/detail`、激活/去激活接口转发 Catalog
- [x] Web 不再读写本地 `operators` 表，旧表及相关 SQL 已删除
- [x] Scheduler `Start()` 时通过 `IOperatorCatalog::UpsertBatch()` 注册 C++ 算子
- [x] Scheduler 准入校验仅认 Catalog `QueryStatus()`，`kNotFound`/`kDeactivated` 分别返回对应错误
- [x] 算子去激活仅阻止新任务，已 `running` 任务不中断并可执行到终态
- [x] `/operators/query` 由 Catalog 提供统一查询入口（C++ + Python 均可见）
- [x] 删除 `/operators/native/query` 及调用方引用
- [x] 端到端操作链路有自动化测试并通过

### Epic 11：Pipeline 增强与异步任务

### Story 11.1：多算子 Pipeline（MVP）

**优先级**：P1
**工作量估算**：3 天
**依赖**：无
**状态**：📋 待开始

**验收标准（Sprint 9 最小闭环）**：
- [ ] 支持 `USING op1 THEN op2` 基础串行链路
- [ ] 支持算子间数据传递（临时 DataFrame 通道链）
- [ ] 任意步骤失败时，所有临时通道被释放（无资源泄漏）
- [ ] `result_row_count` 记录最后一步写入 sink 的行数
- [ ] 至少 2 条端到端自动化用例通过（成功链路 + 失败链路含资源清理验证）

### Story 11.2：异步任务执行（MVP）

**优先级**：P1
**工作量估算**：3 天
**依赖**：Story 11.1（工程排期依赖：先落地 Pipeline 串行语法与执行链路，再联调异步任务）
**状态**：📋 待开始

**验收标准（Sprint 9 最小闭环）**：
- [ ] 提供任务状态模型（`pending/running/completed/failed/cancelled/timeout`）
- [ ] TaskPlugin 加载顺序在 SchedulerPlugin 之前（服务配置文件已更新）
- [ ] TaskPlugin `Start()` 时清理孤儿任务（`pending/running` -> `failed`，`error_code=PROCESS_RESTART`）
- [ ] 提供任务提交与轮询查询接口（最小可用，不依赖 WebSocket）
- [ ] Web 任务列表支持状态列与”查看结果/删除”操作（前端轮询间隔 3s）
- [ ] “查看结果”语义：`failed` 展示错误信息；`completed` 展示结果摘要（最后一步写入 sink 的行数）
- [ ] 历史任务支持删除（删除任务元数据、摘要与错误信息，不影响已写入业务数据）
- [ ] 不持久化完整执行结果数据集，仅持久化摘要与错误信息
- [ ] 至少 1 条异步执行链路自动化用例通过
- [ ] 孤儿任务清理有自动化单元测试并通过

### Epic 11 增强项（不在 Sprint 9 范围）

1. 多算子并行阶段执行与汇聚
2. 算子错误处理策略（终止/跳过/重试）
3. 任务取消、超时控制、WebSocket 实时推送
4. 任务日志与诊断信息查询

---

## 风险与缓解

前提声明（构件阶段）：本 Sprint 不做旧 `operators` 表历史状态兼容迁移，切换后以 Catalog 当前状态为准。

| 风险 | 可能性 | 缓解措施 |
|------|--------|---------|
| 直接切换导致算子状态缺失 | 中 | 切换前执行 Bridge 全量回灌并做一致性校验 |
| Catalog 成为单点后影响查询可用性 | 中 | 增加健康检查、失败注入测试与超时策略 |
| Epic 10 与 Epic 11 并行导致资源冲突 | 中 | 以 Epic 10 为主线，Epic 11 仅交付最小闭环 |
| 新旧接口混用造成行为不一致 | 低 | 统一路由审计，禁止新代码引入 `/operators/native/query` |

---

## 测试计划（初版）

1. 以 `design.md` 中 Part A（Epic 10）第 9 章作为主测试基线
2. 补齐 `/operators/query` 契约测试与大规模性能回归（1k/5k 算子）
3. 为 Story 11.1/11.2 分别新增最小 e2e 用例，纳入 CI 冒烟集
4. 增加任务状态机与“查看结果”语义的接口契约测试

---

## 实施顺序建议

1. 先完成 Story 10.1（Catalog 基础能力）
2. 再完成 Story 10.2（Bridge 同步）
3. 再完成 Story 10.3（Web/Scheduler 切换 + 清理）
4. Epic 10 稳定后，推进 Story 11.1 与 11.2 最小闭环
