# Feature: FlowSQL 阶段化过滤管线

状态：`[x]` 已完成
优先级：P0
前置 Feature：`framework-core`、`pipeline-async`、`npm-packet-contract`（均已完成）
后续 Feature：`npm-offline-filter`、`npm-basic-analysis`、`npm-capture-contract`

## 业务意图

FlowSQL 允许 source 输出以及每一级 operator 输出分别声明过滤条件。框架把过滤解析为绑定到阶段输出
Schema 的类型化谓词，以 Arrow batch 通用执行器保证正确性；channel/operator 只提供可选的精确下推优化，
不各自解析 SQL，也不能因不支持下推而忽略过滤。

目标语法：

```sql
SELECT *
FROM pcapfile.rdp
WHERE has_layer('ipv4') AND has_layer('tcp')
USING npm.basic
WHERE protocol = 'HTTP'
INTO dataframe.http_packets
```

第一段 `WHERE` 绑定 `pcapfile.rdp` 的 `PacketSchema()`；第二段绑定 `npm.basic` 的输出 Schema。packet
领域符号解析和 pcapfile 具体下推由 `npm-offline-filter` 实现，`npm.basic` 的业务 Schema/逻辑由
`npm-basic-analysis` 实现，本 Feature 只提供二者组合所需的公共过滤与输出型管线契约。

## Non-Goals

- 不实现 `npm.basic`、应用协议识别、pcapfile/NPI 领域符号解析或具体 source 下推。
- FlowSQL 阶段过滤首版不支持 JOIN、子查询、聚合过滤、HAVING、动态运行期改写谓词或任意用户自定义
  函数；既有数据库 source 的原生 SQL 能力不因本限制而收窄。
- 不要求所有 channel/operator 实现过滤；不允许下推能力成为查询正确性的前提。
- 不把 packet/DataFrame 转为 JSON 后过滤，不改变 Arrow Schema、raw bytes、sequence 或行顺序。
- 不向既有 `IBlockStreamChannel`、`IBlockStreamOperator` 等接口追加虚函数；新能力使用版本化新 IID。
- 首版不接受 `&`、`==`、裸 `HTTP` 等简写；它们必须报明确语法/绑定错误，后续若增加只能规范化为
  `AND`、`=`、带类型的字面量。

## 语法与阶段绑定契约

首版 pipeline statement 采用以下 EBNF；`{...}` 表示重复，`[...]` 表示可选：

```ebnf
query          := SELECT projection FROM source_ref [WHERE filter_expr]
                  [USING operator_stage {THEN operator_stage}]
                  [INTO destination_ref] EOF
operator_stage := operator_ref [WITH operator_params] [WHERE filter_expr]

filter_expr    := or_expr
or_expr        := and_expr {OR and_expr}
and_expr       := not_expr {AND not_expr}
not_expr       := NOT not_expr | "(" filter_expr ")" | predicate

predicate      := boolean_value
                | value compare_op value
                | field_ref [NOT] IN "(" literal {"," literal} ")"
                | field_ref [NOT] BETWEEN literal AND literal
                | field_ref IS [NOT] NULL
boolean_value  := boolean_field | boolean_call
boolean_call   := function_name "(" [function_arg {"," function_arg}] ")"
function_arg   := field_ref | literal
value          := field_ref | literal
compare_op     := "=" | "!=" | "<" | "<=" | ">" | ">="
literal        := signed_number | single_quoted_string | TRUE | FALSE
field_ref      := identifier
```

- `WHERE` 永远绑定到紧邻其之前的数据生产阶段输出：`FROM` 为 stage 0，首个 `USING` 为 stage 1，
  后续 `THEN` 依次递增；每个阶段最多一个 `WHERE`，operator 的 `WHERE` 位于其可选 `WITH` 之后。
- 每个谓词只能引用该阶段输出 Schema 已存在的字段；不得把 `npm.basic.protocol` 下推到 pcapfile。
- parser 成功前必须消费全部 token；重复、错位或无法绑定的尾部内容一律报错，禁止静默忽略。
- 单条 statement 必须在 `INTO` 目标或最后一个表达式后到达 `EOF`；若入口支持分号，由上层 statement
  splitter 消费分隔符，分号不属于 `filter_expr`。
- 优先级从高到低依次为括号、`NOT`、比较/`IN`/`BETWEEN`/`IS NULL`/函数谓词、`AND`、`OR`；`AND/OR`
  左结合，`NOT` 右结合，`BETWEEN` 内部的 `AND` 不得被解析成逻辑连接符。
- `NOT IN`、`NOT BETWEEN` 分别规范化为 `NOT(IN(...))`、`NOT(BETWEEN(...))`；不接受比较链、空 `IN ()`
  或 `IN` 列表/`BETWEEN` 边界中的字段、子查询和 `NULL`。
- 数值支持有符号十进制整数、小数和科学计数法；boolean 使用 `TRUE/FALSE`。字符串只用单引号，内部单引号
  用 `''` 转义；双引号字符串、未闭合字符串、`field = NULL` 必须失败，NULL 只用 `IS NULL/IS NOT NULL`。
- `field_ref` 首版为 `[A-Za-z_][A-Za-z0-9_]*`，不支持 stage 限定名或 quoted identifier。SQL 关键字和注册
  函数名大小写不敏感，Arrow 字段名及普通字符串比较大小写敏感；领域符号是否规范化由其 resolver 定义。
- boolean 字段可直接作为谓词；比较允许类型兼容的 field-scalar、scalar-field 和 field-field，且至少一侧
  必须是字段。不支持算术、位运算、`LIKE`、正则、数组下标或比较链；字面量溢出或类型不兼容在 binder
  阶段失败，不允许截断转换。
- 函数必须来自当前阶段 Schema 的类型化、确定性函数注册表；首版仅允许返回 boolean 的谓词函数，未知函数、
  参数数量或类型不匹配必须在执行前失败，不得退化成恒真/恒假。
- SQL 三值逻辑统一生效：`WHERE` 只保留 `TRUE`，`FALSE` 与 `NULL/UNKNOWN` 均丢弃。
- `has_layer('ipv4') AND has_layer('tcp')` 表示解码层栈任意位置同时存在 IPv4/TCP；选定有效端点的
  过滤使用实际 Schema 字段或注册领域符号，例如 `transport_protocol = 'TCP'`，不与 `has_layer` 混用。
- `WITH mode=fast` 沿用 operator 参数语法，裸 `fast` 不是过滤字面量；过滤字符串必须写成 `mode = 'fast'`。

合法组合示例：

```sql
WHERE ports_valid
  AND captured_len BETWEEN 64 AND 1500
  AND (src_port IN (80, 443) OR dst_port = 443)
  AND src_port IS NOT NULL
```

首版明确拒绝以下含混或越界写法：

```sql
WHERE ipv4 & tcp                   -- 位运算/BPF 风格不属于本语法
WHERE protocol == HTTP            -- == 非法；HTTP 是字段标识符而非字符串，应写 protocol = 'HTTP'
WHERE src_port = NULL             -- 应写 src_port IS NULL
WHERE 0 < captured_len < 1500     -- 比较链不支持
WHERE src_port IN ()              -- IN 列表不能为空
WHERE unknown_function('tcp')     -- 未注册函数
```

### 多 source 与数据库兼容边界

- `FROM source1, source2 WHERE ...` 的过滤对象不唯一，首版明确拒绝多 source 的 source-stage `WHERE`。
  需要过滤合并结果时写成 `FROM source1, source2 USING merge.operator WHERE ...`，该 `WHERE` 绑定 operator
  输出；每个 source 独立过滤需要未来单独设计 alias/子表达式，不在本 Feature 预先实现。
- 上述 `filter_expr` 是 FlowSQL 管理的 DataFrame/stream/block pipeline 阶段过滤语言。既有数据库 source 的
  `sql_part` 仍是 provider 原生 SQL，可继续包含 `WHERE/GROUP BY/HAVING/ORDER BY/LIMIT`，不得被有限表达式
  parser 拒绝或改写；数据库查询之后的 operator-stage `WHERE` 才使用本 EBNF。
- parser 可以先完成结构切分，planner 在 source 类型解析后决定 source 片段属于原生 SQL 还是阶段过滤；
  字符串、括号或原生 SQL 内出现 `WHERE/USING/THEN/INTO` 文本时不得误切分。

算子链示例：

```sql
SELECT * FROM source.input
WHERE captured_len > 100
USING demo.op1 WITH mode=fast
WHERE score >= 0.8
THEN demo.op2
WHERE result IS NOT NULL
INTO dataframe.output
```

## 核心数据与接口契约

框架内部使用类型化 AST，不以原始 SQL 字符串作为插件契约：

```cpp
enum class FilterExprKind {
    kField, kLiteral, kAnd, kOr, kNot, kCompare, kCall, kIn, kBetween, kIsNull
};

struct FilterExpr {
    FilterExprKind kind;
    uint32_t node_id;  // parser 按先序从 1 稳定编号
    // 字段名、函数名、类型化字面量、比较运算符和递归 operands
};

struct StageFilter {
    uint32_t after_stage;
    std::shared_ptr<FilterExpr> expression;
};
```

独立 expression parser 只负责语法和稳定 AST：保留字段大小写、规范化函数名和 boolean、解码单引号字符串，
不查询 Schema；例如裸 `HTTP` 先解析为字段，随后由 binder 作为未知字段拒绝。`SqlStatement` 保存按 stage
对齐的 `StageFilter`；binder 使用对应输出 Arrow Schema 完成字段索引、类型、NULL 可空性和领域字面量
解析。未知字段、类型不兼容或未知函数必须在执行前失败。

Schema binder 输出只读 `BoundFilterExpr`，保留原 AST 的 `kind/node_id/operands`，并增加 `value_type`、
`nullable`、字段 `field_index`、已按目标类型解析的 Arrow `Scalar` 以及比较/IN/BETWEEN 使用的
`comparison_type`。字段名按 Arrow Schema 精确匹配；同名重复字段视为含混。整数边界由目标 Arrow 类型校验，
不允许截断；field-field 数值比较只提升到可无损覆盖双方的 `int64/uint64/double`，`int64` 与 `uint64` 混合、
64 位整数与浮点数混合等无法安全统一的组合拒绝。逻辑根必须为 boolean，`IS NULL` 本身不可空，其他谓词
按输入传播 nullable。
首个 binder 切片不内置领域函数，函数调用以 `kUnknownFunction` 明确失败，后续注册表不得把未知函数退化为
恒真或恒假。

框架提供统一 Arrow Filter Executor：一次编译谓词，对列生成 Boolean mask 并过滤 `RecordBatch`，禁止
通过 `GetRow()/AppendRow()` 或 JSON 逐行重建。过滤后保持 Schema、metadata、原行顺序、sequence 和
raw binary；全批不匹配时继续消费，不把空批当 EOF，原 block 始终 exactly-once release。

mask 求值入口 `EvaluateFilterMask(batch, bound_expression, output, error)` 只生成长度等于输入行数的 nullable
`BooleanArray`，不执行行过滤。比较节点按 binder 冻结的 `comparison_type` 使用 Arrow safe cast，并映射到
`equal/not_equal/less/less_equal/greater/greater_equal`；`IN` 由 `equal + or_kleene` 组合，`BETWEEN` 由
`greater_equal + less_equal + and_kleene` 组合，逻辑节点统一使用 `and_kleene/or_kleene/invert`。因此
`FALSE AND NULL = FALSE`、`TRUE OR NULL = TRUE`、`NOT NULL = NULL`；最终将 `NULL` 与 `FALSE` 一并丢弃
属于 RecordBatch Filter 切片。空 batch 返回零长度 boolean mask。字段索引或实际列类型与 bound AST 不匹配、
bound AST 形状非法、Arrow kernel/cast 失败及尚未支持的函数节点必须分别返回结构化错误并清空输出，不得
退化为恒真或恒假。

RecordBatch 过滤入口 `FilterRecordBatch(batch, bound_expression, output, error)` 必须复用同一 mask evaluator，
并以 Arrow `FilterOptions::DROP` 固定 SQL WHERE 语义：只保留 `TRUE`，丢弃 `FALSE` 与 `NULL/UNKNOWN`。
输入作为整个 `RecordBatch` 交给 Arrow Filter，禁止逐行或逐列重建；输出必须仍为 `RecordBatch`，且 Schema
及其 metadata 与输入完全相等。过滤保持原相对行序，并原值保留 packet sequence、binary/fixed-size binary、
fixed-size list 及其 child values。空输入和全批不匹配都成功返回保留原 Schema/metadata 的零行 batch，
不能被解释为 EOF；block release 和继续消费属于 runtime Filter Stage 的生命周期责任。

runtime `BlockFilterStage` 在 `Open(input_schema, output_schema)` 时固定输入 Schema，并原样返回同一输出 Schema；
Open 只允许成功一次。有 residual 时用零行 RecordBatch 调用同一个 `FilterRecordBatch()` 做执行能力验证，使
Schema/bound AST 不匹配和不支持的函数在首批数据前失败；无 residual 时建立显式透传 stage。`ProcessBlock()`
只接受与 Open Schema（含 metadata）完全一致的 batch，保持时间戳；有 residual 时复用通用 executor，无
residual 时复用原 batch 指针。每次成功调用恰好返回一个有效 data block，即使结果为零行也不能解释为 EOF；
未 Open、重复 Open、空输入、Schema 不匹配或执行失败时清空输出并返回结构化错误。source Poll/Release、
transform 生命周期和 sink 交付由后续 pipeline runner 负责。

插件以新 IID `IFilterPushdownV1` 暴露无状态规划期能力：输入目标 category/name、阶段输出 Schema、版本化
canonical filter plan 和 planner 允许拆分的候选 `node_id`，只返回其能精确执行的候选子集；provider 不
构造 residual，planner 必须从原 AST 重建 pushed/residual。现有接口不改 ABI。契约要求：

- 顶层 `AND` 可按子项拆分；`OR/NOT` 只有整个子树精确支持时才可下推。
- planner 必须保持 `original ≡ pushed AND residual`；未知、重复或非候选 `node_id` 使规划失败，不能造成
  residual 丢失。
- provider 明确“不支持”时返回空接受集，不是查询错误，Arrow executor 执行完整谓词；协商或执行报错
  则传播为任务失败，不能伪装成“不支持”后继续处理部分数据。
- 近似过滤不得声明为精确接受；任何 residual 都必须执行。
- `EvaluatePushdown()` 为 `const` 且不得保留 request 指针或修改 channel/operator；provider 不拥有目标时返回
  `ENOTSUP` 供 IID 遍历继续匹配，命中目标但不接受任何谓词时返回成功和空接受集。
- planner 必须通过 `Traverse(IID_FILTER_PUSHDOWN_V1, ...)` 匹配 owner，不能用 `First()`；`ENOTSUP` 继续，
  首个返回 0 的 owner 即使接受集为空也以 callback `-1` 停止遍历，其他非零值使协商失败。callback 的停止值
  与 `Traverse()` 自身返回码分别保存；`Traverse()` 非零属于 querier 错误。
- filter 在任务 reader/subscription 创建前绑定，开始 Poll/Process 后不可变；不得修改共享 channel 的
  全局过滤状态。共享 source 仅下推所有订阅者共同的安全谓词，其余在 fan-out 后分别执行。
- planner 验证接受集后，把精确 pushed plan 复制进独占 task session；source adapter 传给独占
  reader/subscription，transform 传给 `BlockTransformTaskConfigV1`。无法建立隔离 session 时禁止下推。
- canonical plan 使用小型版本化 JSON 跨插件传递，只在任务初始化时解析一次；Arrow 数据面不序列化。
- canonical JSON v1 固定为 `{version, root}`，按原 AST 顺序递归并确定性输出；每个节点包含
  `node_id/kind/type/nullable`，field 增加 `field_index/field_name`，literal 以类型和 Scalar 文本携带 `value`，
  compare 增加 `compare_op/comparison_type`，IN/BETWEEN 增加 `comparison_type`，复合节点以 `operands` 保持原序。
  field index 越界、Schema 类型或 nullable 不一致、literal 类型不一致以及当前 binder 不支持的 call 节点均使
  canonical 构建失败，不能继续协商。
- 规划结果进入任务前必须经 `MaterializeFilterTaskSessionPlan()` 重新校验：当前 Schema/原表达式生成的 canonical
  plan 必须与协商输入相同，candidate/accepted ids 必须可由原树重建，未命中 provider 时 accepted 必须为空；
  不一致视为陈旧或串用的协商结果并失败。独占任务只序列化 accepted 子树，不能把包含 residual 的原完整 plan
  交给 provider；空接受使用版本化空 plan `{"version":1,"root":null}`。
- `FilterTaskSessionPlan` 的字符串、accepted ids 与不可变 pushed/residual AST 均由单个任务拥有。transform 通过
  `CreateBlockTransformTaskSession()` 在单次 `CreateTask(BlockTransformTaskConfigV1)` 调用中传入临时字符串指针；
  provider 必须在返回前深拷贝 task id、WITH JSON 与 pushed plan，并为每次调用返回不同的独占 session。
- 当前 source 接口没有订阅级 filter session，因此 shared source 的首版安全策略固定为零下推：忽略该任务协商
  接受集，生成空 pushed plan，并把原表达式完整保留为该订阅者 residual。不得调用共享 channel 的全局
  `SetFilter()`；未来只有先计算出所有订阅者共同谓词并建立隔离 source adapter/session 后，才可放宽此策略。

纯函数 `SplitFilterForPushdown(expression, accepted_node_ids, output, error)` 先验证 bound AST 的 node id 唯一性、
基本 arity 和 boolean 谓词类型，再递归展平根部 AND 骨架；遇到 OR、NOT 或其他谓词时停止拆分，将整个子树
作为一个候选。provider 返回的 accepted IDs 必须是候选的无重复子集，planner 按原候选顺序规范化结果；
重复、原树未知、存在于原树但不是候选的 ID 分别报错并清空输出。空接受复用原树作为 residual，全接受复用
原树作为 pushed；部分接受从原 AND 骨架投影 pushed/residual，只在两侧均保留时复制对应 AND 节点，复用
原 node id 和不可变子树，不生成新 ID。该拆分不调用 provider、不序列化 JSON，也不修改 channel/operator。

为支持 operator 输出后的 `WHERE`，新 IID `IBlockTransformOperatorV1` 是无状态 provider；它以
`CreateTask(config)` 为每个任务创建独占 `IBlockTransformTaskV1`，并由同一 provider `ReleaseTask()`，避免
共享任务状态和跨 `.so` 释放。provider 返回前必须复制 task id、WITH 参数和 pushed plan，不能保留调用方
指针。task session 契约为：

- `Open(input_schema, output_schema)` 恰好一次，校验输入并在首批数据前返回输出 Schema；失败后仅允许取消/
  释放。
- `ProcessBlock(input, ts, outputs)` 每批一次，向调用方空容器输出零/一/多个 batch；返回 continue、stop 或
  负错误，错误时不得夹带输出。
- runtime 排空本次全部输出前不得再次调用该 session，以调用边界传播背压；输出保持 Arrow 数据面。
- 正常 EOF 或 stop 后 `Flush(outputs)` 恰好一次，可产生零/多个输出；取消或错误后不调用 Flush。
- `Cancel()` 可并发调用并及时唤醒阻塞调用；所有调用结束后才可 `ReleaseTask()`。

单 transform runner 由调用方显式提供 source Schema、独占 task 和同步 output consumer，按 source residual
Filter → transform → transform residual Filter → consumer 顺序运行。`BlockPollEvent` 不携带时间戳，因此
输入 transform 的 `ts_ms` 固定为 0；transform 输出时间戳经后置 Filter 原样交付。单次 Process/Flush 返回的
全部输出必须同步过滤并消费完成后才能继续，当前 source data block 在此期间保持存活，并在成功、stop、取消或
错误路径统一 exactly-once `ReleaseBlock()`。正常 EOF 和 `kStop` 分别以 completed/stopped 终态恰好 Flush
一次；取消、Poll/release/filter/transform/consumer 错误不 Flush。错误返回夹带输出、成功返回空 batch 均视为
transform 契约违规，不能交付 consumer；runner 不调用 provider `ReleaseTask()`。

Scheduler 首版只接入单 block source、单 transform 和 DataFrame sink。source stage 使用 `PacketSchema()` 绑定，
且因 channel 没有隔离 filter session，完整谓词作为 residual，不调用 `SetFilter()`。transform 输出 Schema 仅能
由 task `Open()` 获得；存在 operator-stage WHERE 时，Scheduler 先以相同输入 Schema/WITH 参数和空 pushed plan
创建短生命周期 Schema probe task，Open 成功后立即 Cancel 并由 provider ReleaseTask，再用所得 Schema 绑定、
协商和物化独占执行计划；执行 task 由 runner Open，并在 runner 完成全部调用后由同一 provider ReleaseTask。
相同输入 Schema/WITH 参数下 probe 与执行 task 必须返回兼容输出 Schema，runner 的 Filter Stage 在首批前再次
校验；不含 operator-stage WHERE 时跳过 probe。新 transform IID 未命中时继续使用既有终端 block operator，
不得改变其生命周期和响应语义；IID 遍历失败或同名 provider 不唯一必须明确失败，不能伪装成未命中。runner
输出只以 Arrow batch 同步追加到 DataFrame sink，具名结果继续遵守执行响应不序列化、展示时再序列化的契约。
执行 task 在 runner 所有调用结束后由同一 provider exactly-once ReleaseTask；绑定错误在 source Poll 前以
capability-check 失败，provider 协商、会话、Schema 或运行失败作为执行错误传播。

多 transform 接入保持同一 DataFrame sink 边界，并由 Scheduler 以同步组合 task 复用单 transform runner：

- Scheduler 按 SQL stage 顺序逐级发现唯一 provider。多 transform 为获得完整 Schema 链，每一级都先用空 pushed
  plan 创建 probe task，依次 `Open()` 得到本级输出 Schema，再立即 `Cancel()` 并由对应 provider
  `ReleaseTask()`；本级 WHERE 只绑定该输出 Schema，并以本级 category/name 独立协商和物化 task plan。
- 全部规划成功后才为每一级创建执行 task。组合 task `Open()` 按顺序打开各 task，并要求实际输出 Schema 与
  probe Schema 完全一致；任一阶段漂移或失败都发生在 source Poll 前。单 transform 无 WHERE 时跳过 probe 的
  既有行为不变。
- 正常数据严格按 source residual → transform 1 → stage 1 residual → ... → transform N → stage N residual →
  sink 执行。同一次上游调用产生的多个输出按原顺序同步送入下一级；调用返回 `kStop` 时，已返回输出和当前
  source block 已由上游产生的同级兄弟输入仍按原序排空，但不再 Poll 新 source block，随后进入有序 Flush。
- Flush 按 stage 0 到 stage N-1 依次调用且每级恰好一次：stage i 的 Flush 输出先通过其 residual 及全部下游
  transform/residual，再开始 stage i+1 的 Flush。因此下游 Flush 只发生在它已接收全部正常输入和全部上游
  Flush 输出之后，最终顺序保持确定。
- 任一 Process/Filter/Flush 失败时清空该组合调用尚未交付的最终输出，停止调用尚未 Flush 的后续 stage，并
  `Cancel()` 全部执行 task；失败调用夹带输出不得到达 sink。runner 完成或失败后，每个执行 task 再由创建它的
  provider exactly-once `ReleaseTask()`，所有 probe 同样 exactly-once 释放。

既有 `IBlockStreamOperator` 保持终端消费者语义。transform 无需自行过滤；框架在其输出后插入通用 Filter
Stage，实现 `IFilterPushdownV1` 仅作为可选优化。

## 主链路

1. parser 生成 source/operator stages 与各自 `StageFilter`，并拒绝未消费 token；planner 按每级输出
   Schema 完成类型绑定，将谓词拆为 pushed 与 residual，输出可诊断的物理计划。
2. runtime 以 Arrow batch 执行 Source → residual Filter → Transform → residual Filter → Sink；provider
   可在产生 batch 前执行已精确接受的谓词，但通用 Filter Stage 始终保证最终语义。

## 现状迁移边界

- 当前 `SqlStatement` 只有单个 `where_clause`，需迁移为阶段列表并保留单 WHERE 兼容。
- 当前 parser 不验证全部尾部 token，必须先补上严格结束检查，避免算子后 WHERE 被静默忽略。
- 当前 `IStreamChannel::SetFilter()` 的 JSON 注释、原始字符串调用和 unsupported 返回语义不一致；新契约
  不复用其含混行为，旧接口由适配层迁移。
- 当前 `DataFrame::Filter()` 只处理单个简单比较并逐行重建，不作为通用执行器。
- 当前 `IBlockStreamOperator` 无输出 Schema/batch；通过新 IID 增加 transform，不破坏旧插件 ABI。

## 测试锚点

- Parser：source/USING/多 THEN 各自 WHERE 正确绑定；`NOT > comparison > AND > OR`、`BETWEEN ... AND ...`、
  `NOT IN/NOT BETWEEN` 规范化、单引号转义和全部 token 消费有 AST 断言；`&`、`==`、空/重复 WHERE、空 IN、
  比较链、未闭合字符串/括号和未知尾部 token 明确失败。
- Binder：字段只能引用所在 stage Schema；boolean 根、field-field、大小写、未知字段、类型/溢出、NULL 和
  领域 resolver 成功/失败均有断言；裸 `HTTP` 不得被解释成字符串。
- Compatibility：旧单 WHERE SQL 保持兼容；多 source 的 source-stage WHERE 明确失败；数据库原生
  `WHERE/GROUP BY/HAVING/ORDER BY/LIMIT` 原样保留，operator-stage WHERE 仍生成独立 `StageFilter`。
- Arrow executor：整数、字符串、nullable、binary、fixed-size binary/list、三值逻辑、组合表达式、空批；
  packet Schema/metadata/order/sequence/raw bytes 保持不变且 block exactly-once release。
- Pushdown：不支持、全接受、部分 `AND`、拒绝 `OR/NOT` 子树、非法接受节点、provider 失败、近似结果和
  `original ≡ pushed AND residual`；下推开/关必须逐行得到完全相同结果，共享 source 不串改任务谓词。
- Block transform：输出 Schema 先于执行可得，零/一/多输出 batch、Flush、背压、取消和错误传播；source 与
  transform 后过滤均由同一 executor 兜底，旧终端 `IBlockStreamOperator` 回归不变。

## 原子任务

- `[x]` T0：冻结阶段绑定语法、类型化 AST、Arrow 兜底、精确下推/residual、任务级状态和 transform 边界。
- `[x]` T0.1：冻结过滤 EBNF、优先级、字面量/函数错误边界及多 source/数据库兼容契约。
- `[x]` T1：完成公共过滤/transform 契约和 statement 阶段化解析。
  - `[x]` T1.1：实现公共类型化 AST 与独立 expression parser，锚定优先级、规范化和全部 token 消费。
  - `[x]` T1.2：冻结 `IFilterPushdownV1`、`IBlockTransformOperatorV1` 纯虚接口和 ABI fixture。
  - `[x]` T1.3：扩展 `SqlStatement` 和 statement parser，绑定 source/operator WHERE 并保持数据库 SQL 兼容。
- `[x]` T2：实现 Schema binder 与通用 Arrow Filter Executor，覆盖类型、NULL、packet 复杂列和数据不变性。
  - `[x]` T2.1：冻结 bound AST 并实现 Schema binder，覆盖字段索引、类型、nullable、字面量转换和诊断。
  - `[x]` T2.2：实现 bound AST 的 Arrow Boolean mask 求值，覆盖比较、IN/BETWEEN、三值逻辑和空输入。
  - `[x]` T2.3：执行 RecordBatch Filter 并验证 packet binary/fixed-size list、metadata、顺序和原始字节不变。
- `[x]` T3：实现 planner 的 pushed/residual 拆分、`IFilterPushdownV1` 适配及共享 source 任务隔离。
  - `[x]` T3.1：实现顶层 AND 候选提取、accepted node 校验及 pushed/residual bound AST 纯函数重建。
  - `[x]` T3.2：实现 canonical filter plan 和基于 IID 遍历的 `IFilterPushdownV1` 规划期协商。
  - `[x]` T3.3：把精确 pushed plan 复制进独占任务会话，并验证共享 source 禁止全局串改谓词。
- `[x]` T4：接入输出型 block transform 与各阶段 Filter Stage，完成多阶段 E2E 和既有全量回归。
  - `[x]` T4.1：实现可复用 `BlockFilterStage`，冻结 Open/Schema、时间戳透传和零行 batch 非 EOF 语义。
  - `[x]` T4.2：实现 block transform pipeline runner，覆盖 source release、零/一/多输出、Flush、stop、取消和错误。
  - `[x]` T4.3：在 Scheduler 接入阶段 Schema 绑定、IID provider 发现、任务会话和 source/transform 后 residual。
  - `[x]` T4.4：完成多 transform 组合、跨模块 E2E、完整回归和 Feature 归档。
    - `[x]` T4.4a：实现多 transform 同步组合与 Scheduler 接入，完成 Scheduler 定向生命周期回归。
    - `[x]` T4.4b：完成跨模块 E2E、旧终端回归、完整 CTest 和 Feature 归档。

## 完成证据

- Parser、绑定器、Arrow mask/RecordBatch executor、pushdown planner、task session、Filter Stage、单/多
  transform runtime 与 Scheduler 接入均由对应测试锚定，旧接口 ABI 未修改。
- 真实 pcapfile 跨模块 E2E 读取两包 PCAP，经 source WHERE、两个无下推 transform 的逐级 WHERE 后，将唯一
  `protocol = 'HTTP'` 结果写入命名 DataFrame；响应不包含序列化 `data`，两个 provider 的 probe/执行 task
  均 exactly-once 释放。
- 旧 pcapfile→终端 `IBlockStreamOperator` E2E、DataFrame 原始 packet 数据、单 transform 和 Scheduler
  mutation/lifecycle 回归保持通过；多 source 的 source-stage WHERE 断言已对齐 parser 的结构化拒绝语义。
- `cmake --build build -j$(nproc)` 全量构建通过。
- `ctest --test-dir build --output-on-failure` 在允许本机 loopback 的环境中 12/12 通过，50.55 秒；受限沙箱内
  两项 socket 测试因本地监听权限失败，经同一二进制在允许 loopback 的环境重跑确认 2/2 通过。
- `git diff --check` 通过；当前环境没有 `clang-format`，本 Feature 新增源码行均不超过 120 列。

## Feature 完成出口

1. source 与每级 operator 的 WHERE 均有独立 AST、Schema 绑定和结构化错误，parser 不接受任何尾部垃圾。
2. 无 provider 下推能力时，通用 Arrow executor 仍精确过滤并保持 Schema/order/raw bytes/生命周期契约。
3. 任意下推组合与关闭下推结果完全一致，residual 不丢失，共享任务过滤互不影响。
4. 输出型 block transform 可被后置 Filter 消费，旧 channel/operator ABI 和旧 SQL 行为保持兼容。
5. 相关 parser/framework/Scheduler E2E、ABI fixture、完整 CTest 与 `git diff --check` 全部通过。
