# Feature：NPM 数据与算子说明（npm-data-semantics）

状态：`[x]` T1～T7 均已验收，Feature 完成并归档；已有受管对象可直接读取静态字典，真实运行依据单独追溯。覆盖 SQLite、MySQL、PostgreSQL、ClickHouse、通用结构扩展及既有页面。优先级：P1；立项日期：2026-11-02；完成日期：2026-11-03。
所属 [Epic](../specs/epic-data-semantics-dictionary.md#npm-data-semantics)：F4；依赖已完成的 F1～F3、对象说明管理及通道/算子说明精简等七个 Feature。
本规格冻结“NPM 默认领域内容 + 有限结果接入”，优先复用已有模型和机制；完成状态及证据见下文。

## 业务意图与 Non-Goals

让查询现有 NPM 结果的用户及未来诊断程序，从具体结果对象取得与实际产出一致的字段和必要上下文，明确一行代表什么、数值何时有效、如何选择时间/方向及统计/关联结果。
系统提供经生产实现核对的默认领域说明与精确依据，减少把缺失当零、终态当成功、累计修订重复相加和跨运行错误关联；用户无需自行查源码或重新填写产品已知含义。
首阶段交付 DNS 必要说明和真实结果闭环；整个 Feature 仍须完成下文十二个实体及共同验收。

- 产品处于初始构建阶段，不要求与当前代码兼容；不承担旧 API/ABI、参数形式、序列化或存储格式的兼容，不设计旧版适配层、双轨实现、降级回退或跨版本部署。
- 不建设第二套语义模型或存储；现有字典/契约能力足以表达本次内容。必要的接入契约调整须先冻结，并同步调用方、Schema、测试与文档，不因兼容当前代码保留旧入口。
- 不重建标准目录、NPM 专用编辑页、规则语言或自动 SQL 生成；沿用现有对象入口、字段表及作者/本地说明分离。
- 不扩展协议指标、分析算法或业务统计口径；不重构 Baseliner 或为通用模块增加 NPM 名称特判。这是业务范围约束，不是保持当前代码不变的要求。
- 不执行单位转换、累计差分、自动去重/JOIN、外键或样本质量校验；有效条件和聚合限制仍为描述性声明。
- 不接入模型、MCP、Skills 或诊断编排，不实现 DESC、图表提示或任意 SQL 投影/聚合后的来源传播。
- 不纳入实时采集、Kafka 报文、独立 DataFrame 输出、容器部署矩阵或全产品回归；不迁移/回填既有历史运行依据。数据库范围为 SQLite、MySQL、PostgreSQL、ClickHouse，说明机制不以数据库类型枚举限制未来扩展。
- 不重建全部历史运行配置、跨任务来源图或全库自动识别；无可靠依据的对象不得按名称、字段相似或当前最新说明认定来源。

## 已有交付与待接入边界

| 既有能力 | 本 Feature 的复用与新增责任 |
| --- | --- |
| F1 标准/字典/绑定、不可变版本和历史 | 复用存取、校验、引用和冲突机制，生产 NPM 内容，不新增共享标准配置前置 |
| F2 真实结构、对象身份及字段映射 | 按具体托管关系观察结构并绑定，不把数据库连接当数据集 |
| F3 作者契约、配置适配、使用记录和输出观察 | 复用既有接口，补齐 NPM 配置快照及有限托管结果关联，不重做通用检查/推导器 |
| 对象说明管理、通道精简和默认初始化 | 使用同一 describe/保存接口与既有页面，保留人工补充和精确关联，不新增常规用户填写项 |
| 算子说明精简的最终 R8/R9 | 已交付 npm.basic 作者契约、调用说明、十二实体生产结构及总体限制；本次补领域内容与实际结果关联，不将现有 ABI 数量或参数集合冻结为兼容承诺 |

立项时 NPM 尚未接入既有配置适配及托管结果说明；本 Feature 已补齐生产同源适配和逐对象说明发布，`structure_ref` 继续表达生产结构。
通用分支选择使用有效 `normalized_config`；缺少来源能力时保持未确定。NPM 托管目录与通用字典/绑定分别记录，已有目录不等于说明已发布。

## 冻结交付范围

### 实体和必要说明

以下每行同时包含现有 IPv4/IPv6 实体；字段名、顺序、物理类型、可空性和 Schema 版本取生产 descriptor，不手抄第二套结构。

| 实体 | 必须解释的领域边界 |
| --- | --- |
| `dns_transaction_ipv4` / `dns_transaction_ipv6` | 事务身份、五类 outcome、请求/响应方向及捕获时间、重试、RCODE/TC、时延有效性、单边/截断/期限/会话结束原因及观察范围 |
| `basic_ipv4` / `basic_ipv6` | 会话累计、周期增量、UTC epoch 半开周期、安全水位、period_complete 与终态、final 模式 NULL、零增量终态及标签/识别状态 |
| `session_ipv4` / `session_ipv6` | 会话累计平均速率、TCP 握手/RTT/重传、适用状态与 measurement_flags、UDP 不适用、NULL 与无样本的区别 |
| `http1_transaction_ipv4` / `http1_transaction_ipv6` | 明文 HTTP/1 事务与流水线关联、状态码与 matched 的区别、时延、单边和消息/流不完整原因，不宣称 HTTPS/HTTP2/HTTP3 能力 |
| `tls_handshake_ipv4` / `tls_handshake_ipv6` | 可见初始协商、方向/阶段/状态及时间、Hello/明文 alert 和不完整原因；ServerHello 不等于完整握手成功 |
| `icmp_event_ipv4` / `icmp_event_ipv6` | Echo 与控制差错、状态/时间/类型码、引用报文及可缺失信息，不强制归入 TCP/UDP 会话 |

- 全部选中字段有对应含义；必要单位、时间、枚举、NULL/有效性、统计限制和实体行语义必须明确，无依据项保留未知。
- A/B 是规范化端点，不固定为客户端/服务端；IPv4 数值与 IPv6 十六字节表示保持真实区别，不以展示文本替代物理类型。
- 结构和行为以生产实现/测试为准，现有文档为整理入口；发现有证据的说明错误时修正本 Feature 内容，不借此修改算法。

### 唯一生产闭环及阶段上下文

- 主链路为有限 PCAP/PCAPNG → `npm.basic` → 具备托管结果及结构描述能力的数据库通道 → 明确 history/latest/final 关系的对象说明；六类实体复用同一路径，四个既有数据库提供者均须验收。
- 覆盖每个实体的三个公开关系及 `__npm_run_id`、`__npm_task_id`、`__npm_run_status`；内部 data 表和 runs/entities 管理表提供各自静态说明，但不把内部存储表伪装成公开分析视图。
- 运行目录提供本链路必要的 `run_id`、`task_id`、`input_namespace`、`status` 含义与关联；其余管理字段提供静态定义和真实结构，不扩展生命周期管理。
- DNS 阶段只依赖 DNS 两族、上述运行上下文、事件修订和视图规则；不等待完整 Basic/Session 领域说明，也不新增尚无使用场景的关联。
- 后续关联限于 DNS/HTTP1/TLS→同族 Session、Session→同族 Basic 及公开关系→运行目录；缺少目标或身份依据时明确不可确认。
- 关联须限定同一运行、实体/IP 族和适用身份，结合来源命名空间/观测域解释；Basic 周期修订不能被当作一对一会话表。
- 协议事件固定 revision=1、is_final=true；三种视图可含同一事件。latest 为每次运行内每实例最新修订，不表示最近一次运行。

## 核心数据与接口契约

### 模型复用与目标表达

| 数据 | 冻结表达与边界 |
| --- | --- |
| 领域字典 | 既有 `dictionary` 的 `entity` 与 `fields`；行含义/类型、身份/修订/时间/关系/限制及字段 definition/role/measure_kind/unit/time_semantics/null_meaning/validity/aggregation/value_domain；不新增 NPM 专用属性 |
| 生产结构 | 沿用 `datasetSnapshot`、`object_key`、source_revision/schema_fingerprint；分析实体保留 Arrow 事实，数据库关系使用实际原生类型和可空性，不能直接复制 Arrow 物理声明 |
| 作者契约 | 现有 `operator_contract` 及本地 resources/variants/description/limitations/examples；领域正文与已发布契约同源，不把本地 notes 作为作者执行规则 |
| 结果绑定 | 既有通道绑定、精确 dictionary_ref、field_map、source_snapshot 和核对依据；绑定具体公开关系，不依据表名模式猜测 |
| 运行依据 | 既有 `operator_use` 的 component_revision/contract_ref/effective_parameters/normalized_config/config_refs/execution_context；区分通用使用记录 run_id 与 NPM 托管 run_id |
| 说明读取 | 既有 channel describe 与 operator usage/full/精确 resolve；字段和表级限制直接可取，复杂内容按精确引用读取，不要求客户端从长篇算子说明猜字段含义 |

- `validity`、`aggregation.condition` 使用已有 descriptive 条件及 field_ids；`relations` 仅声明精确目标字典与联合字段映射，不承诺唯一性、基数、范围连接或可执行判断。
- `is_final`、run_status、协议 outcome 分别解释生命周期、运行状态和协议观察；保存说明、结构匹配及 metadata_status=known 均不证明业务语义正确。
- 字段裁剪或必要依据缺失时保持既有未知/缺口语义，不把字段不存在、数值 NULL 或历史未记录解释为零、成功或不适用。

### 接入职责与契约冻结

以下接口列出已实现的职责入口，供实施复用，不承诺现有签名或 ABI 不变；必要调整在修改前冻结目标契约，同步全部受影响调用方及测试，内部类型和允许文件在对应工作台冻结。

| 既有接口 | 本 Feature 的使用方式 |
| --- | --- |
| `IOperatorConfigAdapterV1::ResolveInvocation(request_json, result_json, error)` | NPM 复用生产解析器，返回可确定的 effective_parameters/normalized_config/config_refs/inputs/execution_context；供既有检查与实际任务使用同一有效配置依据 |
| `IChannelDatasetDescriptorV1::Describe(request, snapshot, error)` | 只读取得明确数据库关系结构与身份，不读取分析行或自动创建说明 |
| `IOperatorDataContractV1::CheckAndDerive(...)` / `RecordUse(...)` | 沿用检查和可信执行记录；只记录真实观察到的输出，不将静态 Schema 当作实际产出 |
| `IMetadataDescriptionV1::Describe/SaveChannel/SaveOperator` | 普通客户端和页面共用既有读写信封、basis、版本冲突和部分成功处理 |
| 既有 save/import/apply/resolve 控制操作 | 静态定义随作者契约登记；明确保存或业务执行时关联真实对象；跨进程走 HTTP，本进程按 IID 查找，不依赖 Config 具体类或直接写其数据库 |

- 配置适配不运行算法、不扫描报文、不另写默认值；依赖来源能力而尚不可确定的配置保持未确定，实际来源准备后按生产事实核对。
- 输出选择采用单一目标契约：observing 选择 `foreground`，`managed_results` 表达有条件的不透明集合，不为十二实体新增执行端口或伪装成一张表；该选择基于表达需要，不用于维持旧 ABI。
- 各托管关系通过现有通道字典机制分别发布/绑定；集合的结构观察与逐对象说明关联分别报告，不伪造集合端口 matched。
- 使用记录的现有 execution_context 中保存 NPM 运行与实体/关系关联：`npm={run_id,input_namespace,entities:[{entity_id,schema_version,relations:[{view,locator,dictionary_ref,binding_ref}]}]}`。
- 该内容属于既有上下文对象中的领域数据，不新增顶层信封；view 仅 history/latest/final，locator 沿用 F2，引用必须是本次真实成功发布的精确版本。
- 运行特有参数和版本留在对应使用记录；共享关系的默认说明不能声称全部历史行使用当前配置。按明确 run_id 解释结果，缺少该运行记录时保留未知。

### 发布、更新和历史边界

- 静态领域字典随作者契约登记，读取既有对象时由受管目录证明来源并核验真实结构后直接展示；空表无需新执行或绑定。describe 与列表不写元数据。显式保存或真实托管执行才创建对象字典版本/绑定，执行完成时记录运行依据。
- 首次查看自动取得产品静态默认说明；幂等重试不产生无意义版本，关系名称和结构必须来自本次受管目录/真实观察，不要求用户先建标准或填写内部身份。
- 版本引用由现有服务生成并解析；作者组件更新和内容变化使用新精确依据，不硬编码部署中的字典 revision，不改写已保存使用记录。
- 已有人工补充不被默认初始化覆盖；结构/来源变化走既有核对和冲突行为，不静默覆盖绑定、自动套用其他对象说明或更改业务表。
- 元数据失败如实报告：尚未提交数据时失败；已有数据提交时保留数据并沿用 data_state/metadata_state/error 表达，不能伪装整体成功或数据已回滚。
- 服务不可用、来源不符和引用缺失保持可定位错误；不通过关闭检查、吞错或绑定最新版本收口。正常结束、失败、取消沿用现有语义，不伪造终态。
- 不扫描全库或补造旧运行记录；选中的既有受管对象可直接取得静态说明。缺少旧运行参数仍为未知，静态默认不证明历史行使用当前组件。精确引用与用户补充在重启后可读。
- 精确版本、人工补充保护和运行记录用于本 Feature 的持续使用与结果追溯，不构成跨软件版本的数据格式兼容承诺。

## 两条主链路

1. **静态读取与生产关联**：作者契约加载即提供全部静态定义；选定既有对象 → 受管目录确认实体/关系/Schema → 通过通用默认字典机制只读展示。真实运行另行沿用：明确有限来源、模块/标签配置和数据库托管目标 → 生产解析器冻结配置及组件依据 → 真实对象创建/核验 → 发布默认字典并绑定公开关系 → 沿原数据面分析/入库 → 保存对应运行/关系精确依据；失败保留实际数据与元数据状态。
2. **查询与解释**：从数据库浏览器选择具体对象即可 describe；按需沿默认来源的作者契约精确引用读取定义。有实际任务时再取得 run_id/实体/公开关系和运行依据 → 经原查询入口按明确运行读取代表结果 → 结合状态、NULL、时间、方向和修订规则解释；阅读无写入，用户补充经既有保存入口独立提交。

## Feature Tasks

按 T1→T5 顺序实施；完整验收当前 Task 后才勾选。每个 Atomic Slice 单独冻结允许文件/命令/时间盒，验收后关闭当前切片；按用户 2026-11-02 的连续实施授权更新工作台后依次进入下一切片，直至 T5 完成。

- [x] **T1：交付可校验的 DNS 领域说明基线与同源有效配置，使 NPM 的说明发布和输出选择建立在生产事实及统一目标契约上。**
  验收：A1/A2 的 DNS 与公共部分；冻结十二实体清单、来源映射和目标契约，DNS 字典/作者契约可校验，配置适配与生产解析一致；不以保留旧入口或行为作为验收条件，未交付实体明确待办。
- [x] **T2：交付 DNS 两族在 SQLite 托管结果中的完整说明闭环，使普通程序可解释事务结果并取得必要运行依据，成为首期诊断的可独立使用输入。**
  验收：A3/A4/A7/A8/A9 的 DNS 范围，有限真实报文、三种公开关系、运行上下文、保存/重读/冲突/重启及只读访问全部通过；不等待完整 Basic/Session 或其他协议。
- [x] **T3：交付 Basic/Session 两族的默认领域说明与必要会话关联，使累计、周期和性能结果可正确统计且不会混淆修订与有效性。**
  验收：A1/A3/A5/A7/A8 的本阶段范围；沿同一 SQLite 路径完成实际结果解释，关联已有 DNS 时不改写其验收依据。
- [x] **T4：交付 HTTP1/TLS/ICMP 两族的默认领域说明及范围内关联，使其状态和观察限制能通过同一对象接口准确读取。**
  验收：A1/A3/A6/A7/A8 的本阶段范围；不增加协议指标或独立端口，不把 ICMP 引用信息强制关联为会话。
- [x] **T5：完成十二实体的统一验收、受影响回归和使用说明，使领域定义、真实结果及程序读取在冻结范围内形成完整且可追溯的交付。**
  验收：收口 A1～A9，既有页面读取/编辑与普通客户端一致，运行与说明版本不串用；完整 Feature 验收通过后才归档并标记完成。
- [x] **T6：交付生产数据库的通用说明发布，使 MySQL/PostgreSQL/ClickHouse 实际 NPM 结果可通过既有对象入口读取默认字典，未来数据库无需修改说明业务逻辑即可接入。**
  验收：三个生产后端的十二实体、37对象、原生字段/可空性、精确关联、重复运行、人工补充与重启；PostgreSQL公开视图可发现。发布器只依赖实际目录和既有结构描述接口，公共locator类型接受非空无NUL字符串，不含数据库产品白名单；扩展类型须有可执行断言。保留SQLite回归及A1～A9保证，不以SQLite单阶段成绩声明生产能力完成。

- [x] **T7：静态字典随作者契约提供，使已有受管 NPM 表及视图无需重新执行分析即可读取默认说明，运行依据仍保持真实执行边界。**
  验收：空对象及既有对象无新任务时可读完整静态字段说明；目录证明/真实原生结构、只读、人工保存、重启、三生产后端与通用类型扩展、原执行发布回归。

T2 的 DNS 必要说明已具备后续 llm-skill-diagnosis 的数据前提；该能力由后续独立授权实施，不纳入本 Feature。

## 验收锚点与依赖边界

| 编号 | 必须转成可执行断言的结果 |
| --- | --- |
| A1 内容与结构 | 各阶段实体/字段清单无遗漏，定义符合冻结的目标 Schema；逐项核对生产 Arrow descriptor、各后端公开关系原生类型/可空性及必要语义。字段与实体声明不由名称猜测，不复制 Arrow 类型冒充数据库结构 |
| A2 配置与契约 | 按冻结后的目标配置/执行契约核对全部调用入口；适配与生产解析的有效值/错误一致，参数类型、模块/标签依赖和 observing 分支有断言；未确定来源能力不冒充确认。契约调整同步调用方/Schema/文档，不验收旧 ABI、旧参数形式或新旧混用 |
| A3 实际关联 | 本次实际对象/关系取得精确字典/绑定；run_id、组件/契约、有效参数与上下文关联可精确重读。未启用、未观察和已发布分开；对象名称/相似结构不构成可信来源，集合端口不冒充逐实体观察 |
| A4 DNS | 真实 UDP/TCP、IPv4/IPv6 代表结果覆盖五类 outcome；matched+非零 RCODE、query_only 及原因、单边 NULL、可靠/不可靠时延、方向、query_retries、DNS TC 与采集截断区别有独立预期；三视图同一事件不重复计数 |
| A5 Basic/Session | 至少两个周期的累计/增量、尾周期/零增量终态、final 模式 NULL、period_complete/is_final 区别；累计平均速率与 RTT/握手/重传状态、no_sample/ambiguous/UDP 不适用按独立预期解释；连接计数不乘以周期修订 |
| A6 其他协议 | 每族覆盖 HTTP matched+非成功状态及单边、TLS 可见 ServerHello 与不完整、ICMP Echo/控制差错及缺失引用；状态/NULL/时间与说明相符，不要求所有协议×状态×来源的笛卡尔积 |
| A7 身份与视图 | 重复 task 的不同 run 不串用；latest 不等于最近运行，final 不等于成功，history 修订不能直接相加；范围内联合关联包含必要运行/实体身份并明确多修订限制，缺少目标/历史依据保持未知 |
| A8 保存与故障 | 本 Feature 生成的说明初始化幂等，人工补充和精确版本保留；组件/来源变化核对，元数据失败与已提交数据区分；服务/引用错误非零且可定位，同一目标契约下重启重读正确，取消/失败不伪造正常终态；不纳入旧库迁移或跨版本恢复 |
| A9 消费与无副作用 | HTTP/IID 与现有页面使用同一内容；从对象获得必要字段/表级上下文并解释代表数据，按需精确读取；describe 不运行算子、读取分析行、注册/改写定义或绑定。页面沿用原编辑布局，保存本地文字不改变作者机器契约 |

- 以独立预期核对代表结果和说明，不用被测说明生成预期；Schema/结构匹配、内容条数和预检通过均不能替代真实结果验收。
- 每阶段只运行所选实体的 Schema/内容/生产解析断言、原模块相关结果用例、真实有限调度→数据库→说明链路及受影响的共享字典/契约/描述回归；最终收口十二实体，不默认全仓或完整算法复验。
- 必要 fixture 为隔离 PCAP/PCAPNG、隔离 SQLite/三生产后端专用测试库、真实 NPM/NPI 插件及 protocols/精确标签配置、Config/调度宿主、回环 HTTP；现有页面回归另需 Chromium，普通客户端不依赖模型。
- 实施工作台冻结实际 CMake targets、标准 runner filter、选中清单、native 执行位置、配置来源及资源锁；依赖沿用统一测试配置，新增用例同切片登记，外部数据库限上述三后端，不扩展 Kafka/镜像矩阵。
- 必需清单 selected=executed=passed、failed=skipped=not_executed=0；共享接口、格式、Schema 和文档检查通过。说明 docs 以现有 NPM/数据说明为主，原始证据放 evidence/npm-data-semantics/，不入库。

## 完成证据

T1～T5 已分别完成阶段及统一验收，A1～A9 在冻结范围内通过；构建产物和隔离宿主验收不代表常驻服务已加载新能力。

### 规格阶段记录

2026-11-02 已完成本规格及 Backlog/Epic 责任同步；单轮边界审查、公共模型/接口依据、任务/锚点、文档链接、表格及范围校验通过。
本地文档证据位于 `evidence/npm-data-semantics/specification/`（不入库）；T1～T5 保持待办，未执行产品构建或功能测试。
2026-11-02 按初始构建阶段要求修订：取消当前代码、旧 API/ABI、参数及存储格式兼容约束；必要契约调整同步调用方，不增加迁移/双轨实现，业务范围和 T1～T5 不变。本轮文档证据位于 `evidence/npm-data-semantics/specification-no-compatibility/`（不入库）。

### T1 内容与来源清单

十二实体的物理结构统一来自 [NpmFamilyEntityDescriptor](../../src/operators/npm_basic/npm_family_result_contract.cpp)，当前 Schema 版本均为1；下表列数为单族分析列，不包含 SQLite 附加列。
领域内容从下列实现及 [NPM 字段说明](../../docs/npm.md#完整分析与入库字段字典) 核对，组装后通过实际 descriptor 和公共字典校验器审计，不手抄第二套物理结构。

| 模块/实体（IPv4、IPv6） | 单族字段数 | 行为与有效性来源 | 领域内容交付 |
| --- | ---: | --- | --- |
| basic_ipv4 / basic_ipv6 | 31 | [累计投影](../../src/operators/npm_basic/modules/basic/npm_basic_result_projector.cpp)、[周期统计](../../src/operators/npm_basic/modules/basic/npm_basic_periodic_stats.cpp) | T3 已验收 |
| session_ipv4 / session_ipv6 | 49 | [TCP 性能](../../src/operators/npm_basic/modules/session/npm_tcp_performance_tracker.cpp)、[结果编码](../../src/operators/npm_basic/output/npm_session_result_encoder.cpp) | T3 已验收 |
| dns_transaction_ipv4 / dns_transaction_ipv6 | 25 | [事务关联](../../src/operators/npm_basic/modules/dns/npm_dns_udp.cpp)、[TCP 时间/缺口](../../src/operators/npm_basic/modules/dns/npm_dns_tcp.cpp)、[结果编码](../../src/operators/npm_basic/modules/dns/npm_dns_result_encoder.cpp) | T1/T2 已验收 |
| http1_transaction_ipv4 / http1_transaction_ipv6 | 22 | [事务关联](../../src/operators/npm_basic/modules/http1/npm_http1_transactions.cpp)、[结果编码](../../src/operators/npm_basic/modules/http1/npm_http1_result_encoder.cpp) | T4 已验收 |
| tls_handshake_ipv4 / tls_handshake_ipv6 | 25 | [协商观察](../../src/operators/npm_basic/modules/tls/npm_tls_handshake.cpp)、[结果编码](../../src/operators/npm_basic/modules/tls/npm_tls_result_encoder.cpp) | T4 已验收 |
| icmp_event_ipv4 / icmp_event_ipv6 | 27 | [Echo 关联](../../src/operators/npm_basic/modules/icmp/npm_icmp_echo.cpp)、[结果编码](../../src/operators/npm_basic/modules/icmp/npm_icmp_result_encoder.cpp) | T4 已验收 |

十二实体内容在作者契约 resources.<entity>.dictionary 中表达，字段物理声明由生产 descriptor 组装；managed_results 的内容状态仅表示领域正文是否就绪，不表示已发布、已绑定或已观察到实际结果。
同源有效配置与可选配置适配由 T1 / Atomic Slice 2 交付；SQLite 对象说明闭环由 T2 交付。

2026-11-02 本切片验收：新增断言先检出缺失内容；真实 NPM .so 生成两族完整字典，经公共字典/作者契约校验与 usage 投影检查通过。五类 outcome、方向、时间/NULL、时延有效性、RCODE/TC、原因枚举和身份/统计限制有断言，四项既有 DNS 回归提供独立行为预期。
已构建主契约及 DNS UDP/TCP/编码/模块目标；标准入口分两次执行五个功能用例和一个必要 fixture，合计 selected=executed=passed=6，failed=skipped=not_executed=0。源码/产物在验收期间一致，格式、空白、链接和范围检查通过。
本地证据为 `evidence/npm-data-semantics/t1-dns-baseline/` 及其中引用的两轮标准 runner 证据（均不入库）；未改算法、公共 Schema 或运行配置，未部署、重启、提交或推送；工作台 WIP=0，停止于本切片。

### 实施依据与早期检查点

T2 关系发布早期检查点：真实 SQLite 运行目录及 DNS 两族三视图默认说明、原生类型/可空性、精确绑定、幂等、人工补充保护、只读、来源错误及实际插件重启重读已通过。新增集成与 fixture 2/2、既有 SQLite 字典回归 1/1，失败/跳过/未执行均零。本地证据 evidence/npm-data-semantics/t2-relation-publication/ 不入库；当时不勾选 T2，完整闭环证据见下文。

T2 任务接入早期检查点：实际 NPM task 在可信执行检查中冻结精确组件，SQLite 对象创建后发布说明；成功 Open 后同源返回实际配置、有限来源及运行/关系引用，经通用观察保存 operator_use。真实任务说明与通用 block（含 Baseliner）回归及夹具 4/4，失败/跳过/未执行均零，源码与产物一致。证据 evidence/npm-data-semantics/t2-task-lifecycle/ 不入库；当时不勾选 T2，完整有限报文验收见下文。

- [七个已完成 Feature 与目标](../product_backlog.md)、[Epic F4](../specs/epic-data-semantics-dictionary.md#npm-data-semantics)、[NPM 说明](../../docs/npm.md)、[通道与算子说明](../../docs/data-semantics.md)。
- [算子说明最终交付](../archive/feat-operator-description-refinement.md#用户验收修正-r8简洁呈现并填写-npm--baseliner-契约)、[通用算子契约](../archive/feat-operator-data-contract.md)。
- [公共文档 Schema](../../config/semantics/documents.schema.json)、[API Schema](../../config/semantics/api.schema.json)、[OpenAPI](../../config/semantics/openapi.json)、[NPM 作者契约](../../src/operators/npm_basic/npm_data_contract.json)。
- [配置/数据契约接口](../../src/framework/interfaces/ioperator_data_contract.h)、[对象说明接口](../../src/framework/interfaces/imetadata_description.h)、[数据集结构接口](../../src/framework/interfaces/ichannel_dataset_descriptor.h)、[输出观察接口](../../src/framework/interfaces/ioperator_output_observation.h)。

### T1 配置适配验收

2026-11-02 NPM provider 实现既有配置适配接口，复用实际任务与模块解析器；normalized_config 返回类型化有效 core/模块/observing/来源配置，精确标签引用经注册服务解析。来源能力缺失时不冒充确认；不运行算法、扫描报文或创建匹配器，标签选择保留 not_checked 并由真实 Open 核验。
新增断言先证实接口缺失；主契约审计（含 DNS 字典）及环境 fixture 2/2、协议契约回归 1/1 通过，失败/跳过/未执行均零。配置、构建、格式及范围检查通过。本地证据 evidence/npm-data-semantics/t1-config-adapter/ 不入库。
T1 整体完成并勾选；本切片 WIP=0，依据连续实施授权更新工作台后进入 T2。

### T2 完整验收

2026-11-02 真实双族 UDP/TCP 有限 PCAP 经生产调度、标签配置和 NPM 动态插件写入 SQLite；五类 DNS outcome、RCODE/TC/重试/单边与倒退时间、精确运行/使用记录、三视图及重启读取均通过。取消保留 incomplete，提交后记录失败保留 completed 数据且明确元数据失败；说明读取只读。
标准 integration 3 功能及 3 fixture 共 6/6、DNS 单测 4/4，selected=executed=passed=10，failed=skipped=not_executed=0，最终 status=passed。证据目录 `evidence/npm-data-semantics/t2-dns-pipeline/`、runner `20261102T093131Z-e9fd11e3` 与 `20261102T093241Z-7367d07e`（不入库）；编译、格式、范围与空白检查通过。T2 整体验收完成，WIP=0，按连续授权转 T3。

### T3 完整验收

2026-11-02 Basic/Session 两族 31/49 分析字段完整正文经生产 descriptor 与公共字典校验；身份从作者声明组合 run_id，Session→Basic 与 DNS→Session 使用本次实际已发布的同族/同视图精确引用，映射 run/session/观测域，不声明修订一对一。
真实双族 PCAP 的周期与 final 两次执行覆盖三个完整周期、尾周期、零增量终态、final 八列 NULL、独立会话计数、累计平均速率、完整/部分握手、重传、RTT valid/no_sample/ambiguous 和 UDP 不适用；既有 DNS 结果与关系重启回归通过。标准 integration 四功能及 fixture 8/8，周期 unit 3/3，合计 selected=executed=passed=11，failed=skipped=not_executed=0，最终 status=passed；源码/产物摘要稳定。证据 `evidence/npm-data-semantics/t3-session/`、runner `20261102T095024Z-a78949c9` / `20261102T095024Z-e7e2e9dd`（不入库）。格式、空白、范围及预存修改保护通过；WIP=0，按连续授权进入 T4。

### T4 完整验收

2026-11-02 HTTP1/TLS/ICMP 两族全部正文通过公共字典与生产结构审计，十二实体正文齐备。HTTP/TLS→同族 Session 使用本次同视图精确引用，ICMP 仅关联运行目录、不强制会话。真实双族报文经生产调度和 SQLite 核对 HTTP matched+404/请求单边、TLS1.3 ServerHello 与缺失 ServerHello、Echo 重试及双类单边、带/缺引用控制差错与 MTU；三视图事件身份一致。
标准 integration 三功能及 fixture 6/6、六项 HTTP/TLS/ICMP 独立回归 6/6，合计 selected=executed=passed=12，failed=skipped=not_executed=0，最终 status=passed；源码/产物摘要稳定，格式、空白、范围与预存文件保护通过。证据 `evidence/npm-data-semantics/t4-protocols/`、runner `20261102T100220Z-e8f933dc` / `20261102T100220Z-932553b7`（不入库）。WIP=0，按连续授权进入 T5，最终 HTTP/页面与统一验收尚未完成。

### T5 与 Feature 完整验收

2026-11-02 十二实体共358分析字段及三附加列、36公开视图和运行目录共37对象，在真实有限 PCAP/PCAPNG→调度→NPM 动态插件→SQLite 链路统一核对。普通 HTTP 经 Web/Gateway/Router 读取，全部字段顺序/原生类型/可空性独立对照 SQLite PRAGMA；精确字典/绑定、作者契约、有效配置及运行上下文可读取，describe 前后目录不变。
真实 Chromium 从既有数据库浏览入口选择 NPM 视图，字段表保持原四列布局；HTTP 与页面内容一致，保存一处人工文字后其他语义及作者机器契约不变。页面请求记录为16次、说明保存1次、分析行预览/执行0次。再次运行保留人工补充，旧使用记录与旧字典不变；实际停止宿主后经 IID 精确重读37对象，再用 PCAPNG 和 periodic_snapshot 运行验证不同 run/配置隔离与幂等。

最终冻结清单：

- integration 的12功能：test_npm_data_contract、test_npm_result_semantics、test_npm_semantics_pipeline、test_npm_session_semantics_pipeline、test_npm_protocol_semantics_pipeline、test_npm_semantics_http、test_operator_contract_model、test_operator_contract_schema、test_operator_contract_check、test_operator_contract_preflight、test_operator_contract_block、test_metadata_description；连同12项必要环境夹具 selected=executed=passed=24。
- unit：test_data_semantics、test_channel_dictionary_database_unit、test_channel_dictionary_iid、test_npm_basic；selected=executed=passed=4。
- 两轮均 failed=skipped=not_executed=0，最终 status=passed；源码/产物前后摘要一致。runner 为 `20261102T111234Z-120a4ec3` 与 `20261102T102843Z-34ead3f6`，证据缓存于 `evidence/npm-data-semantics/t5-completion/` 及对应标准 runner 目录，不入库。
- 声明目标构建通过，修改/新增 C++ 格式检查通过；database_plugin.cpp 保留既有格式，仅检查改动行。Python/Node语法、文档链接、规格尺寸、空白及允许范围/预存修改保护通过；一轮有界 diff 审查无阻塞问题。

T5 及 A1～A9 已完整达到，规格归档、Backlog/F4 标记完成，工作台 WIP=0。验收限于有限来源与 SQLite，不扩大到实时、Kafka、其他数据库或容器矩阵；不承担旧库迁移或跨版本兼容。未部署常驻服务，未提交或推送代码。


### T6 完整验收：通用发布与三生产数据库

2026-11-02 按用户要求补齐 MySQL、PostgreSQL、ClickHouse。NPM 发布器仅使用真实托管目录和既有结构提供接口，公共 database locator/output locator 的类型为非空无 NUL 字符串；新增后端实现存储及结构适配后无需修改 NPM 领域说明。测试注册 extension_db 提供者，完整发布、绑定与精确重读通过。PostgreSQL 的公开视图已同时进入浏览目录和结构描述；字段类型与可空性遵循各库真实元数据，领域正文取消 SQLite 限定。

三个独立真实后端用例均完成双族全部十二实体、358 分析字段及附加列、37 对象的发布与读取，核对原生类型/可空性、运行状态和代表 DNS/HTTP/TLS 数据、三视图身份集合与同族同视图精确关联；人工补充、重复运行、旧精确引用、HTTP 读取、PCAPNG 周期模式及实际宿主重启均通过。数据真值经既有通用只读查询接口核对，不依赖通用 JSON 小整数展示；本地测试入口仅在测试宿主存在，未新增产品查询接口。

- 最终 integration：12 功能及 12 必要夹具，selected=executed=passed=24，runner `20261102T122206Z-f01de7fd`。
- 最终 unit：NPM 核心、共享语义、可扩展定位 Schema、SQLite 字典回归，selected=executed=passed=4，runner `20261102T123400Z-44b6061c`。
- 两轮均 failed=skipped=not_executed=0、status=passed，源码与产物前后摘要一致；独立用例选择与实际清单一致。
- 构建、修改行格式、Python 语法、链接、空白、范围和预存修改保护通过，证据无私有数据库凭证。完整本地证据为 `evidence/npm-data-semantics/t6-backends/` 及上述标准 runner 目录，不入库。
- 统一运行配置派生自仓库配置，仅更换受管项目名以避开所有权不明的旧测试卷；初始化、认证、权限预检和业务用例共用该配置。未接管或清理原有卷，未使用业务数据库。

验收发现并隔离两个既有查询问题：通用 DataFrame JSON 未覆盖 8/16 位整数，调度批读入口不支持 ClickHouse。定位依据与复现见工作台及本地证据；未将查询引擎改动混入本 Feature，原生结果核对与说明验收完整保留。

T6 当时达到验收条件并归档；该阶段手测曾要求加载新插件后重新执行，T7 已纠正这一设计，现有对象的静态字典不再依赖新执行。此段保留历史验收边界，不代表 T7 已验收或部署；未提交或推送。

### T7 静态字典修正：本地检查点与外部阻塞

2026-11-02 静态模板随 NPM 作者契约提供，复用运行发布的同源领域组装；受管目录证明对象来源，Config 通用说明服务按声明的作者资源解析，核验结构并保留原生类型/可空性，不按数据库类型分支。无绑定时读取返回 producer_default 和精确作者依据，字典/绑定引用为 null，不创建版本或运行。十二实体的 history/latest/final/data 与 runs/entities 共50对象可读取；人工保存后才生成对象说明，后续执行保留人工内容并记录真实运行依据。

- 本地标准入口选中7项功能/共享回归及3项必要夹具，selected=executed=passed=10，failed=skipped=not_executed=0，status=passed。runner `20261102T131549Z-bbebcdb1`；源码/产物摘要一致。覆盖50个既有空对象无新任务读取、无元数据写入、原生结构、目录身份核验、非受管同名对象拒绝套用、通用扩展类型、首次人工保存与后续精确发布、原执行与插件重启回归。
- 冻结的7个构建目标通过；13个C++文件格式、Python语法和本轮范围检查通过，无关预存修改保留。证据缓存于 `evidence/npm-data-semantics/t7-static/`，不入库。
- 三库 doctor 实际失败于 `execution_denied/socket/clickhouse`，尚未认证；外部标准清单实际选择12项，在受管环境准备阶段因 Docker 访问被拒绝退出，executed=passed=0、not_executed=12、status=failed，runner `20261102T131705Z-5b411eeb`。HTTP/浏览器与三生产后端本轮未验证，不能以T6历史成绩代替。
- 当前沙箱禁止所需 socket，受限网络访问提权也已被自动审批拒绝；未绕过策略、重置测试资源或改动业务数据。未部署、重启常驻服务或切换激活副本。

T7 保持未勾选，Feature/Backlog/F4 保持进行中，不重新归档；当前切片停止于本地验收通过、外部验收被明确环境问题阻塞的检查点。


### T7 恢复验收：契约复用修正与当前错误检查点

2026-11-03 从原工作台恢复。三库在原受管专用环境完成 native 真实认证及只读查询，当前已越过上一轮 socket/Docker 环境阻塞；未使用历史成绩替代复验。
首次外部验收12项全部执行、8通过4失败，暴露同步执行等待超时；独立测量表明3.23MB作者契约每次精确读取耗时约0.81～0.84秒，逐关系说明会反复完整解析。
Config 改为按精确不可变 contract_ref 复用单个已验证的只读契约，版本变化重新读取、插件卸载清空；每次仍核验当前组件、目录和实际结构。新增同对象@1→@2→@1说明切换断言，人工补充及旧引用不串用。

- 修正后冻结构建目标通过；本地标准runner `20261103T043610Z-9de7e365` selected=executed=passed=10，failed=skipped=not_executed=0、status=passed。50空对象、只读、保存、发布、通用扩展、来源不符、版本切换和插件重启回归通过。
- 最终外部runner `20261103T043654Z-853dff74` selected=executed=12，passed=8、failed=4、skipped=not_executed=0、status=failed。HTTP用例首次同步执行触发客户端15秒等待；MySQL/PostgreSQL/ClickHouse的本地重复运行、37对象和人工补充检查已通过，随后HTTP第三次周期运行触发相同超时；后续50对象静态及Chromium完整步骤未全部到达，不能视为通过。
- 未增加超时、删断言或跳过失败。两份最终清单源码/产物摘要一致，修改C++格式、脚本语法、空白及范围保护通过。证据 `evidence/npm-data-semantics/t7-resume/` 和上述标准runner目录均不入库。
- 30分钟时间盒内停止新增实现，仅收集已启动清单结果；当前状态为“当前错误待修复”，工作台WIP=0，T7仍未勾选。下一检查点先确认超时后原任务的实际状态，再处理执行开销/同步等待并完成原验收；未部署、重启常驻服务、切换副本、提交或推送。

### T7 完整验收与最终收口

2026-11-03 继续完成原清单。隔离复现保留原任务：客户端15秒超时后原任务仍为running，约15.5秒完成、写入88行，运行目录completed；没有重复提交。HTTP验收改用既有Web异步提交和同一task_id结果轮询，单次HTTP仍为15秒、任务观察总预算30秒，完成后经Gateway既有任务详情精确读取contract_checks，核对task/runtime身份、状态、行数及恰好一次use_ref；原有结果、页面与重启断言保留。
该路径暴露调度器异步快照丢弃执行响应契约检查的阻塞。内部快照按语句顺序保留实际contract_checks，成功或失败均可追溯，既有状态接口仅终态返回，由Task原持久化路径保存；不新增产品路由。新增多语句成功/失败、记录顺序和重复查询回归，原Task生命周期及共享执行契约回归通过。

- 本地7项功能/共享回归及3项必要夹具：runner `20261103T050114Z-5941d932`，selected=executed=passed=10。
- 调度、共享执行契约、Task生命周期3项功能及2项必要夹具：runner `20261103T050330Z-e681f774`，selected=executed=passed=5。
- HTTP/Chromium、三库、NPM作者契约及静态专项6项功能及6项必要夹具：runner `20261103T050443Z-c0c71bce`，12项全执行、11通过，ClickHouse第二次本地运行超时。保留该失败证据；单独标准复验 `20261103T053946Z-1fb5a98f` 的ClickHouse及夹具2/2通过，所有原断言和超时保持。按相同源码/产物的最新逐项结果合并，原集成清单12/12已全部通过。
- 两份本地清单及集成清单的最终逐项结果均通过，failed=skipped=not_executed=0；四轮源码/产物前后及轮间摘要一致，原12项失败轮次不改写为通过。ClickHouse失败后两次诊断宿主启动超时，Git线程出现p9_client_rpc等待；只读核查和产物副本诊断均未改业务/系统配置，辅助扫描结束后原路径标准复验恢复并完整通过。三库从native位置完成真实认证和业务操作，37对象/358分析字段、原生类型/可空性、人工补充、精确历史、重复运行、HTTP、PCAPNG和实际宿主重启均通过；四后端已有50对象无需新任务读取静态说明，SQLite空对象及通用扩展提供者通过。真实Chromium验证原四列布局、HTTP/页面一致、保存一次且不预览/执行分析数据，已绑定和producer_default两种状态均通过。
- 冻结构建目标全部通过；修改行clang-format、Python/Node语法、空白、文档链接及范围检查通过。一轮有界Diff审查无P0/P1阻塞，无关预存修改保持。
- 证据在 `evidence/npm-data-semantics/t7-completion/`、上述标准runner目录及其引用的页面/后端结果缓存，不入库。未增加超时、删断言或跳过用例；未部署或重载常驻服务、切换激活副本、提交或推送。

T7已完整达到验收条件；T1～T7全部勾选，规格归档，Backlog/F4标记完成，工作台WIP=0。本Feature结束，不进入F5或诊断实现。
