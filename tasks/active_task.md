# 即时工作台

事项：实施 `npm-protocol-analysis` T4 兼容回归与组合验收
关联 Feature Task：`tasks/archive/feat-npm-protocol-analysis.md` T4。
当前 Atomic Slice：补齐生产 runtime 组合/隔离锚点，执行 Sanitizer、全量构建、完整 CTest 与 Feature 归档。
状态：已完成；WIP=1，停止于 T4 边界。

## 业务意图

- 证明 T0～T3 交付的协议运行时可以供后续协议 Feature 直接复用，且不会破坏既有 Basic/Session SQL、
  生命周期、预算、标签和插件装载行为。
- 证明多个测试实体、control 输入、并发任务和任务销毁后的 Arrow owner 都遵守已冻结契约。
- 用户明确授权 T4 直至完成；T4 验收通过后完成并归档整个 `npm-protocol-analysis` Feature。

## Non-Goals

- 不实现 DNS、HTTP/1、TLS、ICMP 等具体协议解析，不实现 TCP 重组、结果数据库或生产实时采集接线。
- 不新增生产模块目录项、公共测试 SQL 开关、框架 ABI 或新 Feature Task，不修改后续 Feature 规格。
- 不以机器相关的吞吐/RSS 数值作为通过阈值，不 commit/push。
- 非当前 Feature 的非阻塞失败只记录复现证据；仅处理由本 Feature Diff 引入的 P0/P1 回归。

## 冻结接口与测试契约

- 生产目录仍只登记 `basic`、`session`；测试/未交付协议实体只能通过测试目录注入。
- 两种测试实体与 control 输入通过生产 `NpmBasicTaskRuntime` 的 Open、分发、统一 router、EOF 主链组合验收；
  前台只输出 observing，附加 consumer 收到全部 enabled 实体。
- 并发任务拥有不同 run_id、模块状态、consumer、预算和输出；取消/失败/销毁不跨任务传播。
- runtime 销毁后已交付的 RecordBatch、提取 Array 或切片继续有效，最后 owner 释放后预算归零。
- 既有 SQL/E2E 和完整 CTest 必须通过；定向 AddressSanitizer/UndefinedBehaviorSanitizer 覆盖协议 runtime 测试。
- 完成时检查生产目录、动态依赖、格式、Diff 范围及未交付协议不可用证据；无条件扩大到后续 Feature。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/product_backlog.md`
- `tasks/specs/feat-npm-protocol-analysis.md`
- `tasks/archive/feat-npm-protocol-analysis.md`
- `src/operators/npm_basic/CMakeLists.txt`
- `src/operators/npm_basic/npm_basic_operator.cpp`
- `src/operators/npm_basic/core/npm_basic_task_runtime.h`
- `src/operators/npm_basic/core/npm_basic_task_runtime.cpp`
- `src/operators/npm_basic/core/npm_eof_flusher.cpp`
- `src/operators/npm_basic/core/npm_module_catalog.h`
- `src/operators/npm_basic/core/npm_module_catalog.cpp`
- `src/operators/npm_basic/output/npm_result_router.h`
- `src/operators/npm_basic/output/npm_result_router.cpp`
- `src/operators/npm_basic/output/npm_basic_result_collector.h`
- `src/operators/npm_basic/output/npm_basic_result_collector.cpp`
- `src/operators/npm_basic/output/npm_basic_result_encoder.h`
- `src/operators/npm_basic/output/npm_basic_result_encoder.cpp`
- `src/operators/npm_basic/output/npm_session_result_encoder.h`
- `src/operators/npm_basic/output/npm_session_result_encoder.cpp`
- `src/tests/test_npm_basic/CMakeLists.txt`
- `src/tests/test_npm_basic/test_npm_basic.cpp`
- `src/tests/test_npm_basic/test_npm_protocol_contract.cpp`

现有 T0～T3 未提交差异均属于本 Feature，T4 完成时统一审查；新增实现优先限制在测试文件。
每次 patch 后检查 `git diff --name-only` 与未跟踪文件，所有差异必须在本清单内。

## 验收命令

```bash
cmake -B build src -DFLOWSQL_FLOW_LABELING=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure

# 在独立 build 目录定向启用 ASan/UBSan；只构建并运行协议 runtime 两个测试 target。
cmake -B build-npm-protocol-sanitizer src -DFLOWSQL_FLOW_LABELING=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-npm-protocol-sanitizer --target test_npm_basic test_npm_protocol_contract -j8
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-npm-protocol-sanitizer \
  -R '^(test_npm_basic|test_npm_protocol_contract)$' --output-on-failure

# 本 Feature 修改的 C++ 文件执行 clang-format-18 --dry-run --Werror；既有大文件检查修改区域。
readelf -d build/output/libflowsql_npm_basic.so
git diff --check
git diff --name-only
git status --short --untracked-files=all
```

## 时间盒与停止条件

- 第一时间盒：30 分钟；开始于 2026-09-23 20:05（Asia/Shanghai）。
- T4 全部锚点、定向 Sanitizer、全量构建、完整 CTest、格式与 Diff 审查通过后，勾选 T4，归档规格，
  Backlog 标记完成并停止；不开始后续 Feature。
- 到期未完成时沿用 T4，记录“进行中且检查点通过”或明确错误；不增加任务层级或扩大允许文件。

## 完成证据

- 生产目录仅登记 `basic`、`session`；`dns`、`http1`、`tls`、`icmp` 在 Open 阶段拒绝且不发布半成品。
- 双实体 control 组合测试通过生产 runtime 验证前台 observing、全实体 consumer、并发 `run_id`/状态/预算隔离、
  EOF 恰好一次，以及 runtime 销毁后的 Arrow owner 与 `kPendingOutput` 租约生命周期。
- 独立 ASan/UBSan 定向 CTest 2/2 通过；期间修复 IPv4 TCP、IPv6 UDP 测试构造器对空 payload 执行
  `memcpy` 的未定义行为。
- `FLOWSQL_FLOW_LABELING=ON` 配置和全量构建成功；允许 loopback socket 的最终完整 CTest 同一轮 17/17 通过
  （65.16 秒）。
- 15 个变更 C++ 文件通过 `clang-format-18 --dry-run --Werror`；`readelf` 确认 NPM Basic 没有 DPDK
  NEEDED 项；`git diff --check` 与允许文件范围检查通过，额外构建元数据已清理。
- T4 和整个 Feature 已完成，规格归档、Backlog 标记完成；未启动后续 Feature，未 commit/push。
