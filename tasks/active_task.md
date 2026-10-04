# 即时工作台

事项：按用户要求同步 README 中 npm.basic 当前能力与周期统计用法。
关联 Feature Task：已归档 npm-basic-periodic-stats 的文档同步，不重新开启实现任务；[契约](archive/feat-npm-basic-periodic-stats.md)。
当前 Atomic Slice：同步周期模式、Basic Schema、查询及实时边界；已完成，WIP=0。

## 业务意图

让 README 读者能配置周期统计，正确区分周期增量、会话累计与终态，并了解当前离线/持续输入的结果交付方式及真实采集可用边界。

## Non-Goals

- 不修改算子、接口、Schema、测试或已归档规格，不实现采集后端。
- 不修改独立 Backlog 与 Linux 采集规格，不开始后续 Feature Task；按用户最新指令仅本地提交 README 与工作台，不推送。
- 不将 Session 性能指标或协议事务描述为 Basic 周期增量，不承诺实时结果推送或真实网卡联验完成。

## 冻结契约与主链路

- 以当前配置解析器、Arrow Schema、周期统计实现、Scheduler 接线与归档契约交叉核对文档。
- 来源能力选择模式 → 默认 periodic_snapshot/30 秒 → 安全采集时间封闭周期 → 输出 Basic 周期增量与累计值 → DataFrame 或已有托管关系查询。
- Basic 固定 32 列 Schema v1；Session 标签版本与协议实体继续使用自身契约。

## 允许修改文件

- README.md
- tasks/active_task.md

基线已有工作台和 product_backlog.md 修改，以及未跟踪的 Linux 采集规格；独立规划保持原样。
上一工作台快照：/tmp/npm-readme-workbench-before.md；README 快照和独立文件校验基线也保存在 /tmp。
验证脚本与日志只写入 /tmp。

## 本轮步骤与验收

同步能力概述、默认 SQL、周期配置/字段/时间语义、Basic 版本及查询示例和实时接线边界；检查相对链接、Markdown、源码契约一致性与范围。
本轮只修改 Markdown；复用已有相关 target 和测试验证文档依据，不新增测试。

```bash
python3 /tmp/check-npm-readme.py
cmake --build build --target test_npm_periodic_contract test_npm_periodic_stats test_npm_basic -j8
ctest --test-dir build -R '^test_npm_(basic|periodic_contract|periodic_stats|periodic_runtime)$' --output-on-failure
git diff --check
git diff --name-only
git status --short
```

## 时间盒与停止条件

- 20 分钟；文档核对、相关测试、格式和范围检查通过后更新证据并停止。
- 不修改 Feature 状态或展开下一切片；若发现其他任务问题，只记录现象与依据。

## 完成证据

- README 已同步默认 periodic_snapshot/30 秒、15/30/60 秒及 JSON 配置、事件时间边界、增量/累计/终态语义、固定 32 列 Basic Schema v1 和 history/latest/final 查询；补全模块结果概览及实时接线/真实采集边界。
- 运行中查询明确使用 Scheduler `/scheduler/batch/status` 的 managed_result，不将 Web 任务详情描述为提供该字段。
- 文档核对通过：源码默认值与范围、Basic 32 列/周期 8 列、6 个 JSON 示例、单 INTO、相对链接、Markdown、旧说明清理和 Scheduler 状态响应契约。
- 三个相关 target 构建成功，无 Error/Warning；Basic、periodic_runtime、periodic_contract、periodic_stats 的 CTest 4/4 通过，共 2.05 秒。
- git diff --check 无诊断；本轮修改仅 README.md 与工作台。独立 Backlog 和未跟踪 Linux 采集规格的 SHA-256 与本轮基线一致。
- 文档同步仅修改 Markdown，未新增测试、运行全量回归或真实采集，未开始下一切片。
- 2026-10-04 用户授权提交：重新核对 README 与工作台差异、文档契约、独立文件校验和空白；提交范围仅 README.md 与 tasks/active_task.md，保留 Backlog 与采集规格，不推送。复用文档同步阶段已通过的 4/4 CTest，不重复运行。
