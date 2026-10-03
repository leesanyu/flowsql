# Feature: NPM TCP/UDP 会话周期统计

标识：`npm-basic-periodic-stats`
状态：`[x]` 已完成；2026-10-03 T0–T4 全部实施并验收
优先级：P1
前置 Feature：已交付的 `npm-basic-analysis`、`npm-basic-parameters`、`npm-result-query`、`stream-time-drive`、`npm-basic-realtime-integration`。
关联 Feature：`npm-linux-capture-backends`（NetAdapter，见 [Backlog](../product_backlog.md)）提供真实多输入采集与共同安全进度；本 Feature 已用 PCAP 和假采集 reader 独立验收统计语义，真实网卡联验由采集 Feature 收口。

## 业务意图

让分析 PCAP 与实时网卡流量的用户不必等 TCP/UDP 长会话结束才观察流量变化。同一个 `npm.basic` 任务按可配置周期输出每个会话的双向包数/字节增量及累计总量，以稳定 session_id 串联时序；有限输入和持续输入共用统计规则，区别只在时间进度来源与任务终结边界。

## Non-Goals

- 不实现采集后端、通用流式订阅、HTTP 结果推送、Web 实时图表或新的任务调度系统。
- 不按周期拆分 TCP/UDP 会话，不重置 NPI、标签、TCP 性能跟踪、共享字节流或协议事务。
- 不将 Session RTT/重传/握手指标或 DNS/HTTP/TLS/ICMP 事务改成周期增量指标；它们保留自身实体与生命周期。
- 不提供滑动窗口、重叠窗口、运行中修改周期、无限迟到回补或历史窗口修订。
- 不承诺每个周期结束时立即交付；未知积压、采集时间异常及下游背压可以延后结果，不能伪造完整统计。
- 当前为产品构建阶段，直接替换 Basic 数据契约，不提供旧契约兼容、旧数据迁移或旧版本历史查询。
- 不另建第二套会话引擎、存储消费者或双 INTO 语法。

## 核心数据与接口契约

### 配置与默认行为

复用既有 `output_interval_ns`，它在周期模式下表示统计周期，不再只表示单调快照输出间隔。默认 30 秒；15/30/60 秒分别配置为 `15000000000`、`30000000000`、`60000000000`。沿用既有 10 ms～1 h 的校验范围；任务 Open 时冻结，不增加每张网卡的统计参数。

两类来源省略 `result_mode` 时均使用 `periodic_snapshot`；新语义是带周期增量的会话累计记录。既有显式 `result_mode='final'` 保留整体终态导出能力，两类来源的显式配置语义一致；其周期字段为空，不能把全会话总量当作一个固定周期。

省略 `run_mode` 时，Scheduler 按公共来源能力为有限非采集 source 选择 offline、为持续采集 source 选择 realtime；不根据 `netadapter` 字符串判断。它只选择进度接入方式，不选择不同的统计结果语义。显式模式与来源能力冲突时 Open 前明确失败，Schema probe 与 execution 使用同一冻结配置。

周期与模式可由既有顶层 WITH 或 `parameters.core` 配置；沿用既有配置来源冲突检查，不能同时指定两套共享参数。

```sql
SELECT * FROM pcapfile.trace USING npm.basic
WITH output_interval_ns=30000000000 INTO dataframe.flow_timeline
```

```sql
SELECT * FROM netadapter.eth01 USING npm.basic
WITH output_interval_ns=30000000000 INTO sqlite.npm
```

示例目标均需预先创建。有限查询可以在执行结束后读取 DataFrame 中的全部周期记录；周期统计不要求 PCAP 按墙钟慢速回放。持续采集的生产分析使用已有后台执行入口和两段式托管数据库目标，不将周期历史无限追加到临时或命名 DataFrame；无 INTO 或 DataFrame 目标在启动前明确拒绝，并提示指定托管目标。本 Feature 不把普通同步查询改成结果推送接口。

后台批次通过既有任务/状态查询机制控制当前单条持续 SQL：运行期间可取得 task_id、run_id 和结果关系名，Stop/Cancel 送达当前 runner/reader；正常 Stop 排空最后批次并单次 Flush，Cancel 异常释放。控制请求只传控制信息，不能由第二线程并发调用分析/输出回调，也不能等 runner 无限运行结束后才返回运行身份。

### 统一时间与统计边界

周期按 packet 的 Unix ns 采集时间划分，UTC epoch 对齐的非重叠区间为 `[k * P, (k + 1) * P)`，P 是冻结周期；恰好落在右边界的包进入下一周期。任务收到首包后才建立对应会话的统计状态。

| 来源 | 时间进度 | 统计行为 |
| --- | --- | --- |
| pcapfile | 已处理报文时间推进的保守事件水位，应用已有乱序容忍；EOF 提供有限边界。 | fast/timestamp 回放及不同 batch 切分得到相同周期归属，不以处理耗时计周期。 |
| 实时采集 | 同一 reader 已处理并归还 batch 后的安全采集事实；多输入取共同保守水位。 | 确认水位跨过周期右边界后输出该周期；空闲确认可以推进，普通 Poll timeout 不可以。 |

定时器负责唤醒与维护，不能用单调 deadline 或消费者墙钟替代统计事件时间。水位尚未确认、积压 present/unknown 或回拨时，不封闭新周期、不输出假零；已安全封闭的结果仍可排空，其他模块维护按自身契约执行。输出延迟与记录的周期宽度分别解释。

同一会话跨周期保持 session_id、方向、会话键、NPI 和模块状态；周期只轮换统计桶。从首包所在周期到结束前，每个已确认完整的活跃周期输出一条记录，无包时增量为零，以观察空闲变化；不存在会话时不造行，不在会话创建前或结束后补零。

每个包恰好计入一个未封闭周期。容忍范围内乱序只更新未封闭桶；落入已封闭周期的迟到包明确导致任务失败并报告来源/时间/封闭边界，不静默丢弃、计入当前周期或改写已提交行。未封闭桶、已结束但待输出状态与结果全部纳入既有任务预算；进度停滞时预算不足按既有 fail 策略结束，不能无限保留。

正常会话关闭、idle timeout 或 tuple reuse 时按时间顺序排空该会话尚未输出的周期，最后一条标记终态；周期结束自身不会终结会话。正常 EOF/Stop 排空已封闭周期并输出剩余部分周期，不补齐文件时间边界之后的空闲周期。取消/读取错误/消费者错误只保留已交付前缀并异常回收，不伪造终态。

### Basic 结果数据形状

保留 `basic` 实体和会话身份，将原无标签/带标签两种 Basic Schema 合并为单一固定 32 列结构；标签配置只影响字段值，不改变列集合、类型、nullability 或字段顺序。原身份、端点、协议、标签和终结字段继续存在；新增的最小数据形状如下，公共头文件与 Arrow 字段顺序已在 T0 冻结。

```cpp
struct NpmBasicPeriodStats {
    int64_t period_start_ns;
    int64_t period_end_ns;       // 名义周期右边界，不将尾部短周期拉伸成满周期
    bool period_complete;       // 已确认覆盖到右边界；正常提前终结的尾部为 false
    uint64_t interval_packets_ab;
    uint64_t interval_packets_ba;
    uint64_t interval_wire_bytes_ab;
    uint64_t interval_wire_bytes_ba;
    uint64_t interval_wire_bytes_total;
};
// NpmBasicResult 新增 optional<NpmBasicPeriodStats> period。
// Arrow 展开为上面八列；显式 final 模式这八列均为 NULL。
// NpmBasicResult 另新增 uint64_t wire_bytes_total，Arrow 为非 NULL uint64。
// Arrow 固定包含 nullable uint32 primary_label_id；未启用 labeling 时为 NULL。
```

| 字段/范围 | 语义 |
| --- | --- |
| session_id、revision | 同会话身份稳定；每输出一条递增 revision，终态后不再输出该身份。 |
| primary_label_id | 固定标签列；未启用 labeling 时为 NULL，启用但未命中时为 0，命中时为对应非零标签 ID；会话内命中值沿用既有稳定主标签规则。 |
| period_start_ns、period_end_ns | 固定周期名义范围；用于按时间排序/聚合，不用 observed_at 代替。 |
| period_complete | 周期完整性，与会话是否结束不同；EOF 尾部或提前关闭的尾部为 false。 |
| interval_packets_*、interval_wire_bytes_* | 该条记录新交付的周期增量；非重叠统计可相加。 |
| packets_*、wire_bytes_*、first_ns、last_ns | 截至本条统计边界的会话累计值和已统计报文时间；不能含尚未输出的未来周期，也不能跨 revision 求和。 |
| interval_wire_bytes_total | 本周期双向总流量，等于 interval_wire_bytes_ab + interval_wire_bytes_ba；单位 byte，可按对齐周期跨会话/时间聚合。 |
| wire_bytes_total | 会话双向累计总流量，等于 wire_bytes_ab + wire_bytes_ba；单位 byte，显式 final 模式也输出，不能跨 revision 求和。 |
| observed_at、协议状态/名称 | 实际生成时刻及当时已知识别状态；不承诺事后识别结果反向修改历史行。 |
| is_final、end_reason | 表示会话终态；周期完整行通常 is_final=false，尾部终态可以是部分周期。 |

终结时没有未交付增量，仍输出一次零增量终态标记，累计总量保持不变；可引用最后已输出的周期范围，完整性保持一致。不得重发该周期的非零增量。周期模式的全部 interval 计数之和必须等于最终累计值；空输入不产生会话或终态行。

总流量是当前 Basic 行所属 TCP/UDP 会话的双向合计，使用原方向字段相同的 packet.meta.wire_len 计量口径；捕获截断不改用 captured_len，重传报文仍计入观察到的流量。两项总量由对应方向值求和投影，不另维护独立累加器。uint64 求和溢出明确失败，不回绕或饱和；零流量周期/零增量终态的 interval_wire_bytes_total 为 0，所有周期总流量之和等于最终 wire_bytes_total。

Basic 只保留一个 Schema，统一使用 schema_version=1；标签启用与禁用使用相同 Schema 指纹、metadata 和实体描述符，不按标签开关选择版本。metadata 标明累计列与 interval 列的各自统计范围，实体 revision 仍以累计语义注册。公共结构、Arrow 编码、Schema probe/执行与四后端读写同步采用该契约，不保留旧数据形状或旧版本输出路径。

Session 和协议实体保持现有 Schema、指标含义及实体身份。共享核心提供统计截止通知；Session 可继续输出累计状态，但不能把其累计 RTT/重传字段宣称为本周期增量，也不能为了 Basic 周期重新 Finish/Abort 协议模块。

### 结果交付与查询

周期行沿用同一个结果 router、预算、同步 consumer 和一个 INTO；features 决定持久化实体，observing 只选择前台实体。四种既有托管数据库后端共用单一 Basic Schema 契约，不增加专用周期存储。

| Basic 结果关系 | 查询含义 |
| --- | --- |
| history | 本 run 的全部周期记录及终态标记；观察时序用此关系和 period_start_ns。 |
| latest | 本 run 每个 session_id 的最大 revision；用于累计状态，不是每周期一条。 |
| final | 本 run 的会话终态；整会话总量读取累计列，interval 列仅为最后未交付部分或零。 |

Basic 统一使用 `npm_basic_history_v1`、`npm_basic_latest_v1`、`npm_basic_final_v1`，标签开关不改变关系名；版本后缀沿用现有存储命名规则，只对应这一套 Basic 契约。沿用 run_id 隔离、三段式数据库查询和既有保留期规则；未配置保留期时不自动过期，writing run 不因此新增滚动清理承诺。

## 主链路

1. Scheduler 解析来源能力和冻结配置 → reader/来源绑定 → npm.basic 唯一会话引擎逐包更新 → 报文计入所属未封闭周期 → 安全事件水位跨过边界 → 有界输出周期增量与累计记录 → 既有 router/consumer 交付；PCAP 回放速度和实时定时器不改变周期归属。
2. 会话结束或 source 正常 EOF/Stop → 顺序排空周期及最后部分 → 单次终态 → 释放该会话统计状态；其他会话继续运行，取消/错误沿用异常回收，不补造完整结果。

## Feature Tasks

- [x] T0：交付统一周期配置、时间边界与含主标签和双向总流量的单一 Basic 数据契约，使标签开关不改变结果结构，PCAP/实时来源有相同默认统计语义且增量、累计与终态可被机器区分。
- [x] T1：交付唯一会话引擎内的有界周期累计、乱序处理及正常收尾，使长 TCP/UDP 会话按周期产生多条记录且计数不重不漏、会话和协议状态不被重置。
- [x] T2：交付有限/持续来源的能力选择、时间驱动及既有后台任务控制接线，使两类来源共用统计逻辑，持续任务可查询运行身份并停止，且繁忙、空闲和背压不制造错误周期或无限结果容器。
- [x] T3：交付统一 Basic 结果在既有四种托管后端与 DataFrame 的读写，使周期时序、最新累计和终态总量按 run 隔离查询且标签开关不改变查询关系。
- [x] T4：交付跨来源 SQL 联验与模块生命周期回归，使相同报文时间线在 PCAP/假实时源上产生等价周期统计，并验证长会话、尾部、失败和预算的完整链路。

## Feature 验收

- 配置 GTest：两类来源缺省周期 30 秒、periodic_snapshot 默认、15/30/60 秒配置、既有范围/配置冲突检查、显式 final 周期列为空、来源能力与显式 run_mode 冲突；Schema probe/执行一致。
- 契约 GTest：标签启用/禁用时 Basic 的字段集合、类型、nullability、顺序、metadata、指纹和实体 schema_version 相同且版本恒为 1；primary_label_id 编码分别验证未启用为 NULL、启用未命中为 0、命中为标签 ID，Schema probe 与执行一致。
- 统计 GTest：跨至少三个周期的同一 TCP 与 UDP 会话身份不变，增量与累计正确；边界包只进下一周期，双向计数和 wire length 正确，零流量周期有明确记录，空输入无行。
- 总流量 GTest：单向/双向及零流量均满足两项总量求和公式；截断使用 wire_len、重传保留流量计数，周期总流量之和等于 final 累计总流量；uint64 和值超界明确失败，显式 final 的累计总量有效而周期总量为 NULL。
- 时间 GTest：fast/timestamp PCAP 回放、不同 batch 切分、容忍内乱序结果一致；封闭后迟到明确失败；实时无进度/积压/回拨不造完整周期，安全空闲和共同水位才能封闭，单调/墙钟不决定归属。
- 生命周期 GTest：周期通知不重置识别/标签/Session/TCP 字节流/协议事务；关闭、超时、tuple reuse、EOF/Stop 最后部分与零增量终态恰好一次；取消/错误无伪终态，pending/桶/owner 预算全部归还。
- 存储 GTest/集成：单一 Basic Schema 指纹、nullable 标签/周期列及两项总流量 uint64 无损往返、run 隔离；标签启用/禁用共用同一数据表和公开关系，分别验证 NULL/0/命中标签值读写；四后端 history 保留周期、latest 取会话最大 revision、final 累计正确，interval 求和等于 final 累计，不因终态标记重复计数。
- Scheduler SQL：同一报文时间线通过 pcapfile 和假采集 reader 验证等价周期边界/增量/累计/身份关联；忽略实际 observed_at、run_id 和交付延迟，使用相同观测域配置；真实 NetAdapter 另由关联 Feature 联验。
- 出口/背压：有限输入 DataFrame 保存全部周期行；持续输入无 INTO/DataFrame 明确拒绝，托管目标逐批写入；后台任务运行时可查询 run_id/关系名并读取已写入历史，Stop/Cancel 能唤醒当前 reader 并分别正常/异常收尾，不以只在 SQL 之间检查标志或内部周期回调代替此验收。
- 实施时运行 `test_npm_basic`、`test_framework`、`test_scheduler_e2e`、`test_npm_result_sqlite`、`test_npm_result_backends` 的对应 target/CTest；Feature 收口全量构建、完整 CTest、clang-format 与 diff 检查通过，未执行的集成项保持待验。

## 完成证据

2026-10-03：T0–T4 全部交付；按产品构建阶段直接替换 Basic 契约，未增加旧契约兼容路径。

- T0：固定 32 列、schema_version=1、nullable 主标签三态、周期八列及双向累计/增量总流量；两类来源默认 periodic_snapshot/30 秒，显式 final 的周期列均 NULL。顶层 WITH 与 parameters.core 配置、15/30/60 秒及来源冲突断言通过。
- T1：周期模块接入唯一会话引擎，epoch 对齐桶按安全水位排空；跨三周期 TCP/UDP、边界/方向/wire_len/重传、空闲零、部分尾部、零增量终态、乱序/迟到、预算/消费者失败与资源归还均通过。
- T2：Schema probe 与执行按来源能力选择模式；持续无 INTO/DataFrame 拒绝。后台运行中发布身份和查询关系，Stop 唤醒 reader 后单次 Flush 并 completed，Cancel 保留已交付前缀并 incomplete；回调保持单线程。
- T3：SQLite、MySQL、PostgreSQL、ClickHouse 均实际执行，验证 NULL/0/标签 ID、UINT64_MAX 无损、完整/部分周期与零终态、run 隔离、history/latest/final 和增量求和；有限 SQL DataFrame 保留全部周期记录。
- T4：相同时间线通过 PCAP fast/timestamp、batch=1/4 和假实时 reader 验证周期等价；忽略 observed_at/run_id，生成时间独立检查。回拨/积压/不完整水位、EOF/Stop/Cancel、协议生命周期、背压和 reader/batch 归还均通过。

最终验证：

| 命令/检查 | 实际结果 |
| --- | --- |
| `cmake --build build -j8` | 全量构建通过；最后两处测试工具修改另编译对应 target 通过。 |
| `ctest --test-dir build --output-on-failure` | 沙箱外完整 CTest 43/43 通过，72.72 秒；含完整 test_npm_basic、Framework、Scheduler、四后端及其他模块回归。 |
| `ctest --test-dir /tmp/flowsql-npm-periodic-asan -R '^test_npm_periodic_(contract\|stats\|runtime)$' --output-on-failure` | ASan/UBSan/LeakSanitizer 3/3 通过，0.40 秒；独立配置 FLOWSQL_NPM_PERIODIC_SANITIZERS=ON、FLOWSQL_FLOW_LABELING=OFF。 |
| `build/output/benchmark_npm_basic 4 1` | Basic 与 Basic+Session 两模式均通过冒烟检查；不设吞吐验收阈值。 |
| clang-format-18 修改区域检查、`git diff --check`、允许文件审查 | 均通过；原有其他 Backlog 改动保留，NetAdapter 规格仅更新关联归档链接。 |

日志：`/tmp/npm-periodic-full-build.log`、`/tmp/npm-periodic-full-ctest-final.log`、`/tmp/npm-periodic-asan-runtime-test.log`、`/tmp/npm-periodic-benchmark-smoke.log`。

独立发现：完整 test_npm_basic 的 LeakSanitizer 检出未修改的 NPI 初始化路径泄漏，1,222,349 bytes / 47 allocations，堆栈位于 ObjectsPool/NetworkLayer 和 TestNpiPipelinePoolOptionAndLeaseContract；复现目录 `/tmp/flowsql-npm-periodic-asan`，日志 `/tmp/npm-periodic-asan-test.log`。归属 NPI 生命周期任务，依跨任务隔离规则记录；周期专属 Sanitizer 和普通完整 CTest 的结果如上，未关闭 leak 检查或加入抑制规则。
