# Feature: 流式历史补算

状态：`[ ]` 待规划
优先级：P3

## 业务意图

对已经落地的数据按事件时间窗口或条件执行批处理补算，并把结果幂等地写回目标通道。补算复用现有 batch 任务链路，避免把历史回放职责带入实时流式数据面。

## Non-Goals

- 不实现流内广播回放、持久化 replay 或跨主机分布式调度。
- 不新增 packet 采集、NPM 解析或基线算法能力。
- 不改变现有实时流任务的消费边界和状态机。

## 公共契约

`RecomputeRequest` 必须包含 `source`、`event_time.start`、`event_time.end`、`target` 和 `idempotency_key`；`predicate` 为可选字段，缺失表示窗口内全量数据。`start < end`，时间边界采用半开区间 `[start, end)`。

`RecomputeResult` 至少包含 `task_id`、`status`、`processed_rows`、`written_rows`、`watermark` 和结构化 `error`。相同 `idempotency_key` 在同一目标范围内必须返回同一任务结果，不能产生重复写入。

控制面能力抽象为 `Submit(RecomputeRequest)` 和 `GetStatus(task_id)`；数据读取、变换和写入走现有 batch 通道，不新增热路径协议。

## 主链路

1. 校验 source、时间窗口、目标和幂等键，登记补算边界并创建任务快照。
2. 批量读取窗口数据，执行既有 Pipeline，按目标的幂等策略写入并更新 watermark。
3. 记录行数、边界和错误信息，任务进入 `completed` 或 `failed` 终态。

## 原子任务

- `[ ]` 冻结请求、结果、边界和幂等语义。
- `[ ]` 实现补算任务提交、执行和状态快照。
- `[ ]` 实现目标写入的去重/覆盖策略与重复提交处理。
- `[ ]` 补齐窗口、空结果、失败重试、重复提交和边界一致性测试。
