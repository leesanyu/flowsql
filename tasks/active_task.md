# Active Task

Feature：NPM 离线文件上传与通道管理（`npm-offline-import-web`）
原子任务：D1 刷新归档规格中的回放表单契约
状态：已完成

## 业务意图

- 将已完成的 PCAP 上传回放表单交互同步到归档 Feature 规格，使页面语义、HTTP 线协议和验收锚点与实现一致。
- 明确两种回放模式只展示各自生效的参数，以及字面倍速到 `replay_speed_milli` 的兼容转换边界。

## Non-Goals

- 不修改生产代码、测试、HTTP 线协议、C++ 配置结构或其他 Feature 规格。
- 不重新设计回放算法，不增加倍率档位或任意小数输入。
- 不修改或撤销执行前已有的前端、backlog、Basic 分析规格改动，不读取或修改 `tasks/sprints/**`。
- 不执行 commit/push。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/archive/feat-npm-offline-import-web.md`

## 验收标准与命令

- 页面契约明确默认 `fast` 仅显示批大小，`timestamp` 在同一位置仅显示七档字面倍速。
- HTTP 契约保持 `replay_speed_milli:uint32`，文档明确七档倍率的精确转换表和仅校验当前生效参数的规则。
- 主链路、T4 完成项、测试锚点和完成出口与当前实现及已通过的前端验证一致。
- `git diff --check`
- `git diff --name-only`
- `git status --short --untracked-files=all`

## 时间盒与停止条件

- 时间盒：30 分钟。
- 文档一致性与 Diff 检查通过后记录完成并停止。
- 若发现必须修改实现或其他规格才能完成，则记录阻塞并停止，不扩大范围。

## 执行前基线

- 已有未提交改动：三个 PCAP 上传前端文件、`tasks/product_backlog.md`、`tasks/specs/feat-npm-basic-analysis.md`，
  以及本文件上一项已完成任务记录。
- 上述改动属于执行前基线，本任务全部保留且不修改。

## 完成证据

- 归档规格已在业务意图、HTTP 与页面模型边界、页面契约、上传主链路、T4 完成项、测试锚点和完成出口中同步
  回放参数互斥展示语义。
- multipart 线协议继续冻结为 `batch_packets:uint32` 与 `replay_speed_milli:uint32`；新增七档字面倍率到千分倍率
  的精确映射，并明确只校验当前模式可见参数。
- Markdown 围栏计数为 8 且闭合，关键术语一致性检查和 `git diff --check` 通过。
- 本原子任务新增修改仅限归档规格与工作台；执行前已有前端、backlog 和 Basic 规格改动均保留且未修改，
  未读取或修改 `tasks/sprints/**`，未执行 commit/push。
