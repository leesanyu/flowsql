# Active Task

Feature：NPM Feature 规格归属维护
原子任务：D0 修正待规划 Feature 的规格列语义
状态：已完成

## 业务意图

- 保证 `product_backlog.md` 的“规格”列只把 Feature 自己的规格文件作为正式规格入口。
- 对尚未建立独立规格的 NPM 相关 Feature，保留“待创建”状态和必要的依赖说明，但不再把
  `feat-npm-basic-analysis.md` 表现为它们的规格。

## Non-Goals

- 不提前创建 `stream-time-drive`、`npm-session-analysis`、`npm-protocol-analysis`、`npm-result-query` 或
  `npm-capture-contract` 的空规格。
- 不修改 `feat-npm-basic-analysis.md` 的契约、任务拆分或状态。
- 不调整 Feature 优先级、目标、依赖关系或实施顺序。
- 不修改生产代码、测试或其他任务文档，不执行 commit/push。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/product_backlog.md`

## 验收标准与命令

- `npm-basic-analysis` 保持链接到自己的正式规格。
- 其余五个待规划 Feature 的规格列明确为“待创建”，可保留纯文本依赖说明，但不包含指向 Basic 规格的链接。
- 不创建或修改任何 Feature 规格文件。
- `rg -n "npm-basic-analysis|stream-time-drive|npm-session-analysis|npm-protocol-analysis|npm-result-query|npm-capture-contract" tasks/product_backlog.md`
- `git diff --check`
- `git diff --name-only`
- `git status --short --untracked-files=all`

## 时间盒与停止条件

- 时间盒：15 分钟。
- backlog 的规格归属消歧且所有 Diff 检查通过后，记录完成证据并停止。
- 若必须创建或修改独立 Feature 规格才能完成，则记录阻塞并停止，不扩大范围。

## 执行前基线

- `git status --short --untracked-files=all` 无输出，工作树干净。

## 完成证据

- `npm-basic-analysis` 仍是需求池中唯一链接到 `specs/feat-npm-basic-analysis.md` 的 Feature。
- `stream-time-drive`、`npm-session-analysis`、`npm-protocol-analysis`、`npm-result-query` 和
  `npm-capture-contract` 的规格列已统一为“待创建”，Feature 目标中的依赖与职责描述保持不变。
- 未创建或修改任何 Feature 规格、生产代码或测试；本任务 Diff 仅包含允许的工作台与需求池文件。
- 关键条目核查通过，`git diff --check` 无输出。
