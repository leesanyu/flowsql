# 即时工作台

事项：`npm-labeling` 会话 admission 与协议/标签双维度契约刷新
关联 Feature Task：`tasks/specs/feat-npm-labeling.md` T0/T2；本切片只修正 Feature/MVS、配置模板、执行单位与
协议/标签并存契约，不开始 T0/T2 代码实现。
当前 Atomic Slice：将 Labeling 从逐 packet 识别收敛为“新会话候选 admission 批量分类”，已有会话复用不可变主标签；
保留 DPDK ACL batch 能力，并明确 protocol 是客观协议维度、label 是网络自定义归属维度，二者同时存在。
状态：已完成（2026-09-19）

## 业务意图

- 让未启用标签化的 Basic/Session 用户不因可选能力被迫承担 DPDK 运行依赖，启用标签化的任务又能在打开阶段
  获得明确的 provider、DPDK 环境和配置编译诊断。
- 让 DPDK/EAL 的进程级生命周期与每任务 ACL context 生命周期可审计，同时保持唯一 packet/session 主链；
  Labeling 只对新会话 admission 候选批量执行，避免长连接后续 packet 重复分类。

## Non-Goals

- 不实现插件、IID、CMake、部署或测试代码，不安装 DPDK，不运行构建/CTest。
- 不创建新的 SQL operator、数据通道或独立进程，不把逐包数据经过 HTTP/IPC。
- 不提前抽象通用 `IDpdkRuntimeV1`，不修改 ACL 字段能力、唯一主标签语义或容量前置；本切片同步刷新设计模板的
  admission 与协议/标签并存字段，不实现 YAML 解析器。
- 不处理当前切片前已有的其他工作树改动，不自动 commit/push，不开始 T0；后续仅按用户明确指令提交。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/product_backlog.md`
- `tasks/specs/feat-npm-labeling.md`
- `config/npm-labeling-template.yaml`

本切片前已有 `tasks/archive/feat-config-channel.md`、`tasks/archive/feat-npm-basic-parameters.md`、
`tasks/product_backlog.md` 以及上述允许文件的改动；保留这些改动，只审查本次追加的 admission/NPI 契约。

## 验收命令

```bash
awk '/^## 完成证据/{exit} NF{n++} END{print n+0}' tasks/specs/feat-npm-labeling.md
rg -n 'libflowsql_npm_labeling|IID_NPM_LABELING_PROVIDER_V1|INpmLabelingProviderV1|INpmLabelMatcherV1|admission|NPI' \
  tasks/specs/feat-npm-labeling.md config/npm-labeling-template.yaml
rg -n 'decision_time|session_partition_key_includes_primary_label|classification_scope|classification_batch' \
  config/npm-labeling-template.yaml
rg -n '第二套|新.*operator|逐包.*IQuerier|Config Channel.*插件|IDpdkRuntime' \
  tasks/specs/feat-npm-labeling.md
rg -n '[[:blank:]]+$' tasks/active_task.md tasks/specs/feat-npm-labeling.md config/npm-labeling-template.yaml
git diff --check
git diff --no-index --check /dev/null tasks/specs/feat-npm-labeling.md
git diff --no-index --check /dev/null config/npm-labeling-template.yaml
git diff --name-only
git status --short
```

规格从文件头到 `## 完成证据` 前的非空行不得超过 200；本切片仅修订 Markdown，无代码格式化或构建目标。

## 时间盒与停止条件

- 时间盒：30 分钟；若需延续则沿用本工作台，不扩大到实现。
- 停止条件：规格和模板一致明确基础 session lookup、唯一新会话候选 admission、bounded batch、已有会话复用标签、
  NPI 后置以及 protocol 和 label 双维度并存；原有插件/IID/EAL 契约保持不变，文档尺寸和 Diff 检查通过后记录证据并立即停止。

## 完成证据

- 规格已将 Labeling 的 ACL 执行输入改为 admission window 内按基础 session key 去重的唯一新会话候选；
  session hit 复用已保存的主标签，`label_id=0` 也缓存，tuple reuse 重新分类。
- 规格、模板与 Backlog 已同步冻结 bounded batch、原 packet 顺序 replay、主标签不进入 session key，及 NPI 在 session 建立后采样、protocol 与 label 作为并列结果保存。
- `awk` 统计规格在 `## 完成证据` 前为 200 个非空行；`rg` 确认 admission/NPI 语义及模板关键字段已更新，未发现旧的
  `before_session_lookup` 或 `session_partition_key_includes_primary_label: true` 表述。
- `git diff --check` 与针对未跟踪规格/模板的 `git diff --no-index --check` 均通过；本切片未运行构建或 CTest，后续获得用户明确 commit 授权，未授权 push。
