# 即时工作台

事项：NPM 后续 Feature 边界对齐

关联 Feature Task：无（Backlog Feature 立项边界调整）

当前 Atomic Slice：依据单任务共享分析引擎与观察/持久化分离结论，收敛 NPM 后续 Feature 目标

状态：已完成

## 业务意图

- 明确 `features` 决定同一 `npm.basic` 任务内实际执行的分析模块，`observing` 只决定当前 SQL 前台观察结果，
  避免因观察某类结果而停用其他分析或重复执行基础会话分析。
- 将流式时间驱动、会话分析和协议分析的 Feature 目标调整到上述模型，并把不同协议的独立主链路从过大的
  协议 Feature 中拆出。
- 判断并收敛 `npm-result-query` 的价值边界：持久化消费与前台观察正交，永久存储覆盖全部已启用模块的
  类型化结果，查询正确处理累计快照版本且不要求内存无限保留。

## Non-Goals

- 不创建完整 Feature 规格，不拆解 Feature Task，不修改生产代码、测试、归档规格或 `tasks/lessons.md`。
- 不实现 `features`、`observing`、时间通知、会话/协议算法、数据库写入或查询。
- 不在本轮冻结“持久化与前台观察同时存在”的最终 SQL 语法，不把多个异构结果强行设计为单表，也不选择
  ClickHouse 的物理表结构、更新机制或保留策略实现。
- 不改变现有省略 `features` / `observing` 时 `npm.basic` 的兼容语义和 22 列 Basic 输出契约。
- 不 commit/push，不自动开始任一 Feature 的规格编写或实现。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/product_backlog.md`

## 验收锚点

- `stream-time-drive` 明确时间维护覆盖同一任务内全部已启用模块，不由当前 observer 决定。
- `npm-session-analysis` 明确是 `npm.basic` 内复用统一会话、协议标签、生命周期与预算的可选模块，不重复
  建立会话或识别；模块启用与结果观察彼此独立。
- 原 `npm-protocol-analysis` 收敛为可复用的协议模块基础，DNS、HTTP/1、TLS 握手和 ICMP 按独立可验收
  主链路拆为 Feature；Feature 拆分不导致运行时拆成多个重复执行 Basic 的算子任务。
- `npm-result-query` 面向全部已启用模块的异构类型化结果，持久化 consumer 与 `observing` 正交；支持持续
  写入、版本/最终态查询和保留边界，未配置持久化时不要求保留未观察的终态结果。
- Backlog 只表达每个 Feature 的适用场景、问题和可观察目标，不写实现步骤或未决 SQL/物理存储设计。

## 验收命令

```bash
git diff --check
git diff --no-ext-diff --name-only
git status --short --untracked-files=all
rg -n 'stream-time-drive|npm-session-analysis|npm-protocol-analysis|npm-dns-analysis|npm-http1-analysis|npm-tls-handshake-analysis|npm-icmp-analysis|npm-result-query' tasks/product_backlog.md
```

## 时间盒

25 分钟。

## 停止条件

- 相关 Backlog Feature 的内容、边界、依赖关系和过大风险已按验收锚点收敛，文档检查通过后，将本工作台
  标记完成并停止。
- 若需要修改允许文件之外的内容、冻结未决 SQL/数据库设计或进入具体 Feature 规格，记录为后续工作并停止，
  不扩大本轮范围。

## 完成证据

- `stream-time-drive` 已明确以同一任务全部 enabled feature 的最早截止时间驱动维护，observer 不参与模块
  启停决策；正常 EOF 与错误/取消边界保持分离。
- `npm-session-analysis` 已收敛为 `npm.basic` 内可选模块，复用唯一会话、识别、生命周期和预算，并将指标
  有效性与“序列缺口不等于网络丢包”写入目标。
- `npm-protocol-analysis` 已收敛为共享协议模块基础；DNS、HTTP/1、TLS 握手与 ICMP 按独立价值和主链路
  拆为四个 Feature，同时均保持单个 `npm.basic` 任务内执行、不重复 Basic 分析。
- `npm-result-query` 已收敛为全部 enabled feature 的多实体结果存储命名空间；持久化与 `observing` 正交，
  查询覆盖 latest/final/history revision 和保留边界，不要求内存无限持有未消费结果。
- `git diff --check`、目标条目检索、允许文件和工作区状态检查通过；本轮为纯 Backlog 文档调整，未运行编译或
  CTest，未创建完整 Feature 规格，未 commit/push。
