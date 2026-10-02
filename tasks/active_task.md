# 即时工作台

事项：NPM ICMP 控制消息分析（npm-icmp-analysis）已完成。
关联 Feature Task：T4 已完成；规格见 [归档](archive/feat-npm-icmp-analysis.md)。
当前 Atomic Slice：T4.7 已完成；WIP=0。

## 业务意图

对补齐后的 A1～A10 再做定向 Sanitizer、标准全量构建与完整 CTest；证据齐全后归档规格并标记 Feature 完成。

## Non-Goals

- 不增加协议范围、数据库目标或新运行时接口，不提交或推送。

## 允许修改文件

- tasks/active_task.md
- tasks/specs/feat-npm-icmp-analysis.md
- tasks/archive/feat-npm-icmp-analysis.md
- tasks/product_backlog.md

## 验收命令

```bash
cmake -B build src -DFLOWSQL_NPM_ICMP_SANITIZERS=ON
cmake --build build --target test_npm_icmp_parser test_npm_icmp_echo test_npm_basic -j$(nproc)
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build -R '^test_npm_icmp_(parser|echo)$|^test_npm_basic$' --output-on-failure
cmake -B build src -DFLOWSQL_NPM_ICMP_SANITIZERS=OFF
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
clang-format-18 --dry-run --Werror src/operators/npm_basic/modules/icmp/*.h src/operators/npm_basic/modules/icmp/*.cpp src/tests/test_npm_basic/test_npm_icmp_*.cpp src/operators/npm_basic/core/npm_basic_task_runtime.cpp src/operators/npm_basic/core/npm_module_catalog.cpp src/tests/test_npm_basic/test_npm_basic.cpp src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp
git diff --name-only
git status --short
git diff --check
```

## 时间盒与停止条件

- 2026-10-02 12:35（Asia/Shanghai）起 10～30 分钟；最终验证和归档完成即停。

## 检查点

- T0～T3 完成；T4.1 生产目录/test_npm_basic 通过；T4.2 真实 Scheduler/test_scheduler_e2e 通过。
- T4.3 README 已补使用说明；标准全量构建、完整 CTest 40/40、格式与 diff 检查通过。
- T4.4 ICMP 解析、关联及含预算模块运行时以 ASan/UBSan 构建；`ASAN_OPTIONS=detect_leaks=0` 定向 CTest 3/3 通过。审查发现 A3 的混合输入跨批次等价断言缺口，移至 T4.5；泄漏检测未启用。
- T4.5 混合 TCP/UDP/control 同批与跨 RecordBatch 结果完全相同；前台 Schema、时延、域及 control 不增端口会话通过。恢复普通构建后完整 CTest 40/40、格式及 diff 检查通过。
- T4.6 已补 IPv4/IPv6 差错类型与引用边界、回显键隔离、TCP/UDP 活动会话/结束、namespace 隔离、emitter/预算失败释放断言；定向 CTest 3/3、格式与 diff 检查通过。
- T4.7 最终复验：ASan/UBSan 定向 CTest 3/3（`ASAN_OPTIONS=detect_leaks=0`）；恢复普通配置后标准全量构建与完整 CTest 40/40；全部改动 C++ 的 clang-format-18、`git diff --check` 通过。T4 勾选、规格归档、需求池标记完成；未提交或推送。
