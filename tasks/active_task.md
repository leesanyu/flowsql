# 即时工作台

WIP=0；NPM 优化与用户文档已验收，提交审计通过。

- 关联 Feature：npm-ip-family-throughput、npm-clickhouse-columnar-write（均已验收归档）；用户授权的文档整理已完成。
- 授权：2026-10-13 用户明确要求“提交代码”；执行本地 git commit，不推送。
- 业务意图：将已完成的分族处理/批量结果优化、ClickHouse 列式写入、回归与测量工具、归档状态和用户文档作为可复核版本提交，证据缓存继续留在本地。
- Non-Goals：不增加实现、不修改生产代码/测试/测量数字，不重新开展性能测量，不纳入忽略缓存或其他任务改动，不推送。
- 当前 Atomic Slice：核对验收源码/产物身份、终端回归日志、文档检查和 Diff；按精确清单暂存、审查暂存区、提交并核对提交树及工作树。
- 允许文件：tasks/active_task.md；evidence/doc-reorganization/commit-plan.json 中明确列出的 66 个路径仅允许暂存/提交（源码、测试、工具内容冻结不编辑）；本地审计缓存 evidence/doc-reorganization/commit-*、active-before-commit.md。精确清单生成于提交前，包含初始 HEAD、文件 SHA-256 及删除状态。
- 验收命令：文档 verify_docs.py；验收源/二进制/日志 SHA-256 检查及 Python AST；git diff --check；暂存路径与 commit-plan.json 完全相符、暂存 blob 与工作树/冻结摘要一致；git diff --cached --check；git commit；git show/提交树核查；git status --short；git ls-files evidence docs/evidence 与 git check-ignore。
- 时间盒：30 分钟，自 2026-10-13 15:44 UTC 起。
- 停止条件：本地提交完成、提交树与审计一致、工作树状态明确且缓存未入库后置 WIP=0 并停止；若确认源码或产物与验收不符，先保存错误证据，重新冻结验证边界。
- 起点：HEAD f9bdce72896204057961c414c0e3734508968730，暂存区为空；66 个待提交路径均属于上述已完成任务，前序文档验收和原文保存在 evidence/doc-reorganization/。

## 完成证据

- 2026-10-13：66 个提交路径冻结，46 个源码/CMake/测试路径与两项 Feature 的最终验收 SHA-256 完全一致，8 个测量二进制摘要一致；源码、测试和工具未新增修改。
- 原始最终构建退出 0、完整 CTest 93/93（214.22 秒，零失败/跳过）、相关 ASan/UBSan/LSan 7/7（77.39 秒）的日志及证据 manifest 163 项摘要已核对。本轮未重跑构建、测试或性能测量；既有构建告警的来源与数量保持记录，不表述为全量零告警。
- 文档复核 149 个本地链接及锚点、5 个 JSON 示例通过；两项 Python 工具 AST、C++ 版权与 git diff --check 通过。
- 精确路径/内容、暂存区与提交树审计保存在忽略缓存 evidence/doc-reorganization/commit-{plan,validation,staged,verification}.json；本地提交结果以 Git HEAD 为准，未推送。
- 两项 Feature 和文档整理已收口；evidence 缓存不入库，无其他任务待处理改动。完成后停止。
