# 即时工作台

关联 Feature Task：NetAdapter 控制台执行方式补齐；规格 [NetAdapter](archive/feat-npm-linux-capture-backends.md)。
当前 Atomic Slice：连续采集源自动异步提示与后端同步请求校验。
状态：已完成；WIP=0。用户确认方案后测试先行实施，验收通过并停止。

## 业务意图

连续采集用户在控制台直接提交后台任务，避免同步等待被误报 service unreachable；API 强制同步立即收到明确错误。

## Non-Goals

不更改 packet 支持范围、流量标签、SQL 示例、任务分类、HTTP 超时或捕获水位；不修改其他前序未提交工作。
不创建真实采集任务，不写用户 MySQL，不自动重启用户服务，不提交/推送，不移动/删除目录。

## 冻结契约与主链路

沿用公共 IBlockStreamChannelDescriptorV1 的 is_finite；只对描述明确为连续的源和既有 Stream 要求异步。
Scheduler /scheduler/sql/classify 和 Task /tasks/sql/analyze 新增 requires_async:boolean；task_kind 不变。
旧分类响应无该字段时按 false 兼容；普通数据库和有限 pcapfile 保留同步。
Task batch execute 显式 mode=sync 且 requires_async 时，创建任务前 HTTP400，error_code=ASYNC_EXECUTION_REQUIRED、sql_index。
提示“连续采集任务不支持同步执行，请使用 mode='async'。”；async 请求正常返回任务 ID。
控制台分析连续源后切为 async，禁用同步并显示“当前 SQL 使用连续采集源，已切换为异步执行。”；提交时重分析保证。

## 允许修改文件

- tasks/active_task.md
- src/services/scheduler/scheduler_plugin.h
- src/services/scheduler/scheduler_stream_executor.cpp
- src/services/scheduler/scheduler_routes.cpp
- src/services/task/task_plugin.h
- src/services/task/task_plugin.cpp
- src/tests/test_task/test_task.cpp
- src/tests/test_scheduler_e2e/test_netadapter_web_e2e.cpp
- src/frontend/src/views/Tasks.vue
- src/frontend/src/utils/taskExecution.js
- src/frontend/src/utils/taskExecution.test.js
- docs/netadapter.md
- build/netadapter-validation/async-*（本地基线、构建/验证证据，不提交）
- build/output/static/**（前端构建验证后复制产物，无服务重启）

基线 /tmp/flowsql-async-baseline.json；工作台前序证据 build/netadapter-validation/async-active-before.md。
每次 patch 后 git diff --name-only 并核查相对基线的改动均落在允许范围。

## 验收命令

```bash
cmake --build build --target flowsql_scheduler flowsql_task test_task test_netadapter_web_e2e test_scheduler_e2e test_scheduler_mutation_guard -j8
build/output/test_task
ctest --test-dir build -R '^(test_netadapter_web_e2e|test_scheduler_e2e|test_scheduler_mutation_guard)$' --output-on-failure
node --test src/frontend/src/utils/*.test.js
npm run build --prefix src/frontend
git diff --check
```

先新增断言并验证旧实现失败：真实 NetAdapter 分类要求异步，有限描述不要求，Task sync 创建零任务，async 绑定 ID。
变更 C++ 按 src/.clang-format；target 构建通过后执行针对性与既有回归，不无故扩大测试。

## 时间盒与停止条件

2026-10-05 07:34UTC 起 30 分钟，08:04UTC 截止。
目标实现、相关测试/前端构建和 diff 审查完成后 WIP=0 并停止；未完成保留可验证检查点，不自动扩大切片。

## 完成证据

- 2026-10-05 07:50UTC 完成；本 Atomic Slice 验收后停止，不启动后续任务。
- 先验证旧实现失败：Task analyze 缺 requires_async；真实 Scheduler 分类缺 requires_async；前端缺执行策略模块。
  证据 async-task-before.log、async-netadapter-before.log、async-frontend-before.log。
- Scheduler 使用 descriptor.is_finite 判断连续源，Task classify/analyze 透传及汇总 requires_async。
  控制台自动 async、禁用同步并显示中文提示；API sync 在创建任务前返回 ASYNC_EXECUTION_REQUIRED 和 sql_index。
- CMake 所列 6 个 target 构建通过，无编译错误/警告；test_task 全部断言通过。
- 相关 CTest 3/3 通过：test_scheduler_e2e、test_scheduler_mutation_guard、test_netadapter_web_e2e，24.08 秒。
  验证真实 NetAdapter、其他连续描述源、有限描述源及无 descriptor 的旧 BlockStream。
- 前端 4 个测试文件通过；新增执行策略 3/3 用例通过。npm run build 通过；现有 bundle 大小提示不属于本片修改范围。
  dist 已复制到 build/output/static，index.html 字节一致；未重启用户现有服务，后端需重启后加载新插件。
- clang-format-18 对本轮修改行复核通过，git diff --check 通过；前序文件基线哈希核查通过，无越界修改。
- 日志均位于 build/netadapter-validation/async-*；未进行真实 eth0 采集或用户 MySQL 写入，代码未提交。
