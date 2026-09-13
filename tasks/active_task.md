# Active Task

Feature：NPM 基础分析与模块组合（`npm-basic-analysis`）
原子任务：T3.4.3.1 活动会话与 deadline 索引预算接线
状态：已完成

## 业务意图

- 让活动会话/deadline 使用 runtime 的共享 `kSessionState` 额度；新会话先预留，退役或析构时归还。
- 预算失败不得改变既有会话、deadline、调用方 output 或 session ID 序列。

## Non-Goals

- 不实现 T3.4.3.2/T3.4.3.3 的输入 batch 租约、runtime Process 或正常 EOF 适配。
- 不实现 T3.4.4/T3.4.5 的并发 Cancel、LastError、provider、task adapter、plugin/IID 或生产 `.so`。
- 不计量临时快照、Arrow 输出、模块缓存、source 或进程 RSS；本片只计量活动会话及 deadline 索引。
- 不修改 Packet/Basic Schema、NPI、批次处理、投影、collector、flusher 或框架 ABI。
- 不运行 Feature 全量回归，不 push；仅按用户明确指令提交当前完成切片与既有累计差异。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/specs/feat-npm-basic-analysis.md`
- `src/plugins/npm_basic/npm_session_table.h`
- `src/plugins/npm_basic/npm_session_table.cpp`
- `src/plugins/npm_basic/npm_basic_task_runtime.cpp`
- `src/tests/test_npm_basic/test_npm_basic.cpp`

此前所有累计差异均受保护；尤其不修改 `src/plugins/npi/*`、`npm_analysis_contract.*`、
`npm_basic_task_config.*`、`npm_task_budget.*`、`npm_packet_*`、`npm_protocol_context.*`、
`npm_basic_result_*`、`npm_eof_flusher.*` 与测试 CMake 接线。

## 冻结契约

- `NpmSessionTable(config, budget)` 接受 runtime 共享账本；旧构造路径自建账本，不形成未计量旁路。
- 每个活动会话保存稳定、非零的保守 charge，覆盖会话、deadline key、状态和容器节点；命名空间拥有字节
  计入 charge。
- 新键先预留；超限或后续分配失败保持表、output、ID 和账本不变。已有会话不重复预留，tuple reuse 沿用
  同一 charge。
- closed、idle、EOF 和析构恰好归还；`tracked_bytes()` 用于断言表内 charge 与共享账本一致。

## 验收标准与命令

- 测试锚点：创建/复用/超限原子性，closed/idle/EOF/析构归还，以及 runtime 共享同一账本。
- `cmake --build build --target test_npm_basic -j$(nproc)`
- `ctest --test-dir build -R '^test_npm_basic$' --output-on-failure`
- `git diff --check`
- `git diff --name-only`
- `git status --short --untracked-files=all`

## 时间盒与停止条件

- 时间盒：30 分钟。只完成活动会话/deadline 的任务账本接线，不实现输入或输出预算 owner。
- 验收通过后只勾选 T3.4.3.1；T3.4.3 及全部父任务保持未完成，然后立即停止，不自动进入 T3.4.3.2。
- 若必须修改受保护组件、框架 ABI 或实现 ProcessBlock/Flush 才能完成，记录复现证据并停止，先重新切片。

## 执行前基线

- 基线提交 `62f5b1939aea140b86cec7816ecd3c09daf1314a`；T0～T2、T3.1～T3.3、T3.4.1、
  T3.4.2.1、T3.4.2.2 累计实现均已通过 `test_npm_basic`，尚未提交并必须完整保留。
- 账本已存在，但会话表尚未 Reserve/Release，runtime 也尚未向其注入共享账本。
- provider 位于 T3.4.5，必须等待完整 Process/Flush/Cancel/LastError，不发布临时方法桩。

## 完成证据

- 活动会话与 deadline 使用同一稳定 charge；新会话先预留，closed/idle/EOF/析构归还，tuple reuse 沿用。
- 预算或分配失败不改变会话表、deadline、调用方 output、session ID 或账本；runtime 注入共享账本。
- `cmake --build build --target test_npm_basic -j$(nproc)`：通过。
- `ctest --test-dir build -R '^test_npm_basic$' --output-on-failure`：1/1 通过。
