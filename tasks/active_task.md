# 即时工作台

WIP=1；visualization-dataframe / T5 已完成并验收，Feature 已归档。当前无进行中的 Atomic Slice。

- 用户授权：按 T1→T5 顺序实施，每个 Feature Task 完整验收并在规格勾选后才进入下一项。
- 业务意图：交付默认原生产物、使用说明与聚焦验收证据，使用户按文档从 CSV 导入或 SQL 命名输出完成版本化 DataFrame 探索。
- 结果：T5 验收通过，规格已勾选并移入 `tasks/archive/feat-visualization-dataframe.md`，
  `tasks/product_backlog.md` 标记为已完成。
- 验收证据（本地、不入 Git）：`evidence/visualization-dataframe/implementation/t5-acceptance.json`；
  标准 runner summary `evidence/test-environment/20261022T111047Z-125c2367`（integration 18/18）
  与 `20261022T111508Z-b89f2067`（unit 2/2）。
- 本轮修复：`src/tests/test_builtin/CMakeLists.txt` 补链 `services/catalog/dataframe_transfer.cpp`；
  `src/services/visualization/exploration_service.cpp` 在未配置数据面时返回 `DATA_PLANE_UNAVAILABLE`；
  `src/tests/test_visualization/test_dataframe_transfer.py` 改用 `{workflow,...}` 载荷。
- 未验证：Docker/容器数据面、跨主机传输、四库回归、仓库全量 `all` 与镜像构建。
- 未提交：改动保留在工作区，等待用户明确指令后再 commit/push。
