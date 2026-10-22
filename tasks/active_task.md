# 即时工作台

WIP=0；当前全部改动已完成审查与验收，进入已授权的本地提交出口；不启动其他任务。

## 意图与边界

- 用户授权：2026-10-23“提交所有代码”，涵盖本轮开始时全部 8 个修改/新增文件，仅本地提交，不推送。
- 业务意图：提交 DataFrame 探索私有共享目录的原生部署修复、对应回归/文档，以及待办 Feature 登记与已完成能力整理。
- 关联任务：已交付 `visualization-dataframe` 的原生部署修复；`dataframe-lazy-loading` 仅规格登记，T1～T3 保持未开始。
- Non-Goals：不实施延迟加载、不修改业务数据、不重启用户服务、不提交 evidence/构建产物/私有配置、不运行全量/外部库/Kafka/Docker。
- 时间盒：30 分钟；2026-10-22 18:44 UTC 开始，19:14 UTC 截止。
- 停止条件：冻结验收通过，8 个明确路径 staged diff 审查完成，创建本地提交并核对工作区后停止。

## 冻结提交及允许文件

- `config/deploy-single.yaml`
- `config/deploy-visualization.yaml`
- `docs/exploration.md`
- `src/tests/test_framework/test_native_deploy_config.cpp`
- `src/tests/test_visualization/test_dataframe_native.py`
- `tasks/active_task.md`
- `tasks/product_backlog.md`
- `tasks/specs/feat-dataframe-lazy-loading.md`

- 开始时暂存区为空；初始 HEAD、状态、8 文件摘要在 `evidence/exploration-config-commit/before.json`，本地不入库。
- 产品配置、说明及规格内容保持审查范围；仅可修复当前测试与实际部署契约不一致的阻塞问题。
- 每次 patch 后检查 `git diff --name-only`，只按上述明确路径暂存。
- 提交标题：`fix(exploration): use POSIX shared storage and organize feature backlog`。

## 冻结契约与验收

- 原生 single / standalone visualization 的 DataFrame 共享目录使用 `/var/tmp/flowsql/exploration-shared`，两端一致，保持目录 0700 / 对象 0600。
- Guardian 原配置未接入探索共享目录，目录测试应分别核对其既有 Catalog option，不能把 single-only option 强加给 Guardian。
- 构建：`cmake --build build --target test_native_deploy_config -j2`（使用主 build，标准 src CMake 入口）。
- 单测：`tools/test.sh unit --filter '^test_native_deploy_config$' --jobs 1`，验证 single/Guardian 配置及默认运行产物；先确认既有测试失败证据再修正。
- 集成：`tools/test.sh integration --location native --filter '^test_visualization_dataframe_native$' --jobs 1`，验证修改的配置断言、CSV/SQL 输出、快照及源进程生命周期。
- 依赖：仓库根目录、本地构建产物与隔离临时 SQLite/CSV/0700 目录、随机 loopback 端口；无外部服务/凭证。沿用 dependencies.json 的 native 声明/资源锁及 environment.json。
- 检查 C++ 改动行 clang-format、Python ast、需求池/规格链接与未实施状态、`git diff --check`。
- 核对两次 runner 的 selected=executed=passed、failed=skipped=not_executed=0；实际数量和证据在完成后补录。
- 精确暂存 8 文件，审查 cached diff、路径、无秘密/构建产物，执行 `git diff --cached --check` 后本地提交；不推送。

## 本轮证据

- 初始测试复现：`evidence/test-environment/20261022T184621Z-15337d5c/summary.json`，部署测试因把单进程探索 option 强加给 Guardian 失败（1 执行、1 失败，无跳过）；已修正两类部署的独立期望，不改变 Guardian 配置。
- 主构建 `test_native_deploy_config` 成功，标准 runtime target 同步单进程及独立探索部署配置到 `build/output/config`；未重启用户服务。
- 修复后单测：`evidence/test-environment/20261022T184811Z-c0604f28/summary.json`，selected=executed=passed=1，failed=skipped=not_executed=0；实际为本地用例，无外部服务。
- 原生集成：`evidence/test-environment/20261022T184852Z-73554148/summary.json`，selected=executed=passed=2（1 业务 + 1 必要 fixture），failed=skipped=not_executed=0。
- 原生用例覆盖默认配置断言、CSV 导入、SQL 命名输出、快照版本与计算、来源进程退出后的快照可用性；使用隔离临时目录和私有进程。
- C++ 改动行 clang-format、Python ast、需求池链接/分类、延迟加载 T1～T3 未开始状态及 `git diff --check` 检查通过。
- 全量、外部数据库/Kafka、Docker/镜像、正式发布未执行；本次通过不代表延迟加载已实现或完整产品验收。
- 提交出口：按允许清单精确暂存全部 8 个文件，核对 staged diff 与 `git diff --cached --check` 后，以冻结标题创建本地提交；本工作台随提交入库，提交后停止。
