# Feature: NPM TCP/UDP 会话性能分析

状态：`[x]` 已完成（完整 CTest 13/13 通过）
优先级：P1
前置 Feature：`npm-basic-analysis`、`stream-time-drive`（均已完成）
后续 Feature：`npm-protocol-analysis`、`npm-result-query`、`npm-basic-realtime-integration`

## Non-Goals

- 不建立第二套五元组、方向、`session_id`、协议识别或会话超时；Session 模块只挂载到 `npm.basic` 的唯一
  会话实例。
- 不输出网络丢包数/丢包率。TCP sequence 缺口、乱序、抓包重复、截断或捕获点漏包都不能单独证明网络
  丢包；未来采集 drop counter 也只代表采集损失。
- 不做 IP 分片重组、TCP 字节流重组、DNS/HTTP/TLS/ICMP 解析或事务关联；这些归协议分析 Feature。
- 不实现结果数据库、历史/最新 revision 查询、保留策略或多实体持久化 fan-out；本 Feature 只保留可供
  `npm-result-query` 消费的类型化路由语义。
- 不交付生产实时 source/capture progress 接线、多核分片、跨任务会话合并、双点测量、主动探测或应用
  client/server 角色推断。
- 不增加全局模块 IID、第二套插件加载器或任意模块 DAG；Session 模块与 Basic 引擎仍属于同一
  `npm.basic` task/capability。

## 业务意图

流量分析用户可以在一个 `npm.basic` 任务内启用基础结果与 Session 性能模块，复用一次 packet 解码、会话化、
方向判定、有限 NPI 识别和生命周期处理，得到可过滤的类型化 TCP/UDP 会话性能快照或终态。

启用什么与前台看什么相互独立：`features` 决定任务内实际运行及未来持久化消费的结果模块，`observing`
只决定当前 Block Transform 的单一 Arrow 输出。每个指标都限定为“当前捕获点实际观察到的 packet”，并用
nullable 值、状态和有效性标志区分不可用、证据不足与有效测量。

## 核心契约

### 模块选择与输出观察

`WITH` 新增两个拥有型字符串参数：

| 参数 | V1 语义 |
| --- | --- |
| `features` | 逗号分隔、去除 token 两侧 ASCII 空白的集合；本 Feature 接受 `basic`、`session`，未知、空或重复 token 拒绝。省略时为 `basic`。 |
| `observing` | 恰好一个结果实体；本 Feature 接受 `basic`、`session`，省略时为 `basic`，且必须包含在 `features` 中。 |
| `session_max_tcp_ranges_per_direction` | 十进制整数，默认 1024、范围 8～65536，仅 `features` 含 `session` 时接受；限制每会话每方向 sequence/ACK ledger range 数。 |

任务内会话/NPI/lifecycle 核心不是 feature token：任一合法非空 `features` 都只执行一次核心处理；`basic`
控制 `npm_basic_result` 生成，`session` 控制性能模块状态和 `npm_session_result` 生成。`observing` 不得隐式启用
模块，也不得改变模块状态、维护 deadline、终结事件或未来持久化消费。

Open/probe 根据 `observing` 返回固定单实体 Schema；`WHERE` 只作用于该实体，`INTO dataframe.*` 也只接收
该实体。全部 enabled entity 仍进入同步类型化结果路由；当前没有持久化 consumer 时，非 observing 记录不进入
前台 pending 队列，路由完成即可释放且任务结束后不留存。未来持久化 consumer 必须接收全部 enabled entity，
不能受 `observing` 影响。

兼容规则：旧 SQL 不提供两参数时等价于 `features='basic', observing='basic'`；只提供
`observing='session'` 会因 Session 未启用而拒绝。最小新链路为：

```sql
SELECT *
FROM pcapfile.capture
USING npm.basic
WITH input_namespace='pcapfile.capture', source_domains='0:1',
     features='basic,session', observing='session'
WHERE protocol = 'HTTP'
INTO dataframe.session_metrics
```

该语句只做一次基础分析，同时生成 Basic 与 Session 类型化事件；当前 DataFrame 仅得到满足 Session Schema
过滤条件的性能结果。它不会创建两个算子、重复读包或因 `observing=session` 才启用 Session。

### 经校验的传输层事实

Session 模块不得从 `raw_data` 重复解析 IP/TCP/UDP。Basic binding 在既有边界校验中一次性形成借用事实：

```cpp
struct NpmTransportPacketFacts {
    uint32_t payload_wire_bytes;
    uint32_t payload_captured_bytes;
    bool payload_complete;
    NpmTcpPacketFacts tcp;  // TCP 时 valid，含 flags/seq/ack/window
};
```

`NpmTcpPacketFacts` 至少包含 `valid`、SYN/ACK/FIN/RST、主机序 `sequence/acknowledgment/window`；逻辑 payload
长度来自经校验的 IP/transport 声明边界，captured 长度来自当前借用 span。事实与 `NpmPacketView` 同寿命，
模块不得持有 packet、layer、span 或 batch owner；需跨包保存的 sequence range/timestamp 必须复制并计入
`kModuleState`。

TCP 每方向以 32 位 serial arithmetic 和有界 range ledger 处理 wrap、重叠与累计 ACK；每包工作量、range
数量和内存受 `session_max_tcp_ranges_per_direction`/`max_tracked_bytes` 约束，超限按既有
`overload_policy=fail` 明确失败，不静默丢状态。
UDP 只累计观测到的 datagram/payload，不伪造连接、ACK RTT、重传或 unique TCP byte 指标。

### 指标口径与有效性

- `duration_ns = last_ns - first_ns`；仅在正 duration 上计算累计平均 bps。周期快照从会话首包累计，不表示
  最近窗口速率，多个 revision 不得相加。
- `wire_bps` 使用 Basic 已累计 wire bytes；`payload_bps` 使用观测包的 transport 声明 payload；TCP
  `unique_payload_bps` 使用 sequence range 去重后的 payload bytes。方向始终为规范 A→B/B→A。
- TCP handshake duration 为首个有效裸 SYN 到完成握手 ACK 的捕获时间差；`synack_rtt` 为未发生 SYN
  重传歧义时 SYN 到匹配 SYN-ACK 的时间差。中途开始、时间回退或配对歧义时数值为空并给出状态。
- TCP data RTT 只从当前捕获点同时看到的原始发送与累计 ACK 取样；遵循 Karn 原则排除发生重传的 sequence
  range，每个 ACK 至多贡献一个样本。输出 sample count 与 min/mean/max，不从单向包或时间回退猜测。
- TCP retransmission 只统计与已观察 sequence range 重叠的 packet/逻辑 payload bytes，并显式称为当前
  捕获点的重传观察；未见重叠输出 0，sequence 缺口绝不转换为 loss。

`measurement_flags:uint32` V1 位定义为：`1=midstream_start`、`2=truncated_payload`、
`4=timestamp_regression`、`8=sequence_ambiguous`、`16=syn_retransmitted`。无法直接观察的捕获漏包不设置伪标志；
Schema metadata 固定 `flowsql.measurement_scope=single_capture_observed_packets`。

V1 状态集合固定为：`rate_status=valid/insufficient_span`；
`tcp_handshake_status=complete/partial/not_observed/ambiguous/not_applicable`；
`tcp_rtt_status=valid/no_sample/ambiguous/not_applicable`；
`tcp_retransmission_status=valid/ambiguous/not_applicable`。`ambiguous/not_applicable` 对应的 sequence 派生数值为空；
TCP 的合法零重传必须是 `valid` 加 0，不能伪装为不可用。

### `NpmSessionResult` V1 Schema

| 字段组 | Arrow 类型与语义 |
| --- | --- |
| 身份/版本 | `session_id, observation_domain_id, revision:uint64`；`observed_at:int64`；`is_final:bool`。revision 是该实体/会话内从 1 递增的累计版本。 |
| 会话键 | `ip_family, transport_protocol:uint8`；`a_ip,b_ip:utf8`；`a_port,b_port:uint16`；`first_ns,last_ns,duration_ns:int64`。 |
| 标签/终结 | `protocol_status:utf8`；`protocol_id,protocol_sub_id:uint16?`；`protocol:utf8?`；`end_reason:utf8?`，仅终态非空。 |
| 累计流量 | `packets_ab,packets_ba,wire_bytes_ab,wire_bytes_ba,payload_bytes_ab,payload_bytes_ba:uint64`。 |
| 速率 | `rate_status:utf8`；`wire_bps_ab,wire_bps_ba,payload_bps_ab,payload_bps_ba:float64?`；正 duration 才非空。 |
| TCP unique | `tcp_unique_payload_bytes_ab,tcp_unique_payload_bytes_ba:uint64?`；`tcp_unique_payload_bps_ab,tcp_unique_payload_bps_ba:float64?`，UDP 为空。 |
| TCP 建连 | `tcp_handshake_status:utf8`；`tcp_initiator:utf8?`（`a/b`）；`tcp_handshake_duration_ns,tcp_synack_rtt_ns:int64?`。 |
| TCP RTT | `tcp_rtt_status:utf8`；`tcp_rtt_samples:uint64?`；`tcp_rtt_min_ns,tcp_rtt_mean_ns,tcp_rtt_max_ns:int64?`。 |
| TCP 重传 | `tcp_retransmission_status:utf8`；`tcp_retrans_packets_ab,tcp_retrans_packets_ba,tcp_retrans_payload_bytes_ab,tcp_retrans_payload_bytes_ba:uint64?`。 |
| 证据 | `measurement_flags:uint32`；数值不可用时必须为空，不用 0 代替；UDP 的 TCP status 为 `not_applicable`。 |

字段按表中组和组内顺序固定；metadata 为 `flowsql.entity=npm_session_result`、`flowsql.schema_version=1`、
`flowsql.timestamp_unit=ns`、`flowsql.revision_semantics=cumulative`。终态不得保留 `protocol_status=pending`；
状态枚举、nullable 组合、非负计数、方向合计和 final/end_reason 关系必须由 validator 先于 Arrow 编码校验。

### 生命周期、预算与失败

Session 模块只在 `features` 含 `session` 时创建；每包在唯一会话 Observe/NPI 后串行回调。周期维护向全部 enabled
模块发送同一活动会话快照事件，正常 closed/idle/tuple reuse/EOF 发送一次终态事件；模块结果走类型化 writer。
错误或 Cancel 释放状态但不伪造终态，EOF 正常 Flush 必须排空 observing 的最终结果。

任务内接口在 T0 冻结为：保留 `OnPacket`；新增
`OnSessionSnapshot(const NpmSessionView&, int64_t observed_at_ns, INpmResultWriter&)`；现有 `OnSessionEnd` 增加
`observed_at_ns`；writer 新增 `WriteSession(const NpmSessionResult&)`。这些接口不注册全局 IID，所有借用参数只在
同步调用内有效；任一非零返回值按既有规则使整个 task 失败。

每个 session ID 拥有独立性能状态，tuple reuse 不继承 sequence/RTT；状态、range ledger 与 revision map 计入
`kModuleState`，前台 Arrow owner 计入 `kPendingOutput`。达到预算上限允许，超限失败且账本不变；observing
不得让未观察模块绕过状态预算，也不得为其建立无限 pending output。

## 主链路

1. **离线多模块**：解析并验证 features/observing → 一次 packet binding/会话/NPI → Basic 与 Session 按 enabled
   集合处理 → Session 在结束/EOF 生成累计终态 → typed router 只把 observing Schema 送入 WHERE/DataFrame。
2. **周期快照**：数据或时间通知推进唯一任务 → 全部 enabled 模块接收同一 snapshot/end 事件并维护 revision →
   observing 结果受既有背压保护；非 observing 结果可被持久化 consumer 消费，当前无 consumer 时有界释放。

## Feature Tasks 与测试锚点

- `[x]` T0：冻结并以测试锚定 features/observing、经校验 transport facts、Session 结果 Schema 和模块快照接口，
  为后续实现提供不会因输出选择而改变启用模块的客观契约。
  - `[x]` T0.1：旧默认、合法集合、未知/重复/空 token、observing 未启用及 probe Schema 有断言。
  - `[x]` T0.2：TCP/UDP 完整与截断事实、nullable/status/metadata/validator 有断言。
- `[x]` T1：交付任务私有且受预算约束的 TCP/UDP 性能状态，使连接、RTT、重传和吞吐指标具有可复核依据。
  - `[x]` T1.1：握手/中途开始/SYN 重传、ACK RTT/Karn/时间回退有确定性测试。
  - `[x]` T1.2：sequence wrap/重叠/乱序、TCP unique bytes、UDP 不适用及正 duration 速率有确定性测试。
- `[x]` T2：把 Session 模块和类型化路由接入唯一 `npm.basic` runtime，使模块启用、前台观察、快照终结与
  预算生命周期正交且无重复会话化。
  - `[x]` T2.1：按 `features` 创建任务私有模块并复用唯一 binding/Observe/NPI 链路，使 `observing` 不改变
    模块启用、状态维护或基础分析执行次数。
  - `[x]` T2.2：让全部 enabled entity 进入同步类型化结果路由，并且只将 `observing` 实体保留为前台 Arrow
    输出，使输出选择不改变分析执行且非 observing 结果不会形成无界积压。
  - `[x]` T2.3：完成 realtime 周期快照与 idle 终结链路，使全部 enabled 实体共享同一活动会话事件和连续
    revision，并保证 observing 选择不影响非观察实体的执行、恰好一次终结与状态释放。
  - `[x]` T2.4：完成普通 closed 的数据包驱动终结链路，使 RST/双向 FIN 当前包进入同一会话后生成恰好一次
    终态，并保证最终协议状态、周期 revision 连续与多实体状态/预算释放正确。
  - `[x]` T2.5：完成 tuple reuse 的数据包驱动实例切换链路，使旧实例先以连续 revision 终结、replacement 当前包
    只进入 revision 从 1 开始的新实例，并保证两实例协议、性能、结果和预算隔离。
  - `[x]` T2.6：完成 EOF 正常排空链路，使全部活动会话生成连续且恰好一次的 EOF 终态，observing 输出被完整
    排空且重复 Flush 不产生重复结果。
  - `[x]` T2.7：完成 module、writer、编码、背压失败及 Cancel 的清理链路，使异常路径不伪造终态，并按 Arrow
    owner 生命周期释放 session、module、revision 和 pending-output 预算。
- `[x]` T3：完成离线 SQL、模拟实时、兼容与性能验收，让用户可直接查询 Session Schema 且 basic-only 路径
  保持原行为和固定 Schema。
  - `[x]` T3.1：pcapfile SQL 验证 WHERE 作用于 observing=session，非 observing 不进入 DataFrame。
  - `[x]` T3.2：记录 basic-only 与 basic+session 基准、峰值状态/range；完整构建和全部 CTest 通过。

实施期保持 WIP=1；接口/struct/Schema 先于算法实现冻结。Feature 完成时至少运行：

```bash
cmake -B build src
cmake --build build --target test_npm_basic test_scheduler_e2e -j$(nproc)
ctest --test-dir build -R '^(test_npm_basic|test_scheduler_e2e)$' --output-on-failure
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
git diff --check
```

## 完成证据

- T1：task-private tracker 已覆盖 TCP handshake、ACK RTT/Karn、32 位 serial arithmetic、unique/retransmission
  ledger，以及 TCP/UDP payload 和累计速率；全部动态状态计入 `kModuleState`，定向 `test_npm_basic` 通过。
- T2.1：四种有效 features/observing 配置均复用同一 packet binding、SessionTable Observe 和 NPI 路径；
  basic-only 不创建 Session module，其他三种配置各创建一个任务私有 module，observing 不改变启用状态；
  Cancel 与 runtime 失败释放 module/session 预算，定向 `test_npm_basic` 通过。
- T2.2：Basic/Session typed writer 只保留 observing pending，enabled 非 observing 结果经校验后同步释放；runtime
  按 task feature 配置构造 collector，四种合法配置的 Process 均返回 observing 对应的 0 行 batch，Flush 均返回
  同一 Schema 的 1 行 EOF 终态；Basic 固定 22 列、Session 固定 49 列，定向 `test_npm_basic` 通过。
- T2.3：四种合法 features/observing 配置均以唯一 `ProcessNpmPacket()` 主链路建立会话和 Session tracker；周期
  快照对全部 enabled 实体推进连续 revision，只输出 observing Schema。source idle confirmed 且无 backlog 时，
  idle 终态从周期 revision 连续递增并恰好输出一次；重复 progress 不重复终结，SessionTable、Basic projector、
  Session module 与 pending-output 预算均按生命周期释放，定向 `test_npm_basic` 通过。
- T2.4：普通 closed 已覆盖 RST/双向 FIN 与四种合法 features/observing 配置；同一实例先生成 periodic
  revision 1、2，再由终结包触发 revision 3 closed 终态。单向 FIN 保持 active，终结包计入 packet/wire/payload/
  TCP unique payload 累计；Basic/Session 保持已识别协议与同一 session ID，非 observing Session 也同步执行。
  重复 Drain/周期维护不重放终态，SessionTable、Basic revision、Session tracker/revision 与 pending-output 预算
  按内部状态及 Arrow owner 生命周期释放，定向 `test_npm_basic` 通过。
- T2.5：tuple reuse 已覆盖四种合法 features/observing 配置、active replacement 与 immediate-closed
  replacement；周期场景中旧实例 revision 1、2 后以 tuple_reuse revision 3 恰好终结，replacement bare SYN
  只计入新 session ID 并从 revision 1 重新起算，随后 closed 为 revision 2。两实例协议、packet/wire/payload/
  TCP unique payload、Basic/Session revision 及状态相互隔离；非 observing Session 同步执行，跨实例 Arrow owner
  不阻碍旧状态释放，内部状态与 pending-output 预算均按生命周期释放，定向 `test_npm_basic` 通过。
- T2.6：EOF 已覆盖四种合法 features/observing 配置、final-only 单会话和周期双会话排空；TCP/UDP 活动会话
  先各生成 revision 1、2，再由一次 Flush 按 session ID 顺序完整输出两条 revision 3 EOF 终态。各会话协议、
  packet/wire/payload/transport 性能累计相互隔离，非 observing Session 同样维护周期状态并在 EOF 释放；成功后
  runtime 进入 flushed terminal，session/module/revision/pipeline 资源立即释放，仅已交付 Arrow owner 保留输出预算。
  重复 Flush 与后续 Process 不改变输出或重放终态，定向 `test_npm_basic` 通过。
- T2.7：三种 Session-enabled 配置以真实 TCP range 上限触发 module `ENOSPC`，四种合法配置分别覆盖 Basic/
  Session observing 的周期编码背压失败，并在四种配置各持有 revision 1 非终态 Arrow owner 时 Cancel。失败路径
  保持调用方输出、错误和 failed 终态稳定，Cancel 不生成 closed/EOF；session、module、revision、collector 与
  pipeline 立即释放，调用方预占或 Arrow owner 持有的 pending-output 预算按各自生命周期保留并最终归零。
  既有 writer/encoder/collector/flusher 原子性和并发 Cancel 测试共同通过，定向 `test_npm_basic` 通过。
- T3.1：真实 pcapfile → Scheduler → 动态加载 `npm.basic` → DataFrame 链路执行
  `features='basic,session', observing='session'`，并以 Session 独有的
  `WHERE rate_status = 'insufficient_span'` 成功绑定和过滤。单包 TCP RST 输出恰好一个 49 列
  `npm_session_result` closed 终态，Session Schema metadata、revision、duration、TCP status 与 measurement flags
  均通过断言；同任务启用但未 observing 的 Basic 不产生额外 DataFrame 行或混合 Schema。既有默认 SQL 仍输出
  22 列 `npm_basic_result`，完整 `test_scheduler_e2e` 通过。
- T3.2：同机同进程使用相同的 512 行 TCP RST batch、100 次计时迭代、独立 task/预热与
  `observing=basic`，basic-only 处理 51,200 包耗时 871.266 ms、58,765.084 packets/s；basic+session 处理
  51,200 包耗时 1,179.896 ms、43,393.650 packets/s。两种模式均输出 51,200 行固定 22 列 Basic Schema，逐包
  revision 1、closed 终态通过校验；该结果仅为 2026-09-17 本机单次观测，不是跨机器阈值或 SLA。
- 确定性 Session tracker profile 使用 128 个 TCP 会话、每会话 4 个离散 payload range，观测峰值为 128 个
  session、512 个 outstanding range、105,472 bytes `kModuleState`；预算账本始终等于 tracker tracked bytes，
  `Clear()` 后 session、range、tracker bytes 与 module budget 全部归零。该值不代表完整 task 或 RSS 峰值。
- `cmake -B build src`、benchmark/定向测试 target、`cmake --build build -j$(nproc)` 均通过；定向
  `test_npm_basic`、`test_scheduler_e2e` 2/2 通过，完整 CTest 13/13 通过。
- `git diff --check` 与 benchmark 120 列检查通过；环境未提供 `clang-format` 可执行文件。
