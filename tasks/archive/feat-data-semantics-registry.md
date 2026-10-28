# Feature：标准与字典管理（data-semantics-registry）

状态：`[x]` 已完成；优先级：P1。
所属 [Epic](../specs/epic-data-semantics-dictionary.md#data-semantics-registry)：F1；前置为已交付的配置资源、路由和 Web 管理能力。

## 业务意图与 Non-Goals

让数据维护者和程序调用方集中创建、查询、补充与复用标准和字典，通过同一套表单与 HTTP 契约取得一致的定义。
交付可重启恢复的版本库、精确引用、人工登记的数据对象绑定及引用关系；不完整的定义也可保存，但必须明确缺失信息。
本 Feature 以人工录入或文件导入完成独立闭环，为后续通道接入和算子契约提供基础。

- 不自动发现通道结构、接入真实数据源、随算子上传契约、生成算子结果字典或整理 NPM 领域内容；这些分别属于 F2～F4。
- 不改查询/探索消费入口，不读业务数据验证质量，不执行单位转换、任意表达式或语义规则；消费适配属于 F5。
- 不实现大模型、function calling、MCP、skills、诊断、语义检索或通用规则引擎。
- 不增加审批、多人协同草稿、历史覆盖/删除、自动升级全部绑定或运行任务热更新；不改普通配置通道的现有契约。

## 核心数据契约

### 身份、定义与语义

- `kind=standard|dictionary|binding`；同一部署内以 `(kind,name)` 唯一标识，名称匹配 `[a-z][a-z0-9_.-]{0,127}`。
- 精确引用为 `<kind>.<name>@<revision>`，如 `dictionary.app.requests@2`；禁止持久引用裸名称或 `@latest`。
  revision 系统递增，JSON 中为 1～9007199254740991 的整数；首次保存的 expected 值为 0，已有对象名称不可变。
- 文档格式与内容版本分开：文档 `schema_id` 分别为 `flowsql.data-standard.v1`、`flowsql.data-dictionary.v1`、`flowsql.data-binding.v1`；revision 描述内容修订。
  修改单位、解释或有效条件同样产生新 revision；真实结构标识独立于字典 revision，不以结构相同证明语义相同。

| 模型 | 必须支持的属性 |
| --- | --- |
| Standard | `schema_id,title,definition,scope`；`quantity,representation,unit,value_domain` 描述业务概念、表示与取值约定 |
| Dictionary | `schema_id,title,description,entity,fields`；实体包含 `row_meaning,row_semantics,identity_fields,revision_field,time_fields,relations,limitations` |
| Field | `field_id,name,type,nullable,title`；语义含 `definition,role,measure_kind,unit,time_semantics,null_meaning,validity,aggregation,value_domain,standard_refs` |
| Binding | `schema_id,object_key,object_label,dictionary_ref,field_map,declared_schema`；`object_key={provider_id,object_id}`，标识具体数据集而非整个数据库连接 |
| Revision | `exact_reference,revision,sha256,created_at,base_revision,change_note,document`；current 指向一个已提交 revision |
| ReadResult | 精确引用、文档或字段页、`semantic_gaps`、引用上下文、分页信息；定义声明与检查结果分别表达 |

- `field_id` 在同一字典内稳定且唯一，不由显示名称生成；字段名、类型与可空性是结构声明，普通语义编辑不能暗中改变它们。
  人工新建/导入可提供结构；修订结构需显式进入结构编辑并展示差异。F1 不把这些声明标成真实数据源的观察结果。
- `type={system,name,parameters}`：system 为 `arrow|database|opaque`，name 非空，parameters 为 JSON 对象；F1 按描述精确比较，不猜测跨类型系统的等价性。
- 除身份和必要结构外，语义属性统一用 `SemanticValue<T>={state,value,origin}`：state 为 `declared|unknown|not_applicable`；后两种 value 必须为 null。
  origin 为 `manual|imported|producer|derived`，只记录来源，不构成认证；未提供的语义归一为 unknown，空字符串不替代未知。
- `row_semantics` 的已声明值限 `event|snapshot|cumulative|delta|aggregate`；单位与时间表示独立于数值类型。
  身份、时间和修订字段引用 `field_id`；关系声明目标字典的精确引用和双方字段，不自动执行 Join。
- 字段 role 为 `identity|dimension|time|measure|state`；measure_kind 为 `gauge|cumulative|delta|ratio|statistic`，允许同一快照含累计量和增量列。
  `unit={code,label}`，code 为规范非空单位标识；`time_semantics={kind,unit,epoch,timezone,description}` 区分时间点/时长及未知的基准、时区。
- `validity` 为 `{mode:descriptive,description,field_ids}`；aggregation 为 `{operation,condition}` 列表，operation 限 `sum|count|mean|min|max|percentile|none`，condition 采用同一描述结构。
  `value_domain` 为 `{kind:enum,items:[{code,label}]}` 或 `{kind:range,min,max,min_inclusive,max_inclusive}`；枚举 code 使用字符串保留数值码精度。
  F1 只校验定义形状、内部字段引用及目标存在，不执行这些条件，不输出“数据符合标准”的结论。
- `standard_refs` 和实体关联只引用已存在的精确版本；同名不同版本可区分，错误类型/缺失目标明确失败，不在读取时悄悄换新版本。
  首期标准不再引用其他标准；字典读取返回直接引用上下文，不递归展开整个关系网络。
- `semantic_gaps` 由未知属性路径生成，表示说明未补全；必需结构缺失仍拒绝保存。格式合法、语义已声明、实际数据已验证互不等价。

以下是字段语义的局部示例，并非完整导入文档；实现必须提供可直接导入的标准、字典、绑定完整样例。

```json
{
  "definition": {"state": "declared", "value": "请求耗时", "origin": "manual"},
  "unit": {"state": "unknown", "value": null, "origin": "manual"},
  "null_meaning": {"state": "declared", "value": "没有可用测量值，不等于零", "origin": "manual"}
}
```

### 保存、绑定与引用

- 保存命令为 `{kind,name,expected_current_revision,base_revision?,change_note?,document}`；document 的 schema_id 必须与 kind 一致。
  base_revision 仅允许同一对象的已有版本；恢复历史是保存其副本为新版本，不回拨 current。
- JSON/YAML 经同一类型化模型归一为确定性 JSON：对象键排序、数组顺序保留、未填语义补 unknown，保留整数精度。
  拒绝非有限数值、重复键、未知属性、YAML 自定义标签/alias；不接受 XML、远端 Schema 或网络引用解析。
  origin 保留显式来源，不因格式转换而重写；缺省 origin 统一为 manual，导入行为另记操作来源，不改变已声明事实的来源。
- 保存先校验文档与引用，再在写事务内比较 current；与当前规范内容相同则返回原版本和 `created_revision=false`。
  内容不同时必须匹配 expected 值，冲突不覆盖；current、内容、摘要和引用索引同事务提交，失败不产生可见版本。
- 引用关系由文档生成，不能单独编辑；支持按目标精确版本查询当前引用与历史引用，分别标明来源版本、字段路径和是否为 current。
- binding 也采用不可变 revision；`object_key` 在部署内唯一，创建后不可修改，object_label 可变。
  修改绑定需指定 expected revision；字段映射为字典 field_id 到来源字段名，同一来源字段不得重复绑定。
- `declared_schema` 为 null 或有序的 `{name,type,nullable}` 列表，来源固定为人工/导入声明；不接触实际通道。
  两种情况均要求映射覆盖全部字典字段；空结构可保存并返回 `structure_check=unknown`，有结构时映射须存在且类型/可空性一致，额外源列允许存在。
  缺列、重复字段或不一致映射明确拒绝；`matched` 只表示与给定结构声明相符，`data_validation=not_run` 始终成立。
- 保存定义与更新绑定是两次独立操作；新定义不改变已有绑定。“保存并应用”先保存，再带 expected 值更新所选绑定。
  若应用失败，页面分别显示“定义已保存”和“绑定未更新”，保留新版本，不假称全部失败或回滚已提交内容。
- 绑定详情解析出字典精确版本及其标准引用；后续新版本不改变已返回的快照。实际任务引用接线由 F3/F5 完成，F1 不补造旧任务依据。

### 持久化与进程内接口

- 在现有 Config 插件中增加独立语义注册能力及 `IID_DATA_SEMANTICS_REGISTRY_V1`；其他插件通过 IQuerier 查询，不依赖具体 Provider 类。
  现有 `IConfigChannelRegistryV1::Resolve()` 只读，不能承担发布或绑定；保持其 ABI、普通配置内容及原有 HTTP 行为。
- 沿用配置资源的部署持久目录、SQLite、解析和版本事务能力，在同一数据库中用独立 `semantics_object/revision/reference` 表保存上述事实。
  对象类型含 binding，object_key 唯一约束及 current 更新在事务内维护；不将同一字典另复制为普通 config 快照或第二套持久真相。
- 新表迁移必须事务化且可重复；失败不损坏既有配置，不能静默降级为仅内存。索引为可重建派生数据，查询不得暴露内容/引用不一致的 revision。
  写入、并发和重启保证复用并验证现有机制；不跨独立服务事务拼接“原子保存”，不在逐行/逐包数据面查询注册表。

```cpp
struct DataSemanticsErrorV1 {
    std::string code, message, field_path;
};
struct DataSemanticsSnapshotV1 {
    std::string exact_reference, schema_id, sha256;
    std::shared_ptr<const std::string> document_json;
};
interface IDataSemanticsRegistryV1 {
    virtual ~IDataSemanticsRegistryV1() = default;
    virtual int Resolve(const char* exact_reference, DataSemanticsSnapshotV1* out,
                        DataSemanticsErrorV1* error) = 0;
};
```

返回 0 表示成功，非 0 配合结构化 error；失败不返回旧快照。读取线程安全、内容独立拥有生命周期，调用者不持有 SQLite 句柄。
公共头文件在 T1 编码前按此契约落地；控制操作在插件路由内使用同一校验/事务服务，不扩展旧配置 IID 的虚函数表。

## HTTP 与管理页面契约

- 服务内部前缀 `/semantics/v1/`，Web 代理 `/api/semantics/v1/`；以下操作均为 POST JSON，沿用 Web→Gateway→Router 控制链路。
- 用 JSON Schema Draft 2020-12 定义所有文档、请求和响应，使用 OpenAPI 3.1 描述端点、唯一 operationId、示例及错误。
  只接受随版本发布的 Schema；校验器必须覆盖这些 Schema 实际使用的关键词，不能用 JSON 语法检查代替结构校验。

| 操作 / operationId | 请求要点 | 响应要点 |
| --- | --- | --- |
| `list` / listDataSemantics | kind、名称/字段/说明关键词 q、cursor、limit | current 摘要、精确引用、缺失语义数、next_cursor |
| `resolve` / resolveDataSemantics | exact_reference、view=document/fields、field_ids?、cursor、limit | 完整文档或明确标记的字段投影、实体上下文、标准引用、semantic_gaps |
| `history` / listDataSemanticsHistory | kind、name、cursor、limit | revision、摘要、时间、base_revision、change_note |
| `save` / saveDataSemantics | 上述保存命令；含 binding | exact_reference、created_revision、semantic_gaps；binding 另含检查结果 |
| `references` / listDataSemanticsReferences | exact_reference、scope=current/history、cursor、limit | 来源精确版本、引用位置、current 标记 |
| `binding_lookup` / lookupDataSemanticsBinding | object_key | 当前 binding 精确引用和 dictionary_ref；随后可精确 resolve |
| `import` / importDataSemantics | kind、name、format=json/yaml、content | 归一化待保存 document、缺口或校验错误；不写入、不接受导入自定 revision |
| `export` / exportDataSemantics | exact_reference、format=json/yaml | kind、name、source_reference、document 的导出信封；可回导，revision 仅为来源信息 |

- 成功响应为 `{data:...}`；错误为 `{error:{code,message,field_path,details}}`，不以 HTTP 200 包裹业务失败。
  400 对应格式/内部引用非法，404 为对象或精确版本不存在，409 为 expected 冲突或绑定结构不匹配，413 为超限，503 为 Provider 不可用，500 为持久化错误。
  code 至少区分 `INVALID_DOCUMENT`、`INVALID_REFERENCE`、`NOT_FOUND`、`REVISION_CONFLICT`、`BINDING_MISMATCH`、`LIMIT_EXCEEDED`、`UNAVAILABLE`、`STORAGE_ERROR`；程序不解析 message 判断错误。
- resolve 默认 `view=document`，返回完整规范文档；`view=fields` 只用于字典，返回独立的投影响应 Schema 和分页字段，不冒充完整文档或用于直接覆盖保存。
  import 接受原始文档或 export 信封；信封 kind 须匹配请求，name 可显式改名，source_reference 只作来源，引用的标准/字典须已存在，不批量隐式导入依赖。
- 文档/导入原文限 8 MiB、控制请求限 12 MiB、嵌套深度限 64，沿用当前配置路径已支持的边界；超过边界明确失败，不截断后保存。
  查询 limit 默认为 50、范围 1～100；响应限 12 MiB。列表/历史不展开内容，超预算分页或明确 413，不静默丢字段。
- 游标绑定筛选与排序；列表按 kind/name、历史按 revision 降序、字段按定义顺序。列表不是跨请求快照，每项必须携带当次精确引用。
  字段页固定同一 revision，返回 next_cursor 和 has_more；不认识的 field_id 报错，不能悄悄忽略。
- 选择字段仍返回行粒度、身份/修订/时间/局限、被引用字段的必要结构及精确标准引用；未展开内容列在 required_context_refs，不能标为完整展开。
  缺口返回 JSON Pointer 路径及原因；单位未知不填默认值，未做业务数据验证不输出质量通过结论。
- 模式描述随发行物提供 OpenAPI 与 JSON Schema 文件，并以普通静态 GET 提供 `/api/semantics/v1/openapi.json` 及其本地 Schema 引用。
  Web 与普通客户端使用同一接口；沿用部署现有访问控制边界，不新增一套身份系统，不声称获得此前不存在的对象级权限隔离。
- Web 新增“标准与字典”管理入口：按类型/关键词搜索、实体信息与字段表、缺口提示、标准关联、引用查看、JSON/YAML 导入导出和版本历史。
- 新建/编辑提供结构与语义分区；标准选择固定版本，历史只读。“保存新版本”和“应用到对象”分别显示结果，版本号由系统维护。
  结构编辑显示差异；修改共享定义前展示已有引用，允许另存新名称以保留共享定义。binding 可人工登记、查看映射及修订。
- 编辑内容仅为未提交表单，不承诺服务端草稿；409/保存失败保留输入，离开未保存页面有提示；预览转义显示，保存成功后刷新可读同一版本。

## 主链路

1. **创建与补充：** 用户录入或导入标准/字典 → 共用模型校验并展示缺口 → 携 expected 保存 → 单事务发布内容与引用 → 返回精确版本 → 页面及 HTTP 重读一致；后续修改发布新版本。
2. **绑定与追溯：** 用户选择数据对象声明、字典精确版本及字段映射 → 校验声明结构 → 保存 binding → 从 object_key 取得绑定引用并精确解析 → 字典发布新版后旧绑定/历史仍可读，显式应用才产生新 binding revision。

## Feature Task

- [x] T1：交付类型化标准/字典的不可变版本存取与精确快照，使人工定义可持久保存、恢复且不破坏既有配置资源；验收 A1、A2，冻结公共头文件及对应 JSON Schema。
- [x] T2：交付数据对象绑定和版本化引用关系，使共享定义的使用范围、声明结构匹配及历史依据可查询且更新不误覆盖；验收 A3。
- [x] T3：交付受 JSON Schema / OpenAPI 约束的管理查询与导入导出接口，使普通程序能按需读取、识别缺口并可靠保存；验收 A4、A5。
- [x] T4：交付统一管理页面，使用户通过表单完成搜索、补充、版本保存、应用和追溯，并在冲突或失败时保留编辑内容；验收 A6。
- [x] T5：交付跨重启、并发和真实控制链路的一致性验收及使用说明，使 Web/HTTP/进程内消费得到同一版本事实且旧配置能力无回归；验收 A7、A8，并收口 A1～A6。

## 测试锚点与完成出口

| 编号 | 必测断言 |
| --- | --- |
| A1 定义 | 标准/字典 Schema 正负例；缺失语义可保存但必需结构不可缺；重复字段/非法内部引用/错误标准版本拒绝；完整样例可校验且未知值不伪装 |
| A2 版本 | 首次 revision=1；规范内容相同不增版；并发仅一方新写成功；历史恢复增版；失败/回滚不暴露版本；旧快照读取及普通 config 行为保持 |
| A3 绑定 | binding Schema 正负例；object_key 唯一、精确引用、映射校验、未知结构可识别；已声明匹配不代表实际验证；定义更新不改绑定；当前/历史反向引用准确 |
| A4 HTTP | 实际请求/响应匹配发布的 Schema/OpenAPI；错误状态与 code 一致；搜索覆盖字段/含义；字段裁剪保留必要上下文，分页/无效游标/预算可核验 |
| A5 交换 | 导入预览不产生 revision；JSON/YAML→统一模型→导出→回导一致；拒绝重复键/非法标签/网络引用/超限；导出来源版本不成为新对象的自定 revision |
| A6 页面 | 实际浏览器完成创建、字段补充、标准选择、保存/应用、引用/历史和导入导出；刷新可读；结构显式修改；冲突保留输入、部分成功显示准确 |
| A7 持久化 | 两个客户端并发、两个进程先保存后恢复、可控写入失败；真实 Web→Gateway→Router→Provider 与 IID 读取内容/摘要一致，已有配置记录逐字保留 |
| A8 消费与回归 | 普通 HTTP 客户端按 OpenAPI 完成发现→取得精确版本→读取缺口/限制→绑定→追溯；相关配置单测/E2E、前端和路由回归通过，无模型或外部数据源依赖 |

- 首选隔离原生进程、临时 SQLite、固定 JSON/YAML、随机本地端口及浏览器夹具；不启动 Kafka/外部数据库，不把整个仓库或镜像矩阵作为默认门槛。
- 实施切片在工作台冻结具体 target、标准测试入口/filter、前端/Schema 验证命令及依赖声明；共享 Config Provider 的回归必须包含现有配置存取和真实代理链路。
  JSON Schema 校验依赖按选定工具声明，禁止因工具缺失而跳过；动态库测试保持实例/快照先于库所有者销毁。
- T1～T5 全部验收，冻结清单 selected=executed=passed 且 failed=skipped=not_executed=0；格式/静态检查通过。
  默认启动所用 Config/Web 产物需重建并核验身份，运行时验证在隔离进程完成，不据此声称用户服务已重载。
- 使用说明涵盖未知信息、版本/绑定、保存与应用、导入导出及接口调用；日志与摘要存入 `evidence/data-semantics-registry/`，不提交。
- T1～T5 已逐项验收并勾选；本 Feature 已归档，后续 Epic Feature 保持待办。

## 完成证据

- T1 已完成（2026-10-28）：公共 IID、类型化 Schema、归一化、同库独立语义表、事务版本及快照已交付。
  `test_data_semantics` 与 `test_config_channel`：selected=executed=passed=2，failed=skipped=not_executed=0。
  标准/字典样例通过 Draft 2020-12 校验；C++ 格式与 diff 检查通过。
  本地证据：`evidence/data-semantics-registry/t1-*.log`，runner `20261028T040507Z-de0672d8`，未入库。
- T2 已完成（2026-10-28）：人工绑定、不可变 object_key、完整映射和声明结构匹配、当前/历史反向引用通过。
  `test_data_semantics`、`test_config_channel` 2/2，零失败/跳过/未执行；三份样例 Schema 校验和格式检查通过。
  本地证据 `t2-*.log`，runner `20261028T041030Z-9d72b2bd`；该检查点 T3～T5 未执行。
- T3 已完成（2026-10-28）：Config → Gateway → Router → Web 真实控制链路提供八个管理 POST 操作、静态 OpenAPI/Schema、字段投影、游标分页和 JSON/YAML 导入导出；错误状态与结构化 code、预览不写入、导出信封回导和 8/12 MiB 及深度边界通过。
  `test_config_channel`、`test_config_channel_e2e`、`test_data_semantics`、`test_data_semantics_api`：selected=executed=passed=4，failed=skipped=not_executed=0；构建目标 `test_data_semantics_host test_data_semantics test_config_channel test_config_channel_e2e`、发布 Schema/OpenAPI JSON 检查、Python 编译、C++ 格式和 diff 检查通过。
  本地证据 `evidence/test-environment/20261028T061346Z-66b002d2` 及 T3 检查点日志，未入库；该检查点 T4～T5 未执行。

- T4 已完成（2026-10-28）：新增 `/semantics` 管理入口和状态/来源语义表单，结构显式编辑及差异、精确标准/字典选择、人工绑定、引用/历史、保存及独立应用、导入导出和未保存离开提示可用。大整数在浏览器 JSON 读写中无损保留。
  实际 Chromium 在真实控制链路完成创建、字段补充、刷新、绑定、当前/历史读取、结构修改、历史恢复、另存新名称、导入导出、转义预览、定义冲突输入保留及保存成功/应用冲突的独立结果。
  浏览器及环境夹具 selected=executed=passed=2（runner `20261028T064454Z-585d65a2`），API 1/1（`20261028T064208Z-69087230`），Node 单测/旧 Config 前端回归 16/16；失败/跳过/未执行均为0。
  默认前端静态包与 `test_data_semantics_host` 构建成功，Python/JS 语法及 diff 检查通过。证据 `evidence/data-semantics-registry/t4-*.log`、`browser/`，未入库；该检查点 T5 未执行。

- T5 已完成（2026-10-28）：两个 HTTP 客户端同步竞争仅一方发布成功；引用插入故障使 revision/current/reference 同事务回滚；两个独立进程先保存后恢复，HTTP/IID 的文档与摘要一致，语义操作前创建的旧 config 内容逐字保持。
  普通 Python 客户端按发布 OpenAPI 的 operationId 发现接口，读取对象精确绑定、字段投影/缺口/局限及历史引用；完整说明见 [使用指南](../../docs/data-semantics.md)，README 已链接。
  最终冻结 CTest：`test_config_channel`、`test_config_channel_e2e`、`test_data_semantics`、`test_data_semantics_api`、`test_data_semantics_browser`、`test_router`，加浏览器环境夹具，共 selected=executed=passed=7，failed=skipped=not_executed=0；runner `20261028T070404Z-1b456de0`。
  Node 新增/旧 Config 前端回归 16/16；三份文档样例、八个 OpenAPI 请求示例、三份运行目录契约字节一致、文档链接、Python/JS 语法、C++ 格式/版权和 diff 检查通过。
  默认 `build/output/libflowsql_config_channel.so`、`libflowsql_web.so`、前端静态包已构建并在隔离真实链路验证；公开 Schema/OpenAPI 同步至 `build/output/config/semantics/`。没有重载用户运行服务，没有执行外部数据源/容器/全仓矩阵。
  本地证据 `evidence/data-semantics-registry/t5-*.log`、`browser/`、`closeout.json` 及 `final-run-summary.json`，包含源码/产物摘要，均未入库。仅完成 Epic F1，不提前实施或标记 F2～F5。

## 现状依据

- [配置 Provider](../../src/channels/config/config_channel_provider.h)及[事务实现](../../src/channels/config/config_channel_provider.cpp)；[现有只读 IID](../../src/framework/interfaces/iconfig_channel_registry.h)。
- [当前控制面限额](../../src/channels/config/config_channel_control.cpp)、[Web 代理](../../src/services/web/web_server.cpp)、[配置页面](../../src/frontend/src/views/ConfigChannels.vue)。
- [JSON Schema 2020-12](https://json-schema.org/draft/2020-12/json-schema-core.html)、[OpenAPI 3.1.1](https://spec.openapis.org/oas/v3.1.1.html)。
