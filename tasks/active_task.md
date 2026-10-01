# 即时工作台

事项：NPM HTTP/1 事务分析（npm-http1-analysis）T4。
关联 Feature Task：T4，交付生产目录接入、离线 SQL/托管查询回归与使用说明，使用户可显式启用 HTTP/1 而旧结果保持稳定。
当前 Atomic Slice：T4.3 使用说明与全量验收；WIP=0，状态：已完成。

## 业务意图

让用户按 README 显式启用 HTTP/1，并以完整构建、CTest 和 Sanitizer 关闭 T4 的回归风险。

## Non-Goals

- 不扩展 HTTP 协议范围、改动 V1 ABI 或新增托管数据库实现。
- 不补与 T4 验收无关的重构和测试。
- 不提交或推送。

## 冻结边界

- README 给出 HTTP/1 显式开启、标签快照、前台与托管 SQL、结果解释与边界。
- 标准入口全量构建、完整 CTest、HTTP/1 定向 ASan/UBSan、格式与 diff 检查均通过。
- 证据写入规格；所有锚点通过后归档规格并完成 Backlog。

## 允许修改文件

- tasks/active_task.md
- README.md
- tasks/specs/feat-npm-http1-analysis.md
- tasks/archive/feat-npm-http1-analysis.md
- tasks/product_backlog.md

## 验收命令

```bash
cmake -B build src
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
cmake -B build-npm-http1-sanitizer src -DFLOWSQL_NPM_HTTP1_SANITIZERS=ON
cmake --build build-npm-http1-sanitizer --target test_npm_http1_framer test_npm_http1_transactions test_npm_http1_result_encoder test_npm_http1_module -j$(nproc)
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-npm-http1-sanitizer -R '^test_npm_http1_(framer|transactions|result_encoder|module)$' --output-on-failure
git diff --check
git diff --name-only
git ls-files --others --exclude-standard
```

## 时间盒与停止条件

- 2026-10-01 15:49（Asia/Shanghai）起 10～30 分钟；A10/A11 全部通过后记录证据、归档并置 WIP=0。

## 检查点

- T4.1：生产 catalog/CMake 接入成功，flowsql_npm_basic 与 test_npm_basic 构建通过，定向 CTest 1/1 通过；旧目录假设已更新。
- T4.2：真实 Scheduler SQL 使用五包 HTTP/1 PCAP、测试标签快照、前台 DataFrame 与两次托管写入，断言 matched/200/100000 ns/观测域 77、Schema v1、history/latest/final 及 run_id 隔离；test_scheduler_e2e 和 test_npm_result_sqlite 定向 CTest 各 1/1 通过，git diff --check 通过。E2E 旧 Basic/Session/DNS 路径保持绿色。
- T4.3：README 补显式启用、托管查询和捕获视图边界；`cmake -B build src`、全量构建、完整 CTest 31/31、HTTP/1 定向 ASan/UBSan 4/4（`ASAN_OPTIONS=detect_leaks=0`）、新增/改动范围 clang-format 与 `git diff --check` 通过。规格 T4 已勾选并归档，Backlog 已标完成；未提交或推送。
