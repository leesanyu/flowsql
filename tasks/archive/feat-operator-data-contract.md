# Feature：算子输入输出契约（operator-data-contract）

状态：`[x]` 已完成；优先级：P1；完成日期：2026-10-30（北京时间）。
所属 [数据标准与数据字典 Epic](../specs/epic-data-semantics-dictionary.md#operator-data-contract)：F3。
依赖已完成的 [标准与字典管理](../archive/feat-data-semantics-registry.md) 与 [通道数据字典生成与绑定](../archive/feat-channel-data-dictionary.md)。

## 业务意图与 Non-Goals

让内置及扩展算子的作者一次声明输入形态、字段角色、参数约束和输出保证，让使用者在执行前知道实际数据是否满足要求，并能解释算子产生的字段。
交付后的可观察结果是：静态契约随 C++/Python 算子注册或上传被查询；执行时系统结合实际输入和既有算法参数，自动取得对应契约并解析字段角色；返回兼容、不兼容或未确定的判定，按声明规则生成带来源与算子版本的输出字典。
普通程序和 AI 使用同一份可校验 JSON，能够定位复杂配置分支、输入字段要求和各输出实体的必要解释上下文，无需从 SQL 或长篇说明中猜测契约。

- 不修改既有 `IOperator`、`IStreamOperator`、`IBlockTransformOperator*` 或 C++ 插件 ABI 的既有虚表，不因缺少契约而禁用历史算子。
- 不实现任意代码的静态分析、通用 SQL/表达式语义推导、样本数据质量验证、自动单位换算或隐式字段猜测。
- 不整理 NPM 业务字典、查询/图表消费入口、审批流程、大模型/MCP/Skills 或新的数据面；这些分别属于 Epic 的 F4/F5 或其他 Feature。
- 不把注册成功、Arrow/数据库结构相同或算子执行成功当作业务语义已经验证；契约声明、结构检查和数据验证分别记录。
- 不增加 Python 多输入、任意流 DAG、数据源/后端或任务恢复能力；元数据中能够描述一种形态，不代表现有执行入口能够运行它。
- 不交付 Baseliner 全量领域语义包，不重验完整预测算法、恢复或全部后端组合；执行隔离只保证已覆盖入口的检查与实际执行一致，不重构通用 Worker 生命周期或新增任意热更新能力。
- 不向 SQL 增加 `data_contract` 保留键，不要求调用者传契约文档、契约版本或独立角色映射；SQL 仍只表达来源、算子、算法参数和目标。

## 交付范围与现状边界

| 入口 | 本 Feature 的保证 |
| --- | --- |
| 内置与 C++ 扩展 | 内置 `builtin.passthrough` 携带复制契约；V1/V2 扩展均可选带契约，单/多输入以实际能力为准，C++ 多输入夹具验证角色隔离 |
| Python 注册、创建与上传 | 装饰器和现有 `.py` 创建/上传支持内嵌文档或精确引用；保持当前单 DataFrame 输入/输出，使用真实 Worker/Arrow IPC 验收 |
| 阶段执行及输出 | 覆盖现有 DataFrame 批处理/THEN 与 Arrow 块转换入口；主输出由 Scheduler 观察，命名副输出由生产者在实际结果创建时调用同一记录接口，非 NPM 双实体夹具验收 |
| Baseliner 复杂契约 | 随组件声明嵌套配置、按 dataset/metric 展开的输入和条件输出；按下述场景→能力对应表选择非 NPM DataFrame/SQLite 的代表组合，覆盖 Value、Ratio、Relation 及四类输出，不改变算法与已有运行能力 |
| 其他已有算子 | 可以登记和查看声明；没有接入输出观察的路径明确 `output_check=not_observed`，不得生成声称匹配的结果绑定；NPM 具体实体和托管结果接线属于 F4 |

目前 `OperatorMeta` 只含目录属性，Python `OperatorBase.work()` 是单输入，C++ V2 的 `contract_iid` 用于选择执行能力。
本 Feature 为目录和执行阶段补充独立的语义能力；保留 SQL 现有入口选择及插件租约，不将数据契约 IID 填入执行能力的 `contract_iid`。

## 契约模型

### JSON 规范形式

- JSON 是发布、存储、HTTP/IID 交换与版本比较的唯一规范形式；发布 JSON Schema Draft 2020-12 和 OpenAPI 3.1。YAML 仅为作者编辑/导入格式，经 F1 同一类型化模型归一为确定性 JSON，不维护另一套契约。
- 一个契约版本包含概要、配置 Schema、输入模板、输出结构和语义规则；复杂内容可放入文档的 `resources`，以 `#/resources/<name>` 的文档内 JSON Pointer 引用。Schema 内部可复用 `$defs`。
  作者可分文件维护，发布时必须合并为一个引用闭合的不可变 JSON 文档；局部引用与文档一起增版，外部标准/字典/配置只使用已有版本库的精确引用，不解析网络 URL 或可变文件。
- `description` 用于解释语义，结构约束写入 Schema，字段选择与输出推导使用下述有限规则；不能仅提供大段自然语言并声称机器可校验。JSON Schema 不自动证明跨字段数值关系、模型就绪或真实数据质量。

### 静态定义与运行时实例

| 层次 | 产生时机与责任 |
| --- | --- |
| 算子契约 | 作者在编写/注册/上传算子时提供，声明输入要求、配置 Schema、字段选择规则、输出结构或推导规则；发布为静态不可变版本，不由 SQL 构造或覆盖 |
| 调用绑定与检查 | 系统在规划/执行时读取组件已关联的契约，结合实际数据集、Schema 和既有 WITH 参数解析字段映射；只产生本次调用实例和检查结果，不修改契约 |
| 结果字典与使用依据 | 固定输出可直接引用静态字典；依赖输入或参数的输出按静态规则实例化，在观察到真实结构后记录结果字典、输入及契约精确版本 |

静态契约允许描述参数化行为，例如“measure 角色由 value_column 参数指定”“复制全部输入字段”或“按 mode 选择输出分支”。
这些规则在发布时确定，具体字段和输出 Schema 在运行时解析；参数变化不会让契约本身产生新版本，作者修改声明才发布新版本。

### 身份与版本

- 算子身份沿用 `(category,name)`，`operator_version` 是作者声明的非空版本；没有声明版本的历史算子显示 `version_state=unknown`。
- 契约是不可变版本文档，精确引用为 `operator_contract.<category>.<name>@<revision>`；注册或上传不覆盖旧版本，内容未变化不增版。
- 执行依据同时记录 `(category,name,operator_version,contract_ref,component_revision)`；`component_revision` 由宿主取实际 `.so`/Python 源码内容摘要，内置算子取所属构建产物摘要，不由上传者自报。
  该摘要标识直接组件，不宣称封存所有依赖或证明行为正确。契约文档不内嵌自身组件摘要，避免随组件携带时的自引用。
- 契约文档采用 `schema_id=flowsql.operator-data-contract.v1`，标准/字典引用必须是 F1 的精确版本；输出绑定引用 F1/F2 的字典/绑定，不复制另一套字段语义模型。
- 使用依据为 `kind=operator_use`、`schema_id=flowsql.operator-data-use.v1`，引用为 `operator_use.<run-stage-name>@<revision>`；由执行路径写入，普通 save/import 不得制造已观察记录。
  两种新 kind 复用 Config 的版本库、冲突检测、引用索引及历史读取；旧 standard/dictionary/binding 文档与 IID 不变，不另建语义定义存储。

### 核心结构

| 对象 | 必须支持的属性 |
| --- | --- |
| `OperatorContract` | `schema_id,title,operator{category,name,version},configuration,inputs,outputs,resources?,limitations`；缺口由读取方生成 |
| `Configuration` | `invocation_schema,normalized_config_schema?,decoded_parameters?,affects`；Schema 内嵌或引用 resources，支持嵌套 object/array、required、枚举/范围、oneOf/const 与条件分支；开放参数由 additionalProperties 明示，affects 指向受影响的输入/输出/语义资源 |
| `InputPort` | `port_id,min_count,max_count,shape,roles,repeat_from_config_pointer?,item_key_pointer?,role_templates?`；shape 为 `dataframe`、`record_batch`、`stream_batch` 或 `opaque`；集合模板按归一配置数组展开 |
| `FieldRequirement` | `role_id,required,types,nullable,semantic_requirements,selector`；selector 支持固定 column、已有 parameter.key，或 config 的 scope/pointer/cardinality；cardinality 为 one/many |
| `RoleTemplate` | `repeat_from_pointer,item_key_pointer,discriminator_pointer?,cases`；在当前输入项中展开 metric 等数组，按枚举分支声明角色及必要的子数组模板，不解释任意表达式 |
| `OutputPort` | `port_id,shape,mode,structure_ref?,dictionary_ref?,rules,variants?,availability?,row_cases?,context_semantics?`；mode 为 fixed、derived、configured 或 unknown；多实体使用不同 port_id |
| `RowCase` | `case_id,when,field_rules`；when 为判别字段与常量的相等条件，field_rules 描述条件 NULL 含义、语义精确引用及说明，不改变物理 Schema |
| `Derivation` | `kind,output_field,source_port,source_roles,unit_rule?,aggregation_rule?`；kind 为 `copy`、`rename`、`unit_convert`、`aggregate` 或 `unknown`，投影是显式选择要保留的字段集合 |
| `ResolvedInvocation` | `contract_ref,component_revision,inputs[{port_id,item_key?,input_index,object_key,binding_ref?,source_revision,role_map}],effective_parameters,normalized_config?,config_refs,execution_context`；系统生成，保存参数/配置与实际来源、目标能力；不是 SQL 入参 |
| `CompatibilityResult` | `state,reasons,checked_refs,output_previews,semantic_gaps,data_validation=not_run`；state 为三态，每项 reason 含 JSON Pointer、预期/观察值和检查依据 |
| `OperatorUse` | `run_id,stage_index,operator_ref,component_revision,contract_ref,input_snapshots,role_map,effective_parameters,normalized_config?,config_refs,execution_context,checks,outputs,outcome`；outputs 按 port_id 保存真实结构、字典/绑定引用及 semantic_contexts |

字段类型、SemanticValue、单位、时间、NULL、有效性和聚合说明沿用 F1；types 为允许的完整类型描述集合，空集合表示未声明类型限制。
允许未知语义，但端口/角色身份、必要结构、selector 及内部引用必须合法；selector 只能引用算子已有参数/归一配置，不为元数据额外创造算法参数。
config selector 的 scope 为 root、input_item 或 role_item，pointer 从相应对象起算；many 返回列名数组，每个角色实例保留项身份与数组下标。数组展开单独声明，JSON Pointer 不支持 `*` 通配符。
常规端口按端口及序号绑定；集合端口由既有来源适配器按配置项身份关联实际数据对象，一个 SQL 来源可以包含多个 dataset，但不增加 Python 多输入能力。
FieldRequirement.nullable 为 false 时要求来源不可空，为 true 时允许两种可空性，为 null 时未声明限制；类型集合按完整类型参数比较，不隐式转换或跨 Arrow/数据库类型猜测等价。
输出固定字典引用精确版本；configured 只按已声明参数值选择有限分支，derived 只执行下述规则。未知分支不退回第一项，不解释任意表达式或加载用户代码推导元数据。

### 自动解析、参数与三态检查

- 每阶段 WITH 保持现有算法参数。Scheduler 从本次解析到的组件关联中取得契约精确版本并冻结，调用者无需选择或传递 contract_ref；确实未声明契约的旧调用继续执行并返回 undetermined。
  声明契约是算子作者的可选注册能力，兼容历史算子；本 Feature 所交付的内置示例及契约验收夹具必须提供声明。已经声明但文档丢失/服务不可用时须报错，不冒充未声明。
- 系统按静态 selector 解析角色：固定列名直接定位，参数选择使用已有算法参数及其默认值；用户不另填 role_map，不从“像时间/数值”的字段名猜测。
  必需字段参数缺失或明确选中的列不存在为 incompatible；语义信息不足仍为 undetermined。固定字段算子可无需 WITH；字段可配置算子只使用原有字段参数。
- 输入绑定由宿主从实际对象取得；执行前核对已解析 object_key/source_revision，来源变化返回冲突，不执行依赖旧快照的计划。
  检查使用实际阶段输入 Schema，包括已加载 DataFrame、前一阶段输出和实际适配后的 Arrow 结构；字典声明不能替代观察。列表/预检不隐式加载懒数据，结构未就绪为 undetermined，执行取得 Schema 后再检查。
- 参数先按现有调用规则解码，再校验 invocation Schema；JSON 文本参数在 decoded_parameters 声明 key、encoding=json 与 schema_ref，解码后校验内部结构，不能只校验为 string。
  归一配置来自该算子已有配置解析路径，同一份有效值供算法与元数据使用；Schema 的 default 仅作注释，不自行填充执行默认值。精确配置引用连同归一快照冻结，不重新跟随 latest。
  开放参数按旧接口传递，未声明的输出影响保持未知；凭证/连接秘密不写入定义或证据，也不作为语义推导依据。元数据引擎不另写一套算子业务配置解析器。
- 有效配置与检查按下表分工；T1 冻结独立配置适配接口及请求/响应 Schema，不修改旧虚表。请求使用既有参数和宿主提供的来源/目标上下文；响应复用 ResolvedInvocation 的 effective_parameters、normalized_config、config_refs 及输入项关联，不另建配置模型。
  C++ 适配能力经独立 IID/IQuerier 访问，Python 由 Worker 宿主提供对应协议；组件专属解析留在适配层，Scheduler/Config 不按算子名称增加业务解析分支。

| 责任方 | 交付与限制 |
| --- | --- |
| 算子适配层 | 复用生产配置解析路径，提供含有效默认值的非秘密快照、精确配置版本及配置项→实际输入对象/字段适配依据；与算法共用有效值，不执行算法或为预检加载业务数据 |
| 通用契约层 | 校验 Schema、展开有限模板、三态检查及推导候选字典；不补业务默认值、不解析算子专属表达式、不代替来源适配 |
| Scheduler/执行宿主 | 取得实际组件及来源，编排适配→检查→执行→观察，冻结租约/参数/配置与语义快照；确保被检查的有效值就是本次算法使用的值 |

- 首期机器检查为输入数/形态、角色列存在、类型集合与可空性，以及字典中明确声明的单位、时间表示、measure_kind 和标准精确引用的相等关系。
  F1 validity/aggregation 中的描述性条件不执行；单位等未声明、来源绑定待确认、无法关联适配后的字段或检查规则不支持，均为 undetermined，并给出未检查项。
- 任一明确违反为 incompatible；全部所要求检查通过才为 compatible；其余为 undetermined。compatible 只表示声明与所观察结构满足列出的要求，data_validation 恒为 not_run。
  已覆盖执行入口在调用算法前拒绝 incompatible；undetermined 可按旧能力执行并保留缺口。组件关联的引用不存在、服务不可用或格式错误分别报错，不降级成“没有契约”。
- 每次执行持有独立的组件租约、有效参数和语义快照；不得因 Python 共享实例配置、并发调用或热更新串用参数/版本。任务开始后新契约仅用于后续调用，链中各阶段分别冻结。
  覆盖入口须验证从检查到调用算法期间参数和组件版本不被其他调用替换；Python 配置/执行隔离属于本 Feature 的必要接线，不以当前共享实例可运行作为保证，也不扩展为通用调度或热更新改造。

以下为示例算子的静态声明片段：`measure` 从其已有 `value_column` 参数取字段名，要求 float64、单位 ms，输出规则声明由算法将其转换为 s。
若算法与 Schema 标注的默认值均为 `latency_ms`，本次调用可以使用默认值；完整契约及字典由注册流程提供，SQL 只配置算法已有参数：

```sql
SELECT * FROM dataframe.requests
USING example.latency_seconds
WITH value_column='latency_ms'
INTO dataframe.requests_seconds
```

### 输出规则与真实结果

| 规则 | 可推导条件与输出保证 |
| --- | --- |
| copy / 投影 / rename | 仅继承显式映射字段的语义；保留来源精确引用。行身份/时间/关联等上下文只有在引用仍闭合且行粒度不变时保留，否则置未知并说明原因 |
| unit_convert | 作者显式声明 `target = source * scale + offset`、源/目标单位、输出类型、舍入及 NULL/溢出策略，算法本身负责转换；元数据不修改数值，不自动保留已不适用的标准/值域/有效性 |
| aggregate | 首期仅 sum/count/mean/min/max，明确源角色、分组键、NULL 策略及结果类型；行粒度变为所声明分组，累计/修订去重等前提未能确认时保持未知，不自动认定可直接求和 |
| fixed / unknown | fixed 使用作者声明的字典并核对真实结构；unknown 按实际 Schema 生成结构字典，无法推导的字段/实体语义置 unknown，不自动复制同名输入字段 |

- copy 可显式声明指定 source_port 的 `all_fields`，用于通用 passthrough；只展开该端口当次实际 Schema 和已确认字段映射，未绑定的字段语义仍未知。其他规则逐项声明角色与输出字段。
- availability 使用归一配置/宿主 execution_context 的值存在或枚举相等条件及其合取，区分计划上可用、实际 absent 与已观察；未知条件保持未确定。row_cases 仅声明有限行分支，不逐行扫描或声称行值已校验。
- context_semantics 按配置中 dataset/metric 等身份实例化语义字典；同一结果列随指标含义变化时，公共字典保留 unknown，`OperatorUse.outputs[].semantic_contexts` 保存选择条件及 dictionary_ref。
  消费者必须取得行判别规则及必要上下文引用后解释具体值；无法唯一关联时保留 unknown，不向 F1 严格 v1 字典暗加条件字段。basis/model_basis 等动态值描述来源关系，不按每行或每个模型版本复制一份字典。
- 复制/重命名在无歧义时延续 field_id；单位转换、聚合及多输入身份冲突分配输出字段身份，记录 `(input_index,dictionary_ref,field_id)` 来源，不能以列名充当全局字段身份。
  相同计划重复预览的字段身份稳定；derived origin 代表按声明推导，不能证明算法实现满足公式。完整/局部上下文与 required_context_refs 沿用 F1。
- CheckAndDerive 只生成候选，不保存 revision。输出 Schema 就绪时比较实际字段、类型、可空性和各选中输出实体；零行但有 Schema 的结果同样可核对，无 Schema 为 not_observed。
  主输出由宿主取真实 Arrow Schema；副输出由生产者从已创建的结果对象提供实际 Schema，不用静态契约假装观察；未产生的可选输出明确 absent。
- 输出不符返回 `OUTPUT_SCHEMA_MISMATCH`，该结果不得发布 matched 绑定；流/块路径在 Schema 变化时重核对，不允许继续使用旧字典解释后续不同结构。
  所有元数据解析/持久化在阶段或 Schema 边界完成，不进入逐行/逐包路径；不同端口不得共享一个无法区分实体的绑定。
- 结果字典、绑定和 OperatorUse 复用 F1/F2 的字段映射、对象身份与精确引用；临时结果按 run/stage/port 分配独立对象身份，命名结果替换遵循 F2，不能将同名旧绑定转给新数据。
  执行响应和任务状态返回 use_ref 及各输出 dictionary_ref/binding_ref；注册表保存使用依据及反向引用，任务状态过期后仍能按结果引用读取定义，不补造旧结果历史。
- 已观察的输出定义、绑定和本次使用依据在注册表同一事务内发布；失败不暴露半份语义关联。业务输出与元数据存储不承诺跨系统事务：数据已产生而元数据保存失败时明确报告各自状态，不能宣称绑定成功或回滚了数据。
  长期输出记录只证明截至该观察点的 Schema 与定义，outcome 区分完整/部分/失败；不据此声称任务终态成功或具备断点恢复。

### Baseliner：复杂算子的代表契约

Baseliner 的声明沿用上述通用模型；T1/T2 交付覆盖本节代表能力、可注册且引用闭合的 JSON 模板与 Schema，不承诺补全全部业务语义；以下片段仅示范一个输入集合，省略指标角色模板等其余内容，不是完整可注册文档：

```json
{
  "port_id": "datasets",
  "min_count": 1,
  "max_count": 64,
  "shape": "record_batch",
  "repeat_from_config_pointer": "/datasets",
  "item_key_pointer": "/id",
  "roles": [
    {
      "role_id": "bucket",
      "required": true,
      "types": [{"system": "arrow", "name": "int64", "parameters": {}}],
      "nullable": null,
      "semantic_requirements": {},
      "selector": {
        "kind": "config",
        "scope": "input_item",
        "pointer": "/bucket/column",
        "cardinality": "one"
      }
    }
  ]
}
```

- 配置分调用层与归一 TaskConfig 层：现有 `config` 精确引用与 `parameters` JSON 文本用 oneOf 表达二选一，`model_output` 为可选既有参数；嵌套 clock/calendar/forecast/datasets 及策略对象进入 Schema。
  复用 NormalizeInlineConfig/ResolveConfig，把单数 dataset 简写归一为 datasets，核对 SQL FROM，并记录有效默认值及配置精确引用；不要求 SQL 再携带契约。
  现有 NormalizeSourceConfig 返回规范化 WITH，精确引用路径仍保留 config 引用；它不是完整有效配置快照。由 Baseliner 适配层从生产解析结果提供 TaskConfig 的有效值和输入关联，不在 Scheduler/Config 再实现解析器。
  来源尚未就绪、无法归一时预检为 undetermined；明确非法参数为 incompatible，引用不存在或服务失败按错误返回，不混入未确定状态。
- 输入模板从每个 dataset 的 bucket.column、series_keys、deduplicate.keys 与 metrics 展开角色；Value/Ratio/Relation 按 kind 区分，分别描述 column/aggregate、分子分母表达式、group_space 与子指标数组。
  sampled 的 sample_count 条件必需、不同 kind 的非法混用、forecast 范围及 mode 等由 Schema/既有解析器校验；跨字段关系以生产校验与测试为依据，不声称普通 JSON Schema 能自动证明。
  用户输入是配置化业务字段，内部 observation/progress 不冒充用户表结构；既有整数无损扩宽和 float32→float64 适配后检查实际结构，并保留适配依据。

| 输出实体 | 结构、可用条件与语义 |
| --- | --- |
| results | 引用与 MakeSchema 对齐的结构和字典；evaluation/forecast 共用 Schema，以 result_kind 区分。普通结果表/DataFrame 含两类结果及 Relation routed 结果，不含融合/维护实体 |
| model_parameters | 仅有限 snapshot 且配置 model_output，在正常 EOF 后导出最终模型；parameters_json 按 model_kind/parameters_version 解释，未训练时为 NULL，不等同完整 checkpoint |
| relation_fusion | 仅托管输出且启用 Relation 的独立融合实体；声明 fusion_score、basis_id、status、can_alert 及关联身份，不能与普通 results 混绑 |
| maintenance | 托管输出的独立维护事件实体；说明 event_kind/reason 与各状态数量，不解释为业务观测或异常结果 |

- results 的身份/时间上下文包括 dataset_id、metric_id、series_key、source_epoch、target_bucket、issued_after_bucket、bucket_seconds、timezone；Relation routed 再使用 summary_id/basis_id，模型来源使用 model_basis_id。
  observed/expected/lower/upper 的含义随指标变化，由配置与输入字典共同解析；`unit` 当前保存 value、ratio 等类别，不能当作物理单位，也不将不同指标的 observed 统一声明为同一种量。
- 行规则声明 forecast 的 observed/can_alert 为 NULL 且 target_bucket 大于 issued_after_bucket；evaluation 两桶相等且有 observed；expected/lower/upper 的 NULL 与 status 联动，冷启动不承诺有效区间。
  使用既有 ValidateResult/编码及测试验证这些业务约束；契约通用预检/结构观察仍报告 data_validation=not_run，不因 Schema 匹配宣称预测、异常判定或全部行值正确。
- 代表验收按下表覆盖通用能力，允许一个组合验证多项能力；四类输出均须在真实创建后观察，不要求指标类型×来源×出口的全组合矩阵。poll 的进度要求只忠实描述现有能力，不新增或宣称已复验恢复、外部后端及 NPM 矩阵。

| 代表场景 | 必须证明的通用能力 |
| --- | --- |
| 非 NPM 有限 DataFrame 的 Value，使用 parameters 中的 dataset 简写及完整配置的精确 config 引用 | 生产解析默认值与检查快照一致、JSON 文本/精确引用解析、角色列定位与缺失拒绝；results 的 evaluation/forecast 和冷启动 NULL 可由行分支正确解释 |
| SQLite 有限快照的多 dataset，选取 Value/Ratio 异质指标 | 集合模板、参数作用域与输入身份隔离；同一 observed 等结果列按 dataset/metric 关联不同语义，不共用错误单位 |
| SQLite Relation 及其子指标配置 | discriminator/子数组角色展开、routed 结果及 basis 关联上下文；不重新验收完整 Relation 算法 |
| 有限 snapshot 设置/省略 model_output，并覆盖未训练模型 | 条件输出的已观察/absent、真实 model_parameters 副输出及条件 NULL；参数版本可解释且不冒充 checkpoint |
| SQLite 托管 Relation 输出及既有维护事件场景 | 真实 relation_fusion/maintenance 的生产者观察、独立端口与字典绑定，以及同主 results 的实体区分；不扩展恢复/维护能力 |

- 实施前把每个代表组合映射到上述能力、A9 断言及实际 target/filter，保留非法参数、引用失败、字段缺失和上下文不完整等负向覆盖；不得通过删除必需断言或跳过失败缩小验收。
  只补本次契约接线所需的声明/适配/观察及受影响回归；未来若要求全配置、全实体业务解释的完整 Baseliner 领域语义包，另按独立价值立项，不作为本 Feature 的隐含完成门槛。

### 注册、读取与绑定接口

新增公共头文件 `framework/interfaces/ioperator_data_contract.h` 和独立 IID；T1 在编码前落实以下接口及请求/响应 Schema。
定义及使用依据持久化归 Config，组件关联归 Catalog/对应宿主，执行检查由 Scheduler/生产者编排；进程内通过 IQuerier，跨进程通过 HTTP，不依赖具体插件类。

```cpp
struct OperatorContractSnapshotV1 {
    std::string exact_reference;
    std::string sha256;
    std::shared_ptr<const std::string> document_json;
};
interface IOperatorDataContractV1 {
    virtual ~IOperatorDataContractV1() = default;
    virtual int Resolve(const char* exact_reference,
                        OperatorContractSnapshotV1* out,
                        DataSemanticsErrorV1* error) = 0;
    virtual int CheckAndDerive(const char* request_json,
                               std::string* result_json,
                               DataSemanticsErrorV1* error) = 0;
    virtual int RecordUse(const char* observation_json,
                          OperatorContractSnapshotV1* out,
                          DataSemanticsErrorV1* error) = 0;
};
```

- CheckAndDerive 接收系统生成的 ResolvedInvocation，inputs 包含实际结构快照及精确绑定；返回 CompatibilityResult，不读取业务行或执行算子，也不新增/修改静态契约。
  RecordUse 只供执行路径提交上述冻结依据、run/stage、实际输出对象/Schema、outcome 和 expected_current_revision；返回持久使用记录。重复规范内容不增版，冲突不覆盖，普通导入不授予运行时观察来源。
  调用线程安全，错误清空输出，借用入参只在调用期间有效，快照独立持有文本；来源观察/插件回调在注册表写事务和目录互斥锁之外完成，发布前核对快照及预期版本。
- 内置算子由注册方提供同一 JSON 包；C++ V1/V2 增加可选导出 `extern "C" const char* flowsql_operator_data_contract_v1(int index)`，返回内嵌文档或 `{contract_ref}`，缺省/空值表示未声明。
  index 沿用现有插件索引，字符串在库存活时有效，宿主限长读取并复制，异常不得跨导出；不改 V2 descriptor 的布局或 contract_iid，不提前释放库/实例/产物租约。
- Python `register_operator(..., data_contract=None)` 的 data_contract 仅为作者在注册时提供静态声明的可选参数，接受同一文档或精确引用；SQL 无此参数。Web 创建/上传编辑器提供声明入口。
  同一组件内所有声明先校验，关联切换带预期组件版本；无效声明或引用失败保留原可用关联。
  保存定义与组件关联分开报告；元数据关联成功后才展示对应组件为已关联。上传、去激活、删除及重启保留旧精确文档，不让运行任务改读新组件。
- 复用 `/api/semantics/v1/{save,resolve,list,history,references,import,export}` 管理 operator_contract；operator_use 只允许查询/导出。
  operator_contract 的 resolve 增加 view=summary/resource 与 resource_pointer，返回原契约精确版本、投影片段及 required_context_refs；完整读取保持闭合 JSON，局部资源不产生独立 revision，旧 kind 的读取协议不变。
  扩展 `/api/operators/detail` 返回 `contract_state=declared|missing|unavailable`、组件版本与 contract_ref；新增 `POST /api/semantics/v1/operators/check` 提供不写入的实际对象预检，尚未取得 Schema 时返回缺口。
  预检请求为 `{operator:{category,name},parameters,inputs:[{input_index,locator}],output_locator?}`，locator 沿用 F2；服务端按静态端口/集合及实际来源适配取得契约、结构、绑定与角色；未提供目标时依赖目标能力的输出保持未确定。
  请求不接受替换契约或独立角色映射，不把客户端提交的结构标为实际观察；预检不保存执行计划，真正执行时重新解析并冻结当次依据。
- 发布 JSON Schema Draft 2020-12 / OpenAPI 3.1 的文档、上述请求响应和可运行 C++/Python/HTTP 样例；使用 F1 的文档 8 MiB、控制请求/响应 12 MiB、深度 64、分页及必要上下文约束，不静默截断契约。
  沿用 F1 结构化错误与访问边界，新增 `CONTRACT_INVALID`（400）、`CONTRACT_INCOMPATIBLE`（422）、`COMPONENT_CHANGED`（409）、`OUTPUT_SCHEMA_MISMATCH`（执行错误）；预检三态为正常响应，服务不可用仍为 503。
- 算子详情展示静态输入角色、字段选择规则、参数默认值/影响、各输出及转换、未知项和历史精确引用；预检只选择数据集及已有算法参数，展示系统解析的字段映射，编辑和冲突保留输入。
  返回概要与所需资源/上下文的精确引用，程序和 AI 可先读概要，再取配置分支、输出实体和必要语义；不以整段自然语言替代结构化内容。页面提示只解释检查范围；结果和图表中的就地语义展示属于 F5。

## 主链路

1. **注册与发现：** 内置/C++/Python 携带文档或精确引用 → 校验并保存定义 → 显式关联实际组件版本 → Web/HTTP/IID 读到同一契约；失败分别报告定义与关联状态，旧组件和历史版本不被覆盖。
2. **解析与输出：** 每个 USING/THEN 阶段持有组件租约并读取静态契约 → 适配层提供与算法共用的有效配置及实际输入关联 → 宿主冻结调用依据、通用层按 selector 展开并三态检查 → 按原数据面和同一有效值执行 → 观察各输出结构 → 发布结果字典/绑定/使用依据，契约文档保持不变。

## Feature Task

- [x] T1：交付统一 JSON 契约/使用依据、嵌套 Schema、闭合资源模板及独立契约/配置适配接口，使静态声明可校验、精确引用且生产配置与通用检查职责明确；验收 A1、A2 的存取部分及 A9 的结构部分。
- [x] T2：交付 C++ 内置/扩展、Baseliner 与 Python 注册/创建/上传的契约登记和组件关联，使声明随实际组件可发现且旧 ABI/无契约算子继续可用；验收 A2、A9 的登记部分。
- [x] T3：交付基于实际输入及算法共用有效配置快照的自动模板展开和三态检查，使调用者无需在 SQL 携带契约，字段错误可定位且覆盖入口的检查与并发执行依据一致；验收 A3、A5/A9 的输入部分。
- [x] T4：交付输出规则、行分支和指标上下文的字典实例化、真实结构核对与版本绑定，使多实体结果可解释、可追溯且未知语义不误继承；验收 A4、A5/A9 的输出部分及 A6。
- [x] T5：交付详情/创建/上传/预检页面及普通客户端闭环，使作者与程序/AI 通过同一 JSON 协议取得复杂结果的完整解释依据并保留旧能力；验收 A7、A8，收口 A1～A6、A9，不要求接入模型或 MCP。

## 测试锚点与完成出口

| 编号 | 必测断言 |
| --- | --- |
| A1 模型 | JSON 规范与 YAML 归一一致、嵌套 object/array/oneOf/条件必需、JSON 文本解码、完整模板过 Schema；局部引用闭合/外部精确版本、重复身份/非法 Pointer/分支/默认值/预算；合法 unknown、使用记录不能伪造观察 |
| A2 登记 | 内置、C++ V1/V2 可选导出、Python 装饰器/创建/上传；旧 ABI 与无契约继续运行；无变化不增版、并发/关联失败不覆盖、组件摘要与实际执行相符 |
| A3 绑定 | SQL 无需契约/独立映射；固定列/已有参数/嵌套配置 selector、数组和子数组作用域与单/多输入隔离；适配层有效默认值/精确配置/输入关联与算法一致，Schema default 不擅自填值；三态/pending/来源冲突/无效关联不降级 |
| A4 推导 | 真实非 NPM 数据的 copy/投影/rename、ms→s 显式转换及五种聚合；数值与独立预期一致、字典/标准正确；上下文裁剪、字段身份冲突、未知转换不继承同名语义 |
| A5 执行 | 真实 Scheduler→C++/Python Worker 与块转换；同一契约用于不同字段/配置且不增版；THEN/并发调用从检查到执行不串用有效参数/组件/契约版本，明确不兼容无算法调用/输出；可选实体 absent/未确定、零行/结构漂移、行语义声明不冒充数据验证 |
| A6 结果 | 定义/绑定/use 同事务失败回滚、数据已产生时的元数据失败状态；同名结果替换、任务状态过期后精确追溯、旧字典/契约及运行中版本不漂移、重启读取一致 |
| A7 页面与协议 | 真实浏览器创建/上传、详情、已有参数预检与映射、刷新/冲突输入保留；普通客户端读概要/闭合资源→检查→常规 SQL→use/多实体与行分支/上下文精确引用；缺必要上下文不得标为完整 |
| A8 兼容回归 | F1/F2 版本/绑定与旧配置、目录懒加载、C++ 装卸/租约、Bridge、Scheduler 批/块及受影响流路径；未接线输出明确 not_observed，历史无契约可执行 |
| A9 Baseliner | 按场景→能力表冻结代表组合，声明与生产解析器/MakeSchema 一致；非 NPM DataFrame/SQLite 覆盖 Value/Ratio/Relation、dataset 简写/多 dataset、config/parameters 互斥及字段缺失；真实四类输出、条件 NULL/冷启动、异质指标单位与精确上下文有独立预期；不展开全组合或完整算法复验 |

- 实施时在工作台冻结具体 target/filter 和依赖：新增契约/注册/推导/执行/浏览器用例，加受影响的 `test_data_semantics`、`test_config_channel`、F2 绑定测试、`test_builtin`、`test_bridge`、`test_scheduler_e2e`、框架/块/流及路由回归，按调用链确认清单。
  依赖优先为临时 SQLite 注册表及按场景→能力表选取的 Baseliner 夹具、CSV/命名 DataFrame、可编译 C++ 夹具、真实 Python Worker、Arrow IPC、随机本地端口与 Chromium；加入实际受影响的 Baseliner 配置/读入/结果/生命周期回归，不启动无关外部数据库/Kafka 或 Docker 矩阵。
- 必须验证默认启动使用的契约/算子产物身份，并区分“文档已保存”“输出结构已核对”“运行服务已重载”；未重载用户服务不得宣称已部署。
- 每个 Task 完整验收后再勾选并按授权进入下一项；Feature 冻结清单满足 selected=executed=passed、failed=skipped=not_executed=0，编译/格式/静态检查通过后归档。
  使用说明在既有数据语义说明中补充作者、调用与未知边界；日志和源码/产物摘要存入 `evidence/operator-data-contract/`，不提交。只在对应 Task 验收通过后声明能力交付，构建产物与运行服务状态分别记录。

## 完成证据

- 2026-10-30（北京时间）T5及Feature完整验收完成：本机权限开放后，真实HTTP→Task→Scheduler→C++/Python Worker/Arrow IPC及Chromium页面全部执行；覆盖创建/上传、详情/历史、预检/角色映射、刷新/冲突输入保留、概要/闭合资源/多实体/必要上下文精确读取。普通批任务沿用`sql_text`与`mode=sync`；Task在既有结果摘要中保存并返回各SQL的`contract_checks`及精确use引用，重启后可读取，并保留结构化契约错误及422/409/503状态。严格复制的真实Arrow可空性漂移保留为`OUTPUT_SCHEMA_MISMATCH`负向覆盖，不放宽声明。
- 最终标准运行`20261029T163118Z-9e84331c`：冻结32项本机与10项网络业务/回归，共42项，加25项必需夹具，selected=executed=passed=67，failed=skipped=not_executed=0；42项逐项核对均在passed中，源码/默认产物验收前后摘要一致。此前新HTTP/Worker与真实Chromium检查点`20261029T162910Z-e5650a84`为4/4。标准src入口配置、相关CMake目标/前端构建、修改行格式、Python/Node语法、文档片段/链接及diff检查通过。完整清单、summary、日志及摘要位于`evidence/operator-data-contract/final-accepted/`，默认生产库（含Task）、发布Schema与前端身份在`default-artifacts.json`（本地缓存，未入库）。T1～T5按序验收并标记，Feature归档；外部数据库/Kafka/Docker及完整Baseliner算法/恢复矩阵不在本次范围，用户运行服务未重载，未提交或推送。Epic保留用户起点内容，其旧状态/链接作为跨任务文档同步问题记录于工作台。

以下为实施过程的历史检查点，曾有的T5权限阻塞已由上述最终验收解除。

- 2026-10-30（北京时间）T5最终本机检查点：详情/创建静态声明/上传、实际来源预检与有效配置、组件冲突和草稿恢复、F1新旧类型共存、普通HTTP/真实Chromium夹具及作者/客户端说明已落盘；发布预检响应补齐作者版本并收紧Schema。默认`build/output`相关产物及前端构建通过。冻结31项本机业务/回归加17项必需夹具，标准入口48/48（`20261029T161114Z-bee98da6`），failed=skipped=not_executed=0，验收前后源码/产物摘要一致；格式、文档片段/链接及diff检查通过，Epic保持用户起点内容。证据`evidence/operator-data-contract/final-local/`及`default-artifacts.json`（未入库），用户运行服务未重载。
- T5阻塞未接受：新增HTTP/Chromium标准清单4项（含2项环境夹具），2项夹具通过、2项实际用例均在创建本机socket时以`PermissionError: [Errno 1] Operation not permitted`失败（`20261029T161310Z-fe3e3fe9`），尚未进入HTTP/Worker/浏览器业务断言。用户已授权本机验收，但工具自动审批两次拒绝提权，环境权限尚未开放；10项必需网络回归仍为2项环境失败、8项待执行，不按通过或跳过收口。证据`evidence/operator-data-contract/t5-network-blocked/`；完整待验命令见`tasks/active_task.md`和本地`final-scope.json`。T5保持未勾选，Feature不归档、Backlog不标完成。构建中另有未修改`test_bridge/main.cpp`忽略Arrow返回值的既有警告，未扩大修复范围。

- 2026-10-29 T5协议检查点：实际组件/来源预检路由、生产配置适配与三态、有限目标/未指定目标、SQLite多dataset/Relation精确上下文及pending不读取，直接实际插件路由标准18/18（20261029T153329Z-7555d652），其余零。新增普通HTTP/真实Worker测试已编写，但当前沙箱禁止socket、审批策略拒绝提权，尚未执行；页面/Chromium/最终验收尚未收口，T5保持未勾选。证据 `evidence/operator-data-contract/t5-protocol/local-accepted/`（未入库）。

- 2026-10-29 T4 完成：copy/投影/rename、显式ms→s及五聚合的真实数值/字段身份/单位推导，行分支、独立dataset/metric与Relation子指标精确上下文，unknown不误继承；Scheduler DataFrame/块/THEN及命名新身份绑定、零行与三类输出漂移、数据/元数据失败状态和同事务回滚。Baseliner四实体由真实batch捕获，模型冷启动/零行及命名F2绑定；托管SQLite每次Publish成功回调真实Config插件记录独立实体binding/use，元数据失败不回滚已提交数据。SQLite异质ms/B来源字典经F2绑定，Ratio保持物理单位unknown，来源身份未知仍undetermined。最终标准23/23（20261029T140620Z-2a2d0b8e），其余零，格式/diff通过；本机生命周期/结果编码及旧调度回归通过。证据 `evidence/operator-data-contract/t4-{core,execution,baseliner,final}/`（未入库）；T5尚未实施。

- 2026-10-29 T3 完成：实际来源/适配后Schema与组件租约、生产有效配置共同冻结，固定/参数/嵌套及集合角色三态检查，Python真实Worker并发/版本隔离、C++ V1/V2及Scheduler DataFrame/块/THEN；Baseliner五组DataFrame/SQLite输入、懒CSV、精确config引用及缺列拒绝。最终标准清单16/16（20261029T125449Z-67b43e52），failed=skipped=not_executed=0；此前各输入切片证据与本地 `evidence/operator-data-contract/t3-final/` 汇合，格式/diff通过。SQLite持久身份未知明确undetermined，不宣称语义或行质量已验证。T4/T5尚未实施。

- 2026-10-29 T2 完成：实际组件摘要与精确契约关联；内置/C++ V1/V2/Baseliner 可选导出、Python 装饰器/创建/上传/编辑原子切换与失败回滚；旧 ABI/无契约真实 Worker Arrow IPC 继续可用。组件关联 4/4（20261029T085555Z-2809702c）、C++ 5/5（20261029T091745Z-aa1c85e7）、Python 9/9（20261029T093533Z-d0e3a0ea）、Baseliner 模板/生产解析器/真实组件登记及模型回归 9/9（20261029T094818Z-ebf41dca）；各组 failed=skipped=not_executed=0，格式与 diff 通过。本地证据：`evidence/operator-data-contract/t2-{association,cpp,python,baseliner}/`。执行检查和输出观察尚待 T3/T4。

- 2026-10-29 T1 完成：统一 JSON/使用模型、独立 IID、嵌套配置与 JSON 文本校验、闭合简单/Baseliner 模板、精确版本/冲突/局部读取及重启读取。标准入口 selected=executed=passed=8（含两个环境 fixture），failed=skipped=not_executed=0；格式与 diff 检查通过。证据：`evidence/operator-data-contract/t1/`（本地缓存，未入库）。T2～T5 尚未验收。
- 2026-10-28 规格修订：统一 JSON、嵌套配置/输入集合与 Baseliner 多实体/条件语义；JSON 片段、14 个本地链接、任务/验收编号及文档空白检查通过，不代表功能验收。
- 2026-10-28 尺寸边界复检：当时完成证据前 219 个非空行、5 个一级任务、2 条既定主链路；复杂算子是同一注册→检查→结果语义能力的代表用例，保持单 Feature。
- 2026-10-29 边界审查建议落地：补充场景→通用能力映射、配置适配/通用检查/宿主分工及检查到执行的一致性边界；保留 T1～T5 与四类输出，完整 Baseliner 领域语义包另行立项，本轮未启动实施。
- 本次文档验收：JSON 片段 1 个、18 个本地链接、8 个 Markdown 表格、5 组代表场景及 T1～T5/A1～A9 检查通过；完成证据前 238 个非空行、5 个一级任务，仍为同一注册→检查→结果语义闭环，不按行数拆分。未运行构建或功能测试。

## 依据

- [算子目录接口](../../src/framework/interfaces/ioperator_catalog.h)、[C++ 插件 ABI](../../src/framework/interfaces/cpp_operator_plugin_abi.h)、[通用算子接口](../../src/framework/interfaces/ioperator.h)。
- [块转换算子接口](../../src/framework/interfaces/iblock_transform_operator.h)、[流算子接口](../../src/framework/interfaces/istream_operator.h)、[Python 算子基类](../../src/python/flowsql/operator_base.py)。
- [SQL 工作流指引](../../docs/sql-workflow.md)、[数据标准与数据字典 Epic](../specs/epic-data-semantics-dictionary.md)。
- [Baseliner 使用说明](../../docs/baseliner.md)、[配置解析与结果 Schema](../../src/operators/baseliner/baseliner_contract.cpp)、[结果编码](../../src/operators/baseliner/result_codec.cpp)。
- [配置归一接口](../../src/framework/interfaces/iblock_transform_source_config.h)、[Baseliner 配置接线](../../src/operators/baseliner/baseliner_operator.cpp)、[Python Worker](../../src/python/flowsql/worker.py)、[Python 注册表](../../src/python/flowsql/operator_registry.py)。
