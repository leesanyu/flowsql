# Baseliner 单表与 DataFrame 分析及输出

状态：`[x]` 已完成并归档；优先级 P1。前置：[Baseline 封装算子](feat-baseline-operator.md)。
本规格扩展已归档能力；前序 T7.3 的完成证据及未提交工作保持原样。
T1～T6 已交付三种输入、WITH model_output、INTO 指定结果出口与两出口完成保证，并完成回归、成本和使用文档验收。

## Non-Goals

- 不改 Value/Ratio/Relation 算法、既有评估/预测结果 Schema、forecast 或旧托管模型/结果原子发布协议。
- 不提供 DataFrame 持续轮询、自动跟踪 CSV 更新、外存排序、JOIN 或任意 SQL 映射。
- 不猜测时间、指标、身份、去重键、时区或日历；不增加专用 Web 配置页面。
- 不扩展 SQL Parser 或 INTO 语法；沿用单一 INTO 和现有 WITH key=value 参数语法。
- 新增输出仅覆盖有限 snapshot 分析；不提供模型版本历史导出、模型加载推理或跨通道分布式事务。
- 不为 Relation fusion/维护记录增加新出口；完整副输出继续使用旧数据库托管路径。

## 业务意图

测试数据库单张表和上传 CSV 的离线分析用户，能够直接用 FROM 选择数据，并在 WITH 内声明一个或少数
基线指标，无需为单表任务发布具名配置或重复填写来源、多数据集外层；交付结果与原有聚合/算法一致。
用户还可通过 WITH 指定学习后模型参数的位置，通过 INTO 指定评估/预测结果的位置，两类产物可分别
查询且不会混入来源；输入与两种输出的位置独立选择，原数据库托管与恢复能力保持兼容。

## 输入与配置契约

| 来源 | 数据选择 | 完成/一致性证据 |
| --- | --- | --- |
| 两段数据库通道，例如 `mysql.npm` | 原有 datasets[].table/schema | 原 snapshot/poll 契约 |
| 三段来源，例如 `mysql.npm.samples` | 唯一 dataset；table 绑定第三段，显式 table 必须一致 | snapshot 原有限范围/一致性；poll 仍需真实发布进度 |
| `dataframe.samples` | 唯一 dataset，任务启动时固定一份快照，无 table/schema | 仅 snapshot，真实 EOF 关闭尾桶 |

三段来源支持 SQLite/MySQL/PostgreSQL/ClickHouse，连接租约仍绑定前两段；不接受其他类别的三段来源。
SQL FROM 与完整配置 source 必须精确相等；三段输入不能经配置读取另一张表。
DataFrame 可省略 scope，默认整份快照；显式半开桶范围、filter、字段类型、身份/去重与指标映射仍有效。
DataFrame 一致性标识为 `dataframe_snapshot`，不能替代数据库快照或发布进度；不接受 NPM run scope。
CSV 沿用现有上传/导入通道，无需额外落库；时间列使用显式单位的整数，不猜测字符串日期。

### 单表 WITH 配置

采用既有 parameters JSON，以单数 `dataset` 区别完整 `datasets` 格式；简写只接受三段数据库或 DataFrame。
FROM 提供 source，默认 schema_version=1、mode=snapshot、dataset.id="source"；数据库 table 来自 FROM。
clock/calendar/task_key、fields、bucket、series_keys、deduplicate、metrics 继续显式提供；metrics 数组
允许一个或少数 Value/Ratio/Relation 指标。其余可选策略与默认值沿用原契约。
简写和完整格式、config 和 parameters 混用拒绝；显式 source/table 与 FROM 冲突拒绝。
简写按固定序列化规则归一为 TaskConfig；完整 JSON/具名配置原文、hash、版本语义保持。

```sql
SELECT * FROM mysql.npm.samples
USING explore.baseliner WITH parameters='<下面的单表 JSON>'
INTO mysql.baseline

SELECT * FROM dataframe.samples
USING explore.baseliner WITH parameters='<下面的单表 JSON，删除 scope>'
INTO mysql.baseline
```

```json
{
  "task_key": "samples-baseline",
  "clock": {"bucket_seconds": 60, "timezone": "Asia/Shanghai"},
  "calendar": {"calendar_id": "cn-holiday", "calendar_version": "2026.1"},
  "dataset": {
    "fields": {"bucket": "int64", "link": "utf8", "bytes": "float64"},
    "scope": {"begin_bucket": 0, "end_bucket": 120, "consistency": "consistent_snapshot"},
    "series_keys": ["link"],
    "deduplicate": {"keys": ["bucket", "link"], "on_duplicate": "require_equal"},
    "bucket": {"column": "bucket", "unit": "bucket_id"},
    "metrics": [{"id": "bytes", "kind": "value", "column": "bytes", "aggregate": "sum",
                 "feature_type": "value_basic", "profile": "default"}]
  }
}
```

### 核心结构与接口

复用 TaskConfig/Dataset/ConfigSnapshot；增加来源关系名和 DataFrame 标记，不改原虚表或版本化 struct 布局。
契约库新增纯函数，失败不修改 output，不打开 I/O：

```cpp
ConfigStatus NormalizeInlineConfig(std::string_view json, std::string_view sql_source,
                                   ConfigSnapshot* output);
```

SQL 接线采用独立可选 provider 配置归一化能力，将包含 FROM 绑定的规范 WITH 交给原 V2 CreateTask。
T2 冻结 [IBlockTransformSourceConfigProviderV1](../../src/framework/interfaces/iblock_transform_source_config.h)：
`NormalizeSourceConfig(with_json, exact_source, output, error)` 不创建任务、不打开来源/目标 I/O，
失败保持 output，成功返回拥有型规范 WITH；现有完整 JSON/精确 config 引用保留原文和版本语义。
声明该能力的数据库任务同时提供原数据库输入接口；普通算子不因三段来源被切换到 block 输入。
DataFrame 输入采用独立可选 task 能力，绑定共享通道租约和 exact_source，返回 task 自有 reader/Schema；
创建、取消、归还、释放顺序与数据库输入一致。T3 冻结独立可选
[IBlockTransformDataFrameInputTaskV1](../../src/framework/interfaces/iblock_transform_dataframe_input.h)：
Binding/Input 均验证 struct_size/version=1；绑定共享来源租约和 exact_source，一次固定 Arrow 快照，
成功返回 task 自有 input/Schema 与拥有型来源指纹，失败清空输出；旧虚表不改。
同一公共头中的独立 `IBlockTransformDataFrameInputProviderV1::SupportsDataFrameInput()` 显式声明输入能力。
快照指纹作为来源 epoch，在引擎恢复前与已保存进度校验；初始位置不确认已处理桶，
真实 EOF 和尾桶释放后才发布最终完成位置；仅 DataFrame adapter 声明进度能力。
Scheduler 仅对声明能力的 provider 启用关系/DataFrame 路由，其他算子的普通查询路径保持。

DataFrame reader 保留快照 Arrow 所有权，校验字段并按逻辑类型安全转换；支持整数无损扩宽和
float32→float64，溢出或不支持类型失败。按时间和稳定键排序，按 page_rows 输出原始页，
复用 BucketAggregator→observation→分析引擎。排序/转换/待输出内存受预算限制，超限失败。
原通道内容、Schema、顺序保持；取消不成功 Flush。托管恢复需稳定内容/Schema 指纹作为来源证据，
同内容重放不重复学习，内容变化不能仅凭相同通道名静默恢复。

## 输出契约（T4/T5 已实施）

### SQL 与配置归属

`model_output` 为 baseliner 专属、可选的顶层 WITH 字符串参数，与 config 或 parameters 并列；
二者仍恰选一个，来源归一必须保留 model_output。该执行选项与 TaskConfig 分离，不写入算法配置，
不改变精确 config 内容/版本、规范 parameters 内容或配置 hash；更换模型出口不需发布新配置。

| 位置 | 含义 | 目标形式 |
| --- | --- | --- |
| FROM | 输入数据，沿用上述契约 | 两段数据库、三段数据库表或 DataFrame |
| WITH model_output | 学习后的模型参数快照 | 三段数据库表或 `dataframe.<name>` |
| INTO | 单一主要结果，含 evaluation 与启用的 forecast | 三段数据库表或 DataFrame；兼容旧两段数据库托管 |

新增出口的数据库后端为 SQLite/MySQL/PostgreSQL；ClickHouse 保持仅作来源，不扩大事务保证。
以下 DataFrame、旧托管和三段 INTO 指定表用法均已交付；
原有无 model_output、INTO 两段数据库的 SQL 继续有效：

```sql
SELECT * FROM dataframe.samples
USING explore.baseliner WITH parameters='<上面的单表 JSON，删除 scope>',
     model_output='dataframe.baseline_models'
INTO dataframe.baseline_results

SELECT * FROM mysql.npm.samples
USING explore.baseliner WITH parameters='<上面的单表 JSON>',
     model_output='mysql.analysis.baseline_models'
INTO mysql.analysis.baseline_results

SELECT * FROM mysql.npm
USING explore.baseliner WITH config='config.link_baseline@1',
     model_output='dataframe.baseline_models'
INTO dataframe.baseline_results
```

### 模型参数 Schema 与时间依据

新增版本化 `model_parameters` Schema，每个 dataset/metric/series/source_epoch 输出一份最终状态。
不同模型采用共同身份列和版本化参数 JSON，不能用任务配置或完整 checkpoint 冒充模型参数。

| 字段 | 类型与含义 |
| --- | --- |
| task_key、dataset_id、metric_id、source_epoch、config_hash | 非空 utf8，复用结果身份与配置依据 |
| series_key | 非空 binary，沿用结果序列身份编码 |
| model_kind、model_basis_id | 非空 utf8，Value/Ratio/Relation 类型与该最终模型的依据标识 |
| as_of_bucket | int64，可空；快照截至的已处理桶，不冒充最后纳入学习的桶 |
| status、maturity | int32 非空 / utf8 可空，算法状态与成熟度 |
| parameters_version、parameters_json | uint32 非空 / utf8 可空，按模型类型解释的参数版本与实际学习参数 |

有限任务正常 EOF 后导出最终模型；未训练序列保留状态、参数为 NULL，无序列时输出保留 Schema 的空表。
导出必须只读，不能再次提交输入、推进学习或改变模型状态。
Value/Ratio 参数覆盖实际生效的模型分量；Relation 参数包含 basis 及 routed 状态的明确身份和版本。
未来预测仍由 forecast.horizon_buckets 控制；最终模型并非每条历史结果使用的模型。结果保留
model_basis_id，最终参数不补造历史版本，也不承诺仅靠最终参数重现全部历史预测。

### 输出、持久化与失败

T5 冻结独立可选 [结果出口接口](../../src/framework/interfaces/iblock_transform_result_output.h)：provider 显式声明能力；
task 在输入/Open 前绑定 exact target 与指定表的共享数据库租约，Binding 严格校验 size/version=1；旧 V1/V2/managed 虚表保持。
DataFrame 正常 EOF 后由 Scheduler 确认登记，results_output 与 model_output 各报告 target/status/rows_written；
状态为 pending/staged/committed/failed/cancelled/unknown，只有确认发布的行计数，取消不掩盖此前已提交产物。
MySQL 新表显式使用 InnoDB，已有非事务表拒绝；同实际数据库经独立通道别名绑定的两表仍共享一次发布事务。

- 未指定 model_output 时不额外导出模型；指定后模型出口失败必须使任务失败，不能静默忽略。
- 三段 INTO 写入用户指定表，沿用 results Schema 和逻辑键；evaluation/forecast 以 result_kind 区分。
  Relation routed 评估/预测属于 results；融合与维护的不同 Schema 不混写该表或 DataFrame。
- 数据库表不存在时按声明 Schema 创建，已存在则校验兼容并按逻辑键幂等写入，不清空用户表；
  模型逻辑键为 task/dataset/metric/series/source_epoch/config_hash/model_basis_id。
- DataFrame 产物保留独立 Schema，正常完成才发布为成功输出；同名发布沿用既有通道规则，
  失败/取消不能将半份产物当作成功模型。输出缓冲沿用 read_policy.max_pending_bytes，
  参数序列化沿用 persistence.max_checkpoint_bytes 上限；复用预算不代表启用恢复，超限失败。
- 两种输出可独立选择数据库或 DataFrame；执行前校验目标及 Schema，拒绝模型/结果同目标和覆盖任何来源。
  新指定表模式下，同一数据库的两份输出在同一事务内完成一次发布；跨数据库或数据库与 DataFrame 混合不保证原子提交，
  任务成功须两出口都完成，失败返回各出口完成状态与已写行数，不宣称撤销已提交数据。
- 参数快照不替代完整 checkpoint。新指定表/DataFrame 出口不自动提供重启恢复；需要联合恢复时
  继续使用旧两段数据库托管出口，其模型/进度/结果 generation 协议保持；附加参数导出遵循上述失败语义。
  旧托管附加导出不撤销已发布的 generation，其完成状态单独报告。
- 新 model_output 与三段结果表仅接受 snapshot；原无新增参数的数据库 poll 行为保持。

### 接口与主结果路由

WITH 白名单扩展由 baseliner 解释，不将模型出口写入 SqlStatement.dest 或通用 Parser。
T4 冻结独立可选 [IBaselineModelParametersV1](../../src/framework/interfaces/ibaseline_model_parameters.h) 和
[IBlockTransformModelOutputTaskV1](../../src/framework/interfaces/iblock_transform_model_output.h)：前者只读导出拥有型参数，
后者按 size/version=1 绑定共享目标、暂存/提交模型并确认 DataFrame 发布；既有虚表/版本化 struct 保持。
task 通过公共通道/租约接口持有目标到写入结束，并先于插件卸载销毁模型 Arrow 产物。
Scheduler 按目标形式区分旧两段托管与三段结果表；新指定表/DataFrame 结果出口只接主要 results，
不能向单 Schema DataFrame 追加模型/fusion/maintenance batch。旧两段托管继续处理完整多 Schema 产物，
不受新结果出口的过滤影响。新结果表路由仅对声明该能力的 provider 启用，普通 SQL/其他算子保持原路径。

### 两段数据库输入兼容性

本次静态核查确认新旧功能共用 WITH 解析、来源归一、异步策略和输出路由；兼容性须由分支与回归保证，
不能仅依据 SQL Parser 不变推断无影响。输入形态和输出模式分别判断，不因两段来源强制单表或改变存储。

| 两段来源的使用方式 | 必须保持或交付的行为 |
| --- | --- |
| 原 config/完整 parameters，无 model_output，INTO 两段数据库 | 多 dataset、多指标、snapshot/poll、完整托管输出和联合恢复保持 |
| snapshot，附加 model_output | 支持完整配置的多个 dataset，按 dataset/metric/series 导出；原结果出口语义保持 |
| snapshot，主动改用三段结果表或 DataFrame | 使用新结果出口契约；来源仍可多 dataset，不隐式承诺旧托管恢复 |
| poll，附加 model_output 或三段结果表 | 仅拒绝这组新出口组合；无新出口的原 poll 继续运行并要求异步 |

WITH 解析/创建、NormalizeSourceConfig 与 RequiresAsyncExecution 必须接受同一组选项；完整配置原文、
精确引用、config_hash 与 RestoreCompatibilityHash 保持。两段来源仍显式配置 datasets[].table/schema，
单数 dataset 简写仍只适用于三段来源/DataFrame；不得把其唯一 dataset、默认模式规则应用到旧完整配置。
不选择新出口时，不新增最终参数遍历、序列化或写入，不改变旧 checkpoint/发布频率、TTL、租约和预算。
来源覆盖检查以实际表/DataFrame 为单位，允许输入与输出位于同一数据库的不同表，不能按数据库通道整体拒绝。
核查依据：现有来源匹配区分两段/三段且多 dataset 限制仅适用于后者；旧目标按 INTO 两段绑定托管存储。
共用执行入口的内联配置路径目前会重建仅含 parameters 的 WITH；新增选项须显式保留，并由异步策略一致解释。

## 主链路

1. 数据库：FROM/WITH 归一和表绑定校验 → 数据库租约与 snapshot/poll reader → 桶聚合 →
   三类评估及可选 forecast → INTO 结果输出/旧托管发布；有限 EOF 可另向 model_output 导出最终模型。
2. CSV：原导入注册 DataFrame → 归一并冻结快照 → 类型校验、过滤、排序、分页 → 同一聚合/分析链路；
   真 EOF 关闭末桶 → INTO 结果与可选 model_output 最终参数。有限任务可同步执行，数据库 poll 保持异步要求。

## Feature Tasks

- [x] T1：交付单表来源与内联配置的严格归一契约，使少量任务无需重复来源或多数据集配置且旧配置兼容。
- [x] T2：交付三段数据库来源的唯一关系读取，使测试用户直接分析指定表并保持快照/发布进度保证。
- [x] T3：交付 DataFrame 快照到生产 SQL 的基线分析，使上传 CSV 可直接分析且源数据和生命周期受控。
- [x] T4：交付可选 WITH model_output 的最终模型参数导出，使单表与 CSV 用户可独立查看学习结果且配置可复用。
- [x] T5：交付 INTO 指定结果表及两出口完成保证，使预测结果可直接存入目标表或 DataFrame 且旧托管行为兼容。
- [x] T6：交付完整回归、生产使用与成本证据，使三种输入、独立模型/结果输出的等价性、失败行为和兼容范围可复核。

T1～T6 已完成：输入、模型参数与指定结果出口、兼容/失败保证及完整回归和成本证据已验收；Feature 归档。
本规格重编号映射为旧 T5→新 T4、旧 T6→新 T5、旧 T4→新 T6；完成证据与工作台历史记录保留当时编号，
不改其他 Feature 的任务编号。各阶段历史记录保留其原切片边界。

## 验收锚点

- T1：旧完整 JSON/精确 config，三段表缺省/一致/冲突、多 dataset、非法类别、DataFrame poll/table/schema、
  混合配置、重复/未知字段、失败输出不变、简写/完整字段等价均有断言。
- T2：真实四后端 snapshot 对照；不能读取别表；真实进度来源验证 poll、缺进度拒绝；
  原普通数据库查询/其他算子路由和 source 内容/Schema 保持。
- T3：真实 CSV 导入→DataFrame→Scheduler，乱序数据与数据库结果一致；三类评估/forecast、
  范围过滤、空表/尾桶、类型转换/失败、重复/冲突、预算/取消/租约、同内容恢复/变化拒绝；
  混合 Schema 结果仍使用原数据库托管出口。
- T4：现有 Parser 解析 config/parameters 与 model_output 并列；省略/非法/重复/未知字段和 poll 拒绝；
  来源归一保留目标、配置原文/hash 不变；三类实际模型参数、版本/最终时间依据、导出前后学习状态不变、冷启动与 typed 空输出；
  SQLite/MySQL/PostgreSQL 表与 DataFrame 导出、Schema 冲突/来源覆盖拒绝、预算/取消/失败与租约寿命。
  两段多 dataset 的完整 parameters/精确 config 都可导出；解析/归一/异步策略一致，省略时不额外导出。
- T5：真实数据库三段结果表和 DataFrame 数值对照，目标不清空、逻辑键重放幂等，来源内容/Schema 保持；
  结果/模型/fusion Schema 隔离，旧两段托管与普通 SQL/其他算子兼容；同数据库事务失败无部分发布，
  混合出口失败不报成功、状态和行数可核查；旧联合恢复与模型导出失败均有生产链路断言。
  两段来源的输出模式只由 INTO 判定；同数据库不同表允许、来源表覆盖拒绝，旧托管融合/维护产物完整。
- T6：聚焦 target/CTest、完整构建/CTest、定向 Sanitizer、格式/严格编译/Diff 范围通过；
  覆盖三种输入与数据库/DataFrame 两出口组合，记录排序/转换/模型导出时间、峰值内存与夹具条件并更新文档。
  旧两段基准覆盖完整 parameters/精确 config、单/多 dataset、三类指标/forecast、四来源与三托管目标、
  snapshot/poll 异步、同库不同表、重启/重放、generation/checkpoint/源 Schema、TTL/取消与失败；
  无新出口的配置 hash/恢复兼容 hash 与基准一致，旧托管 Relation fusion/维护记录不减少，原生产用例保留。
  不宣称小夹具证明生产规模、静态检视证明新增实现已兼容或跨通道原子性。

## 完成证据

2026-10-10：T6 完成。标准配置与完整 build、单次完整 CTest 78/78（882.49s）通过；
30 组旧恢复（716.70s）、原生产 43 组与 43 次插件重载、三输入 × 四类结果/模型目标的 12 组、
三后端同库双表回滚、混合失败/取消/空结果/来源和目标保护全部通过。旧多 dataset、精确 config/完整 parameters、
poll 异步、generation/checkpoint、Relation fusion/maintenance 保持。完整基线证据在 /tmp/baseliner-t6-output/。
本轮仅新增成本测试和文档，生产实现、公共头及其他测试与完整基线 hash 一致；最终全量构建通过，无 warning/error。
新增原始数据成本保留旧四路径，增加有序/逆序宽类型、逆序窄类型、DataFrame 模型与 SQLite 双表五路径，五轮全部通过；
每轮每路径 960 条观测/5760 行结果、24 个模型身份，指定模型出口输出 24 行、最终桶 39，源快照及 task_count=0 验证。
DataFrameReader::Open 合并记录指纹/范围/排序/类型转换，模型导出与编码和目标发布分别计时；独立子进程记录峰值 RSS。
窄类型 + DataFrame 双输出总耗时中位数 162.77ms、模型导出/编码 3.89ms；SQLite 双输出 366.96ms、发布 207.85ms，
其建表/Schema/Join 31.28ms 单列。条件、五轮范围、原数据/标准观测区别及不含 Scheduler/CSV 上传/源库扫描均写入验收文档；不作为规模 SLA。
当前源码 94 个翻译单元仪器化，六项 ASan/UBSan/LSan（DataFrame/evaluation/operator/store/lifecycle/query ownership）全绿；
模型与结果 store 三真实后端参与，查询项仅执行生产二进制的专用拥有型入口，其他框架及外部依赖的普通库边界明确记录。
98 个文件整文件格式 + dataframe.cpp 本 Feature 三处修改区间、54 个实现和 11 个公共头严格编译通过；旧明确警告例外留证。
新增文档的完整 JSON、三段单表简写、DataFrame 默认快照三种配置通过真实解析器，SQL 使用及预算/排序/类型条件已补齐。
完整 CTest 的 WSL 路由异常首次失败、中断与重跑记录保留，临时路由已恢复；NET_ADMIN 移除条件明确，不证明特权 DPDK 已修复。
通用 DataFrame 条件查询/CSV 显示现象仍为隔离问题，数值验证读取 owning Arrow 快照，不扩大本 Feature 修复范围。
本轮 /tmp/baseliner-t6-final/ 保存 cost-repeat/measurements/summary、Sanitizer 原始日志与 results、format/strict/doc-check、
test-provenance、before/hash/增量 Diff、最终 verification。完整基线与更新成本各有原始来源，不拼成一次完整 CTest。
全部任务已勾选，规格移入 archive、Backlog 完成、工作台 WIP=0；未提交/推送。

2026-10-10：T5 完成。独立可选结果出口能力接通三段 INTO，指定表/DataFrame 只接 results（evaluation/forecast/Relation routed），
旧两段托管继续发布完整多 Schema 产品及 generation/checkpoint。三真实目标后端创建/校验 Schema、逻辑键重放幂等并保留其他记录；
来源/模型/结果覆盖校验使用实际数据库身份，来源 Schema/内容保持。MySQL 新表显式 InnoDB，已有非事务表拒绝。
同实际数据库经不同通道别名绑定的模型/结果共用会话与一次事务，三后端第二表真实约束失败均无部分发布，既有记录保持。
跨库及 DB/DataFrame 混合失败如实报告两个出口状态和行数，不撤销已提交数据；DataFrame typed 空结果、两出口取消与登记后取消状态均有断言。
最终聚焦 13 项全部通过：11 项取自 final-tests.xml 的未受最终取消修复影响项目（含 732.05s 的 30 组旧恢复矩阵），
最新算子/生产 2 项取自 output-final.xml（0.81s/70.78s）；combined-tests.xml 与 provenance 明确记录原始来源，不称为一次完整 CTest。
生产保留原 43 组及 43 组插件重载恢复，增加三种输入 × 三 DB/DataFrame 结果/模型组合的 12 组数值/NULL/枚举对照、
三后端同库别名双表回滚、跨库及两种 DB/DataFrame 登记失败、typed 空表、来源覆盖与旧托管 fusion 完整性断言。
中间失败日志/XML 保留：独立服务对照夹具、DataFrame 通用查询路径、空表 Query 辅助函数均有核查记录；不将失败记录改为通过。
DataFrame 数值验收直接读取 owning channel 的 Arrow 快照；通用 DataFrame 条件查询及既有 CSV 展示现象已隔离到工作台，不扩大本任务范围。
对应 targets 构建复核无 warning/error；10 个修改 C++ 格式、4 份实现和 1 个独立公共头严格编译、Diff/hash 范围核验通过。
尺寸复检：完成证据前 210 个非空行、6 个一级任务，仍是来源归一→基线分析→两出口的统一主链路，保留同一 Feature，不机械拆分。
证据 `/tmp/baseliner-t5-output/`，说明见 [baseliner](../../docs/baseliner.md)。WIP=0；T6 全量构建/CTest、Sanitizer、成本尚未执行；未提交/推送。

2026-10-10：T4 完成。WITH model_output 与 config/parameters 并列，保留原文、精确版本、配置及恢复 hash；
独立可选只读能力导出 Value/Ratio 实际滚动/bootstrap 分量和 Relation basis/routed 参数，正常 EOF 输出最终模型，
未训练参数 NULL、无序列 typed 空表；参数与缓冲受既有预算约束。SQLite/MySQL/PostgreSQL 模型表支持 Schema 校验、
逻辑键幂等与整批事务回滚，DataFrame 暂存后正常登记；同库不同表允许，来源/结果/托管保留关系覆盖拒绝。
已用真实不同默认数据库的 MySQL 连接复现并修复显式 schema 的来源保护遗漏；模型失败后的清理 Cancel 保留 failed，
显式取消仍报告 cancelled，已有托管 generation/checkpoint/结果保持并如实报告，不承诺跨出口回滚。
聚焦最终 9 项通过：未受最后修复影响的 6 项来自 final-first-tests.xml（含 fixture 与 629.60s 的 30 组恢复矩阵），
最终算子/存储 2 项来自 final-output-tests.xml，最后生产 1 项来自 production-final-tests.xml（33.20s）；
combined-tests.xml 与 combined-test-provenance.json 明确记录原始来源，先前失败运行保留，不称为一次完整 CTest。
生产原 43 组及 43 组插件重载恢复、双 dataset 的完整/精确配置、三类模型的三后端导出/重放、DataFrame/空 CSV、
来源 Schema/内容保持、导出只读、预算/取消/租约、失败后已提交 generation 与模型回滚均通过。
构建复核无 warning/error；21 个 C++ 格式、8 份实现及 2 个独立公共头严格编译、Diff/hash 范围核验通过。
严格编译仅对既有 Required 引用误报及 istream_channel.h 的聚合初始化诊断保留局部例外；新增实现无对应例外。
证据 `/tmp/baseliner-t4-model-output/`，使用说明见 [baseliner](../../docs/baseliner.md)；T5/T6 保持待办，未提交/推送。

2026-10-10：按用户要求将待办重排为 T4 模型输出、T5 结果输出、T6 整体验收；历史编号按上述映射解释。
核查共用解析/归一/异步策略、两段多 dataset、目标路由及托管发布，补充两段输入兼容矩阵与对应验收。
本轮仅文档调整与代码路径静态检视，不声明待实施能力已通过运行兼容验收；下述历史证据保持原样。
尺寸复检：完成证据前 202 个非空行、6 个一级任务，仍为来源归一→学习→模型/结果交付的统一主链路，
兼容约束用于同一 Feature 的验收收口，保留统一规格，不因行数机械拆分。
文档核验通过：任务/验收编号、原示例、相对链接、历史记录和增量 Diff；12 处代码/测试片段完成静态核查。
证据 `/tmp/baseliner-output-compat-spec-20261010/`；仅两份允许文档变化，工作台 WIP=0，未实施新 T4～T6。

2026-10-10：按用户要求补充 WITH model_output / 单一 INTO 的分工、有限最终模型 Schema、
checkpoint 边界和输出失败语义；登记待实施 T5/T6，T4 扩为输入与输出整体验收并后置。
本次仅补文档，不声明新增输出已实现；T1～T3 历史状态和下述验收记录保持原样。
文档核验通过：完成证据前 179 个非空行、6 个一级任务，JSON/SQL 示例、相对链接及增量 Diff 检查通过。
证据 `/tmp/baseliner-output-spec-20261010/`；仅允许的三份任务文档变化，未构建/运行 CTest、未提交/推送。

2026-10-10：核查来源解析、Scheduler 路由、SnapshotReader 通道匹配及 CSV 注册链路；
确认三段数据库和原始 DataFrame 缺少输入接线，完整 WITH parameters 已存在。
按用户“先补充规格、增加任务，再实施”建立扩展；单表 parameters JSON 为当前采用方案。
T1 完成：新增 NormalizeInlineConfig 纯函数；三段数据库来源绑定唯一表，DataFrame 限制为单 dataset snapshot，
简写补齐来源/版本/模式/id，稳定序列化后复用原严格解析。旧完整 JSON 原文/hash 和精确 Config 版本保持。
测试先在三段 source 成功断言处失败（旧解析报 exact category.name source），实现后最终 CTest 1/1 通过
（0.17s，零 failures/errors/skipped）。覆盖四类别表绑定、多 dataset、非法来源、DataFrame 模式/表/schema/run scope、
简写/完整格式冲突、来源冲突、未知/重复字段、失败输出不变、顺序稳定 hash 及三类指标配置。
对应目标构建无 warning/error，三个 C++ 文件 clang-format-18 与 Diff 检查通过；776 个来源/归档文件
核验仅三个允许 C++ 文件变化，773 个前序文件保持；按起始 hash 验证复原快照并审查本次增量 Diff。
证据：`/tmp/baseliner-single-source-{red,final}-*.log`、`final-test.xml`、`final-scope.json` 和 `t1-review/`。
工作台 WIP=0；T2～T4 未实施，三段来源与 DataFrame 尚未接通生产 SQL，不声明完整 Feature 验收，未提交或推送。

2026-10-10：T2 完成。独立可选 provider 接口在 V2 创建任务前绑定 FROM 并归一单表 WITH；
三段关系只对声明该能力的 provider 接通数据库输入，其他算子的普通查询路径保持。
snapshot/poll reader 共用精确来源/连接/唯一表校验，连接租约绑定前两段；poll 发布进度、epoch、
不可变前缀及恢复契约保持，不能经配置读取另一张表。旧公共虚表及版本化 struct 未改。
四真实后端 snapshot 原始页两段/三段对照通过；生产保留原 26 组，增加四后端各三段 snapshot、poll、
单表 parameters snapshot，共 12 组，与两段数值结果逐项一致，三类指标及 forecast 均覆盖。
最终生产 38 组和插件重载后的 38 task 恢复通过，重放 rows_written=0、结果/checkpoint 与源内容/Schema 保持；
四后端拒绝跨表配置和无发布进度的有限 PCAP poll。普通 SELECT 和 builtin.passthrough 的既有 E2E 通过。
对应 targets 构建无 warning/error，定向 CTest 6/6（含目录 fixture，71.01s，零失败/错误/跳过），
十个 C++ 文件格式、新公共头独立 C++17 严格编译及 Diff/hash 范围检查通过，前序 920 个文件保持。
首次回归发现普通关系 fallback 被带入 block 终端路径，已在允许的 Scheduler 路由内修复，沿用原 E2E 断言验证。
WSL 的 127.0.0.1→loopback0 路由导致本机数据库超时；本轮仅使用临时 0600 IPv6 测试配置，未改系统路由。
最终证据 `/tmp/baseliner-single-source-t2/`：verification.json、final-regression.log/xml、
final-ctest-detail.log、final-build.log、final-scope.json、before/ 与 diff/。
WIP=0，停止在 T2；T3/T4 未实施，DataFrame 尚未接通，不声明扩展 Feature 全量验收；未提交或推送。

2026-10-10：T3 第一个切片完成，独立可选 DataFrame task 输入接口冻结；原始页 reader 已实现，
输入固定 Arrow 快照、共享来源租约及精确来源身份，安全整数转换和 float32→float64 后分页；
范围/filter 在最大 revision 选择前生效，相同最大版本保留供冲突校验，按时间/稳定键排序且源顺序保持。
指纹 `dataframe_snapshot_v1:<SHA256>` 覆盖完整原始 Schema/metadata、所有原列逻辑值及原行序，
buffer 切片偏移不影响指纹；原缓冲、排序索引、受限 Arrow 转换/页缓冲计入预算，页/列持有池所有权。
新增测试先失败，最终定向 CTest 4/4（1.06s，零失败/错误/跳过）；三类指标聚合等价、重复/尾桶、
范围/负时间/最大 revision、类型/溢出/NULL/预算拒绝、Schema/内容/未使用列变更、来源替换、
租约/列寿命及空快照/取消均通过；对应 targets、格式、严格编译及 Diff/hash 范围检查通过。
证据 `/tmp/baseliner-single-source-t3-reader/`。工作台 WIP=0；T3 仍未完成，task adapter、Scheduler、
托管恢复及真实 CSV→生产 SQL 待后续同编号切片；T4 未执行，未提交或推送。


2026-10-10：T3 完成。DataFrame task adapter 一次固定非破坏性快照，Scheduler 按独立可选 provider
能力路由，三类指标/forecast 复用原聚合和分析引擎；普通 DataFrame 算子及数据库 snapshot 路径保持。
完整快照指纹绑定来源 epoch，模型恢复前拒绝内容/Schema 变化，初始来源证据随中途 checkpoint 保存；
真实 EOF/尾桶释放后才通过零行进度 batch 发布最终位置，取消不发布成功完成。仅表头 CSV 的显式
Schema 可生成零行 Arrow 快照，空列按声明类型处理，非空字符串仍按既有严格转换契约拒绝。
新增真实乱序 CSV（40 桶、3 组、重复及过滤行）直接导入并通过生产 SQL，精确 config、完整 parameters、
单表 WITH、显式范围及三种托管目标均通过；跨目标逐个逻辑单元格与数据库输入完全一致，原始
Schema/内容/顺序保持。仅表头 CSV 无结果但持久化来源证据，单 Schema DataFrame 出口写入 240 行。
最终生产矩阵 43 组（原 38 + 数据库对照 1 + DataFrame 托管 3 + 空表 1），插件重载恢复 43 组通过；
同内容恢复 rows_written=0、结果/checkpoint 保持；内容/完整 Schema 变化拒绝且模型/结果保持。
任务层覆盖 size/version/租约/精确来源/预算失败及取消后中途恢复（测试模拟写入租约到期）；reader 与
adapter 的排序、类型、范围/filter、重复/冲突、尾桶/空表、租约/列寿命及取消断言均通过。
定向 CTest 的 8 项全部通过：最后完整选择中的 7 项通过，加最后重建后的生产 1 项通过；
combined-test.xml 是带来源记录的汇总，原始 CTest XML/日志保留，未将先前失败记录改写为通过。
目标构建复核无 warning/error，三份相关实现及公共头严格编译、12 个完整 C++ 文件及 DataFrame 修改
范围格式检查、Diff/hash 范围核验通过；前序未提交工作保持。证据 /tmp/baseliner-single-source-t3-integration/。
工作台 WIP=0，停止在 T3；T4 的完整构建/CTest、Sanitizer、成本及使用文档验收未执行；未提交或推送。
