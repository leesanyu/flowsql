# Sprint 9 Review

日期：2026-03-23

## 1. 审查范围
1. Epic 10：算子目录与状态下沉 CatalogPlugin 的架构收敛与回归质量。
2. Epic 11 MVP：Story 11.1（多算子 Pipeline 串行）与 Story 11.2（异步任务执行）的交付完成度。
3. Web/Scheduler/Task 三端接口语义一致性与自动化测试覆盖情况。

## 2. 本轮交付结论
1. Epic 10 主目标完成：算子目录与激活状态已由 Catalog 统一管理，执行前状态校验与查询入口均完成切换。
2. Story 11.1 已完成：`USING op1 THEN op2` 串行链路、算子间数据传递、失败路径资源释放与 `result_row_count` 语义均已落地。
3. Story 11.2 已完成：任务状态模型、孤儿任务清理、提交/查询/删除接口、前端轮询与结果摘要语义已全部落地。
4. Sprint 收尾补齐：补上真实异步 worker 自动化链路、删除任务同步清理 `task_events`、结果接口补齐 `error_code/error_stage/result_target`。

## 3. 关键实现与证据
1. Pipeline 串行语法与 `THEN` 解析已实现：
   - `src/framework/core/sql_parser.cpp`（`USING ... THEN ...` + per-operator `WITH`）。
2. 多算子串行执行与中间 DataFrame 管道已实现：
   - `src/services/scheduler/scheduler_plugin.cpp`（`ExecuteWithOperatorChain`）。
3. Scheduler 返回 `result_row_count`（最终 sink 行数）：
   - `src/services/scheduler/scheduler_plugin.cpp`。
4. 异步任务状态模型与状态持久化已实现：
   - `src/framework/interfaces/itask_store.h`、`src/services/task/task_plugin.cpp`。
5. TaskPlugin 启动时孤儿任务清理（`PROCESS_RESTART`）已实现：
   - `src/services/task/task_plugin.cpp`（`CleanupOrphans`）。
6. 前端任务页已支持 3s 轮询、查看结果、删除：
   - `src/frontend/src/views/Tasks.vue`。
7. 服务配置已保证 TaskPlugin 在 SchedulerPlugin 之前加载：
   - `config/deploy-single.yaml`、`config/deploy-multi.yaml`。

## 4. 自动化测试覆盖评估
1. Story 11.1：已具备端到端用例
   - `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`：
   - T25：双算子串行成功链路。
   - T26：链路第二步失败与资源无泄漏验证。
2. Story 11.2：已具备关键单元/集成用例
   - `src/tests/test_task/test_task.cpp`：
   - `/tasks/submit|list|detail|delete` 路由行为。
   - 进程重启孤儿任务修复为 `failed + PROCESS_RESTART`。
   - 启用 worker 的真实异步执行链路（`pending/running/completed`）。
   - 异步失败链路错误字段校验（`error_code/error_stage`）。

## 5. 问题与风险
1. 文档状态滞后：`planning/backlog` 中 11.1/11.2 曾显示“待开始/待规划”，本次收尾已完成状态回填。
2. 删除语义偏差：历史实现仅删除 `tasks` 主表，收尾已补齐 `task_events` 联动清理。
3. 队列并发能力仍为 MVP 级：当前 worker 并发度为 1，后续增强阶段再升级线程池并发控制。

## 6. 结论
1. Sprint 9 已达到“Epic 10 收敛 + Epic 11 MVP 启动并可运行”的目标。
2. Story 11.1 状态确认为：✅ 已完成。
3. Story 11.2 状态确认为：✅ 已完成（MVP）。
4. 下一阶段可进入 11.3/11.4 增强能力开发。
