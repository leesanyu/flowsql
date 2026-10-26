# FlowSQL 产品需求池

本文件只记录 Feature 级目标，不记录实施拆分、迭代安排或验收细节。每个 Feature 的契约、非目标、主链路和原子任务分别放在对应规格文件中。

状态约定：`[ ]` 待办（含已立项、未实施），`[-]` 进行中，`[x]` 已完成。

## 目录

- [FlowSQL 产品需求池](#flowsql-产品需求池)
  - [目录](#目录)
  - [待办 Feature](#待办-feature)
  - [已完成能力](#已完成能力)
    - [框架、插件与服务](#框架插件与服务)
    - [任务、通道与 Web 管理](#任务通道与-web-管理)
    - [数据探索与可视化](#数据探索与可视化)
    - [流量采集与报文处理](#流量采集与报文处理)
    - [NPM 分析与结果管理](#npm-分析与结果管理)
    - [基线分析与预测](#基线分析与预测)
    - [Kafka 接入与发布](#kafka-接入与发布)
  - [长远规划](#长远规划)

---

## 待办 Feature

暂无当前已立项的待办 Feature。

---

## 已完成能力

按交付能力分类查阅，已完成 Feature 的规格归档至 `tasks/archive/`。

### 框架、插件与服务

| 状态 | Feature | 优先级 | 目标 | 归档规格 |
| --- | --- | --- | --- | --- |
| [x] | C++ 框架核心 (`framework-core`) | P0 | 提供插件式进程框架、统一数据接口和批处理 Pipeline。 | [归档](archive/feat-framework-core.md) |
| [x] | 产品版本与构建信息 (`product-version`) | P1 | 让构建、交付和故障排查人员能够准确识别正在使用的产品版本与源码身份，统一 Web、命令行和交付产物的版本展示与查询，并在发布前核对版本、源码与打包产物一致，避免旧产物被误标为新版本。 | [归档](archive/feat-product-version.md) |
| [x] | 路由与服务对等化 (`routing-services`) | P1 | 统一插件路由、错误映射和跨进程服务调用边界。 | [归档](archive/feat-routing-services.md) |
| [x] | 通道与算子目录 (`operator-catalog`) | P1 | 建立通道、算子元信息和激活状态的统一目录与唯一真相。 | [归档](archive/feat-operator-catalog.md) |
| [x] | C++ 算子插件 (`cpp-operators`) | P1 | 支持 C++ 算子插件独立编译、动态激活、去激活和安全卸载。 | [归档](archive/feat-cpp-operators.md) |
| [x] | C++ 算子源码目录归位 (`operator-source-layout`) | P1 | 建立独立的 C++ 业务算子源码根目录，将 `npm_basic` 与通用能力插件分离，同时保持内置算子和运行时契约不变。 | [归档](archive/feat-operator-source-layout.md) |
| [x] | 数据库平台 (`database-platform`) | P0 | 提供数据库插件、驱动、连接池、多数据库读写和 SQL 过滤闭环。 | [归档](archive/feat-database-platform.md) |
| [x] | 测试环境可重复执行与预防约束 (`test-environment`) | P1 | 面向本地开发、WSL/Docker 及后续任务的数据库与 Kafka 回归，预防连接/认证配置分散、依赖准备遗漏、网络路径不确定和环境失败误计通过；交付统一运行时配置、数据库/Kafka 及必要身份服务的隔离夹具、执行前真实连接认证与依赖预检、一致的测试入口和新测试接入规则，使新终端及重复执行无需手工拼接配置，所需依赖不可用时有界失败并准确归因，完整验收可证明全部必需用例实际执行，并区分 Kafka 元数据验证与实际读写权限。 | [归档](archive/feat-test-environment.md) |

### 任务、通道与 Web 管理

| 状态 | Feature | 优先级 | 目标 | 归档规格 |
| --- | --- | --- | --- | --- |
| [x] | DataFrame 启动恢复优化 (`dataframe-lazy-loading`) | P1 | 让持久化 DataFrame 数量多、数据量大的部署避免在进程启动和目录查询时全量读取 CSV；交付快速可用的服务与通道目录、首次访问时按需加载的数据，以及一致的并发读取、探索快照和通道管理行为。 | [归档](archive/feat-dataframe-lazy-loading.md) |
| [x] | Python 算子与 Web 管理 (`python-web`) | P0 | 打通 C++ 与 Python 算子桥接，并提供算子、任务和通道的 Web 管理入口。 | [归档](archive/feat-python-web.md) |
| [x] | Web 控制台 (`web-console`) | P1 | 提供可操作的任务、通道和系统状态管理界面。 | [归档](archive/feat-web-console.md) |
| [x] | Pipeline 与异步任务 (`pipeline-async`) | P1 | 支持多算子编排、异步执行、取消、超时和结构化诊断。 | [归档](archive/feat-pipeline-async.md) |
| [x] | 流式运行时 (`stream-runtime`) | P2 | 提供流式通道、流式算子、共享 source 和 Group DAG 执行能力。 | [归档](archive/feat-stream-runtime.md) |
| [x] | FlowSQL 阶段化过滤管线 (`stage-filter-pipeline`) | P0 | 支持 source 与每级 operator 输出的类型化 WHERE，以 Arrow 统一执行并允许 channel/operator 做精确谓词下推。 | [归档](archive/feat-stage-filter-pipeline.md) |
| [x] | 流式算子时间驱动 (`stream-time-drive`) | P0 | 让实时有状态算子在暂时无包、持续繁忙及输出背压期间仍能获得版本化时间通知并按期维护；正常 EOF 单次终结且错误/取消不伪装成 EOF，使 `npm.basic` 可按最早截止时间驱动同一任务内全部已启用模块而不受当前观察结果影响。 | [归档](archive/feat-stream-time-drive.md) |
| [x] | 配置资源通道 (`config-channel`) | P1 | 让需要跨任务复用静态规则、映射或字典的用户和算子不再绑定各自的本地配置目录；交付 JSON/YAML/XML 文件上传、具名配置通道、永久保存、重启恢复和不可变递增版本，消费者可用 `config.<name>@<revision>` 精确取得带格式、业务 Schema 与内容摘要的只读快照；不把配置通道作为普通 SQL 数据面或密钥管理系统。 | [归档](archive/feat-config-channel.md) |

### 数据探索与可视化

| 状态 | Feature | 优先级 | 目标 | 归档规格 |
| --- | --- | --- | --- | --- |
| [x] | 基础探索与 Web 入口（SQLite，`visualization-database`） | P1 | 让需要探索 SQLite 数据表的用户，从现有 Web 顶栏“数据探索”进入独立、轻量的 Graphic Walker 工作区，使用过滤、分组和基础聚合观察柱状、折线及散点图；SQLite 原生 SQL 计算，查询有界且可取消，工作区也可直接访问并独立运行。 | [规格](archive/feat-visualization-database.md) |
| [x] | 多数据库探索兼容 (`visualization-database-connectors`) | P1 | 让 MySQL、PostgreSQL、ClickHouse 用户在同一工作区直接探索已有数据库表，获得与 SQLite 基础探索一致且可解释的核心统计能力；依赖 visualization-database，查询使用各源库原生 SQL，并保持类型、只读访问、取消及资源回收保证。 | [规格](archive/feat-visualization-database-connectors.md) |
| [x] | 数据库探索体验优化 (`visualization-experience`) | P1 | 让交互探索数据库表的用户获得充分利用页面空间的图表、按表保留的独立绘图工作页和一致的编辑入口，并能绘制常见 Decimal、识别近似精度及恢复查询错误；依赖已有基础探索与多数据库兼容能力。 | [规格](archive/feat-visualization-experience.md) |
| [x] | 图表选择与字段语义优化 (`visualization-chart-authoring`) | P1 | 让数据库探索用户在默认界面直接选择全部非地图图表，按图表需要选择字段，并仅用一个数值字段绘制由源库计算的直方图。 | [规格](archive/feat-visualization-chart-authoring.md) |
| [x] | 时间统计探索 (`visualization-statistics`) | P1 | 让需要按时间比较趋势及统计不同值数量的数据库用户，在同一工作区使用日/月时间桶与去重计数；依赖已有探索与图表选择能力，四库分别执行原生 SQL，空值及时间语义明确；直方图与数值分箱由 visualization-chart-authoring 交付。 | [归档](archive/feat-visualization-statistics.md) |
| [x] | DataFrame 探索可视化 (`visualization-dataframe`) | P1 | 让需要探索 FlowSQL 中间结果或算子结果的用户，在同一工作区对命名 DataFrame 的不可变快照观察趋势、分布与变量关系，主动刷新数据版本，且已取得的数据不会因生产任务结束而悬空；复用已有探索工作区与计算契约，首版通过同机 Arrow IPC 接入并独立计算。 | [归档](archive/feat-visualization-dataframe.md) |

### 流量采集与报文处理

| 状态 | Feature | 优先级 | 目标 | 归档规格 |
| --- | --- | --- | --- | --- |
| [x] | NPI 报文头布局与安全解析 (`npi-wire-layout`) | P1 | 为通过 NPI 和 NPM 直接读取网络报文的调用方，消除任意偏移下的未对齐访问及固定字段裸偏移造成的可读性下降；交付符合协议布局的具名报文头、明确地址值转换和经真实报文验证的分层/识别结果。 | [归档](archive/feat-npi-wire-layout.md) |
| [x] | NPM 数据包契约与解析基础 (`npm-packet-contract`) | P0 | 定义统一的 packet 数据实体、采集元数据、Arrow Schema 及截断/畸形报文语义，并复用现有 NPI 分层与协议识别能力。 | [归档](archive/feat-npm-packet-contract.md) |
| [x] | NPM 离线数据包全量导入 (`npm-offline-import`) | P0 | 提供 pcap/pcapng 文件的有限流读取与回放，保留原始时间戳、捕获长度、线速长度和报文顺序，接入现有流批任务运行时。 | [归档](archive/feat-npm-offline-import.md) |
| [x] | NPM 离线文件上传与通道管理 (`npm-offline-import-web`) | P0 | 支持用户在 Web 页面上传 pcap/pcapng 文件并原子创建可执行的 `pcapfile` source 通道。 | [归档](archive/feat-npm-offline-import-web.md) |
| [x] | NPM 数据包 DataFrame 查看 (`npm-packet-dataframe-view`) | P0 | 支持将有限 `pcapfile` 数据包原样写入命名 DataFrame，并通过 Web 安全查看截断的 hex 报文内容。 | [归档](archive/feat-npm-packet-dataframe-view.md) |
| [x] | NPM 离线导入过滤 (`npm-offline-filter`) | P0 | 支持离线文件按带时区纳秒时间、MAC、IP、端口及默认双向 TCP/UDP endpoint pair 过滤，以类型化不可变规则实现任务隔离的 pcapfile 精确下推；不执行协议或应用识别。 | [归档](archive/feat-npm-offline-filter.md) |
| [x] | PCAP 文件通道持久化 (`pcapfile-channel-persistence`) | P0 | 将 `pcapfile` 通道的规范化配置持久化到 Scheduler 侧 SQLite，并在服务重启后按原名称恢复可重复执行的离线 source。 | [归档](archive/feat-pcapfile-channel-persistence.md) |
| [x] | 原生 PCAP 存储生命周期一致性 (`native-pcap-storage-consistency`) | P0 | 让原生受管 PCAP 文件与持久数据库共享运行目录生命周期，消除临时目录清理导致的启动恢复失败。 | [归档](archive/feat-native-pcap-storage-consistency.md) |
| [x] | NPM 实时采集通道抽象 (`npm-capture-contract`) | P1 | 让实时 NPM 的后端开发者和任务运行时能按同一规则交换 packet 与采集事实；交付源生命周期、观测域/队列身份、批次限额、保守时间进度、空闲/积压、缓冲归还及丢包/背压统计契约，使不同后端可独立接入并被统一验收。 | [归档](archive/feat-npm-capture-contract.md) |
| [x] | NetAdapter 常规网卡采集通道 (`npm-linux-capture-backends`) | P1 | 让 Linux 镜像口、网卡和虚拟网卡上的 NPM 用户通过Web通道管理创建、编辑、查看和删除具名 netadapter 通道，并采集同一观测域中的多张网卡，在创建配置中选择 AF_PACKET、PF_RING Classic 或 AF_XDP copy/generic-SKB；共享有界采集运行逻辑，交付完整声明范围的 packet 流、保守进度、过滤及逐输入丢包/吞吐诊断，生产 SQL 无需暴露采集技术名称；不包含 native zero-copy、DPDK 或跨观测域合并。 | [归档](archive/feat-npm-linux-capture-backends.md) |

### NPM 分析与结果管理

| 状态 | Feature | 优先级 | 目标 | 归档规格 |
| --- | --- | --- | --- | --- |
| [x] | NPM 基础分析与模块组合 (`npm-basic-analysis`) | P0 | 以同一 `npm.basic` 任务内引擎统一会话、有限采样识别和模块组合；交付离线生产 provider，并以模拟时间/采集事实验证实时周期快照、有界内存、过载和性能契约。 | [归档](archive/feat-npm-basic-analysis.md) |
| [x] | NPM 基础分析算子插件生命周期 (`npm-basic-operator-plugin-lifecycle`) | P0 | 让 `npm.basic` 作为按功能命名的 C++ 算子，通过统一多算子插件 ABI 完成上传、激活、任务租约、去激活和重启恢复。 | [归档](archive/feat-npm-basic-operator-plugin-lifecycle.md) |
| [x] | NPM TCP/UDP 会话性能分析 (`npm-session-analysis`) | P1 | 让流量分析用户在同一 `npm.basic` 任务内按 `features` 启用 Session 模块、以独立的 `observing` 选择是否前台查看类型化会话性能结果；模块复用唯一会话、方向、协议识别结果、生命周期与预算，提供带有效性依据的连接、传输时延、重传和吞吐指标，不重复会话化或把序列缺口直接宣称为网络丢包。 | [归档](archive/feat-npm-session-analysis.md) |
| [x] | NPM 通用参数入口 (`npm-basic-parameters`) | P1 | 让同一 `npm.basic` 任务既需要共享配置又组合多个模块的用户，不再为每类能力增加专用顶层 `WITH` 参数或承受配置归属冲突；交付单一 `parameters` JSON 字符串入口，以 `core` 承载流量标签化引用等任务共享配置、以扁平模块 ID 承载私有配置，严格校验信封、共享字段及已启用模块，自动忽略未启用、未加载或未知模块节点，并在任务打开时冻结拥有型配置；保留旧 Basic/Session SQL 兼容，不交付具体配置内容的业务解释。 | [归档](archive/feat-npm-basic-parameters.md) |
| [x] | 流量标签化 (`flow-labeling`) | P1 | 依赖 `config-channel` 与 `npm-basic-parameters`；让网络性能、安全等分析场景中需要按 MAC、VLAN、IP/CIDR、传输协议和端口确定会话流主归属的用户，不再由各消费模块重复匹配规则或面对多结果冲突；交付通用同进程能力插件 `libflowsql_flow_labeling.so`，当前由显式启用 `labeling` 的 `npm.basic` 任务通过固定 IID 绑定精确配置快照和任务私有 DPDK ACL matcher，为每个双向会话流产生零或一个唯一具名主标签；插件统一拥有进程级 EAL 与 DPDK 依赖，未部署插件或未启用标签化不影响其他任务，启用但 provider/运行环境不可用则在任务打开前明确失败。 | [归档](archive/feat-flow-labeling.md) |
| [x] | NPM Basic 与 Flow Labeling 可运维性及代码结构收敛 (`npm-flow-labeling-refinement`) | P1 | 依赖已交付的 `npm.basic` 与 Flow Labeling；让部署运维者和后续模块开发者不再混淆构建/启动开关、进程 EAL 内存与任务 matcher 预算，也不必在平铺源码中寻找模块；交付受限的插件启动 option、8～256 MiB 连续整数的可解释任务预算、按职责归位的 `npm_basic` 目录和 README 可达的部署/配置/性能文档，保持原有任务与分类结果不变。 | [归档](archive/feat-npm-flow-labeling-refinement.md) |
| [x] | NPM 协议模块运行时基础 (`npm-protocol-analysis`) | P1 | 为在同一 `npm.basic` 任务内扩展协议能力的模块开发者解决输入接入、事务维护与异构结果处理重复的问题；复用已有唯一解码、会话、NPI、`features`/`observing` 和任务预算，补齐模块输入与共享能力消费声明、packet/datagram/session 及独立控制事件分发、公共主标签传递、独立于会话的结果实体身份与事务时间推进、类型化结果消费者接入，使各模块按统一生命周期有界运行；依赖已交付的 `npm-basic-analysis`、`npm-session-analysis`、`npm-basic-parameters` 与 `stream-time-drive`，仅使用标签准入的模块要求 `flow-labeling`；不交付 TCP 重组、具体协议解析器或结果存储，协议关联与完成判定归各模块。 | [归档](archive/feat-npm-protocol-analysis.md) |
| [x] | NPM 多实体结果存储与查询 (`npm-result-query`) | P1 | 让需要留存和回查离线分析及后续持续分析结果的 NPM 用户，能够保存全部已启用结果模块的输出并正确查询同一实体的多个版本；交付不受 `observing` 影响的统一命名空间持久化，覆盖已有 Basic/Session 与后续协议结果，按任务运行实例、实体身份和 Schema 版本区分累计快照与独立事务，支持最新值、最终态、历史查询和保留策略，并明确有界待写与写入失败语义；依赖 `npm-protocol-analysis` 的公共结果与消费者契约，具体协议模块无需各自实现存储；生产实时场景另需 `npm-basic-realtime-integration` 与相应采集能力。 | [规格](archive/feat-npm-result-query.md) |
| [x] | NPM 共享有界 TCP 字节流 (`npm-shared-tcp-stream`) | P1 | 为同一 `npm.basic` 任务内的多个 TCP 应用协议模块解决各自处理 sequence、重传、重叠、乱序与缺口造成的重复状态和结果分歧；交付仅为命中模块所选早期唯一主标签的会话按方向共享、按需创建且任务预算有界的 TCP 有序字节视图，统一多消费者读取与缓存回收，明确连续区间、缺口、截断及方向/会话终结事实；依赖 `npm-protocol-analysis` 的消费者与生命周期契约、`npm-basic-parameters` 和 `flow-labeling`，复用唯一会话核心且不要求启用 Session 性能模块；不默认重组全部 TCP，不模拟完整 TCP 端点，消息定界与事务关联归协议模块。 | [规格](archive/feat-npm-shared-tcp-stream.md) |
| [x] | NPM DNS 事务分析 (`npm-dns-analysis`) | P1 | 为排查 DNS 响应异常的用户提供可见 UDP/TCP DNS 消息的请求/响应关联、观测时延、响应状态及明确的不完整原因；在同一 `npm.basic` 任务内保留 UDP datagram 边界、复用共享 TCP 字节流，以独立事务身份和超时维护有界状态，输出可独立观察并由统一结果存储消费的类型化结果；依赖 `npm-protocol-analysis` 与 `npm-shared-tcp-stream`，TCP 路径经共享流使用标签准入，持久化与查询由 `npm-result-query` 提供；不重复基础分析，不包含 DoT/DoH 解密与解析。 | [归档](archive/feat-npm-dns-analysis.md) |
| [x] | NPM HTTP/1 事务分析 (`npm-http1-analysis`) | P1 | 为排查明文 HTTP/1 服务响应异常的用户提供请求/响应关联、响应状态、观测时延及明确的不完整原因；在同一 `npm.basic` 任务内基于共享 TCP 字节流交付明确支持边界的消息定界、长连接事务身份与超时维护，输出可独立观察并由统一结果存储消费的类型化结果；依赖 `npm-protocol-analysis` 与 `npm-shared-tcp-stream`，经共享流使用标签准入，持久化与查询由 `npm-result-query` 提供；不解密 HTTPS、不包含 HTTP/2 或 HTTP/3，不存储无限正文或重复基础分析。 | [规格](archive/feat-npm-http1-analysis.md) |
| [x] | NPM TLS 握手分析 (`npm-tls-handshake-analysis`) | P1 | 为排查 TCP 上 TLS 连接建立异常的用户提供与 TLS 版本可见范围一致的握手元数据、有观测依据的失败事实及明确的不完整原因，避免将未观察到后续消息直接判为握手失败；在同一 `npm.basic` 任务内基于共享 TCP 字节流维护有界握手状态，输出可独立观察并由统一结果存储消费的类型化结果；依赖 `npm-protocol-analysis` 与 `npm-shared-tcp-stream`，经共享流使用标签准入，持久化与查询由 `npm-result-query` 提供；不解密握手或应用数据，不包含 QUIC 或重复基础分析。 | [归档](archive/feat-npm-tls-handshake-analysis.md) |
| [x] | NPM ICMP 控制消息分析 (`npm-icmp-analysis`) | P1 | 为排查网络连通性异常的用户提供 ICMP/ICMPv6 回显关联、观测时延、错误消息及明确的不完整原因；在同一 `npm.basic` 任务内以独立控制消息身份和有界关联状态处理，允许被引用的 TCP/UDP 会话关联缺失，输出可独立观察并由统一结果存储消费的类型化结果；依赖 `npm-protocol-analysis` 的独立 packet/control-event 路径，持久化与查询由 `npm-result-query` 提供；不依赖共享 TCP 流或会话标签准入，不把 ICMP 强行归入端口会话。 | [归档](archive/feat-npm-icmp-analysis.md) |
| [x] | NPM 基础分析生产实时接线 (`npm-basic-realtime-integration`) | P0 | 让生产实时 SQL 用户在无包、繁忙和背压时仍能取得 `npm.basic` 的周期结果与正确终结状态；依赖基础分析、时间驱动和采集契约，交付同一任务独占 reader 的采集事实与版本化时间通知接线，验证 EOF、取消和任务隔离；不实现采集后端、结果持久化或分析算法。 | [归档](archive/feat-npm-basic-realtime-integration.md) |
| [x] | NPM TCP/UDP 会话周期统计 (`npm-basic-periodic-stats`) | P1 | 让分析 PCAP 与实时网卡流量的用户不必等长会话结束才查看统计；交付两类来源共用的可配置周期记录，包含每周期双向包数/字节增量与会话累计总量，以稳定会话身份串联时序，按采集时间划分周期并在正常结束时收尾，结果可经统一存储查询；不按周期重建会话或改变协议事务语义。 | [归档](archive/feat-npm-basic-periodic-stats.md) |
| [x] | NPM 运行时检视修复 (`npm-runtime-review-fixes`) | P1 | 为离线批处理和实时终态分析修复已复现的超时顺序、输出内存放大、周期空扫描与标签预算未归还问题，使目标流超时结果稳定、默认预算可交付三万会话结果且资源账目随生命周期归零。 | [归档](archive/feat-npm-runtime-review-fixes.md) |
| [x] | NPM 按 IPv4/IPv6 分析与存储 (`npm-ip-family-pipeline`) | P1 | 为用 npm.basic 分析并保存混合 IPv4/IPv6 离线或实时流量的用户，消除 IPv4 为 IPv6 最大表示、端口嵌套及字符串中间结果承担的存储成本；交付分族地址、会话、分析实体和物理数据库表，使两族在同一任务内正确处理、分别查询，并以真实容量与成本证据验收。 | [规格](archive/feat-npm-ip-family-pipeline.md) |
| [x] | NPM 分族链路吞吐与分配优化 (`npm-ip-family-throughput`) | P1 | 为用 npm.basic 分析混合 IPv4/IPv6 离线或实时流量的用户，降低分族后的逐包分配、重复会话访问、结束快照和单行结果编码成本；交付更高的处理吞吐，保留分族空间、原始处理顺序、三万会话容量、默认预算及全部启用实体的正确输出和持久化。 | [规格](archive/feat-npm-ip-family-throughput.md) |
| [x] | NPM ClickHouse 列式批量写入优化 (`npm-clickhouse-columnar-write`) | P1 | 为用 ClickHouse 留存 NPM 分析结果的用户，消除列式结果被展开为逐字段 URL 参数造成的写入开销；交付预算受控、同步可见的 ArrowStream 批量写入与连接复用，以同配置结果行/秒和字节/秒证明收益，保留 MergeTree、Schema、全部实体和失败/排空语义。 | [规格](archive/feat-npm-clickhouse-columnar-write.md) |

### 基线分析与预测

| 状态 | Feature | 优先级 | 目标 | 归档规格 |
| --- | --- | --- | --- | --- |
| [x] | Baseliner 批量结果输出与性能验收 (`baseliner-output-throughput`) | P1 | 为使用 CSV/DataFrame 快照进行分钟级基线分析的用户，解决逐桶生成单行批次、逐条追加反复拼接已有结果造成的分钟级延迟；交付预算受控的批量结果输出，以真实 SQL 的批次数和同配置耗时证明收益，同时保持评估/预测数值、最终模型、取消/失败及既有持久化保证。 | [归档](archive/feat-baseliner-output-throughput.md) |
| [x] | 通用基线检测 (`baseline`) | P1 | 提供 Value、Ratio、Relation 三类基线的 bootstrap、在线 rolling、可信度和风险融合能力。 | [归档](archive/feat-baseline.md) |
| [x] | Baseline 插件检视修复 (`baseline-review-fixes`) | P1 | 为使用 Value、Ratio、Relation 进行历史预热和在线检测的调用方，修复观测顺序、训练兼容、检测口径及资源累积问题；交付一致的历史/在线处理、隔离的状态替换、完整状态释放与可选容量约束，以及纳入常规 CTest 的回归保证，保持既有公共接口和数据格式。 | [归档](archive/feat-baseline-review-fixes.md) |
| [x] | Baseline 封装算子 (`baseline-operator`) | P1 | 让分析已落库 NPM 流量数据的用户，通过显式表/字段配置持续获得实际值、预期值、上下条带和偏离结果，并按配置产出未来预测；交付 explore.baseliner 的三类基线分析、独立结果表与模型/消费位置一致持久化，使结果可查询、源表保持原始事实且重启延续学习，同时提供有限容量与在线不活跃维护；字段映射可复用于普通业务表，探索性分析算子统一归属 explore 类别。 | [归档](archive/feat-baseline-operator.md) |
| [x] | Baseliner 单表与 DataFrame 分析及输出 (`baseliner-single-source`) | P1 | 让测试指定数据库表和上传 CSV 的离线分析用户，直接通过三段数据库或 DataFrame 来源执行基线任务，在 WITH 内配置少量指标并指定模型参数出口，通过单一 INTO 指定评估/预测结果出口；交付可分别查询的最终模型与一致的分析结果，减少重复配置并保持源数据、有限快照、原持续发布进度及托管恢复保证。 | [归档](archive/feat-baseliner-single-source.md) |
| [x] | Baseline/Baseliner 生命周期与评估契约修复 (`baseliner-contract-safety`) | P1 | 为通过 Scheduler 加载并调度 Baseline 分析的用户，修复停机清理与活动任务冲突、取消与输出会话交接冲突，以及冷启动依赖诊断文本的问题；交付先排空任务再释放插件状态、在途取消结束后再关闭会话、通过插件显式状态输出有效评估结果的保证。 | [归档](archive/feat-baseliner-contract-safety.md) |

### Kafka 接入与发布

| 状态 | Feature | 优先级 | 目标 | 归档规格 |
| --- | --- | --- | --- | --- |
| [x] | Kafka 连接与认证 (`kafka-connection-auth`) | P1 | 让接入不同安全策略 Kafka 集群的部署者和通道用户，在 Web 中创建和管理可复用连接配置及具名 source/sink/both 通道，获得可验证、可诊断且可持续运行的认证接入能力，并明确配置保存、连接验证与数据读写能力的不同状态；覆盖 PLAINTEXT、SSL、SASL_PLAINTEXT、SASL_SSL，交付 TLS 服务端与双向认证、主机名校验及 PLAIN、SCRAM-SHA-256/512、GSSAPI、OAUTHBEARER，包含凭证引用、脱敏与适用的更新/续期、构建能力检查和分阶段错误诊断，供四类数据通道复用；不管理集群、账号或 ACL，不将普通配置通道用于保密资源，不承诺未经适配和验收的云厂商专用认证。 | [归档](archive/feat-kafka-connection-auth.md) |
| [x] | Kafka 结构化数据接入 (`kafka-log-source`) | P1 | 让使用外部日志、指标或业务记录的分析用户，通过 Kafka 将结构化消息转换为分析算子可消费的数据批次；依赖 kafka-connection-auth，交付具名数据 source 与明确的实体 Schema，固定字段映射、类型、单位、时间和空值语义，保留 Topic、分区和位点等消息元数据，并明确消费确认、重放及非法消息处理边界；不自动识别任意格式、实现各产品专用日志解析器或从日志恢复原始包，编码支持范围在规格中冻结。 | [归档](archive/feat-kafka-log-source.md) |
| [x] | 分析结果 Kafka 发布 (`kafka-result-sink`) | P1 | 让需要向 SIEM、告警系统或其他订阅者交付分析结果的用户，将数据库查询、DataFrame 或可对接的算子输出形成的标准结果批次发布到 Kafka；依赖 kafka-connection-auth，交付可由 SQL INTO 使用的结果 sink，携带实体与 Schema 版本、结果身份和适用的 revision，区分新增事件与更新记录，并明确有界发送、确认、重复、停止排空及失败语义；不包含 NPM 全部启用实体的直接持续发布接线，不承诺跨 Kafka、外部存储和算子状态的端到端 Exactly-once。 | [归档](archive/feat-kafka-result-sink.md) |
| [x] | Kafka 原始包接入与 NPM 分析 (`kafka-packet-source`) | P2 | 让通过 Kafka 汇聚远端探针流量的 NPM 用户，消费原始包并由现有 NPM 产生会话、协议和性能事实；依赖 kafka-connection-auth，交付具名 packet source，保留原始包与必要采集元数据，明确消费位点、重放、分区顺序、时间进度及重启后的分析连续性边界；不以五元组或统计指标替代原始包，不承诺透明迁移有状态会话或跨崩溃延续同一分析状态及端到端 Exactly-once。 | [归档](archive/feat-kafka-packet-source.md) |
| [x] | 原始包 Kafka 发布 (`kafka-packet-sink`) | P2 | 让需要将网卡采集数据送往集中分析端的分布式采集部署，通过 Kafka 发布原始包与必要采集元数据；依赖 kafka-connection-auth，交付可由 SQL INTO 使用的 packet sink、与 packet source 一致的版本化消息契约和有界批量写出，明确分区路由、发送确认、背压、发送失败及采集丢包的可观察行为；不包含探针部署管理或磁盘缓冲，不以 Kafka 持久化承诺采集端零丢包。 | [归档](archive/feat-kafka-packet-sink.md) |

---

## 长远规划

以下五项 Feature 短期内不规划实施，统一降为 P3；保留待办状态及已有规格，后续按实际需求重新评估。

| 状态 | Feature | 优先级 | 目标 | 规格 |
| --- | --- | --- | --- | --- |
| [ ] | NPM 快速网卡采集通道 (`npm-af-xdp-native-zerocopy`) | P3 | 让具备 native XDP 与网卡零拷贝支持的 Linux 部署获得独立的快速直接采集入口，复用公共 packet、身份、生命周期与安全进度契约，并可验证驱动/队列能力、UMEM 回收和真实收包性能；通道名称待定（候选 netdirect），不以名称承诺固定速度。 | 待创建 |
| [ ] | NPM DPDK 运行环境与设备管理 (`npm-dpdk-runtime`) | P3 | 提供 DPDK EAL、hugepage、PCI/VFIO、NUMA、核心绑定、设备发现、能力探测和启动诊断；不包含 NPM 分析和跨进程数据面。 | 待创建 |
| [ ] | NPM 用户态高吞吐采集通道 (`npm-dpdk-capture`) | P3 | 让已配置 DPDK 运行环境的采集部署通过独立通道取得单进程持续收包能力，复用公共采集契约并可验证端口/队列范围、缓冲回收、时间戳及背压/丢包统计；通道名称待定（候选 netengine），不强制以 Linux 网卡名选择设备，不包含 Primary/Secondary。 | 待创建 |
| [ ] | NPM DPDK 跨进程零拷贝接入 (`npm-dpdk-cross-process`) | P3 | 实现 DPDK Primary/Secondary、共享 mempool/ring、mbuf 所有权回收、Scheduler/NPM 服务接入以及重启清理语义。 | 待创建 |
| [ ] | 流式历史补算 (`stream-recompute`) | P3 | 对已落地存储按时间窗口或条件执行可幂等的批处理补算，不在流式数据面实现回放。 | [规格](specs/feat-stream-recompute.md) |

---

优先级约定：P0 为核心能力，P1 为重要能力，P2 为增强能力，P3 为可选或长远规划能力，不代表近期排期。
