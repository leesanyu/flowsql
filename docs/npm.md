# NPM 网络会话与协议分析

`npm.basic` 分析 PCAP/PCAPNG 或持续采集流量，提供 Basic 周期统计、Session 性能指标以及
DNS、HTTP/1、TLS、ICMP 事件。多个模块共享解码和会话归属；IPv4、IPv6 分别输出，
可以选一个实体写入 DataFrame，也可以将所有启用实体保存到数据库。

从排障问题出发，Basic 用来查看流量变化，Session 用来判断连接和传输表现，协议事件用来定位
DNS、明文 HTTP/1、TLS 或 ICMP 的具体交互。它们分析的是当前捕获点可见的流量，指标的有效性与
捕获是否完整、是否同时看见两个方向有关。

常用入口：

- [运行准备与快速开始](#运行准备与快速开始)
- [选择模块和结果](#选择模块和结果)
- [配置时间、来源和资源](#配置时间来源和资源)
- [Basic 周期统计](#理解统计结果)
- [Session 会话性能指标](#session-会话性能指标)
- [组合协议分析与结果语义](#组合协议分析与结果语义)
- [TLS 初始握手分析](#tls-初始握手分析)
- [保存所有实体并查询](#保存所有实体并查询)
- [共享 TCP 流的边界](#协议解析与共享-tcp-流的边界)
- [性能说明](#性能说明)

## 运行准备与快速开始

执行任务的 Scheduler 进程需要加载 NPI、输入通道插件和 BinAddonHost；通过 Web「算子管理 → C++ 插件」
上传并激活 `build/output/libflowsql_npm_basic.so`。NPI 需配置可读的协议词典，例如
`{"ldfile":"./config/protocols.yml"}`。数据库通道和输入通道应先创建。

读取已创建的 PCAP 通道，同时分析 Basic 与 Session，将 IPv4 Session 结果保存为 DataFrame：

```sql
SELECT * FROM pcapfile.capture
USING npm.basic
WITH features='basic,session', observing='session_ipv4'
INTO dataframe.session_metrics
```

有限 PCAP 可同步执行；持续采集任务通过 `/scheduler/batch/submit` 异步提交。
持续来源使用两段数据库目标保存多实体结果，例如 `INTO mysql.npm`；普通 DataFrame 不能作为持续来源的输出目标。

## 选择模块和结果

| `features` 项 | 结果实体，后缀分别为 `_ipv4` / `_ipv6` | 用途 |
| --- | --- | --- |
| `basic` | `basic_ipv4`、`basic_ipv6` | 会话双向包数、字节数及周期增量 |
| `session` | `session_ipv4`、`session_ipv6` | 速率、TCP RTT、重传等捕获点可观察指标 |
| `dns` | `dns_transaction_ipv4`、`dns_transaction_ipv6` | DNS 事务与响应关联 |
| `http1` | `http1_transaction_ipv4`、`http1_transaction_ipv6` | 明文 HTTP/1 请求与响应关联 |
| `tls` | `tls_handshake_ipv4`、`tls_handshake_ipv6` | TLS 握手可见信息 |
| `icmp` | `icmp_event_ipv4`、`icmp_event_ipv6` | Echo 配对和控制报文事件 |

默认只启用 `basic`。DNS、HTTP/1、TLS 还需在 features 中启用 `labeling`，并配置有效标签规则及协议模块订阅；
`labeling` 是能力项，不是结果实体。配置方法见 [Flow Labeling 使用说明](flow-labeling.md)。
具体模块示例可从 [README 的 NPM 章节](../README.md#npmbasic-网络会话与协议分析) 进入。

`observing` 选择一个已启用的具体族实体，不会隐式启用模块。普通 DataFrame 输出必须显式指定；
它只包含所选实体，mixed 输入中的另一族结果应通过数据库托管目标留存。
两段数据库托管输出可以省略 observing，此时只入库，前台为零列零行；指定 observing 不会缩减入库实体。
同一模块的两族结果拥有固定 Schema，不随标签开关变化。IPv4 地址存为 UInt32
（如 `1.2.3.4 = 0x01020304`），IPv6 地址存为网络序 16 字节二进制；Web 查询展示时转为标准地址文本。

## 配置时间、来源和资源

| 配置 | 默认值或行为 | 说明 |
| --- | --- | --- |
| `input_namespace` | FROM 通道名 | 区分逻辑输入来源；相同五元组在不同 namespace 中不会合并 |
| `source_domains` | `'all'` | 默认每个 source_id 对应独立观测域，也可显式合并来源 |
| `result_mode` | `'periodic_snapshot'` | Basic 周期增量及累计值；`'final'` 仅输出会话终态 |
| `output_interval_ns` | `30000000000` | Basic 周期宽度，默认 30 秒，范围 10 ms～1 h |
| `run_mode` | 按来源能力选择 | 有限离线源为 offline，持续采集源为 realtime；显式冲突会失败 |
| `parameters` | 共享与模块默认配置 | V1 JSON 信封，`core` 放共享配置，模块名放私有配置 |

例如，统计 15 秒周期，同时限制活动会话数量：

```sql
SELECT * FROM pcapfile.capture
USING npm.basic
WITH observing='basic_ipv4',
     parameters='{"schema_version":1,"core":{"output_interval_ns":15000000000,"max_active_sessions":100000}}'
INTO dataframe.flow_timeline
```

使用 parameters 时，不能再混用顶层共享调优参数；features、observing、input_namespace、source_domains 仍可独立指定。
默认活动会话容量为 100,000，tracked 预算为 256 MiB，pending 预算为 64 MiB；两族共享同一任务总量。
预算约束任务计费资源，不等同进程 RSS。达到容量或预算限制会明确失败，不会静默驱逐会话或丢弃结果。

### 共享与模块参数

`parameters` 是一个 JSON 字符串：`schema_version` 固定为数字 `1`，`core` 配置任务共享的时间、容量、
标签和结果保存策略，一级模块名配置该模块的私有参数。例如，同时设置 Basic 周期和 Session 跟踪上限：

```json
{
  "schema_version": 1,
  "core": {
    "output_interval_ns": 15000000000,
    "max_active_sessions": 100000
  },
  "session": {
    "max_tcp_ranges_per_direction": 1024
  }
}
```

将该对象序列化后放入 SQL 的 `parameters='...'`。`features` 仍需包含 `session`；写入模块配置不会
自动启用模块。省略 `core` 或其中的字段使用默认值；Session 可省略私有配置，DNS、HTTP/1、TLS
需要按模块要求提供标签订阅等配置。任务启动时冻结配置，后续更新配置资源不会热修改已运行的任务。

`session.max_tcp_ranges_per_direction` 默认 1024，范围 8～65536，限制每个 TCP 会话每个方向的
TCP 序列号与确认状态的跟踪区间数。它影响传输指标能跟踪的复杂程度，不是活动会话容量；会话容量由
`core.max_active_sessions` 控制。协议解析的共享字节缓存另见[共享 TCP 流配置](#协议解析与共享-tcp-流的边界)。

参数按实际类型校验：数字和布尔值使用 JSON 原生类型，例如 `1024` 而非 `"1024"`；信封、`core`
和已启用模块消费的配置不接受未知字段、`null`、错误类型、重复键或越界值。报错路径如
`/session/max_tcp_ranges_per_direction` 可帮助定位配置。未启用或未知模块的对象不参与运行及业务字段校验，
但仍须是合法 JSON 对象，且整个 JSON 不允许重复键；`framework` 是禁止使用的保留节点。
单份 JSON 最多 64 KiB，嵌套深度最多 64 层。

### 区分输入来源与观测域

来源映射使用 `source_id:observation_domain_id`，多项用分号分隔，例如 `source_domains='0:7;1:7;2:8'`。
普通 PCAP 来源编号为 0，PCAPNG 使用实际接口来源编号；显式映射必须覆盖输入的所有来源。
仅在多个来源属于同一观测范围时将它们映射到同一域。`input_namespace` 区分逻辑输入，
观测域区分该输入中的捕获范围；相同五元组在不同 namespace 或观测域中分别统计。

## 理解统计结果

周期按报文采集时间划分为 UTC epoch 对齐的 `[k × P, (k + 1) × P)`，与文件回放速度无关。
安全时间水位确认跨过右边界后才封闭周期；采集积压或数据库背压可能延后交付。普通轮询超时不能单独证明周期完整。

| 字段或字段组 | 阅读方式 |
| --- | --- |
| `period_start_ns`、`period_end_ns` | 周期的名义左右边界 |
| `period_complete` | 是否已确认覆盖到周期右边界；不足一个周期的尾部为 false |
| `interval_packets_*`、`interval_wire_bytes_*` | 本条新交付增量，可按非重叠周期累加 |
| `packets_*`、`wire_bytes_*` | 截至该边界的会话累计量，不能跨 revision 求和 |
| `session_id`、`revision` | 任务内会话身份与递增版本；周期切换不创建新会话 |
| `is_final`、`end_reason` | 会话终结及原因，与周期是否完整分别判断 |
| `observed_at` | 结果生成时间，不代替统计周期 |

字节计数使用报文的 wire_len，单位 byte，重传仍计入观察到的流量。正常 EOF/Stop 排空尾部并输出终态；
零增量终态用于标记结束，不能当作新的学习事实。`result_mode='final'` 的周期字段为 NULL，累计字段仍有效。
Session 和协议指标用状态与 nullable 值表达证据不足，不能把 NULL 解释为零 RTT 或无重传。
A→B/B→A 是规范化方向，不自动等于客户端/服务端。

## Session 会话性能指标

启用 `features='basic,session'` 并选择 `observing='session_ipv4'` 或 `session_ipv6`，可以在同一次
分析中查看流量统计与连接性能。Session 沿用 Basic 的会话身份、方向和生命周期，切换周期不会重置
TCP 跟踪状态；同一五元组被重新用于新连接时，会生成新的会话实例。

| 想了解的问题 | 主要字段 | 如何解读 |
| --- | --- | --- |
| 会话已观察多久？ | `first_ns`、`last_ns`、`duration_ns` | 持续时间是最后与首次观察报文的采集时间差；时长单位 ns |
| 捕获到多少流量、平均多快？ | `wire_bytes_*`、`wire_bps_*` | 以 wire_len 累计，包括重传；bps 是 bit/s |
| 传输层声明了多少负载？ | `payload_bytes_*`、`payload_bps_*` | TCP/UDP 声明的 payload 量，包括重复序列；不是应用正文大小 |
| TCP 去重后有多少负载？ | `tcp_unique_payload_bytes_*`、`tcp_unique_payload_bps_*` | 按可判定的 TCP sequence 区间去重，不等于应用成功接收量 |
| TCP 建连用了多久？ | `tcp_handshake_duration_ns`、`tcp_synack_rtt_ns` | 前者从初始 SYN 到完成三次握手的 ACK，后者从 SYN 到匹配 SYN-ACK；须有足够证据 |
| 数据传输往返时延如何？ | `tcp_rtt_samples`、`tcp_rtt_min_ns`、`tcp_rtt_mean_ns`、`tcp_rtt_max_ns` | 从同时可见的发送和累计 ACK 取样，排除重传导致对应关系不清的样本 |
| 看到了多少重传？ | `tcp_retrans_packets_*`、`tcp_retrans_payload_bytes_*` | 描述捕获点可见的序列重叠；不输出或推算网络丢包率 |

`*` 代表 `_ab` / `_ba` 两个方向。所有速率使用从会话首包到当前最后观察包的累计量计算，
`bps = bytes × 8 × 10^9 / duration_ns`。它们是累计平均速率，不是最近一个 Basic 周期的速率；
会话累计计数和这些指标不能跨 revision 相加。要查看周期流量变化，读取 Basic 的 interval 字段。

先看状态，再看数值：

| 状态字段 | 值与含义 |
| --- | --- |
| `rate_status` | `valid`：观察时长为正，可计算速率；`insufficient_span`：时长不足，速率为 NULL |
| `tcp_handshake_status` | `complete`：完整观察到握手；`partial`：只有部分阶段；`not_observed`：未观察到；`ambiguous`：无法可靠对应；`not_applicable`：不适用 |
| `tcp_rtt_status` | `valid`：有可靠样本；`no_sample`：尚无样本；`ambiguous`：无法可靠判定；`not_applicable`：不适用 |
| `tcp_retransmission_status` | `valid`：可判定，零值表示未观察到重传；`ambiguous`：无法可靠判定；`not_applicable`：不适用 |

UDP 的 TCP 专属指标为 NULL，相关状态为 `not_applicable`。NULL 表示无可用数值，不能替换为
零 RTT 或零重传；`tcp_rtt_status='no_sample'` 时样本数为 0，时延仍为 NULL。

`measurement_flags` 提示观察条件，可以组合：1 为中途开始捕获，2 为 payload 截断，4 为时间戳倒退，
8 为序列判定存在歧义，16 为 SYN 重传。结合状态字段判断哪些指标可用，不要仅凭 flags 推断服务故障。

## 组合协议分析与结果语义

多个模块在同一次读取中共享报文解码、会话归属、协议识别和主标签，避免重复分析同一份输入。
`features` 的排列顺序不决定结果；`observing` 只选择前台实体，其他启用模块仍继续分析、处理超时并入库。

| 排障场景 | 模块提供的观察结果 | 使用边界 |
| --- | --- | --- |
| DNS 是否收到响应、响应状态如何？ | `dns` 关联 UDP/TCP DNS 请求和响应，给出观测时延与不完整原因 | 不解密 DoT/DoH |
| 明文 HTTP 服务是否响应、返回什么状态？ | `http1` 关联请求与响应，给出状态和观测时延 | 不解密 HTTPS，不分析 HTTP/2、HTTP/3 |
| TCP 上 TLS 初始协商到达哪一步？ | `tls` 提供可见 Hello、协商信息、明文 fatal alert 和不完整原因 | 可见 ServerHello 不等于完整握手成功 |
| 网络是否回显、收到何种差错？ | `icmp` 关联 Echo 请求/响应，解析 ICMP/ICMPv6 控制消息 | 被引用的 TCP/UDP 会话可以不存在，不强制归入端口会话 |

DNS、HTTP/1、TLS 只分析命中所订阅主标签的流量；Basic/Session 仍统计进入任务的全部适用报文。
ICMP 无须标签准入或共享 TCP 流。一个 TCP/UDP 会话可以产生多个 DNS/HTTP 事务，事务身份与
`session_id` 分开管理；因此协议结果行数不能直接当作连接数或输入包数。

`is_final=true` 表示该实体实例的生命周期已结束。是否获得响应、是否失败或证据不足，要看各模块的
业务状态和原因字段；终态本身不表示成功。协议期限依据报文采集时间及可靠的采集时间水位推进，
离线文件回放速度或普通空轮询不能当作协议超时证据。正常 EOF/Stop 会排空并收尾，失败或取消不会
伪造成功终态。每个任务独立保存状态并受预算约束；普通 DataFrame 输出也不会无限积存未观察实体的结果。

## TLS 初始握手分析

TLS 模块用于排查 TCP 上 TLS 1.2/1.3 初始协商。先部署 Flow Labeling，准备精确版本的标签配置，
再订阅要分析的主标签；下面的通道、配置引用和标签 ID 均需替换为环境中实际存在的值：

```sql
SELECT * FROM pcapfile.capture
USING npm.basic
WITH features='tls,labeling', observing='tls_handshake_ipv4',
     parameters='{"schema_version":1,"core":{"labeling":"config.tls-labels@7"},"tls":{"primary_label_ids":[1001],"handshake_timeout_ns":5000000000}}'
INTO dataframe.tls_handshakes
```

| `parameters.tls` 参数 | 默认值与范围 | 用途 |
| --- | --- | --- |
| `primary_label_ids` | 必填，1～256 个互异、非零且存在于标签配置中的 ID | 选择需要分析的流量 |
| `handshake_timeout_ns` | 5000000000，范围 1000000～300000000000 ns | 从候选 ClientHello 首字节的采集时间起计算观察期限 |
| `max_hello_bytes` | 65536，范围 4096～1048576 bytes | 限制单个 Hello 消息大小 |

只从捕获到 SYN 的方向的字节流起点识别初始 ClientHello；不扫描中途流量、STARTTLS 或代理隧道
寻找握手。没有建立 TLS 候选的会话不输出事件，每个候选最多一条终态事件；TLS 事件数不是全部 TCP 连接数。

| 结果字段 | 可读取的信息 |
| --- | --- |
| `client_sni` | 完整 ClientHello 中可见的外层 SNI，不推断 ECH 内部名称 |
| `client_alpn_protocols` | 客户端提出的 ALPN 列表，保存为 JSON 字符串数组；不等于协商结果 |
| `selected_version`、`cipher_suite` | 最终 ServerHello 选择的版本与密码套件；TLS 1.2 为 `0x0303`，TLS 1.3 为 `0x0304` |
| `selected_alpn` | TLS 1.2 明文 ServerHello 中可见的选择；TLS 1.3 的选择被加密，因此为 NULL |
| `server_hello_latency_ns` | 第一次完整 ClientHello 到最终完整 ServerHello 的捕获时差，有可靠时间才填写；不是 TCP 建连时长 |
| `hello_retry_count` | HelloRetryRequest 次数，0 或 1；重试仍属于同一事件，时延从第一次 ClientHello 计算 |

用 `outcome` 和 `incomplete_reason` 判断证据：`server_hello_observed` 只证明看见最终 ServerHello；
`fatal_alert_observed` 只在实际看见明文 fatal alert 时产生；`incomplete` 表示候选的观察不足或
无法继续解析，例如捕获缺口、截断、期限到达或未支持的消息。未见 ServerHello 不能直接判为服务端故障。
`incomplete_reason` 也会说明已见 ServerHello 后因加密边界、期限或会话结束而停止观察的原因。

模块不解密后续握手或应用数据，不验证证书、Finished 或身份，不分析 QUIC/DTLS；TLS 1.3 的后续
加密 record 不用于证明握手成功。超过 Hello 大小上限会得到协议不完整原因，任务预算不足、内存分配
或数据库写入失败则是任务执行失败，两者应分别排查。

## 保存所有实体并查询

SQLite、MySQL、PostgreSQL、ClickHouse 都支持两段数据库托管目标：

```sql
SELECT * FROM pcapfile.capture
USING npm.basic
WITH features='basic,session'
INTO mysql.npm
```

`mysql.npm` 表示 MySQL 类型下名为 npm 的数据库通道，实际数据库由通道配置决定；同样可使用
`sqlite.<通道名>`、`postgres.<通道名>` 或 `clickhouse.<通道名>`。这是保存全部实体的托管目标，
不接受三段式单表目标。每条分析 SQL 只有一个目标；需要 DataFrame 时，从保存后的关系另行查询。

源阶段 `WHERE`（位于 `USING` 前）缩小所有模块的分析输入。指定具体族 `observing` 时，投影和
算子结果阶段 `WHERE` 只处理前台所选实体，数据库仍保存全部启用实体；省略 observing 时要求
`SELECT *` 且没有结果阶段过滤。

任务响应的 `result` 提供 `run_id`、`run_status`、`metadata_status`、`rows_written` 和 `entities[]`。
`rows_written` 是已确认写入的结果行数，不是输入包数；各实体给出 Schema 版本及
`history_relation`、`latest_relation`、`final_relation`，应直接使用这些实际关系名查询。
每个实体按 Schema 版本提供三种只读关系：

| 关系 | 内容 |
| --- | --- |
| `npm_<entity>_history_v<version>` | 运行内全部可见 revision |
| `npm_<entity>_latest_v<version>` | 运行内每个实体实例的最新 revision |
| `npm_<entity>_final_v<version>` | `is_final=true` 的终态 |

```sql
SELECT * FROM mysql.npm.npm_session_ipv4_latest_v1
WHERE __npm_run_id='替换为本次响应中的run_id'
INTO dataframe.latest_sessions
```

关系包含 `__npm_run_id`、`__npm_task_id`、`__npm_run_status`。同一 task_id 的重复执行仍由 run_id 隔离；
latest 表示各运行内每个实例的最大 revision，不是“最近一次运行”。Basic/Session 的快照会更新 revision，
独立协议事件则固定 `revision=1`、`is_final=true`，所以同一事件可以在三种关系中分别查到。
它们是同一保存结果的查询视图，不要把三种查询结果相加统计事件数。
需要基线分析时，按明确 run_id 读取 Basic history 的 interval 增量，见 [Baseliner 使用说明](baseliner.md)。

### 运行状态与结果保留期

| `run_status` | 对查询的意义 |
| --- | --- |
| `writing` | 正在写入；当前可见结果只是已提交前缀 |
| `completed` | 正常完成并排空结果 |
| `incomplete` | 失败或取消；已提交结果可保留用于排查，不能当作完整运行 |
| `purging` | 正在回收过期运行，公开关系不再显示该运行 |

写入失败时不会自动重放提交结果未知的批次。`metadata_status='unknown'` 表示运行状态落库结果无法
确认，即使响应报告失败，也不能据此声称数据库已成功标记为 incomplete；`known` 表示状态落库已确认。
`npm_` 是托管对象的保留前缀，同名对象冲突或手工改变托管表结构会使打开失败，不会自动覆盖用户表。

默认不自动过期。托管目标可设置本次运行的保留期，例如 30 天：

```sql
SELECT * FROM pcapfile.capture
USING npm.basic
WITH features='basic,session',
     parameters='{"schema_version":1,"core":{"result":{"retention_days":30}}}'
INTO mysql.npm
```

`retention_days` 为 1～3650 的整数，仅作用于本次 run，从运行创建时间计算；普通 DataFrame 目标不接受
该配置。到期后公开关系不再显示该运行；终态运行的物理数据由后续托管任务打开时分批回收，
没有新任务时回收可以延后，`writing` 运行的物理数据不自动清除。

ClickHouse 使用 MergeTree 存储，通过 ArrowStream 列式批次同步写入，自动复用写连接；无需额外开关。
结果数据不再展开为 URL 中的逐字段参数，系统列和 IPC 缓冲受同一 pending 预算控制。
成功响应才增加已确认行数；批次在现有 ProcessBlock、Flush/EOF 和发布前排空，数据库变慢会形成背压。

## 协议解析与共享 TCP 流的边界

DNS、HTTP/1、TLS 可复用任务内部的共享有界 TCP 字节流；没有独立的 tcp_stream feature 或结果表。
共享流仅为命中合法订阅的标签创建状态，同一方向的 payload 保存一份，各消费者独立推进。
Data 分块不表示应用消息边界；重传/重叠采用首次捕获可见字节，缺失范围以 Gap 表达。
Gap 可能由捕获截断产生，不能直接解释为网络丢包；End 结束捕获视图，也不保证应用消息完整。

| `parameters.core.tcp_stream` 字段 | 默认值 | 范围与用途 |
| --- | ---: | --- |
| `max_buffered_bytes_per_direction` | 1048576 | 65536～67108864 bytes，限制单方向共享缓存及逻辑计费状态 |
| `gap_timeout_ns` | 1000000000 | 0～60000000000 ns，按 capture watermark 等待缺口 |

缺口在 capture watermark 到达期限或会话终结时确认，不按墙钟超时确认；设为 0 仍需水位到达缺口的证明时间。
慢消费者、预算或分配失败会使任务明确失败，已发布字节和 Gap 不被晚到包改写。
HTTP/1 与 TLS 的分析能力受当前捕获视图约束；TLS 不解密应用数据，无法恢复加密隧道内部流量。
正常终结先排空流与模块再完成结果；失败/取消走中止路径，不伪造正常 EOF。
扩展协议模块的接口和借用生命周期见 [共享 TCP 流契约](../tasks/archive/feat-npm-shared-tcp-stream.md)。

## 性能说明

以下是 2026-10-13 的固定负载样本，本次文档整理没有重新测量。使用 Intel i9-13900H、20 逻辑 CPU、
WSL2/Linux x86_64、GCC 13.3、Release `-O3 -DNDEBUG`，固定 CPU 2，同配置旧/新版本交替至少七次独立进程，结果等价。
packet/s 表示处理的输入网络数据包数；结果 row/s 取决于模块、周期与事件数量，没有通用固定换算。

### 分族处理与结果批量化

以下选取包含 NPI 解码/输入物化、业务校验、必要 EOF 和输出释放的计时口径，不含 PCAP 文件 I/O、Scheduler HTTP
和最终 DataFrame 登记。RST 是 96 字节报文、512 包一批、200 次；ACK 是 96 字节报文、30,000 活动会话、10 次。

| 负载 | 优化前 packet/s | 优化后 packet/s |
| --- | ---: | ---: |
| IPv4 RST，Basic | 12,187 | 73,502 |
| mixed RST，Basic+Session | 5,297 | 124,145 |
| mixed ACK，Basic+Session | 42,530 | 292,790 |

分族结果批量编码摊薄高输出负载的 Arrow 构造成本。三万会话的 EOF 快照拥有期堆下降约 32%～33%，
默认容量与预算不变。持续会话、复杂协议和数据库输出具有不同成本，不能直接套用该表的速率。

### ClickHouse 托管写入

另一次独立比较使用 ClickHouse 24.3.18.7、同步写入（async_insert=0）：计时输入 512 个 mixed TCP RST 包，
启用 Basic+Session，输出 1,024 行，预热不计分子。计时包含 ProcessBlock、Flush/EOF 与输出释放，
解码和读回另计；四个实体各 256 行，仍为四次数据 INSERT。

| 指标，中位数 | URL 参数批写路径 | ArrowStream 路径 |
| --- | ---: | ---: |
| 输入 packet/s | 407.22 | 22,061.69 |
| 结果 row/s | 814.44 | 44,123.39 |
| Arrow 逻辑列数据 MB/s | 0.28 | 15.20 |
| pending 高水位 bytes | 1,044,136 | 653,352 |
| 进程 RSS 高水位 MiB | 46.14 | 43.54 |

该负载每包恰好生成两行，所以 row/s 为 packet/s 的两倍。吞吐中位数提升 54.18 倍；优化后七次范围为
27,404.44～46,144.82 row/s。MB 按 1,000,000 bytes 计算，逻辑数据排除 HTTP/IPC 协议字节，不代表网络带宽。

独立四列探针的 ArrowStream 写入中位数分别为 243,743（256 行/请求）、1,969,488（4,096 行/请求）、
4,613,412（16,384 行/请求）row/s；较窄探针不代表 NPM 完整结果吞吐，也不改变生产排空策略。
旧路径在 16,384 行批次受 URL 请求限制而失败。连接复用、TCP_NODELAY 和列式表示一起变化，未分别测量各项贡献。

这些比较用于说明实现变化在指定负载上的收益，不构成不同数据库的排名或生产容量承诺。
实际部署应根据包长、活动会话数、启用模块、周期宽度、数据库延迟与吞吐预算重新测量。
