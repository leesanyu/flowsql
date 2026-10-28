# 标准与字典管理

FlowSQL 的“标准与字典”页面与 HTTP API 管理同一份版本库。数据维护者可人工定义标准、实体/字段字典，
并将具体数据集登记为绑定；普通程序可读取绑定的精确字典版本、未知属性和使用局限。

当前能力覆盖人工录入和 JSON/YAML 导入。通道结构自动发现、算子输入输出契约、NPM 领域字典和查询/探索中的
自动接线属于后续独立 Feature；登记绑定不会连接数据源、改写业务数据、执行单位转换或自动升级任务。

## 入口与持久化

完成[标准构建及启动](../README.md#快速开始)后，从 Web 顶栏选择“标准与字典”，地址为 `/semantics`。
默认 Web 地址为 `http://127.0.0.1:8081`。现有单/多进程配置加载 `libflowsql_config_channel.so`，
经 Web → Gateway → Router → Config 提供控制 API。

语义库复用 Config 插件的 `db_path`，在同一个 SQLite 数据库中使用独立的
`semantics_object`、`semantics_revision`、`semantics_reference` 表。默认配置为 `./meta/flowsql_meta.db`，
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

## 进程内消费与验证范围

C++ 插件通过 IQuerier 查找 [IDataSemanticsRegistryV1](../src/framework/interfaces/idata_semantics_registry.h)，
用 Resolve 读取精确引用。返回快照独立拥有规范 JSON 内容和摘要；失败清空输出，不返回上一次内容。
消费者不接触 SQLite 句柄，不在逐行/逐包数据面查询注册表。

回归入口见[测试说明](testing.md)。本 Feature 验证隔离原生进程、临时 SQLite、真实控制链路和实际 Chromium 页面，
覆盖同时写入、事务回滚、重启、精确快照与旧配置字节保留。声明结构匹配的结论不扩展为业务数据质量或外部数据源接入保证。
