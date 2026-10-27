# NPM 网络会话与协议分析

通道发现、任务入口和固定 DNS PCAP 的完整准备与核验见 [SQL 工作流指引](sql-workflow.md#e03固定-pcap-的-basicsession-与-dns)。
本文维护 NPM 的模块、参数与结果语义；基线分析通过已发布的结果另起任务衔接。

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
- [完整分析与入库字段字典](#完整分析与入库字段字典)
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
远端探针可通过 [Kafka 原始包 source](kafka.md#原始包-packet-source) 输入同一实时主路径；源观测域及采集事实决定分析时间，重启建立新的 run 和空会话状态。

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
| `observed_at` | 结果观察/交付参考时间，具体来源依路径不同，不代替统计周期；见完整字段字典的时间口径 |

字节计数使用报文的 wire_len，单位 byte，重传仍计入观察到的流量。正常 EOF/Stop 排空尾部并输出终态；
零增量终态用于标记结束，不能当作新的学习事实。`result_mode='final'` 的周期字段为 NULL，累计字段仍有效。
Session 和协议指标用状态与 nullable 值表达证据不足，不能把 NULL 解释为零 RTT 或无重传。
A→B/B→A 是规范化方向，不自动等于客户端/服务端。

## Session 会话性能指标

启用 `features='basic,session'` 并选择 `observing='session_ipv4'` 或 `session_ipv6`，可以在同一次
分析中查看流量统计与连接性能。Session 使用算子共享核心提供的会话身份、方向和生命周期，切换周期不会重置
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
| `selected_alpn` | TLS 1.2 明文 ServerHello 中可见的选择，当前保存为单个 JSON 字符串文本（含引号）；TLS 1.3 为 NULL |
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

## 完整分析与入库字段字典

下列字典覆盖当前六个结果模块的全部分析列，并按实际 Schema 顺序列出。每个模块分别输出 IPv4、IPv6
实体，Schema 版本均为 1；同模块两族字段名、nullable 和业务含义一致，地址类型不同。
托管数据库消费者原样保存这些分析列，不受 observing、前台投影或结果过滤影响。

| 模块 | 实体名（两族） | 分析列数/实体 | 物理表列数/实体 | 公开关系列数/实体 | 结果粒度 |
| --- | --- | ---: | ---: | ---: | --- |
| basic | basic_ipv4 / basic_ipv6 | 31 | 33 | 34 | 会话累计快照 + 周期增量 |
| session | session_ipv4 / session_ipv6 | 49 | 51 | 52 | 会话累计性能快照 |
| http1 | http1_transaction_ipv4 / http1_transaction_ipv6 | 22 | 24 | 25 | 一次 HTTP/1 事务终态 |
| tls | tls_handshake_ipv4 / tls_handshake_ipv6 | 25 | 27 | 28 | 一个初始 TLS 候选握手终态 |
| dns | dns_transaction_ipv4 / dns_transaction_ipv6 | 25 | 27 | 28 | 一次 UDP/TCP DNS 事务终态 |
| icmp | icmp_event_ipv4 / icmp_event_ipv6 | 27 | 29 | 30 | 一次 Echo 关联或错误事件 |

列数包含身份、时间、维度、状态和数值指标，不能把全部列都当成数值指标。六类 Schema 单族合计 179 个
列定义，两族合计 358 个；它们分布在 12 个实体中，不是一张 358 列的宽表。
物理表比分析结果多 2 列，公开 history/latest/final 关系多 3 列，详见[存储附加字段](#存储附加字段与类型映射)。

字典类型为 Arrow 类型：UInt8/16/32/64 为无符号整数，Int64 为有符号整数，Float64 为双精度，
Boolean 为布尔值，Utf8 为字符串。`IP` 在 IPv4 实体是 UInt32，在 IPv6 实体是 FixedSizeBinary(16)。
IPv4 使用无符号地址数值，IPv6 保存网络序 16 字节；nullable 为“是”仅表示允许 NULL，具体填值还须看状态。
绝对时间用 epoch 纳秒，时差用 ns，流量用 byte，速率用 bit/s，次数用对应的包、事务或样本单位。

A/B 按地址的网络序字节排序，同地址时按端口排序；方向 0=A→B，1=B→A。客户端/服务端由
`tcp_initiator`、请求方向或 `client_direction` 判断。Basic/Session 为 cumulative，revision 随交付递增；
DNS/HTTP1/TLS/ICMP 为 event，固定 revision=1、is_final=true。

`observed_at` 的来源依路径不同：离线 Basic 周期结果在交付时取系统墙钟；实时快照沿用时间通知的墙钟，
包触发的结果、协议正常事件和离线结束路径也会使用报文/消息采集时间或任务当前观察参考时间；
协议期限结果可使用采集水位。统一解释为结果参考时间，不能跨模块将其一律当作包时间或数据库写入时间。
分析报文时使用 first/last、请求/响应时间，分析 Basic 周期时使用 period_start/end。

### Basic 输出与共享核心的依赖

`npm.basic` 是组合分析算子的名称，`features` 中的 `basic` 是可选的基础统计结果模块。
各分析模块读取共享核心的报文、会话视图及时间/生命周期通知，不读取 Basic 的结果表作为分析输入。
例如 `features='session'`、`features='http1,labeling'`、`features='tls,labeling'` 和
`features='icmp'` 都可以在未启用 Basic 结果输出时使用相应分析能力；协议模块仍需满足各自配置与准入条件。

| 消费模块或链路 | 实际依赖 | Basic 输出字段变更的影响 |
| --- | --- | --- |
| Session | 共享会话视图、方向、识别结果及生命周期通知；独立构建性能结果 | 不读取 Basic 的 is_final、end_reason 或 interval_*；自身终态字段独立生成 |
| HTTP/1、TLS | 标签准入、共享会话及 TCP 字节流 | 不读取 Basic 指标；仍需要核心和共享流提供连接关闭/EOF 通知以排空未完成事务 |
| DNS | UDP 输入、共享会话；TCP 输入复用共享字节流及标签准入 | 不读取 Basic 指标；事务超时与会话/流收尾继续由现有输入链路驱动 |
| ICMP | 独立控制报文输入，可选关联共享会话 | 不读取 Basic 指标，也不要求启用 Session 性能模块 |
| 统一结果路由与入库 | 实体 Schema、身份、revision、observed_at 及当前强制的 is_final 映射 | 删除 Basic 的 is_final 必须同步调整公共校验和终态关系，其他实体的终态校验仍须保留 |
| Baseliner 的 npm_period_increment | 周期边界、周期增量；读取 is_final 和方向包数增量跳过仅标记终结的记录 | 删除 interval_* 会改变其输入契约，不能直接用累计量替换增量指标 |

当前 5 个 `interval_*` 字段记录周期包数/字节数增量，有独立于会话终结的统计用途。
例如同一会话两次快照的 `wire_bytes_total` 为 100、250，对应周期增量为 100、150；
累计值相加为 350，会重复统计首个周期的 100 字节。只保留累计值时，下游须按同一 run/session 的连续
快照计算差分，并明确首条快照、缺失前序快照与查询时间范围的处理，再用于周期流量汇总或基线学习。
3 个 `period_*` 字段表示采集时间统计窗口及窗口完整性，不判断会话是否终结。

会话关闭、超时、五元组复用与 EOF 的内部处理还负责共享状态回收和协议事务收尾。
Basic 是否保存会话终态字段不改变这些核心职责；Session 的会话终态结果通过独立模块输出。
以下字段字典描述当前实现，尚未实施字段删除。

源码依据：[模块目录与输入适配](../src/operators/npm_basic/core/npm_module_catalog.cpp)、
[共享核心与模块创建](../src/operators/npm_basic/core/npm_basic_task_runtime.cpp)、
[Session 独立结果构建](../src/operators/npm_basic/modules/session/npm_session_analysis_module.cpp)、
[公共实体校验](../src/operators/npm_basic/npm_protocol_contract.cpp)、
[结果入库](../src/operators/npm_basic/npm_basic_result_consumer.cpp)、
[Baseliner 周期增量消费](../src/operators/baseliner/bucket_aggregator.cpp)。

### basic 分析字段

实体：`basic_ipv4` / `basic_ipv6`，每族 31 列。

| 字段 | Arrow 类型 | 可为 NULL | 含义、单位与有效条件 |
| --- | --- | --- | --- |
| `session_id` | `UInt64` | 否 | 身份：任务内唯一会话实例 ID；复用同一五元组的新连接使用新 ID；跨运行须结合 run_id。 |
| `observation_domain_id` | `UInt64` | 否 | 维度：由 source_domains 解析的观测域 ID；不等同输入 source_id。 |
| `revision` | `UInt64` | 否 | 版本：同一实体实例内从 1 递增的结果版本；各实体独立管理，Basic 与 Session 的 revision 不保证对齐。 |
| `observed_at` | `Int64` | 否 | 时间：本条结果的观察/交付参考时间，单位 ns；来源见字典开头的时间口径，不能代替报文时间或统计周期。 |
| `is_final` | `Boolean` | 否 | 生命周期：是否为该实例终态；不代表协议成功或运行完整。 |
| `transport_protocol` | `UInt8` | 否 | 维度：IP 传输协议号，TCP=6、UDP=17；区别于 NPI 应用协议 ID。 |
| `a_ip` | `IP` | 否 | 维度：规范化端点 A 的 IP；不是固定的客户端地址。 |
| `b_ip` | `IP` | 否 | 维度：规范化端点 B 的 IP；不是固定的服务端地址。 |
| `a_port` | `UInt16` | 否 | 维度：规范化端点 A 的 TCP/UDP 端口。 |
| `b_port` | `UInt16` | 否 | 维度：规范化端点 B 的 TCP/UDP 端口。 |
| `first_ns` | `Int64` | 否 | 时间：当前累计范围内最早观察到的报文采集时间，单位 ns。 |
| `last_ns` | `Int64` | 否 | 时间：当前累计范围内最晚观察到的报文采集时间，单位 ns。 |
| `packets_ab` | `UInt64` | 否 | 指标：A→B 累计观察包数，单位 packet；包括观察到的重传包。 |
| `packets_ba` | `UInt64` | 否 | 指标：B→A 累计观察包数，单位 packet；包括观察到的重传包。 |
| `wire_bytes_ab` | `UInt64` | 否 | 指标：A→B 累计 wire_len，单位 byte；包括重传，不是 cap_len 或应用正文长度。 |
| `wire_bytes_ba` | `UInt64` | 否 | 指标：B→A 累计 wire_len，单位 byte；包括重传，不是 cap_len 或应用正文长度。 |
| `primary_label_id` | `UInt32` | 是 | 维度：主标签 ID；关闭 labeling 为 NULL，启用但未命中为 0，命中为非零 ID。 |
| `protocol_status` | `Utf8` | 否 | 状态：NPI 识别状态 pending / identified / unknown；终态不会为 pending。 |
| `protocol_id` | `UInt16` | 是 | 维度：NPI 应用协议 ID；仅 identified 时填写。 |
| `protocol_sub_id` | `UInt16` | 是 | 维度：NPI 子协议 ID；未识别时为 NULL，识别后仍可没有子协议。 |
| `protocol` | `Utf8` | 是 | 维度：由当前 NPI 词典解析的协议名称；仅 identified 时填写。 |
| `end_reason` | `Utf8` | 是 | 状态：终态为 closed / idle_timeout / tuple_reuse / eof；活动快照为 NULL。 |
| `wire_bytes_total` | `UInt64` | 否 | 派生指标：wire_bytes_ab + wire_bytes_ba，单位 byte。 |
| `period_start_ns` | `Int64` | 是 | 时间：UTC epoch 对齐周期的名义左边界，单位 ns；final 模式为 NULL。 |
| `period_end_ns` | `Int64` | 是 | 时间：周期的名义右边界，单位 ns；final 模式为 NULL。 |
| `period_complete` | `Boolean` | 是 | 状态：是否由安全时间水位确认覆盖到周期右边界；尾部不足一周期为 false；final 模式为 NULL。 |
| `interval_packets_ab` | `UInt64` | 是 | 指标：本次交付的 A→B 周期包数增量，单位 packet；final 模式为 NULL。 |
| `interval_packets_ba` | `UInt64` | 是 | 指标：本次交付的 B→A 周期包数增量，单位 packet；final 模式为 NULL。 |
| `interval_wire_bytes_ab` | `UInt64` | 是 | 指标：本次交付的 A→B 周期 wire_len 增量，单位 byte；final 模式为 NULL。 |
| `interval_wire_bytes_ba` | `UInt64` | 是 | 指标：本次交付的 B→A 周期 wire_len 增量，单位 byte；final 模式为 NULL。 |
| `interval_wire_bytes_total` | `UInt64` | 是 | 派生指标：两方向 interval_wire_bytes 之和，单位 byte；final 模式为 NULL。 |

累计字段截至已交付周期边界；周期增量可以累加，累计值不能跨 revision 求和。正常结束可能追加同一
周期的零增量终态记录，终态不重复交付已统计流量。8 个 period/interval 字段在 final 模式全部为 NULL。

### session 分析字段

实体：`session_ipv4` / `session_ipv6`，每族 49 列。

| 字段 | Arrow 类型 | 可为 NULL | 含义、单位与有效条件 |
| --- | --- | --- | --- |
| `session_id` | `UInt64` | 否 | 身份：任务内唯一会话实例 ID；复用同一五元组的新连接使用新 ID；跨运行须结合 run_id。 |
| `observation_domain_id` | `UInt64` | 否 | 维度：由 source_domains 解析的观测域 ID；不等同输入 source_id。 |
| `revision` | `UInt64` | 否 | 版本：同一实体实例内从 1 递增的结果版本；各实体独立管理，Basic 与 Session 的 revision 不保证对齐。 |
| `observed_at` | `Int64` | 否 | 时间：本条结果的观察/交付参考时间，单位 ns；来源见字典开头的时间口径，不能代替报文时间或统计周期。 |
| `is_final` | `Boolean` | 否 | 生命周期：是否为该实例终态；不代表协议成功或运行完整。 |
| `transport_protocol` | `UInt8` | 否 | 维度：IP 传输协议号，TCP=6、UDP=17；区别于 NPI 应用协议 ID。 |
| `a_ip` | `IP` | 否 | 维度：规范化端点 A 的 IP；不是固定的客户端地址。 |
| `b_ip` | `IP` | 否 | 维度：规范化端点 B 的 IP；不是固定的服务端地址。 |
| `a_port` | `UInt16` | 否 | 维度：规范化端点 A 的 TCP/UDP 端口。 |
| `b_port` | `UInt16` | 否 | 维度：规范化端点 B 的 TCP/UDP 端口。 |
| `first_ns` | `Int64` | 否 | 时间：当前累计范围内最早观察到的报文采集时间，单位 ns。 |
| `last_ns` | `Int64` | 否 | 时间：当前累计范围内最晚观察到的报文采集时间，单位 ns。 |
| `duration_ns` | `Int64` | 否 | 派生指标：last_ns - first_ns，单位 ns；是已观察跨度，不是业务请求或完整连接持续时间。 |
| `primary_label_id` | `UInt32` | 是 | 维度：主标签 ID；关闭 labeling 为 NULL，启用但未命中为 0，命中为非零 ID。 |
| `protocol_status` | `Utf8` | 否 | 状态：NPI 识别状态 pending / identified / unknown；终态不会为 pending。 |
| `protocol_id` | `UInt16` | 是 | 维度：NPI 应用协议 ID；仅 identified 时填写。 |
| `protocol_sub_id` | `UInt16` | 是 | 维度：NPI 子协议 ID；未识别时为 NULL，识别后仍可没有子协议。 |
| `protocol` | `Utf8` | 是 | 维度：由当前 NPI 词典解析的协议名称；仅 identified 时填写。 |
| `end_reason` | `Utf8` | 是 | 状态：终态为 closed / idle_timeout / tuple_reuse / eof；活动快照为 NULL。 |
| `packets_ab` | `UInt64` | 否 | 指标：A→B 累计观察包数，单位 packet；包括观察到的重传包。 |
| `packets_ba` | `UInt64` | 否 | 指标：B→A 累计观察包数，单位 packet；包括观察到的重传包。 |
| `wire_bytes_ab` | `UInt64` | 否 | 指标：A→B 累计 wire_len，单位 byte；包括重传，不是 cap_len 或应用正文长度。 |
| `wire_bytes_ba` | `UInt64` | 否 | 指标：B→A 累计 wire_len，单位 byte；包括重传，不是 cap_len 或应用正文长度。 |
| `payload_bytes_ab` | `UInt64` | 否 | 指标：A→B 累计传输层声明 payload 字节数，单位 byte；包括重复序列及未完全捕获的声明负载。 |
| `payload_bytes_ba` | `UInt64` | 否 | 指标：B→A 累计传输层声明 payload 字节数，单位 byte；包括重复序列及未完全捕获的声明负载。 |
| `rate_status` | `Utf8` | 否 | 状态：valid / insufficient_span；duration_ns=0 时为 insufficient_span，速率全部为 NULL。 |
| `wire_bps_ab` | `Float64` | 是 | 派生指标：A→B wire_bytes × 8 × 10^9 / duration_ns，单位 bit/s；仅 rate_status=valid。 |
| `wire_bps_ba` | `Float64` | 是 | 派生指标：B→A wire_bytes × 8 × 10^9 / duration_ns，单位 bit/s；仅 rate_status=valid。 |
| `payload_bps_ab` | `Float64` | 是 | 派生指标：A→B payload_bytes × 8 × 10^9 / duration_ns，单位 bit/s；仅 rate_status=valid。 |
| `payload_bps_ba` | `Float64` | 是 | 派生指标：B→A payload_bytes × 8 × 10^9 / duration_ns，单位 bit/s；仅 rate_status=valid。 |
| `tcp_unique_payload_bytes_ab` | `UInt64` | 是 | 指标：A→B 按 TCP 序列区间去重的累计 payload，单位 byte；UDP 或序列歧义时为 NULL。 |
| `tcp_unique_payload_bytes_ba` | `UInt64` | 是 | 指标：B→A 按 TCP 序列区间去重的累计 payload，单位 byte；UDP 或序列歧义时为 NULL。 |
| `tcp_unique_payload_bps_ab` | `Float64` | 是 | 派生指标：A→B unique_payload_bytes × 8 × 10^9 / duration_ns，单位 bit/s；需正时长和可判定序列。 |
| `tcp_unique_payload_bps_ba` | `Float64` | 是 | 派生指标：B→A unique_payload_bytes × 8 × 10^9 / duration_ns，单位 bit/s；需正时长和可判定序列。 |
| `tcp_handshake_status` | `Utf8` | 否 | 状态：complete / partial / not_observed / ambiguous / not_applicable。 |
| `tcp_initiator` | `Utf8` | 是 | 维度：a / b，表示可识别的 SYN 发起端；complete/partial 时有值，UDP 或无可靠证据时为 NULL。 |
| `tcp_handshake_duration_ns` | `Int64` | 是 | 指标：初始 SYN 到完成三次握手的 ACK 的观察时差，单位 ns；仅 complete 时填写。 |
| `tcp_synack_rtt_ns` | `Int64` | 是 | 指标：SYN 到匹配 SYN-ACK 的观察时差，单位 ns；complete 或有足够证据的 partial 时填写。 |
| `tcp_rtt_status` | `Utf8` | 否 | 状态：valid / no_sample / ambiguous / not_applicable；不是单独的网络健康判断。 |
| `tcp_rtt_samples` | `UInt64` | 是 | 指标：有效 TCP 发送/累计 ACK 时差样本数；no_sample 时为 0，ambiguous/UDP 时为 NULL。 |
| `tcp_rtt_min_ns` | `Int64` | 是 | 指标：有效 RTT 样本最小值，单位 ns；仅 tcp_rtt_status=valid 时填写。 |
| `tcp_rtt_mean_ns` | `Int64` | 是 | 指标：有效 RTT 样本平均值，整数 ns；仅 valid 时填写，按样本和/样本数截取整数。 |
| `tcp_rtt_max_ns` | `Int64` | 是 | 指标：有效 RTT 样本最大值，单位 ns；仅 valid 时填写。 |
| `tcp_retransmission_status` | `Utf8` | 否 | 状态：valid / ambiguous / not_applicable；valid 的零重传与无法判定的 NULL 分别表达。 |
| `tcp_retrans_packets_ab` | `UInt64` | 是 | 指标：A→B 观察到 payload 序列重叠的包数，单位 packet；仅重传状态 valid；不等同网络丢包数。 |
| `tcp_retrans_packets_ba` | `UInt64` | 是 | 指标：B→A 观察到 payload 序列重叠的包数，单位 packet；仅重传状态 valid；不等同网络丢包数。 |
| `tcp_retrans_payload_bytes_ab` | `UInt64` | 是 | 指标：A→B 观察到重复 payload 序列的字节数，单位 byte；仅重传状态 valid。 |
| `tcp_retrans_payload_bytes_ba` | `UInt64` | 是 | 指标：B→A 观察到重复 payload 序列的字节数，单位 byte；仅重传状态 valid。 |
| `measurement_flags` | `UInt32` | 否 | 状态位：1=中途开始，2=payload 截断，4=时间戳倒退，8=序列歧义，16=SYN 重传；可组合。 |

Session 与 Basic 共用会话，Session 的采样与 Basic 采集时间周期边界并非同一机制，不能用相同 revision
直接拼接两者；需同时明确 run、session 和时间口径。Session 的 6 个 bps 字段均为会话累计平均速率。
UDP 的所有 TCP 专属数值/发起端字段为 NULL，TCP 状态为 not_applicable；序列歧义时去重、重传和 RTT
相关数值不可用。RTT 为当前捕获点同时可见的发送与累计 ACK 观察时差，当前 Schema 没有 RTT 分位数或方向列。

### http1 分析字段

实体：`http1_transaction_ipv4` / `http1_transaction_ipv6`，每族 22 列。

| 字段 | Arrow 类型 | 可为 NULL | 含义、单位与有效条件 |
| --- | --- | --- | --- |
| `entity_instance_id` | `UInt64` | 否 | 身份：独立事务/事件 ID；同一会话可有多个事务，跨模块或运行不能仅凭该值关联。 |
| `revision` | `UInt64` | 否 | 版本：独立事件固定为 1。 |
| `observed_at` | `Int64` | 否 | 时间：本条结果的观察/交付参考时间，单位 ns；来源见字典开头的时间口径，不能代替报文时间或统计周期。 |
| `is_final` | `Boolean` | 否 | 生命周期：独立事件固定为 true；须另看 outcome 判断证据。 |
| `session_id` | `UInt64` | 否 | 身份：任务内唯一会话实例 ID；复用同一五元组的新连接使用新 ID；跨运行须结合 run_id。 |
| `observation_domain_id` | `UInt64` | 否 | 维度：由 source_domains 解析的观测域 ID；不等同输入 source_id。 |
| `a_ip` | `IP` | 否 | 维度：规范化端点 A 的 IP；不是固定的客户端地址。 |
| `b_ip` | `IP` | 否 | 维度：规范化端点 B 的 IP；不是固定的服务端地址。 |
| `a_port` | `UInt16` | 否 | 维度：规范化端点 A 的 TCP/UDP 端口。 |
| `b_port` | `UInt16` | 否 | 维度：规范化端点 B 的 TCP/UDP 端口。 |
| `outcome` | `Utf8` | 否 | 状态：matched / request_only / response_only；matched 不代表 HTTP 状态码成功。 |
| `informational_count` | `UInt32` | 否 | 指标：事务关联中观察到的非终结 1xx 响应次数；101 作为终结响应，不计入此值。 |
| `request_direction` | `UInt8` | 是 | 维度：请求方向，0=A→B、1=B→A；response_only 时为 NULL。 |
| `response_direction` | `UInt8` | 是 | 维度：响应方向，0=A→B、1=B→A；request_only 时为 NULL。 |
| `method` | `Utf8` | 是 | 维度：请求方法，如 GET/POST；没有请求时为 NULL。 |
| `target` | `Utf8` | 是 | 维度：请求行中的 request-target，可能含查询参数；没有请求时为 NULL。 |
| `host` | `Utf8` | 是 | 维度：可见 Host 头；没有请求或没有 Host 时为 NULL。 |
| `status_code` | `UInt16` | 是 | 维度：终结 HTTP 响应状态码，包括 101 或 2xx～5xx；没有响应时为 NULL。 |
| `request_headers_at_ns` | `Int64` | 是 | 时间：完整请求头对应的采集时间，单位 ns；缺少请求或可靠时间时为 NULL。 |
| `response_headers_at_ns` | `Int64` | 是 | 时间：完整终结响应头对应的采集时间，单位 ns；缺少响应或可靠时间时为 NULL。 |
| `latency_ns` | `Int64` | 是 | 派生指标：response_headers_at_ns - request_headers_at_ns，单位 ns；仅配对且时间可靠/非倒退时填写；不是正文传输时长。 |
| `incomplete_reason` | `Utf8` | 是 | 状态：matched 为 NULL；单边结果填写未配对/期限/缺口/定界/会话结束等原因，具体取值见下方。 |

HTTP/1 只处理明文 TCP。请求/响应按流水线顺序关联，matched 需满足支持范围内的消息定界完成；
latency 仍按头部完成时间差计算。当前不入库 HTTP 版本、正文、正文长度、Content-Length 或完整头集合。

`incomplete_reason` 取值为 no_pending_request、response_not_observed_by_deadline、pipeline_alignment_lost、
request_path_gap、response_path_gap、capture_truncation、framing_unsupported、header_limit_exceeded、
malformed_response、response_stream_end、session_end；matched 时为 NULL。

### tls 分析字段

实体：`tls_handshake_ipv4` / `tls_handshake_ipv6`，每族 25 列。

| 字段 | Arrow 类型 | 可为 NULL | 含义、单位与有效条件 |
| --- | --- | --- | --- |
| `entity_instance_id` | `UInt64` | 否 | 身份：当前实现直接使用 session_id；每个基础会话最多一条候选握手终态，仍按独立事件实体保存。 |
| `revision` | `UInt64` | 否 | 版本：独立事件固定为 1。 |
| `observed_at` | `Int64` | 否 | 时间：本条结果的观察/交付参考时间，单位 ns；来源见字典开头的时间口径，不能代替报文时间或统计周期。 |
| `is_final` | `Boolean` | 否 | 生命周期：独立事件固定为 true；须另看 outcome 判断证据。 |
| `session_id` | `UInt64` | 否 | 身份：任务内唯一会话实例 ID；复用同一五元组的新连接使用新 ID；跨运行须结合 run_id。 |
| `observation_domain_id` | `UInt64` | 否 | 维度：由 source_domains 解析的观测域 ID；不等同输入 source_id。 |
| `a_ip` | `IP` | 否 | 维度：规范化端点 A 的 IP；不是固定的客户端地址。 |
| `b_ip` | `IP` | 否 | 维度：规范化端点 B 的 IP；不是固定的服务端地址。 |
| `a_port` | `UInt16` | 否 | 维度：规范化端点 A 的 TCP/UDP 端口。 |
| `b_port` | `UInt16` | 否 | 维度：规范化端点 B 的 TCP/UDP 端口。 |
| `outcome` | `Utf8` | 否 | 状态：server_hello_observed / fatal_alert_observed / incomplete；不输出完整 TLS 握手成功结论。 |
| `hello_retry_count` | `UInt8` | 否 | 指标：HelloRetryRequest 次数，0 或 1；不是 TCP 重传次数。 |
| `incomplete_reason` | `Utf8` | 是 | 状态：观察停止或不完整原因；看见 ServerHello 后仍可非空，fatal_alert_observed 时为 NULL。 |
| `client_direction` | `UInt8` | 否 | 维度：ClientHello 方向，0=A→B、1=B→A；候选建立后必填。 |
| `client_hello_at_ns` | `Int64` | 是 | 时间：第一次完整 ClientHello 的采集时间，单位 ns；候选尚不完整或缺少可靠时间为 NULL。 |
| `server_hello_at_ns` | `Int64` | 是 | 时间：最终完整 ServerHello 的采集时间，单位 ns；缺少该消息或可靠时间为 NULL。 |
| `server_hello_latency_ns` | `Int64` | 是 | 派生指标：最终 ServerHello 与第一次完整 ClientHello 的观察时差，单位 ns；包含 HelloRetryRequest 往返，不是完整握手时间。 |
| `client_sni` | `Utf8` | 是 | 维度：首次完整 ClientHello 中可见的外层 SNI；无 SNI/未完整解析为 NULL，不推断 ECH 内部名称。 |
| `client_alpn_protocols` | `Utf8` | 是 | 维度：首次 ClientHello 的 ALPN 列表，保存为 JSON 字符串数组文本，如 ["h2","http/1.1"]；无列表为 NULL。 |
| `client_offered_tls13` | `Boolean` | 是 | 维度：首次完整 ClientHello 的 supported_versions 是否包含 0x0304；未完整解析为 NULL，完整但未包含为 false。 |
| `selected_version` | `UInt16` | 是 | 维度：最终 ServerHello 选定版本，TLS1.2=0x0303、TLS1.3=0x0304；缺少 ServerHello 为 NULL。 |
| `cipher_suite` | `UInt16` | 是 | 维度：最终 ServerHello 的密码套件编号；缺少该消息为 NULL。 |
| `selected_alpn` | `Utf8` | 是 | 维度：TLS1.2 明文 ServerHello 中可见的选定 ALPN；当前编码是含引号的单个 JSON 字符串文本，如 "h2"；TLS1.3 为 NULL。 |
| `alert_level` | `UInt8` | 是 | 维度：仅实际观察到明文 fatal alert 时为 2；其他结果为 NULL。 |
| `alert_description` | `UInt8` | 是 | 维度：实际观察到的明文 fatal alert 描述码；其他结果为 NULL。 |

TLS 只分析 TCP 上可见的初始 TLS1.2/1.3 候选。server_hello_observed 证明看见最终 ServerHello，
fatal_alert_observed 证明实际看见明文 fatal alert；两者都不代表完成证书/Finished 验证。
client_alpn_protocols 和 selected_alpn 是 Utf8 中的 JSON 文本，后者当前包含 JSON 字符串引号。
当前不入库证书、JA3/JA4、完整 offered_versions/密码套件列表或应用数据。

`incomplete_reason` 取值为 encrypted_after_server_hello、encrypted_after_change_cipher_spec、
deadline_after_server_hello、session_end_after_server_hello、client_hello_incomplete、
server_hello_not_observed_by_deadline、hello_retry_request_unfinished、client_path_gap、server_path_gap、
capture_truncation、malformed_record、malformed_hello、hello_limit_exceeded、unsupported_version、
unsupported_framing、server_stream_end、session_end。fatal_alert_observed 时为 NULL；已见 ServerHello 的
结果仍用该列表达观察边界。

### dns 分析字段

实体：`dns_transaction_ipv4` / `dns_transaction_ipv6`，每族 25 列。

| 字段 | Arrow 类型 | 可为 NULL | 含义、单位与有效条件 |
| --- | --- | --- | --- |
| `entity_instance_id` | `UInt64` | 否 | 身份：独立事务/事件 ID；同一会话可有多个事务，跨模块或运行不能仅凭该值关联。 |
| `revision` | `UInt64` | 否 | 版本：独立事件固定为 1。 |
| `observed_at` | `Int64` | 否 | 时间：本条结果的观察/交付参考时间，单位 ns；来源见字典开头的时间口径，不能代替报文时间或统计周期。 |
| `is_final` | `Boolean` | 否 | 生命周期：独立事件固定为 true；须另看 outcome 判断证据。 |
| `session_id` | `UInt64` | 否 | 身份：任务内唯一会话实例 ID；复用同一五元组的新连接使用新 ID；跨运行须结合 run_id。 |
| `observation_domain_id` | `UInt64` | 否 | 维度：由 source_domains 解析的观测域 ID；不等同输入 source_id。 |
| `transport_protocol` | `UInt8` | 否 | 维度：IP 传输协议号，TCP=6、UDP=17；区别于 NPI 应用协议 ID。 |
| `a_ip` | `IP` | 否 | 维度：规范化端点 A 的 IP；不是固定的客户端地址。 |
| `b_ip` | `IP` | 否 | 维度：规范化端点 B 的 IP；不是固定的服务端地址。 |
| `a_port` | `UInt16` | 否 | 维度：规范化端点 A 的 TCP/UDP 端口。 |
| `b_port` | `UInt16` | 否 | 维度：规范化端点 B 的 TCP/UDP 端口。 |
| `dns_id` | `UInt16` | 否 | 维度：DNS 消息的 16 位 ID；可重复，不能代替 entity_instance_id。 |
| `outcome` | `Utf8` | 否 | 状态：matched / query_only / response_only / truncated_response / unsupported_message。 |
| `query_retries` | `UInt32` | 否 | 指标：待关联期间相同问题键的重复查询次数，不含第一次；不是 TCP 重传次数。 |
| `query_direction` | `UInt8` | 是 | 维度：查询方向，0=A→B、1=B→A；只观察到响应时为 NULL。 |
| `response_direction` | `UInt8` | 是 | 维度：响应方向，0=A→B、1=B→A；只观察到查询时为 NULL。 |
| `qname` | `Utf8` | 是 | 维度：单问题 QNAME 展示文本；特殊字节转义，根域为点号；内部关联键另做 ASCII 大小写折叠。 |
| `qtype` | `UInt16` | 是 | 维度：DNS 问题类型编号，如 A=1、AAAA=28；未提供可解析问题时为 NULL。 |
| `qclass` | `UInt16` | 是 | 维度：DNS 问题 class 编号，如 IN=1；未提供可解析问题时为 NULL。 |
| `query_at_ns` | `Int64` | 是 | 时间：首次查询完整消息的采集时间，单位 ns；UDP 使用报文时间，TCP 使用消息完成时间；缺少可靠时间为 NULL。 |
| `response_at_ns` | `Int64` | 是 | 时间：响应完整消息的采集时间，单位 ns；缺少响应或可靠时间为 NULL。 |
| `latency_ns` | `Int64` | 是 | 派生指标：response_at_ns - query_at_ns，单位 ns；需关联成功且时间可靠、非倒退。 |
| `response_rcode` | `UInt16` | 是 | 维度：可解析响应的 RCODE，包含 EDNS 扩展码；0=NOERROR，3=NXDOMAIN；查询单边结果为 NULL。 |
| `response_tc` | `Boolean` | 是 | 状态：DNS 响应 TC 位；不是采集截断标志。 |
| `incomplete_reason` | `Utf8` | 是 | 状态：matched 为 NULL；单边、TC 或不支持消息填写具体原因，见下方。 |

DNS 事务关联键包含会话、方向、dns_id、opcode、大小写折叠的 wire QNAME、qtype 和 qclass；
内部键及原始 wire QNAME 不直接入库。仅支持当前单问题普通查询范围，不输出 answer IP、TTL、
answer/authority/additional 数量或完整资源记录。matched 仅表示匹配到响应，RCODE 可为非零。
truncated_response 表示配对响应 TC=1，仍可有 latency；采集截断则是 capture_truncation。

`incomplete_reason` 当前包括 response_not_observed_by_deadline、response_path_gap、capture_truncation、
malformed_response、session_end、no_pending_query、dns_tc_set、unsupported_opcode_or_transfer；matched 时为 NULL。

### icmp 分析字段

实体：`icmp_event_ipv4` / `icmp_event_ipv6`，每族 27 列。

| 字段 | Arrow 类型 | 可为 NULL | 含义、单位与有效条件 |
| --- | --- | --- | --- |
| `entity_instance_id` | `UInt64` | 否 | 身份：任务内独立控制事件 ID；不依赖 TCP/UDP 会话，跨模块或运行不能仅凭该值关联。 |
| `revision` | `UInt64` | 否 | 版本：独立事件固定为 1。 |
| `observed_at` | `Int64` | 否 | 时间：本条结果的观察/交付参考时间，单位 ns；来源见字典开头的时间口径，不能代替报文时间或统计周期。 |
| `is_final` | `Boolean` | 否 | 生命周期：独立事件固定为 true；须另看 outcome 判断证据。 |
| `observation_domain_id` | `UInt64` | 否 | 维度：由 source_domains 解析的观测域 ID；不等同输入 source_id。 |
| `src_ip` | `IP` | 否 | 维度：Echo 配对/请求单边为请求源，响应单边为响应源，错误事件为外层报文源；不使用 A/B 规范化。 |
| `dst_ip` | `IP` | 否 | 维度：对应事件的目的地址；Echo 配对为请求目的，响应单边/错误事件为当时外层报文目的。 |
| `outcome` | `Utf8` | 否 | 状态：echo_matched / echo_request_only / echo_reply_only / icmp_error。 |
| `icmp_type` | `UInt8` | 否 | 维度：ICMP/ICMPv6 type；Echo 配对结果使用请求 type=8/128，响应单边使用响应 type=0/129。 |
| `icmp_code` | `UInt8` | 否 | 维度：ICMP/ICMPv6 code；结合 IP 族和 type 解释；Echo 为 0。 |
| `outer_truncated` | `Boolean` | 否 | 状态：外层 ICMP body 是否采集不完整；与引用报文是否能解析分别判断。 |
| `incomplete_reason` | `Utf8` | 是 | 状态：Echo 单边结果填写截断/缺少请求/期限/EOF 原因；echo_matched 和 icmp_error 为 NULL。 |
| `echo_id` | `UInt16` | 是 | 维度：Echo identifier；非 Echo 错误事件为 NULL。 |
| `echo_sequence` | `UInt16` | 是 | 维度：Echo sequence；非 Echo 错误事件为 NULL。 |
| `echo_retries` | `UInt32` | 是 | 指标：匹配键相同的重复请求次数，不含第一次；匹配/请求单边可填写，响应单边及错误事件为 NULL。 |
| `request_at_ns` | `Int64` | 是 | 时间：第一次 Echo 请求的采集时间，单位 ns；响应单边或错误事件为 NULL。 |
| `reply_at_ns` | `Int64` | 是 | 时间：Echo 回复的采集时间，单位 ns；请求单边或错误事件为 NULL。 |
| `latency_ns` | `Int64` | 是 | 派生指标：reply_at_ns - request_at_ns，单位 ns；仅配对且时间差可用时填写。 |
| `quote_status` | `Utf8` | 是 | 状态：错误报文的引用解析程度 unavailable / invalid / ip_only / tuple；Echo 为 NULL。 |
| `quoted_ip_family` | `UInt8` | 是 | 维度：可解析引用报文的 IP 族，4 或 6；当前只接受与外层相同族的引用，无法解析为 NULL。 |
| `quoted_protocol` | `UInt8` | 是 | 维度：引用报文 IP 头中的协议号；仅有可解析 IP 头即可填写，不要求找到会话。 |
| `quoted_src_ip` | `IP` | 是 | 维度：引用原始报文的源 IP；不是错误报文自身的源 IP；没有可解析引用 IP 头为 NULL。 |
| `quoted_dst_ip` | `IP` | 是 | 维度：引用原始报文的目的 IP；没有可解析引用 IP 头为 NULL。 |
| `quoted_src_port` | `UInt16` | 是 | 维度：引用 TCP/UDP 报文的源端口；引用字节及头部条件满足时填写。 |
| `quoted_dst_port` | `UInt16` | 是 | 维度：引用 TCP/UDP 报文的目的端口；与源端口成对填写。 |
| `active_quoted_session_id` | `UInt64` | 是 | 关联：当前同 namespace/观测域/IP 族中仍活动的被引用 TCP/UDP 会话 ID；可为 NULL，不创建或延长会话。 |
| `next_hop_mtu` | `UInt32` | 是 | 指标：IPv4 type=3/code=4 或 ICMPv6 type=2 中的 MTU，单位 byte；其他事件为 NULL。 |

ICMP 不强制建立 TCP/UDP 会话，也没有 session_id、端口、主标签或 NPI 字段；可选关联仅由
active_quoted_session_id 表达。Echo 配对按观测域、请求 src/dst、echo_id、echo_sequence 关联。
当前错误类型范围为 IPv4 type=3/11/12、ICMPv6 type=1～4；不为全部 ICMP type 输出分析事件。
引用 IPv6 头后的扩展头没有在此路径展开，quoted_protocol 是可见 IP 头字段，端口解析受该限制。

`incomplete_reason` 当前取值为 capture_truncation、no_pending_request、response_not_observed_by_deadline、
task_eof；配对/错误事件为 NULL。quote_status=tuple 只表示引用五元组可解析，不保证当前活动会话存在。

### 存储附加字段与类型映射

每个实体使用物理表 `npm_<entity>_v1_data`，例如 `npm_basic_ipv4_v1_data`；物理表保存全部分析列
和下列前两项系统列，公开查询关系再通过运行目录补充 run_status。

| 字段 | 所在位置 | 含义 |
| --- | --- | --- |
| `__npm_run_id` | 物理表、公开关系；字符串，必填 | 单次运行 ID，隔离同 task_id 的重复运行 |
| `__npm_task_id` | 物理表、公开关系；字符串，必填 | 调度任务 ID |
| `__npm_run_status` | 公开关系；字符串 | 从 npm_result_runs.status 关联得到，非每个分析行物理复制 |

SQLite/PostgreSQL 的 run/task 系统列为 TEXT，MySQL 为 VARCHAR(128)，ClickHouse 为 String。
物理行的逻辑键是 `(run_id, entity identity, revision)`；实体身份为 Basic/Session 的 session_id 或
协议事件的 entity_instance_id。SQLite/MySQL/PostgreSQL 用主键约束，ClickHouse MergeTree 用对应排序键。
公开关系名为 `npm_<entity>_history_v1`、`npm_<entity>_latest_v1`、`npm_<entity>_final_v1`，
三者读取同一物理结果；latest 取每次运行内各实例最大 revision，final 取 is_final=true。

| 分析 Arrow 类型 | SQLite | MySQL | PostgreSQL | ClickHouse |
| --- | --- | --- | --- | --- |
| UInt8 | INTEGER | TINYINT UNSIGNED | SMALLINT | UInt8 |
| UInt16 | INTEGER | SMALLINT UNSIGNED | INTEGER | UInt16 |
| UInt32（含 IPv4 地址） | INTEGER | INT UNSIGNED | BIGINT | UInt32 |
| UInt64 | TEXT（十进制无损保存） | BIGINT UNSIGNED | NUMERIC(20,0) | UInt64 |
| Int64（时间/时差） | INTEGER | BIGINT | BIGINT | Int64 |
| Float64（速率） | REAL | DOUBLE | DOUBLE PRECISION | Float64 |
| Boolean | INTEGER（0/1） | TINYINT(1) | BOOLEAN | Bool |
| Utf8（名称/状态/JSON 文本） | TEXT | TEXT | TEXT | String |
| FixedSizeBinary(16)（IPv6 地址） | BLOB | BINARY(16) | BYTEA | FixedString(16) |

ClickHouse 可空列使用 Nullable(T)；其他数据库按分析 Schema 设置 NULL/NOT NULL。
IPv4 地址按 UInt32 存储，IPv6 为原始 16 字节；不能根据 Web 展示文本推断物理类型。
SQLite 的 UInt64 列为 TEXT，在直接 SQL 中作求和或数值比较时要考虑文本及完整无符号范围，
不能一律 CAST AS INTEGER 而宣称对所有 UInt64 值无损。

另外有三类管理数据，与报文分析列分开保存：

| 管理表 | 完整字段集合 | 用途 |
| --- | --- | --- |
| npm_result_runs | run_id、task_id、input_namespace、status、started_at_ns、completed_at_ns、expires_at_ns、error_code、error_stage、error_message、metadata_status、purge_cursor | 运行身份、状态、错误、保留与回收进度 |
| npm_result_entities | entity_id、schema_version、module_id、revision_semantics、identity_column、revision_column、observed_at_column、is_final_column、schema_canonical、schema_fingerprint、data_table、history_relation、latest_relation、final_relation | Schema 契约与实体/表/关系映射 |
| npm_result_progress_v1 | run_id、entity_id、relation_name、epoch、position、period_ns、closed_before_ns | 已提交 Basic 周期结果的发布进度，供持续查询/基线消费使用 |

input_namespace 保存于运行目录而非每个分析行；实体 ID、Schema 版本和 IP 族主要由表名、实体目录及
Schema metadata 表达，当前分析结果没有统一 ip_family 列，ICMP 的 quoted_ip_family 是引用报文信息。
SQL 响应中的 rows_written、entities 等汇总值也不是报文分析列；labeling/shared TCP stream 是共享能力，
没有第七张独立指标 Schema。

### 字段选择时需要保留的区别

| 当前字段关系 | 对查询和后续字段选择的意义 |
| --- | --- |
| Basic 与 Session 有 22 个同名同类型字段 | 包含身份、端点、流量、NPI、标签和生命周期；两者时间/版本口径不同，不应按 revision 直接等值关联 |
| Basic 有 wire_bytes_total、interval_wire_bytes_total | 可从两方向相加得到，是显式存储的派生指标 |
| Session 有 duration_ns 和 6 个 bps 字段 | 可从时间及字节量计算，但需保留 rate_status、序列有效性与 NULL 语义 |
| DNS/HTTP1/ICMP 的 latency_ns、TLS 的 server_hello_latency_ns | 以不同消息边界计算，不能无条件统一成“完整事务耗时” |
| 协议事件的 revision=1、is_final=true | 业务上是常量，统一 history/latest/final 和实体契约仍使用这两列 |
| HTTP1/TLS 不存 transport_protocol | 模块限定 TCP；Basic/Session/DNS 同时支持 TCP/UDP，需显式区分 |
| NPI protocol_id/sub_id 和 protocol 名称 | 编号与名称有关联，名称取决于词典；不能仅看重复而忽略查询解释需要 |
| outcome/status/reason 为 Utf8 | 是枚举含义的字符串；存储类型压缩应同时定义枚举值和查询展示契约 |
| 协议事务不存 primary_label_id | 当前只能通过同 run 的 Basic/Session 会话结果补充标签，若未启用这两模块，事务表自身不提供标签 ID |
| ICMP 的 quoted_ip_family 只接受同族引用 | 族值与实体有关联，但 NULL 仍表达引用信息不可得 |
| observed_at 跨路径的时间来源不同 | 不能拿该列替代所有模块的采集时间；指标比较应选各自明确的消息/周期时间列 |

字段契约入口为[分族结果 Schema](../src/operators/npm_basic/npm_family_result_contract.cpp)，
具体填值及有效性由[Basic 编码](../src/operators/npm_basic/output/npm_basic_result_encoder.cpp)、
[Session 编码](../src/operators/npm_basic/output/npm_session_result_encoder.cpp)和
[dns](../src/operators/npm_basic/modules/dns/npm_dns_result_encoder.cpp)、
[http1](../src/operators/npm_basic/modules/http1/npm_http1_result_encoder.cpp)、
[tls](../src/operators/npm_basic/modules/tls/npm_tls_result_encoder.cpp)、
[icmp](../src/operators/npm_basic/modules/icmp/npm_icmp_result_encoder.cpp)编码路径确定；
物理类型及系统列见[数据库消费者](../src/operators/npm_basic/npm_basic_result_consumer.cpp)。

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
