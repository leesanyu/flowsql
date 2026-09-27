# 即时工作台

事项：撤回 SQL 工作台固定展示的 `npm.basic` 观测域说明。
关联 Feature Task：用户对上一轮配置说明交付的修正；`npm-result-query` 已完成。
当前 Atomic Slice：只撤回 SQL 工作台中的固定说明及其样式。
状态：已完成；WIP=0。

## 业务意图

SQL 工作台面向多种算子，不应固定展示单个算子的参数说明；恢复编辑器原有布局。README 保留观测域语法说明。

## Non-Goals

- 不实现 SQL 自动提示、补全或浮动帮助，这些能力后续单独设计。
- 不修改 README、参数解析、来源映射、会话隔离或其他前端页面；本次用户已明确要求提交当前代码，仅本地提交，不推送。

## 冻结撤回契约与验收锚点

- `src/frontend/src/views/Tasks.vue` 恢复到上一轮新增固定帮助面板前的内容；只撤回该面板及 `.npm-domain-help` 样式。
- 前端生产构建通过，将新资源同步到 `build/output/static`；构建资源不再包含固定帮助标题。
- 本轮只修改工作台和 `Tasks.vue`；之前的 README 与 C++ 差异保持原样。

## 既有实现契约（已验收）

- `input_namespace` 选填，SQL 默认取 FROM 来源；显式非空值优先。直接调用没有 SQL 来源时默认取 task_id，纯配置解析默认 `default`。
- `source_domains` 选填；省略或 `all` 时对任意 uint32 source_id 采用 `observation_domain_id=source_id`，不同来源保持隔离。
- 显式 `source_id:domain_id;...` 保留原严格映射：可合并到同一域，重复/非法映射或未映射来源继续报错。
- 新增独立可选接口 `IBlockTransformInputSourceTaskV1::BindInputSource(const char*)`，Scheduler 在 Schema 探测和执行 task 的 Open 前绑定一次；task 拷贝文本。
- 测试锚点：独立省略/全部省略、all、parameters 路径、显式覆盖、uint32 极值、相同五元组多来源隔离、绑定生命周期、无 WITH SQL、托管 SQL 的业务行和 run 来源元数据。

## 允许修改文件

- `tasks/active_task.md`
- `src/frontend/src/views/Tasks.vue`

每次 patch 后执行 `git diff --name-only`，核对本轮增量；上一轮 README 和 10 个 C++ 文件的既有差异保留，本轮不修改。

## 验收命令

```bash
npm run build --prefix src/frontend
cmake -E copy_directory src/frontend/dist build/output/static
git diff -- src/frontend/src/views/Tasks.vue tasks/active_task.md
git diff --check
git diff --name-only
git status --short --untracked-files=all
```

## 时间盒与停止条件

- 10～30 分钟内完成撤回与前端构建。
- `Tasks.vue` 无残余差异、前端构建与静态资源核查通过后，记录完成证据，WIP=0 并停止。

## 上一轮完成证据

- `input_namespace`、`source_domains` 均已选填。Scheduler 在 Schema 探测和正式执行时将 FROM 来源绑定给支持该可选接口的 task；NPM 显式参数保持优先。
- 默认来源映射覆盖全部 uint32 source ID，并将不同 source ID 放入不同观测域；显式映射继续可归一多个来源且对未知来源严格报错。
- 配置单测覆盖独立/共同省略、`all`、`parameters`、显式覆盖、uint32 极值与相同五元组跨来源隔离；SQL E2E 覆盖无 WITH、托管写入和 run 的来源元数据。
- `cmake -B build src`、定向构建及两项定向测试通过；全量构建通过，完整 CTest 19/19 通过；`clang-format-diff-18` 无差异，`git diff --check` 通过。
- 改动仅在冻结的允许文件范围内；未提交或推送。

## 上一轮说明完成证据（本轮撤回其中的前端部分）

- README 的 NPM 配置说明已将旧“必填”描述更新为选填，增加观测域映射的语法、默认值、范围、分隔符、来源编号来源及完整 SQL 示例。
- SQL 工作台曾增加固定的 `npm.basic` 说明面板，本轮按用户要求撤回。
- 上一轮前端构建通过；本轮已重新构建并覆盖当前部署入口引用的静态资源。
- `git diff --check` 通过；本轮增量仅为 README、Tasks.vue 和工作台，上一轮 C++ 差异保留。

## 本轮撤回证据

- `git diff --exit-code -- src/frontend/src/views/Tasks.vue` 通过，SQL 工作台源码恢复为加入固定帮助前的内容。
- `npm run build --prefix src/frontend` 通过；已同步 `build/output/static`。本次 Tasks 构建资源不含该帮助标题，部署资源与构建产物一致。
- `git diff --check` 通过；README 中的 `source_domains` 语法说明和前一轮 C++ 改动均未修改。
