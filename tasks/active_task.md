# Active Task

Feature：PCAP 文件通道持久化（`pcapfile-channel-persistence`）
原子任务：T0 建立 Feature 入口与精益规格
状态：已完成

## 业务意图

- 将 `pcapfile` 通道配置跨进程重启恢复能力加入顶层需求池，并冻结最小持久化契约和原子任务边界。
- 明确上传文件、Scheduler 通道元数据、运行期 reader 和受管删除之间的所有权与失败语义。

## Non-Goals

- 不修改任何 C++、前端、CMake、部署配置或运行时数据。
- 不回写已归档的 `npm-offline-import` / `npm-offline-import-web` 规格，不改变其历史完成状态。
- 不实现目录扫描猜测逻辑通道名，不扩展公共 block stream ABI、packet Schema 或 SQL 语义。
- 不读取或修改 `tasks/sprints/**`，不执行 commit/push。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/product_backlog.md`
- `tasks/specs/feat-pcapfile-channel-persistence.md`

已有 SQL 结果展示修复的生产代码与测试 diff 只保留，不修改。

## 验收标准与命令

- 顶层需求池新增唯一的 `pcapfile-channel-persistence` P0 进行中条目，并链接新规格。
- 规格不超过精益设计所需范围，包含业务意图、Non-Goals、核心数据契约、两条主链路、故障语义和 4 个原子任务。
- 规格冻结 Scheduler 侧 SQLite 持久化、可选 `db_path` 兼容策略、生产配置要求和独立表名。
- 规格明确 SQL 任务只释放独占 reader，不删除基础通道；服务重启后按原名称与规范化 option 恢复。
- 规格明确缺失/损坏文件使插件启动失败，删除成功后不得再次恢复，并覆盖原生/Guardian/Docker 部署。
- `git diff --check -- tasks/active_task.md tasks/product_backlog.md tasks/specs/feat-pcapfile-channel-persistence.md`
- `rg -n 'pcapfile-channel-persistence|pcapfile_channel_store|db_path|重启|缺失|损坏' tasks/product_backlog.md tasks/specs/feat-pcapfile-channel-persistence.md`
- `git diff --name-only`
- `git status --short --untracked-files=all`

## 时间盒与停止条件

- 时间盒：20 分钟。
- Feature 入口与规格满足验收后，将本工作台标记为已完成并立即停止，不自动实施 T1。
- 若规格需要改变公共 ABI、Web 文件所有权或引入目录扫描，则停止并重新收敛，不扩大本任务范围。

## 完成证据

- `tasks/product_backlog.md` 新增 P0 进行中 Feature `pcapfile-channel-persistence`，且唯一链接到新规格。
- 新规格冻结 Scheduler 侧私有 SQLite 表 `pcapfile_channel_store`、可选 `db_path` 兼容模式以及原生、Guardian、
  Docker 的生产持久路径。
- 规格明确 SQLite 是重启恢复唯一真相；SQL completed 只释放任务 reader，显式删除才移除持久记录和基础通道。
- 规格包含上传/执行/重启恢复与显式删除/失败恢复两条主链路，并采用缺失或损坏文件导致全量启动失败、禁止
  部分恢复的严格策略。
- T0～T3 共 4 个原子任务已冻结；本次只完成 T0，没有实施持久层或插件生命周期代码。
- 三个允许文件的 `git diff --check` 和关键契约检索均通过；未读取/修改 `tasks/sprints/**`，未执行构建、测试、
  commit 或 push。
- 既有 SQL 结果展示修复的 `Tasks.vue`、`taskResult.js` 和 `taskResult.test.js` diff 原样保留，未在本任务修改。
