# NPM 按 IPv4/IPv6 分析与存储

状态：已完成；T1～T5 全部验收完成；WIP=0。优先级：P1。
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

T1 实际冻结 Key4 为 56 字节：地址 8、观察域 8、拥有型 namespace 32、端口 4、协议 1、尾填充 3。
Key6 为 80 字节：地址 32、namespace 32、观察域 8、端口 4、协议 1、尾填充 3；地址 offset=0/16。
布局断言针对当前 64 位工具链（sizeof(std::string)=32）；借用键视图为 16 字节，临时键不能构造视图。

PacketLayerInfo 的公共分层事实与地址存储分离；地址使用族专属值/批次，端口放在公共标量区。
混合批次保留公共行序与族内索引，IPv4/IPv6 地址值分别按实际数量存放；非 IP、未提取和截断状态独立表达。
packet 的 Arrow 表达及过滤字段在 T1 冻结，并验证现有单批次入口可承载混合数据；
不得把 nullable fixed_size_binary(16) 全长列或拥有型最大类型包装当作“IPv4 不承担 IPv6 槽位”的实现。
借用视图只在明确回调/批次生命周期内有效；异步持有和结束快照必须拥有相应族的数据，不依赖已释放报文字节。

T1 已冻结 packet 表达：公共事实不内嵌 IP；拥有型 batch 保留公共行和具体族端点数组。
Arrow `ip_endpoints` 使用 dense union（0/4/6），子项为 none、UInt32 地址对、FixedSizeBinary(16) 地址对；
union 的族内 offset 与公共行一一关联。地址是否未请求、不存在、可用或不完整独立编码，零地址为合法值。
过滤逻辑字段 src/dst_ip_v4、src/dst_ip_v6 及族字段绑定到该 union，不构造全长 IPv6 中间列。
具体公共头和机器断言为契约依据，运行时迁移仍由 T2 交付。

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

T1 已冻结两族快照、回调视图及全部具体族结果；DNS/HTTP1/TLS 复用地址无关的事务事实。
12 个 Schema 固定实体与族元数据，IPv4/IPv6 地址列均带地址语义；Session 的 primary_label_id 统一可空，
标签开关不改变实体 Schema；未启用标签时为 null。结束快照拥有具体族键，临时快照不能返回借用视图。

T1 输出选择契约：省略 features 默认 basic；普通输出必须显式 observing，旧实体别名和未启用实体均拒绝。
托管模式省略 observing 为纯入库，前台返回确定的零列 Schema/零行；此时拒绝结果级 WHERE/投影。
显式 observing 及其 WHERE/投影仅影响前台；持久化实体始终为所有启用模块的两族集合。

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

T1 四后端映射为完整实体字段给出物理类型、空值规则、值域/长度约束和原始 Arrow Field，作为读回目标。
IPv4 参数使用 kUInt64 数值，IPv6 使用 kBlob/16 字节；二进制和字符串借用已验证的 Arrow buffer 至同步命令返回。
IPv6 临时值/临时 Array 不能产出借用参数；读回须匹配实体 Schema 及地址元数据，不经字符串地址适配。

### 发布进度

basic_ipv4/basic_ipv6 分别维护 epoch、committed_position、closed_before_bucket；两族使用同一已确认源进度。
相关实体的数据写入和前台输出均成功排空后，才能为相应关系发布闭合边界；无某族报文时也可依据真实源进度推进该族。
禁止用空轮询、MAX(timestamp) 或固定等待推断闭合；同一时间边界必须分别通知两族，不能被单一去重游标吞掉。
某次写入/发布失败或提交状态未知就停止推进并终结失败；保留实际已提交前缀，不承诺两张表及标记跨表原子提交。
Baseliner 使用显式选择的新族关系和该关系的发布事实；更新样例及生产集成，不重开已归档的契约安全 Feature。

T1 发布请求与拥有型确认事实包含具体实体/history 关系/run、周期边界、epoch 和关系内 committed_position。
纯契约检查要求真实源闭合事实、写入和前台排空；只有匹配请求的确认提交推进对应游标，失败/未知保留前缀并停止。
下游转换精确匹配关系/run，按兼容周期得到 closed_before_bucket；同边界双族及空族推进由断言冻结。

## 主链路

1. 混合 PCAP/实时源 → 一次安全解码 → 保留输入顺序的分族地址事实 → 分族键/共享模块算法 → 12 个具体族结果实体。
2. 结果实体 → 分族 Arrow batch → 托管消费者物理分表 → 分实体发布进度 → SQL/Web 查询或 Baseliner 读取指定关系。

## Feature Task 与验收锚点

- [x] **T1 交付端到端分族数据契约，使 IPv4 的拥有型数据不再预留 IPv6 容量并为所有消费者确定唯一表示。**
  冻结公共 struct/interface、packet 混合批次、键/视图生命周期、12 个 Schema、实体选择、四后端映射和发布契约；
  size/align/offset、地址数值/字节序、混合行序、空值及固定长度有可执行断言，完成一次契约评审。
- [x] **T2 交付分族 packet 与会话运行时，使混合流量在全局容量和生命周期保证下按具体地址宽度处理。**
  - [x] **T2.1 交付分族 packet 拥有型存储及过滤/标签接入，使一次解码后的地址正确且原始行序可还原。**
    覆盖 NPI 输出、packet codec、batch view、PCAP 过滤与标签；非 IP、截断和未提取字段不伪造地址。
  - [x] **T2.2 交付两族键和会话索引，使双向会话、ID、超时及准入在共享预算下保持正确。**
    覆盖族隔离、相同地址不同端口、同地址端点、索引句柄失效、总容量和预算归还。
  - [x] **T2.3 交付分族快照与终态处理，使周期、EOF、取消和故障均无悬空引用、漏计或重复终结。**
    混合时序、冷门族无包、结束快照持有、共享 TCP 会话关联与背压均有断言。
- [x] **T3 交付全部模块的具体族结果，使两族都可完成分析并以紧凑 Arrow 实体输出。**
  - [x] **T3.1 交付 Basic/Session 分族结果与路由，使周期统计和性能指标在具体族前台输出中可核对。**
    删除字符串地址中间对象；模块/实体选择、默认与显式配置错误、两族结果计数和预算有回归。
  - [x] **T3.2 交付 DNS/HTTP1/TLS 分族事务结果，使共享 TCP/UDP 分析在两族下输出相同业务口径。**
    更新模块目录、编码与描述；覆盖 IPv4 上 AAAA 查询、IPv6 上 A 查询及地址文本延迟格式化。
  - [x] **T3.3 交付 ICMP 分族控制结果，使 echo 和引用会话查询使用正确族且保持独立控制路径。**
    覆盖 v4/v6、引用完整/截断/非法状态及 active_quoted_session_id 的族内关联。
- [x] **T4 交付分族入库、进度与查询，使全部结果在四后端分别保存并可被前台及下游正确读取。**
  - [x] **T4.1 交付类型化分表持久化，使 12 个实体的地址无损写入并按真实对象计入预算。**
    覆盖建表/元数据、固定二进制绑定与读回、UInt32 上界、含 NUL/高位 IPv6、空引用、保留清理和失败状态。
  - [x] **T4.2 交付分族查询与发布事实，使 observing、history/latest/final 和 Baseliner 消费各自的完整结果。**
    覆盖四后端结果一致、前台选择不漏另一族入库、同边界双族发布、无包族推进及中途写入/发布故障。
    更新 SQL/配置/README、目录与 Web 地址显示、Baseliner 集成样例；不增加旧名别名或兼容视图。
- [x] **T5 交付完整链路与量化成本验收，使分族设计在真实流量、容量和存储条件下形成可交付结果。**
  标准全量构建/完整 CTest、ASan/UBSan/LSan、格式和严格编译通过；四后端与真实 .so 集成均通过。
  固定样本、CPU、批量和模块配置，分别测纯 IPv4、纯 IPv6、混合流量的实际对象/索引/快照、预算、RSS、吞吐和库占用；
  保留现有三万会话容量保证并验证总量限制，不放宽阈值掩盖成本；旧代码仅作测量对照，不作为长期支持路径。
  全部任务验收后再更新完成/归档，不能用候选 sizeof 或部分单测替代整体完成。

## 顺序与实施入口

前置 Feature 完成 → T1 → T2 → T3 → T4 → T5，WIP=1；父任务仅在其全部子任务和目标验收后勾选。
每个任务可跨多个 10～30 分钟 Atomic Slice；具体文件、CMake target 和命令在 active_task.md 冻结，不增加第三层编号。
T1～T5 已完成，完整链路、四后端及实际成本已验收；成本取舍见量化报告。
测试入口覆盖 packet/NPI、npm_basic、periodic、tcp_stream、各协议模块、result_sqlite/result_backends、过滤标签、scheduler 和 baseliner 生产链路。
所有历史完成记录只作问题与语义参考，新 Feature 使用当前新契约的执行证据。

## 完成证据

2026-10-11：完成代码路径核查、Feature 边界与五个阶段任务规划，全部实现任务未开始。
布局模型证据：`/tmp/npi-wire-layout-plan/family-storage-review/`；本轮范围/文档核验：`/tmp/npi-ip-family-feature-plan/`。
本轮未修改生产源码、CMake、测试、数据库、前台页面或已归档规格。

2026-10-12：T1 首片冻结 flowsql::network 公共分析地址、NpmSessionKey4/6 与借用 NpmSessionKeyView。
当前工具链地址 size/align=4/4、16/16、6/2；拥有型键 size/align=56/8、80/16，借用视图为 16 字节。
独立 CMake target 与 CTest 1/1、严格编译、ASan/UBSan/LSan 和格式检查通过；断言覆盖地址值域与网络字节、
0～15 起始偏移转换、容器对齐、临时键拒绝和拥有型复制生命周期。证据：`/tmp/npm-ip-family-t1-address-key/`。
本片未接入生产 packet/会话运行时；T1 的混合批次与结果/数据库/发布契约留在原任务后续切片，T1 不勾选。

2026-10-12：T1 全部契约已完成；packet 与结果切片、输出/映射/发布切片及最终验收均到达当前可验证检查点。
公共契约：analysis_address.h、ipacket_family.h、npm_ip_family_contract.h、npm_family_result_contract.h、
npm_family_output_contract.h；纯契约实现由独立 CMake targets 编译，后续 T2～T4 消费同一类型和 Schema。
混合 packet 在单个 Arrow IPC batch 中保存捕获行序及族内 offset，纯 IPv4 的 IPv6 子数组长度/地址 buffer 为零；
12 个实体均有完整字段、空值、元数据和地址编码断言，快照拥有具体族键；四后端 × 12 个实体映射全部断言通过。
实体选择覆盖默认、显式、旧名/禁用/重复错误和托管纯入库；发布覆盖同边界双族、空族真实推进、失败/未知保留前缀。
标准配置 `cmake -B build src` 与四契约 targets 的 -Wall/-Wextra/-Werror 构建通过；新契约 CTest 4/4。
重新构建相关既有 targets 后终端定向 CTest 12/12（四新契约及 wire/packet/NPI、协议/HTTP1/TLS/ICMP/periodic 契约）。
独立 /tmp 标准入口构建开启 FLOWSQL_IP_FAMILY_CONTRACT_SANITIZERS；ASan/UBSan/LSan CTest 4/4，detect_leaks=1。
沙箱 ptrace 阻断 LSan 启动后经授权在沙箱外执行通过，未关闭泄漏检测。全部新增 C++ 版权和 clang-format 检查通过。
完成一次仅面向 P0/P1 的 T1 契约审查，无阻塞项；完成证据及 SHA256/范围审计：`/tmp/npm-ip-family-t1-complete/`。
T1 勾选、工作台 WIP=0；T2～T5 未开始。当前验收为公共数据契约，生产迁移、真实四后端读写和成本验收按后续任务交付。

### T2（2026-10-12）

T2.1～T2.3 及父任务整体验收完成。NPI、PCAP、NetAdapter、packet codec 和 NPM batch view 消费同一分族契约；
混合输入按公共行序处理，具体族端点数组按实际数量拥有，输入预算包含其副本；过滤直接读取 union 逻辑字段，
标签事实仅同步借用已拥有的具体族字节。packet 预览作为既有消费者支持 union/struct 原始值读取。
Key4/Key6 两张表共用 ID、事件时间、总容量和预算；ID/deadline 索引不复制最大键，节点借用在 rehash 后稳定，
tuple reuse、FIN/RST、超时和 EOF 删除对应旧索引。运行时快照只分配实际族键，深复制独立计费，持有/复制后仍可读；
EOF/超时先完成所有快照准备，预算不足时不部分更新水位、会话和索引。
新增混合真实运行时断言覆盖共享 TCP 数据顺序、两族唯一 ID、冷门族周期、EOF、重复 Flush、取消和故障清理；
既有背压、ICMP 控制路径和三万会话四场景回归保留，既有测试函数未删除。
标准入口配置和当前全量 build 通过；新 packet/session runtime targets 以 -Wall/-Wextra/-Werror 构建通过。
当前相关回归共 46 项全部通过：沙箱内 CTest 44/44、四后端结果回归 1 项、配置 HTTP/重启/真实标签插件 1/1。
独立标准入口 ASan/UBSan/LSan CTest 7/7，detect_leaks=1，FLOWSQL_FLOW_LABELING=AUTO；直接编译的 packet/session
与完整 NPM/TCP 实现受检查，原测试中的真实插件同时加载运行，动态插件及缓存第三方库保留常规构建。
WSL 回环路由问题经临时 local TCP 规则完成数据库回归后恢复，原规则逐字节一致；系统 DPDK 宿主设备探测清理
问题经临时独立网络命名空间完成原配置集成与 Sanitizer 验收，未改变生产设备初始化/清理逻辑。
本 T2 的 68 个 C++ 文件版权/clang-format 检查及 git diff --check 通过，一次 Diff 审查无 P0/P1 阻塞项。
证据与范围/SHA256/保留文件审计：`/tmp/npm-ip-family-t2/final-audit.json`、`t2-diff-review.md` 及终端日志。
工作台 WIP=0；T3～T5 未启动，未提交或推送。结果分族、物理分表/发布/查询和量化成本仍按后续任务验收。

### T3（2026-10-12）

T3.1～T3.3 及父任务整体验收完成。Basic/Session、DNS/HTTP1/TLS 和 ICMP 均由真实模块输出具体族结果；
模块目录注册全部 12 个实体，地址直接编码为 UInt32 / FixedSizeBinary(16)，删除字符串地址中间对象及旧实体别名。
Session 标签列始终 nullable，标签开关不改变 Schema。普通前台必须显式选择具体族；托管省略 observing 为零列零行，
真实托管 factory/consumer 回归确认全部启用模块两族仍被消费，前台选择不丢弃另一族的消费者输出。
协议 retained identity 只拥有实际族键并按实际 charge 归还；混合真实 DNS/HTTP1/TLS 输入分别检查两族业务结果，
IPv4 上 AAAA 与 IPv6 上 A 查询按传输端点分族；IPv6 含 NUL/高位字节无损输出，地址格式化留展示边界。
ICMP 两族 echo 共用容量、ID 和时间轴，deadline 不复制地址键；引用完整/截断/非法及同族 active session 关联有断言，
控制路径维持独立。standalone collector 失败回滚新增 pending，保留旧结果与输出，成功重试无重复。
标准入口配置、全量构建及最新测试 target 构建通过；当前 T3 相关 NPM CTest 30/30，含完整主测试、周期和 TCP runtime。
完整主测试保留三万会话四场景、背压/失败/取消/EOF、真实 NPI/标签插件和算子 V2 集成，原测试函数未删除。
独立标准入口 ASan/UBSan/LSan CTest 7/7，detect_leaks=1；直接编译的完整 NPM 实现受检查，动态插件与第三方库保留普通构建。
沿用已核实的沙箱 ptrace/宿主 DPDK 环境隔离：经授权在沙箱外独立网络命名空间执行，未关闭泄漏检测或修改设备逻辑。
29 个生产编译单元 -Wall/-Wextra/-Werror 严格检查通过；72 个本轮 C++ 文件版权/clang-format、git diff --check 和范围审计通过。
一次 Diff 审查无 P0/P1 阻塞；证据：`/tmp/npm-ip-family-t3/normal-final-ctest2.log`、`sanitizer-final-ctest7.log`、
`strict-check-final.log`、`t3-diff-review.md`、`final-audit.json` 及构建日志。T1/T2 起点改动保留，Backlog 未改，暂存区为空。
T4 下游仅完成受接口影响的直接编译调用迁移，物理分表/查询/进度未验收；Feature 完整 CTest/四后端与成本仍属 T4/T5。
工作台 WIP=0，T4～T5 未启动；未提交或推送，未归档整个 Feature。

### T4（2026-10-12）

T4.1/T4.2 及父任务整体验收完成。全部 12 个具体族实体在 SQLite/MySQL/PostgreSQL/ClickHouse 分表保存；
IPv4 保留 UInt32 全域，IPv6 参数直接借用 Arrow 原始 16 字节，包含 NUL、高位及控制字节的地址无损读回。
SQLite/PostgreSQL 值域与长度 CHECK 建表后复读验证，不兼容已有对象拒绝；prepared 参数对象按实际大小计入预算，
保留 UInt64 全域、空引用、run 隔离、保留清理和失败/未知提交语义。数据库使用新隔离库 flowsql_npm_family_t4。
数据库服务通过既有实体目录的 canonical Schema 恢复单关系直接列/别名的精确类型、nullability 与字段/Schema metadata，
不依赖 NPM 实现；表达式保留 driver 推断类型。ClickHouse 传输按完整字节编码并处理 HTTP 参数 escaped-text，未转为地址文本落库。
basic_ipv4/basic_ipv6 独立确认游标，共享真实捕获水位；写入与 router 前台排空后分别发布，同边界两族及空族均可推进，
负水位不伪造闭合边界。写入/发布失败及确认丢失停止后续推进，保留已提交前缀，不提供跨表原子提交。
Baseliner 精确绑定具体族 history 关系/run，四后端真实 poll 覆盖空 IPv6 族独立推进。
Scheduler 的入口/executor 允许明确 observing 的结果 WHERE/投影，前台选择不缩小全部实体入库；
省略 observing 保持纯入库零列零行及结果 WHERE/投影拒绝。目录/Web 在展示边界格式化地址，Arrow 类型与字节不变；
真实预览回归覆盖同屏 IPv4/IPv6、含 NUL 的 UTF-8 字符串、浮点值和 NULL。SQL/配置/README/Baseliner 样例已同步。
标准入口配置及受影响生产/测试 targets 构建通过；当前 NPM CTest 31/31，SQLite/Framework/Builtin 3/3（SQLite 重叠），
完整 Scheduler CTest 1/1，三远程后端完整存储回归与四后端 Baseliner poll 全部通过；Web taskResult Node 测试 1/1。
独立标准入口开启 FLOWSQL_NPM_RESULT_SANITIZERS：SQLite ASan/UBSan/LSan 1/1、NPM 完整主测试/分族 runtime 2/2，
三远程后端真实读写均通过，detect_leaks=1；数据库/公共/NPM 库及直接编译 runtime 使用 Sanitizer，其他插件/第三方保持常规构建。
按已核实的环境问题在沙箱外/独立网络命名空间运行 LSan/NPM；数据库临时 local TCP 规则在 finally 恢复且逐字节一致。
10 个生产编译单元严格检查通过（仅排除既有头文件 unused-parameter/pessimizing-move/missing-field-initializers 警告）；
23 个本轮 C++ 文件版权/clang-format、git diff --check、测试函数保留及起点 SHA256/范围审计通过；一次 Diff 审查无 P0/P1 阻塞。
证据：`/tmp/npm-ip-family-t4/` 中 npm-final-ctest.log、display-storage-final.log、slice4-scheduler-final.log、
slice4-backends-domain.log、slice4-poll-dual.log、sanitizer-*-final.log、strict-final4.log、t4-diff-review.md、final-audit.json。
T1/T2/T3 及用户既有改动保留，Backlog 未改、暂存区为空、HEAD 未变；工作台 WIP=0，T5 未启动，未提交或推送。
整个 Feature 的完整 CTest、全量回归与量化成本仍由 T5 交付；本轮不归档 Feature。

### T5（2026-10-12）

T5及整个Feature整体验收完成。标准入口配置与最终全量build成功；完整CTest按环境拆分81/81与10/10，
重叠fixture去重后覆盖全部90个登记测试，0失败、0跳过；配置真实标签与Baseliner生产测试显式迁移到具体族实体。
ASan/UBSan/LSan当前24个唯一测试通过（完整NPM主测试扩展后再次通过），detect_leaks=1；
三远程后端Sanitizer真实存储回归通过。直接编译NPM及common/database/NPM库受检查，其他动态插件与缓存第三方保持常规构建。
沿用已核实的沙箱ptrace/宿主DPDK隔离：沙箱外独立netns验收，未关闭LSan或修改生产设备逻辑；
数据库临时local TCP规则在finally恢复，逐字节一致。独立原生驱动MySQL19/19、PostgreSQL7/7、ClickHouse21/21，0skip。
保留三万会话四场景，扩展pure IPv4/pure IPv6/mixed观察两族共16场景；normal/Sanitizer均通过，
族内行数、全局ID、持有期及默认pending预算断言通过；30个对象进程总量30000且30001拒绝，不放宽默认预算。
固定CPU2/512批量：对象与真实.so吞吐各30次，四后端存储24个有效组合完成；mixed存储修正全局ID等价后在新库重测。
实际活动表/索引堆相对旧HEAD中位数下降IPv4 17.35%、IPv6 9.26%、mixed 13.32%；
EOF快照实际堆增加28.54%～47.56%，IPv6/mixed峰RSS增加；未优化构建吞吐中位数下降1.48%～5.79%。
IPv4地址参数下降63.87%，部分物理库占用改善；mixed页/索引/parts及短IPv6样本可能增加占用，不宣称所有成本改善。
量化报告：[NPM分族成本验收](../../docs/npm-ip-family-cost.md)，包含全部口径、范围、预算/RSS、吞吐、页面/索引/压缩与复现条件。
59个生产/测量/主测试编译单元严格检查通过（仅排除已核实的既有公共头警告），142个Feature C++版权/clang-format通过，git diff --check通过。
一次T5 Diff审查无P0/P1阻塞。原始证据、JUnit唯一覆盖、工具链/来源/二进制SHA256及起点保留审计：
`/tmp/npm-ip-family-t5/final-audit.json`、`t5-diff-review.md`、`ctest-coverage.json`、`sanitizer-coverage.json`及各终端日志。
T1～T4与用户原有改动保留，HEAD未变、暂存区为空；规格文件归档，Backlog标记完成，工作台WIP=0；未提交或推送。
