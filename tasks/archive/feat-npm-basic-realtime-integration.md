# Feature: NPM 基础分析生产实时接线

状态：`[x]` 已完成
优先级：P0
前置 Feature：`npm-basic-analysis`、`stream-time-drive`、`npm-capture-contract`（前两项已完成）
后续 Feature：`npm-linux-capture-backends`；生产网卡验收需至少一个可用后端

## 业务意图

让使用 `npm.basic` 的生产实时 SQL 在网卡暂时无包、持续繁忙和结果输出背压时，仍按单调时钟产生有界周期快照，并只依据已处理且无积压的采集事实推进事件时间、清理会话和协议事务。任务可正确区分正常 EOF、取消与故障，同时维持任务私有状态和现有结果 Schema。

## Non-Goals

- 不实现网卡采集后端、packet Schema、NPI 识别、会话/协议算法、结果持久化或新 SQL 语法。
- 不把 `BlockTransformTimeEventV1` 扩成含采集事实的结构，不改变已有 V1/V2 ABI 或离线 SQL 行为。
- 不保证操作系统级硬实时延迟，不在同步 consumer 阻塞时并发回调，不从单调 deadline 推算事件水位。
- 不把 source timeout 当成 EOF/空闲，不把异常终止当成正常 Flush，也不以 `observing` 限制内部模块维护。
- 不支持一个 `npm.basic` 任务同时消费多个实时队列/观测域；跨队列安全水位属于后续归并能力。

## 核心数据与接口契约

`npm.basic` 保留现有 V1 provider 供离线兼容，新增同名 `IBlockTransformOperatorV2` provider；Scheduler 按已有“唯一 V2 优先，否则唯一 V1”选择。V2 task 继承 `IBlockTransformTaskV2`，并额外实现下列任务私有事实入口。入口与 `ProcessBlock`、`OnTime`、`Flush` 在同一 runner 串行执行；并发 `Cancel` 仍使用既有幂等取消语义。runner 只向实时 `npm.basic` 任务提供与 source 同一独占 reader 的身份与事实，不能按进程全局查找最新状态。

```cpp
interface IBlockTransformCaptureFactTaskV1 {
    virtual ~IBlockTransformCaptureFactTaskV1() = default;
    virtual int BindCaptureSource(const CaptureQueueIdentityV1& identity) = 0; // Open 前，复制所需字段
    virtual int AcceptCaptureFact(const CaptureProgressV1& fact) = 0;         // Open 后，按 poll 顺序
};
```

实时 Open 前必须验证 source 有 `ICaptureBlockStreamReaderV1`、单一队列、`PacketSchema()`、稳定观测域、可用单调时间驱动及 capture progress/idle/backlog 能力，再调用 `CreateWithTimeCapabilities`。缺任一能力、身份与 SQL 配置的 `source_domains` 冲突、无效版本或重复绑定均明确失败，且不发布半初始化 task。Schema probe 只验证静态能力和 Schema，不消费事实或执行 `OnTime`；execution task 才绑定 reader 的 generation。V2 的 data/managed sink/source binding 保持当前路径，离线 V1 SQL 结果及生命周期不改变。

`AcceptCaptureFact` 仅记录经身份、generation、`fact_sequence` 校验的未消费事实；重复/倒序/跨队列事实失败，不把事实本身当成 packet。data poll 的事实必须在对应 batch 经 source residual、`npm.basic` 及下游处理、output consumer 返回、`ReleaseBlock` 成功后才投递；空 poll 的事实可直接投递。投递后 runner 立即重查 `GetTimeDriveState`；task 有待处理事实时 arm 即时单调 deadline，`OnTime` 读取并消费事实，调用现有 `DriveRealtimeMaintenance`。事实为空、积压为 present/unknown 或进度不前进时仍可做单调周期快照，但不可通知模块事件水位。处理后 disarm 或重新 arm 严格晚于本次 now 的 deadline，避免空转。

`GetTimeDriveState` 聚合待处理事实的即时工作与 `MaintenancePlan().snapshot_deadline_monotonic_ns`；`MaintenancePlan().event_deadline_ns` 保持事件时间，只有新采集事实推进水位时才检查，不得与单调 deadline 直接比较。首次 Open 可用 deadline 0 初始化单调时间原点；后续 wall clock 仅作为 `observed_at_ns`，回拨不影响调度。`OnTime` 结果使用既有输出 Schema、residual filter、下游链路、managed consumer 与同步背压，`observing` 只决定前台投影。

正常有限 source EOF 时，最后 batch 和事实完成后仅一次 `Flush`，结束剩余会话、模块和结果；实时网卡持续运行时 timeout 不触发 Flush。Stop 可按框架正常终结。外部 Cancel、source/transform/consumer 错误只取消并释放，不输出伪造最终态。两个任务使用不同 reader generation、事实序列、预算和结果上下文，不能交叉推进。

## 主链路

1. Scheduler 为单一实时 source 创建独占 reader，绑定 `npm.basic` V2 execution task 并 Open；runner 按 packet batch → 处理/输出/释放 → 事实投递 → 到期 `OnTime` 顺序执行，时间输出走原有链式输出路径。
2. 无包 poll 提供空闲事实或普通 timeout；事实触发事件进度维护，单调 deadline 独立触发快照。持续繁忙时每 batch 后检查 deadline，背压解除后补做已到期维护；EOF 单次 Flush，取消/故障异常清理。

## Feature Tasks

- [x] T0：交付 `npm.basic` V2 与采集事实任务入口的公共契约和测试锚点，使离线 V1 兼容且实时 Open 能按能力明确准入。
- [x] T1：交付 Scheduler/runner 从独占 reader 到任务的同序事实投递，使批次完成、buffer 归还与事件水位推进具备确定顺序和任务隔离。
- [x] T2：交付 `npm.basic` 的单调 deadline 聚合和实时维护接线，使无包、繁忙及背压解除后均能驱动已启用模块和周期结果。
- [x] T3：交付正常 EOF、Stop、取消、错误和多任务的生产 SQL 验收，使终结状态与结果 Schema 在完整链路可复核。

## Feature 验收

- GTest：实时缺失/无效 capture 扩展、identity、progress、idle/backlog 或 V2 时间能力时 Open 失败；同名 V2 优先且旧离线 SQL 仍通过。
- GTest：假时钟与假 reader 覆盖无包/连续 timeout、连续有包、积压 present/unknown、时间回拨、source filter、输出背压和下游链传播；只有完成处理且可前进的事实推进事件时间，snapshot 仍按单调时间运行。
- GTest：`features` 的全部模块都参与最早维护，`observing` 仅影响前台结果；重复/倒序/跨 generation 事实拒绝；两个实时任务状态与输出隔离。
- Scheduler 端到端 SQL：同一个独占采集 reader、`npm.basic` V2、结果 sink 验证 packet/空闲事实、单次 EOF Flush、Cancel/故障无 Flush、source/transform/consumer 失败回收；不以 mock 直接调用内部 runtime 代替此验收。
- 实施时运行 `test_npm_basic`、`test_framework`、`test_scheduler_e2e` 的对应 target/CTest；Feature 收口全量构建、完整 CTest、clang-format 和 diff 检查通过。

## 完成证据

- T0：`test_npm_basic`、`test_builtin`、`test_scheduler_e2e` 定向 CTest 均通过；同名 V1/V2 插件宿主可激活，旧离线 SQL 和实时无 capture 拒绝由端到端测试验证。
- T1：`test_framework`、`test_npm_basic` 定向 CTest 均通过；假 reader 验证 data/idle 事实同序且 buffer 先释放，输出消费失败不投递事实；任务拒绝重复和跨队列/generation 事实。
- T2：`test_npm_basic`、`test_framework` 定向 CTest 均通过；V2 有包事实、积压 present/unknown、无包与 wall clock 回拨后单调周期快照通过；实时 packet 路径保留解码/模块链，事件时间由归还后的事实推进。已有 runtime 测试覆盖全部启用模块、最早维护、背压与任务隔离。
- T3.1：`test_scheduler_e2e` 通过；真实 SQL 假 reader 验证 packet/idle/timeout、周期快照、EOF 最终行、单 stage 和多 stage 时间输出传播、既有结果 Schema、独占 reader/buffer 回收。
- T3.2–T3.3：真实 SQL 验证 source/transform residual、Stop、取消、source/transform/ReleaseBlock 故障和 managed consumer 写入失败后的 reader 回收；不同任务 reader generation、结果实体和数据互不混用。全量 `cmake --build build -j$(nproc)` 成功，完整 CTest 40/40 通过；修改行 clang-format 核对与 `git diff --check` 通过。未提交或推送。
