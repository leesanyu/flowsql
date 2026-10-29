# 标准与字典管理

FlowSQL 的“标准与字典”页面与 HTTP API 管理同一份版本库。数据维护者可人工定义标准、实体/字段字典，
并将具体数据集登记为绑定；普通程序可读取绑定的精确字典版本、未知属性和使用局限。

当前能力覆盖人工录入、JSON/YAML 导入，以及 Kafka 结构化 source、CSV/命名 DataFrame、已有数据库限定表的结构生成与绑定。
算子可选声明输入输出契约，系统在执行阶段检查并记录结果依据；NPM 领域字典和查询/探索中的展示接线属于独立 Feature。生成与核对读取来源元数据，
不会消费 Kafka 消息、扫描业务行、改写业务数据、执行单位转换或自动升级任务。

## 入口与持久化

完成[标准构建及启动](../README.md#快速开始)后，从 Web 顶栏选择“标准与字典”，地址为 `/semantics`。
默认 Web 地址为 `http://127.0.0.1:8081`。现有单/多进程配置加载 `libflowsql_config_channel.so`，
经 Web → Gateway → Router → Config 提供控制 API。

语义库复用 Config 插件的 `db_path`，在同一个 SQLite 数据库中使用独立的
`semantics_object`、`semantics_revision`、`semantics_reference`、`semantics_observation` 表。默认配置为 `./meta/flowsql_meta.db`，
`./start.sh` 从 `build/output` 运行，因此默认实际路径是 `build/output/meta/flowsql_meta.db`。
部署时将此数据库所在目录放在持久存储中；备份和恢复应包括完整数据库。迁移事务失败会使插件加载失败，
不会退回内存库。原有普通配置通道保持自己的数据和接口。

使用部署现有访问控制边界；本功能没有增加独立登录系统或对象级权限隔离。

## 定义与未知信息

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

## 页面操作

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

## 从通道生成与绑定

在“通道管理”中使用以下入口：

| 对象 | 入口与结构依据 | 适用边界 |
| --- | --- | --- |
| DataFrame / CSV | 行操作“数据字典”；已加载对象使用真实 Arrow Schema | pending 不会隐式加载；点击“显式加载 DataFrame 结构”经已有加载入口取得结构 |
| Kafka | 结构化 source（含 both）的“数据字典” | 使用已保存配置和真实解码 Schema，保留 JSON Pointer、显式单位及五个 `__kafka_*` 列；packet 和 sink-only 不支持 |
| SQLite/MySQL/PostgreSQL/ClickHouse | 连接行“表字典”，填写明确 table 及适用的 database/schema | 一张限定表一个绑定；读取系统目录，空表可用；不接收 SQL、不登记整个连接或扫描全库 |

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
| 413 | LIMIT_EXCEEDED | 减小文档或使用有界字段页；响应上下文过大也可触发 |
| 503 | UNAVAILABLE | Config Provider 或代理链路不可用 |
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

回归入口见[测试说明](testing.md)。本 Feature 验证隔离原生进程、临时 SQLite、真实控制链路和实际 Chromium 页面，
覆盖三类入口、四库真实目录、同时应用冲突、观察记录事务回滚、重启、精确快照/标准上下文与旧配置字节保留。
Kafka 元数据生成不运行 Consumer；本验收不覆盖外部 Kafka 数据面/认证矩阵、容器交付矩阵或全产品发布，结构匹配不等于业务数据质量验证。


## 算子输入输出契约

算子作者在注册时一次声明静态契约，使用者继续按原有 SQL 指定来源、算子、算法参数和目标。
SQL 不需要传契约文档、版本或另一份字段角色映射。没有声明的历史算子仍能执行，显示
`contract_state=missing`、兼容性 `undetermined`；已声明却丢失定义或服务时会报错。

“算子管理”详情显示组件摘要、作者版本、不可变契约版本、参数 Schema、字段选择规则、各输出、行分支与未知项。
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
