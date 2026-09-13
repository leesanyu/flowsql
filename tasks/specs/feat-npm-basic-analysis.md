# Feature: NPM 基础分析与模块组合

状态：`[-]` 实施中，T0 接口与测试骨架、T1 会话基础已完成，T2 未开始；优先级：P0。
前置：[packet 契约](../archive/feat-npm-packet-contract.md)、[阶段化管线](../archive/feat-stage-filter-pipeline.md)。
后续：`npm-session-analysis`、`npm-protocol-analysis`。
实时接入依赖：`stream-time-drive`、`npm-capture-contract`、`npm-result-query`；基础引擎可先用模拟持续源验收。

## Non-Goals

- 本 Feature 不实现 RTT/重传等性能算法、TCP/IP 重组、应用交易解析、完整报文或正文存储。
- 不新增全局模块框架、第二套插件加载器、任意模块 DAG、异步分支、热插拔或多结果表输出。
- 不实现实时采集后端、多核执行器或无限保留的内存结果表；时间驱动扩展由独立框架 Feature 提供。
- 不通过 `basic(raw) → session(raw) → protocol(raw)` 传递并长期保留原始包；不修改既有框架 ABI。
- 首版会话结果覆盖可安全提取端点的 TCP/UDP；其他协议、缺少完整传输头的分片及畸形包不生成会话行。
  不实现隧道身份提取；遇到无法区分隧道上下文的封装流量明确报错，不按内层五元组合并。

## 业务意图与讨论结论

统一消费 pcapfile 有限流和实时采集持续流，在一个任务内完成会话归属、按会话采样识别和基础统计；
后续性能与交易模块同步访问当前包和同一会话视图。Feature 划分研发职责，SQL 算子提供统一执行入口。
按显式输入能力/配置选择离线或实时运行策略，不按通道名分支；离线 EOF 及时结束，在线持续低延迟输出。

| 层次/Feature | 职责与依赖 |
| --- | --- |
| 算子适配层 | 复用 `IBlockTransformOperatorV1 / IBlockTransformTaskV1`，负责配置、固定 Schema、Arrow 输入输出、取消和错误传播。 |
| Basic / 任务内引擎 | 必需基础能力；统一会话身份、方向、生命周期、NPI 采样与标签、基础计数、模块调用及资源预算。 |
| Session 模块（后续） | 复用 Basic，只拥有性能状态，不重建会话规则；UDP 无序列依据时不声称可计算通用丢包率。 |
| Protocol 模块（后续） | 复用 Basic，按需启用重组及增量解析；不依赖全部 Session 指标，Identify 停止不影响持续解析。 |

首版在一个 NPM `.so` 内实现，算子及外部 NPI 能力均按 IID 发现。模块契约先限定在 NPM 内部；独立模块
Provider 待真实部署需求出现后再设计。基础能力先于可选模块执行，组合在任务初始化时确定。

## 核心契约

### 会话与模块

| 类型 | 最小内容与所有权 |
| --- | --- |
| `NpmSessionKey` | 输入命名空间、观测域、地址族、传输协议、规范化双向端点对；会话实例与可复用五元组分离。观测域可包含接口上下文，采集队列/线程编号不自动成为隔离键。 |
| `NpmSessionView` | 任务内唯一 `session_id:uint64`、A/B 端点、起止时间、基础计数、识别状态及协议 ID；由引擎持有，模块只读。 |
| `NpmPacketView` | 元数据、已缓存层路径、借用字节、经 IP/传输长度及捕获边界校验的 payload、当前 A→B/B→A 方向。 |
| `NpmAnalysisConfig` | 离线/实时模式、最终/周期快照模式、输出周期、payload 采样上限、TCP/UDP idle timeout、活动会话与字节/输出预算、过载策略。 |
| 模块私有状态 | 按统一 `session_id` 保存；由模块创建和销毁，计入任务预算，不共享整份可变会话对象。 |

T0.2 冻结以下任务内配置；时间统一使用纳秒，字节范围使用二进制单位。离线默认最终结果，实时默认周期快照；
首版只接受严格失败过载策略，已知运行模式与结果模式可显式组合，未实现的过载值直接拒绝：

| 配置 | 默认值 | 合法范围 |
| --- | --- | --- |
| `output_interval_ns` | 1 秒 | 10 毫秒～1 小时 |
| `payload_sample_packets` | 8 | 1～64 |
| `tcp_idle_timeout_ns` / `udp_idle_timeout_ns` | 60 秒 / 30 秒 | 各 1 秒～24 小时 |
| `out_of_order_tolerance_ns` | 1 秒 | 0～60 秒 |
| `max_active_sessions` | 100,000 | 1～10,000,000 |
| `max_tracked_bytes` | 256 MiB | 1 MiB～1 TiB |
| `max_pending_output_bytes` | 64 MiB | 1 MiB～1 TiB |

`NpmObservationDomainMap` 由非空输入命名空间和至少一个显式 `source_id → observation_domain_id` 绑定组成；
`source_id` 在同一映射内唯一，多个采集队列可绑定同一观测域。未知 `source_id` 必须报错且不得按来源值、
队列号或哈希隐式生成观测域；因此同域跨队列可以归一，不同命名空间或不同观测域保持隔离。

T0.4 冻结的 `NpmSessionKey` 拥有输入命名空间，并保存观测域、地址族、传输协议和类型化 A/B 端点；
`NpmPacketView` 只借用现有 `PacketView`、一次解码得到的 `PacketLayerInfo`、已校验 payload span 和方向，
不包含 owner。`NpmSessionView` 只借用引擎拥有的会话键，并按值暴露起止时间、基础计数和识别状态。

T1.1 的 `BuildNpmSessionPacketBinding()` 只接受已选出完整 TCP/UDP 端点的 decoded/truncated layer，输出拥有
规范化会话键并借用当前 packet 的安全 payload；失败时不修改输出。A/B 端点按网络序地址字节、再按主机序
端口排序，不使用地址类型的主机端整数比较。函数显式解析观测域映射，复核 IPv4/IPv6 分片位置，并按 IP、
TCP/UDP 声明长度和捕获边界裁定 payload。所选网络层之前已有另一网络层时，因会话键不含隧道身份而返回
上下文错误；选择封装包自身完整的外层 TCP/UDP 不受该判据影响。

T1.2 的 `NpmSessionTable` 为单任务私有、无锁容器，以完整 `NpmSessionKey` 查找会话；首个实例 ID 从 1 开始
单调分配，正反向和同域跨来源的规范键复用同一实例。每次 Observe 只复制键和更新基础状态，不保存 packet、
payload 或 batch owner；视图中的键指针借用表内不可变键。包时间按最小/最大值更新，包数与 wire bytes 按方向
累计。达到 `max_active_sessions` 后拒绝新键但仍更新已有键；失败不修改输出或会话状态。本切片不删除会话，
协议状态保持 pending，生命周期推进与结束通知由 T1.3/T1.4 实现。

T1.3 通过显式 `AdvanceCaptureProgress()` 推进事件水位；候选水位为采集时间进度减乱序容忍，并与历史水位取
最大值。离线调用只依赖事件时间；实时调用还要求积压状态已知且无积压，并且本次有包或源空闲已明确确认，
否则分别返回稳定的延后状态。每个活跃会话在有序索引中只保留一个 `last_ns + idle_timeout` deadline，TCP/UDP
使用各自 timeout；水位到达 deadline 时移出活跃表并返回拥有键和基础状态的 idle 退役快照。早于当前水位的
包返回 `kLatePacket`，等于水位的包仍在容忍边界内；因此迟到包不能复活已退役实例，而水位之后的同键流量
可创建使用新 ID 的实例。机器处理速度、回放等待、墙上时钟和 Poll timeout 均不隐式调用或替代此进度契约。

T1.4 在已校验 TCP 头的 packet binding 中按值保存主机序 sequence 与 SYN/ACK/FIN/RST；UDP 不携带有效 TCP
控制事实。裸 SYN 定义为 SYN=1 且 ACK=0：初始裸 SYN 同方向、同 sequence 的重复包属于重传；中途抓包后
首次出现裸 SYN，或方向/sequence 变化时，旧实例以 `tuple_reuse` 退役，当前 SYN 作为新 ID 的首包；SYN+ACK
不单独构成新连接证据。单向 FIN（含同向重传）只记录半关闭，双向 FIN 或任意 RST 的当前包先计数再以
`closed` 退役。`NpmSessionObserveResult` 用活跃借用视图和拥有型结束快照表达上述两类结果；模块通知按结束
快照顺序及注册顺序同步执行，任意非零结果原样短路。

以下 NPM 内部纯虚接口不注册新的全局模块 IID：

```cpp
interface INpmAnalysisModule {
    virtual ~INpmAnalysisModule() = default;
    virtual int OnPacket(const NpmPacketView&, const NpmSessionView&,
                         INpmResultWriter&) = 0;
    virtual int OnSessionEnd(const NpmSessionView&, NpmSessionEndReason,
                             INpmResultWriter&) = 0;
};
```

`INpmResultWriter` 仅接受当前结果模式的类型化记录；首版写入 `NpmBasicResult`，不允许动态加列。传入结果引用
只在 `WriteBasic()` 调用期间有效，writer 必须在返回成功前复制或编码完成；非零返回表示没有接受该结果。
包/会话视图及其内部指针只在回调期间有效；模块不得保存，构造时注入任务独占的 `INpmTaskBudget`，不保存
输入 batch owner。模块和 writer 回调返回 0 表示成功，任意非零值原样成为任务错误，不定义正数控制状态。
模块间不直接访问私有状态；当前任务串行调用模块，取消由算子遵守既有并发契约并在退出处理后释放状态。
引擎负责时间推进和周期快照，模块不自建定时线程；后续模块需要周期结果时在对应 Feature 冻结类型化接口。
现有 `source_id` 可表示接口或队列，必须经输入契约映射到观测域；pcapfile 保留文件/接口隔离，实时源按
`npm-capture-contract` 映射。同一观测域跨队列的双向流量归同一会话，来源字段不能冒充全局会话身份。

会话不要求从握手开始；A/B 为规范端点顺序，不代表 client/server。起止时间取会话包时间的最小/最大值；
迟到包不复活已结束实例。离线 idle 按包事件时间进度推进，T0 固化乱序容忍；机器处理速度和回放等待不改变结果。
TCP 新连接证据、关闭及重传判据在 T1 固化，不能把重传 SYN 或单向 FIN 直接当作新建/完整关闭。
关闭、超时、五元组复用和正常 EOF 由引擎统一处理：先从活跃表退役为拥有型快照，模块通知期间保持快照
状态存活，通知完成后再释放快照；模块不得收到指向已删除表项的借用。

识别状态为 `pending / identified / unknown`，与 packet 的单次识别状态分开。仅前 N 个有效 payload 包
调用 `Identify`，单次未命中继续 pending；成功立即缓存标签并停止采样，耗尽窗口或会话结束仍未命中
则为 unknown。复用 layer path，不再次 Layer；NPI scratch/pipeno 必须按其并发约束独占分配或串行访问。

### 时间驱动与终结

| 输入事件/模式 | 行为 |
| --- | --- |
| 在线暂时无包 | runtime 按单调时钟触发维护及快照，任务继续等待；`kTimeout` 不是 EOF，也不是事件时间水位。 |
| 在线持续繁忙 | 批次之间检查到期时间，避免只在 Poll 超时时调度而饿死定时工作；单次维护工作量有界。 |
| 离线回放等待 | 保持运行，不使用墙上时间老化会话；不把等待解释为结束。 |
| 正常 EOF | 最后一个 batch 处理完成后立即调用一次终结性 `Flush`，结束剩余会话、排空最终结果并释放资源，不等待 idle timeout 或下一次 tick。 |
| Cancel/读取错误 | 按既有异常终止契约清理，不调用正常 Flush，不伪装成完整结果。 |

包时间用于观测指标，单调运行时钟用于调度；实时 idle 判定必须结合采集时间进度、乱序及积压信息。
无包时可按明确的源空闲确认推进清理；积压/源状态不明时不能只凭墙上时间断言网络会话空闲，资源回收需单独标记。
`npm-capture-contract` 冻结空闲确认/进度信息，NPM 不将一次 Poll timeout 当成该确认。

现有 V1 没有时间通知，runtime 对 `kTimeout` 直接继续轮询；`stream-time-drive` 提供可选版本化扩展，
同一任务线程串行调度，覆盖链式包装、结果背压与取消，保留 V1 ABI。时间通知可重复，不能通过重复 Flush 实现；
进入终结后不再通知时间。缺少此能力时不得宣称实时模式完整可用，不能依靠后台线程绕过输出契约。
T0.4 的 `NpmTimeCapabilities` 只声明逻辑门禁，不预定义该扩展的 IID 或版本：离线模式无需这些能力；实时模式
必须同时具备单调时间驱动、采集时间进度、源空闲确认和积压状态，缺少任一项即拒绝 Open。一次 Poll timeout
不等同于上述任一采集进度能力。
`eof` 只表示观测输入结束，不表示 TCP 正常关闭；后续 Protocol 应将未完成交易标记不完整并及时释放缓存。
及时结束不省略最终计算/写入；慢结果端仍受背压、超时和取消约束，不得静默丢弃尾部结果。

### 输出与内存

入口保留 `npm.basic`，输出无 `raw_data` 的 `NpmBasicResult`：离线默认最终模式，每个结束会话一行；
实时默认周期累计快照，活跃会话定期输出，结束时再输出最终版本。Open 固定 Schema，probe 与执行一致。
后续性能/交易模式由对应 Feature 冻结，拒绝尚未实现的选项；交易完成后及时输出，不必等 TCP 会话结束。

| 输出字段 | Arrow 类型/语义 |
| --- | --- |
| `session_id`, `observation_domain_id` | `uint64`, `uint64`；任务内会话及观测域标识，跨任务关联需结合任务标识。 |
| `revision`, `observed_at`, `is_final` | `uint64`, `int64`, `bool`；会话内递增版本、快照生成时间（Unix epoch 纳秒）、是否终结，调度仍使用单调时钟。 |
| `ip_family`, `transport_protocol` | `uint8`；地址族与 IP 传输协议号。 |
| `a_ip`, `b_ip`, `a_port`, `b_port` | `utf8`, `utf8`, `uint16`, `uint16`；规范文本 IP 与 A/B 端点。 |
| `first_ns`, `last_ns` | `int64`；Unix epoch 纳秒。 |
| `packets_ab`, `packets_ba`, `wire_bytes_ab`, `wire_bytes_ba` | `uint64`；计入会话全部包，包括识别前的包；字节使用 wire length。 |
| `protocol_status`, `protocol_id`, `protocol_sub_id`, `protocol` | `utf8`, nullable `uint16`, nullable `uint16`, nullable `utf8`；快照可 pending，最终 identified/unknown，名称来自 NPI 字典，未命中时 ID/名称为空。 |
| `end_reason` | nullable `utf8`；活跃时为空，终结为 `closed / idle_timeout / tuple_reuse / eof`；若启用资源驱逐须另增明确原因。 |

T0.3 冻结表中从上到下、同组从左到右的 22 列顺序；Schema metadata 为
`flowsql.entity=npm_basic_result`、`flowsql.schema_version=1`、`flowsql.timestamp_unit=ns`，且不包含
`raw_data`。`identified` 必须同时提供 `protocol_id` 和 `protocol`，`protocol_sub_id` 可空；`pending` 与
`unknown` 的三个协议字段均为空。活跃结果不得有 `end_reason`；最终结果必须有结束原因且不得保持 `pending`。

任务预算账本将会话状态、模块状态和当前借用输入三类字节合计到 `max_tracked_bytes`，待交付输出单独计入
`max_pending_output_bytes`；两类额度互不冒充。预留达到上限允许成功，超限时不修改账本；释放只能归还原类别
已计量的字节，释放不足同样不修改账本。正常批次与最终 `Flush` 输出使用同一待输出类别，不设置旁路额度。

快照累计计数不可直接跨版本求和；按任务/会话取最新版本或由 sink 更新，协议识别后的快照更新标签。
持续结果的版本处理、持久写入及保留期限归 `npm-result-query`，生产实时链路不得无限追加内存 DataFrame。

输入批次仅在当前处理期间借用，返回的结果不引用原始字节。需要跨包保留的 payload 必须按需复制到
有预算的缓存；后续 Protocol 负责有界识别前缀、乱序/缺口和解析不完整标记，完整大正文存储另行立项。
预算覆盖引擎会话/容器、模块缓存、当前输入的保守占用核算及全部待交付输出；不承诺限制 source 预先分配、
下游存储或整个进程 RSS。首版保留严格失败策略，不静默驱逐或丢结果；它不构成实时持续运行保障。
持续运行过载策略需在启用前冻结拒绝新会话/资源驱逐/停止负载解析的具体行为、计数及不完整标记，未实现时拒绝配置。
实时源不能靠背压保证不丢包；采集丢包、算子丢弃和网络丢包分别记录，未知值不记作零。
`Flush` 的全部输出同样计入预算，不能靠多个返回 batch 绕过上限；超限失败不宣称得到完整结果。

### 实时性能边界

- 采集批次同时限定包数、字节数及等待时长；引擎批量读取列、复用每包会话查找，输出时再转换地址/协议名。
- 避免逐包堆分配和全局锁，不逐包扫描全部会话；周期维护/快照分段执行，输出预算和背压下不承诺硬实时期限。
- 首版单分片建立基线；后续多核按双向会话分片并保持所需顺序，模块/NPI 状态分片独占，跨分片队列必须有界。
- 性能验收记录硬件、包长、模块组合、包速率/吞吐、P95/P99 输出延迟、积压、活动会话、内存峰值与过载恢复；
  覆盖小包、高新建率、长连接、无包、持续繁忙及慢 sink。数值门槛在性能切片开始前按部署目标冻结，不预先承诺线速。

## 主链路

1. **离线**：Open → packet 批次内会话归属/计数/采样识别/模块回调 → 按事件时间维护 → 返回精简结果，
   释放输入 → EOF 立即 Flush 剩余会话（`end_reason=eof`）→ 排空结果并完成，不等待会话自然超时。
2. **在线**：Open 校验持续源及时间驱动能力 → 同样的 packet 处理 → 无包/繁忙时均串行检查定时维护，
   输出带版本的快照并继续采集 → 源正常结束时按 EOF 收口；Cancel/错误直接异常清理，不做正常 Flush。

协议过滤作用于算子结果，不下推到原包以免丢失识别前的会话数据；周期模式过滤的是各版本快照。

目标 SQL（实施完成后可用）：
`SELECT * FROM pcapfile.capture USING npm.basic WHERE protocol = 'HTTP' INTO dataframe.basic_metrics`。

## Task Breakdown 与测试锚点

D0/D1 文档整理已完成，实施任务按以下依赖顺序推进；每次仅载入一个 10～30 分钟原子切片，
任务组超出时间盒时先在本规格拆分并冻结工作台，禁止把整个任务组自动连续实施。

- `[x]` T0 接口与测试骨架：冻结内部头文件、离线/实时配置、观测域映射、快照 Schema 和时间依赖；新增
  `test_npm_basic`，验证借用、固定 Schema 和预算。按以下原子切片实施，全部通过后才勾选 T0：
  - `[x]` T0.1：建立独立的 `test_npm_basic` CMake/CTest 测试目标骨架，不引入生产契约。
  - `[x]` T0.2：冻结核心枚举、配置默认值/合法范围和输入命名空间到观测域的映射契约，以失败断言先行。
  - `[x]` T0.3：冻结 `NpmBasicResult`、固定 Arrow Schema、nullable 语义及预算计量边界，以 Schema/预算断言收敛。
  - `[x]` T0.4：冻结报文/会话借用视图、模块/结果写入接口、时间能力依赖和错误约定，以生命周期断言收敛。
- `[x]` T1 会话基础：按以下原子切片实现双向归属、任务内会话状态与生命周期，全部通过后才勾选 T1：
  - `[x]` T1.1：从已解码 TCP/UDP packet 构造规范化双向会话键、方向与安全 payload；接入观测域映射，
    拒绝无效/不完整端点、非首分片和无法区分上下文的内层隧道流量。
  - `[x]` T1.2：实现任务内会话表、`session_id`、双向查找/计数与中途抓包；验证不同命名空间/观测域隔离、
    同域跨队列归一。
  - `[x]` T1.3：实现事件时间 idle、乱序容忍及采集进度/空闲/积压判据；已结束会话实例不得被迟到包复活。
  - `[x]` T1.4：冻结并实现 TCP SYN 重传/新连接证据、半关闭、完整关闭/RST、五元组复用和模块结束通知。
- `[ ]` T2 NPI 采样识别：假 identifier 断言无 payload 零调用、命中后零追加调用、最多 N 次、最终 unknown；
  真实 NPI 验证层复用及任务并发隔离。
- `[ ]` T3 算子与有界输出：IID/单 reader、最终/周期模式、版本与标签更新、owner 释放、预算/取消；假时钟证明
  无包/繁忙均维护，EOF 不等待 60 秒 idle 或 tick，回放速度不改变会话统计。
- `[ ]` T4 集成验收与收口：PCAP→npm.basic→DataFrame、模拟持续源→有界 sink、WHERE 与任务隔离；性能基线、
  慢 sink/过载证据、用户文档及全量回归。

T3 实时调度集成以 `stream-time-drive` 为前置，真实采集与长期存储分别由后续接入 Feature 验收，模拟源
通过不代表采集后端已完成。T3/T4 执行前按离线收口、实时快照、时间驱动接入、性能验收拆成原子切片。

实施期使用 `cmake -B build src`，新增目标后执行 `cmake --build build --target test_npm_basic -j$(nproc)`
及其 CTest 用例；Feature 收口执行 `cmake --build build -j$(nproc)`、`ctest --test-dir build --output-on-failure`。
任务勾选只表示对应原子切片已通过其工作台验收；未勾选任务仍是验收计划，不构成实现声明。
