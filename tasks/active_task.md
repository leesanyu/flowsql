# 即时工作台

事项：NPM TLS 握手分析（npm-tls-handshake-analysis）T4。
关联 Feature Task：T4，交付生产目录、离线 SQL/托管查询回归与使用说明，使用户可显式启用 TLS 且旧结果保持稳定。
当前 Atomic Slice：T4.5 最终全量验收与归档；WIP=0，状态：已完成。

## 业务意图

复验 TLS 1.2 ALPN 修正后的生产共享库、真实离线 SQL 和完整回归，确认 A10/A11 后归档 Feature。

## Non-Goals

- 不扩展 TLS 协议范围、不新增数据库目标，不提交或推送。

## 冻结边界

- 标准全量构建、完整 CTest、TLS 四个 ASan/UBSan 目标、改动代码格式和 diff 均通过。
- 未启用 TLS 的旧 SQL/实体测试继续通过；真实 Scheduler TLS 结果及双 run_id 三关系可查。
- 证据记录后勾选 T4，Backlog 标完成，规格文件移入 archive，工作台置 WIP=0。

## 允许修改文件

- tasks/active_task.md
- tasks/product_backlog.md
- tasks/specs/feat-npm-tls-handshake-analysis.md
- tasks/archive/feat-npm-tls-handshake-analysis.md

## 验收命令

```bash
cmake -B build src
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
cmake --build build-npm-tls-sanitizer --target test_npm_tls_framer test_npm_tls_handshake test_npm_tls_result_encoder test_npm_tls_module -j$(nproc)
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-npm-tls-sanitizer -R '^test_npm_tls_(framer|handshake|result_encoder|module)$' --output-on-failure
clang-format-18 --dry-run --Werror src/operators/npm_basic/core/npm_module_catalog.cpp src/operators/npm_basic/modules/tls/npm_tls_contract.cpp src/operators/npm_basic/modules/tls/npm_tls_hello.cpp src/operators/npm_basic/modules/tls/npm_tls_framer.cpp src/operators/npm_basic/modules/tls/npm_tls_handshake.cpp src/operators/npm_basic/modules/tls/npm_tls_result_encoder.cpp src/operators/npm_basic/modules/tls/npm_tls_module.cpp src/tests/test_npm_basic/test_npm_basic.cpp src/tests/test_npm_basic/test_npm_tls_contract.cpp src/tests/test_npm_basic/test_npm_tls_hello.cpp src/tests/test_npm_basic/test_npm_tls_framer.cpp src/tests/test_npm_basic/test_npm_tls_handshake.cpp src/tests/test_npm_basic/test_npm_tls_result_encoder.cpp src/tests/test_npm_basic/test_npm_tls_module.cpp src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp
git diff --check
git status --short
```

## 时间盒与停止条件

- 2026-10-01 22:02（Asia/Shanghai）起 10～30 分钟；完整验收通过后归档并置 WIP=0，停止 T4。

## 检查点

- T0～T3 已完成。
- T4.1：生产 catalog 与共享库纳入 TLS，`test_npm_basic` 1/1 通过。
- T4.2：真实 Scheduler 离线 SQL、前台 25 列与双 run_id 三关系查询通过。
- T4.3：README 用法、标准全量构建、完整 CTest 37/37、TLS ASan/UBSan 4/4、格式与 diff 通过。
- T4.4：TLS 1.2 selected ALPN 原始字节转为可逆 JSON 字符串，含 NUL/非 ASCII/引号测试；普通与 Sanitizer 编码器测试各 1/1 通过。
- T4.5：标准 CMake 配置、全量构建、完整 CTest 37/37、TLS ASan/UBSan 4/4、clang-format 和 `git diff --check` 均通过；T4 勾选，Backlog 完成，规格归档。T4 已完成，停止于 Feature 边界；代码未提交或推送。
