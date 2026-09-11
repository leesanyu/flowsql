# Feature: NPM 基础分析与模块组合

状态：`[-]` 规格已建立，实施未开始；优先级：P0。
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
| `NpmAnalysisConfig` | 离线/实时模式、最终/周期快照模式、输出周期、payload 采样上限、TCP/UDP idle timeout、活动会话与字节/输出预算、过载策略；T0 固化类型、默认值和合法范围。 |
| 模块私有状态 | 按统一 `session_id` 保存；由模块创建和销毁，计入任务预算，不共享整份可变会话对象。 |

以下为 NPM 内部头文件的最小接口草案，T0 冻结完整类型及错误约定后再实现；不注册新的全局模块 IID：

```cpp
interface INpmAnalysisModule {
    virtual ~INpmAnalysisModule() = default;
    virtual int OnPacket(const NpmPacketView&, const NpmSessionView&,
                         INpmResultWriter&) = 0;
    virtual int OnSessionEnd(const NpmSessionView&, NpmSessionEndReason,
                             INpmResultWriter&) = 0;
};
```

`INpmResultWriter` 仅接受当前结果模式的类型化记录；首版写入 `NpmBasicResult`，不允许动态加列。
包/会话视图只在回调期间有效；模块构造时注入任务预算服务，不保存输入 batch owner。
模块间不直接访问私有状态；当前任务串行调用模块，取消由算子遵守既有并发契约并在退出处理后释放状态。
引擎负责时间推进和周期快照，模块不自建定时线程；后续模块需要周期结果时在对应 Feature 冻结类型化接口。
现有 `source_id` 可表示接口或队列，必须经输入契约映射到观测域；pcapfile 保留文件/接口隔离，实时源按
`npm-capture-contract` 映射。同一观测域跨队列的双向流量归同一会话，来源字段不能冒充全局会话身份。

会话不要求从握手开始；A/B 为规范端点顺序，不代表 client/server。起止时间取会话包时间的最小/最大值；
迟到包不复活已结束实例。离线 idle 按包事件时间进度推进，T0 固化乱序容忍；机器处理速度和回放等待不改变结果。
TCP 新连接证据、关闭及重传判据在 T1 固化，不能把重传 SYN 或单向 FIN 直接当作新建/完整关闭。
关闭、超时、五元组复用和正常 EOF 由引擎统一处理，通知模块后再删除会话。

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

D0/D1 文档整理已完成，以下均未实施。按依赖顺序执行，每次仅载入一个 10～30 分钟原子切片；
任务组超出时间盒时先在本规格拆分并冻结工作台，禁止把整个任务组自动连续实施。

| 状态 | 任务组 | 契约/可执行验收锚点 |
| --- | --- | --- |
| [ ] | T0 接口与测试骨架 | 冻结内部头文件、离线/实时配置、观测域映射、快照 Schema 和时间依赖；新增 `test_npm_basic`，验证借用、固定 Schema 和预算。 |
| [ ] | T1 会话基础 | 双向归属、不同观测域隔离/同域跨队列归一、中途抓包、idle/复用、SYN 重传/半关闭、乱序与积压判据、无效包及隧道处理。 |
| [ ] | T2 NPI 采样识别 | 假 identifier 断言无 payload 零调用、命中后零追加调用、最多 N 次、最终 unknown；真实 NPI 验证层复用及任务并发隔离。 |
| [ ] | T3 算子与有界输出 | IID/单 reader、最终/周期模式、版本与标签更新、owner 释放、预算/取消；假时钟证明无包/繁忙均维护，EOF 不等待 60 秒 idle 或 tick，回放速度不改变会话统计。 |
| [ ] | T4 集成验收与收口 | PCAP→npm.basic→DataFrame、模拟持续源→有界 sink、WHERE 与任务隔离；性能基线、慢 sink/过载证据、用户文档及全量回归。 |

T3 实时调度集成以 `stream-time-drive` 为前置，真实采集与长期存储分别由后续接入 Feature 验收，模拟源
通过不代表采集后端已完成。T3/T4 执行前按离线收口、实时快照、时间驱动接入、性能验收拆成原子切片。

实施期使用 `cmake -B build src`，新增目标后执行 `cmake --build build --target test_npm_basic -j$(nproc)`
及其 CTest 用例；Feature 收口执行 `cmake --build build -j$(nproc)`、`ctest --test-dir build --output-on-failure`。
上述是验收计划，当前没有代码或测试通过的声明。
