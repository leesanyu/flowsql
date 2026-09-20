# 即时工作台

事项：流量标签化通用能力命名收敛
关联 Feature Task：`tasks/specs/feat-flow-labeling.md` T0；本切片只把 Feature、公共契约草案和配置模板从
`npm-labeling` 统一重命名为 `flow-labeling`，不开始实现。
当前 Atomic Slice：中文产品名保持“流量标签化”，技术标识、插件、IID、公共类型、构建开关和配置 API 使用
`flow-labeling`/`FlowLabeling`，并把 `npm.basic` 明确为首个消费者而非能力归属。
状态：已完成（2026-09-20）

## 业务意图

- 让会话流主标签能力可被网络性能、安全等分析场景复用，命名不再把通用 provider 锁定到 NPM 产品域。
- 让 Feature、文件、配置 Schema、插件二进制和公共 ABI 草案使用单一命名，避免实施 T0 时形成兼容包袱。

## Non-Goals

- 不改变双向会话流、唯一主标签、admission batch、DPDK ACL、EAL、预算和 protocol/label 双维度契约。
- 不扩展 packet 标签、多标签、安全规则引擎或新消费者，不修改 CMake、Docker、部署配置、代码和测试。
- 不安装依赖、不运行构建/CTest，不自动 commit/push。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/product_backlog.md`
- `tasks/specs/feat-npm-labeling.md` → `tasks/specs/feat-flow-labeling.md`
- `config/npm-labeling-template.yaml` → `config/flow-labeling-template.yaml`
- `tasks/archive/feat-npm-basic-parameters.md`

## 验收命令

```bash
test ! -e tasks/specs/feat-npm-labeling.md
test -e tasks/specs/feat-flow-labeling.md
test ! -e config/npm-labeling-template.yaml
test -e config/flow-labeling-template.yaml
awk '/^## 完成证据/{exit} NF{n++} END{print n+0}' tasks/specs/feat-flow-labeling.md
rg -n 'flow-labeling|FlowLabeling|FLOWSQL_FLOW_LABELING|libflowsql_flow_labeling|IID_FLOW_LABELING' \
  tasks/specs/feat-flow-labeling.md tasks/product_backlog.md config/flow-labeling-template.yaml
! rg -n 'npm-labeling|NpmLabel|NPM_LABELING|libflowsql_npm_labeling|IID_NPM_LABELING' \
  tasks/specs/feat-flow-labeling.md tasks/product_backlog.md tasks/archive/feat-npm-basic-parameters.md \
  config/flow-labeling-template.yaml
rg -n '[[:blank:]]+$' tasks/active_task.md tasks/product_backlog.md tasks/specs/feat-flow-labeling.md \
  tasks/archive/feat-npm-basic-parameters.md config/flow-labeling-template.yaml
git diff --check
git diff --no-index --check /dev/null tasks/specs/feat-flow-labeling.md
git diff --no-index --check /dev/null config/flow-labeling-template.yaml
git diff --name-only
git status --short
```

规格从文件头到 `## 完成证据` 前保持不超过 200 个非空行；本切片只做命名和定位收敛。

## 时间盒与停止条件

- 时间盒：30 分钟；若需延续则沿用本工作台，不扩大到实现。
- 停止条件：所有在册引用统一指向 `flow-labeling`，通用 ABI/配置命名不再含 NPM，`npm.basic` 仅作为首个消费方；
  既有语义未改变，文件存在性、尺寸、旧名称清零和 Diff 检查通过后记录证据并立即停止。

## 完成证据

- Feature 已从 `npm-labeling` 收敛为通用 `flow-labeling`，规格和配置模板分别迁移到
  `tasks/specs/feat-flow-labeling.md` 与 `config/flow-labeling-template.yaml`；Backlog、后续 Feature 依赖及已归档参数规格引用已同步。
- 插件、IID、接口、POD、构建开关和配置 API 已统一为 `libflowsql_flow_labeling.so`、`IID_FLOW_LABELING_PROVIDER_V1`、
  `IFlowLabelingProviderV1`、`FlowLabel*V1`、`FLOWSQL_FLOW_LABELING` 和 `flowsql.io/flow-labeling/v1alpha1`。
- 规格明确网络性能和安全等场景可复用该通用 provider，`npm.basic` 只是首个消费者；双向会话流、唯一主标签、
  admission batch、DPDK/EAL、预算与 protocol/label 双维度契约均未改变。
- 文件存在性和旧标识清零检查通过；规格在 `## 完成证据` 前仍为 200 个非空行，尾随空白与 tracked/untracked
  `git diff --check` 均通过。Diff 仅涉及允许文件，本切片未运行构建/CTest，也未 commit/push。
