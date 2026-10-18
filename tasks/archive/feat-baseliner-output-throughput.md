# Baseliner 批量结果输出与性能验收

状态：`[x]` 已完成；优先级 P1。2026-10-18。
前置：[单表与 DataFrame 分析](../archive/feat-baseliner-single-source.md)、[生命周期与评估契约](../archive/feat-baseliner-contract-safety.md)。

## Non-Goals

- 不改 Value/Ratio/Relation 数学公式、冷启动/训练策略、特征、时区/日历或逐桶学习顺序。
- 不改 SQL、parameters/config、结果/模型 Schema、config_hash、checkpoint 格式及公共插件 ABI。
- 不重写共享 DataFrame 存储或全部算子的追加机制，不扩展 NPM、数据库驱动、Kafka 或 Web。
- 不引入并行计算、异步写出、通用缓存/内存池、新的用户调参项或跨出口事务保证。
- 不增加读取页数、内存预算、超时或减少结果字段来取得性能收益；Release 构建不替代输出路径修复。

## 业务意图与边界

为将 CSV 注册为 DataFrame 后执行分钟级基线分析的用户，减少结果单行编码与逐次全量拼接的成本，
使五万余条单序列数据的真实 SQL 分析从分钟级降至可复核的秒级，并保持原有分析结果和发布保证。
主交付是 Baseliner 内部有界批量编码；共用路径上的数据库、预测及托管输出必须通过等价回归。
优化粒度是一个执行回调内的结果批次，不改变 observation 的分析粒度，也不跨输入回调等待凑批。
本 Feature 不宣称通用 DataFrame 追加在任意数据规模下达到线性复杂度。

## 已核查事实与成本依据

原始样本为仓库内 `src/tests/data/csv/link_data_2_month.csv`：57,761 行，单序列，连续 60 秒间隔。
其 SHA-256 为 `6e91cd0c86e2275bd67b043176488ae8280989d346024fc337308d12056f80a7`。
生产路径对每个 observation 分别编码结果，Scheduler 逐批 Append，DataFrame 每次拼接全部已有结果。
现有原始输入成本用例先按页收集结果再编码，与上述生产粒度不同，不能作为该热点的完整覆盖。

| 2026-10-18 隔离分段诊断 | 耗时/数量 |
| --- | --- |
| 基线评估和滚动更新 / 单行 Arrow 编码 | 2.43 秒 / 6.05 秒 |
| 57,761 次 DataFrame 单行批次追加 | 149.18 秒 |
| 对照：既有结果每 4,096 行合并后追加 | 1.18 秒；三路结果逐值等价 |

以上为当前无优化编译选项的生产函数诊断，未包含完整 SQL 调度，不是修复后的性能证据。
本地持久任务同 SQL 记录为 149 秒；用户反馈约 5 分钟的那轮完整耗时尚未逐段还原。
诊断原始文件缓存于 `evidence/baseliner-output-throughput/diagnosis/`，由 Git 忽略，不构成运行依赖。

## 核心数据与接口契约

### 批次边界与已有接口

复用既有 `IBlockTransformTaskV2`、`BlockTransformOutputV1` 与输出绑定接口，不新增公共纯虚接口。
T1 在内部头声明和测试中冻结以下最小数据契约；统计仅供验收，不新增用户配置或公共诊断 API。

```cpp
struct ResultBatchLimits {
    uint32_t max_rows = 4096;
};
struct ResultBatchStats {
    uint64_t result_rows = 0;
    uint64_t result_batches = 0;
    uint64_t encoded_bytes = 0;
    uint64_t peak_staging_bytes = 0;
};
```

- `EvaluationRow` 暂存拥有 identity、source_epoch、model_basis_id、字符串和所需结果，不借用循环栈对象。
- 每个结果批次至多 4,096 行；达到行数或可用字节预算前排空，单行仍须满足既有硬预算。
- 同一结果 Schema 的行按原发射顺序合批；不同 Schema、时间戳或发布边界不得合并。
- 每桶继续先评估后学习；evaluation/forecast 的顺序、target/issued_after_bucket 和模型版本引用不变。
- Value/Ratio 每桶 evaluation 后的 forecast 保持顺序；Relation 保持 routed 与 fusion 的既有交付次序。
- `ProcessBlock`、`OnTime` 正常返回前排空本回调接受的行；正常 `Flush` 排空末批并只导出一次最终模型。
- 正常 EOF 的 Schema 空批次沿用原契约；统计非空主结果批次时不把它计入，也不能漏发已有 Schema。
- 行数/字节达到边界时切批，不对单行重新求值、重新学习或修改数值来适配批次。

### 预算、失败与发布

- 沿用 `read_policy.max_pending_bytes` 及各出口已有预算；计入新增暂存的容量、字符串与编码同时峰值。
- 可用预算扣除当前路径已保留的主结果/托管 pending；释放或所有权转移后归还对应暂存计费。
- 输出 Arrow 必须拥有 buffer/allocator 生命周期；task、产物、Schema 销毁顺序仍早于插件卸载。
- 允许按预算切分，但不能靠切批绕过单行、整个回调或指定出口累计结果的既有超限拒绝。
- 取消、编码失败或预算失败不交付本次失败回调的部分 outputs；清理新暂存并保留原始错误。
- 不把已发生的算法更新承诺为可回滚；错误任务按原终态结束，不允许重试本回调重复学习。
- 指定 DataFrame 仍在成功完成后登记发布；失败/取消不将半份新结果登记为成功，不覆盖先前已发布通道。
- 托管 generation、checkpoint、消费位置、租约和计数口径保持；同库两出口事务及跨出口实际提交状态保持。

## 两条主链路

1. 有限快照 → 既有校验/排序/分页/聚合 → 逐桶评估和更新 → 拥有型结果暂存 → 按预算批量编码
   → Scheduler 原结果接收器 → EOF 最终模型导出 → 两出口按原条件发布。
2. 数据库托管/时间驱动 → 原模型与进度边界 → 在本回调内批量编码并排空 → 原 generation 发布；
   取消/失败 → 丢弃本次未交付结果和新增暂存 → 保留此前已提交状态及错误。

## 标准负载与复现契约

先从仓库原始 CSV 按 `Asia/Shanghai` 解析 `time`，保留原列、行序与 Mbps，增加 `time_s:int64` 和
`link:utf8=link_1`。首末 Unix 秒为 1624692180 / 1628157780，去重键无重复，共 57,761 个桶。
现有转换文件为 `build/output/dataframes/link_data_2_month_baseliner.csv`；可从原样本独立重建，
不依赖 Python Worker 上的用户算子或历史 `/tmp` 路径。SQL 在隔离 Scheduler 的同名通道执行：

```sql
SELECT * FROM dataframe.link_data_2_month_baseliner
USING explore.baseliner
WITH parameters='{
  "task_key": "link-data-2-month-mbps",
  "clock": {"bucket_seconds": 60, "timezone": "Asia/Shanghai"},
  "calendar": {"calendar_id": "cn-holiday", "calendar_version": "2026.1"},
  "bootstrap": {"mode": "cold"},
  "forecast": {"horizon_buckets": 0},
  "read_policy": {"page_rows": 4096, "max_pending_bytes": 268435456},
  "dataset": {
    "fields": {"time_s": "int64", "link": "utf8", "Mbps": "float64"},
    "series_keys": ["link"],
    "deduplicate": {"keys": ["time_s", "link"], "on_duplicate": "require_equal"},
    "bucket": {"column": "time_s", "unit": "s"},
    "metrics": [{"id": "mbps", "kind": "value", "column": "Mbps",
      "aggregate": "mean", "feature_type": "value_basic", "profile": "default"}]
  }
}', model_output='dataframe.link_mbps_final_models'
INTO dataframe.link_mbps_baseline;
```

期望 57,761 条 evaluation、0 条 forecast、1 条最终模型，原输入保持；Schema 与结果字段不变。
计时从 Scheduler 开始执行到完成发布并进入 completed，包含读取、计算、编码、追加、模型导出及
完成路径实际执行的持久化；初始 CSV 导入、排队等待、前端轮询和绘图另计，不混入计算分段。

## 验收契约

### 机器断言与真实路径

| 验收对象 | 必须证明的保证 |
| --- | --- |
| 批次结构 | 经真实 `.so` 的标准负载输出非空主结果批次 ≤16；每批 ≤4,096 行，不能在测试外先聚合替代算子 |
| 精确等价 | 同构建基线逐行比对所有列、NULL、身份、顺序；最终模型参数/学习次数与 checkpoint 状态语义一致 |
| 切批边界 | 0/1/4095/4096/4097 行、最后不足整批、字节恰好/超限、单行超限、变长身份及 page_rows=1/256/4096 |
| 三类分析 | Value/Ratio/Relation，多序列/多指标，cold/history，horizon=0/2；预测和 fusion 各保持原顺序/Schema |
| 输入与出口 | DataFrame/SQLite 快照、指定结果+模型、旧托管 snapshot/poll、EOF、时间维护、恢复后续读 |
| 错误生命周期 | 暂存中取消、编码/预算失败无半批交付，旧输出保持，重复 Flush 拒绝、task/产物/插件释放安全 |

结构、等价、边界及小样本通过正常 CTest；完整样本使用真实算子和 Scheduler 接收路径，不能仅测引擎。
标准负载按批编码和追加主结果；不要求改变普通算子或共享 DataFrame 的存储表示。

代表成本夹具固定为：32 桶单序列 Value；4,096 桶×4 序列×2 个 Value 指标；4,096 桶×4 序列
Ratio（numerator=100+bucket%3、denominator=1000）；4,096 桶×4 序列 Relation（3 组质量 5:3:2）；
4,096 桶单序列 Value 且 horizon=2。Value 值为 100+bucket%3，第二指标取其两倍；均 cold、60 秒桶、
page_rows=4096、256 MiB 预算；身份、桶序及其他参数在 T1 固定，新旧使用相同夹具，不按结果选取负载。

### 性能门槛与计时方法

- 实施前固定源码/产物、完整编译参数、CPU/内存/系统、输入和配置摘要；保存可重跑的原实现基线。
- 主比较使用当前默认构建参数且新旧完全相同；Release 可补测，必须新旧成对，不能混用构建模式计算收益。
- 单并发、相同执行位置与机器，预热 1 次后新旧各独立执行 5 次，使用耗时中位数，保留全部原始样本。
- 每轮建立新的 cold 任务与私有输出，读取同一输入快照；不得复用已训练模型、覆盖用户通道或并行跑其他基准。
- 标准负载完整 SQL 中位数比基线降低 **至少 80%**，且 **不超过 30 秒**；两项同时满足。
- 真实生产路径的结果编码+DataFrame 追加合计中位数降低 **至少 90%**；分段和未插桩总耗时分别记录。
- 16,384 / 32,768 / 57,761 行前缀各测 5 次；相邻完整 SQL 中位数比值分别 ≤2.5 / ≤2.2。
- 上述五组代表成本负载各自新旧中位数退化不得超过 10%，结果全部等价。
- 统计真实结果批次/行数、Arrow 字节、暂存峰值、墙钟/CPU 时间和 RSS；保留全量结果做比较的内存单独标明。
- 性能门槛适用于上述冻结环境和负载，不作为生产容量 SLA；未达到即保持未验收，不以调大超时或历史诊断替代。

### 依赖、回归与 DoD

新增结构/等价/性能用例使用本机隔离 Scheduler、真实 `.so`、私有 DataFrame/SQLite 和仓库 CSV，
不依赖外部数据库、Kafka 或采集权限；测试加载器用 RAII 同时保留插件及产物的生命周期所有权。
修改或新增依赖测试时同步声明 fixture、执行位置和资源锁；外部回归统一使用运行时测试配置及实际位置预检。
执行对应 target、Baseliner/Scheduler 定向回归和 ASan/UBSan/LSan；Feature 收口要求全量构建、
不带筛选的标准 all（覆盖完整 CTest 和全量回归），选中=执行=通过，失败/跳过/未执行均为 0。
格式、Linter 和 diff 审查通过；外部环境不可用则标明未验证，不能把 Feature 标成完成。
原始日志、基线快照、源码/产物摘要与 summary 留在 `evidence/baseliner-output-throughput/`，不入 Git；
当前能力与带日期、负载、环境和限制的性能说明维护在已有 Baseliner 用户说明，不新建重复能力文档。

## Feature Tasks

- [x] T1：交付经过真实算子的结构断言、等价参照与可重跑的原实现成本基线，使优化针对生产热点且收益可比较。
  验收：单行批次问题由机器断言复现，完整样本及前缀基线/编译环境冻结；内部批次和预算数据契约对齐。
- [x] T2：交付本回调内预算受控的拥有型批量结果输出，使标准样本主结果批次降至 ≤16 且逐桶计算和结果保持。
  验收：结构、数值/NULL/顺序、行数/字节边界通过；每批 ≤4,096 行，无跨回调等待或单行重算。
- [x] T3：交付批量输出在各分析类型、出口和失败生命周期下的完整保证，使性能优化不损害发布、恢复与资源安全。
  验收：三类分析、预测、模型/托管出口、EOF/时间维护、取消/失败/恢复及 Sanitizer 全部通过。
- [x] T4：交付达到量化门槛的真实 SQL 成本和全量回归证据，使用户可复核秒级分析收益及适用边界。
  验收：同配置五次中位数、批次数、增长曲线和代表负载退化门槛通过，完整 DoD 通过并更新用户说明。

同一 Feature Task 可跨多个 10～30 分钟 Atomic Slice；允许文件、命令和本轮步骤仅在工作台冻结。
逐个任务完成验收并记录证据后再勾选；任务状态以勾选和当前完成证据为准，不提前标记未验收任务完成。

## 完成证据

2026-10-18：仅完成立项与规格编写，T1～T4 均未实施；诊断结果和对照试验不计为优化验收。

2026-10-18：完成 T1 前置的独立批量编码可行性试验，未完成正式 T1～T4 验收；生产源码/产物未修改。
同默认构建、原 57,761 行样本、各预热一次后各五轮：单行路径总耗时中位数 175.076 秒，
仅批量追加 10.492 秒，批量编码+追加 2.924 秒（2.901～3.115），约 59.88 倍、耗时降低 98.33%。
编码/合并/追加中位数 170.748 → 0.452 秒；批量编码和追加均 16 次，每批 ≤4,096 行。
17 次完整结果/最终模型精确比对通过；另经真实 .so 核验，原算子 57,761 个单行批次与原型参照完全等价。
本地审计 54 项通过、0 失败；证据为 evidence/baseliner-output-throughput/feasibility/（不入库）。
建议实施以本样本完整 SQL ≤10 秒、争取 5～10 秒为优化目标；尚未测量 Scheduler 完成发布/持久化，
不把核心链路 2.924 秒当作完整 SQL 耗时，不更改或宣称通过原冻结性能门槛。

2026-10-18 T1 完成：真实 .so 的 10 组原结果/模型等价用例通过；标准结构测试按预期红灯，
4,095 行仍产生 4,095 批。真实隔离 Scheduler SQL 16,384/32,768/57,761 行分别为
14.598/64.346/173.306 秒；五组固定代表负载及完整 IPC 参照保存成功，32 行标准成本 CTest 通过。
内部数据契约、编译环境、原库副本及无关文件/暂存区摘要已冻结，格式和 diff 检查通过。
证据：evidence/baseliner-output-throughput/implementation/t1-* 与 references/（不入库）。

2026-10-18 T2 完成：拥有型 Arrow builders 在本回调内有界编码；不新增 EvaluationRow 暂存副本，
受限 allocator 计入所有已完成批次、builder 及扩容同时峰值，逐 buffer 持有 allocator。
原结构红灯转绿；0/1/4095/4096/4097/8193、page=1/256/4096、horizon=2、预算恰好/少一字节、
按字节切批、累计失败空 outputs 与独立列拥有期通过。定向标准测试 3/3，零跳过。
同规模合成输入直接回调 15 批；隔离 Scheduler SQL 首轮 4.702 秒，全部 30 列及最终模型与旧 IPC 精确一致。
此为 T2 检查点，不替代 T4 五次中位数。证据：implementation/t2-*（不入库）。

2026-10-18 T3 本机检查点通过，外部矩阵被明确环境权限阻塞，T3 保持未勾选，T4 未开始。
真实 CSV 经生产 DataFrame 输入及 .so 输出 57,761 行/16 批/1 模型，全部列和模型与旧参照精确一致；
与同规模合成输入直接回调的 15 批区别记录，末桶读取边界未改。
最终相关 targets 全部重建后标准本机回归 selected/executed/passed=20，失败/跳过/未执行=0，
summary 为 evidence/test-environment/20261018T110150Z-7a14b3b9/summary.json。
独立 Sanitizer 构建完成，ASan/UBSan/LSan 定向 10 项业务测试加 1 项夹具共 11/11；
覆盖本次行数/分页/预算/三类分析/中途取消和编码失败、原算子/只读模型/资源释放；第三方 Arrow/客户端未插桩。
原受限沙箱中的 LSan 线程检查 fatal 未计成功，获批纯本机执行后全绿；原聚合用例超时日志保留，
全部原断言按五个独立 CTest 入口执行，单用例上限仍为 90 秒，未减少负载或放宽断言。
数据库 doctor 在 socket 阶段 execution_denied；扩大执行权限的 doctor 被自动审批拒绝，
等待用户明确授权统一测试配置所声明的连接与隔离夹具。未执行外部发布/恢复/托管矩阵、正式五轮性能和 all，
不归档、不标记 Feature 完成。格式/diff/无关文件/暂存区审计通过，未提交/推送。
证据：implementation/implementation-checkpoint.json、t3-unit-rebuilt-final.log、t3-sanitizer-final.log/xml、
t3-production-structure.*、t3-doctor.log、t3-scope-final.json；本地缓存均不入库。

2026-10-18 T3 整体完成：用户续作授权后标准隔离项目真实数据库矩阵 17/17，零失败/跳过/未执行，
summary=evidence/test-environment/20261018T113455Z-c5fe4fb2/summary.json。与已验证的本机20/20、
Sanitizer11/11和真实CSV等价证据共同满足T3。旧同名卷归属不明，未接管或清除；从统一配置仅
复制独立project，复用其余镜像/角色/地址及标准夹具。源码/产物一致，无关修改与暂存区保持。
T4正式性能和全量DoD仍待验收。

2026-10-18 T4 完成：全部正式负载同机、同默认无O2/O3构建、单并发，预热1次后新旧各5次。
完整Scheduler SQL中位179.490327915→4.922357173秒（新范围4.684642412～5.172186598），
降低97.2576%，约36.46倍，≤30秒及≥80%门槛通过；全部30列/NULL/行序/最终模型精确相等。
16,384/32,768/57,761行新中位1.437938131/2.940406057/4.922357173秒，
增长比2.044876614/1.674039938，低于2.5/2.2。
真实reader/聚合/.so/DataFrame追加的独立诊断，编码+追加中位173.800553827→0.704984231秒，
降低99.5944%；总分段180.046249561→3.386846059秒，与未插桩SQL分开报告。
结果57,761行、1模型、57,761→16批（每批≤4,096）；新暂存峰值4,425,600字节；
比较前RSS旧中位313340KiB、新189516KiB，比较参照53,117,628字节在计时外加载并单独记录。
五代表负载旧→新中位秒：small0.031488029→0.022028197、value39.30166343→1.400782519、
ratio9.550301406→0.734068482、relation52.545022159→54.037384187、
forecast1.988433899→0.320501585；Relation+2.8402%低于10%，全部结果/fusion/模型相等。
标准完整配置和全量构建退出0，随后无筛选all selected/executed/passed=164，
failed/skipped/not_executed=0；summary=evidence/test-environment/20261018T130312Z-e7fccdc4/summary.json。
源码/产物前后摘要一致；本Feature格式/diff/有界审查及无关文件/暂存区审计通过，
ASan/UBSan/LSan既有11/11证据与未改生产源码保持对应。用户说明docs/baseliner.md已补测量方法、
结果/限制与独立复跑命令。旧诊断副本统计池生命周期首轮错误已修复，失败日志保留且不计样本。
所有原始样本、摘要及命令位于evidence/baseliner-output-throughput/implementation/，不入库。
T1～T4全部勾选，规格文件归档，Backlog完成、工作台WIP=0；保留无关改动，未提交或推送。
