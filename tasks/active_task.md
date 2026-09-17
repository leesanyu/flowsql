# 即时工作台

事项：设计配置资源通道（`config-channel`）Feature 规格

关联 Feature Task：无（本切片创建 Feature 规格并拆分后续 Feature Task）

当前 Atomic Slice：冻结配置资源的不可变快照、系统自动版本、持久化、控制面接口和通道管理页面契约，
创建精益 Feature 规格并将 Backlog 标记为进行中

状态：已完成

## Non-Goals

- 不实现 C++、HTTP、SQLite、前端代码或测试，不运行构建、前端构建和 CTest。
- 不冻结 NPM Application Scope 等具体消费者的业务 Schema，也不实现 `npm.basic` 参数解析。
- 不把配置资源通道接入普通 SQL `FROM`/`INTO` 数据面，不扩展 DataFrame/Stream 通道 ABI。
- 不实现运行中任务热更新、用户自定义版本号、协同草稿、自动合并、审批流或密钥管理。
- 不实现通道重命名、历史版本物理删除、保留策略或垃圾回收。
- 不修改或清理工作树中其他切片的既有改动，不 commit/push，不自动开始 T1 实现。

## 业务意图

- 让需要跨任务复用静态规则、映射和字典的用户，可在现有通道管理页面上传或编辑 JSON/YAML/XML，
  并发布为具名、持久化、重启可恢复的只读配置快照。
- 由系统只在合法内容成功持久化时自动分配单调递增版本；旧版本不可覆盖，使算子按
  `config.<name>@<revision>` 精确绑定后，在新版本发布或进程重启后仍取得同一内容。
- 明确配置属于有界控制面资源；消费者在任务打开阶段解析一次，逐包热路径不访问数据库或 HTTP。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/specs/feat-config-channel.md`
- `tasks/product_backlog.md`

工作树其他修改属于此前切片，本切片保留且不覆盖。

## 验收锚点

- 新建规格包含业务意图、Non-Goals、核心数据/接口契约、两条主链路、3～5 个结果导向 Feature Task、
  测试锚点和完成出口。
- 规格明确版本由系统在成功发布事务中自动分配；草稿、校验失败和持久化失败不消耗版本；旧版本编辑或恢复
  只能发布为新版本，并发发布不能互相覆盖。
- 规格明确通道管理页面的配置通道列表、创建/上传、编辑并发布、版本历史和精确引用复制行为，页面不提供
  revision 输入框。
- 规格明确 JSON/YAML/XML 的有界安全校验、不可变持久化和精确快照解析职责，领域字段校验由消费者负责。
- Backlog 中 `config-channel` 改为 `[-]`，并链接到新规格；规格进入实施前的非空行数不超过 200。
- 本轮 patch 仅涉及允许文件，Markdown diff 无空白错误。

## 验收命令

```bash
git status --short
git diff --name-only HEAD
git diff --check HEAD -- tasks/active_task.md tasks/specs/feat-config-channel.md tasks/product_backlog.md
rg -n "[[:blank:]]+$" tasks/active_task.md tasks/specs/feat-config-channel.md tasks/product_backlog.md
awk '/^## 完成证据/{exit} NF{n++} END{print n+0}' tasks/specs/feat-config-channel.md
rg -n "系统自动|expected_current_revision|配置通道|JSON|YAML|XML|IConfigChannelRegistryV1|config\\.<name>@<revision>" \
  tasks/specs/feat-config-channel.md
```

## 时间盒

30 分钟。

## 停止条件

- 规格、Backlog 状态和链接完成且验收命令通过后，记录证据并立即停止。
- 若现有接口事实不足以冻结具体实现位置，则在规格中保留实现选择，不扩大到生产代码核查或实现。
- 如需开始任何 Feature Task、实现配置 Provider 或修改前端，必须另起 Atomic Slice。

## 完成证据

- 已新增 `tasks/specs/feat-config-channel.md`，冻结独立 Config Channel Provider、
  `IConfigChannelRegistryV1` 精确快照接口、SQLite 不可变存储、两条主链路和 4 个结果导向 Feature Task。
- 已确定 revision 只在合法快照事务提交成功后由系统从 1 自动递增；相同 current 内容幂等返回，校验失败、
  冲突和持久化失败不占版本，旧版本编辑/恢复只能生成新 revision。
- 已冻结“通道管理”中的配置通道列表、文件/编辑器发布、历史抽屉、纯文本预览/下载和精确引用复制；页面不提供
  revision 输入，409 冲突不自动覆盖。
- 已冻结 512 KiB 内容上限、JSON/YAML/XML 安全解析、Base64 有界控制面传输、单页 100 条分页及
  `config.<name>@<revision>` 精确引用；配置不进入普通 SQL 数据面。
- `tasks/product_backlog.md` 已将 `config-channel` 标记为 `[-]` 并链接新规格；规格完成证据前非空行数为 154。
- `git diff --check` 和尾随空白检查通过；本轮未修改生产代码/测试，未运行构建/CTest，未 commit/push，
  未开始 T1。
