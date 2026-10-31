# 通道与算子说明

在“通道管理”打开具体数据对象，即可查看结构、补充含义并保存说明；在“算子管理”打开具体算子，
可阅读作者声明的输入输出契约、维护补充说明并显式预检。Web 与普通 HTTP 客户端读取同一份对象说明。
日常使用由系统处理字典名称、字段身份和绑定，无需先建立标准或填写内部映射。

字段说明支持 CSV/命名 DataFrame、Kafka 结构化 source/both，以及 SQLite/MySQL/PostgreSQL/ClickHouse 的明确限定表。
结构来自当前来源，人工文字只补充含义。点击尚未加载的 DataFrame/CSV 实例时，页面自动加载该实例，再进入说明配置。
普通 HTTP 说明查询和目录读取保持只读，不加载 CSV、不读取业务行、不消费 Kafka、不执行算子。
数据预览、契约预检和 SQL 执行保留各自入口；`data_validation=not_run` 表示未验证业务数据质量。
NPM 领域说明、SQL 工作台 DESC、结果/图表就地说明和模型接入属于后续能力。

## 入口与持久化

完成[标准构建及启动](../README.md#快速开始)后，从 Web 顶栏选择“通道管理”或“算子管理”。
原“标准与字典”顶栏入口已移除，旧 `/semantics` 路由及精确历史链接仍可直接访问，供兼容和高级维护使用。
默认 Web 地址为 `http://127.0.0.1:8081`。现有单/多进程配置加载 `libflowsql_config_channel.so`，
经 Web → Gateway → Router → Config 提供控制 API。

语义库复用 Config 插件的 `db_path`，在同一个 SQLite 数据库中使用独立的
`semantics_object`、`semantics_revision`、`semantics_reference`、`semantics_observation` 表。默认配置为 `./meta/flowsql_meta.db`，
`./start.sh` 从 `build/output` 运行，因此默认实际路径是 `build/output/meta/flowsql_meta.db`。
部署时将此数据库所在目录放在持久存储中；备份和恢复应包括完整数据库。迁移事务失败会使插件加载失败，
不会退回内存库。原有普通配置通道保持自己的数据和接口。

使用部署现有访问控制边界；本功能没有增加独立登录系统或对象级权限隔离。

## 在通道中查看和保存说明

| 对象 | 当前入口 | 结构与读取边界 |
| --- | --- | --- |
| DataFrame / CSV | 点击对象名称，打开“通道详情 → 数据说明” | 尚未加载时自动通过原预览接口加载当前实例，完成后显示真实 Arrow Schema 和统一编辑表单；已加载时直接进入，不重复预览 |
| 数据库表 | 连接行“浏览”→在表目录选表→“数据说明” | 使用连接配置及选定的明确表；选表和查看结构只读元数据，切换到“数据预览”才读取业务行，空表也可说明 |
| Kafka source/both | 原“详情”内的“数据说明” | 按已保存配置和真实解码 Schema 展示字段，保留 JSON Pointer、来源单位及 `__kafka_*` 列，不启动 Consumer |
| 配置、其他流通道、Kafka 连接/packet/sink-only | 原详情或配置入口 | 显示基础身份、原配置和 unsupported 原因，不开放字段说明编辑 |

数据库连接本身只用于进入表目录；连接没有字段字典不代表缺失说明。每张限定表有自己的说明。
DataFrame 行内仍可预览、重命名、删除；数据库保留浏览、删除；Kafka 保留详情、编辑、验证、复制引用、删除及原禁用规则。
打开列表不逐行加载说明。字段表工具栏的“搜索字段名…”按包含关系筛选，可同时显示多个匹配字段；清空搜索恢复全部字段，所有字段草稿仍保留。
DataFrame 首次加载期间显示进度，完成前不能编辑或保存；加载失败时显示错误，可点击顶部“刷新”重试，已有草稿保留。
加载本身不会保存或初始化说明；关联选择器也不会自动加载尚未加载的目标，应先从该目标的实例入口进入一次。
字段表头最右侧的向上箭头可隐藏搜索栏和全部字段内容，方便查看底部信息；收起后点击向下箭头恢复显示，筛选条件和未保存草稿保持不变。悬停箭头可查看“收起字段表 / 展开字段表”提示。

1. 打开具体对象，查看当前结构与已有说明。名称、类型和可空性只读；类型摘要保留精度、小数位、嵌套结构及时间精度/时区，完整表示可悬停查看。
2. 在同一区域填写对象说明和可选的“一行表示什么”，在字段行填写字段说明与必要单位。
   枚举、空值含义、有效条件和限制可直接写进字段说明。已有 NULL 含义、有效条件等补充单独只读展示，不会自动拼入编辑文本。
   需要时间解释或关联时，在对应字段行按需打开；日常表单无需选择内部状态、来源或填写字典/绑定身份。
   不明确的属性保持未知，未填写可选说明不会阻止保存。
3. 点击顶部的“保存”，该按钮与“刷新”并排显示，字段表收起后仍可保存。系统保留已有字段身份及未编辑的完整语义，生成内部定义和字段映射，只更新当前对象。
   原字典被其他对象共用时，当前对象的修改使用独立副本，其他对象说明不随之改变。
4. 进入说明页或点击“刷新”时自动检测当前来源；发现字段新增、删除、类型/可空性或来源声明等实际差异时，弹框列出变化，点击“知道了”后继续编辑。
   字段表下不再显示人工确认复选框；只有数据库原生身份无法可靠观察、没有实际差异时正常进入，不弹出空变化提示。
   查看提示不会保存或改写说明；删除字段在下次显式保存时移除，其说明不会迁移到其他字段。
   没有未保存编辑时采用本次检测结果作为保存依据；有原稿时保留其依据，防止自动覆盖。
   保存前仍会重新核对来源；若编辑期间发生变化，保存被拦截，页面重读并提示最新差异，保留已有原稿和原依据，不自动重试写入。
   来源或版本冲突时，可在核对后选择“核对后使用当前版本继续编辑”，合并改动再提交。
5. 若显示“定义已保存、绑定未更新”，记录已保存精确引用，核对当前来源与错误后重试。
   这是部分成功，不能按保存完成处理；相同内容的重复提交会复用已有定义，避免无意义修订。

当前浏览器会话会按对象保留说明草稿，切换对象不会混用。会话草稿不能代替服务端保存或备份。
结构 pending 时可参考已保存说明，待显式加载并刷新后才开放字段保存。
旧版浏览器草稿会完整保留，并提示先按旧协议保存，再进入新表单；高级语义和原来源不会因界面精简丢失。
“技术详情（最近读取结果）”默认折叠，可复制完整 describe JSON；其中不含未保存草稿，刷新读取结果后才更新。
页面使用中文提示加载、来源变化、冲突和部分成功；下表状态值供 HTTP 客户端及技术详情对照。

### 时间解释与单位

字段行的“时间”下拉框只会出现当前实际类型支持的模式：

| 模式 | 使用方式 |
| --- | --- |
| Unix 时间戳 | 适用于数值字段，明确选择 `s/ms/us/ns`；固定基准为 `1970-01-01T00:00:00Z`，不根据字段名或数值位数猜测 |
| 日期时间 | 适用于原生日期/日期时间；有明确来源时区就采用该事实，否则可补充，也可保持未知，不默认 UTC；不提供日期字符串格式解析 |
| 时长 | 适用于数值或原生时长，明确选择 `s/ms/us/ns`；不设置 epoch 或时区，不被纳入时间点字段汇总 |

受支持的 Arrow timestamp/duration 单位和时区、数据库原生日期时间类别，以及明确生产者声明可提供预填。
预填与读取不写存储，不把普通整数识别为时间；未识别类型与有歧义的数据库 TIME 不作推断。
保存使用既有 `time_semantics` 的嵌套语义值和 `{code,label}` 单位，时间字段只保留一个单位编辑入口；矛盾输入会被拒绝。
已有人工/导入声明优先保留，与来源事实冲突时提示核对；暂不支持简化编辑的历史时间表示会继续显示和保留，明确选择新解释才替换。
清除人工时间解释会保留一般单位；来源本来明确的时间事实仍可能重新显示。系统只维护此次涉及字段的时间点汇总，其他成员及来源保留。

### 单字段与联合字段关联

1. 在源字段行打开“关联”：已有关系时先显示关联列表，可新增、编辑或删除；点击“新增关联”后进入表单。没有关系时直接进入新增表单，未完成的关联草稿会优先恢复。
   在新增表单中选择 DataFrame、数据库表或结构化 Kafka 来源。
   目录按需读取：数据库先选连接再选表，仅选定目标才 describe；不逐对象读取全库说明。
2. 目标结构已知但尚无说明时，选择器会用当前结构创建一份对象本地的空白默认说明并绑定当前精确版本，然后提供目标字段；不会要求先手工填写目标说明。
   目标尚未加载、结构不匹配或来源不可用时仍会提示从目标对象入口处理；选择器不会自动加载 pending DataFrame、读取业务行或消费 Kafka。
3. 选择源/目标字段；“增加字段配对”形成同一个联合关联。一个关系至少一对，内部源字段和目标字段分别不能重复；不同关系可复用字段。
   点击“保留关联草稿”后，再点击对象顶部的“保存”提交。正在编辑的关联也按对象保留在当前浏览器会话，可关闭后继续。
4. 使用“编辑关联”修改整个关系，或“删除关联”明确移除。历史关系按已选精确目标版本读取；目标出现新版本不会自动升级引用。
   目标暂不可读时原关系和草稿保留并提示，不能因筛选或暂时无法解析而静默丢弃。

关联保存只修改源对象的说明，不修改目标说明、绑定或业务表。它表示人工选择的字段映射，不验证唯一性、外键约束、基数或业务数据可连接性，也不会生成或执行 JOIN。


| 状态字段 | 含义 |
| --- | --- |
| `structure.state=known` | 当前可取得结构，是否已经填写说明另行表达 |
| `structure.state=pending` | 当前结构未加载或尚未确定，已保存说明尚未核对当前结构 |
| `structure.state=unsupported` | 该对象接口不提供字段结构说明，原因在 `structure.reason` |
| `description.state=present` | 有当前说明；仍需结合结构核对、缺口和验证状态理解 |
| `description.state=missing` | 尚无说明；已知字段仍正常返回。进入关联选择时可按当前结构创建对象本地默认说明 |
| `description.state=needs_review` | 已有说明需要按当前结构、对象身份或确认项重新核对 |

对象不存在返回 404；已声明引用丢失、来源认证失败或超时会返回具体错误，不伪装为 missing。
结构匹配、人工含义声明和业务数据质量验证是三个不同结果。

## 在算子中阅读和维护说明

从 Python/内置算子的“查看”或“编辑”进入详情；C++ 先打开插件详情，再选具体算子。
用途、使用注意和调用示例可在“补充说明”中编辑并保存；参数、输入角色及 selector、输出实体、
行分支/NULL 规则和必要上下文按组阅读，完整 JSON 和精确引用放在技术详情中。
例如普通 `explore.chisquare` 可查看参数和输入角色；`explore.baseliner` 位于 C++ 插件中，保留嵌套配置及
results、model_parameters、relation_fusion、maintenance 四个输出实体，不合并成单张字段表。

补充说明与作者执行契约分别保存。文字保存不改变算法、参数默认值或契约检查；预检参数也不会写回全局配置。
组件版本变化后，旧说明保留为旧组件依据，不自动套用到新组件。冲突时先保留原稿，核对当前作者声明后再明确保存。

- Python：先从列表去激活，再从详情“编辑源码与静态声明”进入原源码编辑流程；上传 `.py` 同样随组件更新声明。
  提交使用读取时的组件版本，冲突保留草稿。
- C++：通过“更新所属插件”上传新 `.so`，按原管理流程去激活并删除旧插件，再激活新插件，重新打开具体算子核对声明。
  在线补充文字不会改变二进制能力。
- 需要判断实际输入时，在“实际对象预检”中选择来源、填写本次参数并点击“检查实际对象”。执行仍使用原 SQL 入口，
  执行时重新取得当前组件和来源，预检不冻结执行计划。

作者发布声明、预检及多实体结果的详细规则见下文[算子输入输出契约](#算子输入输出契约)。

## 普通客户端：发现、描述与显式执行

[对象说明客户端](../examples/metadata_description/client.py)仅依赖 Python 标准库。在仓库根目录执行，
将 Web 地址和输入名替换为实际部署中的对象；例如按 [SQL 指引](sql-workflow.md#本地示例的准备与执行)准备 `samples`：

```bash
python3 examples/metadata_description/client.py \
  --web-url http://127.0.0.1:8081 --input samples
```

默认只查询 DataFrame/算子目录及对象说明，不加载来源、激活算子或创建任务。返回 source、operator 和 `executed=false`。
若输入结构 pending，先在通道详情显式加载；需要执行时，先在算子管理激活 `builtin.passthrough`，再选择未使用的输出名：

```bash
python3 examples/metadata_description/client.py \
  --web-url http://127.0.0.1:8081 --input samples \
  --output description_demo --execute
```

`--execute` 先显式预检，再通过原批任务入口执行
`SELECT * FROM dataframe.samples USING builtin.passthrough INTO dataframe.description_demo`，
读取返回的精确 use/dictionary 依据和必要上下文。示例拒绝覆盖已有输出，输入/输出名仅接受小写字母、数字和下划线，
首字符为小写字母或下划线。未填写单位保持未知，不据字段名猜测。完整 SQL 组织和任务状态见 [SQL 指引](sql-workflow.md)。
这条调用路径无需模型服务；实际接入模型时仍需显式使用原任务执行入口。

### 简化保存 HTTP 输入

`POST /api/semantics/v1/channels/save-description` 沿用 describe 返回的 `locator`、`basis` 和必要的 `confirmations`。
新客户端可提交 `editor_changes`，与旧 `changes` 恰好选择一种；响应及来源/版本核对规则不变。输入契约见服务发布的 `api.schema.json` 与 `openapi.json`。

```json
{
  "description": "请求采样结果，重试可能产生多行",
  "row_meaning": "一行表示一次请求观察",
  "fields": {
    "observed_at": {"time": {"mode": "unix_timestamp", "unit": "ms"}},
    "latency": {"definition": "请求耗时，缺失不等于零", "time": {"mode": "duration", "unit": "us"}}
  }
}
```

上例仅为信封中的 `editor_changes`。源字段名必须来自当前结构，系统生成或沿用字段身份；无需提交结构、字段 ID 或绑定映射。
省略键保留原值；文本 `null`/空白清除为未知，不表示不适用。`time:null` 清除人工解释并保留一般单位及明确来源事实。
`relations` 省略保留原集合，数组替换集合，`[]` 为明确移除；每项为 `{dictionary_ref,field_map}`，field_map 使用“源字段名 → 目标 field_id”。
目标引用必须包含精确版本，目标字段来自该版本读取结果；包含未修改关系才能在替换集合时保留它们。
`describe.context.editor_fields` 提供只读的类型适用模式、来源单位/时间事实及冲突提示；有效已保存声明仍来自 `description.document`。
旧 `changes` 和通用字典入口继续使用原语义；自动时间汇总维护仅作用于新编辑分支明确涉及的信息。

## 兼容与高级维护：定义与未知信息

以下标准、字典、绑定与导入导出接口继续保留，可通过旧 `/semantics` 页面或 HTTP 使用。
对象详情内保存说明已自动处理日常关联；仅在需要共享定义、精确映射或历史维护时使用这些步骤。

| 对象 | 用途 | 例子 |
| --- | --- | --- |
| standard | 共享业务概念、适用范围、表示、单位和取值域 | 请求耗时的单位和定义 |
| dictionary | 实体行粒度、身份/修订/时间/关联、字段结构及语义 | 一行表示一次请求，耗时缺失不等于零 |
| binding | 具体对象到某个精确字典版本的映射 | `manual / app.requests` 的字段映射 |

名称在同一 kind 内唯一，使用 `[a-z][a-z0-9_.-]{0,127}`。系统生成从 1 开始的 revision；
例如 `dictionary.app.requests@2`。持久引用必须包含精确版本，不能用裸名称或 `@latest`。

每个语义属性都有 `state`、`value` 和 `origin`：

- `declared`：有明确声明，value 为该属性对应的文本、枚举或结构。
- `unknown`：尚未说明，value 为 null；缺失属性保存时自动补成此状态。
- `not_applicable`：不适用，value 同样为 null。
- origin 为 `manual`、`imported`、`producer` 或 `derived`；默认 manual，只表示来源，不表示认证或验证。

示例：

```json
{"state":"declared","value":"没有可用测量值，不等于零","origin":"manual"}
```

`semantic_gaps` 给出未知属性的 JSON Pointer 路径。未知单位不会被补成默认单位。
`data_validation=not_run` 表示没有读取实际业务数据进行验证；格式校验通过、语义已声明、结构声明匹配是不同的结果。
必需身份或结构缺失仍会被拒绝，不能用“未知”绕过。

## 兼容页面操作

1. 选择“新建标准”，填写名称、标题及必要说明；语义不明确时保持“未知”，保存后得到精确版本。
2. 选择“新建字典”，说明一行表示什么，添加字段身份、字段名、类型系统、类型名和可空性。展开字段语义，
   补充含义、角色、单位、时间、NULL、有效条件、聚合规则，并在“标准版本”中选择精确版本。
3. 保存后查看缺失信息、精确引用上下文、引用来源与历史。列表可按类型、名称/字段/说明关键词筛选，支持继续分页。
4. 再次修改时选择“编辑当前版本”。普通语义编辑不会开放字段结构；修改结构需勾选“显式编辑结构”，
   核对显示的原结构与新结构，再保存新版本。field_id 是稳定身份，不根据显示名称重新生成。
5. 历史版本只读；“复制为新修订”把历史内容发布为新的 revision，current 不回拨。
   “另存新名称”创建独立定义。共享定义的已有引用会列在编辑区下方，便于先核对使用范围。

单位、条件、字段列表、关系等复杂属性使用 JSON 输入，语义状态和来源单独选择。
字段引用使用 field_id；类型为 `{system,name,parameters}`，system 为 `arrow|database|opaque`。
F1 精确比较类型描述，不推断不同类型系统的等价性。关系、有效条件和聚合条件只登记声明，不自动 Join 或执行规则。

保存冲突或失败会保留输入，不自动用服务端内容覆盖表单。遇到 `REVISION_CONFLICT`，先保留所需改动，
重新读取当前版本，再重新编辑或另存；不要只递增 expected 值盲目覆盖。离开有未保存内容的页面会提示。
编辑内容只在当前浏览器表单中，没有服务端草稿保存保证。

## 高级来源候选与绑定

需要精确复用既有字典、控制映射或修订历史时，可通过旧 `/semantics` 的来源编辑器，或下文通道 HTTP 接口操作。
选择明确 DataFrame、Kafka 结构化 source/both 或限定数据库表；数据库 locator 包含适用的 database/schema/table，
不登记整个连接，不接收任意 SQL 或扫描全库。日常入口使用前述“数据说明”，原行内“数据字典／表字典”按钮已移除。

“从来源生成候选”只预览，不保存版本。字段名、完整类型参数、可空性来自本次结构；Arrow 保留时区、
时间精度、Decimal 和嵌套信息，数据库保留原生类型与 dialect。SQLite 只表达列声明，不保证每行的实际值类型。
无类型声明或结构缺失会列出 structure_gaps；pending/incomplete 不能伪装成可直接应用的完整字典。

展开字段补充定义、角色、单位、时间、NULL、聚合和标准精确引用；行含义等不能从名称推断，默认保持 unknown。
producer 表示来源配置的声明，不代表业务质量已验证。人工说明和来源声明冲突时同时保留，并在差异详情展示。

- 新建候选填写独立的字典名称与绑定名称，依次保存和应用；也可使用“保存字典并应用”。
- 选择已有字典精确版本后，检查并显式提交 field_id → 来源字段的映射；必须全覆盖、不重复，允许额外源列。
  复用不会自动认定 Arrow 与原生数据库类型等价。类型不符时，可“按来源适配候选”并保存新版本，或“另存新字典”。
- 结构变化展示新增、删除、类型、可空性、顺序、生产者声明及可能重命名；未变字段保留 field_id、人工内容和标准。
  删除字段身份使用 JSON 数组，重命名通过 field_id → 新来源字段的显式映射确认；内部身份/时间/关联引用需先修订。
  历史字典与旧绑定保留原说明，同名重新出现的字段不会自行恢复已删除语义。
- 共享与历史引用范围在编辑区展示。修改共享字典不会自动更新其他绑定；可另存以保留共享定义。
- 勾选本次 required_confirmations 后应用。预览后来源已变化返回 SOURCE_CHANGED；重新获取预览并核对，不能盲目重试。
  409 保留表单。若字典已保存但应用失败，页面同时显示已保存精确版本和“绑定未更新”；可基于当前绑定重新确认后应用。

“刷新核对”重取当前来源，保持当前编辑内容；失败会清除旧的 matched。状态含义：

| state | 含义 |
| --- | --- |
| unbound | 当前对象没有绑定 |
| pending | 当前结构尚未确定，未强制加载 |
| matched | 此次观察的结构/映射一致，所需身份或语义确认已记录 |
| mismatch | 缺列、原生类型/可空性等与绑定字典不符 |
| needs_confirmation | 身份连续性、来源声明或历史导入需要显式确认 |
| unavailable | 本次来源不可访问；HTTP 同时返回具体错误 |

matched 与 semantic_gaps 分开，data_validation 始终为 not_run。应用不冻结外部表；后续变化在下一次显式核对或页面刷新时反映。

Kafka/DataFrame 的受管身份独立于名称且可重启恢复；重命名保留身份，同结构追加不换绑定，整体替换/删除重建产生新身份。
同名替换时页面展示旧/新对象身份及旧字典精确版本，可显式复用旧定义并重新应用，旧绑定不会迁移。
数据库使用持久连接实例及限定表身份；可取得的 PostgreSQL OID、ClickHouse UUID 参与对象隔离。
连接改指向或可观测表重建需要重新确认。SQLite/MySQL 等无法证明库外同名重建的来源为 identity_state=unknown，
需明确确认本次语义连续性；确认是用户声明，不能证明未观察的外部 DDL 或业务含义从未变化。

v2 绑定保留 source_snapshot 和 confirmation；只有 channels/apply 重取来源并核验后才在同一事务中建立服务端观察记录。
导入/通用 save 的快照只作历史声明，即使内容自洽也不会自行变为可信当前观察，需要 observation 确认并重新应用。
旧 v1 的 declared_schema 继续表示人工声明，显式升级时保留并单独检查，旧版本保持不变。

## 人工绑定与保存后应用

“新建绑定”需要独立名称、对象标签、Provider 身份、具体数据集身份、字典精确版本和字段映射。
`object_key={provider_id,object_id}` 在部署内唯一且创建后不可改变；它标识数据集，不是整个数据库连接。

字段映射方向为字典 field_id → 来源字段名，必须覆盖全部字典字段且来源字段不能重复。
声明结构可为 null/空数组，或 `{name,type,nullable}` 的有序列表：

```json
{
  "field_map":{"request_id":"id","elapsed":"elapsed_ms"},
  "declared_schema":null
}
```

- 没有声明结构时，`structure_check=unknown`。
- 提供结构且映射的字段名、类型与可空性一致时，`structure_check=matched`；允许额外源列。
- 缺列、重复或类型/可空性不匹配会返回 `BINDING_MISMATCH`。
- matched 只说明与给定人工结构声明相符，实际数据验证仍为 not_run。

保存字典不会更新已有绑定。在字典编辑区选择对象，可使用“保存并应用”；也可先“保存新版本”，再“应用到对象”。
这两步是独立事务：若定义保存成功但绑定更新冲突，页面会同时显示“定义已保存”和“绑定未更新”，保留已提交版本。
重新选择当前绑定版本后再应用。旧绑定和旧字典版本仍可追溯。

## JSON / YAML 导入导出

页面支持选择文件或粘贴内容，选择格式后“校验导入预览”，核对规范文档和缺口，再“采用预览”和保存。
预览不创建 revision。更改字典结构的导入也必须显式启用结构编辑。

完整可导入样例：
[标准](../examples/semantics/standard.json)、[字典](../examples/semantics/dictionary.json)、
[绑定](../examples/semantics/binding.json)。依次以 `standard/request.duration`、`dictionary/app.requests`、
`binding/app.requests` 保存；字典引用标准 @1，绑定引用字典 @1，依赖目标需要先存在。

导出信封包含 `kind,name,source_reference,document`，JSON 和 YAML 可互相回导。
导入时可明确指定新名称；source_reference 只记录来源，新对象仍从 revision 1 开始。
导入不会递归创建引用依赖，也不会改写已声明的 origin。原始文档与导出信封都不接受自定 revision。

拒绝重复键、未知属性、非有限数字、超出无损 64 位范围的整数、YAML 自定义标签/alias 和远端 Schema 引用。
浏览器、JSON 与 YAML 交换保留 64 位整数，不经过 JavaScript 浮点舍入。
文档/导入原文上限 8 MiB、控制请求和响应上限 12 MiB、嵌套深度上限 64。
超限明确失败，不截断后保存。

## HTTP 契约与可运行示例

所有控制操作使用 POST JSON；成功为 `{data:...}`，错误为 `{error:{code,message,field_path,details}}`。
部署内前缀为 `/semantics/v1/`，Web 对外前缀为 `/api/semantics/v1/`。
浏览器及普通客户端使用同一套接口，机器可读文件通过普通 GET 获取：

```text
/api/semantics/v1/openapi.json
/api/semantics/v1/api.schema.json
/api/semantics/v1/documents.schema.json
```

仓库发布文件位于 [config/semantics](../config/semantics/openapi.json)，使用 OpenAPI 3.1 和 JSON Schema Draft 2020-12。

| POST 操作 | 用途 |
| --- | --- |
| describe | `target` 为明确通道 locator 或算子 category.name；返回同一对象的结构、说明、契约、核对与精确依据 |
| channels/save-description | `locator`、完整 `basis`、`changes` 及人工确认项；系统生成内部身份/映射并保存当前对象说明 |
| operators/save-description | `name`、`expected_component_revision`、`expected_description_ref`、`notes`；只保存当前组件的补充文字 |
| list | kind/q 筛选 current 摘要，返回精确引用与缺口数量 |
| resolve | exact_reference 读取完整文档，或 view=fields 读取明确标记的字段投影 |
| history | kind/name 的修订历史，按 revision 降序 |
| save | kind/name/document、expected_current_revision；可选 base_revision/change_note |
| references | 查询目标精确版本的 current/history 引用 |
| binding_lookup | 按 object_key 取得当前 binding 及字典精确引用 |
| import | kind/name/format/content，返回待保存规范文档，预览不写入 |
| export | exact_reference/format，返回可回导信封和 content |
| channels/describe | locator 获取来源结构快照和本次观察时间 |
| channels/preview | locator、可选精确字典与显式映射/重命名/删除，返回候选、差异和确认项；不写入 |
| channels/check | locator 重取来源，返回精确引用、核对状态、结构/语义缺口 |
| channels/apply | 精确字典、映射、expected binding revision、来源基准和确认项；重取来源后原子保存 v2 绑定 |
| operators/check | 实际算子、算法参数和输入/输出定位符；显式执行静态契约预检，不运行算法 |

统一描述的两个完整请求示例：

```json
{"target":{"kind":"channel","locator":{"kind":"dataframe","name":"samples"}},"timeout_ms":5000}
```

```json
{"target":{"kind":"operator","name":"builtin.passthrough"},"timeout_ms":5000}
```

成功数据包含 `subject`、`basis`、`structure`、`description`、`contract`、`checks`、`gaps` 和 `context`。
`structure` 表达当前来源，`description` 区分作者声明/绑定字典与本地补充，`contract` 保留作者执行契约。
`basis` 包含当前对象身份、source revision、结构摘要及 binding/dictionary/component/contract/description 精确引用，
不适用项为 null。保存通道说明时原样回传完整 basis，不能自行构造或只取定义库的最新版本。

`channels/save-description` 的 `changes` 只提交已编辑属性：对象 `description`、`entity` 中的行/时间/关联等语义，
以及按来源字段名索引的 `fields`。各属性沿用下文 SemanticValue 的 state/value/origin 结构；
字段名称、类型、可空性及内部 field_id 不可通过 changes 改写。`confirmations` 只提交用户实际确认的键。
返回 `status=applied` 才表示完成当前关联；`status=definition_saved_not_applied` 同时返回已保存的 `dictionary_ref`、
当前 `binding_ref`、`basis` 与具体 `error`。部分成功也可以是 HTTP 200，客户端必须检查 status，并保留草稿与精确引用。
来源变化、并发修订或缺少必要确认返回 409；相同内容重试复用已保存依据。

`operators/save-description` 的 notes 固定为
`{"purpose":"用途","usage_notes":"使用注意","examples":["调用示例"]}`。
expected 字段使用 describe 的 `basis.component_revision` 与 `basis.description_ref`，首次后者为 null。
保存返回精确 `description_ref`、`component_revision` 和 `created_revision`；内容相同可返回原修订。
notes 复用 Config 版本资源，`schema_id=flowsql.operator-description.v1`，按具体组件关联。
读取精确 Config 内容时使用原 `/api/channels/config/resolve`，解码其 `content_base64`；执行契约继续使用语义 resolve。

统一 describe 和通道保存的 timeout_ms 默认 5000，范围 1～30000。描述读取的各子调用共享一次总预算，
耗尽返回 504/SOURCE_TIMEOUT；未执行显式预检时不会把描述响应当作 compatible。
响应保留解释所需的实体/行/时间/NULL 上下文和精确引用；不会自动展开整个引用图。

首次保存 `expected_current_revision=0`，后续传读取时的 current revision。
内容与当前规范文档相同时返回同一版本、`created_revision=false`；不同内容且 expected 不匹配返回 409。

在仓库根目录使用 Python 标准库演示首次导入三个样例（无需外部数据库或模型服务）：

```bash
export FLOWSQL_SEMANTICS_URL=http://127.0.0.1:8081
python3 - <<'PY'
import importlib.util, json, os
from pathlib import Path
spec = importlib.util.spec_from_file_location('client', 'examples/semantics/client.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
client = module.Client(os.environ['FLOWSQL_SEMANTICS_URL'])
for kind, name in [('standard', 'request.duration'), ('dictionary', 'app.requests'), ('binding', 'app.requests')]:
    preview = client.call('importDataSemantics', {
        'kind': kind, 'name': name, 'format': 'json',
        'content': Path(f'examples/semantics/{kind}.json').read_text()})
    saved = client.call('saveDataSemantics', {
        'kind': kind, 'name': name, 'expected_current_revision': 0,
        'document': preview['document']})
    print(json.dumps(saved, ensure_ascii=False))
PY
```

已有同名不同内容会明确冲突；不要用演示命令覆盖现有定义。
样例保存后，通过[普通客户端](../examples/semantics/client.py)读取绑定、字段说明和引用：

```bash
python3 examples/semantics/client.py "$FLOWSQL_SEMANTICS_URL" manual app.requests --field elapsed
```

客户端先按 OpenAPI 的 operationId 发现接口，再从 object_key 取得精确绑定和字典版本，读取字段页、缺口、
实体局限及历史引用；无大模型依赖。Python 保留返回的整数精度。

字段页包含 `projection=true`、实体上下文、`context_fields`、精确标准上下文和 `required_context_refs`。
后者列出仅保留必要结构而未展开全部语义的字段；字段页不能直接当作完整文档覆盖保存。
`semantic_gaps` 保持原文档路径。不存在的 field_id 明确失败。

分页 limit 默认 50、范围 1～100，读取 `has_more` 与 `next_cursor`；继续请求时保留相同筛选与排序参数。
列表每项携带当次精确版本，不是跨请求快照；字段页固定同一 revision。
示例客户端保留分页标记，第一批结果不代表完整清单。

| HTTP 状态 | error.code | 处理含义 |
| --- | --- | --- |
| 400 | INVALID_DOCUMENT / INVALID_REFERENCE | 修正文档形状、内部字段或精确引用 |
| 404 | NOT_FOUND | 对象/版本不存在，先登记依赖或核对版本 |
| 409 | REVISION_CONFLICT / BINDING_MISMATCH | 核对当前版本，或修正映射/结构声明 |
| 409 | SOURCE_CHANGED / CONFIRMATION_REQUIRED | 保留草稿，重新核对对象/组件及来源差异，再明确确认 |
| 413 | LIMIT_EXCEEDED | 减小文档或使用有界字段页；响应上下文过大也可触发 |
| 503 | UNAVAILABLE / SOURCE_UNAVAILABLE | Config Provider、来源认证/目录或代理链路不可用 |
| 504 | SOURCE_TIMEOUT | 本次元数据总预算耗尽，不能沿用旧 matched |
| 500 | STORAGE_ERROR | 存储事务/完整性错误，检查部署日志与持久目录 |

程序按状态与 code 分支，不解析 message 判断业务结果。

## 通道 HTTP 示例

以下代码对已经保存的 Kafka 结构化 source `events` 生成候选；将 locator 换成
`{"kind":"dataframe","name":"sample"}` 或
`{"kind":"database_table","type":"postgres","name":"analytics","database":"app","schema":"public","table":"requests"}`
可使用同一控制流程。DataFrame pending 时先通过现有显式加载入口加载，再请求预览。

```python
import importlib.util, os
spec = importlib.util.spec_from_file_location('client', 'examples/semantics/client.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
client = module.Client(os.environ['FLOWSQL_SEMANTICS_URL'])
locator = {'kind': 'kafka_source', 'name': 'events'}
preview = client.call('previewChannelDictionary', {'locator': locator, 'timeout_ms': 5000})
assert preview['source_snapshot']['schema_state'] == 'known'
# 补充 preview['dictionary'] 的人工语义，核对差异、引用范围及 required_confirmations。
saved = client.call('saveDataSemantics', {
    'kind': 'dictionary', 'name': 'app.events', 'expected_current_revision': 0,
    'document': preview['dictionary']})
source = preview['source_snapshot']
# 首次绑定示例：存在绑定时必须采用读取时的当前 revision。
# 只有人工逐项确认后，才把已确认键填入 confirmations；不自动同意所有项。
applied = client.call('applyChannelDictionaryBinding', {
    'locator': locator, 'name': 'app.events', 'dictionary_ref': saved['exact_reference'],
    'field_map': preview['field_map'], 'expected_current_revision': 0,
    'source_basis': {k: source[k] for k in ['object_key', 'source_revision', 'schema_fingerprint']},
    'confirmations': []})
check = client.call('checkChannelDictionaryBinding', {'locator': locator})
assert check['binding_ref'] == applied['exact_reference']
key = source['object_key']
context = client.consume(key['provider_id'], key['object_id'])
assert context['dictionary_fields']['exact_reference'] == check['dictionary_ref']
```

四个通道操作的 timeout_ms 默认 5000，范围 1～30000；Web/Gateway/Router/数据库读取共用一次总预算。
来源超时返回 504/SOURCE_TIMEOUT；不可访问或认证/目录读取失败返回 503/SOURCE_UNAVAILABLE；
不支持来源和不完整结构分别返回 400/UNSUPPORTED_SOURCE、400/INCOMPLETE_STRUCTURE，
确认缺失为 409/CONFIRMATION_REQUIRED，来源变化为 409/SOURCE_CHANGED。
错误时不沿用上一次 matched。保存和应用是两个事务，保存成功后遇到应用错误不会撤销字典。

## 进程内消费与验证范围

C++ 插件通过 IQuerier 查找 [IDataSemanticsRegistryV1](../src/framework/interfaces/idata_semantics_registry.h)，
用 Resolve 读取精确引用。返回快照独立拥有规范 JSON 内容和摘要；失败清空输出，不返回上一次内容。
消费者不接触 SQLite 句柄，不在逐行/逐包数据面查询注册表。

C++ 对象说明调用通过独立 [IMetadataDescriptionV1](../src/framework/interfaces/imetadata_description.h) IID，
提供 Describe、SaveChannel、SaveOperator JSON 信封接口；原执行 ABI 不变。跨进程仍走 HTTP。
回归入口和依赖配置见[测试说明](testing.md)。本次对象说明验收使用隔离原生进程、临时 SQLite、
DataFrame/CSV、保存的 Kafka 配置、真实 Python/C++ 组件、普通客户端和 Chromium，覆盖冲突、部分成功与重启。
其他数据库通过既有 locator/类型契约复用 provider，本次未重跑其真实认证和数据面矩阵；
外部 Kafka 数据面/认证矩阵、容器交付和全产品发布也未纳入。结构匹配不等于业务数据质量验证。


## 算子输入输出契约

算子作者在注册时一次声明静态契约，使用者继续按原有 SQL 指定来源、算子、算法参数和目标。
SQL 不需要传契约文档、版本或另一份字段角色映射。没有声明的历史算子仍能执行，显示
`contract_state=missing`、兼容性 `undetermined`；已声明却丢失定义或服务时会报错。

“算子管理”详情按组显示组件摘要、作者版本、不可变契约版本、参数约束与嵌套分支、字段选择规则、各输出、行分支与未知项；
补充文字在同一详情维护，完整作者 JSON 在技术详情中读取。
C++ 插件详情中的算子按钮可以打开对应声明。切换历史版本只改变阅读视图；预检始终重新取得当前组件的关联。
预检输入为实际来源和既有算法参数；服务端从 F2 描述器取得结构/绑定并解析角色，不读取尚未加载的 CSV 业务行。
源码和预检输入保存在当前浏览器会话中；刷新可恢复草稿，组件版本冲突时保留原稿和原版本依据。
核对冲突后关闭编辑框、重新打开当前版本，再合并改动并保存。

### 作者创建与上传

Python 装饰器新增可选的 `data_contract`，接受完整 JSON 对象或 `{contract_ref: 精确版本}`。
创建页可以填写“静态契约 JSON”，页面将其写进现有装饰器源码；也可以直接在源码中内嵌声明。
上传 `.py` 使用相同声明，Python 执行 API 仍为单 DataFrame 输入、单 DataFrame 输出。
复制契约要求实际 Arrow 字段类型和可空性也一致；Polars/Pandas 的默认 Arrow 导出可能改变这些结构属性。
若作者声明严格复制非空字段，应显式生成对应 Schema；实际结构变化会报告 `OUTPUT_SCHEMA_MISMATCH`，不会自动放宽声明。
下面是可创建为 `demo_identity.py` 并上传的最小复制算子：

```python
from flowsql import OperatorBase, register_operator

CONTRACT = {
    "schema_id": "flowsql.operator-data-contract.v1",
    "title": "复制实际输入字段",
    "operator": {"category": "demo", "name": "identity", "version": "1"},
    "configuration": {
        "invocation_schema": {"type": "object", "additionalProperties": True},
        "affects": {},
    },
    "inputs": [{"port_id": "input", "min_count": 1, "max_count": 1,
                "shape": "dataframe", "roles": []}],
    "outputs": [{"port_id": "output", "shape": "dataframe", "mode": "derived",
                 "rules": [{"kind": "copy", "source_port": "input", "all_fields": True}]}],
    "limitations": ["只检查结构，不验证业务行质量"],
}

@register_operator(category="demo", name="identity", data_contract=CONTRACT)
class Identity(OperatorBase):
    def work(self, df):
        return df
```

创建或上传成功后，在目录中激活，再执行：

```sql
SELECT * FROM dataframe.input USING demo.identity INTO dataframe.identity_result;
```

其中 `dataframe.input` 是已有的实际数据集，可替换为自己的来源。参数的生产默认值来自算子共用的解析器，
通用检查不会根据 Schema 的 `default` 擅自填值。源码编辑提交读取详情时的 `expected_component_revision`；
变更组件或声明不会覆盖运行任务已经取得的组件租约与精确版本。

C++ 插件可在现有 ABI 导出之外提供可选的 `flowsql_operator_data_contract_v1(int index)`，
返回随库携带的完整文档或精确引用；不改变旧执行虚表和 V2 descriptor 的 `contract_iid`。
可编译样例见 [C++ 契约输出算子](../src/tests/test_builtin/fixtures/fixture_contract_output.cpp)，
`cmake -B build src` 后执行 `cmake --build build --target fixture_contract_output -j8`，
将生成的 `build/output/libfixture_contract_output.so` 通过 C++ 插件页上传和激活。
该样例提供复制、重命名、显式 ms→s、五种聚合及双输出，适合用临时数据集验证接口。

### 普通 HTTP 客户端

OpenAPI 的 `checkOperatorDataContract` 对应 `POST /api/semantics/v1/operators/check`：

```json
{
  "operator": {"category": "demo", "name": "identity"},
  "parameters": {},
  "inputs": [{"input_index": 0, "locator": {"kind": "dataframe", "name": "input"}}],
  "output_locator": {"kind": "dataframe", "name": "identity_result"}
}
```

`parameters` 的值与 SQL WITH 一样为字符串。`inputs` 只包含按顺序编号的 F2 定位符；
不接受客户端 Schema、替换契约或独立 `role_map`。F2 的数据库输入定位符指定实际限定表，
复杂算子的生产适配器按已有配置继续展开其他 dataset；所选输入必须与配置对应。
`output_locator` 可省略，依赖目标的输出会保持未确定；具体数据库表是普通表目标，不等同托管通道。

下面的标准库客户端以已上传并激活的 `demo.identity` 和已有的 `dataframe.input` 为例，
从概要/局部资源/闭合文档到预检、普通 SQL、精确使用依据及必要字典上下文完成读取。
运行前设置 `FLOWSQL_SEMANTICS_URL` 为实际 Web 地址，例如 `http://127.0.0.1:8081`：

```python
import json
import os
import urllib.request

base = os.environ["FLOWSQL_SEMANTICS_URL"].rstrip("/")
def post(path, body):
    req = urllib.request.Request(base + "/api/" + path,
        data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.load(response)

def resolve(ref, **view):
    return post("semantics/v1/resolve", {"exact_reference": ref, **view})["data"]

detail = post("operators/detail", {"name": "demo.identity"})
ref = detail["contract_ref"]
summary = resolve(ref, view="summary")
input_rules = resolve(ref, view="resource", resource_pointer="/inputs")
closed_contract = resolve(ref)["document"]
checked = post("semantics/v1/operators/check", {
    "operator": {"category": "demo", "name": "identity"}, "parameters": {},
    "inputs": [{"input_index": 0, "locator": {"kind": "dataframe", "name": "input"}}],
})["data"]
print(checked["check"])
if checked["check"]["state"] == "incompatible":
    raise RuntimeError("输入不符合静态契约")
result = post("tasks/batch/execute", {
    "sql_text": "SELECT * FROM dataframe.input USING demo.identity INTO dataframe.identity_result",
    "mode": "sync",
})
for check in result["contract_checks"]:
    use = resolve(check["use_ref"])["document"]
    for output in use["outputs"]:
        print(output["port_id"], output["state"], output.get("context_state"))
        if "dictionary_ref" in output:
            print(resolve(output["dictionary_ref"])["document"])
        for context_ref in output.get("required_context_refs", []):
            print(resolve(context_ref))
        for context in output.get("semantic_contexts", []):
            for context_ref in context.get("required_context_refs", []):
                print(resolve(context_ref))
```

预检的 `compatible|incompatible|undetermined` 都是 HTTP 200 的正常结果；结构未取得、身份未知、
缺少语义绑定等情况保留缺口。普通执行明确不兼容返回 422，组件/来源冲突返回 409，服务不可用返回 503。
真正执行会重新解析并冻结当次组件、配置、来源和契约，预检结果不是可复用的执行计划。
输出结构不符会返回执行错误；数据已产生而元数据发布失败时会明确记录数据/元数据状态。

### 多实体、行规则与未知语义

输出结构实际生成后，系统才记录 `operator_use`、结果字典及绑定；这些记录共享同一事务。
未接入观察的路径显示 `output_check=not_observed`，不会生成声称已匹配的结果绑定；
`absent` 表示条件确定为不产生该实体，与“尚未观察”不同。
结果同名替换会产生新的 F2 对象身份，历史任务仍可按精确 use/契约/字典引用追溯。
普通 save/import 不能制造声称已观察的使用记录。

[Baseliner 静态声明](../config/semantics/examples/operator-contract-baseliner.json) 分别描述 `results`、
`model_parameters`、`relation_fusion` 和 `maintenance`。同一数值列可能包含 Value、Ratio 或 Relation 子指标，
其解释必须结合 dataset/metric 身份、来源字典、模型/时间等必要上下文；`unit` 列的 value/ratio 类别不是物理单位。
客户端应核对 `context_state` 和各 `required_context_refs`，缺引用时保持 incomplete；
Ratio 或未知转换不会按同名字段猜测、继承物理单位。

`row_cases` 说明 evaluation/forecast、未训练模型等条件下 NULL 的含义，不执行逐行数据质量验证。
例如 forecast 的 observed/can_alert 为 NULL；冷启动 evaluation 可能没有区间，不能把同次任务的 forecast
也一概视为没有区间。`data_validation=not_run` 始终不表示预测算法、异常判定或所有业务行已经验证。
进一步的结果/图表就地解释属于独立的消费展示能力。
