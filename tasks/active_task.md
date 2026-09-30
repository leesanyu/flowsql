# 即时工作台

事项：NPM 共享有界 TCP 字节流（`npm-shared-tcp-stream`）T4 实施。
关联 Feature Task：T4 — 交付端到端回归与接入说明，证明批次边界、失败/取消和既有 Basic/Session 使用方式不破坏共享流保证。
当前 Atomic Slice：T4 A11 跨 RecordBatch 类型化结果与 owner 回收、A12 完整回归和接入说明。
状态：已完成；WIP=0。

## 业务意图

用同一组捕获事实的不同 RecordBatch 切分驱动真实 NPM runtime，验证共享流事件与类型化结果一致；完成全量回归并说明后续协议模块的内部接入方式和现有离线用户边界。

## Non-Goals

- 不新增 DNS/HTTP/TLS 生产模块、SQL 结果实体、第二套会话链或实时采集入口。
- 不重写 T0～T3 的流核心、预算或运行时机制；仅在本轮测试暴露 P0/P1 阻塞问题时修复。
- 不修改无关模块，不提交或推送。

## 冻结契约与测试

- 保留 `INpmProtocolModuleV1` 虚表及 `INpmTcpStreamConsumerV1` 数据契约；测试模块只通过本地 catalog 注入，生产目录仍仅 Basic/Session。
- A11 测试模块按 Data/Gap/End 发射带身份、revision、时间、区间及字节的类型化 Arrow 实体，`NpmResultRouter` 前台和结果消费者均接收有效行。
- 相同报文按单批次和跨批次送入时，按事件字段规范化后序列一致；不同 `observing` 只改变前台实体，结果消费者看到全部启用实体；输入批次 owner 在流仍存活时可释放。
- 既有失败/取消、Basic/Session 和流核心锚点由定向与完整 CTest 覆盖；相关运行时和流目标通过 ASan/UBSan。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/specs/feat-npm-shared-tcp-stream.md`（完成后移入 `tasks/archive/`）
- `tasks/archive/feat-npm-shared-tcp-stream.md`（归档目标）
- `tasks/product_backlog.md`（仅 Feature 状态与规格链接）
- `src/tests/test_npm_basic/test_npm_basic.cpp`
- `README.md`
- `docs/npm-tcp-stream.md`（新增）

每次 patch 后检查 `git diff --name-only` 与未跟踪文件；T0～T3 原有未提交改动是起点，禁止覆盖或清理。起点提交 `4cb275e`。

## 验收命令

```bash
cmake -B build src
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
cmake --build build --target test_npm_basic test_npm_tcp_stream test_npm_tcp_stream_shared test_npm_protocol_contract -j8
ctest --test-dir build -R '^test_npm_(basic|tcp_stream_runtime|tcp_stream_shared|tcp_stream|protocol_contract)$' --output-on-failure
clang-format-18 --dry-run --Werror src/tests/test_npm_basic/test_npm_basic.cpp
git diff --check
```

相关流核心、共享方向和运行时另用 `FLOWSQL_NPM_TCP_STREAM_SANITIZERS=ON` 的独立构建执行 ASan/UBSan；核对生产 catalog 只含 Basic/Session，并记录外部依赖状态及完整 CTest 结果。

## 时间盒与停止条件

- 2026-09-30 16:25（Asia/Shanghai）开始；每 10～30 分钟记录同一 T4 的检查点，用户要求持续到 T4 完成。
- 每个检查点只能记为“进行中且检查点通过”“当前错误待修复”或“被明确问题阻塞”，不扩文件或任务边界。
- A11/A12、文档、构建/测试/格式/diff 审查全通过后勾选 T4，归档 Feature、更新 Backlog，WIP=0，停止。

## 检查点

- 2026-09-30 16:25（Asia/Shanghai）：T4 边界已冻结；待添加 A11 断言并执行验收。
- 2026-09-30 16:46（Asia/Shanghai）：T4 已完成。跨 RecordBatch 类型化 Data/Gap/End 结果在单批次和逐包批次一致；stream 与 session 两种 observing 的前台输出符合选择，结果消费者仍收到 stream、Basic、Session；输入 owner 及时释放，EOF/任务预算归零。
- 验收：标准 CMake 配置和全量构建通过；完整 CTest 22/22 通过、0 失败，覆盖 NPM/参数/标签/结果后端/Scheduler；独立 ASan/UBSan/LeakSanitizer 流核心、共享方向和运行时 3/3 通过、0 失败。沙箱内 LeakSanitizer 受 ptrace 限制，在沙箱外重跑通过。格式、版权、diff 检查通过；生产 catalog 仍仅 Basic/Session。
- T4 已勾选，规格移入 `tasks/archive/`，Backlog 标记完成。T0～T3 的未提交改动仍保留；本轮未提交或推送。
