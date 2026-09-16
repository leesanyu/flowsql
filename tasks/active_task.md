# 即时工作台

事项：流式算子时间驱动 T3 Scheduler V2 接线

关联 Feature Task：`stream-time-drive` / T3

当前 Atomic Slice：T3 交付 V2 优先/V1 回退、时间 task 端到端执行和动态 capability lease 生命周期

状态：已完成

## 业务意图

- 让 Scheduler 对每个 Block Transform stage 优先选择唯一 V2 provider，没有 V2 时回退唯一 V1；V2 歧义、
  遍历错误或非法动态 lease 必须明确失败，不能静默降级。
- 让 V2 Schema probe 仍只执行 `Open → Cancel → ReleaseTask`，execution task 的 data/time 接口进入既有
  runner 或同步 chain；动态插件 lease 必须覆盖 probe、execution 和最终 `ReleaseTask()`。

## Non-Goals

- 不修改公共 Block Transform ABI、C++ 插件 ABI、BinAddon 能力发布或 T1/T2 runner/chain 调度算法。
- 不实现 NPM 模块调度、实时接线、采集事实、水位、持久化或结果查询。
- 不修改 SQL 语法、Filter Planner 公共接口、StreamRuntime、旧 `IStreamOperator::Tick()` 或新增 timer 线程。
- 不顺手修改既有 V1 provider/task 行为；V1 算子继续零迁移执行。
- 不 commit/push，不开始后续 `npm-basic-realtime-integration`。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/specs/feat-stream-time-drive.md`（仅 T3 完整验收后更新状态和完成证据）
- `tasks/product_backlog.md`（仅 Feature 完整验收后更新状态/规格链接）
- `tasks/archive/feat-stream-time-drive.md`（仅 Feature 完整验收后归档规格）
- `src/services/scheduler/scheduler_plugin.h`
- `src/services/scheduler/scheduler_stream_executor.cpp`
- `src/services/scheduler/scheduler_routes.cpp`
- `src/tests/test_scheduler_e2e/test_scheduler_mutation_guard.cpp`

已知工作树中的公共 Block Transform 接口、BinAddon、fixture、`pipeline`、`test_framework`、`test_builtin`、
Backlog 和规格改动来自已完成的规格/T0～T2；本切片保留这些改动，只完成 Scheduler T3 接线与定向测试。

## 验收锚点

- provider 引用明确区分 V1/V2；发现顺序是唯一 V2 优先，否则唯一 V1。同一版本静态/动态候选总数大于一、
  IID 遍历失败或动态 Acquire 成功却返回空 capability/lifetime 时明确失败，且不回退另一版本。
- V2 task config 由 Scheduler 零初始化并填写 `struct_size`、V2 `contract_version` 和三段借用字符串；失败或
  null task 不发布 session。V2 probe 不查询时间；execution 暴露同一 task 的 data/time 两个接口。
- 单 stage V2 把 time task 交给 `BlockTransformPipelineRunner`；多 stage 把 stage 对齐的可空 time task 交给
  `SynchronousBlockTransformChainTask`，仅存在 V2 stage 时启用 chain 时间接口；全 V1 路径保持原行为。
- 静态 V2、动态 V2、V2 优先于 V1、V2 重复冲突、非法 V2 lease、V1 回退、时间输出/Stop、probe/execution
  生命周期和 lease 释放顺序有可执行测试；既有 V1 单/多 stage 测试继续通过。
- T3 完成后运行 `test_framework`、`test_builtin`、`test_scheduler_e2e`、`test_scheduler_mutation_guard`，随后执行
  Feature 级全量构建和 CTest；全部通过才勾选 T3、更新 Backlog 并归档规格。

## 验收命令

```bash
git diff --check HEAD -- src/services/scheduler/scheduler_plugin.h \
  src/services/scheduler/scheduler_stream_executor.cpp src/services/scheduler/scheduler_routes.cpp \
  src/tests/test_scheduler_e2e/test_scheduler_mutation_guard.cpp tasks/active_task.md \
  tasks/archive/feat-stream-time-drive.md tasks/product_backlog.md
git diff --name-only HEAD -- src/services/scheduler/scheduler_plugin.h \
  src/services/scheduler/scheduler_stream_executor.cpp src/services/scheduler/scheduler_routes.cpp \
  src/tests/test_scheduler_e2e/test_scheduler_mutation_guard.cpp tasks/active_task.md \
  tasks/archive/feat-stream-time-drive.md tasks/product_backlog.md
git status --short --untracked-files=all
cmake -B build src
cmake --build build --target test_framework test_builtin test_scheduler_e2e \
  test_scheduler_mutation_guard -j$(nproc)
ctest --test-dir build \
  -R '^(test_framework|test_builtin|test_scheduler_e2e|test_scheduler_mutation_guard)$' \
  --output-on-failure
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## 时间盒

30 分钟。

## 停止条件

- V2 优先/V1 回退、V2 task 时间接线、动态 lease 生命周期及完整 Feature 回归全部通过后，勾选 T3、归档
  Feature、记录完成证据并停止，不开始后续 Feature。
- 若实现必须修改公共 ABI、BinAddon、NPM、CMake 或允许文件外代码，记录明确阻塞依据并停止。

## 完成证据

- Scheduler provider 引用已区分 V1/V2，严格执行唯一 V2 优先、零 V2 时唯一 V1 回退；V2 歧义、遍历失败和
  非法动态 lease 均失败且不降级。
- V2 task 使用零初始化且带结构大小/版本的 config；Schema probe 只执行 Open/Cancel/Release，execution task
  的 data/time 接口接入单 task runner 或 stage 对齐的混合 chain。
- 测试覆盖静态 V2 优先、V1 回退、V2/V1 混合 chain、时间输出/Stop、动态 V2 lease 到 ReleaseTask、重复 V2
  冲突和非法 V2 lease；既有 V1 单/多 stage 与 BinAddon 生命周期测试保持通过。
- 定向构建和四项 CTest 4/4 通过；全量构建通过；完整 CTest 在允许 loopback socket 的环境中 13/13 通过。
- T3 已勾选，规格完成证据已补充，Backlog 已标记完成并将规格归档；未开始后续 Feature，未 commit/push。
