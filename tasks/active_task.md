# Active Task

Feature：`stage-filter-pipeline`
原子任务：T4.4b 跨模块 E2E、完整回归与 Feature 归档
状态：已完成

## 业务意图

- 在现有 Scheduler E2E 中用真实 pcapfile 插件读取测试 PCAP，经两个测试用
  `IBlockTransformOperatorV1`、逐阶段 WHERE 和命名 DataFrame sink，锚定 parser、provider IID、PacketSchema、
  Arrow residual、Scheduler runtime 与 DataFrame 注册的完整链路。
- 跨模块用例验证 source/stage 过滤后的确定内容、命名结果响应不包含序列化 `data`，以及 probe/执行 task
  exactly-once 释放；旧 pcapfile→终端 block operator E2E 必须保持通过。
- 定向 E2E 通过后执行标准全量构建与完整 CTest；全部绿色后完成规格、backlog 和归档收口。

## Non-Goals

- 不新增或修改生产能力，不实现 npm.basic、pcapfile 领域函数或具体 source 下推。
- 不修改 parser、binder、planner、runner、Scheduler、packet/DataFrame 或插件接口 ABI；若 E2E 暴露生产阻塞，
  以“当前错误待修复”停止并重新拆分任务，不在验收切片内扩大范围。
- 不增加 Web/UI E2E；展示时 hex 序列化已由已完成 Feature 独立覆盖，本切片只断言命名执行响应不序列化数据。
- 不读取 `tasks/sprints/**`，不清理或覆盖累计未提交改动，不执行 commit/push。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/specs/feat-stage-filter-pipeline.md`
- `tasks/archive/feat-stage-filter-pipeline.md`
- `tasks/product_backlog.md`
- `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`

## 验收命令

- 增加真实 pcapfile→两级 transform→命名 DataFrame E2E，覆盖逐级 WHERE、结果内容、响应契约和任务释放。
- `cmake --build build --target test_scheduler_e2e -j$(nproc)`
- `ctest --test-dir build -R '^test_scheduler_e2e$' --output-on-failure`
- `cmake --build build -j$(nproc)`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`
- `git status --short --untracked-files=all`
- `git diff --name-only`

## 时间盒与停止条件

- 时间盒：30 分钟。
- 跨模块 E2E、全量构建、完整 CTest 和 diff 审查全部通过后，勾选 T4/T4.4/T4.4b，将 Feature 在 backlog 标记
  完成并把规格归档，更新工作台后立即停止。
- 任一定向或完整回归失败时，仅根据当前累计 diff 定位；需要修改允许列表外生产文件时，以“当前错误待修复”
  停止，不扩大本切片。

## 完成证据

- 新增真实 pcapfile→两级 transform→命名 DataFrame E2E：source 和两个 operator stage 的 WHERE 均由通用
  Arrow residual 执行，最终只保留 sequence=1、captured_len=4、protocol=`HTTP` 的一行。
- 命名执行响应只有 status/rows/result_row_count/result_target，不包含 `data`；两个 provider 各创建并
  exactly-once 释放一个 probe 和一个执行 task，第二级收到零行 batch 后继续处理下一批，证明零行不是 EOF。
- 兼容回归：旧 pcapfile→终端 block operator 保持通过；T30 改为断言当前 parser 的多 source source-stage
  WHERE 拒绝文案，未修改生产代码。
- 定向构建和 `test_scheduler_e2e` 通过，1/1 Passed，21.15 秒。
- `cmake --build build -j$(nproc)` 全量构建通过；允许本机 loopback 的完整 CTest 12/12 通过，50.55 秒。
- 受限沙箱首次完整 CTest 的两项 socket 测试因监听权限失败；同一二进制在允许 loopback 环境定向 2/2 通过，
  随后完整 12/12 通过，确认不是代码回归。
- `git diff --check` 通过；新增测试源码行均不超过 120 列，环境没有 `clang-format`。
