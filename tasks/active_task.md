# 即时工作台

WIP=0；指令日志文档删除已验收，本地提交范围已冻结。

- 关联：用户明确要求删除 docs/commands.md，非新增 Feature。
- 授权：用户明确要求“提交”；执行本地 git commit，不推送。
- 业务意图：将已验收的旧指令日志删除保存为可复核版本。
- Non-Goals：不改功能文档、历史归档或代码，不纳入其他修改或证据缓存，不推送。
- 允许文件：docs/commands.md（已删除，仅暂存/提交）、tasks/active_task.md（提交范围与验收状态）。
- 本轮步骤：核对文件缺失和两文件 Diff；冻结摘要；精确暂存；审查暂存内容；本地提交；核对提交树与工作树后停止。
- 验收命令：文件不存在及路径/摘要断言；git diff --name-only、git diff --check、git diff --cached --check；git commit；git show 与 git status --short。
- 时间盒：10 分钟，自 2026-10-13 16:31 UTC 起。
- 停止条件：本地提交仅包含文件删除和工作台更新，提交树与冻结内容一致，工作树状态明确。
- 提交起点：HEAD f34b75429c6ccba95077390ee66f0f056164a15e，暂存区为空，仅文件删除和工作台更新待提交；引用仅在 docs/archive/design_database_channel.md 和 tasks/sprints/sprint4/planning.md 中作为历史记录出现，无实际 Markdown 链接。

## 完成证据

- 已删除 docs/commands.md；扫描全部跟踪文件确认没有现行 Markdown 链接指向该文件，两份归档中的历史提及保持。
- 文件不存在断言、git diff --check 和范围检查通过；Diff 仅含该文件删除与工作台更新，暂存区为空、HEAD 未变。
- 删除切片仅涉及文档，无需构建或测试；其完成时未暂存、提交或推送。后续提交按用户明确授权执行。
- 提交前文件缺失、两文件范围和 Diff 检查通过；精确暂存 docs/commands.md 删除及本工作台，核对暂存内容和最终提交树。本地提交结果以 Git HEAD 为准，不推送。
