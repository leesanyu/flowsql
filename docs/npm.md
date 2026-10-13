# NPM 网络会话与协议分析

`npm.basic` 分析 PCAP/PCAPNG 或持续采集流量，提供 Basic 周期统计、Session 性能指标以及
DNS、HTTP/1、TLS、ICMP 事件。多个模块共享解码和会话归属；IPv4、IPv6 分别输出，
可以选一个实体写入 DataFrame，也可以将所有启用实体保存到数据库。

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

来源映射使用 `source_id:observation_domain_id`，多项用分号分隔，例如 `source_domains='0:7;1:7;2:8'`。
普通 PCAP 来源编号为 0，PCAPNG 使用实际接口来源编号；显式映射必须覆盖输入的所有来源。
仅在多个来源属于同一观测范围时将它们映射到同一域。更完整的字段范围见
[参数契约](../tasks/archive/feat-npm-basic-parameters.md)。

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

## 保存所有实体并查询

SQLite、MySQL、PostgreSQL、ClickHouse 都支持两段数据库托管目标：

```sql
SELECT * FROM pcapfile.capture
USING npm.basic
WITH features='basic,session'
INTO mysql.npm
```

从任务响应取得本次 `run_id` 和实际关系名后查询。每个实体按 Schema 版本提供三种只读关系：

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
不要仅凭 latest 关系名推断它属于哪次运行。失败或取消保留已提交前缀并标记 incomplete；未知提交不自动重放。
需要基线分析时，按明确 run_id 读取 Basic history 的 interval 增量，见 [Baseliner 使用说明](baseliner.md)。

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
