# Feature: NPM DNS 事务分析

状态：[x] T0～T4 已完成（2026-10-01）
优先级：P1
前置：[协议模块运行时](../archive/feat-npm-protocol-analysis.md)、[共享有界 TCP 字节流](../archive/feat-npm-shared-tcp-stream.md)、[流量标签](../archive/feat-flow-labeling.md)。
结果存储复用 [NPM 多实体结果存储与查询](../archive/feat-npm-result-query.md)；本 Feature 不另建存储链。

## Non-Goals

- 不实现 DNS 递归解析、缓存、权威答案校验、RDATA 语义解析、DNSSEC 验证或按答案生成资产事实。
- 不分析 DoT、DoH、DoQ、mDNS、LLMNR、AXFR/IXFR 或跨 UDP/TCP 的 TC 回退关联；不解密、不做 IP 分片重组。
- 不以 NPI/DNS qname/响应内容决定 TCP 流准入；不新增会话、方向、NPI、标签 matcher、结果数据库或实时采集入口。
- 不把未观察到响应解释为网络超时或服务故障；不保证被动捕获点完整、报文与端点真实接收内容相同。
- 不改变 Basic/Session 指标和 Schema，不扩展 `INpmProtocolModuleV1`、`INpmTcpStreamConsumerV1` 或框架插件虚表。

## 业务意图

排查可见 DNS 响应异常的离线 NPM 用户，需要将 UDP datagram 与 TCP 长连接上的请求、响应关联为可查询的
独立事务，看到查询对象、返回码、截断标志、可观察的响应时长，以及缺失事实的原因。
同一 `npm.basic` 任务显式启用 `dns` 后，仅对命中冻结主标签的会话建立 DNS 状态；Basic/Session 继续走
原有输入和结果链。输出 `dns_transaction` 类型化事件实体，前台由 `observing` 选择，托管结果消费者收到
所有已启用实体。

## 核心契约

### 打开、配置与准入

- T0 在测试目录验证模块 ID `dns` 与实体 ID `dns_transaction` 的契约；T4 才将其注册到生产目录。届时 `features='dns,labeling'` 明确启用，
  `observing='dns_transaction'` 才将 DNS 行送往普通 DataFrame。未启用 `dns` 时参数节点不启动模块；
  未启用 labeling、未提供有效标签集合或缺少 stream consumer 时 Open 原子失败。
- V1 同时订阅 UDP datagram 和 TCP stream，共用冻结的 `primary_label_ids`；当前 `NpmModulePlanV1` 对两种传输使用同一会话级标签过滤。0、未命中标签与无合法订阅不建立 DNS 事务或 TCP 私有解析状态。标签由已有 matcher 在会话首次分类时确定，不由 DNS 报文或端口反推。
- `parameters.dns` 是启用模块的拥有型配置节点；省略节点非法，因为 `primary_label_ids` 必填。
  沿用 `parameters` 的 `schema_version=1` 信封；`primary_label_ids` 为 1～256 个互异、非零且能在
  冻结标签目录中找到的 uint32。重复/未知字段、null、错误类型及越界值在 Open 拒绝，错误带
  `/dns/...` 路径；失败不发布 runtime。
- `response_timeout_ns` 默认 5000000000，范围 1000000～300000000000；
  `max_pending_per_session` 默认 256，范围 1～4096。前者仅由 capture watermark 驱动，是“截至
  观测水位未见响应”的判定期限；后者限制同一会话内未完成逻辑查询数。TCP DNS 帧长还受协议的
  65535 字节上限、共享流限额和任务预算约束。
- 配置示例：`{"schema_version":1,"dns":{"primary_label_ids":[1001],"response_timeout_ns":5000000000}}`；
  `1001` 必须存在于本任务冻结的标签目录，示例不创建该标签。
- 模块计划设置 UDP/TCP input mask、`requires_labeling=true`、`requires_tcp_stream=true` 和已冻结
  标签集合；同一 `protocol` 对象实现 `INpmTcpStreamConsumerV1` 并作为工厂的非拥有型指针返回。
  TCP `OnInput(kTcpPacket)` 仅接收 segment facts，不再次解析 payload；真正的 DNS/TCP 字节只来自共享流回调。

### 数据结构、接口与输出

现有纯虚接口保持不变；DNS 模块实现 `INpmProtocolModuleV1` 的输入、时间、会话终结和任务生命周期方法，
以及 `INpmTcpStreamConsumerV1::OnTcpStreamReadable`。通过
`INpmResultEmitterV1::Emit("dns_transaction", rows)` 发送固定 Schema。
T0 先冻结内部 `NpmDnsConfigV1`、`NpmDnsQuestionKeyV1`、`NpmDnsTransactionV1` 和 Arrow Schema，
再实现解析器；这些结构只在 NPM 算子内部使用，不形成跨插件 ABI。

| 数据 | 冻结语义 |
| --- | --- |
| `NpmDnsQuestionKeyV1` | 会话 ID、查询方向、DNS ID、opcode=QUERY、ASCII 不区分大小写的 wire QNAME、QTYPE、QCLASS；同会话不同方向或不同问题不互配。 |
| `NpmDnsTransactionV1` | 任务内唯一的 `entity_instance_id`、首个完整查询时间、查询方向、重试次数、可选响应及 deadline；同 key 未完成时相同查询合并为重试，不新建实体，也不重置首见时间。 |
| `NpmDnsConfigV1` | 拥有型主标签 ID 集合、响应期限与单会话 pending 上限；跨回调仅保存已计费的标量/复制数据。 |
| TCP 帧状态 | 每个已准入方向持有最多一个未完成的 2 字节长度前缀和消息副本；每个字节归任务 `kModuleState`，不保留借用 cursor/span。 |

`dns_transaction` Schema v1 使用 `NpmRevisionSemanticsV1::kEvent`，metadata 为
`flowsql.schema_version=1`。每个逻辑事务仅发一行；身份非零、revision=1、is_final=true。

| 列（Arrow 类型） | 约束 |
| --- | --- |
| `entity_instance_id:uint64`、`revision:uint64`、`observed_at:int64`、`is_final:bool` | 必填；符合现有类型化实体契约。 |
| `session_id:uint64`、`observation_domain_id:uint64`、`transport_protocol:uint8` | 必填；当前会话的稳定身份。 |
| `a_ip/b_ip:utf8`、`a_port/b_port:uint16` | 必填；沿用规范化 A/B 端点，不宣称客户端/服务端角色。 |
| `dns_id:uint16`、`outcome:utf8`、`query_retries:uint32` | 必填；无查询的行重试次数为 0。 |
| `query_direction/response_direction:uint8`、`qname:utf8`、`qtype/qclass:uint16` | 可空；仅填已安全解析的事实。 |
| `query_at_ns/response_at_ns/latency_ns:int64`、`response_rcode:uint16`、`response_tc:bool`、`incomplete_reason:utf8` | 可空；缺失不填默认零。 |

`session_id` 可用于同一 run 内关联 Basic/Session。`outcome` 固定为 `matched`、
`truncated_response`、`query_only`、`response_only`、`unsupported_message`。
`matched` 的 `incomplete_reason` 为 null；其他值分别使用已冻结的原因：
`truncated_response/dns_tc_set`、`response_only/no_pending_query`、
`unsupported_message/unsupported_opcode_or_transfer`，或查询未完成时的
`response_not_observed_by_deadline`、`session_end`、`response_path_gap`、`capture_truncation`、
`malformed_response`。
这些是捕获视图事实，不是网络原因。完整收到但 TC=1 的响应保留 RCODE 与时间，不能称为完整答案。
响应早于查询完整观测时间或任一时间缺失时，`latency_ns` 为 null，不产负值。
`observed_at` 是响应或独立消息的完整捕获时间、到期 watermark 或会话终结通知时间，随终态原因确定。

### DNS 消息、关联与生命周期

- UDP 一个完整 datagram 至多解析一个 DNS 消息；`body_complete=false` 的 datagram 不生成成功匹配。若 header、问题及 key 均可安全解析，可将对应 pending 以 `capture_truncation` 结束；否则不按 DNS ID 猜测配对，现有 pending 留待水位或会话终结。
- TCP 仅在方向流 origin 为 SYN 且无前置 Gap 时按网络序 2 字节长度读取 DNS 帧；长度 0、超过
  协议/预算界限、midstream 起点或 Gap 不猜测下一帧边界。已丢失边界的方向只排空流事件至 End，
  不再产生该方向消息；响应方向 Gap 使相关未完成查询以 `response_path_gap` 收口。单方向 End 不
  提前终结对向仍可能应答的查询。
- 只关联完整、结构合法、opcode=QUERY 且恰有一个问题的消息。解析 QNAME 时检查 label 长度、
  255 字节总长、压缩指针范围/环、所有 section 与 RR 长度，拒绝越界；名称匹配使用规范 wire label
  （仅 ASCII 大小写折叠），输出转为可逆转义的 UTF-8 展示串，不作 IDNA 转换。响应 RCODE 合成
  header 低 4 位及合法 OPT 的扩展位；不解析答案 RDATA。
- 可完整、安全解析但 opcode 非 QUERY 或问题类型为 AXFR/IXFR 的消息发 `unsupported_message`，
  不建立 pending。畸形/截断消息不产生 `matched`；安全解析出完整 key 的畸形响应以
  `query_only/malformed_response` 收口，否则不凭 DNS ID 猜测。非 DNS/不可安全定位的 payload
  不使任务失败，也不伪造事务行。
- 合法响应只匹配同会话、反向方向、相同 DNS ID/问题的一个 pending；完整响应发最终行并删除状态。无 pending 的合法响应发 `response_only`，时长为 null；重复/迟到响应也遵循这一可观察规则。相同 key 的多个请求在未决期间合并为重试，其响应时长从最早完整查询算起，不声称区分真实重传与新的同 ID 请求。
- 查询完整时间：UDP 为 datagram 捕获时间；TCP 为组成整帧字节的最大捕获时间。响应时间同理。
  deadline 是查询完整时间加配置期限，溢出时饱和到 `INT64_MAX`。只有合法推进的 capture watermark 到达 deadline
  才发 `query_only/response_not_observed_by_deadline`；不由墙钟、批次边界或不变水位触发。
  EOF/idle/tuple reuse 按已有会话终结顺序完成 pending 并发 `query_only/session_end`；失败/取消只 Abort，
  不伪造终态。
- 模块私有帧、副本、pending、索引与输出构建所需状态先计入任务 `kModuleState` 再分配；共享 TCP payload 不重复长期持有。达到单会话 pending 限额、任务预算或分配失败时明确使任务失败，已消费结果前缀不回滚。完成、超时、会话终结、失败与取消均释放计费；不静默逐出、丢弃慢事务或生成伪成功行。

## 主链路

1. **打开与输入**：解析 `features/parameters.dns` → 校验 labeling/标签/stream 消费接口并冻结 Schema →
   唯一 packet 解码/会话/标签/NPI → 已准入 UDP datagram 解析，或经共享 TCP 流分帧 → 按方向和问题关联 →
   `NpmResultRouter` 将类型化终态送往全部结果消费者及被选择的前台实体。
2. **时间与收尾**：合法 capture watermark → DNS pending 到期并输出可观察缺失原因 → 会话终结/EOF 排空
   TCP 方向及未完成事务 → 模块 Finish → 结果消费者 Finish；处理失败/取消仅 Abort/Cancel 并释放状态。

## Feature Tasks 与测试锚点

| 任务 | 交付结果与目标价值/可观察保证 | 验收锚点 |
| --- | --- | --- |
| [x] T0 | 交付冻结的 DNS 配置、消息/事务和结果 Schema 契约，使非法能力组合在 Open 前失败且旧模块 ABI 保持稳定。 | A1、A2 |
| [x] T1 | 交付有界 UDP DNS 消息解析与事务关联，使完整 datagram 的查询、响应、重试和缺失事实能形成唯一终态结果。 | A3～A5 |
| [x] T2 | 交付共享流上的 DNS/TCP 分帧与异常收口，使跨包/跨批次消息可关联而 midstream、Gap 和半关闭不产生伪完整事务。 | A6、A7 |
| [x] T3 | 交付水位驱动的期限、预算与统一结果路由，使高并发 DNS 状态可控且前台/托管消费保持一致。 | A8～A10 |
| [x] T4 | 交付生产目录接入、组合回归与使用说明，使离线用户可显式启用 DNS 并观察结果，既有 Basic/Session 行为保持稳定。 | A11、A12；全部前序锚点 |

| 锚点 | 必须落成的自动断言 |
| --- | --- |
| A1 | 缺 labeling、缺/空/0/重复/未知标签、缺 stream consumer、错配对象、非法参数类型/范围/重复字段均 Open 失败且无半成品；未启用 dns 时节点不启用功能，旧 V1 模块仍可编译运行。 |
| A2 | `dns_transaction` 固定 Arrow Schema/metadata、identity/revision/finality/nullable 列校验；`observing=dns_transaction` 仅在启用 dns 时合法；T0 生产目录仍只有 Basic/Session，T4 接入后为 Basic/Session/dns。 |
| A3 | UDP 查询与反向响应同 ID/问题匹配，NXDOMAIN/SERVFAIL/EDNS 扩展 RCODE、TC 标志和时长正确；不同 session/方向/问题同 ID 不串配。 |
| A4 | 同 key 查询重试合并且首见时间不变；孤儿/迟到响应单独成行、时长 null；同 ID 不同问题并发独立。 |
| A5 | 长度不足、压缩指针环/越界、QDCOUNT 非 1、截断 datagram 不越界、不生成 matched；能安全定位 key 的截断或畸形响应按对应原因收口，不能定位时不猜测。 |
| A6 | DNS/TCP 两字节前缀和多个帧跨 Data、packet、RecordBatch 正确；相同输入不同切分得到相同事务；输入 batch owner 可及时释放。 |
| A7 | SYN 起点、midstream、Gap、FIN 半关闭、RST、idle、tuple reuse、EOF 的事件顺序和未完成原因正确；方向丢失帧边界后不扫描猜测，不重复终态。 |
| A8 | deadline 前、不动水位、恰到 deadline、`INT64_MAX` 饱和、乱序时间、EOF/取消各只产生契约允许的行；`NextEventDeadlineNs` 纳入现有维护计划，不混用单调时钟。 |
| A9 | 单会话 pending 上限、任务预算和分配/编码/emitter/consumer 失败明确终止，已消费前缀不回滚，Abort/Finish 后预算回基线。 |
| A10 | `features` 换序、不同观测域、未启用 Session、`observing` 在 basic/session/dns 间切换：DNS 事务轨迹不变，全部实体仍送结果消费者，前台仅收到所选 Schema。 |
| A11 | 离线 SQL/Scheduler 与托管结果的 history/latest/final 链路可读 `dns_transaction`，Schema 版本和 run_id 隔离；未启用 dns 的原 SQL 和指标不变。 |
| A12 | 标准 CMake 构建、完整 CTest、UDP/TCP/预算/终结路径 ASan/UBSan、格式及 diff 检查通过；记录真实外部依赖与测试结果。 |

T0～T4 按当前结果边界顺序实施。每个实现切片须先在工作台冻结允许文件、CMake target、测试、时间盒与停止条件；单个锚点通过不等于 Feature Task 完成。

## 完成证据

- 2026-10-01：T4 与 Feature 完成。生产目录注册 Basic/Session/DNS，`parameters.dns` 解析失败在 Open 阶段返回，协议对象与非拥有型 TCP stream consumer 指向同一实例；真实 runtime 的组合测试改用生产目录，旧 Basic/Session SQL 保持通过。SQLite 托管消费者为 DNS Schema v1 建立 `history/latest/final`，两个 `run_id` 各自可读同一实体身份；Scheduler SQL 将三种关系读入 DataFrame，按运行 ID 查询隔离。README 增加显式启用、标签快照、观察和托管查询示例。标准 `cmake -B build src` 与全量 `cmake --build build -j$(nproc)` 通过，完整 CTest 26/26；独立 DNS 测试以 ASan/UBSan 构建，UDP/TCP/预算/终结相关四项定向 CTest 4/4。首次 Sanitizer 运行因本机 LeakSanitizer 在 ptrace 环境下不能工作而在测试入口前失败，设置 `ASAN_OPTIONS=detect_leaks=0` 后 ASan 内存访问检查与 UBSan 4/4 通过；未声称泄漏检测通过。依赖实际使用本机 pyarrow、系统 RapidJSON 与 DPDK 23.11.4、缓存的 Arrow 以外第三方构建包；编译告警来自 pyarrow 头文件的 unused-parameter。改动 C++ 格式、`git diff --check` 和 P0/P1 diff 审查通过；未提交或推送。
- 2026-09-30：T3 完成。DNS 协议对象复用已有 `INpmProtocolModuleV1`、共享流 consumer 与 `NpmResultRouter`，对 UDP/TCP 终态使用固定 `dns_transaction` Arrow Schema；水位严格推进时才到期，最近 DNS deadline 进入 runtime 维护计划。pending、TCP 帧、会话身份、终态暂存和单行编码在分配前计入 `kModuleState`，响应、Gap、到期、会话终结、Finish/Abort 与失败释放计费；预算拒绝、单会话上限、Arrow 分配注入、emitter/consumer 错误均以非零失败，不回滚已消费前缀。测试目录的真实 runtime 用例覆盖 `features` 换序、两个观测域、未启用 Session、`observing` 在 DNS/Basic/Session 间切换，以及 consumer 对全部启用实体的接收；DNS 事务轨迹一致，前台仅返回所选 Schema。`cmake -B build src`、六个定向 target 构建、六项定向 CTest（6/6）、全部改动 C++ 格式和 `git diff --check` 通过。编译中可见本机 pyarrow 头文件的 unused-parameter 告警；未运行完整 CTest（Feature 的 T4 验收项）。生产目录仍只注册 Basic/Session，DNS 接入留待 T4。
- 2026-09-30：T2 完成。DNS/TCP 从共享流借用 Data 中按方向复制并拼接长度前缀及完整帧，支持跨回调、多帧和借用 owner 释放；仅 SYN 起点解析，midstream、零长度和 Gap 后不猜测帧边界。响应路径 Gap 将对应 pending 以 `response_path_gap` 收口；单方向 FIN/Reset/End 保留对向应答机会，会话 End 收口剩余查询。测试覆盖切分等价、时间缺失、真实共享流多次 Push、Gap、半关闭、RST、EOF、idle 通知和会话复用。`cmake -B build src`、两个定向 CMake target、四项定向 CTest（4/4）、`clang-format-18 --dry-run --Werror` 与 `git diff --check` 通过；本轮 P0/P1 diff 审查未发现阻塞项。任务预算和 Arrow 路由仍属 T3，生产目录注册属 T4。
- 2026-09-30：T1 完成。新增最多 65535 字节的 UDP DNS 消息解析和单会话 pending 上限内的事务核心；按会话、反向方向、ID 和规范 wire 问题匹配，重试沿用首见时间和身份。完整响应、TC、孤儿/迟到响应、可定位 key 的畸形/捕获截断响应及会话终结分别产生契约终态；无法安全定位 key 的输入不猜测。测试覆盖 NXDOMAIN、SERVFAIL、EDNS 扩展 RCODE、负时长抑制、压缩指针/名称长度、RR/OPT 边界及 A3～A5。`cmake -B build src`、`test_npm_dns_udp` 构建、三项定向 CTest（3/3）、`clang-format-18 --dry-run --Werror`、`git diff --check` 通过。任务预算和 Arrow 路由按 T3 接入，生产目录仍按 T4 接入。
- 2026-09-30：T0 完成。新增拥有型 DNS 配置及消息/事务结构、固定 `dns_transaction` Arrow Schema 和 UDP/TCP 同标签计划；通过测试目录验证配置解析、标签与 labeling、stream consumer 及 `observing` 的 Open 门禁和原子失败。生产目录仍为 Basic/Session，按 T4 再接入。`cmake -B build src`、两个定向 CMake target、`ctest -R '^(test_npm_protocol_contract|test_npm_basic)$'`（2/2）、`clang-format-18 --dry-run --Werror` 与 `git diff --check` 通过；未运行完整 CTest（Feature 尚未完成）。
- 2026-09-30：规格设计完成一轮 P0/P1 审查。复用现有模块与流接口避免 ABI 破坏；借用字节只在同步回调内使用，跨回调副本计费；匹配 key 含会话/方向/问题并检查压缩指针，避免严重错配和越界；单会话上限与任务预算阻止无界状态；正常收口和 Abort 分离。未发现已知 P0/P1 设计阻塞项。
- 本轮仅设计规格、更新 Backlog/工作台；未实现模块、运行新增测试或提交代码。实施验收以以上自动断言为准。
