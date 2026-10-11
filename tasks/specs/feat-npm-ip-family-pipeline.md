# NPM 按 IPv4/IPv6 分析与存储

状态：规划完成，待实施；T1～T5 均未开始。优先级：P1。
前置：[NPI 报文头布局与安全解析](../archive/feat-npi-wire-layout.md)。
依据：[NPM 基础分析](../archive/feat-npm-basic-analysis.md)、[多实体存储查询](../archive/feat-npm-result-query.md)。

## 业务意图与 Non-Goals

为用 npm.basic 分析和保存混合 IPv4/IPv6 离线或实时流量的用户，消除 IPv4 长期承担 IPv6 最大存储容量、
端口嵌套填充和字符串地址中间结果的成本；交付分族的地址、会话、分析结果与物理数据库表，
使两族能在同一任务中正确处理、分别查询，并以实际对象、预算、RSS、写入量和吞吐验证收益。

产品处于构建阶段，直接修改公共类型、接口、Arrow Schema、配置实体名和表定义。
不设计旧 ABI/Schema/表名别名、兼容视图、双写双读、历史数据迁移、旧版本并行支持或滚动升级。
验收使用新建隔离数据库/命名空间；不自动清理用户已有数据库。
不新增协议、分析指标、采集后端、通用异构多输出 SQL、跨表原子提交或完整协议栈。
不复制两套分析算法，不重写 TCP 重组、应用事务和 Baseliner 模型；不顺手做字符串驻留或通用内存池项目。

## Feature 边界与责任

NPI wire 安全可独立复用，由前置 Feature 完成。本 Feature 拥有 packet → 分族会话/控制分析 → 分族结果 → 入库/查询的一条主链路。
分析类型和落库实体必须一起验收；不把它们拆成要求过渡适配层的独立 Feature，也不按文件数或时间盒拆分。
T1 统一冻结公共数据契约，后续任务消费同一契约；T1～T4 的阶段检查点不等于本 Feature 已可交付。

| 边界 | 核查到的现状 | 本 Feature 责任 |
| --- | --- | --- |
| packet/NPI 输出 | 两个 IpAddress variant；PacketRecord/LayerInfo 固定内嵌地址 | 具体地址值、分族拥有型存储与混合输入顺序 |
| NPM 会话 | NpmEndpoint 嵌套；键进入会话/超时/准入/快照 | 分族键和索引、生命周期、方向、全局容量与预算 |
| 六类模块结果 | Basic/Session/DNS/HTTP1/TLS/ICMP 普遍使用地址字符串 | 具体族结果类型、编码、实体描述与路由 |
| 托管存储 | 四后端，整数/普通 binary 映射；缺 fixed_size_binary 映射 | 紧凑地址列、分族物理表、读回、实体元数据 |
| 周期发布 | router/consumer 写死 basic，单一发布游标 | basic_ipv4/basic_ipv6 的真实进度与失败语义 |
| 查询与显示 | observing 单 Schema，SQL 文档及生产测试绑定旧实体 | 明确族实体、地址显示、新表查询与下游集成 |

## 核心数据契约

### 地址与拥有型存储

| 分析值 | sizeof / alignof | 要求 |
| --- | --- | --- |
| IPv4Address | 4 / 4 | 四字节地址，无 IPv6 存储槽 |
| IPv6Address | 16 / 16 | 十六字节地址，保留所需的自然对齐 |
| EthernetAddress | 6 / 2 | 六字节地址，按现有整数访问需要对齐 |

分别定义 NpmSessionKey4、NpmSessionKey6；允许模板复用定义和算法，但实体内只有对应族的两个地址。
端口、协议、观察域等为键的直接字段；两个 IPv6 地址连续置于 16 字节对齐位置，端口排入标量区。
键内地址族由具体类型表达；分派视图/句柄可以有一个族标记，不给每个地址另建拥有型通用容器。
当前工具链的候选键 sizeof=56/80；T1 用实际公共类型和 offset 断言冻结目标，并解释全部新增字段成本。
不使用按值 variant<Key4,Key6>、最大尺寸 union 或 IPv4-mapped IPv6 作为会话/快照/结果批次的统一存储。

PacketLayerInfo 的公共分层事实与地址存储分离；地址使用族专属值/批次，端口放在公共标量区。
混合批次保留公共行序与族内索引，IPv4/IPv6 地址值分别按实际数量存放；非 IP、未提取和截断状态独立表达。
packet 的 Arrow 表达及过滤字段在 T1 冻结，并验证现有单批次入口可承载混合数据；
不得把 nullable fixed_size_binary(16) 全长列或拥有型最大类型包装当作“IPv4 不承担 IPv6 槽位”的实现。
借用视图只在明确回调/批次生命周期内有效；异步持有和结束快照必须拥有相应族的数据，不依赖已释放报文字节。

### 同一任务的分析保证

一个任务读取 source 一次，按捕获顺序处理；按地址族选择数据容器，共用模块算法、事件时间和任务预算。
两族会话共用唯一 session_id 分配器；相同地址字节前缀与端口不能跨族碰撞，正反向报文仍归属同一会话。
max_active_sessions、预算和背压按两族总量执行；超时索引使用有生命周期保证的轻量句柄，避免再复制最大键。
周期快照、乱序处理、FIN/RST、EOF、取消和故障覆盖两族；分批存放不能先处理全部 IPv4 再处理 IPv6 而改变事件顺序。
共享 TCP 状态继续按唯一会话关联；标签事实/匹配器及离线过滤同步消费具体族地址，不重复会话化或协议识别。
ICMP 保持独立控制路径；引用报文按其验证后的族取得具体地址，当前异族引用判 invalid 的规则不扩展。

### 结果实体与用户选择

features 仍选择 basic/session/dns/http1/tls/icmp 模块；启用模块同时支持两族，不新增两套模块配置。
所有会话端点和 ICMP 地址在拥有型结果中使用对应分析值；编码前不构建地址字符串。

| 模块 | IPv4 实体 | IPv6 实体 |
| --- | --- | --- |
| basic | basic_ipv4 | basic_ipv6 |
| session | session_ipv4 | session_ipv6 |
| dns | dns_transaction_ipv4 | dns_transaction_ipv6 |
| http1 | http1_transaction_ipv4 | http1_transaction_ipv6 |
| tls | tls_handshake_ipv4 | tls_handshake_ipv6 |
| icmp | icmp_event_ipv4 | icmp_event_ipv6 |

实体族由传输/控制报文端点确定；例如 IPv4 上查询 AAAA 的 DNS 事务仍属于 dns_transaction_ipv4。
ICMP 的可空引用地址列采用该实体对应族的类型；保持未取到引用、非法引用和只取到 IP 的状态区分。
族由实体描述/Schema 元数据表达，普通结果行不重复保存恒定 ip_family；端口为独立 UInt16 列。
普通单 Schema 前台结果显式选择 observing='basic_ipv4' 等具体实体；托管数据库模式可省略 observing，
并保存全部已启用模块的两族实体，前台选择与 WHERE 投影不作为持久化筛选条件。
不保留 basic/session 等旧结果实体别名；模块名和结果实体名继续各自承担职责。

### Arrow、数据库与查询

IPv4 地址列为 Arrow UInt32，定义为地址的无符号数值：1.2.3.4 = 0x01020304；与内存网络字节显式转换。
IPv6 地址列为 Arrow FixedSizeBinary(16)，内容为网络字节；端口为 UInt16 数值，不混入地址编码。
分别建立 npm_<entity>_v1_data 物理表及 history/latest/final 查询关系；v1 标记新实体的当前契约，不并行支持旧 Schema。
例如 npm_basic_ipv4_history_v1 与 npm_basic_ipv6_history_v1，分别查询、过滤与聚合。

| 后端 | IPv4 列 | IPv6 列 |
| --- | --- | --- |
| SQLite | INTEGER，校验 UInt32 值域 | BLOB，校验 16 字节 |
| MySQL | INT UNSIGNED | BINARY(16) |
| PostgreSQL | BIGINT，校验 UInt32 值域 | BYTEA，校验 16 字节 |
| ClickHouse | UInt32 | FixedString(16) |

PostgreSQL/SQLite 的整数物理宽度不承诺为 4 字节；实际页占用、索引、压缩和分配器成本分别测量。
补齐 fixed_size_binary 的参数绑定、DDL/元数据核验、读回和结果预算；不能经过 UTF-8、十六进制文本持久化或截断 NUL 字节。
SQL/目录/Web 结果按地址语义格式化，字符串仅在展示/导出边界生成；查询示例明确类型化地址筛选和具体族关系。
保留 run_id、实体身份、revision、累计/事件语义和真实失败状态等业务保证；它们不是旧版本兼容要求。

### 发布进度

basic_ipv4/basic_ipv6 分别维护 epoch、committed_position、closed_before_bucket；两族使用同一已确认源进度。
相关实体的数据写入和前台输出均成功排空后，才能为相应关系发布闭合边界；无某族报文时也可依据真实源进度推进该族。
禁止用空轮询、MAX(timestamp) 或固定等待推断闭合；同一时间边界必须分别通知两族，不能被单一去重游标吞掉。
某次写入/发布失败或提交状态未知就停止推进并终结失败；保留实际已提交前缀，不承诺两张表及标记跨表原子提交。
Baseliner 使用显式选择的新族关系和该关系的发布事实；更新样例及生产集成，不重开已归档的契约安全 Feature。

## 主链路

1. 混合 PCAP/实时源 → 一次安全解码 → 保留输入顺序的分族地址事实 → 分族键/共享模块算法 → 12 个具体族结果实体。
2. 结果实体 → 分族 Arrow batch → 托管消费者物理分表 → 分实体发布进度 → SQL/Web 查询或 Baseliner 读取指定关系。

## Feature Task 与验收锚点

- [ ] **T1 交付端到端分族数据契约，使 IPv4 的拥有型数据不再预留 IPv6 容量并为所有消费者确定唯一表示。**
  冻结公共 struct/interface、packet 混合批次、键/视图生命周期、12 个 Schema、实体选择、四后端映射和发布契约；
  size/align/offset、地址数值/字节序、混合行序、空值及固定长度有可执行断言，完成一次契约评审。
- [ ] **T2 交付分族 packet 与会话运行时，使混合流量在全局容量和生命周期保证下按具体地址宽度处理。**
  - [ ] **T2.1 交付分族 packet 拥有型存储及过滤/标签接入，使一次解码后的地址正确且原始行序可还原。**
    覆盖 NPI 输出、packet codec、batch view、PCAP 过滤与标签；非 IP、截断和未提取字段不伪造地址。
  - [ ] **T2.2 交付两族键和会话索引，使双向会话、ID、超时及准入在共享预算下保持正确。**
    覆盖族隔离、相同地址不同端口、同地址端点、索引句柄失效、总容量和预算归还。
  - [ ] **T2.3 交付分族快照与终态处理，使周期、EOF、取消和故障均无悬空引用、漏计或重复终结。**
    混合时序、冷门族无包、结束快照持有、共享 TCP 会话关联与背压均有断言。
- [ ] **T3 交付全部模块的具体族结果，使两族都可完成分析并以紧凑 Arrow 实体输出。**
  - [ ] **T3.1 交付 Basic/Session 分族结果与路由，使周期统计和性能指标在具体族前台输出中可核对。**
    删除字符串地址中间对象；模块/实体选择、默认与显式配置错误、两族结果计数和预算有回归。
  - [ ] **T3.2 交付 DNS/HTTP1/TLS 分族事务结果，使共享 TCP/UDP 分析在两族下输出相同业务口径。**
    更新模块目录、编码与描述；覆盖 IPv4 上 AAAA 查询、IPv6 上 A 查询及地址文本延迟格式化。
  - [ ] **T3.3 交付 ICMP 分族控制结果，使 echo 和引用会话查询使用正确族且保持独立控制路径。**
    覆盖 v4/v6、引用完整/截断/非法状态及 active_quoted_session_id 的族内关联。
- [ ] **T4 交付分族入库、进度与查询，使全部结果在四后端分别保存并可被前台及下游正确读取。**
  - [ ] **T4.1 交付类型化分表持久化，使 12 个实体的地址无损写入并按真实对象计入预算。**
    覆盖建表/元数据、固定二进制绑定与读回、UInt32 上界、含 NUL/高位 IPv6、空引用、保留清理和失败状态。
  - [ ] **T4.2 交付分族查询与发布事实，使 observing、history/latest/final 和 Baseliner 消费各自的完整结果。**
    覆盖四后端结果一致、前台选择不漏另一族入库、同边界双族发布、无包族推进及中途写入/发布故障。
    更新 SQL/配置/README、目录与 Web 地址显示、Baseliner 集成样例；不增加旧名别名或兼容视图。
- [ ] **T5 交付完整链路与量化成本验收，使分族设计在真实流量、容量和存储条件下形成可交付结果。**
  标准全量构建/完整 CTest、ASan/UBSan/LSan、格式和严格编译通过；四后端与真实 .so 集成均通过。
  固定样本、CPU、批量和模块配置，分别测纯 IPv4、纯 IPv6、混合流量的实际对象/索引/快照、预算、RSS、吞吐和库占用；
  保留现有三万会话容量保证并验证总量限制，不放宽阈值掩盖成本；旧代码仅作测量对照，不作为长期支持路径。
  全部任务验收后再更新完成/归档，不能用候选 sizeof 或部分单测替代整体完成。

## 顺序与实施入口

前置 Feature 完成 → T1 → T2 → T3 → T4 → T5，WIP=1；父任务仅在其全部子任务和目标验收后勾选。
每个任务可跨多个 10～30 分钟 Atomic Slice；具体文件、CMake target 和命令在 active_task.md 冻结，不增加第三层编号。
T1 首个切片聚焦公共地址/键/借用视图及布局测试，后续沿 T1 冻结 packet 与结果/数据库契约，不直接整仓改型。
测试入口覆盖 packet/NPI、npm_basic、periodic、tcp_stream、各协议模块、result_sqlite/result_backends、过滤标签、scheduler 和 baseliner 生产链路。
所有历史完成记录只作问题与语义参考，新 Feature 使用当前新契约的执行证据。

## 完成证据

2026-10-11：完成代码路径核查、Feature 边界与五个阶段任务规划，全部实现任务未开始。
布局模型证据：`/tmp/npi-wire-layout-plan/family-storage-review/`；本轮范围/文档核验：`/tmp/npi-ip-family-feature-plan/`。
本轮未修改生产源码、CMake、测试、数据库、前台页面或已归档规格。
