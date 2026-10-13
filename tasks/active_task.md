# 即时工作台

WIP=0；NPM 用户说明与 Backlog 分类已验收；本地提交范围已冻结。

- 关联：用户授权的 NPM 功能说明补充与 Backlog 分类整理，文档交付已完成，非新增 Feature。
- 授权：用户明确要求“提交”；将本次文档整理执行本地 git commit，不推送。
- 业务意图：将面向使用者的 NPM 说明、README 功能入口和分类后的需求池保存为可复核版本。
- Non-Goals：不继续修改正文或 Feature 状态，不修改代码、接口、配置、性能数字和归档规格，不重新开展性能测量，不提交证据缓存，不推送。
- 允许文件：tasks/active_task.md（提交边界/验收状态）；README.md、docs/npm.md、tasks/product_backlog.md（正文冻结，仅核验/暂存/提交）；evidence/npm-user-guide-backlog/（本地提交审计缓存，不入库）。
- 本轮步骤：核对 Git 状态与四个允许文件的 Diff；重跑文档校验；冻结文件摘要；精确暂存四个文件；核对暂存路径和 blob；本地提交；核对提交树与工作树后停止。
- 验收命令：python3 evidence/npm-user-guide-backlog/verify.py；git diff --check；精确路径与 SHA-256 校验；git diff --cached --check；git commit；git show/提交树核对；git status --short；git check-ignore。本轮仅文档，无需构建/CTest。
- 时间盒：20 分钟，自 2026-10-13 16:18 UTC 起。
- 停止条件：本地提交完成，提交树与冻结摘要一致，工作树状态明确，证据缓存未入库；若内容与前序验收不符，保存证据，不提交。
- 起点：HEAD bdfb8a680952a152b32aeafb96a39cf74127e8f4；暂存区为空，仅上述四个跟踪文件有待提交修改。

## 完成证据

- 2026-10-13 16:14 UTC：五份 NPM 归档规格中的使用者内容已补入 docs/npm.md，包含共享/模块参数、Session 指标与状态、协议组合、TLS 配置和可见性、结果查询/运行状态/保留期；新增导航，示例使用当前分族实体。README 的八处相关入口指向用户说明，归档规格保留。
- Backlog 当前工作集的 34 条已完成 Feature 已归入五类能力目录；当前工作集保留 5 条待办，已完成能力共 47 条。52 条 Feature 原始整行集合逐字节一致，每条仅出现一次，优先级约定和待办顺序保留。
- 执行 python3 evidence/npm-user-guide-backlog/verify.py：81 个相关本地链接/锚点、4 处 JSON 示例、31 个 Session/TLS 字段与参数名称/范围检查通过；现有性能章节逐字节不变。过滤/投影语义按当前 Scheduler 实现及既有回归核对，不采用旧规格已过期的限制。
- git diff --check 与 Diff 审查通过；修改范围仅 README.md、docs/npm.md、tasks/product_backlog.md 和本工作台；其他 1,022 个跟踪文件摘要、HEAD 和空暂存区保持。证据缓存位于已忽略的 evidence/npm-user-guide-backlog/。
- 整理切片仅涉及文档，未运行构建/CTest；整理完成时未暂存、提交或推送。后续本地提交按用户明确授权执行。
- 2026-10-13 16:20 UTC：提交前重新执行文档验证，全项通过；四个文档的 Diff 已审查，其他 1,022 个跟踪文件、原 HEAD 和空暂存区不变。提交范围仅 README.md、docs/npm.md、tasks/product_backlog.md、tasks/active_task.md，按冻结摘要核对暂存内容及最终提交树；本地提交结果以 Git HEAD 和 evidence/npm-user-guide-backlog/commit-verification.json 为准，不推送。
