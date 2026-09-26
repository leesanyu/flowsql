# 即时工作台

事项：`npm-result-query` T4 显式保留、清理恢复与组合回归
关联 Feature Task：`tasks/archive/feat-npm-result-query.md` T4。
当前 Atomic Slice：无。
状态：已完成；WIP=0。T0～T4 已验收并归档。

## 业务意图

- 托管 run 可显式设定 1～3650 天保留期；无策略时永久保留。到期的终态 run 从查询中消失，并由后续 Open 有界清理。
- 清理中断后继续推进，不自动删除 writing；并发新 run 不与旧 run 混淆。

## Non-Goals

- 不实现崩溃后 writer fencing、人工篡改表修复、备份恢复、跨后端复制或后台定时服务。
- 不改 NPM 实体业务 Schema、普通单表 sink、三段式查询语义或现有结果消费者 ABI。
- 不 push；T0～T3 的差异与 T4 一同按用户明确指令在本地提交。

## 冻结契约与测试锚点

- `parameters.core.result.retention_days` 为可选整数 1～3650；只允许托管 sink 使用。无值时 `expires_at_ns` 为 NULL。
- factory Open 按当前时间计算本次 run 的 `expires_at_ns`，只清理到期 completed/incomplete 或已标记 purging 的 run。
- 清理先条件更新为 purging；公开关系立即隔离。每次 Open 最多选择一个旧 run、一张目录数据表及固定数量业务行；目录游标持久化，重试幂等。
- 清理完成所有数据表后删除 run 元数据。writing 和无期限 run 保留；维护错误保留现场，后续 Open 可重试，不让当前新 run 错标终态。
- 测试锚点：参数边界与非托管拒绝、无策略、到期/未到期、writing 保留、purging 隔离、跨表分批与中断恢复、失败注入与并发 run、四后端同语义。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/archive/feat-npm-result-query.md`（验收后归档）
- `tasks/product_backlog.md`
- `src/operators/npm_basic/config/npm_parameters.h`
- `src/operators/npm_basic/config/npm_parameters.cpp`
- `src/operators/npm_basic/config/npm_basic_task_config.h`
- `src/operators/npm_basic/config/npm_basic_task_config.cpp`
- `src/operators/npm_basic/npm_basic_operator.cpp`
- `src/operators/npm_basic/npm_basic_result_consumer.h`
- `src/operators/npm_basic/npm_basic_result_consumer.cpp`
- `src/tests/test_npm_basic/test_npm_basic.cpp`
- `src/tests/test_npm_basic/test_npm_result_sqlite.cpp`
- `src/tests/test_npm_basic/test_npm_result_backends.cpp`
- `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`

每次 patch 后核对 `git diff --name-only` 和未跟踪文件；T0～T3 既有差异不清理、不覆盖。

## 验收命令

```bash
cmake -B build src
cmake --build build --target test_npm_basic test_npm_result_sqlite test_npm_result_backends test_scheduler_e2e -j8
ctest --test-dir build -R '^(test_npm_basic|test_npm_result_sqlite|test_npm_result_backends|test_scheduler_e2e)$' --output-on-failure
cmake --build build -j8
ctest --test-dir build --output-on-failure
git diff --check
git diff --name-only
git status --short --untracked-files=all
```

定向 Sanitizer 使用独立构建目录，至少执行结果存储/清理专项测试；C++ 变更通过 `clang-format-diff-18 -p1` 核查。

## 时间盒与停止条件

- 用户明确要求完成 T4 并忽略 30 分钟切片时间限制；本轮持续至 T4 验收完成或出现有复现证据的外部阻塞。
- 完成上述锚点和全量验证后勾选 T4、记录证据、归档规格并更新 Backlog；不启动其他 Feature。

## 完成证据

- 实现：托管模式解析 1～3650 天保留期，精确计算本次 run 到期时间；无策略为 NULL，非托管目标拒绝保留配置。
- 清理：后续 Open 先标记到期终态 run 为 purging，再按一个 run、一张实体表、最多 64 行推进；持久游标支持中断恢复；writing 保留，公开关系在到期和 purging 时隔离。
- 兼容：四后端识别并验证 T3 旧目录后追加清理游标列；不兼容目录继续拒绝。
- 测试：SQLite 注入失败与跨表分批恢复、四后端到期/未到期及旧目录迁移专项通过；ASan/UBSan SQLite 和四后端专项通过（禁用当前环境无法运行的 LeakSanitizer）。
- 回归：`cmake -B build src`、`cmake --build build -j8`、完整 CTest 19/19；MySQL 19/19、PostgreSQL 7/7、ClickHouse 21/21 且零 skip。
- 质量：clang-format 检查和 `git diff --check` 通过；T0～T3 差异与 T4 一同本地提交，不 push。
