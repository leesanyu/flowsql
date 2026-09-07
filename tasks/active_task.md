# Active Task

Feature：`npm-packet-dataframe-view`
原子任务：T5 移除命名 DataFrame 写入响应的全量展示序列化
状态：已完成

## 业务意图

- `SELECT ... INTO dataframe.<name>` 只把原生 Arrow 数据写入命名 DataFrame，不在执行完成阶段调用
  `DataFrame::ToJson()`。
- 执行响应只返回状态、行数和 `result_target`；packet 的 JSON/hex 转换只发生在 Catalog 分页 preview。

## Non-Goals

- 不改变无 `INTO dataframe.*` 查询的即时结果响应，不改变 database/stream sink 语义。
- 不修改 `pcapfile`、DataFrame/Arrow 存储、Catalog serializer、Web 或前端。
- 不改变 packet Schema、raw hex 64 字节截断或 preview 分页契约。
- 不清理其他 Feature 的既有未提交内容，不读取 `tasks/sprints/**`，不执行 commit/push。

## 契约与测试锚点

- 命名 DataFrame 成功响应恰好包含 `status`、`rows`、`result_row_count`、`result_target`，不包含 `data`。
- 修改生产代码前，Scheduler E2E 和 Web E2E 必须因执行响应仍包含 `data` 而红灯。
- 命名 DataFrame 内部仍保持完整 `packet::PacketSchema()`、行序和原始 binary；既有直接读取断言继续通过。
- Web E2E 随后调用 preview，继续验证按页生成 raw hex 及 layer 数字/数组。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/product_backlog.md`
- `tasks/archive/feat-npm-packet-dataframe-view.md`
- `tasks/specs/feat-npm-packet-dataframe-view.md`
- `src/services/scheduler/scheduler_routes.cpp`
- `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`
- `src/tests/test_scheduler_e2e/test_pcap_web_e2e.cpp`

## 验收命令

- 先只增加执行响应契约断言，构建并运行 `test_scheduler_e2e`、`test_pcap_web_e2e` 确认旧实现红灯。
- `cmake --build build --target test_scheduler_e2e test_scheduler_mutation_guard test_pcap_web_e2e -j$(nproc)`。
- `ctest --test-dir build -R '^(test_scheduler_e2e|test_scheduler_mutation_guard|test_pcap_web_e2e)$' --output-on-failure`。
- Feature 收口运行 `ctest --test-dir build --output-on-failure`、`git diff --check`；新增源码行不超过 120 列。

## 验收结果

- 红灯复现：生产修改前，Scheduler E2E 与 Web E2E 均因命名 DataFrame 执行响应有 5 个字段而不是
  4 个字段失败，确认额外字段为全量序列化的 `data`。
- 最小修复：`INTO dataframe.*` 仍读取 Arrow 快照取得行数，但不调用 `DataFrame::ToJson()`，也不在
  执行响应中写入 `data`；无 `INTO dataframe.*` 路径保持原逻辑。
- Scheduler E2E 继续验证完整 `PacketSchema()`、行序和原始 binary；Web E2E 继续验证后续分页 preview
  的 raw hex 与 layer 数字/数组，证明展示序列化仅发生在 preview。
- 相关 3 个 CMake target 编译通过；相关 CTest 3/3 通过（22.94 秒）。
- 完整 CTest 12/12 通过（51.53 秒）；`git diff --check` 和未跟踪 E2E 空白检查通过。
- 环境未安装 `clang-format`；本任务新增源码行无超过 120 列的行。
- 未修改 DataFrame、Catalog、pcapfile、Web 或前端，未清理其他 Feature 的既有未提交内容，
  未执行 commit/push。

## 时间盒与停止条件

- 时间盒：20 分钟。
- 红灯、最小修复、相关回归和完整 CTest 通过后，勾选 T5、重新归档 Feature、更新 backlog 与工作台，
  然后立即停止。
- 若必须改变 DataFrame ABI、Catalog preview 或无 `INTO dataframe.*` 查询响应才能实现，以“当前错误待修复”停止。
