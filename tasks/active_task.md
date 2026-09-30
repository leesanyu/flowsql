# 即时工作台

事项：NPM DNS 事务分析（npm-dns-analysis）T4。
关联 Feature Task：T4，交付生产目录接入、组合回归与使用说明，使离线用户可显式启用 DNS 并观察结果，既有 Basic/Session 行为保持稳定。
当前 Atomic Slice：T4 使用说明与 Feature 全量验收；WIP=0，状态：已完成；T4 与 Feature 整体已完成。

## 业务意图

说明离线 SQL 显式启用 DNS、观察前台和查询托管结果的方法，并用全量构建、完整 CTest 与 Sanitizer 验证最终交付。

## Non-Goals

- 不新增 DNS 协议能力、存储后端或实时采集入口；不更改现有插件 ABI。
- 不提交或推送。

## 冻结边界

- README 增加准确的 DNS 配置、标签依赖、观察与托管关系说明。
- DNS 定向测试在 ASan/UBSan 构建下覆盖 UDP、TCP、预算、终结路径；运行标准全量构建和完整 CTest。
- 仅修复本 Feature 验收阻塞，完成后勾选 T4、归档规格、更新 Backlog 和工作台。

## 允许修改文件

- tasks/active_task.md
- tasks/specs/feat-npm-dns-analysis.md
- tasks/archive/feat-npm-dns-analysis.md
- tasks/product_backlog.md
- README.md
- .gitignore
- src/tests/test_npm_basic/CMakeLists.txt
- src/tests/test_npm_basic/test_npm_dns_udp.cpp
- src/tests/test_npm_basic/test_npm_dns_tcp.cpp
- src/tests/test_npm_basic/test_npm_dns_result_encoder.cpp
- src/tests/test_npm_basic/test_npm_dns_module.cpp
- src/operators/npm_basic/modules/dns/npm_dns_udp.cpp
- src/operators/npm_basic/modules/dns/npm_dns_tcp.cpp
- src/operators/npm_basic/modules/dns/npm_dns_result_encoder.cpp
- src/operators/npm_basic/modules/dns/npm_dns_module.cpp

## 验收命令

```bash
cmake -B build src
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
cmake -B build-npm-dns-sanitizer src -DFLOWSQL_NPM_DNS_SANITIZERS=ON
cmake --build build-npm-dns-sanitizer --target test_npm_dns_udp test_npm_dns_tcp test_npm_dns_result_encoder test_npm_dns_module -j$(nproc)
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-npm-dns-sanitizer -R '^test_npm_dns_(udp|tcp|result_encoder|module)$' --output-on-failure
clang-format-18 --dry-run --Werror src/operators/npm_basic/core/npm_module_catalog.cpp src/operators/npm_basic/modules/dns/*.cpp src/operators/npm_basic/modules/dns/*.h src/tests/test_npm_basic/test_npm_dns_*.cpp src/tests/test_npm_basic/test_npm_basic.cpp src/tests/test_npm_basic/test_npm_result_sqlite.cpp
git diff --check
git diff --name-only
git ls-files --others --exclude-standard
```

## 时间盒与停止条件

- 2026-10-01 00:21（Asia/Shanghai）起，每个 10～30 分钟检查点继续同一 T4，直至 A11/A12 全部满足并归档；用户已明确要求中间不要停。

## 检查点

- 2026-09-30 23:48～2026-10-01 00:05：生产目录注册 DNS，真实 runtime 测试改用生产目录；`test_npm_basic` 通过。
- 2026-10-01 00:06～00:20：SQLite DNS v1 三关系、两个 run_id、Scheduler SQL/DataFrame 三关系读回与旧 SQL 组合通过；三项定向 CTest 3/3，改动行格式和 diff 检查通过。
- 2026-10-01 00:21～00:34：README 增加 DNS 显式启用、标签快照、前台观察和托管关系查询说明；标准配置与全量构建通过，完整 CTest 26/26。独立 ASan/UBSan 四项 DNS 定向测试 4/4；本机 ptrace 下 LeakSanitizer 无法启动，设置 `ASAN_OPTIONS=detect_leaks=0` 保留 ASan 内存访问检查与 UBSan。全部改动 C++ 格式、Scheduler 改动行格式、`git diff --check` 通过；P0/P1 diff 审查未见阻塞。T4 已勾选、规格已归档、Backlog 已完成；未提交或推送。
