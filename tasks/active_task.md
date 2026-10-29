# 当前工作台

WIP=0；状态：`operator-data-contract`实施与本地提交审查已完成；Feature T1～T5已按顺序验收并逐项勾选。

## 本地提交边界（用户明确授权）

- 业务意图：将已完整验收的Feature保存为可审查的本地提交，保留无关工作区改动。
- Non-Goals：不修改生产源码或重做已接受验收，不推送/打标签/发布/部署，不重载服务，不提交证据、构建产物、私有配置或Epic。
- 允许文件：仅本工作台可编辑；提交范围为`evidence/operator-data-contract/local-commit/reviewed-paths.txt`冻结的115条精确路径。`tasks/product_backlog.md`仅暂存本Feature完成行，其余规划段落和Epic起点改动保持未暂存；其余114条按已审查全文暂存。
- 提交信息：`feat: add versioned operator input and output contracts`；起点HEAD和索引/路径/摘要、完整提交计划保存在同目录`initial-state.json`及`plan.json`。
- 验收：核对标准运行`20261029T163118Z-9e84331c`的67/67与42项必需业务/回归清单；已验证1103项源码/配置与232项构建产物仍匹配验收后摘要。只新增工作台提交记录，无需重复构建/业务测试；冻结范围外矩阵、性能对照与用户服务重载保持未验证。
- 命令：精确路径暂存与Backlog单行索引写入；`git diff --cached --name-only`、`git diff --cached`、`git diff --cached --check`；本地`git commit`；提交后核对路径/索引、无关改动和文件摘要。
- 时间盒：20分钟；停止条件：提交审查通过、恢复WIP=0并完成一次本地提交，验证无关改动保留后停止。
- 提交审查已通过：115条暂存路径与冻结清单逐项一致；Backlog索引仅新增本Feature完成行；全部源码在已接受摘要范围内，发布Schema匹配，未暂存二进制/凭证/证据。`git diff --cached --check`通过，提交后身份与无关改动核对结果保存在`evidence/operator-data-contract/local-commit/`。提交后不启动其他工作。

## 本轮交付与边界

- 最后Atomic Slice：T5权限开放后的真实HTTP/Worker/Chromium闭环与Feature最终验收；时间盒自2026-10-29 16:16 UTC起30分钟，已在时间盒内完成。
- 业务结果：作者可创建/上传声明，用户可查看详情/历史并按实际来源及已有算法参数预检；普通SQL自动取得组件关联契约，结果通过精确使用依据、字典和必要上下文追溯。
- 实施阶段Non-Goals：不接入模型/MCP，不扩展Python多输入或通用DAG，不重验外部数据库/Kafka/Docker及完整Baseliner算法/恢复矩阵，不重载用户服务，不修改Epic；本地提交另由上方本轮用户授权，仍不推送。
- 本轮允许边界与验收前工作台已冻结在`evidence/operator-data-contract/final-accepted/active-task.md`；后续没有修改源码、测试、配置或构建产物。收口仅更新本工作台、Feature规格及Backlog，移动单个规格文件归档，不移动/删除目录。

## 最终验收与依据

- 标准配置入口：`cmake -B build src`；默认产物为`build/output`。冻结目标及前端构建通过；本轮修复Task后重建`test_task`和`test_operator_contract_host`及其生产依赖通过。
- 冻结清单：本机32项+网络10项，共42项业务/回归；另25项自动必需环境/工作区夹具。具体用例和选择依据见`final-accepted/frozen-scope.json`及验收前工作台。
- 选择依据：契约模型/Schema、登记、输入三态、执行/并发隔离、输出规则/绑定/use；Baseliner代表组合；受影响F1/F2、framework/stream/builtin、Bridge/Python、Task/Scheduler/Router及真实浏览器回归。
- 运行依赖：native执行；临时SQLite/CSV/DataFrame、隔离插件宿主、真实Python Worker/Arrow IPC、随机localhost端口、Chromium。无外部业务数据库/Kafka或Docker；用户授权后socket实测成功。
- 最终标准运行：`20261029T163118Z-9e84331c`，selected=executed=passed=67，failed=skipped=not_executed=0。42项必需用例逐项核对全部通过，额外项仅上述25项必需夹具；源码/默认产物验收前后摘要一致。
- 最终日志、summary、CTest XML、源码/产物摘要：`evidence/operator-data-contract/final-accepted/`。默认生产库（含Task）、发布Schema及18个静态前端文件身份已核对，见`evidence/operator-data-contract/default-artifacts.json`，`services_reloaded=false`。
- 静态出口：修改C++行格式/版权、Python/Node语法、文档Python/JSON片段及本地链接、`git diff --check`通过；未修改的`test_bridge/main.cpp`既有忽略Arrow返回值编译警告未扩大修复。

重跑同一已冻结范围的标准入口：

```bash
python3 - <<'PYTHON'
import json
import subprocess
from pathlib import Path
scope = json.loads(Path("evidence/operator-data-contract/final-accepted/frozen-scope.json").read_text())
raise SystemExit(subprocess.call(["tools/test.sh", "all", "--location", "native", "--filter", scope["patterns"]["full"]]))
PYTHON
```

## T5修复与接受的检查点

- 历史socket权限阻塞已解除；首轮网络17项15通过2失败，真实链路定位到宿主漏载Task、普通客户端错误使用内部SQL请求形状及浏览器异步等待竞态。
- HTTP宿主加载真实Task；客户端沿用`sql_text`+`mode=sync`。Task在既有结果摘要中聚合各SQL的contract_checks/use引用，同步返回及重启后任务详情均可读取；契约结构化错误保留422/409/503等状态及字段依据。
- Task新增断言先复现红灯再修复，既有Task回归一同通过并纳入标准CTest/依赖声明。浏览器仅等待目标行和目标操作按钮可用，保留全部业务断言。
- Python严格copy保留真实Polars默认Arrow可空性漂移负向断言，返回`OUTPUT_SCHEMA_MISMATCH`；成功样例显式生成匹配Schema，通用检查未放宽。
- 新HTTP/Worker与Chromium检查点`20261029T162910Z-e5650a84`为4/4；最后完整67/67收口，历史失败日志保留在`evidence/operator-data-contract/t5-network-resumed/`，不冒充最终状态。

## 完成出口与跨任务记录

- T5已在规格勾选，T1～T5全部完成；规格移至`tasks/archive/feat-operator-data-contract.md`，其中Epic相对链接已修正；Backlog仅将本Feature改为完成并更新规格链接。
- 保留用户及前序diff；`tasks/specs/epic-data-semantics-dictionary.md`全文与实施起点快照一致。该Epic中原有“已立项、未实施”和旧specs链接会陈旧；属于Epic文档同步问题，按本次明确不改Epic的边界保留，后续单独处理。
- 实施验收结束时用户运行服务未重载、代码未提交；随后用户明确授权本轮本地提交，仍不推送/发布/部署。证据缓存由Git忽略，未纳入上述冻结范围的模块与部署矩阵不声明已验证。
- 本Feature已完成并停止，没有自动启动其他Feature或Task。
