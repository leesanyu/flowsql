# Feature: NPM HTTP/1 事务分析

状态：[x] T0～T4 已完成（2026-10-01）
优先级：P1
前置：[协议模块运行时](../archive/feat-npm-protocol-analysis.md)、[共享有界 TCP 字节流](../archive/feat-npm-shared-tcp-stream.md)、[流量标签](../archive/feat-flow-labeling.md)。
结果存储复用 [NPM 多实体结果存储与查询](../archive/feat-npm-result-query.md)；本 Feature 不另建数据库链路。

## Non-Goals

- 不解密 HTTPS，不分析 HTTP/2、HTTP/3、h2c、WebSocket 帧、CONNECT 隧道内协议或代理链；不解析 URI、Cookie、认证信息或应用正文语义。
- 不保存正文、全量头部或任意长的 URL/Host；不把 HTTP method、Host、路径、状态码或 NPI 结果用于 TCP 流准入。
- 不新增会话、方向、标签 matcher、TCP 重组、结果数据库、实时采集入口或公共虚表；不改变 Basic/Session/DNS 的输入与 Schema。
- 不将未观察到响应解释为服务故障；不保证被动捕获点的字节与任一端点最终接收内容相同。

## 业务意图

离线排查明文 HTTP/1 服务响应异常的用户，需要在同一 TCP 长连接中辨认请求、最终响应和可观察的响应头时延，
并将未配对、缺口和定界失败表达为捕获视图事实。显式启用 `http1` 后，模块只读取命中冻结主标签的共享 TCP
字节流，输出独立的 `http1_transaction` 事件实体；`observing` 选择前台 DataFrame，托管消费者收到全部已启用实体。

## 核心契约

### 打开、配置与接口

- 模块 ID 为 `http1`，实体 ID 为 `http1_transaction`。`features='http1,labeling'` 显式启用；
  `observing='http1_transaction'` 仅在模块已启用时合法。T0 在测试目录验证契约；T4 才加入生产目录。
- `parameters.http1` 是启用时必填的拥有型 object；沿用 `parameters.schema_version=1`。
  `primary_label_ids` 为 1～256 个互异、非零、存在于冻结标签目录的 uint32；无 labeling/provider、无合法标签、
  缺共享流 consumer、consumer 与协议对象错配时，Open 原子失败。重复/未知字段、null、错误类型及越界值按
  `/http1/...` 路径拒绝；未启用 http1 时参数节点不启动模块。
- `response_timeout_ns` 默认 5000000000，范围 1000000～300000000000；仅由合法推进的 capture watermark
  触发。`max_pending_per_session` 默认 128，范围 1～4096；`max_header_bytes` 默认 65536，范围 1024～1048576，
  同一消息的起始行、头部、chunk 行和 trailer 合计受此限额约束。上限不是正文缓存额度，正文只按定界跳过。
- 配置示例：`{"schema_version":1,"http1":{"primary_label_ids":[1001],"response_timeout_ns":5000000000}}`；
  其中 1001 必须由本任务的标签快照提供，示例不创建标签。
- `NpmModulePlanV1` 使用 TCP input mask、`requires_labeling=true`、`requires_tcp_stream=true` 和冻结标签集合。
  标签 0、未命中或无合法订阅的会话不建立 HTTP 私有解析状态；Basic/Session/DNS 仍按原路径处理输入。
  同一对象实现现有 `INpmProtocolModuleV1` 与 `INpmTcpStreamConsumerV1`，工厂返回非拥有型 stream 指针；
  `OnInput(kTcpPacket)` 只收 segment facts，不解析 payload。字节仅经同步 `OnTcpStreamReadable` 读取。
  不扩展这些 V1 虚表或插件 ABI；T0 先冻结内部配置、消息头、方向解析状态和事务结构及公共内部头文件。

| 内部结构 | 拥有的事实 |
| --- | --- |
| `NpmHttp1ConfigV1` | 标签 ID、response timeout、单会话 pending 上限、消息头上限。 |
| `NpmHttp1MessageHeadV1` | 请求的 method/target/Host 或响应的状态码、HTTP/1.0 或 1.1、头部完成捕获时间与正文定界种类；不持有借用 span。 |
| 方向解析状态 | 至多一份受限头部副本、Content-Length 剩余量或 chunked 扫描标量、SYN 对齐与 Gap/End 状态。 |
| `NpmHttp1TransactionV1` | 任务内唯一身份、会话与请求方向、完整请求头时间、FIFO 位置、信息响应计数和 deadline。 |

### 消息定界与配对

- 仅从方向流 `origin=kSyn` 且之前无 Gap 的首字节开始识别明文 HTTP/1.0、HTTP/1.1；midstream、未知起点
  或非 HTTP 起始字节不扫描寻找下一条起始行。首个合法请求/响应头确定该会话的请求/响应方向，之后不反转。
  `Data` 分块不是消息边界；跨包、批次保存的字节必须复制并计费，不保留 cursor、span 或输入 batch owner。
- 起始行与头部按 CRLF 定界；method 为有界 token、target 为有界原始可打印字节的可逆转义展示值、
  status 为三位 100～599。字段名按 ASCII 不区分大小写，拒绝裸 LF、obs-fold、非法控制字节、冒号前空白、
  冲突/重复 Content-Length、重复 Transfer-Encoding、同时存在 Transfer-Encoding 与 Content-Length、
  重复 Host、数值溢出及超限头部。只记录安全解析的单个 Host（去两端 OWS 后可逆转义），不保存其他头部。
- V1 支持无正文、单个十进制 Content-Length，以及 `Transfer-Encoding: chunked` 的分块长度、CRLF 与有界
  trailer 扫描；正文内容同步跳过，不长期复制。HEAD 响应、1xx、204、205、304 均按无正文定界；无长度的
  HTTP/1 响应为 close-delimited，发出最终响应头结果后不再寻找下一条响应，直到该方向 End。
  不支持的传输编码、畸形 chunk/trailer、超出 `max_header_bytes` 或无法唯一确定长度时停止该会话的后续
  HTTP 定界与配对，不从正文中猜测新起始行。头部超限标为 `header_limit_exceeded`，不是任务预算失败；
  HTTP/1.0 无长度请求按无正文处理。
- 只有头部语法及正文定界信息均验证后，请求才进入 FIFO pending，最终响应才配对队首；任何定界错误
  都不得先发 `matched`。100/102/103 等 1xx 仅增加队首
  `informational_count`，不结束事务；101 作为最终响应输出后停止两方向 HTTP 解析。
  CONNECT 收到 2xx 后同样停止解析隧道字节。无 pending 的最终响应发 `response_only`，不伪造请求或时延。
  支持 pipelining 与跨批次 keep-alive，但不按 method、Host、URL 或状态码猜测配对。
- 一旦任一方向出现 Gap、捕获截断导致的 Gap、错误帧界或队首 response deadline 到达，该会话不再安全
  确定 FIFO 对齐：按原因终结当前 pending，随后排空流事件至 End，不再生成新的配对事务。
  Gap 已标记捕获截断时优先记录 `capture_truncation`，否则按请求/响应方向记录路径 Gap。
  deadline 命中时队首为 `response_not_observed_by_deadline`，其余为 `pipeline_alignment_lost`；不会把迟到响应
  配给下一请求。响应方向 FIN 在无最终响应时可结束 pending；请求方向 FIN 仍允许反向响应。
- 请求头完成时间与响应头完成时间分别为构成头部字节的最大捕获时间；任一缺失或响应时间较早，
  `latency_ns` 为 null。`matched` 只表示双方头部被完整观察，绝不宣称响应正文完整。
  deadline 从完整请求头时间起算，超时只由 capture watermark 触发，加法溢出饱和到 `INT64_MAX`；
  EOF/idle/tuple reuse 按会话结束顺序收口 pending，失败/取消只 Abort，不生成正常终态。

### 输出与资源保证

`http1_transaction` 固定 Arrow Schema v1，metadata 包含 `flowsql.entity=http1_transaction`、
`flowsql.schema_version=1`、`flowsql.timestamp_unit=ns`、`flowsql.revision_semantics=event`、
`flowsql.measurement_scope=single_capture_observed_packets`。每个事务最多一行，`entity_instance_id` 非零、
`revision=1`、`is_final=true`；通过 `Emit("http1_transaction", rows)` 路由。结果身份在同一 run 内独立于 Basic/Session/DNS。

| 列（Arrow 类型） | 约束 |
| --- | --- |
| `entity_instance_id:uint64`、`revision:uint64`、`observed_at:int64`、`is_final:bool` | 必填；事件身份、终态与观测时间。 |
| `session_id:uint64`、`observation_domain_id:uint64`、`a_ip/b_ip:utf8`、`a_port/b_port:uint16` | 必填；沿用规范化会话端点。 |
| `outcome:utf8`、`informational_count:uint32` | 必填；计数仅含 1xx 非 101。 |
| `request_direction/response_direction:uint8`、`method/target/host:utf8`、`status_code:uint16` | 可空；只填安全观测的头部事实。 |
| `request_headers_at_ns/response_headers_at_ns/latency_ns:int64`、`incomplete_reason:utf8` | 可空；缺失时不填零。 |

`outcome` 固定为 `matched`、`request_only`、`response_only`；`matched` 的 `incomplete_reason` 为 null，
`response_only` 为 `no_pending_request`。`request_only` 原因固定为
`response_not_observed_by_deadline`、`pipeline_alignment_lost`、`request_path_gap`、`response_path_gap`、
`capture_truncation`、`framing_unsupported`、`header_limit_exceeded`、`malformed_response`、
`response_stream_end` 或 `session_end`。
`observed_at` 是响应头完成时间、deadline watermark、Gap/解析失败通知或终结通知时间；
无可用捕获时间时使用当前事件通知时间。
这些值只描述捕获视图，不推断端点行为。

- 头部副本、chunk/trailer 扫描、会话角色、FIFO pending、索引和输出构建所需私有状态在分配前计入任务
  `kModuleState`；共享流字节只由共享流计一次。达到单会话 pending 限额、任务预算或实际分配失败时明确失败，
  不静默丢请求、逐出慢事务或把资源错误伪装为捕获缺口。
- emitter/托管消费者失败使任务按既有链路 FailRun/Abort；已消费结果前缀不回滚。正常结束、超时、
  会话终结、失败和取消均释放计费；输出 batch 生命周期沿用结果路由，不借用已销毁的输入。

## 主链路

1. **打开与输入**：解析 `features/parameters.http1` → 冻结标签/Schema 和单实例消费接口 → 唯一 Basic 会话链
   准入共享 TCP 流 → 方向头部/正文定界 → FIFO 请求与最终响应关联 → `NpmResultRouter` 将终态送全部消费者，
   前台只见 `observing` 所选实体。
2. **时间与收尾**：合法 capture watermark 推进 → 队首期限触发并封闭失去对齐的会话 →
   FIN/会话终结/EOF 排空两方向与 pending → 模块 Finish → 结果消费者 Finish；失败/取消仅 Abort/Cancel。

## Feature Tasks 与测试锚点

| 任务 | 交付结果与目标价值/可观察保证 | 验收锚点 |
| --- | --- | --- |
| [x] T0 | 交付冻结的 HTTP/1 配置、消息头与事务 Schema 契约，使非法能力组合在 Open 前失败且现有 V1 ABI 不变。 | A1、A2 |
| [x] T1 | 交付有界 HTTP/1 流式定界，使跨批次的头部、固定长度和 chunked 正文不会被误认作下一消息。 | A3、A4 |
| [x] T2 | 交付 FIFO 管线事务与水位/终结收口，使多请求长连接只产生有观测依据的响应配对和时延。 | A5～A7 |
| [x] T3 | 交付任务预算与统一结果路由，使解析状态有界且前台/托管消费对失败及多实体保持一致。 | A8、A9 |
| [x] T4 | 交付生产目录接入、离线 SQL/托管查询回归与使用说明，使用户可显式启用 HTTP/1 而旧结果保持稳定。 | A10、A11；全部前序锚点 |

| 锚点 | 必须落成的自动断言 |
| --- | --- |
| A1 | 缺 labeling、缺/空/重复/未知标签、缺/错 stream consumer、参数重复/未知/null/越界均 Open 原子失败；未启用 http1 时不启动模块，旧 V1 模块可编译运行。 |
| A2 | 固定 Schema/metadata、事件身份、nullable 列与 `observing=http1_transaction` 门禁；T0 生产目录仍为 Basic/Session/DNS，T4 才增加 http1。 |
| A3 | 相同 HTTP/1.0/1.1 输入按不同 Data/packet/batch 切分得到同样的消息边界；Content-Length、chunked+trailer、无正文与 close-delimited 均不把正文误判为起始行，输入 owner 可释放。 |
| A4 | 裸 LF、obs-fold、重复/冲突长度、TE+CL、畸形 chunk、超限头部、非 HTTP 与 midstream/Gap 都不越界、不扫描猜测下一起始行；资源限额与协议不支持分别收口。 |
| A5 | keep-alive 与 pipelining 的两个请求/两个最终响应按 FIFO 逐一配对；100/103 不占事务，101、CONNECT 2xx 后无隧道误解析；孤儿最终响应独立成行。 |
| A6 | 请求/响应头完成时间与 nullable 非负时延正确；deadline 前、不动水位、恰到 deadline、饱和与乱序时间只产生允许的结果；队首超时后不把迟到响应错配给下一请求。 |
| A7 | 请求 FIN 半关闭仍可配响应，响应 FIN、Gap/截断、RST、idle、tuple reuse、EOF 与取消按规定收口；正常结束最多一行，Abort 无伪终态。 |
| A8 | pending 上限、任务预算、分配/编码/emitter/consumer 失败使任务非零终止；头部上限仅封闭该会话且不误报预算失败；释放后预算回基线，已消费前缀不回滚。 |
| A9 | `features` 换序、不同观测域、未启用 Session、`observing` 在 basic/session/dns/http1 间切换不改变 HTTP 事务轨迹；全部启用实体送托管消费者。 |
| A10 | 离线 SQL/Scheduler 可显式启用 HTTP/1；托管 `http1_transaction` 的 history/latest/final、Schema v1 与 run_id 隔离可查询，未启用 http1 的旧 SQL/指标不变。 |
| A11 | 标准 CMake 构建、完整 CTest、定界/管线/预算/终结路径 ASan/UBSan、格式与 diff 检查通过，记录实际依赖和运行结果。 |

每个实施切片先在 `active_task.md` 冻结允许文件、CMake target、测试、时间盒和停止条件；单个锚点通过不等于 Feature Task 完成。

## 完成证据

- 2026-10-01：完成一轮 P0/P1 设计审查。复用现有协议与共享流虚表，HTTP 对象和 stream consumer 同实例以避免 ABI/所有权破坏；借用字节仅在回调内使用，跨回调解析副本受头部上限与任务预算双重约束；Gap、无唯一帧界和队首超时后停止 FIFO 配对，避免严重错配。未发现已知 P0/P1 设计阻塞项。
- 2026-10-01：本轮仅冻结规格并更新 Backlog/工作台；未实施 T0～T4、编译新增模块、运行新增协议测试或提交代码。实施结果以以上自动断言为准。
- 2026-10-01：T0 完成。新增拥有型 HTTP/1 配置与内部消息/方向/事务结构、TCP 与 labeling 模块计划、固定 22 列 `http1_transaction` Schema v1；测试专用 catalog 覆盖 A1 的配置/能力/consumer 原子失败与 A2 的 Schema、事件行和 observing 门禁。生产目录仍为 Basic/Session/DNS。`cmake -B build src`、`cmake --build build -j$(nproc)`、完整 CTest 27/27、clang-format 与 `git diff --check` 通过；未实施 T1～T4，未提交或推送。
- 2026-10-01：T1 完成。新增单方向有界 HTTP/1 定界器，直接接收共享流 Data/Gap/End，头部与正文完成以同一消息 ID 分别通知；Content-Length、chunked/trailer、无正文与 close-delimited 按状态跳过正文，101/CONNECT 2xx 后停止扫描。A3/A4 定向测试覆盖多种 Data 切分、输入 owner 释放、时间、边界/非法头、畸形 chunk、未完成 End、midstream/Gap 与累计控制字节上限。标准 CMake 全量构建、完整 CTest 28/28、clang-format、`git diff --check` 通过；定向 ASan/UBSan 通过（环境 ptrace 限制使 LeakSanitizer 关闭）。生产目录未接入 HTTP/1；未实施 T2～T4，未提交或推送。
- 2026-10-01：T2 完成。新增会话级 FIFO 事务跟踪器，按已验证头部顺序关联最终响应，在双方安全定界后才输出 `matched`；支持 HEAD/CONNECT/101、信息响应计数、孤儿响应、跨批次管线与方向隔离。严格推进的 capture watermark 触发 deadline，迟到响应不再配对；Gap/截断、FIN、RST、idle、tuple reuse、EOF 和 Abort 按捕获事实收口。A5～A7 定向断言通过；标准 CMake 全量构建、完整 CTest 29/29、clang-format、`git diff --check` 与定向 ASan/UBSan 通过（环境 ptrace 限制使 LeakSanitizer 关闭）。本阶段仅产生拥有型事务事实；任务预算、Arrow 结果编码与路由仍属于 T3，生产目录接入属于 T4。未提交或推送。
- 2026-10-01：T3 完成。HTTP/1 私有会话、pending、单次流事件暂存、结果行和 Arrow 行构建在分配前预留 `kModuleState`，会话终结、失败及 Abort 释放；固定 22 列 Schema v1 经同步 `Emit(http1_transaction)` 进入既有结果路由。A8 定向断言覆盖 pending/预算拒绝、头部上限原因、编码分配与 emitter 错误、取消及预算回基线；A9 在测试目录以真实 HTTP/1 模块和临时 catalog 覆盖 feature 顺序、两个观测域、未启用 Session、basic/session/dns/http1 的 observing 切换、全部启用实体的托管消费和消费者失败保留已消费前缀。标准 CMake 全量构建、完整 CTest 31/31、clang-format、`git diff --check` 及 HTTP/1 定向 ASan/UBSan 4/4 通过；因环境 ptrace 限制，Sanitizer 运行时使用 `ASAN_OPTIONS=detect_leaks=0`。生产目录与 SQL 接入仍属于 T4；未提交或推送。
- 2026-10-01：T4 完成。生产 catalog 和共享库接入 `http1/http1_transaction`；真实 Scheduler 离线 SQL 用 SYN 对齐的 HTTP/1 PCAP、任务内标签 provider 与精确快照验证前台 `matched`、状态码 200、100000 ns 头部时延、观测域 77 和 22 列 Schema v1；同一托管目标的两次运行在 history/latest/final 查询中按 run_id 隔离，旧 Basic/Session/DNS SQL 回归通过。README 已给出显式配置、托管查询与捕获视图解释。`cmake -B build src`、`cmake --build build -j$(nproc)`、完整 CTest 31/31、定向 ASan/UBSan 4/4、改动范围 clang-format 与 `git diff --check` 通过；Sanitizer 运行使用 `ASAN_OPTIONS=detect_leaks=0`。未提交或推送。
