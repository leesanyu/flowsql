# Feature：工作台与探索中的说明增强

- 标识：`data-semantics-consumption`；归属 [数据说明 Epic](../specs/epic-data-semantics-dictionary.md#data-semantics-consumption) 的 F5。
- 状态：2026-11-24 完成 T1～T5 并归档；优先级 P1。按用户调整先于 `llm-skill-diagnosis` 交付。
- 依赖：已交付的对象 describe、精确版本读取、算子使用依据、静态说明展示，以及 SQL 工作台和数据库/DataFrame 探索。
- 本文件冻结目标契约；公共头文件、JSON Schema/OpenAPI、调用方和可执行断言在对应实施切片同步落地。各 Task 通过验收后记录完成证据。

## Non-Goals

- 不接入模型、Skills 或自动诊断，不建设新字典管理页、存储引擎或通用 SQL 血缘系统。
- 不推导任意表达式、JOIN、子查询、窗口函数或自定义算法的业务含义，不新增 SQL 计算能力。
- 不自动转换单位、补 WHERE、去重、JOIN 或阻止用户选择聚合；说明是依据和提示，不是数据质量认证。
- 不修改 NPM 算法、领域定义或三种关系的计算口径；不补造历史运行依据，不按同名字段猜语义。
- 不保存探索成果、不扩展快照租约/跨主机数据面、不支持流式结果逐批语义传播；不引入旧格式兼容分支。

## 业务意图与首期范围

用户在写 SQL、阅读结果和配置图表时，能够直接看到字段含义、单位、NULL/有效性、时间及聚合限制，并知道说明来自哪个对象和版本。
例如读取 DNS 时延字段时能看见有效状态和单位；对 Session 累计修订求和时能查到重复累计风险，系统不替用户修改查询。

| 场景 | 首期可观察结果 |
| --- | --- |
| SQL 工作台 | 单条 DESC 同步展示通道或算子说明，不创建任务；普通 SELECT 继续使用既有执行入口 |
| 工作台查询结果 | 有限、单源查询的直接列、`*`、别名和已支持基础聚合可查看来源与本次说明；同步、异步结果一致 |
| 数据探索 | 当前来源字段、所选图表字段及计算结果可查看相同说明；支持数据库与已取得的 DataFrame 快照，内嵌/独立入口一致 |
| 来源不明确或读取失败 | 分别显示未声明、无法推导、结构变化或说明服务错误；保留已成功返回的数据与图表设置 |

结果首期覆盖 SQLite/MySQL/PostgreSQL/ClickHouse 单表查询及命名 DataFrame 的现有投影能力；聚合只在当前执行器已支持的路径消费。
算子输出已具有精确字典/使用记录时复用；无记录的任意 USING/THEN 链不重建推导规则，仍可单独 DESC 算子。
所有既有图表复用同一字段说明入口；代表性验收选柱状图、折线图、散点图、直方图及日/月分桶，无须为每种图表实现一套语义逻辑。

## 核心接口与数据契约

### 1. DESC：服务端识别、同步只读

```sql
DESC CHANNEL dataframe.samples;
DESC CHANNEL kafka.events;
DESC CHANNEL sqlite.analytics TABLE dns_results;
DESC CHANNEL postgres.analytics TABLE dns_results SCHEMA public;
DESC OPERATOR npm.basic;
```

- 文法为 `DESC CHANNEL <type>.<name> [TABLE <table> [DATABASE <database>] [SCHEMA <schema>]]` 或 `DESC OPERATOR <category>.<name>`。
  数据库通道必须提供 TABLE；DATABASE/SCHEMA 按现有 locator 与驱动契约校验，不能套用到 DataFrame/Kafka。
  Kafka 仅描述已配置的结构化 source/both；其他类型沿用对象接口的 unsupported/缺失表达，不扩大结构提供者范围。
- 关键字大小写不敏感，允许首尾空白、SQL 注释及一个末尾分号；标识符支持双引号包围与双写双引号转义。
  引号内的点、空白、分号属于名称，不能拆成新对象或新语句；不提供 DESCRIBE 等兼容别名。
- 只接受一条 DESC；多条 DESC、与 SELECT 混合、缺对象、额外尾部语法均在任何执行或写入前拒绝。
  使用服务端词法/语句解析统一识别，前端不能用正则或字符串前缀决定是否创建任务。
- 现有 SQL analyze/classify 响应增加 `command_kind: describe|execute`；describe 分支返回统一 describe 的 `target`，
  `task_kind=null`、`requires_async=false`，execute 分支继续返回真实任务分类。全部调用方同切片更新。
- 新增 `POST /api/semantics/v1/describe-sql`，输入 `{sql, timeout_ms?}`；服务端重新校验单条 DESC 并构造 target，
  复用 `IMetadataDescription::Describe`，成功/错误信封与普通 describe 一致，算子采用 usage 展示、全文按需精确读取。
  timeout_ms 沿用 5000 默认及 1～30000 范围，解析与下游共用一次总预算；不新增插件虚表。
- Web/Gateway/Router/Config 同步接线；既有任务创建、同步/异步执行、批量执行入口均拒绝 DESC，避免绕过工作台创建空任务。
- DataFrame pending 原样展示“结构待加载”；DESC 及其只读面板不调用预览/加载入口，不扫描业务行、不消费 Kafka、不保存绑定。
  既有通道管理字典页的显式查看加载行为保持其自身契约，不复用到 DESC。

### 2. 查询/图表结果的说明快照

沿用统一 describe 的 `target`、`basis`、`description`、`context` 和精确 resolve；新增的消费信封只表达结果与这些依据的关联。
下列为逻辑 Struct（对象类型直接引用现有 JSON Schema，不复制字典字段定义）；在共享响应 Schema 中统一声明：

| 字段 | 类型与语义 |
| --- | --- |
| `semantic_context.state` | `available / partial / unknown / error`；描述本次说明可用性，与查询执行状态分离 |
| `semantic_context.subject` | 工作台为实际结果/语句身份；探索为 source_id、schema_revision、可选 snapshot_id；服务端生成 |
| `semantic_context.observed_at` | 本次取得说明的时间，不冒充业务数据采集时间或历史运行时间 |
| `semantic_context.sources[]` | 本次来源的 target、basis、精确字典/作者资源/使用记录引用及所需说明内容；按来源去重 |
| `semantic_context.fields[]` | 与真实结果列对应的 output_index、output_name、state、reason、source_index、source_field_id、operation |
| `fields[].state` | `known / unknown / conflict`；来源可定位但未填写含义仍为 unknown，并保留来源 |
| `fields[].operation` | `identity / alias / aggregate / time_bucket / histogram / unknown`；携带实际函数或桶配置，不携带猜测的业务规则 |
| `semantic_context.error` | 仅 error/部分读取失败时携带结构化 code/message，保留失败阶段；不能伪装为正常未声明 |

- 结果列按位置绑定，探索字段按服务端 field_id 绑定；展示名或别名不能充当字段身份。零行结果仍按真实 Schema 显示说明。
- 在现有查询准备/快照取得过程中捕获来源身份、结构及精确说明依据，与实际结果列核对后发布；不在每一行/包路径查询元数据。
  结构不匹配标为 conflict，不把旧映射套到新列；没有可靠来源证据则 unknown，不二次运行查询来补依据。
- 字典/作者定义按精确引用读取；producer_default 保留作者精确引用和资源位置，引用缺失时不伪造独立字典版本。
  结果保留本次需要的说明投影，随现有结果记录持久化；探索说明随工作页/快照生命周期保留，不新增全局语义仓库。
- 同步结果、异步终态及历史任务详情传递同一信封。之后修改当前说明或删除/替换同名来源，不改写已捕获内容；
  已有结果没有使用依据时明确 unknown，不回填当前定义。旧图快照不因刷新来源列表而获得新说明。
- 静态说明与实际运行依据分别展示；没有 run_id/use_ref 时，禁止声称当前组件/配置产生了历史行。
  混合运行结果只展示可证明的公共定义，不取首行运行信息代表整张表。
- 说明读取按需展开且按精确引用复用，不随鼠标悬停重复下载全文；说明投影计入既有结果/快照预算，超限明确报告说明错误。
  查询本身成功而说明不可用时保留数据及 error，显式重试只能补读已经捕获的精确依据；缺失依据须重新执行/采集才可获得。

### 3. 来源映射和展示规则

| 已有操作 | 可显示的说明 | 不得推断的内容 |
| --- | --- | --- |
| 单源 `*`、直接字段、别名 | 精确源字段含义、单位、NULL/有效性、时间、行粒度；别名保持同一来源 |
| 过滤、排序、LIMIT/Top-N | 保留直接字段来源，同时保留实际过滤/结果范围 | 不把过滤视为满足全部有效条件，不把 Top-N 当全量 |
| count(*)、count、distinctCount | 实际计数对象、NULL/去重规则和计算口径；结果单位“个” | 不继承被计数字段的物理单位 |
| sum/mean/min/max | 实际聚合函数、来源定义及已声明的聚合限制 | 未有明确规则时，派生业务含义/单位为未知；不宣称累计修订可相加 |
| 日/月时间桶、直方图 | 来源时间解释/单位、实际桶设置、源库日历或快照时间语义，频数为计数 | 不猜整数 epoch 单位，不自动转换时区或输出新的领域定义 |
| 任意表达式、多源或无来源输出 | 显示真实列名/类型以及无法推导原因 | 不按别名、字段同名或 SQL 文本相似程度套用说明 |

- 工作台 DESC 复用已有通道/算子阅读组件，采用只读模式；结果表列头提供“说明”，不把 DESC 塞进 rows/columns。
- 探索提供当前来源字段面板、所选轴/分组/过滤/聚合字段的说明入口及图表口径说明；简洁/高级模式使用相同依据。
  显示完整的已声明限制，长内容按需展开，支持键盘聚焦/点击；说明文本按文本渲染，不执行其中的 HTML/脚本。
- 当前对象可导航到原管理页；结果依据可打开捕获的精确版本，明确区分“当前对象”和“本次结果依据”。
  独立探索没有父页面时也可阅读，不能依赖 Vue 组件实例或父页内存；可用 `web_url` 指定当前管理页的 http(s) origin，仅用于导航，不作为说明/计算上游。
  未配置或地址无效时展示当前对象定位信息，不生成指向探索端的管理链接。
- 探索服务通过已配置的控制面 HTTP 读取说明，浏览器只提交 source_id/schema_revision/snapshot_id，服务端解析 locator；
  不接受浏览器提供任意上游地址。必要的新 URI、代理白名单、配置和交付说明同切片同步。
- 切页、切源、重新执行/采集时以实际身份和请求代次隔离；旧响应不覆盖新页。仅打开说明不得执行 compute、acquire、预览或写入。

## 两条主链路

1. **命令阅读：** 工作台提交文本 → 服务端 analyze 分类 → 单条 DESC 走 describe-sql → 统一 describe → 原只读阅读组件。
   SELECT 走既有执行入口；混合输入在分派前失败；对象结构 pending 保持 pending，用户可自行转到管理页处理。
2. **数据消费：** 用户执行有限查询/取得探索快照 → 捕获本次来源与精确说明 → 按真实输出建立映射 → 返回数据与 semantic_context →
   按需阅读字段和图表口径。再次执行/采集建立新上下文；历史结果与旧快照继续展示其自身依据，未记录的部分保持未知。

## Feature Tasks

依次实施 T1→T5。每个 Task 可跨多个 Atomic Slice；每轮工作台冻结允许文件、具体测试与停止条件，按项目规则 §1.4 的用户授权范围连续推进或停止。

- [x] **T1：交付同步只读的 DESC 服务端入口，使工作台和普通 HTTP 客户端能取得同源对象说明且不会误建执行任务。**
  验收 A1/A2：完成语法、分类、路由及 Schema，普通执行/混合输入边界、预算和错误映射有自动化断言。
- [x] **T2：交付绑定实际结果和精确版本的字段说明上下文，使直接查询与异步历史结果可追溯且未知转换不会冒用原语义。**
  验收 A3/A4/A5：同步/异步、数据库与 DataFrame、零行、别名/聚合/未知、修改说明和来源替换后重读均有断言。
- [x] **T3：交付工作台中的 DESC 阅读和结果列说明，使用户在原 SQL 流程就地查阅来源并区分当前对象与历史依据。**
  验收 A2/A3/A6 的工作台范围；真实浏览器验证通道/算子阅读、字段入口、失败重试、导航和普通执行回归。
- [x] **T4：交付探索字段与图表的同源说明，使内嵌和独立工作区都能解释所选字段、统计口径及快照依据。**
  验收 A3/A4/A5/A6 的探索范围；数据库与 DataFrame、简洁/高级模式、代表图表及切页/快照更新一致。
- [x] **T5：交付完整场景验收与当前使用说明，使用户可依据文档完成 DESC、结果和图表阅读，并明确已验证范围。**
  验收 A1～A7；同步数据说明、SQL 工作流和探索文档，汇总最终实际执行清单后才完成 Feature/归档。

## 验收锚点与验证范围

以下断言须在对应实现前进入 GTest、Node 或 HTTP/Chromium 用例；本规格的文字清单不代替可执行测试。

| 编号 | 必须断言的结果 |
| --- | --- |
| A1 分类与契约 | 示例目标转换准确；大小写/注释/引号分号正确；多句/混合/缺参/尾部拒绝；未创建任务；DESC 与 describe 内容/错误一致 |
| A2 只读边界 | DESC/展开说明前后任务数、字典/绑定/使用记录数不变；pending CSV 无文件读取、Kafka 无消费、数据库无业务行扫描；超时有界 |
| A3 映射正确 | `*`、投影、别名、零行及重复显示名按实际列对应；count 单位不误继承；聚合/分桶显示真实操作；任意表达式/多源保留 unknown |
| A4 历史与身份 | 保存新说明后旧结果保持旧依据、新执行取得新依据；异步及任务重启重读一致；同名替换、结构变化、producer_default、无运行记录不误标 |
| A5 快照与错误 | 旧 DataFrame 快照不串新说明；新采集获得新上下文；缺失/服务失败/结构冲突分开表达；失败保留已成功数据；切页旧响应不覆盖 |
| A6 真实页面 | 工作台及探索内嵌/独立可读、可键盘操作；简洁/高级模式一致；单位/NULL/有效性/限制和精确依据可见；正文脚本不执行；打开说明不触发数据读取 |
| A7 完整交付 | 新增测试、受影响共享契约回归、四库及 DataFrame 场景实际通过；文档示例可执行；失败/环境跳过/未执行为零 |

- 验收环境：native、隔离 SQLite/CSV/插件宿主及真实 Chromium；四库对象定位与列映射分别验证，统一配置来自
  `config/testing/environment.json`、依赖/fixture/资源锁来自 `config/testing/dependencies.json`。涉及外部库须从实际进程位置先认证并验证必要权限。
- 四库矩阵的依据是本 Feature 明确支持其 locator、实际结果列和探索来源映射；不重跑 NPM 报文分析全矩阵、Kafka 数据面或容器交付矩阵。
  NPM 场景复用已验收定义和隔离受管结果夹具，至少覆盖 DNS 有效性、Session 累计修订警示、无历史运行依据三个语义案例。
- 实施时将新增用例注册标准 runner；工作台冻结精确名称和完整筛选，使用 `tools/test.sh unit|doctor|integration --filter ... --location native`。
  现有回归候选包括 `test_framework`、`test_task`、`test_metadata_description_api`、`test_metadata_reading_frontend`、
  `test_visualization_workflow`、`test_visualization_client`、`test_visualization_dataframe_workflow`、`test_visualization_dataframe_browser`、
  `test_visualization_experience`、`test_visualization_statistics` 和 `test_visualization_connectors_e2e`；按实际调用链取必需清单，不能漏掉声明能力。
- CMake 入口固定 `cmake -B build src`，再构建当轮受影响目标（含 `flowsql_frontend`/`flowsql_exploration` 及测试宿主）；
  具体目标、过滤表达式、夹具及权限在实现前写入工作台，不在规格中堆积实现步骤。不默认执行仓库全量测试。
- 完成出口：冻结目标零编译错误、必需清单全绿、修改 C++ 符合 clang-format、相关语法/Lint 检查无报警、一轮有界 Diff 审查。
  运行日志、实际清单、源码/产物摘要放入忽略缓存 `evidence/data-semantics-consumption/`，不提交；未运行范围明确标为未验证。

## 完成证据

2026-11-24 完成全部 Task；按切片实际执行的 27 项不同功能用例最终全部通过，对应环境夹具已执行，最终失败/跳过/未执行均零。

- T1：共享解析、describe-sql HTTP、分类及全部执行入口拒绝通过；有效记录 `20261124T040539Z-437974a2`、`20261124T041505Z-1910fc03`。
- T2：真实四库与 DataFrame 的位置映射、零行、别名/聚合/未知、异步保存和重启、说明更新与同名替换后历史依据通过；四库查询最终记录 `20261124T060544Z-a1a9102c`，共享框架/ClickHouse 回归见本地 T2 工作台。
- T3/T4：真实 Chromium 验证工作台与探索独立/内嵌、简洁/高级、键盘与文本安全、只读边界、所选字段/图表及快照；`20261124T055356Z-3e843212` 为 14/14，后续变更对应回归及原管理页验证通过。
- T5：四库探索矩阵及 NPM DNS/Session 静态依据、无历史运行记录、精度和布局回归通过；三项失败修复后 `20261124T061122Z-547f920f` 为 8/8。独立管理导航及无效地址检查通过；最后上下文与静态包真实热升级 `20261124T062955Z-ab99f30a` 为 4/4。
- CMake 受影响目标、Vue/React 构建、相关语法检查、改动行及新增 C++ 文件格式检查、`git diff --check` 与一轮有界 Diff 审查通过。三份默认配置通过解析，native 分发保持现有 Worker 命令替换；使用说明已同步。

完整用例→最终通过 run_id、有效运行的源码/产物摘要和早期失败修复依据保存在 `evidence/data-semantics-consumption/final-acceptance.md`、`final-test-matrix.json` 及 T1～T5 工作台，均为未入库本地缓存。
未验证仓库全量、Kafka 数据面、NPM 全后端算法矩阵及容器镜像/部署矩阵；不据本 Feature 的验收声明这些范围已通过。

## 实现依据

- [统一对象接口](../../src/framework/interfaces/imetadata_description.h)、[说明 API Schema](../../config/semantics/api.schema.json)、[说明使用文档](../../docs/data-semantics.md)。
- [SQL 解析公共头](../../src/framework/core/sql_parser.h)、[工作台页面](../../src/frontend/src/views/Tasks.vue)、[SQL 工作流](../../docs/sql-workflow.md)。
- [探索服务](../../src/services/visualization/exploration_service.cpp)、[探索页面](../../src/exploration/src/main.jsx)、[探索说明](../../docs/exploration.md)、[测试规范](../../docs/testing.md)。
