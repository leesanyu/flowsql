# Feature：通道数据字典生成与绑定（channel-data-dictionary）

状态：`[x]` 已完成；优先级：P1。
所属 [Epic](../specs/epic-data-semantics-dictionary.md#channel-data-dictionary)：F2；依赖已完成的 [F1 标准与字典管理](../archive/feat-data-semantics-registry.md)。

## 业务意图与 Non-Goals

让接入 Kafka 结构化 source、CSV/命名 DataFrame 和已有数据库表的用户，从字段配置或实际结构取得默认字典，
在对象详情补充说明、引用标准、选择已有定义并绑定精确版本；结构变化后保留人工内容，明确需要确认的差异。
可观察结果是同一数据集在 Web 与普通 HTTP 客户端中取得一致的结构依据、字典引用、映射和核对状态。

- 不生成算子输入输出契约、推导任意 SQL/算子结果语义、整理 NPM 领域字典或接入查询/绘图消费流程；分别属于 F3～F5。
- 不消费 Kafka 消息猜测含义，不扫描全库、抽样验证业务质量、自动 Join、转换单位或改写 Topic/表数据。
- 不覆盖 packet source/sink、普通配置通道或任意流通道；不新增 Kafka 编码、数据库后端、数据导入格式或凭证管理。
- 不自动发布预览、升级全部共享绑定、覆盖历史或热更新运行任务；不引入审批、服务端草稿、大模型、MCP 或诊断编排。

## 入口与结构事实

| 来源 | 发现与生成边界 | 必须保留的事实 |
| --- | --- | --- |
| Kafka 结构化 source | 已保存且 `payload_kind=structured` 的具名 source 配置；兼有 sink 时只描述 source 侧 | 实际解码器输出的字段、JSON Pointer、类型、可空性、显式 unit、实体标签和五个 `__kafka_*` 元数据列；不打开 Consumer 或解析密钥 |
| CSV / 命名 DataFrame | 现有 CSV 导入及已注册 DataFrame；列表与未加载对象只取元数据 | 已加载 Arrow Schema；未加载为 pending，用户经现有显式加载入口取得真实结构后再生成；不额外扫描 CSV 推断第二套类型 |
| 数据库明确表 | 已配置 SQLite、MySQL、PostgreSQL、ClickHouse 连接中用户指定的限定表 | 列顺序、原生完整类型及参数、可空性；读取系统目录/表结构，不读取业务行；空表同样可生成 |

- 数据库 locator 包含后端、连接身份、适用的 database/schema 和 table，按后端规则引用标识符；请求不接受任意 SQL。
  同一连接的不同表、不同 schema 的同名表分别绑定；不把整个连接当作数据集，不展开全库自动登记。
- 复用 Kafka 的结构化配置解析与 Schema 生产代码、Catalog 的真实 Arrow Schema、Database 的表结构访问能力。
  现有 DataFrame 列表投影和数据库 `describe` 的通用行列响应只作入口依据，不能直接假定已含完整的类型/可空性契约。
- Arrow 类型参数保留时间精度/时区、Decimal 精度/标度及嵌套结构；数据库保留原生类型与 dialect，不用查询值或有损 Arrow 转换替代原生声明。
  SQLite 动态类型只代表列声明，不保证实际值类型；无法确定的属性列入 `structure_gaps`，不得填入猜测值并标成完整匹配。
- 字段重复、类型描述无法无损表达或缺少必需结构时返回可定位错误/缺口，预览不截断后保存；不以空字段表伪装读取成功。

## 核心数据与绑定契约

### 对象身份与结构快照

| 对象 | 最小结构 |
| --- | --- |
| DatasetLocator | kind 为 `kafka_source`、`dataframe` 或 `database_table`，以及上述来源限定信息；仅定位，不含凭证或数据样本 |
| DatasetSnapshot | `schema_id=flowsql.channel-dataset.v1,object_key,locator,source_revision,identity_state,schema_state,fields,schema_fingerprint,producer_metadata,structure_gaps` |
| SourceField | `name,type,nullable`；复用 F1 类型描述，来源已声明的路径/单位等放在 `producer_metadata`，不混入人工语义 |
| DictionaryPreview | F1 字典候选、候选映射、来源快照、基准字典/绑定精确引用、差异、待确认项和 `semantic_gaps`；不创建 revision |
| ChannelBinding | `schema_id=flowsql.data-binding.v2`；保留 F1 绑定属性，增加 `source_snapshot` 与 `confirmation`；仍属 `kind=binding` |
| CheckResult | `binding_ref,dictionary_ref,state,differences,structure_gaps,semantic_gaps,checked_at,data_validation=not_run`；失败不沿用旧的 matched |

- `object_key={provider_id,object_id}` 沿用 F1。受管 Kafka/DataFrame 的持久对象身份独立于名称；重命名保持身份，删除重建或同名替换产生新身份。
  进程内指针、Catalog 当前用于探索的临时 source_id、连接地址及 Schema 摘要均不能单独作为持久对象身份。
- 受管对象身份与创建/删除/替换生命周期保持一致并可重启恢复；DataFrame 同实例、同结构追加不因数据行增加而重新绑定。
  替换整份快照或重新导入即使名称、字段相同也需确认语义；旧绑定可追溯，但不自动转给新对象。
- 数据库对象由持久连接实例和限定表标识定位，可取得的原生对象身份纳入核对；连接指向新库或可观测的表重建需重新确认。
  对无法识别的库外同名同结构重建，明确 `identity_state=unknown` 和待确认边界，不宣称能自动识别未观察的 DDL/业务含义变化。
- `source_revision` 是来源结构/身份及生产者声明的非秘密不透明版本；字段提取路径、显式单位变化必须使其变化，认证密钥及行数不进入该版本。
  Kafka Topic/连接所指数据集变化同样需重新确认；不声称识别未经观察的集群外 Topic 删除重建。
  `schema_fingerprint` 为规范的有序 `{name,type,nullable}` 列表 SHA-256，与业务含义、字典 revision 分离；pending 时 fields 和 fingerprint 为 null，incomplete 的 fingerprint 为 null。
- `identity_state=known|unknown`；`schema_state=known|pending|incomplete`。来源读取失败通过错误响应表达，不伪装成 pending 或空结构。
  incomplete 的字段缺项可为 null，并由 `structure_gaps` 定位；它不是可直接保存的 F1 字典，须取得必要结构后再生成/匹配。
  `checked_at` 为响应中的本次观察时间，不参与定义内容和无变化增版；状态只对已声明的观察点有效。

### 默认生成、补充与结构变化

- 已知且完整的结构生成 F1 字典候选：字段名称、类型、可空性取自来源，title 初值可用字段名，field_id 分配独立稳定身份。
  行含义、身份/修订/关联键、业务时间、NULL 含义、聚合规则及标准引用不能由名称推断；没有依据的属性为 unknown。
- 配置明确声明的单位/时间表示及已定义元数据列可按现有生产契约填 `origin=producer`，保留来源版本；类型本身不证明业务时间或单位。
  producer 表示声明来源，不代表业务质量验证；人工补充沿用 F1 `SemanticValue`、标准精确引用、缺口与校验规则。
- 同一对象重复预览基于既有绑定/字典保留 field_id；未绑定时首次预览的候选可供后续保存使用，不以名称哈希代替字段身份。
  选择已有字典必须显式给出 field_id→来源字段名映射，覆盖全部字典字段且不重复；允许额外源列，类型/可空性按描述精确比较。
  跨 Arrow/原生数据库类型不自动认定等价；不一致时显式适配为新字典版本或另存，不能为复用而放宽检查。
- 核对至少区分新增、删除、类型变化、可空性变化、顺序变化、生产者声明变化和可能重命名。
  未变字段保留全部人工说明、标准引用和 field_id；新字段语义保持未知；类型/可空性或生产者声明变化保留原说明并列为待确认。
- 删除字段保留在旧字典版本及差异详情，候选不直接丢失其说明；影响身份/时间/关联等内部引用时先显式修订，禁止悬空引用。
  可能重命名只展示删/增候选，用户确认映射后才沿用 field_id 与说明；同名重新出现也不自动恢复旧语义。
- 结构核对不修改字典或绑定。更新共享字典前展示引用范围，允许另存；新定义不改变其他对象或历史绑定。
  来源声明与人工补充冲突时展示双方内容，禁止自动覆盖人工值；未知信息不以空字符串或默认单位补齐。

### 保存、应用与状态

- 复用 F1 唯一版本库、精确引用、乐观并发、反向引用和独立保存/应用事务，不另建字典存储或修改普通 config 内容。
  v1 绑定继续可读写、导入导出；其 `declared_schema` 保持人工声明语义，不能改称实际观察。
- v2 的 `source_snapshot` 由服务端描述器获取，保留真实结构/配置依据；`confirmation` 记录被确认的来源版本、字段映射及待确认项。
  v2 对映射执行与来源快照的结构检查；如保留非空 declared_schema，还须通过原有声明检查。两者检查结果分别返回。
  F1 `structure_check` 的含义不变，实际观察使用 CheckResult；不向严格的 v1 文档暗加属性，不修改旧公共 IID 虚函数表。
- 人工 v1 绑定不会自动升级或标成观察通过；识别到同一 object_key 时可显式核对并保存 v2 新 revision，历史 v1 保持原样。
  pending/incomplete 可查看或继续人工维护定义，但不自动生成完整字典，也不创建声称来源结构已匹配的绑定。
- 应用请求携字典精确引用、映射、expected binding revision、预览时 object_key/source_revision/fingerprint 及显式确认项。
  服务端重新描述来源并比较预览基准，变化返回 409；映射不全、不匹配或确认项缺失拒绝应用。
  无法确认对象连续性时，须显式记录本次身份/语义确认；确认只代表用户声明，不伪装成库原生身份已证明。
- 保存字典仍使用 F1 save；随后应用失败要分别返回“字典已保存”和“绑定未更新”，保留表单和已提交版本。
  相同规范字典/绑定内容不增版；重新检查时间不导致增版，冲突不覆盖，不通过自动重试提高 expected 值。
- 状态为 `unbound|pending|matched|mismatch|needs_confirmation|unavailable`：无绑定、结构待定、观察匹配、结构不符、身份/语义待确认、来源不可用分别表达。
  matched 只表示该观察结构与映射一致且所需确认已记录；semantic_gaps 单列，data_validation 始终为 not_run。
  本 Feature 不跨来源与注册表做分布式事务或冻结源表；应用后的再次变化在下一次显式核对/对象详情刷新中反映。

## 插件与 HTTP 契约

- 在 T1 编码前落地新公共头文件、`IID_CHANNEL_DATASET_DESCRIPTOR_V1` 和以下纯虚契约；Kafka、Catalog、Database 各自描述其拥有的对象。
  本进程通过 IQuerier 枚举该 IID 并按 Provider 定位描述器，不假设 First 返回所有类型；不依赖具体 Provider 类。
  跨进程经 HTTP 路由，Web 不直连库或 Kafka。

```cpp
struct ChannelDatasetRequestV1 {
    std::string locator_json;
    uint32_t timeout_ms = 5000;
};
struct ChannelDatasetSnapshotV1 {
    std::shared_ptr<const std::string> document_json;
};
interface IChannelDatasetDescriptorV1 {
    virtual ~IChannelDatasetDescriptorV1() = default;
    virtual const char* ProviderId() const = 0;
    virtual int Describe(const ChannelDatasetRequestV1& request, ChannelDatasetSnapshotV1* out,
                         DataSemanticsErrorV1* error) = 0;
};
```

- 返回 0 成功，非 0 返回 F1 结构化错误并清空输出；JSON 按 DatasetSnapshot Schema 校验，快照独立拥有内容。
  ProviderId 为插件生命周期内稳定的非秘密标识；Describe 线程安全、只读取元数据，不隐式加载 DataFrame。
  四个外部操作均接受 timeout_ms，默认 5000、范围 1～30000；控制转发和数据库读取共用一次请求总预算，不逐层重置。
  不持注册表写锁进行 HTTP/数据库访问；不在逐行/逐包热路径查询字典。插件仍按 Option→Load→Start 批次加载。
- 各 Provider 的内部描述路由分别为 POST `/channels/kafka/dataset/describe`、`/channels/dataframe/dataset/describe`、`/channels/database/dataset/describe`。
  Config 负责字典预览、核对与绑定持久化，经描述能力取得事实；跨进程请求使用既有 Gateway/Router，不由客户端伪造观察快照。
- 对外新增以下 POST `/api/semantics/v1/channels/` 操作，内部对应 `/semantics/v1/channels/`；沿用 F1 成功/错误信封和访问边界。

| 操作 / operationId | 请求 | 响应 |
| --- | --- | --- |
| `describe` / describeChannelDataset | locator、timeout_ms? | 来源快照、checked_at；pending 不触发加载 |
| `preview` / previewChannelDictionary | locator、base_dictionary_ref?、field_map?、重命名/变更确认选择 | 默认或合并候选、基准版本、差异、待确认项；无持久写入 |
| `check` / checkChannelDictionaryBinding | locator、timeout_ms? | 当前精确绑定/字典引用及 CheckResult；重取来源，不修改绑定 |
| `apply` / applyChannelDictionaryBinding | locator、binding 名称、字典精确引用、映射、expected 值、来源基准和确认 | v2 绑定精确引用、created_revision、来源快照和检查结果 |

- 扩展 F1 的 JSON Schema Draft 2020-12、OpenAPI 3.1、静态发布、导入导出及引用索引；保留旧端点契约和 v1 样例。
  v2 完整样例可导入/导出追溯，但导入的来源快照仅为历史声明，应用到真实对象必须重新获取来源，不能凭导入文档标为当前匹配。
  通用 save/import 不授予观察可信性；只有 channels/apply 从描述器重取事实并核验，check 始终重取当前来源，不能从客户端传入的 matched/确认记录直接判定通过。
- 400 为 locator/文档/映射或确认非法，404 为对象/精确版本不存在，409 包含 REVISION_CONFLICT、SOURCE_CHANGED、BINDING_MISMATCH、CONFIRMATION_REQUIRED。
  413 为 LIMIT_EXCEEDED，503 为 SOURCE_UNAVAILABLE/UNAVAILABLE，504 为 SOURCE_TIMEOUT，500 为 STORAGE_ERROR；不解析 message 判断分支。
  沿用 F1 文档 8 MiB、请求/响应 12 MiB 和深度 64 上限；字段过多明确超限，不裁掉字段后保存或把异常写成成功状态。
- Kafka 通道、CSV 导入/命名 DataFrame 及数据库选表详情提供“生成默认字典 / 选择已有字典”、字段详情和补充入口，复用 `/semantics` 编辑能力。
  结构事实只读，来源待定时显示加载引导；展示差异、映射、确认项、精确引用及核对时间；保存和应用结果分别显示。
  刷新可重读，409/来源失败保留编辑，历史只读；同名替换展示旧/新身份及旧定义可复用选项，不自动迁移绑定。

## 主链路

1. **生成与绑定：** 用户导入/选定对象 → 描述器取得配置或结构（未加载则 pending）→ 默认字典或已有字典映射预览 → 补充语义/标准 → F1 保存精确字典版本 → 服务端重新核对来源并应用 → 页面/HTTP 读取同一 binding 与字典。
2. **变更与保留：** 用户刷新核对 → 比较来源版本、身份、结构与已绑定版本 → 展示增删/类型/可空性/声明变化和重命名候选 → 保留人工内容并显式确认 → 保存新字典或另存 → 应用到所选对象；旧绑定、共享引用和历史定义保持可追溯。

## Feature Task

- [x] T1：交付可追溯的来源快照、默认生成与变更核对及 v2 绑定契约，使结构事实、人工语义和确认依据分离且不破坏 F1；验收 A1、A2，以隔离描述器夹具冻结公共头文件、Schema 与 HTTP 模型。
- [x] T2：交付 Kafka 结构化 source 和 CSV/命名 DataFrame 的生成绑定闭环，使配置及真实 Schema 可复用，并保持懒加载、受管对象身份和同名替换保证；验收 A3、A4。
- [x] T3：交付四类数据库明确表的生成绑定闭环，使空表、限定表身份和原生字段事实可核验，连接或结构变化不会静默沿用旧定义；验收 A5。
- [x] T4：交付各对象接入/详情中的字段补充、字典复用及差异确认流程，使 Web 与 HTTP 使用同一版本并准确呈现冲突、未知和部分成功；验收 A6。
- [x] T5：交付持久化、并发、真实控制链路的统一验收及使用说明，使绑定可恢复、历史可追溯且既有通道和版本能力无回归；验收 A7、A8，并收口 A1～A6。

## 测试锚点与完成出口

| 编号 | 必测断言 |
| --- | --- |
| A1 模型与生成 | 新 Schema 正负例；默认字段与来源完整对应；缺口不伪装；未知语义及 producer 来源正确；预览无写入；稳定 field_id；v1 文档/端点兼容，v2 完整样例可交换 |
| A2 差异与绑定 | 增删/类型/可空性/顺序/生产者声明变化；重命名确认后保留 ID 和人工说明；删除引用需修订；映射全覆盖且不重复；共享/历史不变；缺确认拒绝；无变化不增版；预览后源变化返回 SOURCE_CHANGED |
| A3 Kafka | 用真实保存配置和实际 StructuredSchema 比较全部业务及五个元数据字段；unit/path 变化可识别；无 Consumer/网络/密钥解析调用、无位点变化；packet/sink-only 明确不支持；实体名不被推断成行语义 |
| A4 DataFrame | 实际 CSV 导入及注册 DataFrame 对照真实 Arrow Schema；空数据但 Schema 已知可生成；未加载列表/describe/check 不触发 Loader；显式加载后核对；重命名、追加、替换、删除重建及重启身份按契约处理 |
| A5 数据库 | SQLite/MySQL/PostgreSQL/ClickHouse 各走真实结构入口；空表、原生类型参数/可空性、限定表隔离、增删列/改类型核对；连接改指向及身份未知需确认；无业务行读取/写入；认证/权限/超时明确失败 |
| A6 API 与页面 | 实际请求/响应通过发布 Schema/OpenAPI 校验；普通客户端和浏览器在三类入口完成生成/选择、补充、标准引用、保存、应用、刷新及变更确认；冲突留表单，定义保存后应用失败显示准确；来源故障不显示旧 matched |
| A7 一致性 | 临时库跨进程恢复及双客户端并发；写入失败不暴露半个绑定；来源调用不持写锁；真实 Web→Gateway→Router→Provider/IID 得到一致快照；导入快照不能冒充当前观察；旧 v1 绑定与普通 config 内容保持 |
| A8 消费与回归 | 普通程序按公开契约定位对象→核对→取 binding/dictionary 精确版本及标准上下文；F1 版本/引用、配置、Catalog 懒加载、Kafka 配置/解码 Schema、数据库管理与路由的受影响回归全部通过 |

- T1～T5 顺序执行，每个 Task 完整验收后再勾选；Atomic Slice 的允许文件、target、命令/filter、依赖及停止条件只在工作台冻结。
  本规格不把实现文件数或时间盒当成 Feature Task 拆分依据；Task 状态以实际验收记录为准，不以单个切片代替完整任务验收。
- 新用例覆盖上述锚点；复用并按影响纳入 `test_data_semantics`、`test_data_semantics_api`、`test_data_semantics_browser`、
  `test_config_channel`、`test_config_channel_e2e`、`test_catalog_lazy_loading`、`test_kafka_structured`、`test_kafka_channel_admin`、
  `test_database_manager_unit` 和 `test_router`，以及实际改动入口的既有前端/数据库结构回归；实施前在工作台冻结最终清单与选择理由。
- 纯单测使用临时 SQLite、CSV、真实配置模型和隔离描述器；A3 的元数据发现不依赖 Kafka broker，不因此重跑数据面/认证矩阵。
  A5 必须使用真实四库：SQLite 为临时库，其余采用统一 `config/testing/environment.json` 的受管专用库和 fixture。
  按实际 native 执行位置运行 `tools/test.sh doctor`、`unit|integration` 及筛选范围，同步维护 dependencies 声明和资源锁。
  真实认证与目录读取权限、必要夹具 DDL 权限分别预检；不以 TCP/healthy 代替，不猜凭证或重置已有容器/卷，外部依赖缺失记未验收。
- A6/A7 使用隔离原生进程、随机本地端口及真实浏览器；同时验证 Provider 进程内 IID 和跨进程控制路由，不以 mock HTTP 替代完整接线。
  动态库测试持有 RAII 库所有权，实例及其快照先于库销毁；不启动无关协议、全仓、镜像或部署矩阵。
- 完成须 A1～A8 及冻结清单 selected=executed=passed，failed=skipped=not_executed=0；相关构建、格式与静态检查通过。
  重建默认启动所用受影响动态库、前端与公开契约并核验产物；隔离运行通过不等于用户服务已重载。
  更新 [现有使用说明](../../docs/data-semantics.md)，覆盖入口、未知、结构差异、复用/绑定、同名替换和 API 示例；证据放 `evidence/channel-data-dictionary/`，不入库。
  全部 Task 完成后再归档并将 Backlog 标记完成；不提前启动 F3。

## 完成证据

- T2 已完成（2026-10-28）：真实 Kafka/Catalog 描述、生成/补充/保存/绑定/核对、本进程 IID 与跨进程 Web→Gateway→Router、CSV 显式加载、持久身份和重启恢复均通过。
  最终 runner `20261028T093500Z-d4d4179a`：selected=executed=passed=18，failed=skipped=not_executed=0；源码与产物前后摘要一致。
  包含来源故障不沿用 matched、真实控制转发总预算/504、F1 API/Config E2E、Router、来源及框架相关回归；修复发布契约路径与错误码遗漏，修复卸载时路由回调和测试快照的库生命周期。
  默认相关 SO、嵌入公开契约和测试目标重建，格式/Python 编译/diff 范围通过；未重载用户服务，未提交或推送。证据 `evidence/channel-data-dictionary/t2-acceptance.log` 及 runner 目录，未入库。

- T1 已完成（2026-10-28）：公共描述 IID、来源快照/默认生成/差异核对/应用服务、v2 绑定兼容及四个操作的 Schema/OpenAPI 模型已交付。
  `test_channel_dictionary`、`test_channel_dictionary_schema`、`test_data_semantics`、`test_config_channel`：selected=executed=passed=4，failed=skipped=not_executed=0。
  最终 runner：`20261028T080414Z-91737661`；Config 默认动态库及测试目标重建，新增文件与改动行格式、版权、范围和 diff 检查通过。
  验证使用临时 SQLite 和注入式来源；未声称真实 Kafka/Catalog/Database 描述器或 Web 接线已完成。证据缓存 `evidence/channel-data-dictionary/t1-*`，未入库。


- T3 已完成（2026-10-28）：SQLite/MySQL/PostgreSQL/ClickHouse 真实空表、完整原生类型/可空性/限定表、增删改列、生成保存绑定与核对、持久连接身份、原生表重建隔离及连接改指向通过。
  runner `20261028T101406Z-0dceb563`：selected=executed=passed=20，failed=skipped=not_executed=0，源码/产物前后摘要一致。真实 native 认证预检通过；HTTP 本进程/跨进程 SQLite 和三外库握手总预算/504 通过，四驱动和数据库管理/Router 回归通过。
  标准配置派生独立受管项目 `flowsql_dictionary_20261028`，保留原有同名卷；重建旧 probe 及默认 Database SO。格式、Python 编译与 diff 检查通过。证据 `evidence/channel-data-dictionary/t3-*` 及 runner 目录未入库；未重载用户服务，未提交/推送。

- T4 已完成（2026-10-28）：DataFrame、Kafka 结构化 source、数据库限定表三个实际入口共用来源字典编辑器，真实 Chromium 完成生成、显式加载、人工说明/标准、精确复用映射、保存应用刷新和类型/生产者变更确认。
  runner `20261028T102956Z-baf9f92a`：selected=executed=passed=9，failed=skipped=not_executed=0；源码/产物前后摘要一致。409 保留表单、定义保存后应用冲突、来源删除清除旧匹配、F1 页面/API 回归均通过。
  默认 `build/output/static` 和相关 SO/宿主经 CMake 重建；Node/Python/diff 检查通过。缓存 `evidence/channel-data-dictionary/t4-*` 与 `browser/` 未入库；未重载用户服务、未提交推送。

- T5 与 Feature 已完成（2026-10-28）：最终 runner `20261028T105820Z-7417d24e`，selected=executed=passed=34，failed=skipped=not_executed=0；27 项能力/回归与 7 项标准依赖 fixture 全部通过。
  A1～A8 收口包含真实四库、三类入口 Chromium、本进程 IID/跨进程控制链路、总预算、重启、普通 OpenAPI 客户端精确 binding/dictionary/标准上下文及全部冻结的受影响回归。
  新增服务端原子观察记录：通用 save/import 不授予可信性，自洽导入仍需 observation 确认；channels/apply 重取来源后提交。验证事务末尾注入失败时 revision/current/引用/观察一起回滚、同内容不增版授予观察、双客户端应用仅一个成功、来源调用阻塞不持注册表写锁、v1 升级保留声明及历史。
  映射校验→版本冲突→变更确认顺序及幂等应用保持既有契约；同名替换展示旧/新身份和旧精确字典复用入口，禁止自动迁移绑定。已更新现有 `docs/data-semantics.md`。
  sources-before/after、artifacts-before/after 完全一致；默认相关 SO、`build/output/static` 和公开契约摘要存于 `evidence/channel-data-dictionary/delivery.json`，验收日志 `feature-acceptance.log` 与 runner 目录未入库。
  CMake 构建、改动格式、Python/Node、diff/范围及文档文件链接核查通过。未验证外部 Kafka 数据面/认证、容器交付或产品发布全量；未重载用户服务，未 commit/push，既有卷及非目标资源均保留。
