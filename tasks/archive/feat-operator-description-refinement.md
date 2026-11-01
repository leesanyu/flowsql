# Feature：算子契约说明重构与精简（operator-description-refinement）

状态：`[x]` 已完成，T1～T5 与 A1～A14 全部验收通过；优先级：P1；日期：2026-10-31。
依赖已交付的 [算子输入输出契约](../archive/feat-operator-data-contract.md) 与 [通道与算子说明管理](../archive/feat-metadata-description-query.md)。
后续用户验收修正 R1～R9 已完成，当前页面布局与说明编辑契约以文末修正记录为准；原 T1～T5 证据保留。
依据当时确认的“六项内容”、系统整理重复信息与内部依据、复杂内容按需读取、未知信息保留，以及 Python“基本信息、源码在前，契约入口在后”的决定；本规格承载既有契约核查、布局、读取契约与验收边界。
与 [通道说明配置重构与精简](../archive/feat-channel-description-refinement.md) 独立验收，不以其实施完成为前置条件，不改变原 Feature 的完成结论。

## 业务意图与 Non-Goals

先核查既有算子契约与真实实现是否一致，纠正有证据的声明错误并明确缺口；再让阅读或维护 Python、内置及 C++ 算子的用户和未来模型，围绕六项必要内容理解和配置调用，减少内部状态、重复内容和完整 JSON 的阅读负担。
交付后，三类算子共用“使用说明 / 调用检查”面板；页面和普通 HTTP 客户端读取同一份精简说明，背后仍是一份完整契约。Python 源码保留主要位置，说明按需展开。

- 不重构算法、配置适配器、批/流调度或数据面，不扩展 Python 单 DataFrame 执行形态；发现算法问题按跨任务规则隔离。
- 不要求向后兼容：允许调整本 Feature 涉及的算子契约结构、HTTP/IID 请求响应及页面流程，直接调用方和测试同次更新；不维护旧协议、旧布局、旧草稿格式或旧版本适配，不设计历史数据迁移。
- 不新增持久模型、独立 HTTP 路由或公共 IID，不把结构化契约改存为自由文本；现有接口与资源读取能力作为实现起点，完整结构与精简视图以本次冻结的契约为准。
- 不引入新的作者简写语言；自动整理限于核查后事实的确定性提取、归并与引用。作者声明的修正必须有实现与测试依据，不补猜业务默认值、时间单位或输出公式。
- 不调用大模型、不实现 Agent/MCP、自动关联、累计指标/比率/复杂业务指标计算；本 Feature 不承诺模型实际调用效果。
- 不扩展支持的通道、输入形态或输出目标，不实现通用全 JSON Schema 表单平台，不重构通道说明页或全站管理布局。
- 不为说明阅读或调用检查加载 CSV 业务行、消费 Kafka、执行算法或创建任务；调用检查不产生可复用的执行计划。

## 既有算子契约核查

契约核查是本 Feature 的交付结果，区别于页面中对某次输入与参数的调用检查；通过 Schema 校验或调用预检不能单独证明作者声明正确。

- 核查清单以实施起点的仓库注册和发布来源为准，逐项登记随产品交付的内置/C++ 算子及仓库提供的 Python 算子；已有声明全部核对，无声明项明确记录缺口。真实扩展插件用于验证公共发布与读取流程，不扫描用户私有上传或任意第三方插件。
- 以 `builtin.passthrough`、Baseliner 和卡方为代表；Python 仅有测试组件时明确其身份，不把测试声明当作生产算子覆盖。NPM 领域说明仍归独立 Feature，不在本次补建完整语义包。

| 核查维度 | 声明与实现的对应依据 |
| --- | --- |
| 身份与输入 | 注册/导出身份、输入数量与形态、必需字段、selector 及实际输入适配规则 |
| 参数与配置 | 生产解析器的参数名称、必填/互斥条件、默认行为、允许值、JSON 文本解码及字段映射 |
| 输出与限制 | 实际输出构造的字段/动态列、行类别、NULL/有效条件、副输出条件、必要上下文和已知限制 |

- 每项留下算子/组件/契约标识、核查维度、源码依据、对应测试及“已核实 / 已修正并验证 / 信息缺口”的结论；有证据的错误声明必须修正并以真实解析或最小执行样本验证，信息缺口不得写成已核实。
- 测试预期来自实现和独立样本；新增/修改声明的关键边界转为可执行断言。核查测试可显式运行最小样本，页面阅读和调用检查仍不得运行算法；不重验完整算法精度、性能或全部后端矩阵。
- T1 同时冻结完整契约与 usage 的具体 JSON Schema、参数控件/字符串编码映射、局部详情规则，以及简单算子和 Baseliner 的代表响应，后续服务端与页面据此实施。

## 已确认的六项内容与简化方式

六项是页面与程序共同使用的阅读结构，不是要求用户再填写六组表单；已有契约和补充说明由系统整理，人工补充仍使用原有入口。

| 六项内容 | 使用者和模型需要知道什么 | 表达方式 |
| --- | --- | --- |
| 算子身份与用途 | 调用哪个算子、解决什么问题、适用什么场景 | 名称和简短用途；作者说明与本地补充可辨别 |
| 输入要求 | 需要几个输入、什么形态、哪些字段及必要条件 | 紧凑结构化要求配合字段文字说明 |
| 调用参数 | 参数名称、是否必填、允许取值、怎样指定输入字段 | 参数与对应字段要求合并表达，保留必要约束和已声明默认值 |
| 输出含义 | 返回哪些结果、字段表示什么、不同结果行如何解释 | 输出摘要和字段说明，复杂条件按需展开 |
| 使用限制 | 哪些前提影响使用和解释结果 | 文字为主，已有可校验约束保留结构化形式 |
| 调用示例 | 怎样组合来源、算子和参数 | 可选的已有文本或代码示例，可复制、不自动执行 |

- 调用必须知道的信息清楚保留，例如字段对应参数、时间点/时长、秒/毫秒和已知时区；用途、统计解释和注意事项用文字即可，不为每种业务描述增加配置项。
- 明确一对一的参数与字段要求合并呈现，例如“col1：选择输入表中的第一个分类字段”；复杂多输入映射保留原关系，不能为简短而丢掉条件。
- 算子身份、版本与已有字段关系由系统取得和核对；普通阅读不要求用户填写代码摘要、内部角色编号或版本引用。
- 简单算子直接读六项；复杂算子先给必要摘要和详细规则入口，检查后优先展示与本次调用有关的输出及条件。尚不能确定的分支保留提示，不当作不存在。
- 未声明的空白可选项减少展示；这类省略表示未声明，不表示任意值都合法、已经检查或不适用。已知但按需展开的内容保留详情入口；影响判断的缺口明确显示，系统与模型不能按字段名猜含义。

## 用户可见布局

### 对象入口与页面顺序

| 算子类型 | 从上到下的布局 | 交互保证 |
| --- | --- | --- |
| Python | 基本信息 → 源码查看器/编辑器 → “说明与契约”按钮 → 按需展开的说明面板 → 原保存/关闭操作 | 每次打开详情默认收起说明，入口和面板均不放在源码上方；源码保留最大化、行号、滚动和草稿能力 |
| C++ 插件 | 插件基本信息 → 插件内算子列表 → 选中算子的说明面板 → 原插件操作 | 继续从列表选择具体算子，切换后对象身份明确；不把插件当作单个算子 |
| 内置 | 算子基本信息 → “说明与契约”入口及按需面板 → 原关闭操作 | 复用相同内容，不引入源码或插件编辑操作 |

- Python 展开/收起说明不丢源码草稿；源码最大化时以代码为主，退出最大化后恢复原说明展开状态。
- 源码保存与补充说明保存分开；本次会话内详情切换、刷新和关闭后重开不丢源码、说明与调用草稿；保留激活/去激活、删除和插件更新功能，操作遵守当前组件生命周期约束。

### 共用说明面板

| 区域 | 默认展示与按需操作 |
| --- | --- |
| 面板顶部 | 算子名称、简短作者版本；“编辑补充说明”“刷新”“更多”；更多中放历史版本和适用的作者维护入口 |
| 视图切换 | 默认“使用说明”，用户主动进入“调用检查”；历史版本进入只读阅读状态 |
| 说明正文 | 按上述六项组织；已有内容自动整理，空白可选内容不堆叠占位，复杂参数按组展开 |
| 输出展开 | 所有输出先给摘要，主结果默认展开，其余按需展开；字段/类型与含义并列，条件与不同类型行在所属输出内查看 |
| 示例展开 | 默认折叠，可复制已有示例，不自动执行 |
| 技术详情 | 默认折叠；保留精确引用、组件版本、完整契约及最近读取结果，可复制 JSON，明确不包含未保存草稿 |

- 默认阅读状态不出现三个大文本框、预检 JSON 编辑器、常驻内部状态串、组件摘要串或精确引用选择器。
- 契约缺失、不可用、来源不可用和版本冲突以可操作的中文提示显示；正常页面不重复展示“未验证”声明。
- 默认主结果优先使用契约明确的主结果；无明确标识时按声明顺序展示首项，不推测业务优先级，其他输出始终可发现。

## 核心数据与接口契约

### 结构化事实与统一精简读取

沿用 `IMetadataDescriptionV1`、`IOperatorDataContractV1`；服务端从完整契约与本地补充说明生成同一份精简读取结果，页面与普通客户端共用，不创建第二份可执行契约。
Web 复用 `/api/semantics/v1/describe`、`resolve`、`history`、`operators/save-description`、`operators/check` 及原对象目录；内部路由仍省略 `/api`。

以下为本 Feature 已交付的读取契约；T2～T4 实现 usage/full 接口及页面消费，T5 完成整体回归与使用说明：

- 算子 `describe` 默认返回 usage，显式 `view:"usage"` 等价；请求示例为 `{"target":{"kind":"operator","name":"explore.chisquare"},"view":"usage"}`，保留有界超时。
- 算子显式 `view:"full"` 才返回完整说明与契约；局部规则通过精确引用和 `resolve` 的 resource 视图读取。通道描述不纳入本次重构，通道指定算子视图或未知 view 返回 `INVALID_DOCUMENT`。
- usage 响应仍采用 `{data: ...}` 信封；`data` 包含 `subject, view, basis, contract_state, usage, gaps, details, required_context_refs`，其中 `usage` 固定为下表六个成员。

| usage 成员 | 核心数据约束 |
| --- | --- |
| `identity` | 对象，包含名称、已有标题与用途文字；作者内容和本地补充分别标明来源，不能由标题猜出未声明用途 |
| `inputs` | 有序输入摘要数组，保留端口关联、数量、形态、字段要求及明确参数映射；复杂集合模板提供同版本详情入口 |
| `parameters` | 参数项及组级约束的对象；保留必填、合法值、声明默认值、字符串/JSON 编码要求，简单字段参数附对应输入要求，跨参数条件不能被拆散丢失 |
| `outputs` | 有序输出摘要数组，保留形态、字段含义、产生条件与行类型提示；必要语义上下文和复杂规则可按精确引用继续读取 |
| `limitations` | 作者限制与本地使用注意的文字项数组，逐项保留来源；补充文字不能覆盖或冒充作者约束 |
| `examples` | 已有示例项数组，保留内容与来源，允许为空，不自动编造可运行示例 |

- `basis` 保留当前组件、契约与补充说明的版本依据，由调用工具保存和核对，普通页面折叠显示；生成视图仍执行原来源变更检查、超时和错误处理。
- `details` 项为 `{section, exact_reference, resource_pointer, reason}`，指向同一精确契约版本的局部资源；缺少必要语义上下文时保留 `required_context_refs`，不使用 latest 或网络引用。
- `gaps` 明确尚未声明的必要信息；已知但需展开的内容记录在 `details`，不能混成未知。缺失契约仍返回 missing 及缺口，已声明却不可读取仍报错，不降级为无约束。
- 精简响应不附带完整契约、全部 resources、归一配置 Schema 或重复作者说明；必要约束可以直接保留，无法可靠归并的部分提供明确摘要与详情入口，禁止静默截断。
- 页面默认请求 usage，展开相关规则或技术详情时才请求精确局部资源或 `describe view:"full"`；不得先下载完整 describe 再仅在前端隐藏，普通 HTTP 客户端必须实际得到精简内容。
- 视图通过固定规则生成，不调用模型、不解析自由文本为机器约束；完整存储、发布与精简读取使用核查后契约，同步验证 JSON Schema/OpenAPI 与 IID、HTTP 两种读取结果。

| 数据 | 真相来源与必须保持的含义 |
| --- | --- |
| 作者契约 | 当前组件的 `contract_ref` 与核查后的精确 resolve 文档；输入、配置、输出、限制及 resources 是完整事实来源，所有简明说明均可追溯到对应声明 |
| 补充说明 | `notes = {purpose, usage_notes, examples}`；不覆盖作者要求、不改变参数默认值、不参与调用符合性判定 |
| 版本依据 | 当前 `component_revision`、`contract_ref`、`description_ref` 与历史阅读引用分别保留；作者版本文本不能替代组件身份 |
| 本地视图状态 | 当前对象、展开状态、所选视图、阅读版本、补充说明草稿、调用草稿及最近检查依据；切换对象时不能混用内容 |
| 调用草稿 | 一份输入顺序、字符串参数映射与可选输出目标；常用表单和高级 JSON 编辑同一份数据，保留未被常用控件覆盖的合法参数 |

- 输入 selector 仅按已声明规则转成“由参数 col1 指定字段”等文字；无法简化的规则在所属区域提供局部展开，不猜参数用途。
- 参数说明与输入要求的一对一合并仅改变阅读组织，保留原字段关联及约束出处；普通调用方无需另外构造角色映射，服务端继续负责检查和输出推导。
- 类型摘要保留精度、嵌套、时间单位/时区等事实；`types=[]` 表示未限定类型，不承诺任意类型都能运行；`nullable=null` 显示“未声明”，不能当作非空。
- Schema `required`、`default`、枚举、范围及条件分支按原含义展示；声明默认值只作提示，不自动提交、不代替算法实际配置结果。
- 多输出、`row_cases`、动态字段、NULL/有效条件、上下文和转换规则不能简化丢失；例如 Baseliner 的 evaluation/forecast 与副输出条件必须可读。
- 复杂规则给出所属区域与必要摘要，并可展开原规则/JSON；不能只在页面最底部保留一份 JSON 就视为已解释已知限制。

### 补充说明、历史与作者维护

- 默认展示文字；点击“编辑补充说明”才进入用途补充、使用注意、调用示例三个编辑项，提供保存和取消；取消回到最近读取内容。
- 保存使用 `{name, expected_component_revision, expected_description_ref, notes}`；服务端执行冲突校验、幂等与持久化，成功后重新读取。
- 组件或说明版本变化时保留原稿与原依据，提示核对后显式采用新依据；刷新、来源变化或编辑器切换不能静默覆盖草稿或自动套用旧说明。
- 历史版本在“更多”中按需加载、分页读取，精确版本只读；历史阅读不混入当前补充说明，不开放补充编辑和调用检查，返回当前组件后再操作。
- Python 作者契约通过源码维护，C++ 作者契约通过插件更新；入口执行当前生命周期约束，更新后重读当前身份，不由补充说明按钮替代发布声明。

### 调用检查草稿与结果

本 Feature 的调用检查请求采用以下结构；参数值最终为算法实际接收的字符串，JSON 文本参数仅编码一次：

```json
{
  "operator": {"category": "explore", "name": "chisquare"},
  "parameters": {"col1": "region", "col2": "result"},
  "inputs": [{"input_index": 0, "locator": {"kind": "dataframe", "name": "sample"}}]
}
```

| 配置项 | 简化交互与协议约束 |
| --- | --- |
| 实际输入 | 按需选择 DataFrame、明确限定的数据库表或结构化 Kafka source/both；复用现有 F2 locator 与只读目录/结构能力，保留高级 locator 编辑，不增加 packet、sink-only 或任意流来源 |
| 多输入 | 依已声明数量展示有序输入行，增删后 `input_index` 从 0 连续；可选来源不代表算子必然支持该形态，是否符合输入要求由调用检查判定 |
| 字段参数 | 只有明确参数 selector 的字段角色才提供对应输入的字段选择；结构未知或参数映射不明确时保留文本方式，不按参数名称猜测 |
| 普通参数 | 简单标量/枚举使用输入或下拉；设置与未设置可区分，空字符串是显式值；控件类型及数字/布尔字符串编码必须依据已核实的参数声明和生产解析规则 |
| 复杂参数 | 嵌套配置按组展开，可编辑原有 JSON 字符串；条件 Schema 或无法可靠映射的类型保留高级编辑，不丢未知但合法键、不重复 JSON 编码、不擅自转换 null |
| 输出目标 | 默认收起、可省略；指定时复用原 `output_locator`，仅 DataFrame 或明确数据库表；依赖目标的输出在缺目标时保持未确定 |
| 执行动作 | “检查输入与参数”显式调用原接口，不执行算法、不保存全局配置；表单/高级编辑切换不丢参数，JSON 无效时保留原文并提示修正 |

- 只有进入调用检查并需要选择来源时才读取相关目录/结构；阅读说明不预取全部数据集或历史页，某个目录不可用不能阻断说明阅读。
- 检查始终针对当前组件；本地输入、参数、输出目标或已观察组件发生变化后，旧结果显示“需重新检查”，不继续作为当前结论。
- 请求期间切换对象或修改草稿，旧响应不得覆盖新对象/新草稿的结果；组件或来源冲突单独报错，不混入调用符合性结论。
- `compatible` → “满足已声明要求”；`incompatible` → “不符合要求”并指出具体项；`undetermined` → “信息不足”并列出缺口；HTTP/服务失败单独呈现。
- 字段映射和输出预览表格化，完整依据、实际参数与来源快照折叠保留；结果区一次说明“仅检查已声明结构与语义，未验证业务数据或运行算法”。
- 根据既有检查结果优先呈现本次有关的字段与输出；明确未启用的输出可收起并注明条件，未确定的输出仍列出缺口，适用的行类型、NULL 含义和上下文不能隐藏。此整理不新增配置推理或算法执行。
- `data_validation=not_run`、`output_check=not_observed` 等原状态保留在技术详情；不把检查成功或输出预览当作实际执行、统计适用性或数据质量保证。

## 主链路

1. **核查、阅读与维护**：实施时按清单对照声明与生产实现、修正并测试 → 页面或普通客户端读取 usage → 按当前精确依据取得六项内容及同版本详情 → 按需编辑补充说明并携带依据保存 → 重读或冲突保留原稿；历史阅读只读，作者声明通过源码/插件维护。
2. **检查调用**：依据六项内容选择实际输入、字段与参数 → 提交字符串参数和明确来源 → 服务端按生产规则解析配置并检查 → 展示本次有关的映射、输出、条件与未知项 → 草稿变化后重检；实际执行通过 SQL 入口重新取得依据，用普通客户端验证，不调用模型。

## Feature Tasks

按 T1→T5 顺序实施，完整验收并勾选当前 Task 后再进入下一项；每次 Atomic Slice 的允许文件、命令和时间盒在工作台冻结。未达到完整验收条件的 Task 保持未完成。

- [x] **T1：交付既有算子契约核查、声明修正及确定的读取数据契约，使后续说明与配置建立在可复核的实现事实之上。**
  验收：A13、A14；核查清单与结论完整，错误声明修正并经测试，缺口明确；完整/usage Schema、参数映射与代表响应冻结。
- [x] **T2：交付页面与普通客户端共用的六项精简读取，使调用所需信息直接可取、复杂规则按需展开且完整事实可追溯。**
  验收：A3、A4 的数据部分及 A11、A12 的接口部分；HTTP/IID 一致，默认 usage、显式 full、精确局部读取正确，覆盖简单 Python、内置/C++、卡方与 Baseliner。
- [x] **T3：交付三类算子的六项阅读布局、补充说明编辑和历史/作者维护入口，使 Python 源码保持主要位置且草稿、身份与版本冲突可控。**
  验收：A1、A2、A3/A4 的页面部分、A5、A6；按需读取与视图切换、最大化恢复、当前/历史隔离、并发修改与重启后重读正确。
- [x] **T4：交付有明确依据的调用检查表单和本次相关结果，使用户能够选输入、填参数并识别不匹配、适用条件与未知。**
  验收：A7、A8、A9；输入顺序、参数往返、复杂 fallback、三态结果、条件输出与行类型、冲突及过期响应均有可执行断言。
- [x] **T5：完成三类入口、直接调用方和受影响流程的回归及使用说明，使核查后的契约、精简布局与程序读取形成可验证的完整交付。**
  验收：A1～A14 全部通过，更新契约核查边界、六项说明及精简/完整读取指引，记录实际构建与运行验证边界，再完成归档。

## 验收锚点与执行边界

| 编号 | 必须转成可执行断言的结果 |
| --- | --- |
| A1 | Python 基本信息→源码→说明入口顺序正确、默认收起；展开/收起、刷新、关闭重开和最大化恢复不丢源码/说明/调用草稿；C++ 多算子及内置入口身份正确 |
| A2 | 默认使用说明、主动进入调用检查；编辑表单/历史/技术详情按需打开；说明阅读不请求全部来源或历史页，目录失败不阻断阅读 |
| A3 | 六项布局与参数/字段合并准确；类型/可空性、必填/声明默认值/限制和时间单位不丢；空 types、未知 nullable 与省略项不误判，作者和本地补充可区分，默认值不写入请求 |
| A4 | 卡方混合行、动态列与 error 行，Baseliner 多输出、row_cases、条件/NULL/上下文、嵌套配置均可找到；局部展开与完整 JSON 覆盖核查后契约，不丢已知限制 |
| A5 | 补充说明读写、取消、冲突保留原稿、显式换依据及服务重启后内容正确；源码/补充说明分别保存，其他对象与算法配置不受影响 |
| A6 | 历史按需分页、精确解析且只读；回当前后才允许编辑/检查；Python 源码维护与 C++ 插件更新遵守当前生命周期约束，新组件不自动套用旧说明或旧结果 |
| A7 | 既有三类 F2 输入选择及高级 locator 可用，多输入顺序/数量正确；字段/标量/枚举和复杂参数往返不丢值，省略、空串、布尔/数字字符串及 JSON 编码不混淆；非法 JSON 保留原文且不提交 |
| A8 | compatible/incompatible/undetermined 与 HTTP 失败分开；本次输出及条件可读，未启用与未确定可区分，forecast 行 NULL/必要上下文不丢；缺目标、pending 来源和语义缺失不误判，部分预览不能冒充完整确认 |
| A9 | 参数/输入/目标/组件变化标记需重检；旧对象或旧草稿的异步响应不覆盖当前结果；真实来源/组件冲突正确处理，表单与普通客户端提交相同字符串请求并取得一致结果 |
| A10 | 1280×800、1440×900、1920×1080 三视口、深浅主题和缩放下真实点击/键盘操作可达，无页面横向撑宽或遮挡；原管理动作可用，阅读和检查不加载业务行、不消费、不执行算法或新增注册表版本 |
| A11 | 请求/响应符合本次冻结的 Schema/OpenAPI，HTTP/IID 一致；算子默认 usage、显式 full 和精确局部读取正确，非法视图拒绝，missing 与不可读取区分；版本切换、超时、补充说明变更后读取依据正确 |
| A12 | 普通客户端可读六项并按精确引用展开必要规则，再构造调用检查请求；usage 响应及页面默认读取不携带完整契约/全部资源，省略空项不放宽约束，跨参数条件/JSON 编码/未知时间单位不丢；复杂详情闭合可读，无需模型推理即可验证 |
| A13 | 核查清单覆盖实施起点仓库交付算子的声明情况；已有声明的身份/输入、参数/默认行为/解码、输出/行语义/NULL/条件逐项对应实现与测试；错误声明修正且真实解析或最小执行断言通过，无声明与信息缺口明确，核查结论不由 Schema/预检成功替代 |
| A14 | 完整契约与 usage 的具体 Schema 和简单/Baseliner 响应样例可校验；参数与字段关联、控件/字符串/JSON 编码、必填/未设置/空串/默认值、组级约束及详情引用有确定结构与可执行断言，直接调用方清单和新契约映射明确 |

- 各 Task 先把核心行为转为契约/生产解析断言、接口或浏览器用例，再实现；复杂性以卡方及 Baseliner 为锚点，不建设通用 Schema 引擎。
- 受影响核心回归包括实际改动声明的注册/生产解析/最小输出断言、算子契约浏览器、元数据算子浏览器、预检 API、补充说明 HTTP 与普通客户端；读取覆盖描述服务/IID、API Schema/OpenAPI、显式 full 与按需资源读取。直接调用方和测试按新契约更新，通道页仅按共享调用影响补充回归，不设置旧协议兼容验收。
- 精简效果记录相同契约的完整/usage 响应字节数和默认请求清单，重点断言去除重复内容且必要信息可取；不设任意压缩率，不将字节减少当作模型正确率提升，模型实测属于后续接入范围。
- 复用 native 临时 SQLite、DataFrame/CSV、保存的结构化 Kafka 配置、真实 Python/C++ 测试组件、回环 HTTP 和 Chromium；Kafka 仅验元数据，不要求外部 broker 或数据面消费。
- 实施前在工作台冻结实际 CMake targets、标准测试入口 filter、前端单测/生产构建、测试依赖与资源锁；如改依赖声明或夹具同切片维护，不默认运行全库、外部四库认证、镜像或模型推理。
- 来源只读计数、组件更新和保存后重读须有行为证据；契约文字/Schema 夹具不替代浏览器真实操作，预检结果不替代算法正确性验收。
- 完成要求 selected=executed=passed，failed=skipped=not_executed=0，相关格式与文档检查通过；记录主产物、隔离宿主行为及常驻服务是否重载，证据放 `evidence/operator-description-refinement/`，不提交本地证据。

## 完成证据

2026-10-31 规格阶段：已按用户决定纳入既有契约核查、声明修正和读取契约冻结交付，取消向后兼容要求并调整任务顺序；依赖 Feature 的历史通过结果不作为本 Feature 完成证据。

2026-10-31 T1 首个切片完成，T1 进行中且检查点通过，T1～T5 均保持未勾选：

- 已冻结 15 个精简/完整读取 Schema 定义，新增 passthrough、卡方和 Baseliner 三组声明样例及参数关联、条件配置、编码、缺失状态和精确引用的正负断言；尚未切换 describe 路由行为，样例不代表实际 HTTP 观察。
- Config 主库与选定测试目标构建成功；最终标准入口精确 filter 执行 4 个功能测试及 3 个自动环境入口，selected=executed=passed=7，failed=skipped=not_executed=0（`20261031T071715Z-f5fc2d70`）。原生 RapidJSON 探针通过三组正例和四类非法输入拒绝检查；9 项静态/范围检查通过。
- 核查清单和直接调用方映射已建立；发现 passthrough 的 force_fail/delay_ms 未在参数声明中列出，完整生产核查、声明修正和真实解析/最小执行断言仍属 T1 后续切片，不以 Schema 样例通过替代 A13。
- 证据、清单和源码/主产物摘要位于 `evidence/operator-description-refinement/t1-schema/acceptance.json` 及同目录缓存（不入库）。主产物已重建，新增 usage HTTP/页面行为未验证，常驻服务未重载；T2～T5 未开始，模型调用未执行。

2026-10-31 T1 已完成：A13/A14 验收通过，核查结论与读取契约冻结如下：

核查范围为实施起点的 9 个产品算子身份、1 个仓库提供的 Python 样例，以及单独标识的卡方测试组件；NPM 两个执行 ABI 导出属于同一个 `npm.basic` 身份。无声明项只确认缺口，不推断其输入、默认值或输出语义。

| 算子或组件 | 结论与关键事实 | 源码依据与可执行断言 |
| --- | --- | --- |
| `builtin.passthrough`（产品） | 已修正并验证：补齐 `force_fail`、`delay_ms` 字符串参数及默认行为；只复制首个输入，保留类型和 NULL，其余输入不参与输出；上限截断依据源码，不声称完成 60 秒计时试验 | `services/builtin/builtin_registry{,_plugin}.cpp`、`framework/builtin/dataframe/passthrough_operator.{h,cpp}`；`test_operator_contract_registration`、`test_operator_contract_cpp`、`test_operator_contract_output` |
| `explore.baseliner`（产品） | 已核实并补全说明：配置引用/一次 JSON 文本互斥、默认配置、集合输入及类型适配；四种输出 Schema 逐字段对应；forecast 的九个观测/判定字段为 NULL，副输出依实际目标/上下文启用，缺业务单位和可空性不补猜 | `operators/baseliner/{baseliner_plugin_exports,baseliner_contract,baseliner_effective_config,baseliner_data_contract,dataframe_reader,snapshot_reader,result_codec,baseliner_operator}.cpp`；`test_baseliner_contract_schema`、`test_baseliner_contract_invocation`、`test_baseliner_contract`、`test_baseliner_dataframe`、`test_baseliner_operator` |
| `builtin.dataframe_dispatch_stream`（产品） | 信息缺口：未发布结构化作者契约，身份之外的三维声明不标记已核实 | `services/builtin/builtin_registry.cpp` 与 `builtin_registry_plugin.cpp` 的注册/发布清单 |
| `builtin.concat`（产品） | 信息缺口：未发布结构化作者契约 | 同上 |
| `builtin.hstack`（产品） | 信息缺口：未发布结构化作者契约 | 同上 |
| `builtin.passthrough_stream`（产品） | 信息缺口：未发布结构化作者契约 | 同上 |
| `builtin.count_window_stream`（产品） | 信息缺口：未发布结构化作者契约 | 同上 |
| `builtin.tcp_service_merge_stream`（产品） | 信息缺口：未发布结构化作者契约 | 同上 |
| `npm.basic`（产品） | 信息缺口：执行能力 IID 不等于作者数据契约；完整 NPM 语义包仍归独立 Feature | `operators/npm_basic/npm_basic_plugin_exports.cpp`，无数据契约导出 |
| `explore.link_timestamp`（提供的 Python 样例） | 信息缺口：源码未发布结构化作者契约；样例不等于自动注册的生产算子 | `samples/python_operator/explore_link_timestamp.py` |
| `explore.chisquare`（仅测试组件） | 已修正并验证：补齐汇总 `col1/col2`，在组件内适配 Worker 的 Polars 输入；显式/默认字段、独立 2×2 样本、混合行 NULL、动态分类列与 error 行均有实际 Arrow 路径断言；其他统计异常和字段名冲突不作保证 | `tests/test_config_channel/fixtures/description_chisquare.py`、`python/flowsql/{operator_base,operator_registry,worker,arrow_codec}.py`；`test_chisquare_contract_audit`、`test_operator_contract_python`、`test_metadata_description_api` |

表中 `services/`、`operators/`、`framework/`、`tests/`、`python/` 均相对 `src/`；全部条目的身份/输入、参数/解码、输出/限制及源码摘要见本地 `evidence/operator-description-refinement/t1-audit/contract-audit.json`，不入库。8 个无契约项包括 7 个产品算子和 1 个 Python 样例；卡方的测试结果不计为产品覆盖。

读取数据契约冻结于 `config/semantics/api.schema.json` 的 15 个新增定义；三组 usage 样例位于 `config/semantics/examples/operator-usage.json`。完整作者契约沿用现有 Schema，本次只纠正有证据的声明；样例是约定形状，不能当作已观察到的 usage HTTP 响应。

| 映射或边界 | 已冻结的规则与断言 |
| --- | --- |
| 请求与读取 | 算子 `operatorDescribeRequest` 接受默认 usage、显式 usage/full 和有界超时；`operatorUsageResponse` 提供六项及依据/缺口/详情，`operatorFullDescriptionResponse` 独立校验完整内容；实际路由切换留在 T2 |
| 参数与字段 | `name/source_pointer/schema/required/input_roles` 保留声明；只有明确 parameter selector 才用 field 控件；`types=[]`、`nullable=null` 保持未声明，不猜类型或非空 |
| 字符串及默认值 | 实际请求的参数值全部是字符串；省略与空串分别保留，布尔/数字/null 值拒绝；默认值仅提示，不写入请求。passthrough 的布尔开关和数字延迟仍使用 text，以保留生产解析器接受的字符串语义 |
| 复杂条件及 JSON | Baseliner `parameters` 使用 json 控件与一次 JSON 文本编码，保留 decoded_schema_pointer；`config/parameters` 的 required 为 null，组级 oneOf 在 constraints 保留，不扁平化为可选；无法可靠映射的规则保留 advanced/局部详情 |
| 输出及局部读取 | 同步原声明顺序、字段、availability、row_cases 与 context_semantics；details 为 section/exact_reference/resource_pointer/reason，只引用 basis.contract_ref 的同一精确版本，不用 latest 或网络引用；不附带完整 resources 或归一配置 Schema |

`test_operator_contract_schema` 对全部声明参数、输入角色、四种输出及三组样例执行正负断言；原生 RapidJSON 探针对相同 usage/完整样例验证合法响应，并拒绝非法对象类型、空契约依据、非法 JSON Pointer 和不完整全文。声明修正的真实性另由上表生产解析与最小执行用例验证。

| 后续直接调用方 | 新契约映射 |
| --- | --- |
| `src/channels/config/metadata_description.cpp`、`config/semantics/openapi.json` | T2 将算子请求绑定 operatorDescribeRequest，默认生成 usage，显式 full 走全文；通道契约不变，无旧视图适配 |
| `src/frontend/src/components/OperatorContractPanel.vue` | T3/T4 读取六项，按同版本局部详情或显式 full 展开，维护同一份字符串调用草稿 |
| `examples/metadata_description/client.py` | T2 按使用目的消费 usage 或显式 full，不依赖旧默认全文 |
| `src/tests/test_config_channel/test_metadata_description.cpp`、`test_metadata_description_api.py`、`test_metadata_description_client.py` | T2 更新 IID/HTTP/普通客户端的读取断言，通道请求隔离及原有补充说明行为继续回归 |
| `src/tests/test_config_channel/metadata_operator_browser.mjs`、`metadata_workflow_browser.mjs` | T3～T5 更新算子读取断言；完整配置比较显式 full，默认 usage 单独验证 |
| `docs/data-semantics.md`、`docs/sql-workflow.md` | T5 同步默认 usage、显式 full、核查边界及同版本详情指引 |

T1 最终验收（2026-10-31）：

- 按工作台冻结清单构建 Config/Builtin/Baseliner 主产物和对应测试目标；15 项功能/契约回归与 11 项自动环境入口，共 selected=executed=passed=26，failed=skipped=not_executed=0，run `20261031T080007Z-869aa5a8`。统一执行器的运行前后产物指纹核对通过，未运行全库测试。
- 三组 usage/full 原生正例和四类负例通过；修改的 C++ clang-format 检查、Python 编译、JSON/引用/清单/允许范围和 diff 检查通过。规格在完成证据之前为 168 个非空行，维持 5 个一级 Task，无新增层级。
- 验收记录及源码/产物摘要：本地 `evidence/operator-description-refinement/t1-audit/acceptance.json`、`static-checks.json`、`contract-audit.json`；均不入库。Config/Builtin/Baseliner 主产物已更新并经隔离测试宿主验证；常驻服务未重载，usage HTTP 和新页面仍未实现/验证，T2～T5 未开始。
- 工作台已恢复 WIP=0；T1 单独勾选，Feature 保持进行中。既有及过程中出现的其他工作区修改保留原样，未提交、未推送。


2026-10-31 T2 已完成：A3/A4 数据部分与 A11/A12 接口部分验收通过。

- `IMetadataDescriptionV1` 与既有 HTTP describe 共用确定性提取：算子默认 usage、显式 usage 等价、显式 full 返回完整说明；通道保持原响应并拒绝算子 view。Schema/OpenAPI 和普通客户端已同步，不新增 IID/路由/持久模型。
- 六项保留作者/本地来源、字段 selector、字符串默认提示、互斥/组约束、一次 JSON 编码、未知类型/可空性、多输出 availability/row_cases/NULL/context；复杂输入集合、参数影响和输出规则按同一 contract_ref 的局部 pointer 读取。用途未声明与缺契约显式给出缺口；已声明但不可读取仍报错。
- 真实 Python 发布（简单组件及卡方）、内置、C++ 和 Baseliner 的 usage/full 响应均通过 Schema；普通客户端逐项展开详情及其本地引用，构造字符串参数检查。读取不新增注册表版本，lazy 输入保持 pending；本地说明保存后重读、新组件隔离旧说明、版本变化、不可读取与超时均有断言。
- 标准清单：7 项功能/契约回归 + 6 项环境入口，selected=executed=passed=13，failed=skipped=not_executed=0，run `20261031T083518Z-d4cc0f9e`；统一执行器源码/产物前后指纹一致。最初沙箱 socket EPERM 后按授权在沙箱外运行回环验收；中间构建重叠及测试夹具/排序问题已修正，以上为稳定产物最终结果。
- 实际 HTTP 原始响应字节（usage/full）：passthrough 2394/2384、C++ fixture 2649/5352、简单 Python 2366/2349、卡方 5103/4357、Baseliner 25932/33745。精简读取移除了全文/全部资源；部分小契约因控件、来源和 pointer 信息变大，未宣称统一压缩率或模型效果。
- Config 主产物及冻结目标构建成功，迟后修改的 native 测试单独重建成功；C++ clang-format、Python 编译、Schema 与 diff/允许范围检查通过。证据与源码/产物摘要位于本地 `evidence/operator-description-refinement/t2-reading/acceptance.json`、`response-sizes.json` 和日志，不入库。
- Config/Web 主产物由隔离宿主实际加载并完成 HTTP 验证，常驻服务未重载；浏览器/页面、含浏览器的完整 client suite、外部服务/镜像/模型未验收。T3～T5 未开始，Feature 保持进行中；工作台 WIP=0，按当前切片完成即停，未提交/推送。


2026-10-31 T3 已完成：A1/A2、A3/A4 页面部分及 A5/A6 通过完整验收。

- Python 详情按基本信息、源码、默认收起的“说明与契约”、保存/关闭排列；源码保存依据折叠。说明展开/收起、最大化恢复、刷新、跨对象和关闭重开保留源码、说明及调用草稿。C++ 插件基本信息在列表之前，具体算子分别读取；真实三算子插件已验证切换身份。内置入口没有源码或插件维护操作。
- 共用面板默认六项使用说明；参数合并明确的字段要求，保留未知类型/可空性、组级条件、默认值提示和一次 JSON 编码。主输出按声明顺序默认展开，其他输出按需读取；Baseliner 的条件、行类型、forecast NULL、必要上下文及嵌套资源可达，卡方混合行与动态列可读。示例默认折叠，可实际复制；技术详情默认折叠，最近读取及完整 JSON 可复制且不含未保存草稿。
- 普通阅读的实际请求为一次 Python 对象详情、一次默认 usage describe 和一次面板对象详情；未预取来源、历史、full 或局部资源。进入调用检查后才读来源；注入目录失败不阻断说明，页面业务行/执行请求计数均为 0。调用检查保留既有 JSON 草稿，T4 新表单及结果整理未实施。
- 补充说明与源码分别保存；取消、版本冲突保留原稿、显式换依据、保存期间继续输入及重启后读取均有断言。源码换依据期间禁用保存，旧保存响应不关闭含后续草稿的详情；当前对象切换后的旧读取不覆盖新对象。新组件不自动套用旧说明或检查结果，激活、去激活、删除和插件替换流程通过。
- 历史复用已有作者契约接口，按需每页 10 条；12 个真实作者版本验证两页读取、精确解析及只读，返回当前版本后才允许说明编辑与检查。补充说明继续使用已有精确配置引用，不增加历史接口类型。
- 最终标准清单 run `20261031T091009Z-7b84fac1`：6 项功能/回归与 6 项环境入口，selected=executed=passed=12，failed=skipped=not_executed=0；前端既有单测 49/49，失败/跳过/取消均为 0。标准 CMake host/fixture 与主静态资源构建成功，JS/Python 语法、diff/允许范围检查通过；运行前后源码、主产物及额外记录的页面产物指纹一致。中间发现的布局、历史类型、保存时序及旧测试夹具假设均已修正，以本次稳定清单为准。
- 本地证据 `evidence/operator-description-refinement/t3-reading/acceptance.json`、`static-checks.json`、`operator-browser/result.json` 与日志/截图均不入库。主启动静态资源已更新，并由 native 隔离宿主和实际 Chromium 验证；常驻服务未重载，外部数据库/Kafka、镜像、模型调用未验收。无提交/推送，原有任务外修改保持基线。
- 仅勾选 T3，工作台恢复 WIP=0；T4/T5 保持未实施，Feature 继续进行中。


### T4：同一调用草稿、相关结果与检查依据（2026-10-31）

- A7：输入支持真实 DataFrame、SQLite 具体表及保存的结构化 Kafka 来源，过滤真实 sink-only/packet 通道；按需读取目录/只读结构，lazy 来源保持 pending。表单与高级 locator 共用有序草稿，增删/排序后 input_index 连续；字段控件仅采用唯一的已声明输入映射，结构或关系不明确时使用文本。输出目标支持 DataFrame 与明确数据库表，默认折叠并可省略。
- 参数设置/省略/空串严格区分；声明默认值只提示。字段、枚举与普通字符串编辑保留未知合法键及数字/布尔/null 字符串；复杂参数按组编辑 JSON 文本，内层仅校验不重编码；非法外层/内层 JSON 与非法 locator 原文保留，禁止提交。新增 6 项工具单测覆盖编码、顺序、选择与边界，前端合计 55/55。
- A8：按钮为“检查输入与参数”；三态分别显示“满足已声明要求 / 不符合要求 / 信息不足”，请求失败单独报错。字段映射与本次输出使用表格；适用/未启用/未确定分别呈现，未返回的部分预览有显式缺口提示；完整依据、实际参数和来源快照折叠。Baseliner 四输出、evaluation/forecast 与 forecast 九项必须 NULL、必要上下文及来源语义均有真实浏览器断言；缺目标与未绑定语义不冒充确认，技术状态保留 not_run/not_observed。
- A9：输入、参数、目标、显式来源刷新及组件依据变化使结果需重检；旧草稿/旧对象的延迟响应不会覆盖当前。真实 Kafka 配置变化使旧绑定返回 SOURCE_CHANGED；真实 Python 组件重新发布后检查响应与阅读依据不符，显示 COMPONENT_CHANGED 并保留草稿。浏览器提交的字符串请求与普通 HTTP 客户端取得的完整结果一致；页面全过程无业务行 preview 或算法执行请求。
- 夹具：复用既有 channel_dictionary_host 的保存 Kafka 结构能力及独立 Gateway，只将 Kafka 前缀连接到算子 Gateway；修正 operator_contract_host 的 Config 插件使用本次随机 Gateway 地址，避免默认端口与路由心跳覆盖。仅修改测试宿主/依赖，无产品后端新接口、IID、持久模型或数据面改动。
- 最终标准运行 `20261031T122231Z-7941961e`：6 项功能/回归 + 6 项环境入口，selected=executed=passed=12，failed=skipped=not_executed=0；CTest 219.39 秒。选择清单与依据保持工作台冻结范围，不扩展外部服务或镜像矩阵。
- 标准 CMake 配置及 test_operator_contract_host、主 output/static 构建通过；clang-format、JS/Python 语法与 git diff --check 通过。前后源码 1127、主产物 234、静态资源 19 项指纹一致；本轮范围与原有修改基线核对通过。
- 本地未入库证据：`evidence/operator-description-refinement/t4-check/acceptance.json`、`static-checks.json`、`scope-check.json`、`tests-final.log`、`frontend-tests-final.log`、`contract-browser/` 和 `operator-browser/`；标准运行完整清单另见 `evidence/test-environment/20261031T122231Z-7941961e/summary.json`。
- T4 完整完成并勾选，工作台 WIP=0；T5 保持未实施。真实验证限 native 隔离宿主、临时 SQLite/DataFrame 与保存的 Kafka 元数据；无外部 broker/数据库矩阵、镜像或常驻服务重载，无提交/推送。


2026-10-31 T5 与 Feature 已完成：A1～A14 的当前源码、主产物及真实页面回归全部通过。

- 使用说明：更新 `docs/data-semantics.md`、`docs/sql-workflow.md` 与 README 入口，明确六项阅读、三类详情顺序、补充说明/历史/作者维护、来源选择及字符串参数、默认值只提示、三态与 NULL/必要上下文；普通调用方使用默认 usage、显式 full 和同精确版本 resource，未知与 missing/不可读取保持区分。核查范围为 9 个产品身份、1 个 Python 样例及独立标识的卡方测试组件，未将测试组件记为生产覆盖。
- 新增 A10 行为断言：1280×800、1440×900、1920×1080，Python/内置/C++ 三类详情共 18 个深浅主题场景，深色场景使用 110% CSS 缩放；六次真实 Tab、文本输入和 Enter 提交检查，鼠标点击目标可见且未被遮挡，无页面横向溢出。阅读/检查请求记录 rows=0、consume/execute=0，注册表版本不变；原管理动作、源码/插件更新、说明保存及重启继续通过。
- 本轮直接执行用户文档中的标准库 Python 片段：真实上传并激活 `demo.identity`，默认读六项、同版本局部输出规则及调用检查 compatible；另核对显式 full 与相同引用的 resource 一致。该片段不执行算法，完整客户端/SQL 执行及结果 use/dictionary 闭合由标准回归单独验证。
- 构建：`cmake -B build src` 及工作台冻结的 Config/Builtin/Baseliner/宿主/语义注册等目标全部通过；主页面目录为 `build/output/static`。前端 55/55，failed/skipped/cancelled=0；11 组静态检查通过，包括新增链接、代码块 JSON/Python、forecast 九项 NULL 与声明一致、C++ clang-format/版权、JS/Python 语法和 Diff 空白。
- 最终标准运行 `20261031T125018Z-fffc8be6`：22 项功能/契约/受影响流程与 17 项自动环境入口，selected=executed=passed=39，failed=skipped=not_executed=0，CTest 403.96 秒。包括 T1 的真实生产解析/注册/最小输出、T2 的 IID/HTTP/Schema、T3/T4 浏览器、普通客户端及跨页 SQL、共享通道说明和原语义 HTTP；不默认扩展全仓。
- A1/A2/A5/A6 由两项算子浏览器和说明服务/HTTP覆盖；A3/A4 由 Schema、IID/HTTP、卡方实际 Arrow 与 Baseliner 复杂说明覆盖；A7/A8/A9 由表单/结果浏览器、调用检查/推导/API及前端工具覆盖；A10 由新增矩阵和既有管理/跨页操作覆盖；A11/A12 由 Schema/OpenAPI、IID、实际 HTTP、普通客户端与文档片段覆盖；A13/A14 由注册、生产解析、最小执行及三组完整/usage 样例覆盖。完整对应表在本地 `t5-delivery/acceptance-map.json`，每项关联本轮实际通过的用例。
- 本轮响应量观察（含信封字节）：passthrough usage/full 为 2394/2384，卡方 5103/4357，Baseliner 25932/33745；默认 usage 不携带全文或全部资源，必要规则可同版本展开。简单契约可能因角色映射/引用而略大，不以任意压缩率收口，不声称模型效果已验证。默认页面只读取对象详情及 usage，无历史/resolve/full/来源目录预取。
- 指纹：1127 项源码、234 项主产物、19 项静态资源验收前后一致；T1 核查所用 22 份源码/声明摘要当前复核无变化。只保留 T5 允许文件内增量，既有 T1～T4 和用户修改保留；本地证据位于 `evidence/operator-description-refinement/t5-delivery/acceptance.json`、`acceptance-map.json`、`scope-check.json`、`static-checks.json`、`doc-client.json`、`fingerprints-{before,after}.json`、浏览器截图及日志，不入库。
- 实际边界：native 临时 SQLite/DataFrame/CSV、保存的 Kafka 结构配置、Python/C++ 测试组件、真实回环 Web/Gateway/Router/Config/Worker 和 Chromium；主产物已构建并由隔离宿主验证，常驻服务未重载。外部数据库/Kafka broker/认证矩阵、镜像、模型及完整算法精度/性能未纳入本次结论。
- 已勾选 T5，T1～T5 全部完成；规格文件归档至 `tasks/archive/`，Backlog 标为完成，工作台恢复 WIP=0；无提交、无推送，无其他 Feature 启动。

## 用户验收修正 R1：三项说明直接编辑

用户确认契约围绕输入、参数、输出及成立条件，要求配置页简洁直接。本修正替代上文关于六个页面区域、编辑开关、更多菜单和默认无文本框的布局约定；原作者执行契约、三态检查及原任务完成证据保留。

- `[x]` 名称/用途之后仅分输入、参数、输出，条件在所属内容中查看或编辑；空内容为空编辑框，已删除重复缺失提示、作者空版本和编辑补充说明开关。顶部保存/刷新，通用条件/示例与技术详情次要折叠。
- `[x]` 输入来源只出现在调用检查首部；历史置于技术详情，源码与插件维护继续使用原管理入口。历史只读、CAS 冲突、期间输入和分对象会话草稿保护均保留。
- `[x]` 既有 notes 保存新增可选 input_requirements/parameter_usage/output_meaning 字符串，各最多30000字符；usage.annotations 总是返回这三项，未提供为空。可编辑文字明确是本地说明，不覆盖作者结构化事实，也不参与符合性判断；无契约时补充文字后机器状态仍为 missing/undetermined。
- 独立验收：20261031T132747Z-36791681，7项功能/直接调用方回归及7项环境入口，selected=executed=passed=14，failed=skipped=not_executed=0；前端55/55；主构建与 build/output/static 通过。
- 三视口×三类算子×深浅主题共18场景，深色110%缩放，六次真实键盘调用检查；无契约Python/C++真实键盘直编、保存/重开、HTTP与宿主/Worker重启恢复通过；未读取业务行、消费或执行。
- 改动C++格式、JS/Python语法、JSON/文档链接与Diff核对通过；源码1136及相关主产物/静态资源65项在最终验收前后指纹相同，标准runner源/产物指纹也一致。证据 evidence/operator-description-refinement/page-simplification/acceptance.json 与同目录截图/日志，均不入库。
- 首轮旧驱动和旧字段数/状态名称断言失败已记录并修正，最终未跳过或放宽必需行为。常驻服务未重载，未提交、未推送。


## 用户验收修正 R2：移除独立调用检查

用户确认契约配置页仅需简洁说明输入、参数、输出及成立条件，同意移除独立调用检查。本修正替代上文与 R1 中关于调用检查页签、调用表单、结果与调用草稿的页面约定；原 T1～T5、R1 的完成证据保留，底层契约、预检接口与执行时校验不变。

- 页面直接进入说明正文，移除使用说明/调用检查切换、输入来源选择、本次参数、输出目标、检查按钮和结果区。
- 清理该页专属前端组件、工具和调用草稿状态；说明草稿、CAS 保存与冲突保护、历史只读、复杂规则和完整 JSON 按需读取保留。
- 浏览器验收锚点改为三类算子无调用界面且不请求来源目录、结构或预检；保留说明保存/重开/冲突/期间输入及源码管理回归，后端预检由原 API 回归覆盖。
- `[x]` R2 已完成。最终标准运行 20261031T135116Z-52d3a5d4：4 项功能/直接回归及 4 项环境入口，selected=executed=passed=8，failed=skipped=not_executed=0，CTest 213.05 秒；前端 49/49，相关 CMake 构建通过。
- 三视口×三类算子×深浅主题共18场景、深色110%缩放及六次真实键盘说明编辑通过；无契约空框、说明保存/重开/宿主与Worker重启持久化、历史只读、冲突/期间输入、源码草稿及组件隔离均通过。契约UI来源/结构/预检、业务行读取及执行请求为0。
- 初轮8项中7通过、1因驱动把源码中的标题当作正文已加载而失败；已改为等待usage正文，保留首轮证据后重跑完整清单，未跳过或放宽行为断言。
- 验收前后1132源码及65相关主产物/静态资源指纹一致，标准runner的源码/产物指纹也一致；本轮14文件增量均在允许列表，既有用户修改保留。README、SQL指引与用户说明已同步。
- 本机Web实际提供的新index/算子资源与主构建一致，刷新页面即可加载；常驻服务未重启，未提交/推送。WIP=0，独立证据 `evidence/operator-description-refinement/remove-check-ui/acceptance.json`、同目录截图/日志及标准summary均为本地缓存，不入库。

## 用户验收修正 R3：技术详情仅展示一份完整 JSON

用户确认技术详情只需一份完整 JSON 与一个复制按钮。本修正收敛 R2 保留的技术详情展示，底层契约和既有接口不变，原完成证据保留。

- `[x]` 删除 usage 摘要与单独 configuration 片段，技术详情只展示当前 full 响应或选定历史版本的 resolve 精确记录，并用同一个“复制 JSON”按钮复制；历史分页入口保留。
- 浏览器通过真实 HTTP 对比当前/历史显示与剪贴板内容；未保存用途/输入说明不混入，返回当前后重新读取 full；读取等待和版本不一致时不出现摘要 JSON 或复制按钮，错误后可重试。
- CMake配置与主宿主/前端构建通过，build/output/static已更新；前端49/49，JS/Python语法、文档文件链接22处及diff空白检查通过。当前与历史技术详情截图已审查，均仅一份JSON和一个复制按钮。
- 最终标准运行20261031T140827Z-50bb0c54：2浏览器及2环境入口，selected=executed=passed=4，failed=skipped=not_executed=0，CTest158.10秒。原保存/重开/重启持久化、冲突/期间输入、源码管理/草稿、组件隔离及18布局场景全部通过。
- 验收前后1132源码及65相关主产物/静态资源指纹一致，标准runner源码/产物指纹也一致；本轮8文件增量均在允许清单，已有修改保留。独立证据位于`evidence/operator-description-refinement/single-technical-json/acceptance.json`及同目录截图/日志，标准summary位于`evidence/test-environment/20261031T140827Z-50bb0c54/summary.json`，均不入库。
- 本机8081实际提供的index、入口和Operators-CP45TSwA.js与新主构建一致，刷新页面即可加载；常驻服务未重启，未提交/推送。工作台WIP=0，停止。

## 用户验收修正 R4：输入、参数、输出结构化表格编辑

用户确认三个区域改为表格逐条填写，“通用条件”改为“使用限制”，并明确不考虑兼容性。本修正替代R1～R3中的三段文本说明及“通用条件与示例”合并折叠约定；原任务证据保留。

- `[x]` 输入、参数、输出均可添加行、行内编辑和删除；输入/输出条目通过port归属不同数据集，类型、必需、可空、默认值、含义和条件按条目记录。未知值保持空白，不推断默认值；使用限制与调用示例分别折叠，技术详情继续只显示一份JSON。
- `[x]` notes必需包含purpose、usage_notes、examples及inputs/parameters/outputs三个结构化数组，usage.annotations原样返回数组，无本地说明时为空数组；每组最多100条。旧文本字段不再接受，旧会话草稿不再读取，无兼容或迁移。作者执行契约与本地说明保持独立，保存说明不会将missing执行契约变为已声明。
- 输入行包含port/name/type/required/nullable/description/constraints；参数行包含name/type/encoding/required/default/description/constraints；输出行包含port/name/type/nullable/description/conditions。属性均必需，名称/端口/类型最多256字符，说明/约束/条件/default最多30000字符；布尔和default使用null表示未知，default空字符串与null分开。完整接口说明见docs/data-semantics.md。
- 草稿与保存快照深拷贝，保存中继续编辑不污染在途内容或已保存JSON。真实HTTP验证行顺序/多端口/未知/显式空值原样读回、幂等和冲突、拒绝错误结构/超限与旧字段；浏览器验证增删后保存、重开/重启恢复、作者规则、组件隔离与历史只读。
- 构建test_metadata_description、test_operator_contract_host、flowsql_runtime_config通过，主Config/Web库、前端与公共契约资产均同步；前端49/49，修改C++格式、JS/Python语法、文档链接/JSON及diff检查通过。
- 首轮12项中11通过，普通客户端旧浏览器断言等待R3已移除的configuration片段失败，已改为完整响应对比；截图发现删除列换行后修正并新增布局断言。最终标准运行20261101T023553Z-944cb2a2：6功能/直接回归及6环境入口，selected=executed=passed=12，failed=skipped=not_executed=0，CTest257.05秒，未删减或放宽必需清单。
- 三视口×三类算子×深浅主题18布局场景、深色110%缩放、真实鼠标/键盘条目编辑和三视口跨页SQL结果全部通过；最终表格截图已审查，删除按钮单行且不越出单元格。验收前后1133源码、73相关主产物/静态资源/公共契约资产指纹一致，标准runner源码/主产物指纹也一致；本轮19文件增量均在允许清单，既有修改保留。
- 实际验证为native隔离宿主/Worker/Chromium；本机8081静态资源已更新，常驻后端尚未重载新协议，需要重启加载新库后使用新保存格式。本轮未重启现有服务、未提交/推送；工作台WIP=0。独立证据evidence/operator-description-refinement/structured-tables/acceptance.json及同目录日志/截图、标准summary均为本地缓存，不入库。


## 用户验收修正 R5：精简字段并使表头与配置项一致

用户确认输入5列、参数6列、输出5列（均含操作列），多输入/输出按名称分组，并明确不考虑兼容性。本修正替代R4的平铺行、组合表头、独立编码和默认值勾选框约定；历史完成证据保留。

- `[x]` 每个表头对应一个配置项，每个数据单元格只有一个对应控件；输入为字段名/类型/填写要求/说明/操作，参数为参数名/类型/必填/默认值/说明/操作，输出为字段名/类型/允许空值/说明/操作。删除单元格内重复标签，说明列保留较宽空间，删除按钮不换行。
- `[x]` 输入填写要求合并为必填非空、必填可空、可选，未填写保持空白；可选不推断NULL许可。参数类型包含字符串、整数、数值、布尔和JSON，不再单独填写传值方式；默认值直接编辑，空白未声明，双引号空串文本表示明确空字符串。输出保留允许空值，字段规则/约束并入字段说明，整体规则和产生条件放在组说明。
- `[x]` 单输入/输出不逐行填写端口，多组使用输入名称/结果名称。inputs/outputs为{name,description,fields}数组，parameters为{name,type,required,default,description}数组；六项notes属性均必需。Schema/OpenAPI/样例、usage/full原样往返、文档和新草稿key同步；旧行、encoding/constraints/conditions等旧字段和旧草稿不兼容或迁移。作者执行契约、算法和调用行为保持独立。
- CMake配置及test_metadata_description、test_operator_contract_host、flowsql_runtime_config构建通过，主前端/Config/Web库和公共契约资产已同步；前端49/49，修改脚本语法、42处文档文件链接、29个JSON代码块及diff检查通过。
- 首轮20261101T082015Z-0de39a08：selected=executed=12、passed=11、failed=1，skipped=not_executed=0。唯一失败为另一个浏览器脚本遗漏同步“输入 1 含义”标签；按失败依据将直接脚本纳入允许清单并修正3处标签，保留所有断言。首轮截图/日志独立复制到first-run/，未删减验收范围。
- 最终完整运行20261101T082621Z-50b3dd05：6功能/回归及6环境入口，selected=executed=passed=12，failed=skipped=not_executed=0，CTest256.38秒。18布局场景、深色110%缩放、真实键盘编辑、分组增删/重开、默认值文本与JSON类型、保存中编辑、冲突/历史隔离以及宿主/Worker重启恢复全部通过；新表格和分组截图已审查。
- 验收前后1133源码、73相关主产物/静态资源/公共契约资产指纹完全一致，标准runner的源码与产物前后快照也一致；本轮16文件增量均在允许清单，既有其他修改保留。证据位于evidence/operator-description-refinement/compact-tables/acceptance.json及同目录日志/截图，标准summary位于evidence/test-environment/20261101T082621Z-50b3dd05/summary.json，均为本地缓存、不入库。
- 本机8081的index和入口资源与新主构建一致；常驻进程仍映射已删除的旧Config库inode，新保存结构需要后端重启加载新库。本轮未重启常驻服务、未清理既有数据、未提交/推送；工作台WIP=0，停止。

## 用户验收修正 R6：下拉框深浅主题可读性

- `[x]` 表格控件和按钮改用有效主题背景--bg-card，option明确使用主题背景/文字；组件内声明随主题切换的color-scheme，修复深色下原生菜单文字与背景不匹配。保存结构、作者执行契约及分组交互保持当前行为。
- 修改前真实浏览器捕获透明背景；首次修复的回归捕获深色select仍为light，核对Vue编译产物后修正组件选择器，避免产生全站.dark规则。完整回归中一次进程返回143且未产出summary，独立保存中断证据后按原清单重新执行，未增加超时、删减用例或放宽断言。
- CMake配置和test_operator_contract_host/flowsql_runtime_config构建通过，主静态资源更新；前端49/49，JS/Python语法和diff检查通过。最终标准运行20261101T101702Z-e69a6387：selected=executed=passed=2，failed=skipped=not_executed=0，CTest177.93秒。
- 18布局场景、6组主题控件检查、四类下拉深浅主题8次真实鼠标展开及键盘选择通过，展开截图已审查；控件/选项最低对比度浅色15.80:1、深色14.64:1。原保存/冲突/重开、隔离及宿主/Worker重启恢复全部通过。
- 1133源码及73主产物验收前后指纹一致，本轮5文件均在允许清单，其余既有修改保留。8081实际提供的index及Operators JS/CSS与主构建一致；未重启常驻服务、未提交/推送。证据evidence/operator-description-refinement/select-theme/acceptance.json及同目录日志/截图、标准summary均为本地缓存、不入库；WIP=0。
- 用户在验收期间提出取消组、将归属写入字段说明：已确认方案可行并说明作者执行契约继续提供精确标识；本修正没有实施该结构调整，后续实施需重新冻结工作台。

## 用户验收修正 R7：取消组，直接维护字段列表

用户确认取消配置页的组概念，将归属写入字段说明，并要求实施。本修正替代R5的分组编辑与保存格式；沿用不考虑兼容性的约定，历史完成证据保留。

- `[x]` 输入、参数、输出各一张可编辑表；删除组名称、组说明、组容器和全部组增删操作。新增按钮分别为“添加输入字段”“添加参数”“添加输出字段”；每行直接编辑或删除，表头与配置项保持一一对应。
- `[x]` inputs/outputs直接保存字段数组，字段归属、单位和局部规则写入行description，整体使用前提写入usage_notes；允许同名字段以说明区分归属并保持行序。参数格式及三态未知值沿用R5，保留R6下拉框深浅主题配色。
- `[x]` Schema/OpenAPI、样例、HTTP/usage/full及普通客户端往返、文档和浏览器草稿key同步；拒绝旧fields分组格式，不兼容或迁移旧说明及旧草稿。作者执行契约继续提供精确输入/输出标识，本地文字不会转换为执行规则。
- CMake配置和test_metadata_description/test_operator_contract_host/flowsql_runtime_config构建通过，主前端、Config/Web库及公共Schema资产已同步。前端49/49，JS/Python语法、42处文档文件链接、29个JSON代码块和diff检查通过。
- 标准运行20261101T103735Z-2cb05b7c：6项功能/回归及6项环境入口，selected=executed=passed=12，failed=skipped=not_executed=0，CTest259.63秒。覆盖空表、字段增删/行序、同名字段归属、未知值、JSON/default文本、保存中继续编辑、冲突/历史及组件隔离、真实HTTP往返和宿主/Worker重启恢复。
- 18布局场景、6组主题控件检查和深浅主题8次真实下拉菜单展开/键盘选择通过；平铺字段保存/重开、深浅布局及菜单截图已审查。验收前后1133源码和73相关主产物指纹完全一致，标准runner源码/产物快照也一致。
- 本轮16文件增量均在允许清单，既有其他修改保留。证据位于evidence/operator-description-refinement/flat-fields/acceptance.json及同目录日志/截图，标准summary位于evidence/test-environment/20261101T103735Z-2cb05b7c/summary.json，均为本地缓存、不入库。
- 本机8081提供的index及Operators JS/CSS与新主构建一致；常驻后端仍映射已删除的旧Config库inode，新保存格式需要重启后端加载新库。本轮未重启常驻服务、未清理既有数据、未提交/推送；工作台WIP=0，停止。

## 用户验收修正 R8：简洁呈现并填写 NPM / Baseliner 契约

用户确认保持现有简洁布局，并要求实施时填写npm.basic与explore.baseliner两份契约。本修正为既有作者契约补充可选短说明，沿用同一通用页面及本地三张编辑表；原T1～T5和R1～R7证据保留。

- `[x]` 作者契约顶层description/examples、输入description/本地structure_ref、configuration.description及输出description纳入公共Schema；usage确定性投影原文并保留作者来源、精确版本和完整结构资源。文字不转换为执行规则，没有第二份可执行契约。
- `[x]` 通用页面直接展示用途、输入、参数和输出短说明，复杂角色/递归结构/字段规则留在现有完整技术详情；使用限制与示例按需展开。本地补充独立保存，保留三张平铺编辑表；未增加组、调用检查、缺失提示、模式切换或算子名称特判。
- `[x]` Baseliner复用现有生产声明，补充用途、输入/传参/四输出说明和带明确前提的SQL示例；配置式数据集、value/ratio/relation、产生条件、forecast九项NULL规则及语义上下文完整保留。
- `[x]` NPM真实.so两个ABI索引导出同一作者契约，描述报文输入、17个WITH字符串键、V1 JSON与12项旧调优参数互斥、时间能力/模块依赖及累计/事件/终态语义。12种IPv4/IPv6实体的358字段、类型/可空/元数据取自生产descriptor，数值默认/范围取自生产定义；foreground选择一个实体，managed_results为有条件的托管集合，含无observing零列零行分支。没有有效normalized_config时通用静态推导保持未确定，实际Open仍由生产解析器验证。
- 审计从真实新.so读取声明，与同源生产Schema及解析器逐项对照。测试宿主激活NPM需要真实NPI池，已补齐flowsql_npi及仓库protocols.yml；未伪造能力或改变生产依赖。
- CMake配置、主构建和最终宿主重建通过，主库/前端/Schema产物已生成；前端49/49，修改C++格式、脚本语法、24处文档链接、7个JSON代码块及diff检查通过。
- 首轮20261101T121227Z-31db89ba完整执行28项，27通过、1浏览器驱动失败，零跳过，源码/产物指纹一致；驱动仍点击已直接展示的model_parameters旧summary。已验证直接条件说明并改为真实鼠标展开使用限制，保留全部布局/完整规则断言；首轮证据保存在first-run/，未删减必需清单或放宽断言。
- 最终标准运行20261101T121646Z-eea43342：16功能/回归及12环境入口，selected=executed=passed=28，failed=skipped=not_executed=0，CTest323.25秒。覆盖IID/HTTP/普通客户端、真实发布与精确引用、NPM/周期/多族输出、Baseliner配置与输出，以及有限PCAP经NPM/托管结果至Baseliner的真实调度链路；所选services=[]，未扩展外部后端矩阵。
- 浏览器18布局场景、6主题控件检查、8次真实菜单展开/键盘选择、两份作者说明与完整文档逐项相同、示例/技术详情按需读取、保存期间编辑/冲突/历史/组件隔离和宿主/Worker重启恢复通过。两份compact截图与代表性深浅布局已审查。
- 验收前后1137源码及79相关主产物指纹一致，标准runner源码/产物快照也一致；本轮21文件均在允许清单，既有其他修改保留。证据evidence/operator-description-refinement/compact-contracts/acceptance.json及同目录日志/截图，标准summary位于evidence/test-environment/20261101T121646Z-eea43342/summary.json，均为本地缓存、不入库。
- 本机8081静态入口与主构建一致，但常驻进程仍加载旧Config及旧上传组件，两份describe仍为contract_state=missing；新说明需要重载后端并更新两份算子组件。本轮完成代码、构建与隔离真实发布链路，未部署或重启常驻服务、未清理数据、未提交/推送；工作台WIP=0，停止。

## 用户验收修正 R9：已有说明直接进入对应编辑框

用户指出Baseliner用途等已有文字应填写在对应编辑框中。本修正替代R8的框外短说明展示；保持同一通用页面和三张平铺字段表，历史完成证据保留。

- `[x]` 用途、输入说明、参数说明、输出说明、使用限制和调用示例均直接预填作者原文并原位编辑；当前页面删除重复的框外作者段落，历史声明继续只读，完整机器契约保留在技术详情中。
- `[x]` notes新增可选input_description/parameter_description/output_description，最大30000字符；usage.annotations按原文投影。用途/单项示例上限同为30000。页面优先已有本地文本，既有空框以作者原文初始化；本版保存后尊重显式空值，清空、重开和重启后不再填回作者原文。
- `[x]` 三张字段表、CAS版本校验、保存期间继续编辑、草稿/组件隔离及历史只读保持；旧版空白会话草稿不覆盖作者初值。Schema、OpenAPI、样例和用户说明同步，编辑文字不会改写作者机器契约或执行算法。
- CMake配置和主构建通过，Config/Web及最终前端和公共Schema产物更新；54项前端测试全部通过。修改C++格式、JS/Python语法、24处文档文件链接、7个JSON代码块和git diff --check通过。
- 首轮20261101T130945Z-937fbb86完整执行14项，13通过、1失败、零跳过，源码/产物指纹一致。独立浏览器驱动仍查找已移入编辑框的四个框外输出节点；依据日志将该直接驱动纳入允许范围，改为与真实full契约逐项精确比对四份输出说明并确认无框外重复。首轮证据独立保存在first-run/，原清单和超时不变。
- 最终标准运行20261101T131604Z-bde14316：7功能/回归与7环境入口，selected=executed=passed=14，failed=skipped=not_executed=0，CTest272.64秒。覆盖共享Schema/IID、真实HTTP、普通客户端、两算子完整浏览器和NPM生产契约审计；没有扩展外部后端或重复算法矩阵。
- 六处编辑文字与两份真实作者契约逐项相同，修改/清空/保存/重开及宿主/Worker重启恢复通过；18布局、6主题控件检查、8次真实菜单展开、草稿/期间编辑/冲突/历史/组件隔离全部通过，两份compact和代表性深浅截图已审查。
- 验收前后1139源码和79相关主产物指纹一致，标准runner源码/产物快照也一致；本轮15文件均在允许清单，既有其他修改保留。证据evidence/operator-description-refinement/inline-editors/acceptance.json及同目录日志/截图，标准summary位于evidence/test-environment/20261101T131604Z-bde14316/summary.json，均为本地缓存、不入库。
- 现场8081静态入口与主构建一致，两算子已更新组件，describe均为contract_state=declared；常驻PID1413915仍加载旧Config库。刷新即可读取新预填页面，保存新增说明需重启后端加载新Config库，无需再次上传算子。本轮未重启用户服务、未清理数据、未提交/推送；工作台WIP=0，停止。
