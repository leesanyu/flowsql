# Feature: SQL 统一采集、分析与输出指引

状态：`[x]` 已完成（2026-10-27）
优先级：P1
标识：`sql-workflow-guidance`

## 业务意图

面向通过 SQL 构建采集、分析和输出流程的用户，交付与当前实现一致的能力发现步骤、入口选择规则和可复现示例，
让用户能够查到已配置通道与已激活算子，完成数据准备、执行、结果核验和结束操作，避免沿用旧语法或误选执行入口。

统一说明落在 `docs/sql-workflow.md`，README 提供入口；愿景保留产品方向，并明确区分当前能力与未来设想。
本 Feature 是后续数据字典和交互式诊断的使用基础，不以前置完成这两项为条件。

## Non-Goals

- 不扩展 SQL 方言、Parser、算子组合能力或调度语义，不新增统一目录服务、HTTP API、插件 IID 或 ABI。
- 不增加 Web 向导、自动生成 SQL、自动选算子、模型接入或 skills 编排。
- 不实现数据字典、长期任务恢复、探索成果保存或原始报文调查；不承诺通用暂停续跑或 exactly-once。
- 不重写 NPM、Kafka、NetAdapter、Baseliner 的专业说明，不复制完整字段字典或全部参数表。
- 不以文档承诺替代功能交付；核查发现的实现缺口单独记录，除当前任务 P0/P1 阻塞外不在本 Feature 修复。
- 不增加数据库、认证、采集后端或容器部署支持，不把这些既有矩阵的重新验收作为指引交付的默认门槛。

## 当前依据与交付边界

以下依据已于 2026-10-27 静态核查；编写使用说明时须按实际 checkout 再核对，不能视为本轮运行验收。

| 当前依据 | 本 Feature 的约束 |
| --- | --- |
| [Parser 数据结构](../../src/framework/core/sql_parser.h)、[解析实现](../../src/framework/core/sql_parser.cpp) | 使用现有 SELECT / FROM / USING / WITH / THEN / INTO 结构及阶段过滤规则。 |
| [Web 路由](../../src/services/web/web_server.cpp)、[Task 分析与提交](../../src/services/task/task_plugin.cpp) | 对外入口使用 `/api/`，内部 Scheduler 路径单独注明，保持请求字段的实际差异。 |
| [当前任务矩阵](../../README.md#sql-任务能力矩阵当前)、[Scheduler 路由](../../src/services/scheduler/scheduler_routes.cpp) | 区分 batch、stream、mixed 与有限/持续来源，不能依据名称或 INTO 推断执行类型。 |
| [愿景](../../docs/vision.md) | 旧“算子前置”、GRANULARITY、模型训练及 PAUSE/RESUME 等设想不能作为当前可执行示例。 |
| [NPM](../../docs/npm.md)、[Kafka](../../docs/kafka.md)、[NetAdapter](../../docs/netadapter.md)、[Baseliner](../../docs/baseliner.md) | 专业能力与限制引用原说明；发现同一能力的表述冲突时以实现和本次断言核对后同步修订。 |

## 核心契约

### 能力发现与执行入口

复用既有纯虚接口及 HTTP 契约，不新增公共头文件或虚函数。指引中的能力清单是使用说明，不另建运行态真相。

| 对象 | 发现方式与必须解释的边界 |
| --- | --- |
| DataFrame / CSV | Web 通道管理及 `GET /api/channels/list`；说明列表主要查询 DataFrame、存在本地回退，不能充当全类型实时目录；未加载时 rows/schema 可为 null。 |
| 数据库 | `POST /api/channels/database/query`、`tables`、`describe`；区分已配置连接、真实可连接及目标读写权限。 |
| Stream / block 来源与出口 | `POST /api/channels/stream/query` 与 `/api/channels/stream/definitions/query`；区分通道定义、实例和实际来源类型，覆盖 PCAP、NetAdapter 及 Kafka 四类适配器。 |
| 算子 | `GET /api/operators/list?type=builtin`，type 分别取 builtin、python、cpp；说明 category.name、active、插件加载及依赖就绪的区别，不假定三类响应完全相同。 |
| 配置通道 | 复用配置管理与 `config.<name>@<revision>`；它是算子配置引用，不按通用数据来源展示。 |
| SQL 分类 | `POST /api/tasks/sql/classify` 使用 `sql`；多语句分析 `/api/tasks/sql/analyze` 使用 `sql_text`；说明 task_kind、requires_async 及分析结果不等于可执行性验证。 |

任务入口说明必须包含以下决策表，并说明 Web 的模式选择与 API 的可接受模式各自边界：

| 来源 / 组织方式 | 提交与结果边界 |
| --- | --- |
| DataFrame、数据库、有限 PCAP | `/api/tasks/batch/execute`，参数 `sql_text`；默认 async，有限输入可显式 sync；多条 batch 按分号切分、顺序执行。 |
| NetAdapter、持续 Kafka block source | 同一 batch 入口，必须 async；持续不等于 IStreamChannel；显式 sync 在创建任务前拒绝。 |
| IStreamChannel 单 SQL | `/api/tasks/stream/execute`，`execution_kind=single`、`sql_text`，异步且需要流式算子。 |
| Stream / mixed 多 SQL | 同一 stream 入口，`execution_kind=group`、`group_mode=dag`；至少两条，分号分隔；同一语句不混用 stream 与非 stream source。 |

batch 的任务列表、结果、取消入口与 stream 的 list/status/stop 入口分别说明；`task_id` 与 `runtime_task_id`
按实际响应和查询接口配对。明确正常 Stop、Cancel、EOF 的结果边界，不把“取消”按钮名称等同于底层立即 Cancel。
同步 Web 上游读取等待为 30 秒，独立于 `timeout_s`；出现 `service unreachable` 时先查原任务状态与结果再决定重试。

### SQL 与输出语义

```text
SELECT <projection> FROM <source> [WHERE <source-filter>]
  [USING <operator> [WITH <params>] [WHERE <operator-filter>]
    {THEN <operator> [WITH <params>] [WHERE <operator-filter>]}]
  [INTO <destination>]
```

这是结构说明；尖括号和花括号不作为可执行 SQL。指引必须明确：

- USING/INTO 是否可省略依具体来源、算子与出口契约决定；THEN 表示同一语句算子链，分号表示多条语句。
- WITH 绑定所属算子；JSON 参数按 SQL 字符串规则转义，配置引用固定 revision；不引入全局参数或隐式 latest。
- WHERE 绑定紧邻的生产阶段；类型化 source/operator 过滤仅用于已支持的 block 路径与可绑定输出 Schema。
  数据库原生 SQL 的过滤/聚合、旧 DataFrame 与 IStreamChannel 过滤分别说明；不宣称统一支持 JOIN、聚合或全部表达式。
- 两段数据库名与三段表名依算子契约解释；不能把 `npm.basic` 的多实体托管出口等同于任意普通表写入。
- 普通 NPM 输出需显式选择已启用的 observing 实体；持续 NPM 使用两段数据库托管目标，不能输出到普通 DataFrame。
- NPM 与 Baseliner 通过已发布的 DataFrame/数据库结果衔接，不承诺 `npm.basic THEN explore.baseliner` 可执行。
- Kafka source/sink、packet source/sink 分别列出数据契约、方向和支持的 SQL 形状，不因都叫 kafka 而互换。

### 示例记录

每个示例使用稳定 ID，采用下列最小记录；记录用于文档与测试关联，不建立通用工作流引擎或新持久化格式。

```text
WorkflowExampleV1 {
  id; purpose; level: executable | conditional_template;
  prerequisites; setup; request: {method, path, body};
  expected: {status, schema_or_entity, values_or_counts};
  observe_and_stop; cleanup; limitations;
}
```

`executable`：有受管输入、准备步骤和可执行断言；验收直接读取文档中带 ID 的 SQL/请求块，仅替换明确声明的
通道名、路径和 fixture 参数，不能另外手写等价 SQL 充当文档验收。预期值应由固定输入推导。
`conditional_template`：明确外部环境、权限和替换项，链接专业说明；不能计为执行通过，也不据此声明目标部署已就绪。
正文与测试证据按 ID 关联，证据不成为使用前提。缺凭证、依赖或必需用例未执行均不能按成功收口。

## 主链路

1. 发现与编写：从通道/算子管理或分类查询确认名称与状态 → 按能力表选择兼容来源、算子和出口 →
   准备通道、插件与输入 → 参考带 ID 示例编写 SQL → 使用既有 SQL 分析结果选择入口与模式。
2. 执行与核验：提交 SQL → 记录接口返回的任务标识 → 查询状态和结果/托管实体 → 对照输入核验输出 →
   有限任务等待 EOF，持续任务按对应接口停止 → 释放示例专用资源；失败时保留任务与阶段证据再判断下一步。

## Feature Tasks

- [x] **T1：交付与现有目录和调度行为一致的能力发现与入口说明，使用户能够确认可用对象并避免执行类型误判。**
  - 覆盖核心契约中的资源分类、状态区别、请求字段和完整决策表；以运行时目录及分类断言核对示例所需对象。
  - 验收锚点：G1、G2；清单中的“已实现”“需配置”“愿景”具有明确依据，不由 active 单独推断可运行。
- [x] **T2：交付可复现的有限数据采集、分析与输出示例，使用户能从固定输入取得可核验的分析结果。**
  - E01：CSV 注册 → DataFrame → builtin.passthrough 的 USING/THEN 独立参数链 → 命名 DataFrame，核验内容与行数。
  - E02：隔离 SQLite 表的原生 WHERE/聚合 → 结果输出，明确本例的后端语义；不外推到所有通道或数据库。
  - E03：固定 PCAP → npm.basic → observing DataFrame / SQLite 托管结果，覆盖 Basic、Session、DNS 的准备与结果定位。
  - E04：独立 DataFrame/SQLite 快照 → explore.baseliner → 评估/预测及模型出口，核验桶、范围与结果类型。
  - 验收锚点：G3；NPM 与 Baseliner 分次提交，以已完成结果衔接，配置 JSON 完整可用。
- [x] **T3：交付持续来源与 Stream 编排的使用边界，使用户能选择异步入口、观察结果并正确结束任务。**
  - E05：使用现有本地受控来源验证 Stream single；E06：Group DAG 分别给出纯 stream 与 mixed 两组完整 SQL，核验各自输出。
  - 持续来源用受控 provider 验证 batch 异步准入与停止；NetAdapter、Kafka 四类适配器提供条件模板及专业说明入口，明确未验证真实设备/Broker。
  - 验收锚点：G2、G4；明确 late join 不补历史、断连重试的交付边界和新 run，不承诺任意故障恢复。
- [x] **T4：交付统一且可持续核对的文档入口，使愿景、快速开始和专业说明对当前能力的陈述一致。**
  - 愿景改用现有语法描述当前主链路；旧模型/生命周期语句等保留内容须就近标为规划，避免与可执行示例混排。
  - README 链接统一指引；既有专业说明仅同步必要交叉引用及冲突表述，详细能力继续维护在原说明。
  - 验收锚点：G5，并汇总 G1～G4 的完整清单、源码/产物身份和验证边界后收口。

按 T1 → T2 → T3 → T4 推进；每项整体验收后再勾选。Atomic Slice 的文件、命令和时间盒在工作台冻结。

## 测试锚点与验收范围

以下为待实现的测试契约，规格编写不表示已通过；断言可使用 GTest、既有 assert 测试或 Python 测试接入标准入口。

| ID | 必需断言与证明范围 |
| --- | --- |
| G1 能力发现 | 分类接口查得 fixture 通道和所需 builtin/cpp 算子；未激活状态可辨；DataFrame 未加载不伪造已知行数；不将单一列表当成全部通道。 |
| G2 入口与参数 | 实际分类结果与表一致；classify 的 sql、analyze/execute 的 sql_text 分别正确；持续来源 sync 返回 ASYNC_EXECUTION_REQUIRED；错误入口与单语句混源失败。 |
| G3 有限主链路 | 直接执行 E01～E04 的文档 SQL/请求；核验输出 Schema/实体、确定性计数或数值；测试读取实际结果，不能只检查解析或 HTTP 200。 |
| G4 编排与结束 | 直接执行 E05/E06，断言 single/group/mixed 输出；受控持续 provider 的状态查询及正常停止可完成；分号字符串不会误拆，缺必需算子或不支持的出口明确失败。 |
| G5 文档一致性 | 链接有效，ID 唯一且执行清单完整；可执行样例准备后的请求无未声明占位符，结构示意与规划内容就近标识；条件模板不冒充已执行样例，配置与日志不含秘密。 |

本地验收使用原生构建产物、临时 SQLite/CSV/PCAP、随机端口和受控持续 provider；C++ 算子加载真实 `.so`。
DNS 的协议词典、标签 provider 及其构建依赖须声明并实际可用；Baseliner 使用真实 Baseline 插件。
这些验证证明 SQL 接线和示例结果，不证明真实网卡收包、Broker 认证/交付、四库矩阵或容器部署。

新增示例验收接入 `tools/test.sh`；选择新增/修改测试与因示例依赖而需要的 Parser、Scheduler、NPM、Baseliner
共享契约回归，在实施工作台冻结具体 CMake target、测试名、filter、fixture 与理由，不默认执行仓库全量测试。
仅变更专业说明引用不要求重跑其全部矩阵；若将条件模板升级为 executable，须先补外部依赖声明与真实数据面断言。
外部测试统一使用 `config/testing/environment.json` 和 `dependencies.json`，先从实际执行位置预检，不另写账号或地址。

Feature 收口要求：T1～T4 全部验收；冻结清单 selected=executed=passed，failed=skipped=not_executed=0；
新增/修改代码满足构建及格式要求，文档与 Diff 检查通过。证据缓存在 `evidence/sql-workflow-guidance/`，不入库；
未覆盖的真实外部环境保持明确标注，不宣称产品全量或部署矩阵通过。

## 完成证据

以下按实施阶段保留历史检查点，最终状态以 T4 / Feature 完成证据为准。

- 2026-10-27：规格已编写；仅静态核查当前代码与说明。实施任务全部未开始，G1～G5 与 E01～E06 尚未执行。

- T1（2026-10-27）：统一指引的目录、语法、执行入口与结束语义已交付；真实原生 HTTP 验收及 Parser 回归通过，`evidence/test-environment/20261027T104247Z-7d04fd5b/summary.json`，selected=executed=passed=3，failed=skipped=not_executed=0；不含真实网卡或 Broker 验证。

- T2（2026-10-27）：E01～E04 原文准备脚本/请求由真实插件执行；CSV 内容、SQLite 聚合、NPM Basic/Session/DNS 的实体与计数、40 行评估/80 行预测及模型出口均核验通过。主验收 `evidence/test-environment/20261027T110032Z-482e0cf1/summary.json` 为 5/5/5，共用准备流程的 discovery 回归 `evidence/test-environment/20261027T110339Z-171d4c62/summary.json` 为 2/2/2；failed=skipped=not_executed=0。证据与源码/产物摘要本地缓存未入库；不外推外部环境覆盖。
- 用户要求完成 T2 后暂停；工作台 WIP=0，T3/T4 未开始，Feature 保持进行中。

- T3（2026-10-27 继续后）：E05 single 输出 6 行、持续 ring status/stop；E06 纯 Stream 两节点各处理 6 行并显式正常 Stop，mixed batch/stream 输出原始 3 行，均以真实文档请求核验。纯 Stream 中间持续 ring 不随上游 EOF 自动关闭的边界已实测写明；持续 block 的准入、托管结果及 Stop/Cancel 由 discovery 与受控 Capture provider 回归覆盖。C01～C05 为 NetAdapter/Kafka 条件模板，不计真实环境执行。最终标准清单 `evidence/test-environment/20261027T125056Z-95ce76e4/summary.json` selected=executed=passed=7，failed=skipped=not_executed=0；标准构建、静态检查及身份摘要通过。T4 未开始，Feature 未完成或归档。

- T4 / Feature 完成（2026-10-27）：README 与 NPM、Kafka、NetAdapter、Baseliner 说明统一链接 SQL 指引；愿景开头改为现有主链路和结构，旧 SQL、SDK、模型与生命周期设想就近标识规划。G5 标准检查核验 24 个唯一文档块、22 个请求与执行调用的完整对应、替换项、50 个链接/锚点及条件模板边界。
- 当前文档的整体验收：`evidence/test-environment/20261027T130719Z-df5174b2/summary.json`，selected=executed=passed=12，failed=skipped=not_executed=0。包括 docs/discovery/finite/stream 四项工作流检查、四项必需 fixture，以及 test_framework、test_scheduler_e2e、test_npm_dns_module、test_baseliner_operator；G1～G5 与 E01～E06 全部通过，未以历史各阶段结果代替最终复验。
- `cmake -B build src` 与冻结五个构建目标通过；Python AST、依赖 JSON、Diff 空白/范围检查通过，无 C++ 修改。源码及产物在验收前后保持一致；11 个 Feature 源码/文档和 24 个实际运行产物的 SHA256、完整清单与 G1～G5 映射记录于 `evidence/sql-workflow-guidance/feature-completion.json`，构建与验收日志同目录缓存，均不入 Git。
- T1～T4 全部完成，规格归档，Backlog 标记完成，工作台 WIP=0。仅证明隔离原生进程、真实插件、本地 CSV/SQLite/PCAP 和受控 Stream/provider；C01～C05 未执行真实设备或 Broker，未复验外部认证、数据库/容器矩阵或产品全量。不提交、推送或重载用户服务。
