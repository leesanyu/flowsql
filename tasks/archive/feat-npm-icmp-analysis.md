# Feature: NPM ICMP 控制消息分析

状态：[x] T0～T4 已完成
优先级：P1
前置：[NPM 协议模块运行时](../archive/feat-npm-protocol-analysis.md)。
结果存储复用 [NPM 多实体结果存储与查询](../archive/feat-npm-result-query.md)；不依赖共享 TCP 字节流或 Flow Labeling。

## Non-Goals

- 不做 IP 分片重组、ICMP 校验和验证、主动探测或端到端送达证明；捕获点之外的请求、应答和网络故障不可推断。
- 不分析 ICMP 信息类消息、IPv4 redirect/source quench、邻居发现、路由器通告、组播管理、隧道内层或引用报文的应用载荷。
- 不创建 ICMP 端口会话，不重新解码基础包、执行 NPI/标签 matcher、订阅 TCP 字节流或改变 Basic/Session/DNS/HTTP/1/TLS Schema。
- 不建立引用报文与历史 TCP/UDP 会话的因果关系；活动会话 ID 只是相同观测域与五元组的当前候选，不代替错误引用中的原始包身份。
- 不新增结果数据库、实时采集接线或公共协议 V1 虚表。

## 业务意图

排查网络连通性异常的离线 NPM 用户，需要把可见的 ICMP/ICMPv6 回显请求与应答关联，
查询观测时延和未见应答的原因，并独立查看差错 type/code、被引用的网络五元组及可能对应的活动 TCP/UDP 会话。
显式启用 `icmp` 后，模块只消费现有独立 control packet 输入，输出 `icmp_event` 类型化事件；
`observing` 决定普通 DataFrame，托管消费者接收全部已启用实体。

## 核心契约

### 配置、内部接口与数据结构

- 模块 ID 为 `icmp`，实体 ID 为 `icmp_event`；`features='icmp'` 显式启用，
  `observing='icmp_event'` 仅在启用后合法。沿用顶层 `parameters.schema_version=1`；
  `parameters.icmp` 可省略，存在时必须为 object，只接受以下两个字段。未启用模块时不解析其节点。
- `echo_timeout_ns` 默认 5,000,000,000，范围 1,000,000～300,000,000,000；
  `max_pending_echo` 默认 4096，范围 1～4096，限制整个任务的未决回显键数。
  重复/未知字段、null、错误类型和越界值在 Open 以 `/icmp/...` 路径拒绝，失败不发布 runtime。
  配置示例：`{"schema_version":1,"icmp":{"echo_timeout_ns":5000000000,"max_pending_echo":4096}}`。
- `NpmModulePlanV1` 只设置 `kControlPacket` 输入位，`requires_labeling=false`、
  `requires_tcp_stream=false`、`primary_label_ids=null`。模块实现既有
  `INpmProtocolModuleV1`；`OnInput` 的 `transport/session` 必为空，
  `OnSessionSnapshot/OnSessionEnd` 不参与 ICMP 状态。无需新公共 ABI。
- T0 先冻结以下任务内结构与头文件，再实现解析；借用的 `NpmInputEventV1`、body、layer
  和引用会话视图只在同步回调内有效。

| 内部结构/接口 | 拥有的事实与边界 |
| --- | --- |
| `NpmIcmpConfigV1` | 回显期限、任务未决上限；Open 后不可变。 |
| `NpmIcmpEchoKeyV1` | 观测域、IP 家族、请求源/目的 IP、16 位 identifier/sequence；不含端口或标签。 |
| `NpmIcmpPendingEchoV1` | 任务内非零事件 ID、首次完整请求捕获时间、重复请求次数和 deadline；不保留 payload/span。 |
| `NpmIcmpQuotedFlowV1` | 外层控制包的观测域，以及可安全解析的引用 IP 家族、协议、源/目的 IP 与 TCP/UDP 端口。 |
| `INpmIcmpActiveSessionLookupV1` | 任务私有只读接口：`FindActive(const NpmIcmpQuotedFlowV1&) -> optional<uint64_t>`；实现绑定当前 input namespace，按传入观测域调用现有 `NpmSessionTable::Find`，不创建或延长会话。 |

### 输入、回显关联与差错可见性

- control 输入沿用 `BuildNpmControlInput` 的网络边界、观测域、`body_complete` 和捕获时间。
  IPv4 protocol 1、IPv6 next-header 58 只按各自家族解释；只读取已捕获的 ICMP body，
  不保存输入 owner。IPv4 外层 MF/非首片、IPv6 外层 fragment 的 M/非零 offset 不建立事件或关联；
  非首片及非法网络范围沿用现有 binding 错误。V1 不从分片猜测完整消息。
- 回显仅接受 IPv4 request/reply type 8/0、IPv6 type 128/129，code 必须为 0，
  body 至少 8 字节。完整捕获的请求以回显键建立 pending；同键未决的再次请求记为 retry，
  保留首次时间和同一事件 ID。完整捕获的反向同键应答结束一个 pending；
  无 pending 或迟到的应答独立成行。键关联不比较 payload，也不宣称能区分同键重试与新请求。
- `body_complete=false` 且 8 字节回显头已安全可读时，单独输出
  `echo_request_only` 或 `echo_reply_only`，原因 `capture_truncation`，
  不建立/结束 pending；连头都不完整则不生成行。畸形 code 或未支持的 ICMP type
  不建立 pending、不伪造回显结果。
- 差错仅接受 IPv4 destination unreachable/type 3、time exceeded/type 11、
  parameter problem/type 12，以及 IPv6 destination unreachable/type 1、
  packet too big/type 2、time exceeded/type 3、parameter problem/type 4。
  可读的 8 字节外层 ICMP 头即可独立输出 `icmp_error`；type/code 保留原始值。
  IPv4 type 3/code 4 与 IPv6 type 2 提取可见的 next-hop MTU；其他情况为 null。
- 差错引用从 ICMP 头之后开始有界读取，按引用自身的 IPv4/IPv6 头长、声明长度和分片偏移校验；
  引用通常只含原包前缀，不要求捕获完整的引用 IP 包；
  可安全读取 IP 头但缺端口时只记录 IP/协议，不能按半个端口查会话。
  仅 TCP/UDP 且完整可读前 4 字节端口时形成引用五元组；IPv6 引用只支持基础头后直接
  TCP/UDP，不跨扩展头猜测端口。引用缺失、截断、畸形或非目标协议仍保留外层差错事件，
  分别用 `quote_status` 表明可见范围，不使任务失败。
- 对完整引用五元组可在差错回调时只读查询同任务的当前活动会话；`active_quoted_session_id`
  仅是该时刻的同键候选，未找到、会话已结束或引用不足时为空。回显 pending 不因差错自动结束；
  差错行与回显行各有独立身份。

### 时间、结果与资源

- 完整请求的 deadline 为首次请求捕获时间加期限，溢出时饱和到 `INT64_MAX`；
  retry 不延长期限。只有既有合法前进的 capture watermark 到达期限时，才以
  `echo_request_only/response_not_observed_by_deadline` 终结；不变/回退水位和墙钟不触发。
  正常 EOF 以 `echo_request_only/task_eof` 收口剩余 pending；
  错误/取消只 Abort、不发正常终态。应答时间早于请求时间时仍可键关联，但时延为 null。
- `icmp_event` 使用 Arrow Schema v1 与 event revision 语义；metadata 固定为
  `flowsql.entity=icmp_event`、`flowsql.schema_version=1`、
  `flowsql.timestamp_unit=ns`、`flowsql.revision_semantics=event`、
  `flowsql.measurement_scope=single_capture_observed_packets`。
  每个逻辑回显或差错一行，`entity_instance_id` 非零且任务内单调不复用，
  `revision=1`、`is_final=true`，通过 `Emit("icmp_event", rows)` 路由。

| 列（Arrow 类型） | 约束 |
| --- | --- |
| `entity_instance_id:uint64`、`revision:uint64`、`observed_at:int64`、`is_final:bool` | 必填；统一事件身份、版本和终态。 |
| `observation_domain_id:uint64`、`ip_family:uint8`、`src_ip/dst_ip:utf8` | 必填；回显取请求（孤儿应答取应答），差错取外层包；IP 家族为 4 或 6。 |
| `outcome:utf8`、`icmp_type:uint8`、`icmp_code:uint8`、`outer_truncated:bool` | 必填；type/code 取该行起始包，截断指整个外层 ICMP body 的捕获事实。 |
| `incomplete_reason:utf8`、`echo_id/echo_sequence:uint16`、`echo_retries:uint32` | 原因与 echo 字段可空；retries 仅有完整请求时有值。 |
| `request_at_ns/reply_at_ns/latency_ns:int64` | 可空；时延仅在两端时间存在且非负时填写，不能用通知时间冒充包时间。 |
| `quote_status:utf8`、`quoted_ip_family:uint8`、`quoted_protocol:uint8` | 状态仅差错行必填；引用 IP 家族/协议仅在安全读出时填写。 |
| `quoted_src_ip/quoted_dst_ip:utf8`、`quoted_src_port/quoted_dst_port:uint16` | 可空；IP 与端口成对填写，端口只限完整 TCP/UDP 引用。 |
| `active_quoted_session_id:uint64`、`next_hop_mtu:uint32` | 可空；前者只表当前活动五元组候选，后者仅表明确的 MTU 字段。 |

`outcome` 只取 `echo_matched`、`echo_request_only`、`echo_reply_only`、`icmp_error`。
`incomplete_reason` 对 matched/error 为 null；request_only 只取
`response_not_observed_by_deadline`、`task_eof`、`capture_truncation`，
reply_only 只取 `no_pending_request`、`capture_truncation`。
`quote_status` 对非差错为 null；差错只取 `unavailable`（引用 IP 头不足）、
`invalid`（引用 IP 头畸形）、`ip_only`（安全 IP 事实但无完整 TCP/UDP 端口）、
`tuple`（完整五元组）。`observed_at` 取结果触发包的捕获时间、到期水位或 EOF 通知时间。

- pending、期限索引、引用解析副本与结果编码临时内存在分配前计入任务 `kModuleState`；
  输入 body 不跨回调保留。达到 `max_pending_echo`、预算不足、分配/编码/emitter/消费者失败使
  任务明确非零失败，不静默逐出或伪造协议结果；已消费前缀沿用统一结果路由语义。
  matched、期限、EOF、失败和取消均释放模块计费。结果保留缓冲继续由既有 `kPendingOutput` 负责。

## 主链路

1. **打开与报文**：冻结 `icmp` 配置/Schema 和任务私有活动会话只读查询 → 现有 packet 解码与独立 control 分发 → 回显键关联或差错引用解析 → 类型化事件经结果路由送全部消费者，普通 DataFrame 只显示 `observing` 实体。
2. **时间与退出**：合法 capture watermark 终结到期回显 → 正常 EOF 终结剩余回显并 Finish → 结果消费者 Finish；处理错误/取消只 Abort/Cancel 并归还预算。

## Feature Tasks 与测试锚点

| 任务 | 交付结果与目标价值/可观察保证 | 验收锚点 |
| --- | --- | --- |
| [x] T0 | 交付冻结的 ICMP 配置、拥有型结构、只读活动会话查询与 Schema 契约，使非法配置在 Open 原子失败且公共 V1 ABI 不变。 | A1、A2 |
| [x] T1 | 交付有界 ICMP/ICMPv6 回显与差错解析，使 type/code、引用五元组和截断范围来自安全可见字节。 | A3、A4 |
| [x] T2 | 交付有界回显关联及水位/EOF 收口，使时延与未见应答原因可复核且同键结果不重复。 | A5、A6 |
| [x] T3 | 交付只读活动会话候选、预算计费和统一结果路由，使差错能按可见五元组辅助定位且任务隔离。 | A7、A8 |
| [x] T4 | 交付生产目录、离线 SQL/托管查询回归与使用说明，使用户可显式启用 ICMP 且旧结果保持稳定。 | A9、A10；全部前序锚点 |

| 锚点 | 必须落成的自动断言 |
| --- | --- |
| A1 | 缺省/合法配置、未知/重复/null/错误类型/越界参数、未启用 icmp 的节点、observing 门禁和 Open 原子性；生产目录到 T4 前仍拒绝 icmp，旧 V1 模块可编译运行。 |
| A2 | 固定 Schema/metadata、event 身份/nullable 列、control-only 计划和任务私有查询接口；无 labeling/TCP stream 也能 Open，control 不产生端口会话/NPI/标签调用。 |
| A3 | IPv4/IPv6 回显、所列差错 type/code 与 MTU 正确；混合 TCP/UDP/control 批次顺序和跨 RecordBatch 输入得到相同结果；输入 owner 可释放。 |
| A4 | 8 字节不足、截断、MF/fragment、引用 IPv4/IPv6 长度/偏移、IPv6 扩展头、非 TCP/UDP 引用均不越界，不产生错误匹配；差错保留可见 type/code 与准确 quote_status。 |
| A5 | 同域正反向回显匹配、同键 retry、不同域/家族/端点/ID/sequence 隔离、孤儿/迟到应答和截断回显行的身份、原因、时延正确。 |
| A6 | deadline 前/恰到/不动水位、饱和加法、时间倒序、正常 EOF 与 Abort/Cancel 各只产生契约允许的终态；无负时延、无重复行。 |
| A7 | 完整引用仅查询同域/namespace 的活动 TCP/UDP 五元组，反向规范化一致；无引用、会话结束、无匹配时 ID 为空，查询不建立或延长会话，字段明确只是候选。 |
| A8 | pending 上限、任务预算和分配/编码/emitter/consumer 失败明确终止；长回显 payload/引用不使私有状态随字节增长，Finish/Abort 后预算回基线，前台/托管消费一致。 |
| A9 | 真实离线 SQL/Scheduler 显式启用 icmp，前台 `icmp_event` 和托管 history/latest/final 的 Schema v1、run_id 隔离可查；未启用 icmp 的旧 SQL/实体不变。 |
| A10 | 标准 CMake 全量构建、完整 CTest、解析/关联/预算路径 ASan/UBSan、clang-format 和 diff 检查通过，记录实际运行结果。 |

实施时每个切片先在 `active_task.md` 冻结允许文件、CMake target、测试、时间盒和停止条件；
单个锚点通过不等于 Feature Task 完成。

## 完成证据

- 2026-10-02：规格 S0 完成，T0～T4 尚未实施，表中自动断言是后续验收目标。
  唯一一轮 P0/P1 契约审查确认：control 输入不携带 session，引用会话只读查询按任务 namespace
  与报文观测域定位；差错引用允许只有原包前缀，不要求完整原包；分片不伪装为完整回显，
  新接口仅限算子内部，不改变公共 V1 ABI。未发现已知 P0/P1 阻塞项。
- 2026-10-02：T0 完成。内部头文件冻结配置、回显键/pending、引用五元组、只读活动会话接口与
  28 列 `icmp_event` Schema；严格配置解析拒绝未知/重复/null/越界字段，测试目录验证
  production 尚拒绝 icmp、非法配置 Open 原子失败、合法无标签/流能力 Open 与 observing Schema。
  标准 CMake 配置、`test_npm_icmp_contract` 1/1、`test_npm_basic` 1/1、clang-format 与 diff 检查通过。
- 2026-10-02：T1 完成。纯解析器从独立 control 输入读取 IPv4/IPv6 回显、所列差错、MTU 与
  引用 IPv4/IPv6 TCP/UDP 五元组；不保留包 owner。定向测试覆盖首片带 MF/M、短头、
  引用 IP 头不足/畸形、无端口及 IPv6 扩展头不猜测，`test_npm_icmp_parser` 1/1、
  clang-format 与 diff 检查通过。跨批次运行时组合回归留在 T4。
- 2026-10-02：T2 完成。回显 tracker 用任务内事件 ID 和有序 deadline 索引处理同键 retry、
  反向应答、孤儿/截断、合法水位和 EOF；未决键按任务预算预留/释放。定向测试覆盖域隔离、
  上限、到期边界、饱和期限、时间倒序、失败与 Abort，`test_npm_icmp_echo` 1/1 通过。
- 2026-10-02：T3 完成。模块以固定 28 列事件 Schema 经现有 emitter/router 输出；只读
  会话查询在 runtime 创建模块后、发布前绑定当前会话表与 namespace，测试验证引用双向规范化、
  观测域隔离、引用缺失、预算释放和模块事件编码。首次回归发现新模块符号未编入生产共享库，
  `ldd -r` 定位后已将实现源文件纳入共享库（catalog 仍未注册 icmp）。`test_npm_basic`
  1/1、格式、`git diff --check` 与共享库链接检查通过。
- 2026-10-02：T4 完成。生产目录显式注册 `icmp/icmp_event`；真实离线 Scheduler SQL 从
  ICMP PCAP 得到前台一行 `echo_matched`、观测域 77、时延 100000 ns 和固定 28 列
  Schema v1。同一 SQLite 目标两次运行的 `history/latest/final` 均可按各自 `run_id`
  查询，旧 SQL 回归保持通过。README 给出显式启用、单数据库目标及捕获可见性说明。
  混合 TCP/UDP/control 同批与跨 RecordBatch 输出完全相同；差错类型和引用边界、
  回显键隔离、活动 TCP/UDP 会话候选/失效、namespace 隔离及失败释放有定向断言。
  最终 `cmake -B build src`、全量 `cmake --build build -j$(nproc)` 与完整 CTest 40/40
  通过；ICMP 解析、回显与含预算运行时以 ASan/UBSan 构建，定向 CTest 3/3 通过。
  Sanitizer 运行设置 `ASAN_OPTIONS=detect_leaks=0`，不声称泄漏检测通过。
  改动 C++ 的 clang-format-18 与 `git diff --check` 通过；未提交或推送。
