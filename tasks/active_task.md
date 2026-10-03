# 即时工作台

事项：`npm-basic-periodic-stats` 已完成；用户授权从 T0 到 T4 连续实施。
关联 Feature Task：T0–T4，均已整体验收并勾选。
当前 Atomic Slice：T4 归档关联链接核查；已完成，WIP=0。实现与全量验收已完成。
规格：[归档](archive/feat-npm-basic-periodic-stats.md)。

## 业务意图

交付 PCAP/持续采集共用的事件时间周期统计、单一 Basic 契约、托管查询与后台 Stop/Cancel；完成全量验收并归档。

## Non-Goals

- 不实现真实采集后端；真实网卡联验由 NetAdapter Feature 收口。
- 不改变 Session/协议实体契约，不实现旧数据迁移或兼容层。
- 用户已明确授权本 Feature 本地提交；不推送，不提交独立采集规划，不删除/移动目录，不混入 NPI 生命周期修复。

## 允许修改文件

- src/framework/core/pipeline.cpp
- src/framework/core/pipeline.h
- src/operators/npm_basic/CMakeLists.txt
- src/operators/npm_basic/config/npm_basic_task_config.cpp
- src/operators/npm_basic/config/npm_basic_task_config.h
- src/operators/npm_basic/config/npm_parameters.cpp
- src/operators/npm_basic/config/npm_parameters.h
- src/operators/npm_basic/core/npm_basic_task_runtime.cpp
- src/operators/npm_basic/core/npm_basic_task_runtime.h
- src/operators/npm_basic/core/npm_eof_flusher.cpp
- src/operators/npm_basic/core/npm_module_catalog.cpp
- src/operators/npm_basic/core/npm_packet_processor.cpp
- src/operators/npm_basic/core/npm_session_table.h
- src/operators/npm_basic/modules/basic/npm_basic_result_projector.cpp
- src/operators/npm_basic/npm_analysis_contract.cpp
- src/operators/npm_basic/npm_analysis_contract.h
- src/operators/npm_basic/npm_basic_operator.cpp
- src/operators/npm_basic/npm_protocol_contract.cpp
- src/operators/npm_basic/npm_protocol_contract.h
- src/operators/npm_basic/output/npm_basic_result_collector.cpp
- src/operators/npm_basic/output/npm_basic_result_collector.h
- src/operators/npm_basic/output/npm_basic_result_encoder.cpp
- src/services/scheduler/scheduler_batch_runtime.cpp
- src/services/scheduler/scheduler_batch_runtime.h
- src/services/scheduler/scheduler_routes.cpp
- src/services/scheduler/scheduler_stream_executor.cpp
- src/tests/test_framework/main.cpp
- src/tests/test_npm_basic/CMakeLists.txt
- src/tests/test_npm_basic/benchmark_npm_basic.cpp
- src/tests/test_npm_basic/test_npm_basic.cpp
- src/tests/test_npm_basic/test_npm_protocol_contract.cpp
- src/tests/test_npm_basic/test_npm_result_backends.cpp
- src/tests/test_npm_basic/test_npm_result_sqlite.cpp
- src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp
- tasks/active_task.md
- tasks/product_backlog.md
- src/operators/npm_basic/modules/basic/npm_basic_periodic_stats.cpp
- src/operators/npm_basic/modules/basic/npm_basic_periodic_stats.h
- src/tests/test_npm_basic/test_npm_periodic_contract.cpp
- src/tests/test_npm_basic/test_npm_periodic_stats.cpp
- tasks/specs/feat-npm-basic-periodic-stats.md
- tasks/archive/feat-npm-basic-periodic-stats.md
- tasks/specs/feat-npm-linux-capture-backends.md

## 验收命令

```bash
cmake --build build -j8
ctest --test-dir build --output-on-failure
ctest --test-dir /tmp/flowsql-npm-periodic-asan -R '^test_npm_periodic_(contract|stats|runtime)$' --output-on-failure
build/output/benchmark_npm_basic 4 1
python3 /tmp/npm-periodic-format.py --check
git diff --check
git diff --name-only
git status --short
```

## 时间盒与停止条件

- 归档关联链接切片为 10 分钟，仅修正 NetAdapter 对本 Feature 的引用；三个关联文档的本地链接核查通过，已完成并停止。
- 用户连续授权范围 T0–T4 已收口；规格移入 archive，Backlog 标记 [x]。
- 本地提交仅纳入本 Feature 代码、测试、工作台、归档和对应 Backlog 行；NetAdapter 规格及其他已有规划改动保留在工作区。

## 完成证据

- 全量构建 exit 0；benchmark 与 Scheduler 最后两处测试工具修改分别重新编译对应 target 通过。
- 完整 CTest 沙箱外 43/43 通过，72.72 秒；四后端均实际执行，数据库/HTTP 集成没有跳过。
- 周期契约/统计/runtime 的 ASan、UBSan、LeakSanitizer 3/3 通过，0.40 秒；Sanitizer 独立配置 FLOWSQL_NPM_PERIODIC_SANITIZERS=ON、FLOWSQL_FLOW_LABELING=OFF，复用主仓库依赖缓存。
- Scheduler 验证 PCAP fast/timestamp、batch=1/4 与假实时源等价；后台运行身份与已交付历史可查询，Stop completed、Cancel incomplete，reader/batch 和模块预算归还。
- Basic 固定 32 列/schema_version=1，标签三态；默认 periodic_snapshot/30 秒，显式 final 周期 NULL。UINT64_MAX、周期增量求和和四后端 history/latest/final 断言通过。
- benchmark 4 packets / 1 iteration 的 Basic、Basic+Session 两模式均通过；未设置性能阈值。
- clang-format-18 修改区域检查、git diff --check 和允许文件审查通过。既有 NPM 测试函数未移除；原有其他 Backlog 改动保留，NetAdapter 规格仅维护指向本归档的链接。
- 日志：/tmp/npm-periodic-full-build.log、/tmp/npm-periodic-full-ctest-final.log、/tmp/npm-periodic-asan-runtime-test.log、/tmp/npm-periodic-benchmark-smoke.log。

## 跨任务问题隔离

- 归属：NPI 生命周期。完整 test_npm_basic 的 LeakSanitizer 发现未修改的 ObjectsPool/NetworkLayer 初始化泄漏，1,222,349 bytes / 47 allocations；堆栈经过 TestNpiPipelinePoolOptionAndLeaseContract。
- 复现：/tmp/flowsql-npm-periodic-asan 中的 test_npm_basic；日志 /tmp/npm-periodic-asan-test.log。plugins/npi 与 common/algo/objects_pool.hpp 无本 Feature diff。
- 按跨任务隔离规则记录；周期专属 Sanitizer 通过，普通完整 test_npm_basic 保留执行，未关闭 leak 检查或加入抑制规则。
