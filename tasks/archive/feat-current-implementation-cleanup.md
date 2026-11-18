# Feature: 当前实现统一与兼容冗余清理

标识：`current-implementation-cleanup`；优先级：P1；状态：`[x]` 已完成（2026-11-18，T1–T4 全部验收）。
依据：[项目规则第 0 节](../../AGENTS.md#0-产品阶段与兼容性策略)及用户明确要求保留产品自身版本。
前置：已完成的兼容性静态审计和产品版本范围复核；实施进度以工作台及完成证据为准。

## Non-Goals

- 不调整产品版本政策、版本来源或发布流程，不删除产品版本、包版本、构建身份及发布校验。
- 不删除业务修订、CAS、精确历史引用、模型训练版本或当前契约校验，不以删历史、清空数据代替格式整理。
- 不修改第三方协议、模型和依赖的真实版本，不缩减现有数据库后端、时间驱动、单/多输入等有效能力。
- 不扩展算子算法、长期任务恢复、模型诊断等业务能力，不引入兼容开关、永久迁移框架或另一套版本化接口。
- 不按关键词整文件改写，不重写归档规格中的历史证据；尚未确认的兼容候选不直接作为删除目标。

## 业务意图与范围

面向产品尚未上线阶段的开发与维护者，消除同一能力同时维护旧接口、旧格式和兼容命名的负担，
让调用方、实现、测试、部署样例和当前用户文档共同使用唯一当前契约；产品版本和数据业务含义继续可追溯。

范围涵盖插件执行、管理与配置、数据语义与探索、持久化与恢复四类契约。
交付结果是现有主链路在单一契约下完整运行，产品正常运行路径不再选择、转换或兜底接受历史实现。
跨模块范围已复检：这些变更共同服务“只维护当前实现”，以调用和持久数据两条链路验收；不因命中文件数拆分 Feature。
四个 Feature Task 各自完成实现、调用方、测试和文档闭环；具体文件、接口声明及验收命令在实施工作台冻结。

## 必须保留的边界

| 类别 | 保留契约 |
| --- | --- |
| 产品版本 | 根目录 VERSION、SemVer/预发布解析、CMake project VERSION、生成头与 build-info.json、version/revision/dirty。当前 0.1.0 不因清理递增或删除。 |
| 运行与交付身份 | C++/共享库/Worker/前端内嵌身份、--version、GET /api/version 及 no-store、UI 版本展示、Python PEP 440 与 npm 包版本、探索 ui_version/manifest.build、Git/镜像标签、OCI 元数据及源码/产物一致性校验。 |
| 当前数据一致性 | 配置/字典/结果 revision、CAS、精确历史引用、模型训练与 calendar_version；schema_version、abi_version、结构尺寸和配置摘要等仅验证当前契约的检查。 |
| 外部真实版本 | HTTP、IPv4/IPv6、TLS legacy_version、TPACKET_V3、SQLite API 的 _v2、Graphic Walker 及其工作流格式、OpenAPI/JSON Schema 标准和依赖锁定版本。 |
| 外部 HTTP 对接 URL | 调用 OpenAI 等外部服务，或供外部客户端/模型工具调用的项目接口，可按实际或已明确规划的对接契约保留 /v1 等路径段。保留 URL 不代表支持历史版本，产品仍只维护当前契约。 |
| 现行业务能力 | IPlugin/IQuerier、批次生命周期、可选能力、文本/文件上传、未声明算子契约状态、地址族模板及单/多输入采集；不可把职责区别当旧兼容删除。 |
| 已修正误报 | Kafka 密钥校验固定明文 flowsql-kafka-key-v1：不是表/键标识，也无新旧选择，不属于本次改名目标。 |

同一文件可同时包含待清理代码和上述保留内容；例如清理 Web 列表别名及语义路由时必须保持产品版本接口。
[产品版本规格](../archive/feat-product-version.md)和[版本说明](../../docs/versioning.md)继续有效，未来发布阶段的政策不等于当前运行时兼容层。

## 当前接口与数据契约

| 契约 | 统一后的结构与行为 |
| --- | --- |
| C++ 算子 ABI | 使用现有 descriptor/capability 机制作为唯一创建、描述和销毁契约；每个逻辑算子只有一个导出索引。自有类型、常量和元数据导出符号去掉版本后缀；仅接受当前 ABI，不按历史版本选择导出组。 |
| BlockTransform 与 Baseline | 统一为无版本后缀的当前接口/IID，保留时间通知、采集事实、托管 sink 和 readiness 结果。Baseline 仅保留完整提交结果契约，不再以旧返回值包装新接口。 |
| 共享类型与职责命名 | 自有 interface/struct/enum、函数、模块、模板和 IID 按职责命名；同职责的两代接口合一，不同职责保留独立类型。采集进度按单输入/多输入区分，不删除其中一个。 |
| 列表与算子身份 | 列表统一使用 /api/channels/list 和 /api/operators/list；算子统一 category.name，删除裸名注册及查找回退。源码和文件上传的两种当前用途保留。 |
| 部署与流配置 | Gateway plugins 统一对象形式 {name, option, databases}；流通道统一顶层 type/name/role/options，options 为对象。同步当前调用方，删除旧 option 字符串转换、从旧字段推导 role 和同用途 config 包装回退。正常默认值仍按当前契约定义。 |
| SQL 解析结果 | SqlStatement 以 sources、operators、operator_with_params 为唯一来源，算子与参数按索引对应；保留列、目的通道和分阶段过滤。删除单 source/op_category/op_name/with_params 副本及无算子历史 WITH 语法。 |
| NPM 参数 | 使用 parameters JSON 字符串信封承载 core 及一级模块名参数，移除旧平铺调优字段读取和新旧来源枚举；features、observing、input_namespace、source_domains 等现行独立字段保留。当前格式校验、参数默认值和错误反馈保持。 |
| 通道说明与绑定 | 保存统一 editor_changes；绑定采用当前包含结构信息的格式，schema_id 使用无版本标签名称。精确修订、来源、声明状态和关联语义保留，不再接受旧 changes、旧绑定或在线升级。 |
| 时间说明 | Unix epoch 仅使用 1970-01-01T00:00:00Z；整理声明不改变时间数值、单位、时区和冲突判断的业务含义。 |
| HTTP/schema/模板与探索 | 对 /api/semantics/v1/*、/api/visualization/v1/* 等自有路径逐条判断：仅内部用途的去掉版本段，承担实际或已明确规划的外部契约者可保留。自有 schema_id、模板及探索类型仍按职责命名；前后端、OpenAPI、JSON Schema、bridge 和资源生成一致，当前匹配检查与第三方格式保留。 |
| 数据通道与存储 | 自有 packet/observation/snapshot 标识和 NPM/Baseline 表、视图、进度存储名去掉版本标签；同步生产、消费、查询、清理和目录。当前表结构、配置身份、checkpoint/artifact 字段和身份关系必须齐全。 |

接口形状以当前有效能力为基准，禁止借改名丢失能力或保留转发别名。编码前在对应切片输出并对齐公共头文件、
纯虚接口/struct 和序列化契约；声明、所有实现及消费者同切片同步，不通过旧新接口并存过渡。
动态库边界保持创建/销毁配对、实例/任务/Arrow 产物先于库卸载，以及现有不跨导出边界抛异常的契约。
涉及公共 ABI 时统一重建宿主、全部受影响的自有插件及动态库夹具，不以混用旧二进制验收新契约。
删除旧接受路径后，由系统边界返回现有明确错误；不在内部增加重复判空，也不要求逐个旧格式编写特殊识别器。
T3 实施前逐条记录 URL 的用途、调用方和对接契约；未来 OpenAI 调用或模型工具接口已有明确规划时也可保留 /v1，
不得按路径含 v1 批量删除。该例外只调整 URL 清理范围，不保留旧路由别名、多版本分派或内部版本命名。

## 数据整理与恢复契约

- 涉及持久内容的任务均须在切片开始时盘点实际配置、数据库、草稿、检查点和 artifact；不能等到 T4 才备份 T2/T3 涉及的数据。
- 在一致快照或停止写入条件下先备份，记录对象范围、结构、行数/业务摘要及引用关系，在隔离位置验证备份可恢复，再一次性整理到当前契约。
- 整理同时覆盖仍需查询的历史修订及精确引用；保留业务身份、修订顺序、数据与结果含义。编码变化引起的摘要变化须联动引用，旧新摘要映射仅留在整理证据中，不成为产品查询别名。
- 旧字段不能无损推导时停止该对象整理，保留原件并明确缺失依据；禁止伪造模型元数据、重置身份或静默丢弃记录。浏览器草稿先导出备份，再在产品外整理需要保留的内容。
- 一次性整理材料只用于已盘点数据，保存在本地证据中，不被产品启动、打开表、恢复任务或发布包调用；不交付通用多版本迁移框架。
- 整理后核对记录、引用、摘要、查询和恢复结果；验证通过才使用当前实现。失败时恢复备份，禁止半整理状态继续写入。
- 正常产品运行仅创建新对象的当前结构、读取/校验当前数据；不对旧表补列、不加载时重写旧配置、不补齐旧 checkpoint/artifact。新对象生成身份的正常逻辑保留。

## 主链路

1. 作者按当前配置/SQL/接口发起操作 → Web/Gateway/Scheduler 校验单一契约 → 插件按当前能力创建任务 →
   数据通道与结果消费者交换当前结构 → UI/调用方按同一 Schema 展示；错误输入明确失败，版本查询继续报告真实产物身份。
2. 维护者盘点需保留数据 → 一致备份与隔离恢复校验 → 产品外一次性整理并核对引用 → 当前实现加载/执行/查询/恢复 →
   定向回归确认数据与能力保留；不能整理的对象保留原件并停止处理，不用运行时兼容分支兜底。

## Feature Tasks

- [x] T1：交付唯一当前插件与共享执行契约，使全部自有生产者和消费者以职责命名协作，保留现有能力与动态库生命周期保证。
- [x] T2：交付统一管理入口、部署配置和 SQL/参数契约，使同一操作只有一条明确解释路径，缺失必要部署依赖时可诊断地失败。
- [x] T3：交付统一通道说明、语义绑定与探索/数据通道契约，使前后端和数据消费者共享当前格式，业务修订与精确引用保持可追溯。
- [x] T4：交付完成备份整理的当前持久化与恢复路径，使目录、结果、配置和模型状态无需运行时迁移即可完整加载、查询和恢复。

默认从 T1 开始，后续任务使用已统一的共享接口；每次只执行工作台冻结的一个切片，完成即停。
每个任务包含其生产者、全部调用方、fixtures、当前用户文档和相关回归；跨切片沿用任务编号，不把测试/文档延后为独立扫尾任务。

## 已核实范围与任务归属

下列编号对应 2026-11-17 静态审计：18 组行为和 7 类命名存在交集，不相加为独立缺陷数。
该表自包含必要范围；456 个命名 token、312 个文件只是定位索引，不是整文件修改清单或允许文件表。

| 编号 | 必须收敛的范围 | 归属 |
| --- | --- | --- |
| A01 | C++ 宿主双 ABI、旧 create/destroy 与当前 descriptor/capability 两套加载及真实动态库夹具。 | T1 |
| A02 | BlockTransform 双接口、Scheduler 降级查找、NPM 重复导出及任务配置。 | T1 |
| A03 | Baseline 旧提交包装及要求旧客户端继续可用的测试。 | T1 |
| A04 | Web 通道/算子列表旧路由别名。 | T2 |
| A05 | Gateway plugins 旧字符串与对象混合配置、部署样例。 | T2 |
| A06 | 流通道增改的旧 option 转换及取值回退；包装用途按调用链核对，统一同用途入口。 | T2 |
| A07 | SQL 单值/数组重复字段、Scheduler 回填/回退及无算子历史 WITH。 | T2 |
| A08 | Builtin/Catalog 裸算子名别名及 Scheduler 二次查找。 | T2 |
| A09 | NPM 平铺 WITH 与 parameters 双解析、旧来源标记及等价断言。 | T2 |
| A10 | 通道说明旧 sessionStorage 草稿、保存旧草稿 UI、changes/editor_changes 双保存。 | T3 |
| A11 | 绑定双 schema_id、继承旧 declared_schema 的在线升级及双 Schema。 | T3 |
| A12 | 前后端 Unix epoch 旧文本别名及等价测试。 | T3 |
| A13 | Catalog/BinAddon 启动给 operator_catalog 补 category/plugin_id。 | T4 |
| A14 | Database 加载 YAML 补 dataset_instance_id 并重写文件。 | T4 |
| A15 | NPM SQLite 与非 SQLite 打开旧结果表时补 purge_cursor。 | T4 |
| A16 | Baseliner 检查点按旧版本恢复缺失活动/父子/释放状态字段。 | T4 |
| A17 | Baseline artifact 缺 relation_fusion_metadata 补默认及旧 feature_id 身份替代。 | T4 |
| A18 | Catalog 缺 Python publisher 的旧发布路径及 Builtin 忽略契约注册失败。 | T2 |
| B01 | 数据库、采集、过滤、Kafka、语义、Baseline 等共享接口、结构和 IID 版本命名。 | T1 |
| B02 | NPM 自有模块、类型、函数、模板名称中的版本标签；协议 HTTP1/TLS/IP 含义保留。 | T1 |
| B03 | NPI 池和单/多输入采集进度类的版本命名。 | T1 |
| B04 | 内部自有 HTTP 路径、语义/算子 schema_id、flow-labeling 模板标识及 Schema/OpenAPI/示例引用；外部对接 URL 按契约保留版本段。 | T3 |
| B05 | 探索 Field/SqlPlan/TableWorkspace 等类型与内部自有路由；对外契约 URL 版本段、包身份和当前 bridge 匹配保留。 | T3 |
| B06 | NPM 数据表及 history/latest/final 视图、progress 和 Baseline 结果/融合/维护存储标识。 | T4 |
| B07 | 自有 packet、baseline observation、DataFrame snapshot 通道格式名。 | T3 |

T1 同步修正 Baseline 文档的 append-only 兼容要求及 NPM 契约中已不存在的旧 reader 名称；后者不代表存在两套 reader 实现。
SQLite 逐行写入、NPI Top 投影及前端 null 加载态经 T4 核实均属当前能力，继续保留；
前端测试使用当前完整分类字段，逐行写入的过时注释同步纠正，不凭 legacy 注释删除有效能力。

## 验收锚点与依赖

下表在实施时落实为可执行断言；静态审计和文档检查不计为产品验收通过。

| 验收组 | 必需结果 |
| --- | --- |
| T1 插件与执行 | 当前真实动态库加载、描述、创建/销毁和卸载配对通过；每个逻辑算子单次导出；非当前 ABI 拒绝；时间驱动、单/多输入、readiness、采集和托管结果能力保留。共享接口的全部实际消费者构建及相关回归通过。 |
| T2 入口与参数 | 当前列表/配置/SQL/parameters 全链路通过；旧路由不再注册，旧格式不转换，裸名不回退；无算子正常查询和当前默认参数仍有效；缺当前必要依赖明确失败，未声明算子契约状态按当前业务处理。 |
| T3 语义与探索 | 当前 editor_changes 保存、绑定、时间说明、精确修订/CAS、查询与探索交互通过；旧接受分支消失；自有格式生产消费一致。逐条核对 URL 分类，已有外部对接路径及调用方契约通过，明确规划的路径保留依据，不为验收新增 OpenAI 接入。历史整理后引用闭合、业务含义一致，第三方工作流和产品 ui_version/manifest.build 保留。 |
| T4 数据与恢复 | 实际受管数据有可恢复备份和整理核对记录；新建、整理后重启、查询、结果清理及 checkpoint/artifact 恢复通过；旧/缺字段对象明确拒绝且不被产品静默改写；模型和配置身份不重置。无存量旧数据时记录盘点结果，并用隔离夹具验证整理与恢复，不伪报实数验证。 |
| 产品版本保留 | 版本解析/派生、CLI、GET /api/version、前端构建展示及发布一致性拒绝路径回归通过；实际受影响产物报告自身 version/revision/dirty，不被当前源码重标。没有容器交付变更依据时不默认重建镜像。 |
| 范围收口 | A01–A18、B01–B07 逐项有变更与验证依据；当前代码、配置、样例及用户文档无已确认的旧接受路径或自有版本命名。历史归档、整理夹具、拒绝旧输入的负向样本及明确保留项不作为关键词零命中的目标。 |

- 依照[测试说明](../../docs/testing.md)使用标准 CMake 入口与定向 target、统一测试入口和筛选；实施工作台冻结具体命令、用例、fixtures、配置来源、权限和执行位置。
- 纯单测不启动外部服务；数据库/Kafka/浏览器验收按改动调用链选择，使用统一运行时配置、隔离夹具和资源锁，从实际执行位置预检。
- 涉及 NPM 结果持久化及共享数据库契约时，覆盖其受影响的 SQLite/MySQL/PostgreSQL/ClickHouse 路径；涉及 Kafka 数据通道时覆盖真实生产消费，认证矩阵仅按实际影响扩展。
- 必需清单全部实际执行且无环境跳过；依赖不可用按失败阶段记录，不能以删用例或放宽断言收口。Feature 完成核对四个任务及共享契约影响，不默认执行全仓测试。
- 每个任务检查差异、版权和格式；证据放在 evidence/current-implementation-cleanup/，不入库，用户文档不依赖本地证据才能使用当前功能。

## 完成证据

- 立项依据为 evidence/current-implementation-audit/ 中的 report.md、scope-review.md 及命名索引（本地缓存，未入库）；上述范围、保留项和验收要求已在本规格中列明。
- 首轮检查点：B03 职责命名切片已验收，当时 T1 尚未完成；后续完成情况见下方最终证据。
- T1 首轮完成改名和消费者重建，9 项定向回归中 8 项通过；Kafka→NPM 夹具补齐 Config 依赖并发布真实标签快照后，仍发生 source_not_drained。首轮失败与检查点保存在 evidence/current-implementation-cleanup/t1-role-names/（未入库）。
- 2026-11-17 续作消除验收阻塞：复用固定文档/API Schema，保留每次独立验证器及原超时/断言，补充并发接受/拒绝验证。Kafka→NPM 全部场景通过，5 项受影响语义/算子契约测试与 3 项环境夹具通过；结合源码/产物摘要核对未变的前轮 8 项，共 14 项必需回归及 3 项环境检查全绿，无跳过。证据 evidence/current-implementation-cleanup/t1-progress-regression/（未入库）；仅收口 B03，T1 保持未勾选。

- 2026-11-17 T1/A03：Baseline value/ratio/relation 任务的 SubmitObservation 统一返回完整 result 与 readiness，删除旧包装及可选扩展接口；当前真实 .so 客户端、确定性/拒绝/恢复/readiness、批量故障和滚动/预测验证通过。11 个构建目标、13 项测试及 1 个工作区 fixture、2 个独立评估程序全部通过；证据 evidence/current-implementation-cleanup/t1-baseline-contract/（未入库）。T1 其他范围继续实施。

- 2026-11-17 T1 最终验收完成：A01/A02/A03/B01/B02/B03 全部收口。C++ 仅保留当前 descriptor/capability ABI；BlockTransform 唯一当前接口、NPM 唯一索引；Baseline 提交返回完整 result/readiness；共享类型、函数、IID、NPM 模块/模板及进度类按职责命名，无兼容别名。产品 `0.1.0`、外部真实版本和现有独立能力保留。
- 构建与验证：177 个受影响目标及独立 C++ 插件样例构建通过；冻结的 157 项用例按影响分组全部实际执行，失败修复后复测，最终 157/157 通过、零环境跳过、零遗漏。包含产品版本、动态库加载/实例隔离/卸载、单/多输入、时间驱动、采集事实、托管结果、SQLite/MySQL/PostgreSQL/ClickHouse 与真实 Kafka 数据面；4 项 Kafka 实测各覆盖 11 个认证 profile，包括声明的续期/断连场景。
- 最终执行路径修复覆盖 28 项受影响回归：Scheduler 完整端到端通过（131.11 秒），JSON 边界检查通过，末轮 6 项 unit 与 20 项 integration（另 20 个环境检查）全绿；Baseliner 生产矩阵以最终产物通过（188.90 秒），未增加超时或放宽断言。A03 的两个独立批量/滚动评估程序亦通过。
- Diff、允许范围、版权和修改行 clang-format 检查通过。原完整构建 64 条警告均逐行核实为基线未改动语句；本轮回归修复重建零 warning/error，不宣称全仓无警告。各测试组执行前后源码/产物摘要一致，最终 Scheduler 的运行库与已验收产物一致。
- 最终清单、逐例结果、源码/产物摘要、完整工作台与审查见本地 `evidence/current-implementation-cleanup/t1-shared-names/` 下的 `final-validation.json`、`final-source-manifest.json`、`final-artifact-manifest.json`、`completed-workbench.md` 和 `final-review.md`（未入库）。T1 完成后停止，T2–T4 未实施，整个 Feature 保持进行中。

- 2026-11-18 T2 最终验收完成：A04/A05/A06/A07/A08/A09/A18 全部收口。列表仅保留 `/api/channels/list`、`/api/operators/list`；Gateway plugins 仅对象；流定义仅顶层 type/name/role/options；SQL 仅 sources/operators/operator_with_params；算子仅 category.name；NPM 仅 parameters JSON 调优入口。旧路由、旧格式转换、裸名别名与查找回退、平铺参数及来源枚举、缺 publisher 的旧发布路径均删除；Builtin 契约注册失败使启动明确失败。
- 当前调用方、测试、部署样例和用户说明同步。无算子正常查询、默认参数、未声明算子契约及现有独立能力保持；产品 `0.1.0`、业务修订和外部真实版本保持。验收发现独立基准仍断言双索引、真实采集夹具仍查询旧 Basic 表名，分别同步为当前唯一索引和 IPv4 结果表，完整断言重跑通过，未修改生产存储或放宽验收。
- 受影响共享头与实际消费者按切片定向构建；注册/发布切片 33 个目标通过，最终 23 个目标及两个独立程序修复重建零 warning/error。冻结清单去重后 50 项 CTest 业务用例、26 项环境检查和 4 个独立程序全部实际执行通过，零跳过、零遗漏；覆盖当前入口/负向拒绝、发布与启动依赖、Scheduler、插件/版本、SQLite/MySQL/PostgreSQL/ClickHouse、语义/浏览器/重启、真实 Kafka 与采集。三个 Kafka 原生数据面入口各保留既有 11-profile 认证矩阵；NetAdapter 三个 SQL 模式仅验证 af_packet 当前参数入口，不宣称全部采集后端或性能矩阵。
- 存量配置和数据先一致备份并隔离恢复：2 条 stream 定义、4 个 NPM 契约业务修订在产品外一次性整理，当前实现读取原库通过；业务身份、修订、精确引用、CAS 及其他字段/记录保持，摘要按当前文档重算。最终完整 dump、完整性和外键核对通过，产品未加入迁移兼容代码。
- 最终汇总、逐例清单、数据整理、源码/产物摘要和 Diff 审查见本地 `evidence/current-implementation-cleanup/t2-management-contract/` 下的 `final-validation.json`、`final-source-manifest.json`、`final-artifact-manifest.json`、`completed-workbench.md`、`final-review.md`（未入库）。各标准测试组前后源码/产物一致；随后两个独立夹具修复另行重建并验收，其余产物保持。另记录 T1 非阻塞描述残留：NPM 输入契约仍写旧 reader 名称，实际接口已改名，待 Feature 收口核对。T2 完成后停止，T3/T4 未开始。

- 2026-11-18 T3 验收完成：A10/A11/A12/B04/B05/B07 收口。editor_changes、完整绑定与唯一epoch、无版本schema/模板/探索类型、packet/observation/snapshot生产消费全部同步。公共 `/api/semantics/v1/*` 有实际OpenAPI/外部客户端契约，保留URL；内部 `/semantics/*` 和探索路径均使用当前无版本路径，无旧路由别名。产品和第三方版本保持。
- 按实际调用链分切片验收12、10、19、18及最终35项业务用例（有重叠），另5项时间说明及13项Kafka UI Node断言全部通过，零环境跳过。覆盖语义/CAS/精确历史/绑定、真实浏览器交互与重启、探索4后端/bridge、真实Kafka数据面和采集、共享算子契约、完整Scheduler及产品版本。最终Scheduler157秒通过；就绪夹具修复由首次poll屏障锚定，实测Open需3772/3411ms，原300次操作轮询与5秒release挂起断言保持，生产逻辑及CTest超时未改。
- 存量先一致备份、隔离恢复：18语义修订schema、240个结构化epoch声明、4个reader描述及2个配置修订在产品外一次性整理；所有业务身份/修订/精确引用/来源/确认和其他记录保持，内容长度/摘要同步。隔离及原库以真实IID读取全部18修订、HTTP读取3配置并核对完整dump、完整性和外键通过。无实际旧浏览器草稿，以隔离Chromium证明旧草稿导出、离线转换后当前恢复/保存及重启。
- 证据为本地 `evidence/current-implementation-cleanup/t3-semantics-contract/`、`t3-metadata-editor/`、`t3-exploration/`、`t3-data-channel/`、`t3-data-closure/` 的清单、数据manifest、构建/测试、摘要和工作台（未入库）。组内源码/产物稳定，夹具修复后定向复测，未修改共享产物者保留已通过证据。最后扫描确认capture_v2资源键及UI文案属T1描述残留，已纳入T4结果契约同文件和4历史修订整理；T4未提前勾选。

- 2026-11-18 T4 验收完成：A13/A14/A15/A16/A17/B06 全部收口。Catalog/BinAddon 仅校验完整当前目录；持久 YAML 必须有稳定 dataset_instance_id，启动不生成或重写；四数据库后端不再补 purge_cursor。NPM/Baseline 表、视图、进度与动态字典资源按职责命名，生产、查询、清理、发布、前端及真实消费者同步。
- Baseliner 仅恢复完整当前检查点，缺活动墙钟、父模型、释放 epoch 或序列年龄即拒绝且状态不变；不再按旧版本推导。Relation artifact 加载、导出和 seed 不补融合元数据，feature_base 与当前任务精确一致；当前壁钟回退、value/ratio 契约及模型真实版本保持。产品 0.1.0 和实际对外 `/api/semantics/v1/*` URL 保留，5 项产品版本回归通过。
- 三切片分别验收 13、41、22 项业务用例，去重后 69 项全部实际通过，必需用例零跳过、零遗漏；另 17 项 Node 断言、3 个隔离采集 SQL 模式、2 个独立批量/滚动评估程序和实际 artifact 只读加载/导出/seed 校验通过。覆盖当前创建/拒绝/重启/查询/清理/恢复、SQLite/MySQL/PostgreSQL/ClickHouse、真实 Kafka、Scheduler、浏览器及产品版本，未执行全仓测试或无关交付/性能矩阵。
- 存量先一致备份并隔离恢复：2 条历史目录记录已整理并用真实插件加载；4 个 NPM 业务修订一次性整理 72 个字典键及 4 个 capture 资源键，原库全部 18 个语义修订、3 个配置读取及完整 dump/完整性/外键核对通过。业务身份、修订、精确引用和其他记录保持。实际无旧结果表、缺身份持久配置或旧检查点，不伪报转换；现有 value artifact 备份恢复后只读验证，原字节和模型身份保持。
- 首轮失败均保留：遗漏的 Database 共享查询结构/资源消费者及动态测试名称已同步并复测；ClickHouse 夹具共享锁超时未计通过，随后相同预算串行真实用例通过；模型维护负向夹具对空模块的错误索引改为遍历实际序列并断言数量，两个共享测试入口通过。原生产断言和超时保持；各标准组内源码/产物摘要稳定，维护修复核对仅测试源码和对应产物变化，其他 20 项记录继续有效。
- 最终 Diff、允许文件、版权和修改行 clang-format 检查通过；T4 定向构建零 C++ error/warning，不据此宣称全仓无告警。三切片完整证据见本地 `evidence/current-implementation-cleanup/t4-catalog-config/`、`t4-storage/`、`t4-model-restore/`；Feature 汇总及 A01–A18/B01–B07 逐项复核见根证据目录 `final-validation.json`、`scope-audit-final.json` 和 `scope-audit-final.md`（均未入库）。全部任务完成，本规格归档，backlog 标记完成；未 commit/push。
