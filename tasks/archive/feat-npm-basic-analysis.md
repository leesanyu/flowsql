# Feature: NPM 基础分析与模块组合

状态：`[x]` 已完成；离线生产能力与实时任务内引擎已交付，生产实时接线归后续 Feature；
优先级：P0。
前置：[packet 契约](../archive/feat-npm-packet-contract.md)、[阶段化管线](../archive/feat-stage-filter-pipeline.md)。
后续：`npm-basic-realtime-integration`、`npm-session-analysis`、`npm-protocol-analysis`。
生产实时接线归 `npm-basic-realtime-integration`，依赖本 Feature、`stream-time-drive` 与
`npm-capture-contract`；实时结果持久化/最新版本查询归 `npm-result-query`。

## Non-Goals

- 本 Feature 不实现 RTT/重传等性能算法、TCP/IP 重组、应用交易解析、完整报文或正文存储。
- 不新增全局模块框架、第二套插件加载器、任意模块 DAG、异步分支、热插拔或多结果表输出。
- 不实现实时采集后端、多核执行器或无限保留的内存结果表；时间驱动扩展由独立框架 Feature 提供。
- 不把模拟维护入口接入生产 Scheduler/source；生产实时 SQL 由后续 `npm-basic-realtime-integration` 交付。
- 不通过 `basic(raw) → session(raw) → protocol(raw)` 传递并长期保留原始包；不修改既有框架 ABI。
- 首版会话结果覆盖可安全提取端点的 TCP/UDP；其他协议、缺少完整传输头的分片及畸形包不生成会话行。
  不实现隧道身份提取；遇到无法区分隧道上下文的封装流量明确报错，不按内层五元组合并。

## 业务意图与讨论结论

以 `npm.basic` 任务内引擎统一处理离线/实时 packet 的会话归属、有限采样识别、基础统计和模块调用。
本 Feature 交付离线生产 provider，并用模拟时间/采集事实验证实时引擎；生产实时接线由后续 Feature 完成。

| 层次/Feature | 职责与依赖 |
| --- | --- |
| 算子适配层 | 复用 `IBlockTransformOperatorV1 / IBlockTransformTaskV1`；负责配置、固定 Schema、Arrow 输出、取消及错误。 |
| Basic / 任务内引擎 | 会话身份与生命周期、NPI 采样、基础计数、模块调用及预算。 |
| Session / Protocol（后续） | 复用 Basic 会话；分别拥有性能状态与按需协议解析，不在本 Feature 实现。 |

首版在一个 NPM `.so` 内实现，算子与外部 NPI 能力按 IID 发现；模块契约仅限 NPM 内部。

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

`WITH` 使用拥有型 JSON string 配置：必填 `input_namespace` 与 `source_domains`（十进制
`source_id:observation_domain_id`，以分号分隔）；其余字段复用上表，枚举仅接受已实现值。未知、重复、
非法或越界值拒绝，失败不修改 output；未指定模式时默认离线。

`NpmSessionKey` 拥有命名空间、观测域和规范化双向端点；`NpmPacketView` 只借用当前 packet、解码层路径、
已校验 payload 与方向；`NpmSessionView` 只借用会话键，按值提供计数和识别状态。

`BuildNpmSessionPacketBinding()` 只接受端点完整的 TCP/UDP；按网络序地址字节与主机序端口规范化双向键，
显式解析观测域，以 IP/传输层声明长度和捕获边界裁定借用 payload。无法区分上下文的内层隧道流量报错；
失败不修改输出。

任务私有 `NpmSessionTable` 按规范键复用双向实例；ID 从 1 单调递增，只保存键与基础状态，不保存 packet/
batch owner。时间取最小/最大值，包数和 wire bytes 按方向累计；新会话数量超限时拒绝且不修改输出。

`AdvanceCaptureProgress()` 的水位为历史最大值与“采集时间减乱序容忍”的最大值；实时还需已知无积压，
以及有包或明确空闲确认。按 TCP/UDP deadline 退役并返回拥有型快照；早于水位的包不得复活旧实例。
机器处理速度、回放等待与 Poll timeout 不推进采集时间。

TCP 裸 SYN 同方向同 sequence 为重传；新 SYN 证据使旧实例以 `tuple_reuse` 退役、新 ID 承接当前包。
单向 FIN 不关闭，双向 FIN 或 RST 当前包先计数再以 `closed` 退役；模块按结束快照和注册顺序通知。

每个会话仅尝试前 N 个非空 payload：空 payload 不调用 NPI，识别成功后停止采样，耗尽或结束时剩余
`pending` 变为 `unknown`；tuple reuse 的新 ID 拥有独立采样窗口。

`IProtocolPipelinePoolV1` 在 Option 冻结容量（1～16）、Load 分配各 `pipeno` scratch；任务私有
`NpmProtocolContext` 只按 IID Acquire/Release 独占 pipeline，复用已有层路径并借用词典。名称优先非零 sub ID，
否则使用主 ID；上下文不得越过 provider Stop/Unload。

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

`INpmResultWriter` 同步复制类型化 `NpmBasicResult`；packet/会话视图只在回调期间有效，模块不得持有 batch
owner。模块创建时注入任务级 `INpmTaskBudget`，私有状态按 `kModuleState` 预留和归还。模块串行调用，任何
非零回调结果成为任务错误；时间推进由引擎统一负责。

### 时间驱动与终结

| 输入事件/模式 | 行为 |
| --- | --- |
| 在线暂时无包 | runtime 按单调时钟触发维护及快照，任务继续等待；`kTimeout` 不是 EOF，也不是事件时间水位。 |
| 在线持续繁忙 | 批次之间检查到期时间，避免只在 Poll 超时时调度而饿死定时工作；单次维护工作量有界。 |
| 离线回放等待 | 保持运行，不使用墙上时间老化会话；不把等待解释为结束。 |
| 正常 EOF | 最后一个 batch 处理完成后立即调用一次终结性 `Flush`，结束剩余会话、排空最终结果并释放资源，不等待 idle timeout 或下一次 tick。 |
| Cancel/读取错误 | 按既有异常终止契约清理，不调用正常 Flush，不伪装成完整结果。 |

包时间用于指标，单调时钟用于调度。实时 Open 必须具备单调时间、采集进度、源空闲确认和积压状态；缺少
任一能力即拒绝，Poll timeout 不冒充进度。版本化通知由 `stream-time-drive` 提供，源事实由
`npm-capture-contract` 提供，`npm-basic-realtime-integration` 负责生产适配；`eof` 仅表示输入结束，
不表示 TCP 正常关闭。

### 输出与内存

`npm.basic` 输出无 `raw_data` 的固定 `NpmBasicResult`；离线默认每个结束会话一行最终结果，实时默认周期累计
快照并在结束时输出最终版本。Open 的 probe 与执行 Schema 必须一致。

| 输出字段 | Arrow 类型/语义 |
| --- | --- |
| `session_id`, `observation_domain_id` | `uint64`, `uint64`；任务内会话及观测域标识，跨任务关联需结合任务标识。 |
| `revision`, `observed_at`, `is_final` | `uint64`, `int64`, `bool`；会话内递增版本、快照生成时间（Unix epoch 纳秒）、是否终结，调度仍使用单调时钟。 |
| `ip_family`, `transport_protocol` | `uint8`；地址族与 IP 传输协议号。 |
| `a_ip`, `b_ip`, `a_port`, `b_port` | `utf8`, `utf8`, `uint16`, `uint16`；规范文本 IP 与 A/B 端点。 |
| `first_ns`, `last_ns` | `int64`；Unix epoch 纳秒。 |
| `packets_ab`, `packets_ba`, `wire_bytes_ab`, `wire_bytes_ba` | `uint64`；计入会话全部包，包括识别前的包；字节使用 wire length。 |
| `protocol_status`, `protocol_id`, `protocol_sub_id`, `protocol` | 非空 `utf8`；ID 均可空 `uint16`；名称为可空 `utf8`，来自 NPI 字典。 |
| `end_reason` | nullable `utf8`；活跃时为空，终结为 `closed / idle_timeout / tuple_reuse / eof`；若启用资源驱逐须另增明确原因。 |

表中从上到下、同组从左到右固定为 22 列；metadata 为 `flowsql.entity=npm_basic_result`、
`flowsql.schema_version=1`、`flowsql.timestamp_unit=ns`。最终结果不得为 `pending` 且必须有 `end_reason`；
`identified` 必须有主协议 ID 和名称，`pending/unknown` 不携带协议字段。
周期快照计数为累计值；同一任务/会话的多个 revision 不得直接求和，消费端按最新 revision 更新或查询。

会话、模块和当前借用输入合计到 `max_tracked_bytes`；待交付输出单独计入 `max_pending_output_bytes`，正常批次
与 Flush 不设旁路额度。达到上限允许，超限或释放不足不修改账本。结果不得引用输入；需跨包保留的字节必须
复制到有预算的状态。首版过载只支持严格失败，不静默驱逐或丢结果；持久化和快照版本查询归
`npm-result-query`。

## 主链路

1. **离线**：Open → packet 批次内会话归属/计数/采样识别/模块回调 → 按事件时间维护 → 返回精简结果，
   释放输入 → EOF 立即 Flush 剩余会话（`end_reason=eof`）→ 排空结果并完成，不等待会话自然超时。
2. **生产在线（后续接线必须满足）**：Open 校验持续源及时间驱动能力 → 同样的 packet 处理 → 无包/繁忙时
   均串行检查定时维护，输出带版本的快照并继续采集 → 源正常结束时按 EOF 收口；Cancel/错误直接异常清理，
   不做正常 Flush。

协议过滤作用于算子结果，不下推到原包以免丢失识别前的会话数据；周期模式过滤的是各版本快照。

目标 SQL（实施完成后可用）：
`SELECT * FROM pcapfile.capture USING npm.basic WITH input_namespace='pcapfile.capture',source_domains='0:7'
WHERE protocol = 'HTTP' INTO dataframe.basic_metrics`（映射值需与实际输入的 source ID 对齐）。

## Task Breakdown 与测试锚点

保持 WIP=1；叶子任务的文件清单、具体断言和命令只写入 `tasks/active_task.md`。以下保留依赖和完成状态：

- `[x]` T0 契约与测试骨架
  - `[x]` T0.1 建立 `test_npm_basic` CMake/CTest 目标。
  - `[x]` T0.2 冻结配置、枚举和观测域映射。
  - `[x]` T0.3 冻结结果 Schema、nullable 语义和预算类别。
  - `[x]` T0.4 冻结借用视图、模块接口和时间能力门禁。
- `[x]` T1 会话基础
  - `[x]` T1.1 构造规范化 TCP/UDP 会话键、方向和安全 payload。
  - `[x]` T1.2 实现任务私有会话表、双向计数和隔离。
  - `[x]` T1.3 实现事件水位、idle deadline 和迟到包规则。
  - `[x]` T1.4 实现 SYN/reuse、FIN/RST 生命周期和结束通知。
- `[x]` T2 NPI 采样识别
  - `[x]` T2.1 实现每会话有限 payload 采样与最终识别状态。
  - `[x]` T2.2 接入真实 NPI 与词典
    - `[x]` T2.2.1 实现版本化 pipeline pool 和独占租约。
    - `[x]` T2.2.2 实现按 IID 获取的任务级 RAII 协议上下文。
- `[x]` T3 算子与有界输出
  - `[x]` T3.1 实现固定 Packet RecordBatch 借用解码。
  - `[x]` T3.2 实现 Basic 结果投影与 Arrow 编码
    - `[x]` T3.2.1 投影会话、协议标签和 revision。
    - `[x]` T3.2.2 编码固定 22 列 Arrow Schema。
    - `[x]` T3.2.3 以输出 owner 管理 `kPendingOutput` 租约。
  - `[x]` T3.3 实现批次处理原语和正常 EOF 收口
    - `[x]` T3.3.1 EOF 全量退役剩余会话。
    - `[x]` T3.3.2 逐包处理及离线事件时间推进
      - `[x]` T3.3.2.1 单 packet binding、会话、识别与模块回调。
      - `[x]` T3.3.2.2 RecordBatch 循环、行号错误和结束事件聚合。
    - `[x]` T3.3.3 统一结果收集与 EOF 排空
      - `[x]` T3.3.3.1 收集模块/基础结果并统一预算化编码。
      - `[x]` T3.3.3.2 单次 EOF Flush 和终态约束。
  - `[x]` T3.4 完成 `npm.basic` 离线 provider
    - `[x]` T3.4.1 解析并复制 `WITH` 任务配置。
    - `[x]` T3.4.2 创建固定 Schema 和任务私有 Open runtime
      - `[x]` T3.4.2.1 创建共享、线程安全的任务预算账本。
      - `[x]` T3.4.2.2 原子聚合 NPI、会话、结果和 EOF 资源；不含处理期预算接线。
    - `[x]` T3.4.3 完成处理期预算与离线 Process/Flush runtime
      - `[x]` T3.4.3.1 将活动会话/deadline 接入共享 `kSessionState` 预算。
      - `[x]` T3.4.3.2 按实际 Arrow 字节借用输入并处理、排空一个离线 batch。
      - `[x]` T3.4.3.3 将正常 EOF 接入 runtime，失败不得伪装完整或重试。
    - `[x]` T3.4.4 实现并发 Cancel、稳定 LastError 和资源终结。
    - `[x]` T3.4.5 接入完整 task/provider 与生产插件
      - `[x]` T3.4.5.1 接入五个完整 task 方法、配置复制和 provider owner。
      - `[x]` T3.4.5.2 注册 IID、生成 `.so` 并验证插件生命周期。
  - `[x]` T3.5 实现版本化实时引擎原语与模拟时间验收
    - `[x]` T3.5.1 输出稳定有序的活动会话借用视图。
    - `[x]` T3.5.2 以模拟时间和采集事实驱动周期维护与版本化输出。
- `[x]` T4 集成收口
  - `[x]` T4.1 验证 pcapfile → npm.basic → dataframe 离线 SQL 主链路。
  - `[x]` T4.2 验证实时模拟、任务隔离及慢 sink/预算边界。
  - `[x]` T4.3 完成性能基线、文档、标准部署接线与 Feature 全量回归。

实施期只运行 `active_task.md` 冻结的定向 target；Feature 收口才执行全量构建和完整 CTest。生产实时适配、
实时采集后端、持久化和多核执行分别归 `npm-basic-realtime-integration`、`npm-capture-contract`、
`npm-result-query` 与后续 Feature，不在 T3/T4 内补做。

## 完成证据

- 默认基准处理 51,200 包并产生 51,200 行；吞吐仅作机器相关观测，不作为跨环境门槛。
- 完整构建零 Error，CTest 13/13 通过；离线 Scheduler E2E 和三套标准部署契约均已覆盖。
- 生产实时适配未混入本 Feature，由 `npm-basic-realtime-integration` 在两个前置 Feature 完成后交付。
