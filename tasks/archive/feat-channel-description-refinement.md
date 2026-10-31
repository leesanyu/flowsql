# Feature：通道说明配置重构与精简（channel-description-refinement）

状态：`[x]` 已完成，T1→T5 已按顺序逐项验收并标记；优先级：P1；立项：2026-10-30；完成：2026-10-31。
依赖已交付的 [通道与算子说明管理](../archive/feat-metadata-description-query.md)，复用其 F1/F2/F3 存储、来源和执行契约基础。
依据：[讨论报告](../../evidence/metadata-description-query/channel-description-review-2026-10-30.md)及后续“增加关联、暂缓复杂指标自动计算”的决定。
报告为本地未入库材料；本规格完整承载实施边界，不依赖报告或旧对话才能理解。原 Feature 的验收结论不因本次立项改变。

## 业务意图与 Non-Goals

让维护数据库表、DataFrame/CSV 和结构化 Kafka source/both 说明的用户，在紧凑字段表中完成必要配置，减少理解内部状态、来源和完整 JSON 的负担。
交付后，普通字段以文本说明为主；确有需要时配置单位、时间解释和关联，保存后普通 HTTP 客户端能够读到与页面一致的结构化事实，旧说明和历史版本不丢失。
这是独立的说明配置体验与保存适配优化，不新增一套语义系统，也不将模型接入列为验收前提。

- 不实现累计计数器差分/重置、比率汇总、指标公式、业务口径计算；不为这些能力增加度量分类、分子分母、公式等配置项。
- 不调用模型、不自动提取自由文本语义，不实现 Agent、MCP、自动 JOIN、外键建表、关联基数或业务数据正确性验证。
- 不支持任意日期字符串格式解析；首轮时间配置覆盖数值 Unix 时间戳、原生日期时间和数值/原生时长。
- 不修改执行 ABI、算子算法、批/流调度、数据面或业务数据；不扩展现有通道类型和数据库支持矩阵。
- 不重构算子说明页、独立标准管理或全站表格；旧语义管理入口及既有 HTTP 客户端继续兼容。
- 不删除既有高级语义，不做全库迁移、全库扫描或消费 Kafka；列表、HTTP 说明查询及目标关联选择不自动加载 CSV，目标关联选择只读取目录和元数据。

## 用户可见结果

| 内容 | 精简后的交互与保证 |
| --- | --- |
| 对象说明 | 文本填写用途及整表限制；保留可选的“一行表示什么”，收进同一紧凑区域，不另加行语义枚举 |
| 字段表 | 名称/类型/可空性只读；字段说明为主要输入；单位及按需时间、关联入口位于字段行内，不再按字段追加独立高级区域 |
| 字段说明 | 新增枚举含义、空值含义、有效条件和限制使用文本；不新增独立 NULL、度量或聚合编辑列 |
| 时间解释 | 默认不展开；可设置 Unix 时间戳、日期时间或时长，按实际含义显示必要输入 |
| 可选关联 | 选择目标数据实体与字段；一个关系可包含多个字段配对，字段行展示所属关系摘要，可添加、编辑和删除 |
| 类型摘要 | 例如 `id · BIGINT · 非空`、`amount · DECIMAL(18,2) · 可空`；保留精度、小数位、嵌套结构、时间单位/时区等关键事实，过长内容按需查看 |
| 字段搜索框 | 保留筛选功能，仅在字段表工具栏显示输入框，以灰色占位文字“搜索字段名…”提示，不显示独立“筛选字段／搜索列名”标签；单关键词包含匹配，可同时显示多个字段，清空恢复全部且保留所有字段草稿 |
| 状态提示 | 移除常驻 known/missing/unbound 字串和重复边界声明；保留加载未完成、来源变化、版本冲突、引用不可用、部分成功等中文提示 |
| 技术详情 | 默认折叠，名称为“技术详情（最近读取结果）”；可复制完整 describe JSON，明确不包含未保存草稿 |

- “时间、关联与使用限制”“枚举、时间、聚合及其他说明”两个原配置区移除；普通编辑流程不显示状态/来源选择器、字段 ID 或字典/绑定身份。
- 已有 NULL 含义、限制等文字在相应说明区域作为已有补充可读；不自动拼入可编辑文本，不要求编辑者重新录入，不隐藏已知限制使其只剩技术 JSON 可查。
- 未知可选语义不逐项渲染占位或错误；未保存不等于不适用，填写声明不等于数据验证通过。
- 搜索框输入后占位文字自然消失，清空后恢复；灰色提示在深浅主题下可读，保留不可见的可访问名称。该功能只筛选页面字段行，不涉及索引或业务数据查询。
- 复用现有对象入口；packet、sink-only、连接和配置等不支持字段说明的对象继续保留原只读身份与原因。
- 进入或刷新说明页自动检测来源，实际差异以弹框列出，“知道了”后继续编辑；移除字段表下的人工确认项和空差异列表。只有 identity 观察限制时正常进入，技术 JSON 中仍保留真实身份状态。
- 点击 DataFrame 实例进入说明时，pending 状态自动复用原预览加载接口（`{name,page:1,page_size:1}`），成功后重新 describe 并显示统一编辑表单；已加载实例不重复预览。加载期间显示进度并禁用编辑/保存，失败保留草稿且可通过“刷新”重试，不再提供“显式加载 DataFrame 结构”按钮。该用户交互变更覆盖原 metadata-description-query 中 Web 点击实例不加载的约束；HTTP describe、列表和关联目标读取仍保持只读，加载不隐式保存说明。

## 核心数据与接口契约

### 复用持久模型与兼容入口

复用 `IMetadataDescriptionV1` 及 `/api/semantics/v1/describe`、`channels/save-description`；旧 `changes` 请求与响应保持兼容，内部部署路径仍省略 `/api`。
不新增 IID/虚函数，不改变字典 Schema 的 `SemanticValue`、`time_semantics`、`entity.relations` 或 SQLite 表结构；新表单是既有模型的编辑投影。
继续使用 `basis`、精确引用、幂等保存、当前对象副本隔离和既有错误信封；`applied|definition_saved_not_applied` 的含义不变。

为避免新字段尚无字典 ID 时由前端构造身份，保存请求增加 `editor_changes` 分支，与旧 `changes` 二选一；两者同时提交或均缺失均拒绝。
同切片发布该增量的 JSON Schema / OpenAPI；下表为新分支的完整输入边界，`?` 表示可省略。

| 数据结构 | 成员及含义 |
| --- | --- |
| 保存信封 | `{locator, basis, editor_changes, confirmations?, timeout_ms?}`；locator/basis/确认/总预算沿用原契约 |
| `editor_changes` | `{description?: TextEdit, row_meaning?: TextEdit, fields?: {源字段名: FieldEdit}, relations?: RelationEdit[]}` |
| `TextEdit` | 字符串或 `null`；空白文本按清空处理 |
| `FieldEdit` | `{definition?: TextEdit, unit?: string\|null, time?: TimeEdit\|null}`；禁止写结构、身份和其他未声明键 |
| `TimeEdit` | `{mode: unix_timestamp\|datetime\|duration, unit?: s\|ms\|us\|ns, timezone?: string}`；单位/时区的适用条件见下文 |
| `RelationEdit` | `{dictionary_ref: 精确目标字典引用, field_map: {当前源字段名: 目标field_id}}`；至少一个字段配对，目标身份来自选择器读取，不要求用户输入 |

- 所有编辑键省略表示保留，`null` 表示清除对应人工声明并保留未知，不自动生成“不适用”；未编辑属性及来源不重写。
- `relations` 省略保留原关系；提交数组表示替换该对象的关系集合，`[]` 为明确移除。页面必须携带未修改关系，不能因筛选或目标暂不可用而丢掉它们。
- 后端先得到当前对象候选字典及稳定字段 ID，再将源字段名转换成内部映射；前端不生成 ID、不修改绑定、不提交物理类型覆盖。
- 编辑字段不存在、关系配对重复或引用无效时，返回已有结构化错误并定位字段；不自动猜测重命名或补齐目标。
- 原始 `changes` 及通用字典保存继续采用原含义；新增自动维护只作用于新编辑分支明确涉及的信息，不改变其他调用方的声明。

### 时间与单位的最小表达

下表省略已有 SemanticValue 的 `state/value/origin` 包装；例如 `kind` 实际位于 `time_semantics.value.kind`。

| 用户输入 | 持久化映射与边界 |
| --- | --- |
| Unix 时间戳 | `time_semantics.kind=instant`，`epoch=1970-01-01T00:00:00Z`，必须明确 `s/ms/us/ns`；不从字段名或数值位数推断 |
| 日期时间 | `kind=instant`；来源有时区则采用明确事实，否则时区可补充，也可保持未知；不默认 UTC，首轮不提供字符串格式输入 |
| 时长 | `kind=duration`，必须明确 `s/ms/us/ns`；不设置时间基准/时区，不加入事件时间点字段汇总 |
| 一般单位 | 非时间字段可填写单位文字；保存为既有 `{code,label}` 结构，不自动建立标准或猜测物理量 |

- `unix_timestamp` 适用于数值字段，`datetime` 适用于原生日期/日期时间类型，`duration` 适用于数值或原生时长；无法确认类型适用性时保留原信息并提示，不伪造可用解释。
- `TimeEdit.timezone` 仅日期时间模式接受，`TimeEdit.unit` 仅 Unix/时长模式接受；日期时间精度取明确来源，未知则保留未知，模式不适用的输入拒绝。
- 时间单位使用统一编码与展示标签；同一时间字段的 `unit` 与 `time_semantics.unit` 一致，界面只保留一个编辑入口。一次请求的两处输入矛盾则拒绝，不能任取其一。
- 清除时间解释不自动清除一般单位；来源原本明确的时间事实仍可展示。已有非本轮支持的时间表示只读保留，明确替换前不降级或清空。
- 明确的生产者声明或受支持原生类型可提供预填；首轮覆盖 Arrow timestamp/duration 的明确单位/时区和数据库原生日期时间类别，未识别类型保留未知；数据库 TIME 等有歧义的类型不猜测为时长。
- 预填不产生写操作；保存时只补充缺失事实，不覆盖既有人工/导入声明。来源变化与声明冲突保留原值并提示核对，不能静默修正用户说明。
- 用户输入记录 `manual`；生产者明确声明保留 `producer`，由类型/字段配置计算的内容记为 `derived`；不显示来源选择器。
- 时间编辑完成后，以该字段保存后的有效时间解释更新 `entity.time_fields`：已明确时间点加入，时长/未指定移除；清除人工覆盖后来源仍明确为时间点的继续保留。其他未编辑成员保留，空汇总不推定所有字段均无时间含义。
- 单位和时间之外的行语义、度量类型、聚合、有效条件等保持已有内容或未知，不从自由文本派生结构化事实。

### 可选关联的选择与保存

用户从源字段行打开关系编辑，选择目标数据实体及其字段；“增加字段配对”组成同一个联合关联，多条独立关联可以并存。
后台复用目录、describe、精确 resolve 与绑定映射；界面显示对象/表/字段名称，保存为现有 `entity.relations` 的精确字典引用及 `源field_id → 目标field_id`。

- 新增或替换关联的目标须有可读取且与当前结构匹配的已保存说明。未配置时提示从目标对象入口先保存说明，不隐式创建目标字典，不要求手填内部引用。
- 目标目录按需读取，只有选中目标才描述/解析其字段；不逐对象读取全库说明，不读取业务行，不加载未加载的 DataFrame。
- 每个联合关联至少一对字段，同一关系内源字段和目标字段分别不重复；不同关系允许复用字段，不强制每字段只能关联一次。
- 保存验证源字段及精确目标版本内的字段存在；人工选择产生 `manual` 声明。存在与否不等于唯一性、外键约束或实际数据可连接性验证，不承诺生成或执行 JOIN。
- 精确目标版本不会自动跟随最新版本。已有历史/无当前对象定位的关系仍按精确引用展示标题与字段；无法读取时保留原关系和草稿，提示问题，禁止借简化表单静默删除。
- 关联编辑改变源对象说明，不修改目标对象的说明、绑定或业务表；联合关系作为一个整体显示与修改，不能被拆成几条含义不同的独立关系。

### 保存保真与异常行为

- 以原字典为基底，合并明确编辑的语义；隐藏的 `null_meaning/role/measure_kind/validity/aggregation/value_domain/standard_refs` 及表级限制、身份、行语义原样保留。
- 修改其他字段不能重写未编辑的 `unknown/not_applicable`、来源、标准引用或关系版本；文本与既有结构化信息并存时分别表达，不自动覆盖或声称相互一致。
- 沿用 `SOURCE_CHANGED/REVISION_CONFLICT` 与保存前来源复核；实际字段删除、对象替换和来源变更先自动弹框告知，再由显式保存应用。弹框及读取不写入；无未保存编辑时使用最新检测依据，有原稿时保留其依据。失败保留本对象草稿，来源变更时重读并提示最新差异，不自动重试写入，切换对象不得串用。
- 定义已保存但绑定未应用必须显示部分成功及可重试状态；刷新读取结果后才更新技术详情。重复提交不制造无意义新版本，历史版本仍可精确读取。

## 主链路

1. **读取与编辑**：进入已有对象说明 → describe 捕获结构、绑定与精确说明 → 渲染紧凑字段表并展示可靠预填/已有补充 → 编辑文本、必要单位/时间；需要关联时按需选择目标与字段配对。读取和草稿编辑不写注册表、不访问业务行。
2. **保存与重读**：提交原 basis 与 editor_changes → 核验当前来源 → 服务端生成/沿用字段身份、映射并合并语义 → 校验并保存版本 → 沿用 F2 apply → 按成功/冲突/部分成功展示 → describe 重读；同一普通 HTTP 客户端可取得精确时间与关联信息。

## Feature Tasks

各 Task 的完整目标验收后才勾选；实施步骤、允许文件、命令和时间盒在工作台逐次冻结。用户已授权按 T1→T5 连续实施，每项完整验收并勾选后再进入下一项。

- [x] **T1：交付兼容旧调用方的简化编辑与局部保存基础，使用户无需提供内部身份即可首次保存并保留既有语义。**
  验收：A1、A2；新分支 Schema/信封及文本/单位保存可用，未实现的时间/关联行为不得伪装为已交付。
- [x] **T2：交付紧凑的对象与字段说明界面，让日常填写集中在文本和必要信息，并保持原操作和草稿可用。**
  验收：A3 及 A8 的基本编辑、搜索、切换与异常提示；高级状态表单消失，旧补充内容可读且不改写。
- [x] **T3：交付按需时间解释及有依据的自动维护，使普通客户端能够区分时间点与时长并读取明确单位。**
  验收：A4、A5；首存、修改、清除、来源冲突和服务重启后的时间信息一致。
- [x] **T4：交付可选单字段与联合字段关联，使用户通过对象名称完成关联配置并保留可追溯的精确映射。**
  验收：A6、A7；新增/修改/删除、多关系、精确目标版本、无目标说明提示和原关系保留均可验证。
- [x] **T5：完成跨入口、保存与消费链路回归及使用说明，使精简界面与既有客户端共享同一可靠语义结果。**
  验收：A1～A10 全部通过，更新当前使用说明并记录实际产物与运行验证边界，再完成归档。

## 验收锚点与执行边界

| 编号 | 必须转成可执行断言的结果 |
| --- | --- |
| A1 | 新旧保存请求均可用；changes/editor_changes 互斥及非法键被拒绝；未绑定对象首次保存、稳定字段身份、共享字典对象隔离、幂等、重启重读正确 |
| A2 | 含全部高级语义及四种来源的旧字典仅改一处文本，其余语义不变；省略/清空/空关系集合含义明确，NULL 说明不被重复拼接或丢失 |
| A3 | 两个高级区和状态/来源选择器不出现；关键异常仍可处理；名称/类型/可空性紧凑可读，Decimal/嵌套/时间精度不丢；搜索框无独立可见标签，灰色“搜索字段名…”占位提示随输入/清空切换，多行匹配且不丢草稿 |
| A4 | 数值 Unix 秒/毫秒/微秒/纳秒、无时区原生日期时间、时长、Arrow 明确类型正确往返；缺时区保持未知，时间/单位冲突及不适用类型拒绝，普通整数不自动识别为时间 |
| A5 | 时间字段汇总按涉及字段维护，时长不被当作时间点；未编辑成员/来源保留，原生预填不隐式写入，来源与人工声明冲突不覆盖；清除行为可重读验证 |
| A6 | 首次保存源说明时可建立单键/联合键/多关系；源名称映射正确、精确目标可解析；重复配对/缺失字段/不存在引用拒绝，不把关联声明当作数据验证 |
| A7 | 目标无说明/pending/不匹配时提示且零隐式创建；目录按需读取；历史关系、目标版本变化及暂不可读不静默删除或升级；目标数据与说明不被修改 |
| A8 | 真实浏览器从 DataFrame、数据库表、结构化 Kafka 入口完成编辑保存；搜索/切换/刷新/冲突/部分成功草稿保留；技术 JSON 是最近读取结果，服务重启后状态一致 |
| A9 | 普通 HTTP 与页面读取一致，新时间/关联信息满足现有文档 Schema；旧客户端、精确 resolve、注册表引用检查及受影响算子预检/输出派生回归通过，不新增模型或复杂指标能力 |
| A10 | 1280×800、1440×900、1920×1080 三视口及深浅主题下真实点击可达，字段表和局部展开不撑宽页面、不遮挡按钮；原列表/预览操作与读取边界保持 |

- 测试锚定顺序：先新增/调整所选失败断言，再实现对应 Task；本规格不以自然语言审查代替实现后的机器验收。
- 基础夹具复用 native 临时 SQLite、DataFrame/CSV、Kafka 保存配置、回环 HTTP、Chromium；类型表示用契约夹具覆盖，数据库目录使用受管 SQLite，不默认扩大到外部四库/Kafka 认证或数据面矩阵。
- 核心回归覆盖 Config 元数据保存/HTTP、F1 字典规范化/引用、F2 描述/绑定/来源、通道浏览器和普通客户端；时间/关联共享语义还需 F3 兼容检查/输出派生。若修改共享编辑工具，回归旧语义页及算子页相应使用路径。
- 原先外部 provider 未改变时不默认重跑其部署矩阵；若实际改动 provider 行为或公共类型提取，实施前在工作台增加对应回归及选择依据。
- 实施前冻结实际 CMake targets、标准测试入口 filter、前端单测/生产构建、fixture 与资源锁；不执行无筛选全量测试或默认镜像构建。
- 只读断言检查加载/查询/消费/注册表修订计数；保存断言检查真正读取和重启结果。主产物构建、隔离宿主行为验证和用户常驻服务是否重载分别记录。
- 完成要求 selected=executed=passed，failed=skipped=not_executed=0，相关格式与文档检查通过；证据写入 `evidence/channel-description-refinement/`，不提交本地证据。

## 完成证据

- [x] **UI 补充（2026-10-31）：点击 DataFrame 实例自动加载后进入说明配置。** 进入时先 describe，仅 pending DataFrame 自动调用原预览加载接口后重读结构；移除显式加载按钮，加载期间显示进度并禁用保存/刷新，失败可刷新重试，已加载实例不重复预览。关闭详情后刷新列表的状态/行数/结构；保留实际草稿及原依据，加载零隐式说明写入，列表/HTTP describe/关联 pending 目标不自动加载。旧页面已由新增断言复现失败；前端主产物构建、语法、增量格式、范围及截图复核通过。通道浏览器及环境夹具 selected=executed=passed=2，failed=skipped=not_executed=0，覆盖首次自动加载、进度/失败重试、无重复加载、列表更新、零隐式保存，以及既有关联/来源差异/预览分页/草稿/三种视口与主题/重启回归。Summary：`evidence/test-environment/20261031T063046Z-328d4569/summary.json`；完整本地证据：`evidence/channel-description-refinement/dataframe-auto-load/acceptance.json`。用户常驻服务未重载，未提交或推送。

- [x] **UI 补充（2026-10-31）：进入说明页自动检测来源差异并弹框提示。** 删除字段表下的人工勾选项和空变化列表；无实际差异的数据库表直接编辑保存，实际字段/来源变化以可读弹框告知，“知道了”后继续编辑。无编辑时自动采用最新依据，有原稿时保持原依据；结合已保存来源快照识别真实顺序变化，避免说明字段排列引起反复提示。保存期间来源变化仍被服务端拦截，页面重读并提示最新差异，不自动重试写入。前端构建、语法、增量格式及范围检查通过；通道浏览器最终 selected=executed=passed=2，failed=skipped=not_executed=0，覆盖真实 SQLite/Kafka、进入即检测、类型/新增/删除/重排、零隐式写入、草稿及三种视口/深浅主题。Summary：`evidence/test-environment/20261031T061412Z-890c99a5/summary.json`；完整本地证据及深浅主题截图：`evidence/channel-description-refinement/source-diff-dialog/acceptance.json`。后端接口与旧语义页未改动，未重载用户常驻服务，未提交或推送。

- [x] **UI 补充（2026-10-31）：删除清除时间解释时的多余提示。** 移除“保存后仍会保留来源明确的时间事实，一般单位不变。”；组件增量仅删除该段落，时间选择、单位、草稿和保存语义保持原样。前端主产物构建通过；既有通道浏览器回归 selected=executed=passed=2，failed=skipped=not_executed=0，Summary：`evidence/test-environment/20261031T054021Z-8ca3b169/summary.json`。组件及产物文案检查、增量格式和范围核对通过；本地证据：`evidence/channel-description-refinement/remove-time-hint/acceptance.json`。

- [x] **UI 补充（2026-10-31）：展开/收起改为箭头图标。** 表头右侧复用 Element Plus 上/下箭头，以 28px 图标按钮替代可见文字；保留悬停提示、无障碍名称和展开状态，原折叠与草稿行为不变。前端主产物构建通过；既有通道浏览器在三种宽度及深浅主题下通过，Summary：`evidence/test-environment/20261031T052837Z-3c5db9ca/summary.json`（selected=executed=passed=2，failed=skipped=not_executed=0）；已复核两种状态的实际截图。完整本地证据：`evidence/channel-description-refinement/collapse-icon/acceptance.json`。

- [x] **UI 补充（2026-10-31）：字段表一键收起，保存与刷新移至顶部。** 表头最右侧提供“收起 / 展开”，隐藏搜索栏和全部字段内容但保留表头、筛选条件与草稿；顶部并排显示“保存”“刷新”，收起状态可保存，原 busy/legacy/pending 禁用及冲突、部分成功、旧稿处理保持有效。前端主产物构建通过；三种宽度 × 深浅主题的真实点击、展开/收起截图、草稿保留和受影响通道流程通过，Summary：`evidence/test-environment/20261031T052235Z-ebf60ea0/summary.json`（selected=executed=passed=2，failed=skipped=not_executed=0）。完整本地证据：`evidence/channel-description-refinement/collapse-toolbar/acceptance.json`；未修改服务端或 SQL，未重载用户常驻服务，未提交或推送。

- T1：兼容保存、稳定身份、保真、冲突/部分成功与重启断言通过。原冻结清单在 native 完整执行：selected=executed=passed=18，failed=skipped=not_executed=0（含 6 个环境夹具）。证据：`evidence/test-environment/20261030T154859Z-5c35a9ee/summary.json`、`evidence/channel-description-refinement/t1-tests-native.log`（本地不入库）。此前 socket 阻塞已解除；新增 HTTP 请求补齐既有 required_confirmations，不改变服务端确认检查。主 Config 与测试宿主已构建、格式及范围检查通过。

- T2：紧凑表单、补充只读、搜索、多视口/主题/真实点击、旧稿恢复、冲突与部分成功草稿、管理操作和重启通过。最终稳定产物分两次验收合计 selected=executed=passed=4，failed=skipped=not_executed=0；summary：`evidence/test-environment/20261030T160029Z-d58289d0/summary.json`、`evidence/test-environment/20261030T160208Z-e2a6b52c/summary.json`；截图及日志 `evidence/channel-description-refinement/t2-browser/`。中途产物变化的验收被 runner 拒绝，未用作完成依据。

- T3：时间编辑、可靠预填、清除/冲突/旧表示保真、汇总、重启及 F3 消费通过；selected=executed=passed=20，failed=skipped=not_executed=0。Summary：`evidence/test-environment/20261030T161705Z-0de0ab2c/summary.json`；构建和验收日志 `evidence/channel-description-refinement/t3-build.log`、`t3-tests-final.log`。仅新增编辑分支维护时间；旧 changes/共享模型/provider/执行 ABI 保持兼容。

- T4：首次保存的单键/联合键/多关系、非法配对、精确历史版本/重启、按需目录、目标缺说明零隐式创建、目标暂不可读保留、关联草稿恢复、数据库目标选择均通过；selected=executed=passed=20，failed=skipped=not_executed=0。Summary：`evidence/test-environment/20261030T163244Z-73d9876c/summary.json`；`evidence/channel-description-refinement/t4-tests-final.log`。浏览器发现并修复新增关系属性未触发 Vue computed 更新的问题；关闭前/恢复后完整关系基底已加入断言，最小复现和草稿证据均保存在本地。

- T5：A1～A10 完整清单通过，包含同一个旧字典具备全部高级语义及四种来源、仅修改一处文本的整文档保真断言，以及普通客户端新时间/精确关联 → 显式预检 → 原 passthrough SQL → 结果字典的消费闭环；目标说明版本不变。当前使用说明及 SQL 指引已更新。
  最终 selected=executed=passed=28（17 项业务测试、11 项环境夹具），failed=skipped=not_executed=0；Summary：`evidence/test-environment/20261030T164549Z-b472547b/summary.json`；完整日志：`evidence/channel-description-refinement/t5-tests-final.log`。前端公共语义工具另通过 5/5、零跳过：`evidence/channel-description-refinement/t5-frontend-tests-final.log`。补强保真断言前的 28/28 为中间检查点，最终结论以上述重跑为准。
- 主程序、Config/Web 插件、静态资源及所选测试目标已按 `src` 构建入口构建，隔离宿主和 SQL 测试实际加载本轮产物。完整验收期间源码及产物摘要一致；最终清单、源码/产物 SHA256、A1～A10 测试对应及运行边界见 `evidence/channel-description-refinement/acceptance.json`。C++ clang-format、`git diff --check`、本地文档链接、Feature 增量 Diff 审查与基线范围检查通过，既有工作树修改保留；证据均为本地缓存，不入库。
- 未修改 SQL 执行 ABI、算子算法、批/流调度和数据面；本轮 SQL docs/discovery/finite、F1/F2 与受影响 F3 回归已执行。未选择 SQL Stream、外部数据库部署矩阵、Kafka 认证/数据面、容器发布及模型验收，不将本轮通过表述为这些范围已验证。用户常驻服务未重载；未提交或推送代码。全部任务完成后归档，工作台恢复 WIP=0。
