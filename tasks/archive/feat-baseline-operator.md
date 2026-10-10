# Feature: Baseline 封装算子 (`baseline-operator`)

状态：`[x]` 已完成；T1～T7 全部验收通过，2026-10-10 归档，工作台 WIP=0。
优先级：P1；SQL 算子标识确定为 `explore.baseliner`，产品简称 `baseliner`。
`explore` 统一归属对数据进行探索性分析的算子；采用动词表达操作意图，保留简短类别名，不影响 Baseline 算法插件命名。
前置：[Baseline 检视修复](../archive/feat-baseline-review-fixes.md)、数据库通道、Config Channel、流式时间驱动。
T1 已交付并冻结公共头文件、严格配置 Schema 和可执行测试；后续实现按这些契约执行。

## Non-Goals

- 不修改 Value/Ratio/Relation 检测算法，不在算法插件内连接数据库、Scheduler、定时器或后台线程。
- 不支持 `INTO ... USING ...` 新语法、隐式双 sink、落库自动触发任务、CDC 平台或跨数据库分布式事务。
- 不扫描所有表自动建模，不猜测指标业务含义，不自动 JOIN、迁移源表或跟随最新 Schema/配置版本。
- 不提供任意 SQL/脚本/UDF 映射、历史修订重算、模型跨不兼容版本自动迁移或外部 HTTP 并发查询在线 task。
- 不默认启用 TTL，不用处理耗时淘汰离线身份，不创建新的通用模型平台或本 Feature 专用配置编辑页面。
- 不向 NPM 原始分析表追加预测列、回写原行或改变其 Schema；不新增任意 Relation 全分布预测算法。
- NPM 输入不提供拆桶、插值或非整数倍重采样；Baseline 桶宽限定为 NPM 周期的正整数倍。

## 业务意图与范围

主要服务已落库的 NPM 流量分析数据；用户通过具名配置明确表、字段、业务对象和时间桶，获得持续更新的基线，
以及可查询的实际值/预期值/上下条带/偏离结果，并按配置获得未来预测；同一映射契约可用于普通业务表。
baseliner 对上游是数据消费者，对看板、异常分析及后续探索是结果生产者；训练模型是内部过程，分析结果是必交付价值。
交付数据库来源适配、通用封装算子和独立结果表，模型/消费位置/结果一致发布；算法插件保持被动调用。

首版读取 SQLite/MySQL/PostgreSQL/ClickHouse；模型与结果托管目标支持 SQLite/MySQL/PostgreSQL。
ClickHouse 可作数据源，首版不能作要求原子提交和条件更新的模型目标；Open 明确拒绝，不弱化恢复保证。
保留已有 Arrow block 接入场景：输入必须满足同一标准观测 Schema 和显式进度契约，不能将 `npm.basic`
的单一 observing 批次当成数据库全部实体。`npm.basic` 落库与 baseliner 是两个独立 Scheduler 任务。

## 立项时事实与交付范围

| 已核实能力 | 本 Feature 必须补齐 |
| --- | --- |
| SQL 使用 `USING category.name ... THEN ... INTO ...`；托管 block sink 仅允许单算子。 | 数据库来源到 V2 block transform 的适配；不靠追加第二个 USING 接线。 |
| NPM 已有 `npm_result_entities`，含实体/版本、规范 Schema/指纹、身份/revision 列和三类关系名。 | 可作为显式关系的可选校验辅助；当前目录不提供持续读取的已提交位置和桶关闭证明。 |
| 数据库已有 `/channels/database/tables`、`describe` 控制路由及拥有型通道租约。 | 普通表/视图结构核验、受限查询计划和 task 私有 reader；浏览路由不能充当增量消费契约。 |
| `IDatabaseChannel` 尚无 task 独占事务/原子条件提交公共能力。 | 目标侧新增独立版本化能力，冻结回滚、未知提交、超时/取消和会话所有权，保持旧虚表。 |
| Baseline 已有 artifact/seed 导出、artifact 导入和独立状态控制 IID。 | 包含在线学习状态的 checkpoint/restore；观测 snapshot JSON 不能直接作为恢复格式。 |
| Submit 返回当前桶条带与偏离；PredictRolling/PredictRoutedSummary 提供只读未来桶预测。 | 明确预测时点、有效性、独立结果 Schema 和持久发布，不只保存模型。 |

## 数据来源与显式表名契约

`mysql.npm` 是数据库通道，实际 database/schema 来自通道配置；`mysql.npm.<relation>` 才是单个查询关系。
数据库任务配置通过 `datasets[].table` 显式指定表或视图，`schema` 可选，缺省使用通道配置的默认 schema。
NPM 与普通业务数据共用该入口，例如 `npm_basic_history_v1`；不根据 entity_id 推导关系名，也不自动选择最新版本。
`fields` 显式声明所有所用字段的预期逻辑类型，其他节点指定字段映射和读取范围；Open 只核验配置选定的关系并冻结结构。
表不存在、缺字段、类型/单位不兼容时失败；增加未使用列兼容。数据库浏览/实体目录仅为可选的发现与校验辅助，目录不作为读取前置。

支持一个配置的有限 `datasets[]` 选择同一数据库通道中的多张表；每个 dataset 的多个指标可共享读取批次。
不跨 dataset 自动合并、JOIN 或碰撞身份；series/source 身份由数据命名空间、dataset、指标及有类型的业务键编码组成。
模型跨 run 复用必须配置稳定业务键；`run_id` 是读取过滤条件，不默认作为模型身份，`session_id` 不默认代表服务。
NPM `history` 包含多次 revision，`latest` 只选每个会话最新 revision，`final` 只含会话终态；
会话终态不等于所有时间桶已关闭，`writing/incomplete` 不冒充完整数据。

### 快照与持续读取

- `snapshot`：冻结有限 run/时间范围与一致读取边界；NPM 默认只接受 completed、未过期/未清理的 run。
  普通表必须提供一致快照或不可变范围。耗尽才是正常 EOF；空范围可成功结束且不伪造模型。
- `poll`：异步持续任务；要求源适配器提供 `epoch + committed_position + closed_before_bucket`。
  position 指向按已提交顺序可恢复读取的不可变发布记录；所有 bucket 小于 closed_before_bucket 的相关行已发布且不再修订。
  自增 ID、MAX(timestamp)、轮询为空、固定等待时间均不能自行证明提交完整或桶关闭。
- NPM poll 的新增前置纳入 T3.1/T3.2：托管写入发布可恢复提交位置及来自真实采集进度的桶关闭事实；
  发布标记须晚于相关实体写入成功。仅有旧目录/视图时拒绝 poll，不能宣称现有 NPM 已支持此保证。
- 按冻结的提交上界分页读取，同一桶的分页/revision 汇齐后才提交观测；记录待关闭桶、复合分页位置和进度。
  超过 pending 上限或源保留期使所需数据消失时明确失败，不跳到“最新”；轮询空批次返回 timeout，不返回 EOF。
- 每个身份按 bucket 单调提交；关闭后出现修订/迟到数据失败并定位源行，不重训旧桶；缺桶默认保留缺失，
  只有配置显式要求且源已证明区间完整时才补零。bootstrap 与在线检测采用不重叠的半开时间范围。

## 任务配置与映射契约

仅接受二选一的 `WITH config='config.<name>@<revision>'` 或 `WITH parameters='<JSON>'`；
Config Channel 首版内容为 JSON。Open 前复制并冻结原文、精确 revision、hash 和有效算法配置；后续发布不影响在途任务。
配置版本固定 `schema_version=1`，未知字段、重复键、混用入口、类型错误在源读取/目标写入之前失败；不接受 latest 引用。

| 配置节点 | 最小内容 |
| --- | --- |
| `task_key/source/mode` | 持久任务键、精确数据库通道或 Arrow 输入、snapshot/poll；SQL FROM 与配置 source 必须一致。 |
| `datasets[]` | 唯一 ID、显式 table/可选 schema、fields 预期逻辑类型、run/时间范围、identity/revision 去重规则、series/source 键。 |
| `clock/bucket/calendar` | 时间列、ns/us/ms/s 单位或既有 bucket_id、固定 bucket_seconds、时区、日历精确版本；桶为 UTC epoch 对齐的半开区间。 |
| `metrics[]` | 唯一指标 ID、kind、字段/聚合/缩放、feature_type/profile、日历精确版本；Relation 另含有版本的 group space。 |
| `bootstrap` | cold 或 history；history 指定训练窗、最低数据量及不足时 fail/cold，训练截止之后才输出检测结果。 |
| `forecast` | horizon_buckets：0 禁用，正整数表示每次合法消费后预测未来桶数，不超过 T1 冻结的有限上限；缺省为 0。 |
| `read_policy/state_policy` | 分页/轮询和 pending 字节限额；runtime/model/版本额度、容量策略、可选在线 idle_timeout 和维护预算。 |
| `persistence` | checkpoint 数据/时间提交间隔、writer 租约/续期间隔、大小/保留版本限额、恢复 require/if_exists/fresh；唯一 SQL INTO 目标。 |

首版映射仅提供列引用、sum/count/mean/min/max、数值 scale 与固定 divisor；谓词是类型化字段比较，值绑定、标识符引用。
Value 提供 value 和必要的 sample_count；Ratio 分别聚合 numerator/denominator 后交由算法解释，不能取比例的平均；
Relation 按 source×bucket 汇齐固定 group_idx 的各 metric，metrics 数组按算法配置同序对齐。
数值窄化/溢出、NaN/Inf、NULL 依指标配置 fail/skip；skip 有可计数原因，不推进学习或活动时间。
累计量不能把每次 revision 当新增量；NPM 周期统计使用 interval_*，部分周期显式 include/exclude；累计字段差分不在首版映射范围。
周期增量行按 run/session/revision 独立消费，不按 session/period 取最大 revision；零增量终态不替代周期行、不参与 sample_count，也不重开旧桶。
group_idx 必须来自配置的稳定字典/已有数值键，未知组只允许明确的 other 或拒绝，不能每批重新编号。

NPM 时间映射仅支持以下契约，普通业务表仍按显式时间列映射：

- 来源周期为 P，Baseline 目标周期为 B；统一时间单位后须满足 P>0、B>=P、B%P=0，倍数 m=B/P。
  两者沿用 UTC epoch 对齐；P 由冻结的来源契约及规范 period_start_ns/period_end_ns 核验，不从相邻记录间隔推断。
- m=1 为常见路径，沿用来源桶边界，仅执行必要的业务维度汇总；m>1 将目标桶内 m 个来源周期的增量流式汇总。
  目标 bucket_id=floor(period_start_ns/B)；m 是时间宽度倍数，不是收到的行数，缺行不自动补零。
- 目标桶内跨页、跨来源周期的相关行汇齐后只生成一次身份/指标观测；poll 的来源关闭进度须覆盖目标桶右边界。
  Value/Ratio/Relation 沿用既有指标汇总口径；桶宽在任务中冻结，非法倍数或来源周期不兼容时明确失败。
- 数据库按行数/字节预算分页，标准 Arrow 观测批次允许包含多个已关闭目标桶；批次边界不代表桶关闭或持久提交。
  同一算法 task 串行逐条 Submit，每个桶先评估、再按策略学习并生成启用的未来预测，随后处理下一桶。
  在线交付不等待凑满批次；结果/checkpoint 按独立配置阈值发布；同一输入的分页与批次划分不改变观测和算法结果。

以下是已开启 60 秒周期统计的 completed NPM run 的配置示例；运行标识替换为实际分析响应：
```json
{
  "schema_version": 1, "task_key": "link-byte-baseline",
  "source": "mysql.npm", "mode": "snapshot",
  "datasets": [{
    "id": "link",
    "table": "npm_basic_history_v1",
    "fields": {"__npm_run_id": "utf8", "session_id": "uint64", "revision": "uint64",
               "observation_domain_id": "uint64", "period_start_ns": "int64", "period_end_ns": "int64",
               "period_complete": "boolean", "interval_wire_bytes_ab": "uint64"},
    "scope": {"run_ids": ["completed-run-id"]},
    "series_keys": ["observation_domain_id"],
    "row_semantics": "npm_period_increment",
    "deduplicate": {"keys": ["__npm_run_id", "session_id", "revision"], "on_duplicate": "require_equal"},
    "bucket": {"column": "period_start_ns", "unit": "ns"},
    "filter": [{"column": "period_complete", "op": "eq", "value": true}],
    "metrics": [{"id": "bytes_ab", "kind": "value", "column": "interval_wire_bytes_ab",
                 "aggregate": "sum", "null_policy": "skip", "feature_type": "value_basic", "profile": "default"}]
  }],
  "clock": {"bucket_seconds": 60, "timezone": "Asia/Shanghai"},
  "calendar": {"calendar_id": "cn-holiday", "calendar_version": "2026.1"},
  "bootstrap": {"mode": "cold"},
  "forecast": {"horizon_buckets": 0},
  "read_policy": {"page_rows": 4096, "max_pending_bytes": 16777216},
  "state_policy": {"max_runtime_identities": 1024, "max_model_identities": 1024,
                   "max_basis_versions": 2, "capacity": "reject", "release_scope": "RuntimeOnly"},
  "persistence": {"checkpoint_every_buckets": 1, "max_checkpoint_bytes": 16777216,
                  "retain_generations": 2, "restore": "if_exists"}
}
```
例中仅统计完整周期行，表名与预期字段类型均由配置指定。idle_timeout 缺省禁用，预算/时间提交/租约采用 T1 冻结的有限默认值。

## SQL 与框架接线契约

```sql
SELECT * FROM netadapter.eth0
USING npm.basic
WITH input_namespace='netadapter.eth0', source_domains='0:1', features='basic', output_interval_ns=60000000000
INTO mysql.npm

SELECT * FROM mysql.npm
USING explore.baseliner WITH config='config.link_baseline@1'
INTO mysql.baseline
```
第一条沿用现有 NPM 落库；第二条使用本 Feature 已交付并经 T7.3 生产 SQL 验收的数据库 namespace 来源能力。
示例 snapshot 配置只读取已完成的 run；持续采集须另用满足进度契约的 poll 配置及异步 Scheduler 提交。
首版托管基线任务为单算子、SELECT *、无外层 WHERE，筛选/映射在配置中完成，避免 SQL 与配置形成两个读取计划。
不自动启动或耦合两任务；任务响应给出 task_key、执行 run_id、配置 hash、数据关系、模型 generation、结果关系和进度。

公共契约已冻结为独立版本头/IID，旧 Baseline task/service、数据库通道和 V2 transform 虚表保持：

- [数据库输入与进度](../../src/framework/interfaces/iblock_transform_database_input.h)：拥有型源租约，版本化 binding，task 私有 reader 与拥有型 Schema；归还批次后交付类型化进度。
- [独占原子目标](../../src/framework/interfaces/idatabase_atomic_target.h)：独占物理会话、参数化条件命令、精确受影响行数、目标时钟、超时/取消及三种提交结果。
- [完整 checkpoint 能力](../../src/framework/interfaces/ibaseline_checkpoint.h)：独立 service Bind，拥有算法 task 和容量控制句柄，长度/格式/版本检查；原子恢复规则在接口中冻结，状态编码在 T5 实现。
- [配置与数据 Schema](../../src/operators/baseliner/baseliner_contract.h)：严格 JSON 解析和拥有型配置快照，有限默认值/上限、标准观测/结果/进度及逻辑键；解析器与契约测试作为配置 Schema 的可执行依据。

Restore 仅用于新建空 task，在容量绑定后、首次消费前一次性成功应用；校验失败保持整个状态及恢复资格。
Scheduler 通过可选数据库输入能力创建 task 私有 block reader，随后沿用 V2 Open/ProcessBlock/OnTime/Flush；
reader 持有源租约，拥有型 Arrow 批次携带标准观测，poll 等待可取消。非能力算子保留既有数据库查询路径。
输入 reader 与算子协作的 pending/已提交位置只能串行推进；源恢复位置以成功持久发布的 generation 为准。

标准输入：dataset/metric/kind/identity 为类型化身份字段，bucket_id=int64；Value/Ratio 按既有 observation struct，
Relation 使用 list<uint32> group_idx 和有序 list<struct> metrics，不用无 Schema JSON 行传递。
batch 的版本化进度信封携带各 dataset 的 epoch/position/closed_before_bucket；允许无观测的进度更新，不视为 EOF。
标量结果统一包含 evaluation/forecast 类型、业务身份/目标桶、实际值/预期值/上下界及依据版本；Relation routed/fusion 使用有类型子关系。
维护记录单独标识 release/capacity/recovery、原因和 usage，不当作异常告警；所有输出、Schema 和文本均拥有生命周期。

## 结果生产与独立表契约

每条合法的 Value/Ratio 观测及 Relation routed summary 必须产生当前桶评估，包括冷启动/不足数据状态；不能仅导出模型。
已有模型时沿用 Submit 的顺序：以当前桶之前的模型构造检测条带 → 比较实际观测 → 按算法策略学习/降权/跳过更新。
记录更新前的数值依据；不得在学习当前值后重新预测同桶并把拟合值当预测。冷启动首条初始化及无有效模型时条带输出 NULL，附有效性状态。
合法消费后，启用 forecast 时只读预测 [t+1, t+1+horizon_buckets)；Value/Ratio 用 PredictRolling，Relation 仅覆盖已有 routed summary。
forecast 实际值为 NULL；失败/不成熟预测返回状态，不以默认 0 冒充有效数值。预测调用不学习、评分、不延长活动期限。
当前检测条带与未来预测区间沿用各自算法口径，并记录 band_kind；confidence 是可信程度，不宣称经验证的 95% 区间覆盖率。

| 结果字段组 | 最小契约 |
| --- | --- |
| 身份/时间 | task_key、dataset_id、metric_id、series_key、source_epoch、result_kind、target_bucket、桶宽/时区与单位；Relation routed 另含 summary/basis 身份。 |
| 数值/有效性 | observed、expected、lower、upper；evaluation 附偏离/评分/学习/告警状态；observed 可用时保留，无有效预测时 expected/lower/upper 为 NULL，forecast 不伪造偏离或告警。 |
| 依据/追溯 | issued_after_bucket、model_basis_id、config_hash、published_generation、算法 status/可信/成熟度；依据状态与发布后的 checkpoint 区分。 |

独立版本化物理表 baseline_results_v1 保存 evaluation/forecast；Relation fusion 和维护使用独立类型化表，模型状态另存。
结果粒度是业务对象×指标×目标桶，允许多条原始会话行聚合成一条结果，不把聚合结果复制到每条源行。
evaluation 逻辑键为 task/dataset/metric/series/source_epoch/target_bucket；forecast 再包含 issued_after_bucket；共表用 result_kind 区分，Relation routed 键另含 summary/basis 身份。
原预测记录不可被新预测或后来实际值覆盖；实际值到达时写 evaluation，查询按相同业务键/目标桶关联，允许一条实际值对应多个预测时点。
查询原始数据与基线时按相同配置先聚合再关联，不承诺单行一对一；查询/视图组合不改变原始事实表。
独立建表允许与源同库或使用另一个目标通道；源关系与托管结果/模型表须不重叠，Open 先核验，防止自读结果。
源适配器负责字段/周期/进度，算子负责分析编排，Baseline 插件负责算法，task 私有存储适配器负责表/事务/方言。
存储适配器复用公共数据库/托管 sink 接口，不让算法依赖驱动或 NPM 表名，不另建服务或通用存储平台。

## 模型持久化与恢复契约

模型包括有效参数配置、历史 artifact/seed 与持续学习的状态，不能只保存预测上下界或检测结果。

| 托管对象 | 必须保存的内容 |
| --- | --- |
| `baseline_tasks` | task_key、精确配置/hash、源/字段指纹、时钟/日历/group-space 版本、current generation、owner_run/writer_epoch/lease_deadline。 |
| `baseline_model_versions` | task_key/generation、checkpoint 格式/算法版本、校验和与完整参数状态 payload；artifact/seed、rolling/校准/消费游标、Relation basis/版本/fusion 状态。 |
| `baseline_source_positions` | 同 generation 的各 dataset epoch/读取位置/桶关闭边界、每身份已消费 bucket 与待关闭桶/分页/revision 状态。 |
| `baseline_results_v1` 及 Relation/维护关系 | 同 generation 的当前桶评估/未来预测/维护记录；独立于源表，保留稳定逻辑键、输入依据和模型版本。 |

在唯一 INTO 目标中原子发布模型、源位置、结果和 current generation；使用持久 task_key + 预期 generation 条件更新，
同批 evaluation 的条带依据为更新前模型，forecast 为消费后模型；记录精确 model_basis_id/issued_after_bucket，不能仅用提交 generation 代替数值依据。
同一 task_key 只能一个执行者写入，重复/并发启动在消费前拒绝。不把业务 task_key 等同每次变化的 Scheduler task_id。
写入租约按目标数据库时间条件获取/续期；故障后仅过期租约可接管并递增 writer_epoch，每次发布同时核验 epoch/generation，旧执行者不能提交。
原子保证必须由目标后端的独占事务及条件更新能力实现；不在共享连接上拼 BEGIN/COMMIT 假装隔离。
周期/终结 checkpoint 成功后才确认持久进度并公开该批结果；失败或提交结果未知立即终止，不自动重试。
重启重读最后确认的原子 generation；未发布输入可重新处理，已发布结果按逻辑键保持一次，不宣称跨源/目标分布式 exactly-once。
每次提交同步反压，未发布结果/pending/序列化缓冲均有限；正常 EOF 提交尾批，错误/取消不调用成功 Flush。
checkpoint 导出/恢复同 task 串行。参数/Schema/日历/group-space/源 epoch 不兼容或 payload 损坏时失败；
fresh 仅允许不存在模型的新 task_key；require 必须有兼容模型，if_exists 只在无模型时冷启动，不自动清空不兼容模型。
恢复策略是本次运行控制；仅切换 restore 不改变模型兼容指纹或固定 snapshot 范围 epoch，精确原文/hash 仍按代保存。
模型参数以版本化 payload 完整保存，可查询配置、generation、训练范围、覆盖/成熟度和校验和；观测快照是查询投影。
保留版本数和单次 payload 大小有限，清理只删除非 current 且不被恢复引用的旧 generation；目标与源同库时仍只读配置选定的关系。

## 状态、时间与生命周期

- StatePolicy 的 runtime/model/basis 限额为正，Relation 的 basis 版本额度至少为 2；capacity 显式 reject 或 evict_idle。
  没有已到期候选时拒绝新身份；先成功释放再准入，不无界重试，不使用无限身份 tombstone。
- 在线 idle_timeout 默认禁用；启用后，以成功消费观测的单调处理时间记录期限。
  预测、查询、非法/重复/已消费观测重放不续期；离线 snapshot 禁用处理时间 TTL。
- RuntimeOnly 在下一 generation 删除 runtime/活动/身份消费索引，仅保留父 artifact/seed；重入从 seed 恢复，没有 seed 则冷启动。
  AllState 同时删除该身份父模型并发布，重启不能复活；poll 源位置继续过滤旧输入，Arrow 重入须有上游进度或新 epoch。
- 跨进程不沿用旧 monotonic deadline；checkpoint 保存活动年龄，恢复时将可验证的停机时间计入不活跃时长；
  wall clock 回退或年龄不可验证时明确重置期限并报告原因，不据此推进 bucket 或造零。
- ProcessBlock/OnTime/Flush/Bootstrap/Checkpoint/Release 全部同 task 串行并建立 happens-before；
  新增维护线程 0、新增算法状态 mutex 0。外部查询只读持久关系/已发布快照。
- OnTime 按回收/checkpoint/租约续期的最早期限维护；遍历、释放、Relation 扇出和 checkpoint 字节均受预算约束；
  checkpoint 为同步有界工作，测量其最坏维护延迟，不能仅按“释放次数”宣称恒定耗时。
- Cancel 可并发，只发原子取消信号并唤醒 reader/数据库调用，不访问模型/活动容器；
  ReleaseTask 等在途回调结束后关闭算法任务及 reader，结果/Schema/句柄先于插件库销毁。

## 两条主链路

1. **创建与首次检测**：解析 SQL/config → 源/目标租约和 task_key 写入权 → 显式表/字段/进度核验 →
   新建算法任务并绑定容量 → 可选历史 bootstrap → 归一化关闭桶 → Submit 先评估再学习 → 可选未来只读预测 →
   存储适配器原子发布模型/位置/独立结果表；OnTime 维护无数据、繁忙和背压期间的期限。
2. **重启与继续消费**：精确配置和兼容指纹 → 最后原子 generation → 恢复算法及 pending/读取位置 →
   从确认位置继续 → 同一桶不重复学习、结果不重复发布 → EOF 提交尾批；
   错误/取消保留最后成功 generation 和结构化失败位置，待全部回调结束再关闭。

## Feature Task 与测试锚点

一级任务汇总一个交付阶段，直接子任务是可独立验收的结果边界；实现工作台一次只载入一个子任务中的 Atomic Slice。
每个子任务可以跨多个时间盒继续，保持编号，不把下面的结果再拆成第三层，也不把文件/函数/编译步骤写成规格任务。

- [x] T1：交付可编译且测试锚定的配置与公共契约，使输入、预测、恢复和存储在实现前具有唯一解释及兼容边界。
  - [x] T1.1：交付严格配置解析和标准观测/结果 Schema，使显式表、字段、身份与预测时点在 Open 前形成拥有型快照。
    锚点：单入口/精确 revision、未知/重复字段、类型/单位、逻辑键、NULL/状态、forecast/预算上限、配置失败无副作用。
  - [x] T1.2：交付版本化数据库输入、独占原子目标和 checkpoint 接口，使调用方能按明确所有权接入且不破坏旧 ABI。
    锚点：公共头编译、IID/版本/Bind、租约/取消/回滚/未知提交、恢复空 task 限制；此项不实现后端或状态编码。

- [x] T2：交付有限数据库范围到标准观测的接入，使已完成 NPM run 或普通表能按冻结结构正确聚合并结束。
  - [x] T2.1：交付配置选定关系的有限 reader 与 SQL 接线，使四种源后端都能有界读取一致范围并正确区分空结果/EOF/错误。
    锚点：表/视图/schema/字段核验、源租约、分页/取消、uint64 无损、snapshot 不启用处理时间 TTL；不包含 poll。
  - [x] T2.2：交付 Value/Ratio 桶聚合和 revision 解释，使多行数据得到稳定业务序列且周期增量不被累计/终态误计。
    锚点：NPM interval/零增量终态、m=1/m>1/非法倍数、跨页合桶/批次等价、目标桶关闭、缺桶、NULL/溢出、Ratio 独立聚合、dataset 隔离。
  - [x] T2.3：交付有版本 group space 的 Relation 观测，使 source×bucket 的完整分组与 metric 顺序准确对齐算法输入。
    锚点：稳定 group_idx/other、重复组、跨页/跨来源周期汇齐、metric 缺失/错序、group/pending 预算；不执行预测算法。

- [x] T3：交付有事实依据的持续读取，使运行中的 NPM 数据只在已提交且桶完整时进入基线，不因空轮询或背压丢失进度。
  - [x] T3.1：交付 NPM 已归还输入与已排空实体对应的安全发布进度，使桶关闭事实不早于全部相关分析结果。
    锚点：多输入共同进度、backlog present/unknown、空闲确认、繁忙/背压、部分尾桶/错误；先用测试消费者验收，不含数据库发布。
  - [x] T3.2：交付持久提交位置与桶关闭记录，使四种 NPM 存储后端都能在写入成功后发布可恢复的不可变前缀。
    锚点：run/epoch 隔离、记录晚于实体写入、无数据进度、失败/未知提交不推进、旧目录缺能力拒绝、源过期识别。
  - [x] T3.3：交付 task 私有 poll reader 与异步调度，使标准进度下的分页、待关闭桶和恢复位置可有界、可取消地持续推进。
    锚点：timeout 非 EOF、提交上界冻结、重复/迟到、进度信封、pending 上限、持续/离线隔离；不增加后台算法调用线程。

- [x] T4：交付当前桶评估与可选未来预测，使用户获得与直接算法调用一致的实际值/预期值/条带/偏离和依据时点。
  - [x] T4.1：交付 Value/Ratio 历史预热或冷启动后的评估，使当前桶按先评估后学习产生结果且训练范围不泄漏。
    锚点：历史不足 fail/cold、训练/检测窗分界、冷启动 NULL、异常跳过/降权、重复/非法观测不改状态。
  - [x] T4.2：交付 Relation routed/fusion 评估，使完整关系观测产生准确子结果并保留 summary/basis 身份。
    锚点：metric 顺序、routed/basis/fusion 直接调用对照、版本切换、缺失/不足数据状态；不预测完整关系分布。
  - [x] T4.3：交付显式启用的未来桶预测，使 Value/Ratio 和已有 routed summary 的多个预测时点可追溯且不修改学习状态。
    锚点：跨度/未来范围、检测/预测 band_kind、issued_after_bucket、失败/不成熟状态、只读与不续期。

- [x] T5：交付完整在线模型的原子导出/恢复，使进程重建后下一条结果与不中断运行一致且非法导入不污染状态。
  - [x] T5.1：交付 Value/Ratio 完整 checkpoint，使 artifact/seed、rolling/校准/成熟度和消费游标在容量内无损往返。
    锚点：正常/跳过学习状态、预测/下一次 Submit 对照、空/关闭任务、版本/配置/日历不兼容、损坏/超限导入不部分应用。
  - [x] T5.2：交付 Relation 完整 checkpoint，使 basis/历史版本/routed 模型/fusion 及 source 游标保持联合一致。
    锚点：多 metric/版本/handover、父/子资产、释放后状态、扇出/容量、恢复下一次 routed/fusion 结果；不含数据库写入。

- [x] T6：交付独立结果表与模型/消费位置的一致持久发布，使结果可查、源事实不改写且故障恢复不重复学习或发布。
  - [x] T6.1：交付 SQLite 的独占事务与条件更新能力，使同 task_key 的写入权、失败回滚和旧 writer 拒绝有可执行保证。
    锚点：独占会话、并发获取/租约续期/过期接管、writer_epoch/generation 条件、取消/提交失败；不绑定业务表结构。
  - [x] T6.2：交付 SQLite 的独立结果与模型存储，使评估/预测/位置/checkpoint 在一代提交内可查询且预测历史不覆盖。
    锚点：逻辑键/源表不变、多行聚合/多任务/多预测时点、模型依据与发布代分开、原子可见性、版本保留与清理。
  - [x] T6.3：交付 MySQL/PostgreSQL 同一原子目标契约，使现有两种生产目标具有与 SQLite 一致的类型、失败和隔离保证。
    锚点：非跳过真实后端往返、独占事务、初始化冲突、回滚/未知提交/旧 writer、结果查询和 current 保护；拒绝 ClickHouse 目标。
  - [x] T6.4：交付源位置/pending 与模型联合恢复，使重启能从最后成功 generation 继续且输出与连续运行一致。
    锚点：提交前后崩溃、未知提交核查、forecast/评估不重复、source epoch/保留期缺口、require/if_exists/fresh；不实现自动重试。

- [x] T7：交付时间维护、可靠关闭和 Feature 组合验收，使无数据、背压、取消与长期运行下资源及模型生命周期可管理。
  - [x] T7.1：交付有预算的容量/不活跃维护，使 RuntimeOnly/AllState 的回收与持久恢复一致且未到期身份不被误淘汰。
    锚点：模拟时间/停机恢复、禁用 TTL、reject/evict_idle、扇出/索引/usage、失败不误删、AllState 不复活。
  - [x] T7.2：交付正常终结与错误/取消/卸载的完整生命周期，使尾批只在正常结束提交且全部在途调用先于资源销毁结束。
    锚点：EOF/Stop 单次 Flush、错误/Cancel 无成功尾批、回调非重叠、reader 唤醒、预算/句柄归零和插件库 RAII。
  - [x] T7.3：交付生产 SQL、跨后端及性能的完整验收证据，使整个 Feature 在声明范围内满足完成出口。
    锚点：NPM 落库→分析→查询/重启、三类任务/两模式、四源三目标、定向 Sanitizer、全量构建/CTest/格式和同负载成本测量。

依赖：T1.1/T1.2 先冻结；T2 是 T4 的观测前置，T3 是 poll 的额外前置；T5 不依赖数据库，可用直接算法输入验收。
T6.1 依赖原子目标契约；T6.2 依赖 T4/T5 已验收类型的结果与 checkpoint，T6.3 扩展同一目标契约；T6.4 依赖相应来源/模型/存储。
执行优先收口 T2.1/T2.2 + T4.1 + T5.1 + T6.1/T6.2 的有限标量/SQLite 闭环，再补 Relation、forecast、poll 与生产目标；不把局部闭环当 Feature 完成。
父任务只有全部子任务验收才勾选；T6.2 可先覆盖标量，但须纳入 Relation/forecast 后才能完成，T6.4/T7.3 必须覆盖全部声明组合。
测试随子任务实现；每次工作台冻结实际支持范围/允许文件/验收命令，切片通过即停，不自动开始下一个子任务。

## 完成出口

- 配置驱动的 NPM/普通表输入、Value/Ratio/Relation、snapshot/poll 和三种托管目标全部有非跳过测试证据。
- 当前桶评估和启用后的未来预测均可从独立表查询；实际值/条带/依据准确关联，源表内容/结构/指纹保持，冷启动不伪造预测。
- 同一稳定输入证明分页与重启不改变模型/结果；重复桶不学习，缺进度/不兼容恢复明确失败；无数据仍能维护状态。
- 持久模型可查且完整恢复，current 不被清理，AllState 不复活；配置/Schema/模型/消费位置/运行错误均可追溯。
- 对应 targets、GTest/断言测试、定向生命周期 Sanitizer、全量构建/完整 CTest、clang-format 与 Diff 检查全部通过。
- 以相同数据量、身份数、Relation 扇出、数据库延迟比较吞吐/内存/维护延迟；公共接口或实质性能代价先按约定讨论。

## 完成证据

2026-10-08：扩充既有 Feature 和规格，补齐数据发现、通用任务配置、完整模型持久化/恢复及增量一致性要求。
2026-10-08：按用户决定将算子确定为 explore.baseliner，表/视图统一采用任务配置显式指定，目录仅作为可选辅助。
2026-10-08：按角色及存储讨论明确 baseliner 生产当前桶评估/可选未来预测，独立结果表存储，不追加源表字段；更新关联、时点、职责和验收契约。
2026-10-08：保持单个 Feature，按独立验收结果将任务调整为 7 个一级阶段、20 个直接子任务；拆开持续进度/有限读取及 checkpoint/原子存储/恢复。
实施准入确认时仅文档验收；当时 T1～T7 及其子任务未实施，SQL 示例中的基线任务、新增输入能力、NPM 发布进度及 checkpoint 接口均待交付。
2026-10-08：实施准入确认通过。核对当前 V2/托管 sink、数据库租约、Config 快照、Baseline 状态控制及算法接口，未发现明确 P0/P1 设计阻塞；现有 baseline CTest 12/12 通过。
实施入口保持 T1.1/T1.2：先冻结公共头、配置/观测/结果 Schema 与可执行测试，再实施接线、后端及状态编码；本轮未实施新算子，未完成任何 Feature Task，准入核验记录在 /tmp/baseline-operator-readiness/。
2026-10-08：T1.1 完成：独立 baseliner 契约库交付严格 JSON 解析、Config 精确快照/SHA-256、类型化映射、有限默认值、五类 Arrow Schema、无碰撞业务身份及结果/进度校验；test_baseliner_contract 先失败后通过，构建和格式检查通过。字段与上限以 src/operators/baseliner/baseliner_contract.h 及可执行解析器为准；算法 profile/calendar 由后续算法服务解析，不把创建配置描述冒充已训练模型。记录 /tmp/baseline-operator-t1/。
2026-10-08：T1.2 与 T1 完成：新增三个独立版本公共头，明确 task 私有输入/拥有型 Schema、进度传递、独占原子会话/目标时钟/条件更新、取消与已提交/已回滚/未知结果，以及容量绑定后的空 task 一次恢复。三个头单独按 C++17 -Wall -Wextra -Werror 编译通过；test_baseliner_interfaces 验证 IID、虚接口签名、struct_size/version/长度、源租约和结果分类。真实事务/恢复状态行为仍由 T5/T6 验收。
最终 test_baseliner_contract/test_baseliner_interfaces 构建无 warning/error；新契约与既有 baseline 共 14/14 CTest 通过，JUnit failure/skipped 均 0（1.50 秒）；全部新增 C++ 文件 clang-format-18 --dry-run --Werror 通过，旧公共头 hash 保持，git diff --check 与范围检查通过。未运行 Feature 全量构建/完整 CTest，未修改算法/后端/SQL 接线，未提交或推送；工作台 WIP=0，T2～T7 未启动。

2026-10-08：T2.1 有限来源与 SQL 接线完成。独立 snapshot 会话保持旧虚表，四后端参数化/结构核验/UINT64_MAX/分页一致范围/空范围/错误/取消/预算及 completed run 状态通过；ClickHouse 拒绝 consistent_snapshot。snapshot 有效 TTL=0。test_baseliner_snapshot 1/1（1.36 秒），test_scheduler_e2e 1/1（22.29 秒）无跳过。SQL 两段式 namespace 由同一 V2 task 创建/释放 reader，拥有源租约和 Schema，三段式关系保留旧路径。记录 /tmp/baseline-operator-t2/；T2.2/T2.3 待实施。

2026-10-08：按用户明确约束，将 NPM 时间映射收敛为 B=mP（m 为正整数），常见 m=1，m>1 精确合桶；排除拆桶/插值/非整数倍重采样。补齐批量读取/标准观测与逐桶串行 Submit 的边界及 T2 测试锚点；本轮仅文档收敛，未修改实现或公共接口，未执行构建/CTest，T2.2/T2.3 保持未验收。

2026-10-09：T2.2 完成。Value/Ratio 同桶及整数倍合桶、跨页/跨批次等价、规范 NPM 增量与零终态、稳定身份、require_equal/最大 revision 选择、NULL/Inf 与精确窄化/样本计数、有限补零和身份/字节预算均有断言；标准 snapshot block 输入拥有源租约、单 outstanding 批次、取消和正常尾桶 EOF。四真实来源 NPM fixture 与 SQLite/MySQL/PostgreSQL/ClickHouse Schema/分页验证通过（无 skip），ClickHouse 使用真实 Bool 字段；定向 4/4 CTest、格式/diff 通过。正常分页使用稳定键 seek，重复等键组单独读齐；未创建来源索引，不声明规模性能已验收。证据 /tmp/baseline-operator-t2-resume/；T2.3 待实施。

2026-10-09：T2.3 与父任务 T2 完成。Relation 按 source×目标桶汇齐稳定数值/字典 group_idx，支持显式 other/拒绝；同组多事实及跨页/来源周期合并，组值与配置顺序 metrics 对齐，total/active_count 有断言，缺失字段/NULL/负质量/索引窄化/身份与 group/pending 预算明确处理。四真实来源标准 Relation 批次通过页大小 1/4096 等价验证。审查复现并修复 latest_revision 字符串 collation 身份合并，最大 revision 同桶/跨桶冲突均失败；latest_revision 有限 dataset 摘要受 pending 预算约束。
最终构建无 warning/error；reader/aggregator/block input 额外 -Wall -Wextra -Werror 编译通过；baseliner、既有 Baseline 与 Scheduler 相关 CTest 17/17（25.93s），JUnit failures/errors/skipped 均 0。新增聚合核心及断言在 AddressSanitizer/UndefinedBehaviorSanitizer/LeakSanitizer 下通过（沙箱 ptrace 不支持 LSan，授权沙箱外运行通过）；本轮 C++ clang-format 和 git diff --check 通过。T1 契约/公共头及无关初始改动 hash 保持；记录 /tmp/baseline-operator-t2-resume/。未运行 Feature 全量构建/完整 CTest 或四后端规模基准，不声明源布局/索引性能已验收；未进入 T3～T7，未提交/推送。

2026-10-09：T3.1 安全发布能力与 runtime 接线验收。多输入共同进度、backlog present/unknown、idle、无数据、实体写入/发布失败、EOF 部分尾桶通过；test_npm_basic --progress-only 与周期/采集契约回归通过。独立消费者接口保持旧虚表；证据 /tmp/baseline-operator-t3/t31-*。

2026-10-09：T3.2 完成。SQLite/MySQL/PostgreSQL/ClickHouse 追加 npm_result_progress_v1，按 run/epoch 隔离，显式递增 position 与源周期/关闭边界晚于全部相关实体写入成功；无数据也可发布。非零迟到事实拒绝，终态零增量不重开桶；写入或发布失败、提交应答丢失后 writer 失效，不重试、不继续推进或成功 Finish。重复 run ID 拒绝，保留期清理分批删除发布记录；旧 run 缺真实发布能力、过期/清理中/不可恢复位置均失败。

2026-10-09：T3.3 与父任务 T3 完成。新增独立版本来源能力和 task 私有 PollInput；每轮冻结真实 position/closed_before_bucket，复用有界分页与标准观测聚合，逐页及确认前校验来源有效性。B=mP，m 为正整数；未关闭目标桶暂留来源，不预读为观测，m>1 仅输出完整目标桶。标准批次可包含多个完整桶；范围全部读完后的拥有型零行进度批次，只有成功 ReleaseBlock 后才确认位置。timeout 非 EOF；精确恢复验证 epoch/position/周期/保留期，多 dataset 隔离，取消唤醒等待。
Scheduler 将进度在 ProcessBlock、输出消费及 ReleaseBlock 全部成功后串行交给同一 task，失败/异常不成功 Flush。独立 provider 执行策略只解析配置，不创建 task 或打开 I/O，使持续模式进入既有异步要求链路；生产 baseliner provider/算法注册仍由 T4 交付。

最终受影响 targets 构建通过，无 warning/error；T3、baseliner、既有 Baseline、Scheduler/framework、NPM 周期与存储组合 CTest 25/25 通过（59.11s），JUnit failures/errors/skipped 均 0，四真实来源均运行。覆盖冻结上界期间 writer 推进、部分桶/无数据、失败及未知提交、精确恢复、预算、重复/冲突、页内来源失效、零行进度与取消。定向 ASan/UBSan/LSan 的 poll 测试通过，仪器范围为 baseliner 契约/分页/聚合/输入及测试，数据库与 NPM 共享库使用普通构建；新增数据库适配器另经严格警告编译。本轮 C++ clang-format-18 与 git diff --check 通过，T1 公共头/配置、T2 聚合及分页核心、用户原有 Backlog 改动 hash 保持；证据 /tmp/baseline-operator-t3/final-*、sanitizer-*、scope-checks.json。
本轮完成 T3，工作台 WIP=0；未进入 T4～T7，未提交/推送。未执行整个 Feature 的全量构建/完整 CTest、模型联合恢复或数据库规模基准，不声明生产算法链路、结果持久化与索引性能已验收。

2026-10-09：T4.1 完成。EvaluationEngine 按 dataset/metric 独立创建既有算法任务并绑定容量，以拥有型二进制身份的 hex 作为算法 key；按身份记录 epoch/消费位置，单条及整批语义核验先于 Submit。Value/Ratio 冷启动和半开历史窗 Bootstrap 与直接调用数值/条带/评分/学习权重对照；训练不产生检测结果，历史不足 fail/cold、低样本/低分母、异常跳过/降权、重复/非法/epoch 变化和缓冲预算均有断言。当前评估使用 Submit 返回的更新前条带；first_observation 对外 NotTrained，保留实际值，expected/lower/upper 为 NULL。

2026-10-09：T4.2 完成。完整 Relation 观测按配置 metric 顺序提交，保留 routed 的实际 child metric、summary 和 basis 版本；fusion 采用独立冻结 Schema，不推导算法未提供的全局 can_alert。多 metric、缺失/错序/零总量拒绝且模型不变、collecting/冷启动、basis 版本切换与 handover、routed/fusion 数值及快照均与直接算法调用一致。版本用例覆盖至少 8 个不同 basis 版本；接入结果行只持身份元数据，避免每条 routed/forecast 深拷贝完整关系输入。

2026-10-09：T4.3 与父任务 T4 完成。horizon=0 禁用；启用时在合法消费后预测未来 h 桶，Value/Ratio 复用 PredictRolling，Relation 按每个已有 routed summary 和每个未来桶调用 PredictRoutedSummary。多点数值/状态及 model_basis_id/issued_after_bucket 对照通过；预测后模型/子模型快照不变，未来桶溢出在可变调用前拒绝，扇出受预算限制。forecast 的 observed/偏离/评分/学习/告警为 NULL，失败状态不以 0 伪造条带；不实现完整关系分布预测。

生产 libflowsql_baseliner.so 提供 explore.baseliner V2/IPlugin 及异步执行策略；Create/策略解析不打开 I/O 或创建算法，精确 Config 引用仅读取一次拥有型快照，Open 按 IID 查找服务。ProcessBlock 解码/整批核验后逐桶串行评估，标准 Arrow 输出保持批次划分等价；数据库输入复用 T2/T3 私有 reader 和源租约。动态加载 RAII、40 桶整批/单批等价、独立 Relation fusion 编码、实际 SQLite snapshot 数据库流水线、poll 异步分类/进度接收、失败与取消均有断言。

最终受影响 targets 构建通过，无 warning/error；新增核心/codec/provider/exports/两个测试额外按 -Wall -Wextra -Werror 编译。baseliner、既有 Baseline、Scheduler/framework、NPM 周期/存储组合 CTest 27/27 通过（58.52s），JUnit failures/errors/skipped 均 0，四真实来源必跑用例完成。定向 ASan/UBSan/LSan 的 evaluation 与动态 operator/SQLite pipeline 均通过；仪器范围限于本轮核心/codec/provider/exports/测试，既有算法、reader、数据库及框架共享代码使用普通构建，不宣称全栈 Sanitizer。新改 C++ clang-format-18 与 git diff --check 通过；57 个前序文件按初始 hash 核验，除工作台/规格及两个允许的 CMake 接入文件外均保持，包括 T1 公共头/配置、T2/T3 核心和用户 Backlog 改动。证据 /tmp/baseline-operator-t4/final-*、sanitizer-*、verification.json、scope-checks.json。
本轮完成 T4，工作台 WIP=0；未进入 T5～T7，未提交/推送。Arrow 结果 published_generation=0，托管 sink 尚未实现，Scheduler 会拒绝托管目标；目标事务落库、checkpoint/联合恢复、TTL/完整关闭及全 Feature 构建/CTest/性能验收仍由 T5～T7 交付。

2026-10-09：T5.1 完成。独立 Checkpoint IID 按已冻结接口绑定容量控制和新空 task，native 字段编码完整 Value/Ratio artifact/seed、rolling/校准/成熟度/已消费游标；二进制 key、双精度数值与排序 map 无损往返。外层版本/精确长度/SHA-256、完整 schema、有效配置/编译日历、模型结构与容量先校验，再统一交换；失败不改变状态或恢复资格，成功仅一次，关闭/已消费任务拒绝。
机器验收覆盖空/冷启动/历史模型/异常跳过/低支持、四点预测/继续 Submit 严格对照，非法版本/损坏/负尺度/维度/配置/超限/容量/重复绑定与动态插件独立 IID/句柄所有权。完整 checkpoint 内容逐字节一致；snapshot 仅剔除依分配容量变化的 state_size_bytes 诊断字段。五个受影响 target 已重建，相关 CTest 20 项全部通过（数据库两项因沙箱连库限制在沙箱外重跑通过）；定向 ASan/UBSan（checkpoint、plugin、Base/Value/Ratio task；LSan 未启用）、-Wall/-Wextra/-Werror、clang-format-18、diff/scope hash 审查通过。证据 /tmp/baseline-operator-t5/scalar-final-build2.log、scalar-regression.log、scalar-db-regression.log、sanitizer-build2.log、sanitizer-test.log。T5.2 未验收，T5 父项仍未完成；未进入 T6/T7，未提交/推送。

2026-10-09：T5.2 与父 T5 完成。Relation checkpoint 保存父 artifact/seed、各 shard 的 routed spec/seed/rolling、完整 basis accumulator/active basis/stable refresh/handover、各 metric 的有序保留版本、source 已消费游标、fusion persistence/完整 last_result 及维护计数/游标。管理索引仅编码拥有型 key；在临时 task 的 shard map 中重建稳定引用，验证父子资产/身份/group-space/配置/日历/版本/扇出和容量后统一交换。保存并校验 fusion 哈希桶布局和遍历顺序，使有界清理恢复后选择一致；不持久化指针，不重放观测，不修改旧公共接口虚表。
机器验收覆盖二进制 source key、两 metric/三次以上版本切换及活动 handover、200 桶父子历史模型/20 桶在线、RuntimeOnly/AllState 释放后往返与继续提交、多个未来 routed 预测只读、完整 routed/fusion 结果与 checkpoint 内容逐字节对照、五 source 下低预算清理/TTL/容量/持久键回收一致。非法引用/shard/metric/版本/负质量/父子模型维度/source 游标/group-space/哈希布局、输入容量/两父模型的模型容量、空/关闭/重复恢复均有断言；合法 INT32 最大策略与 UINT32 最大版本上限验证计算无溢出。拒绝后目标完整导出不变，随后合法恢复仍成功。
最终五个相关 target 已重建，相关 CTest 在沙箱外完整执行 20/20 通过（包含四库 snapshot/poll）；最新定向 ASan/UBSan（checkpoint、plugin、Base/Value/Ratio/Relation task、basis runtime 与 fusion，LSan 未启用）及 -Wall/-Wextra/-Werror 编译和运行通过；全部本轮 C++ clang-format-18、git diff --check 通过。初始 146 文件与 T5.1 完成快照的 hash 范围核验通过，保留 T1～T4、冻结公共接口和 Backlog 改动。证据 /tmp/baseline-operator-t5/relation-final-build.log、relation-limit-build.log、final-regression.log、final-sanitizer-build.log、final-sanitizer-test.log、final-scope-audit.log。
T5 完成，工作台 WIP=0 并停止；T6/T7 未执行，Feature 未归档，未提交/推送。数据库持久发布/联合恢复、算子 TTL/完整关闭及全 Feature 组合和性能验收仍待后续阶段。

2026-10-09：T6.1 SQLite 原子目标完成：独占新物理连接、参数化精确行数/UINT64、调用时时钟、有界忙等待/取消、失败提交确定回滚；writer 采用目标时间租约和 epoch/generation CAS，接管拒绝旧 writer。test_baseliner_atomic 1/1、format/diff 与初始 hash 范围核验通过；证据 /tmp/baseline-operator-t6/atomic-*、t61-scope.log。首轮测试配置错误已修正，不将该红灯计作实现缺失复现。未提交或推送。

2026-10-09：T6.2 SQLite 独立结果/模型存储完成：三类算法完整 checkpoint 与算子消费/训练缓冲元数据导出，评估/forecast/fusion、源位置和 current 在同代事务提交；逻辑键保留多预测时点，模型保留限额不删 current。真实算法/查询、源内容不变、重复结果触发整代回滚、预测依据和发布代分离断言通过。测试 PID 重用导致持久 fixture 碰撞，已改唯一键；最新 3/3 和重复执行 1/1、format/diff/hash 范围检查通过，证据 t62-final-test.log、t62-repeat-test.log、store-fixture-build.log、t62-scope.log。联合恢复及生产算子托管接线由 T6.4 收口。

2026-10-09：T6.3 MySQL/PostgreSQL 原子目标完成：独占物理连接、原生绑定/精确行数/UINT64、调用时数据库时钟、超时/取消及未知提交分类；三真实目标存储/forecast/完整模型查询与保留、同键锁等待、初始化竞争、租约接管/旧 writer、丢失提交确认后 durable generation 核查全部通过，ClickHouse 目标明确拒绝。真实测试无跳过；server-test2.log 存储 1/1 和 atomic-server-test3.log 原子能力 1/1，format/diff/hash 范围检查通过。未自动重试、未修改旧公共虚表、未提交推送。

2026-10-09：T6.4 与父 T6 完成。托管算子串行保存完整模型、消费索引/bootstrap 缓冲、source epoch/position 及 pending 窗口起点；按数据/时间阈值同步发布、维护写入租约，持久成功后确认公开进度，错误/未知提交终止且不重试。poll 未关闭桶保留源侧，从持久窗口起点重读并用已恢复身份游标跳过已消费桶；epoch/保留期缺口在消费前失败。
真实四源×三目标×snapshot/poll 的 24 组冷启动恢复，以及三目标×两模式的 6 组 history bootstrap 恢复均覆盖 Value/Ratio/Relation、forecast、完整 checkpoint/数值结果与连续运行对照、提交后再次重启和真实 require JSON 再重启；三目标四类提交故障注入核查 durable generation，预测/评估不重复，损坏后保持空状态与恢复资格、if_exists/require/fresh 和配置不兼容均有断言。
验收中修复 MySQL 原子 reader 的 double 文本窄化、恢复策略误纳入兼容 hash，以及 snapshot epoch 随 restore 策略切换的问题；精确 config 原文/hash 与恢复兼容指纹分开保存，公共接口/算法保持。最终相关 CTest 23/23（JUnit failure/error/skipped=0），最后 snapshot 修正后定向 2/2 同样零失败/跳过；最新定向 ASan/UBSan 完成全部 30 组，halt_on_error=1，LSan 未启用；严格编译、clang-format、diff 和初始/切片 hash 范围审查通过。
证据：/tmp/baseline-operator-t6/final-regression.log、final-regression.xml、final-restart-test.log、final-restart.xml、final-sanitizer-build.log、final-sanitizer-test.log、final-format.log、final-diff-check.log、final-scope.log、t64-scope.log。T1～T5 及无关初始改动保留，WIP=0，停止在 T6；T7 尚未实施，Feature 未归档、未执行全 Feature 性能验收、未提交或推送。

2026-10-09：T7.1 完成。有序活动索引只在合法消费后续期；OnTime 选择活动/checkpoint/租约最早期限，按释放数和保守工作字节预算串行回收，不新增线程或算法状态锁。遍历、Relation 有限扇出、bootstrap 缓冲及 checkpoint 检查/发布均计入边界；预算不足、Release 失败在删除前失败，未到期身份不淘汰。
RuntimeOnly 删除 runtime/活动/身份消费/replay 索引、保留父 artifact/seed 和模型额度；AllState 同时删除父模型，完整 checkpoint 重启不复活。每模块有限的生命周期 epoch 与 dataset 发布进度负责旧输入过滤，不使用无限身份 tombstone；首次消费保持原契约。重入有 seed 时恢复，无 seed 冷启动；维护记录按 generation 保留，不覆盖同身份不同生命周期事件。
checkpoint v2 保存活动年龄及可核验 wall time，并兼容 v1 导入；停机时间计入年龄，wall clock 回退或年龄不可验证时重置期限并输出 deadline_reset 原因。维护事件、回收后的模型/位置/消费索引和 current generation 一次事务发布；未知/失败提交终止，仍须待 writer 租约到期才能接管。
机器验收覆盖模拟时间/不续期/TTL 禁用、reject/evict_idle、释放数/字节/checkpoint 预算、失败不误删、三算法两释放范围/父模型保留/Relation 子状态归零、停机和时钟回退恢复，以及 SQLite/MySQL/PostgreSQL 三真实目标的维护查询、两次重入/重启和提交失败后原 generation 恢复。最终相关 CTest 24/24，JUnit failures/errors/skipped 均 0，既有恢复矩阵全部 30 组通过；最新定向 ASan/UBSan/严格编译通过，halt_on_error=1，LSan 未启用。最终编译日志无 warning/error，clang-format-18、git diff --check 和初始/切片 hash 范围核验通过。
维护延迟夹具为 64 Relation 来源、256 routed 状态，每次最多释放 4 个来源，包含 checkpoint 导出；普通构建最慢一次 53.71ms。真实目标同步空闲维护发布另记录各后端时间，含完整序列化/事务提交；不宣称恒定耗时或已完成 T7.3 性能组合验收。真实数据库恢复矩阵最终耗时 279.48s，240s 上限曾超时，测试 TIMEOUT 调为 480s；断言和数据库调用 deadline 不变。
证据：/tmp/baseline-operator-t71/final-regression3.log/xml、final-ctest-detail.log、final-acceptance.json、final-maintenance-stdout.log、final-build.log、final-maintenance-build.log、final-sanitizer-build2.log、final-sanitizer-test2.log、final-format.log、final-diff-check.log、final-scope.log、final-frozen-scope.log。WIP=0，T7.1 完成并停止，父 T7/T7.2/T7.3 保持待办；前序及无关改动保留，Feature 未归档，未提交或推送。

2026-10-09：T7.2 完成。snapshot/task/poll progress/selection/window 初始化的可取消对象在阻塞调用前安全公开，公开后复查取消信号；目标会话同样补偿早到 Cancel。框架 Cancel 同时唤醒 source 和 transform 的数据库调用，Stop 保持正常收尾；初始化取消回调在 reader/task 释放前解除绑定。Scheduler 初始化 Cancel 与 StopAll 取消、join 全部执行线程后再卸载均有断言；没有新增维护线程或算法状态 mutex，旧公共 ABI 保持。
EOF/Stop 仅一次 Flush，并验证托管尾批 generation=1；来源错误、容量错误和提交中 Cancel 均无成功尾批，新 task 的持久 generation 保持 0。维护之后发生输入错误时，负返回输出为空。真实 PollInput 在途 Stop 的补测先复现 ReleaseBlock=-1 / Flush=0，再以只允许已取得批次归还和已确认进度快照读取的最小修复通过；同一位置 Cancel 仍终态 cancelled、generation=0，不读取新数据。重复 Close 释放 reader/目标会话和恢复索引、引擎用量归零；task、拥有型 Arrow 批次/Schema 先于插件库 RAII 所有者销毁。
四真实来源 SQLite/MySQL/PostgreSQL/ClickHouse 与三原子目标的长调用均在执行中被 Cancel 唤醒，错误输出为空，调用结束后才释放句柄；普通/定向 Sanitizer 本机记录为 0～4ms（按毫秒取整，非生产延迟保证）。提交结果未知的终止/不重试契约由既有原子恢复矩阵继续验证。
对应 targets 构建无 warning/error；相关 Baseline/Baseliner/Scheduler/framework/NPM 组合 CTest 29/29（403.26s），末次 poll 补修后的受影响 CTest 4/4（355.83s），其中 30 组恢复矩阵 334.31s，两份 JUnit failures/errors/skipped=0。最新定向 ASan/UBSan 生命周期测试全部通过，halt_on_error=1、LSan 未启用；clang-format-18、git diff --check、初始/分切片/最终冻结 hash 范围检查通过。证据 /tmp/baseline-operator-t72/：final-regression.log/xml、poll-final-regression.log/xml、poll-final-ctest-detail.log、poll-final-sanitizer-build.log、poll-final-sanitizer-test.log、poll-final-format.log、poll-final-scope.log。父 T7/T7.3 待办，Feature 未归档，未执行 T7.3 全量/性能验收，未提交或推送。

2026-10-10：T7.3、父 T7 和 Feature 完成。生产 SQL 经真实 Scheduler/NPM/数据库入口验证四来源×三目标×snapshot/poll 24 组及 SQLite B=2P 双模式，全部含 Value/Ratio/Relation 与 forecast；实际插件重载后全部 26 task 恢复，本次新增 rows_written=0，源内容/Schema/指纹保持，结果/模型/消费位置一致。额外联合恢复矩阵 30 组通过。
T7.3 验收修复托管 rows_written 摘要和查询 IPC 借用内存问题；两批覆盖回归与定向 ASan/UBSan/LSan 均通过。本续切片仅补 CTest 目录 fixture、证据和归档，前序 C++ 实现保持初始 hash。
完整构建通过，末次无 warning/error；完整 CTest 77/77（716.78s），JUnit failures/errors/skipped=0。定向算法/checkpoint/reader/目标/provider/pipeline 生命周期与查询 ASan/UBSan/LSan 通过；87 个 C++ 文件 clang-format-18、49 个翻译单元严格语法编译（含已核实并记录的告警例外）、文档配置解析、Diff/hash 范围检查通过。
960 条标准观测、24 身份、Relation 3 组/1 指标、horizon=2、批量 24、checkpoint_every_buckets=7 的五轮成本重复通过；直接引擎/Arrow/SQLite 托管/SQLite 每提交 +2ms 的观测吞吐中位数为 8942.33/3458.19/1302.10/1261.78 条每秒，峰值 RSS 中位数为 28.14/30.55/43.67/43.72 MiB。托管每轮 11 次提交、最终 checkpoint 374769 字节，无数据维护中位数 63.13/66.50ms；全部路径 5760 行及数值对照一致。条件、范围、CPU/单批/内存及维护范围见 [验收记录](../../docs/baseliner-acceptance.md)，[使用说明](../../docs/baseliner.md)；不宣称源库扫描或生产规模保证。
验收环境核查：WSL IPv4 本机路由临时修复后已恢复；旧 CTest 进程/缺失目录夹具已处理。系统 DPDK 在 NET_ADMIN 下自动 NetVSC/failsafe 清理崩溃是跨任务问题，堆栈和复现已记录，未修改其实现；最终全量使用 setpriv --bounding-set=-net_admin，数据库/算法/生产断言不跳过，不宣称特权 NetVSC 已修复。
证据 /tmp/baseline-operator-t73/：full-ctest.log/xml、final-build.log、lifecycle-sanitizer.log、query-sanitizer.log、cost-repeat.log、cost-summary.json、final-format.log、strict-compile.log、strict-exceptions.log、doc-config-check.log、scope-check.log、verification.json、wsl-route-before.txt、wsl-route-restored.txt。规格归档，Backlog 完成，WIP=0；未提交或推送。
