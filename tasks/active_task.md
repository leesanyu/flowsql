# 即时工作台

关联 Feature Task：[NPM 运行时检视修复](archive/feat-npm-runtime-review-fixes.md) T4 与 Feature 整体验收。
当前 Atomic Slice：恢复中断的标签预算修复与整体验收；状态：已完成，WIP=0。

## 业务意图与 Non-Goals

完成用户已授权的四项修复，核对中断点并补齐验收证据。不实施其他协议/结构优化，不提交/推送。

## 冻结契约

内部 Create / CreateWithTimeCapabilities 末尾的 matcher_reserved_bytes 默认为 0；非零时调用者转移同一 budget 中实际预留的 matcher 额度。MatcherReleaser 释放 matcher 后归还自身额度一次。未预留的 mock 不误释放其他模块预算；外部 Arrow 输出保有其预算生命周期。T1/T2/T3 的离线截止、紧凑累积和周期门槛契约见归档。

## 允许修改文件

- tasks/active_task.md
- tasks/product_backlog.md（仅本 Feature 状态与归档链接）
- tasks/specs/feat-npm-runtime-review-fixes.md → tasks/archive/feat-npm-runtime-review-fixes.md（仅移动文件）
- src/operators/npm_basic/core/npm_basic_task_runtime.h
- src/operators/npm_basic/core/npm_basic_task_runtime.cpp
- src/operators/npm_basic/npm_basic_operator.cpp
- src/tests/test_npm_basic/test_npm_basic.cpp
- build/**（构建及验收证据，现有运行配置保持）
- /tmp/flowsql-npm-fixes/**（独立 Sanitizer 构建，复用主仓第三方缓存）

T1/T2/T3 已完成源码和用户原有 Backlog 修改保留；恢复后未新增源码修改。

## 验收命令

```bash
cmake --build build -j8
ctest --test-dir build --output-on-failure
cmake -B /tmp/flowsql-npm-fixes/asan src -DFLOWSQL_NPM_PERIODIC_SANITIZERS=ON -DFLOWSQL_FLOW_LABELING=ON
cmake --build /tmp/flowsql-npm-fixes/asan --target test_npm_basic test_npm_periodic_contract test_npm_periodic_stats -j8
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir /tmp/flowsql-npm-fixes/asan -R '^(test_npm_basic|test_npm_periodic_runtime|test_npm_tcp_stream_runtime|test_npm_periodic_contract|test_npm_periodic_stats)$' --output-on-failure
git diff --check
```

## 时间盒与停止条件

沿用 T4，不新增 Feature Task。05:53 UTC 开始恢复核查；因恢复后的大小写路径沙箱映射只读，权限等待至 06:27 UTC 后恢复写入和完整验收。实际实现反馈时间盒 06:27–06:57 UTC，30 分钟；验收完成后 WIP=0，停止，不开始其余优化。

## 完成证据

- 四项修复及关键边界断言全部通过，全仓构建零 warning/error，完整 CTest 49/49，ASan+UBSan 5/5。
- 中断前的真实 DPDK 路径异常在恢复后的 GDB 与完整 CTest 中未复现；MySQL/PostgreSQL/ClickHouse 集成当前均通过，未修改无关插件或测试来规避失败。
- 三万会话四场景成功交付，pending 峰值 7,421,952 / 7,290,816 字节；准确 matcher 预算及外部 Arrow 生命周期断言通过。
- 修改行格式零替换、版权和 diff 检查通过，用户原有 Backlog 内容与运行配置保留，未提交/推送。
- 本轮新证据：build/npm-review-evidence；归档包含各项验收结果及历史证据边界。

完成时间：2026-10-07 06:36 UTC。
