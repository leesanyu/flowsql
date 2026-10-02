# Feature: NPM TLS 握手分析

状态：[x] T0～T4 已完成并验收（2026-10-01）
优先级：P1
前置：[协议模块运行时](../archive/feat-npm-protocol-analysis.md)、[共享有界 TCP 字节流](../archive/feat-npm-shared-tcp-stream.md)、[流量标签](../archive/feat-flow-labeling.md)。
结果存储复用 [NPM 多实体结果存储与查询](../archive/feat-npm-result-query.md)；本 Feature 不另建数据库链路。

## Non-Goals

- 不解密 TLS 握手或应用数据，不声称验证 Finished、证书、身份或连接建立成功；不分析 QUIC/DTLS、SSLv2/v3、TLS 1.0/1.1、TLS 内部应用协议或 TLS 会话恢复语义。
- 不扫描 TCP 中途字节寻找 TLS，不分析 STARTTLS/代理隧道内 TLS、重协商或同一 TCP 连接的后续握手；不将 SNI、ALPN、证书、NPI 或解析结果用于共享流准入。
- 不新增 TCP 重组、会话、方向、标签 matcher、结果数据库、实时采集入口或公共 V1 虚表；不改变 Basic/Session/DNS/HTTP/1 的输入和 Schema。
- 不将缺少 ServerHello、FIN/RST、捕获缺口、期限到达或未见后续密文解释为服务端故障；不保证被动捕获点字节等于端点最终接收内容。

## 业务意图

排查 TCP 上 TLS 初始连接建立异常的离线 NPM 用户，需要看见 ClientHello、可见的 ServerHello、
明文 fatal alert、版本协商与可观察时延，并知道哪些阶段因加密、捕获缺口或会话终结无法判断。
显式启用 `tls` 后，模块仅消费命中冻结主标签的共享 TCP 字节流，每个会话至多输出一个
`tls_handshake` 事件实体。普通 DataFrame 由 `observing` 选择；托管消费者收到全部已启用实体。

## 核心契约

### 打开、配置与内部接口

- 模块 ID 为 `tls`，实体 ID 为 `tls_handshake`；`features='tls,labeling'` 显式启用，
  `observing='tls_handshake'` 仅在启用后合法。`parameters.tls` 是启用时必填的拥有型 object，
  沿用顶层 `parameters.schema_version=1`；未启用 TLS 时不创建模块。
- `primary_label_ids` 为 1～256 个互异、非零且存在于冻结标签目录的 uint32；
  `handshake_timeout_ns` 默认 5,000,000,000，范围 1,000,000～300,000,000,000；
  `max_hello_bytes` 默认 65,536，范围 4,096～1,048,576。重复/未知字段、null、错误类型及越界值
  按 `/tls/...` 路径拒绝。缺 labeling/provider、无合法标签、缺共享流 consumer 或协议对象与
  consumer 不同实例时，Open 原子失败。
- 配置示例：`{"schema_version":1,"core":{"labeling":"config.tls-labels@7"},"tls":{"primary_label_ids":[1001],"handshake_timeout_ns":5000000000}}`；快照与标签须在运行环境中存在。
- `NpmModulePlanV1` 声明 TCP input mask、`requires_labeling=true`、`requires_tcp_stream=true` 与
  冻结标签集合。同一对象实现现有 `INpmProtocolModuleV1` 和 `INpmTcpStreamConsumerV1`，工厂返回
  非拥有型 stream 指针；`OnInput(kTcpPacket)` 不解析 payload，字节只在同步
  `OnTcpStreamReadable` 中读取。T0 先冻结以下内部拥有型结构与头文件，不修改 V1 ABI。

| 内部结构 | 拥有的事实 |
| --- | --- |
| `NpmTlsConfigV1` | 标签集合、握手期限、单个 Hello 上限。 |
| `NpmTlsHelloFactsV1` | 完整可见的 ClientHello/ServerHello 版本、SNI、ALPN、cipher suite、HelloRetryRequest 标志和头部完成捕获时间；无借用 span。 |
| 方向解析状态 | TLS record 头/剩余字节、握手消息头/受限 Hello 副本、起点、Gap、End 与明密文边界。 |
| `NpmTlsHandshakeV1` | 单会话候选身份、客户/服务方向、阶段、最多一次 HelloRetryRequest、期限和结果事实。 |

### 可见性、定界与阶段

- 只从 `origin=kSyn` 且无先前 Gap 的方向偏移 0 识别 TLS record；偏移 0 不是合法 TLS record
  起点的方向立即封闭，不向后扫描。首个 record 的首条握手消息必须从该方向偏移 0 的 record
  内容开始且类型为 `ClientHello`，才建立候选和客户方向；没有候选不输出 TLS 事件。
  midstream、非 TLS、未命中标签和 label 0 不建 TLS 私有解析状态。相反方向仅作为同一候选的服务方向，
  不靠端口或 NPI 猜测角色。
- TLS record 头为 5 字节，长度校验上限 18,432 字节；握手消息使用 1 字节类型和 3 字节长度，
  允许跨 record、Data 和 batch 分片，也允许同一 record 多条消息。ClientHello、ServerHello 和
  重试后的 ClientHello 各自受 `max_hello_bytes` 限制；其他明文握手消息只按已验证长度跳过，
  不保存证书或正文。缺口、截断、非法长度、握手跨非握手 record、语法/向量长度错误均停止该会话的
  后续判定，不从剩余字节猜测同步点。输入所有权和 cursor/span 仅在回调内有效。
- 只解析 TLS 1.2/1.3 初始 Hello 的安全可见字段：ClientHello 的 `legacy_version`、
  `supported_versions`、单个合法 ASCII SNI host_name（最多 255 字节）及 ALPN 提议列表
  （可逆转义为 JSON 字符串数组后最多 512 字节）；ServerHello 的最终 `selected_version`、`cipher_suite`，
  以及 TLS 1.2 明文 ServerHello 中的 `selected_alpn`。字段超限不截断，按 `hello_limit_exceeded` 收口。
  扩展与向量必须完整、长度一致，重复的所需扩展或 SNI/ALPN 项按协议畸形收口；未知扩展按长度跳过。
  TLS 1.3 的 ALPN 选择位于加密 EncryptedExtensions，输出 null；不把 legacy record/Hello 版本
  0x0303 当作最终 TLS 版本。SNI 只表示可见的外层 ClientHello 值，不推断 ECH 内部名称；
  TLS 1.2 `selected_alpn` 也使用可逆转义表示原始协议 ID 字节。
- 固定随机值识别 TLS 1.3 HelloRetryRequest，最多允许一轮，并要求同方向第二个完整 ClientHello
  与反方向最终 ServerHello；重试消息不是最终 ServerHello，不产生第二事件。结果的 ClientHello 元数据
  与完成时间取第一次完整 ClientHello，最终 ServerHello 时延也从第一次计算；协商版本必须由两次
  ClientHello 都提供。仅以最终 ServerHello 的 `supported_versions=0x0304` 判为 TLS 1.3；
  没有此选择且 `legacy_version=0x0303` 为 TLS 1.2。选择版本未提供、无效 cipher suite、
  重试顺序错误或不支持的版本均按明确的不完整原因封闭。
- TLS 1.3 最终 ServerHello 是本 V1 最后的可见握手语义边界：随后 `application_data` record
  可能承载加密握手、alert 或应用数据，均不解析；不以其出现宣称成功。TLS 1.2 在最终 ServerHello
  后只继续校验 record 定界与明文 fatal alert，首个 ChangeCipherSpec（CCS）即封闭可见窗口；
  不解析后续加密 Finished。TLS 1.3 的兼容 CCS（单字节 `0x01`）与 ClientHello 后的 0-RTT
  `application_data` 只按 record 边界跳过，不当作失败或握手完成。仅在尚未越过加密边界、
  长度恰为 2 字节的明文 `alert` 中，level=2 是 `fatal_alert_observed`；warning 和无法解密的
  alert 不作为失败事实。
- 候选首次 ClientHello 字节的捕获时间加期限，构成饱和到 `INT64_MAX` 的 deadline；
  仅合法推进的 capture watermark 可使其到期。无该时间时期限不可算，只由 Gap/End/会话终结收口。
  ClientHello/最终 ServerHello 完成时间为组成消息字节的最大捕获时间；两者存在且服务时间不早于
  客户时间才有非负 `server_hello_latency_ns`。通知时间不冒充字节捕获时间。
- 每个候选至多一条最终结果。`fatal_alert_observed` 仅在明确看到明文 fatal alert 时输出；
  `server_hello_observed` 只证明最终 ServerHello 可见，在 TLS 1.3 ServerHello、TLS 1.2 首个 CCS、
  期限或会话结束时收口，均不表示握手成功；其 `incomplete_reason` 记录可见窗口结束原因。
  其余已建立候选为 `incomplete`。任一方向 Gap 后立即封闭，捕获截断优先标 `capture_truncation`；
  迟到字节和重传不得再改结果。客户端 FIN 不阻止处理反向已缓冲数据；RST、idle、tuple reuse、
  EOF 遵循共享流 final drain 先于模块 OnSessionEnd。失败/取消只 Abort，不产生正常终态。

### 输出与资源保证

`tls_handshake` 固定 25 列 Arrow Schema v1，metadata 为 `flowsql.entity=tls_handshake`、
`flowsql.schema_version=1`、`flowsql.timestamp_unit=ns`、`flowsql.revision_semantics=event`、
`flowsql.measurement_scope=single_capture_observed_packets`。每个候选至多一行，
`entity_instance_id` 非零、`revision=1`、`is_final=true`，经 `Emit("tls_handshake", rows)` 路由。

| 列（Arrow 类型） | 约束 |
| --- | --- |
| `entity_instance_id:uint64`、`revision:uint64`、`observed_at:int64`、`is_final:bool` | 必填；事件身份、版本、终态和结果通知时间。 |
| `session_id:uint64`、`observation_domain_id:uint64`、`a_ip/b_ip:utf8`、`a_port/b_port:uint16` | 必填；沿用规范化会话端点。 |
| `outcome:utf8`、`hello_retry_count:uint8` | 必填；结果只取 `server_hello_observed`、`fatal_alert_observed`、`incomplete`；重试计数为 0 或 1。 |
| `incomplete_reason:utf8`、`client_direction:uint8` | 原因仅对非 fatal 行必填；方向在候选建立后必填。 |
| `client_hello_at_ns/server_hello_at_ns/server_hello_latency_ns:int64` | 可空；只存完整消息的字节捕获时间与可证实时延。 |
| `client_sni:utf8`、`client_alpn_protocols:utf8`、`client_offered_tls13:bool` | 可空；仅完整 ClientHello 填写；ALPN 是受限 JSON 字符串数组，不表示协商值。 |
| `selected_version:uint16`、`cipher_suite:uint16`、`selected_alpn:utf8` | 可空；仅最终 ServerHello 填写；TLS 1.3 的 selected_alpn 必为空。 |
| `alert_level:uint8`、`alert_description:uint8` | 可空；只在明文 fatal alert 行填写。 |

`incomplete_reason` 固定为以下值：

- `server_hello_observed`：`encrypted_after_server_hello`、`encrypted_after_change_cipher_spec`、
  `deadline_after_server_hello`、`session_end_after_server_hello`。
- `incomplete`：`client_hello_incomplete`、`server_hello_not_observed_by_deadline`、
  `hello_retry_request_unfinished`、`client_path_gap`、`server_path_gap`、`capture_truncation`、
  `malformed_record`、`malformed_hello`、`hello_limit_exceeded`、`unsupported_version`、
  `unsupported_framing`、`server_stream_end`、`session_end`。

`observed_at` 为触发最终结果的捕获水位、Gap/End、fatal alert 或会话终结通知时间；
不冒充 Hello 时间。无候选的会话无行。

- 每个候选、两个方向的定界标量、Hello 副本、SNI/ALPN 字符串、期限索引和结果编码状态
  在分配前计入任务 `kModuleState`；共享流字节由共享流计一次。记录体只流式跳过，私有状态不随证书
  或密文总量增长。Hello 上限是协议不完整原因；任务预算不足、分配/编码/emitter/消费者失败则任务
  非零失败，不伪装成协议异常。正常终结、Abort 与 Cancel 均释放计费；托管结果保留已消费前缀并沿
  既有 FailRun 语义收口。

## 主链路

1. **打开与输入**：解析 `features/parameters.tls` → 冻结标签/Schema 和同实例 stream consumer → 唯一 Basic 会话链按标签准入共享流 → record/Hello 定界与版本可见性判断 → 输出单条类型化事件 → 结果路由送全部消费者，前台只见 `observing` 实体。
2. **时间与收尾**：capture watermark 推进候选期限 → 明文 fatal alert、版本可见边界、Gap/End 或会话终结收口 → 模块 Finish → 结果消费者 Finish；失败/取消仅 Abort/Cancel。

## Feature Tasks 与测试锚点

| 任务 | 交付结果与目标价值/可观察保证 | 验收锚点 |
| --- | --- | --- |
| [x] T0 | 交付冻结的 TLS 配置、候选状态和 Schema 契约，使无效能力组合在 Open 前失败且现有 V1 ABI 不变。 | A1、A2 |
| [x] T1 | 交付有界 record 与 Hello 流式定界，使跨包/record/batch 的明文元数据准确且畸形输入不越界。 | A3～A5 |
| [x] T2 | 交付版本感知的握手阶段与期限/终结收口，使仅有明确可见证据的 fatal alert 被报告为失败。 | A6、A7 的阶段与终态；预算释放纳入 A8 |
| [x] T3 | 交付预算计费和统一结果路由，使长流状态有界、单候选单行且前台/托管消费一致。 | A8、A9 |
| [x] T4 | 交付生产目录、离线 SQL/托管查询回归与使用说明，使用户可显式启用 TLS 且旧结果保持稳定。 | A10、A11；全部前序锚点 |

| 锚点 | 必须落成的自动断言 |
| --- | --- |
| A1 | 缺 labeling/provider、空/0/重复/未知标签、缺/错 stream consumer、缺/未知/重复/null/越界参数均 Open 原子失败；未启用 tls 时不启动模块，旧 V1 模块可编译运行。 |
| A2 | 固定 Schema/metadata、事件身份、nullable 列与 observing 门禁；T0～T3 测试目录可用，生产 catalog 到 T4 才注册 tls。 |
| A3 | TLS 1.2/1.3 ClientHello/ServerHello 在 record、Data、RecordBatch 各种切分下得到同一版本/SNI/ALPN/cipher/timestamp；输入 owner 可释放。 |
| A4 | 首字节非 TLS、midstream、Gap 前无候选、未知扩展、重复或坏长度扩展、越界 record/Hello、截断和零长度异常在定界器中明确为无候选或候选封闭错误；不扫描重同步、不越界。最终行数由 A7 验证。 |
| A5 | 同一 record 多条握手消息、跨 record Hello、TLS 1.3 兼容 CCS 与 0-RTT、TLS 1.2 非目标握手消息均保持正确边界；证书/密文长度增长不增加持久私有正文。 |
| A6 | TLS 1.2 最终 ServerHello、TLS 1.3 最终 ServerHello、HelloRetryRequest+第二 ClientHello+最终 ServerHello 的 selected_version 与时延正确；TLS 1.3 selected_alpn 为 null，均不产出 success。 |
| A7 | 明文 fatal alert 与 warning、加密 alert、deadline 前/恰到/不动水位、乱序时间、Gap/截断、半关闭、RST、idle、tuple reuse、EOF、Abort：仅前者为 fatal；无候选无行，已建候选异常后最多一条终态事实且原因正确。任务预算释放由 A8 验证。 |
| A8 | Hello 上限、任务预算、分配/编码/emitter/消费者失败可区分；长证书/密文流不使私有状态无界，失败非零且无伪终态，已消费前缀保留。 |
| A9 | features 换序、不同观测域、未启用 Session、observing 在 basic/session/dns/http1/tls 间切换不改变 TLS 事件；全部启用实体送托管消费者。 |
| A10 | 真实离线 SQL/Scheduler 显式启用 tls，DataFrame 与托管 `tls_handshake` 的 history/latest/final、Schema v1、run_id 隔离可查；未启用 tls 的旧 SQL/指标不变。 |
| A11 | 标准 CMake 全量构建、完整 CTest、record/阶段/预算/终结路径 ASan/UBSan、格式与 diff 检查通过，并记录实际运行结果与依赖。 |

实施时每个切片先在 `active_task.md` 冻结允许文件、CMake target、测试、时间盒和停止条件；一个锚点通过不等于 Feature Task 完成。

## 完成证据

- 2026-10-01：完成唯一一轮 P0/P1 设计审查。候选只在 SYN 对齐的方向首条握手消息建档，避免流内重同步导致严重误判；HelloRetryRequest 的第一次 ClientHello 事实与时延基准已固定。沿用现有协议与共享流 V1 虚表、同实例 consumer、同步借用视图及任务预算，避免 ABI、所有权和无界缓存风险；TLS 1.3 ServerHello 后不推断加密握手结果。未发现已知 P0/P1 阻塞项。
- 2026-10-01：规格设计时仅冻结规格、更新 Backlog/工作台；当时 T0～T4 均未实施。验收以表中自动断言为准。
- 2026-10-01：T0 完成。测试专用 catalog 验证缺 matcher、未启用 labeling、未知标签、非法参数、缺失或错配 stream consumer 均在 Open 原子失败；合法配置返回固定 `tls_handshake` Schema，未启用 TLS 的旧路径和 observing 门禁保持有效。独立契约测试覆盖拥有型状态、配置边界、能力计划、25 列 Schema/metadata 与事件行；生产 catalog 仍未注册 TLS，公共 V1 ABI 未修改。标准全量构建和完整 CTest 32/32 通过；定向 CTest 2/2、clang-format 与 `git diff --check` 通过。未提交或推送。
- 2026-10-01：T1 完成。完整 Hello 解析器验证 TLS 1.2/1.3 可见字段、HRR、拥有型输入、扩展/向量错误和 SNI/ALPN 上限；双方向定界器验证跨 Data/record 的 ClientHello 与 ServerHello、同 record 多消息、零长度及畸形封闭、Gap/End、CCS/0-RTT 与长正文跳过。测试专用 catalog 经真实共享流回调比较单批与跨 RecordBatch 的 Hello 字段和捕获时间，并验证输入 owner 释放。标准全量构建、完整 CTest 34/34、定向 CTest 3/3、改动代码 clang-format 与 `git diff --check` 通过；生产目录仍未注册 TLS，T2～T4 未实施。未提交或推送。
- 2026-10-01：T2 完成。单会话 tracker 将 TLS 1.2/1.3、一次 HRR 与最终 ServerHello 转为拥有型终态事实；第一次 ClientHello 固定时延基准，TLS 1.3 ServerHello 后不报告成功，TLS 1.2 可见窗口在明文 fatal、CCS、期限或会话结束收口。测试覆盖 warning、加密不可见 alert、严格推进的捕获水位、饱和期限、Gap/截断、FIN/RST、idle/tuple reuse/EOF、Abort、无候选无结果及单候选单结果。真实共享流测试证实 final drain 先于 OnSessionEnd。修复 T1 定界器 alert 两字节正文只保存第一字节的缺陷。标准全量构建、完整 CTest 35/35、定向 CTest 3/3 通过；生产目录仍未注册 TLS。预算计费及 Arrow 结果输出留给 T3。未提交或推送。
- 2026-10-01：T3 完成。单行编码器输出固定 25 列 `tls_handshake`，验证 Schema、nullable、ALPN JSON、事件身份、预算不足、Arrow 分配失败和 emitter 错误；双接口任务模块经共享流游标将候选状态与编码临时状态计入 `kModuleState`，Abort/SessionEnd 释放计费，双方向无候选拒绝后提前释放解析预算。长密文和非 TLS 正文测试验证私有预算不随流量增长；真实 Basic 运行时测试覆盖不同观测域、features 换序、basic/session/dns/http1/tls 前台 observing、全部已启用实体送托管消费者，以及消费者失败时保留已消费前缀并 Cancel。生产目录未注册 TLS，SQL/托管持久化留给 T4。标准全量构建、完整 CTest 37/37、定向回归、clang-format 与 `git diff --check` 通过；未提交或推送。
- 2026-10-01：T4 完成。生产 catalog 注册 `tls/tls_handshake`，六个 TLS 实现文件纳入 `libflowsql_npm_basic.so`；真实离线 Scheduler SQL 使用 SYN 对齐的 TLS 1.3 PCAP 与任务内标签快照，前台得到一行固定 25 列 Schema v1、`server_hello_observed`、版本 `0x0304`、观测时延 100000 ns 和观测域 77。同一 SQLite 目标两次运行的 `history/latest/final` 均可按各自 `run_id` 查询，旧 SQL 回归继续通过。README 补充显式启用、单个托管数据库目标与捕获可见性说明。最终审查补齐 TLS 1.2 `selected_alpn` 原始协议 ID 的可逆 JSON 字符串转义，并以 NUL、非 ASCII、引号字节验证。标准 `cmake -B build src`、全量构建和完整 CTest 37/37 通过；TLS 定界、阶段、编码、模块四目标 ASan/UBSan 4/4 通过（`ASAN_OPTIONS=detect_leaks=0`）；`clang-format-18 --dry-run --Werror` 与 `git diff --check` 通过。构建使用当前工作区缓存的第三方依赖和系统 DPDK；未提交或推送。
