# FlowSQL 产品需求池

本文件只记录 Feature 级目标，不记录实施拆分、迭代安排或验收细节。每个 Feature 的契约、非目标、主链路和原子任务分别放在对应规格文件中。

状态约定：`[ ]` 待规划，`[-]` 进行中，`[x]` 已完成。

---

## 当前工作集

| 状态 | Feature | 优先级 | 目标 | 规格 |
| --- | --- | --- | --- | --- |
| [x] | NPM 数据包契约与解析基础 (`npm-packet-contract`) | P0 | 定义统一的 packet 数据实体、采集元数据、Arrow Schema 及截断/畸形报文语义，并复用现有 NPI 分层与协议识别能力。 | [归档](archive/feat-npm-packet-contract.md) |
| [x] | NPM 离线数据包全量导入 (`npm-offline-import`) | P0 | 提供 pcap/pcapng 文件的有限流读取与回放，保留原始时间戳、捕获长度、线速长度和报文顺序，接入现有流批任务运行时。 | [归档](archive/feat-npm-offline-import.md) |
| [x] | NPM 离线文件上传与通道管理 (`npm-offline-import-web`) | P0 | 支持用户在 Web 页面上传 pcap/pcapng 文件并原子创建可执行的 `pcapfile` source 通道。 | [归档](archive/feat-npm-offline-import-web.md) |
| [x] | NPM 数据包 DataFrame 查看 (`npm-packet-dataframe-view`) | P0 | 支持将有限 `pcapfile` 数据包原样写入命名 DataFrame，并通过 Web 安全查看截断的 hex 报文内容。 | [归档](archive/feat-npm-packet-dataframe-view.md) |
| [x] | FlowSQL 阶段化过滤管线 (`stage-filter-pipeline`) | P0 | 支持 source 与每级 operator 输出的类型化 WHERE，以 Arrow 统一执行并允许 channel/operator 做精确谓词下推。 | [归档](archive/feat-stage-filter-pipeline.md) |
| [x] | NPM 离线导入过滤 (`npm-offline-filter`) | P0 | 支持离线文件按带时区纳秒时间、MAC、IP、端口及默认双向 TCP/UDP endpoint pair 过滤，以类型化不可变规则实现任务隔离的 pcapfile 精确下推；不执行协议或应用识别。 | [归档](archive/feat-npm-offline-filter.md) |
| [x] | PCAP 文件通道持久化 (`pcapfile-channel-persistence`) | P0 | 将 `pcapfile` 通道的规范化配置持久化到 Scheduler 侧 SQLite，并在服务重启后按原名称恢复可重复执行的离线 source。 | [归档](archive/feat-pcapfile-channel-persistence.md) |
| [x] | 原生 PCAP 存储生命周期一致性 (`native-pcap-storage-consistency`) | P0 | 让原生受管 PCAP 文件与持久数据库共享运行目录生命周期，消除临时目录清理导致的启动恢复失败。 | [归档](archive/feat-native-pcap-storage-consistency.md) |
| [x] | 配置资源通道 (`config-channel`) | P1 | 让需要跨任务复用静态规则、映射或字典的用户和算子不再绑定各自的本地配置目录；交付 JSON/YAML/XML 文件上传、具名配置通道、永久保存、重启恢复和不可变递增版本，消费者可用 `config.<name>@<revision>` 精确取得带格式、业务 Schema 与内容摘要的只读快照；不把配置通道作为普通 SQL 数据面或密钥管理系统。 | [归档](archive/feat-config-channel.md) |
| [x] | NPM 基础分析与模块组合 (`npm-basic-analysis`) | P0 | 以同一 `npm.basic` 任务内引擎统一会话、有限采样识别和模块组合；交付离线生产 provider，并以模拟时间/采集事实验证实时周期快照、有界内存、过载和性能契约。 | [归档](archive/feat-npm-basic-analysis.md) |
| [x] | C++ 算子源码目录归位 (`operator-source-layout`) | P1 | 建立独立的 C++ 业务算子源码根目录，将 `npm_basic` 与通用能力插件分离，同时保持内置算子和运行时契约不变。 | [归档](archive/feat-operator-source-layout.md) |
| [x] | NPM 基础分析算子插件生命周期 (`npm-basic-operator-plugin-lifecycle`) | P0 | 让 `npm.basic` 作为按功能命名的 C++ 算子，通过统一多算子插件 ABI 完成上传、激活、任务租约、去激活和重启恢复。 | [归档](archive/feat-npm-basic-operator-plugin-lifecycle.md) |
| [x] | 流式算子时间驱动 (`stream-time-drive`) | P0 | 让实时有状态算子在暂时无包、持续繁忙及输出背压期间仍能获得版本化时间通知并按期维护；正常 EOF 单次终结且错误/取消不伪装成 EOF，使 `npm.basic` 可按最早截止时间驱动同一任务内全部已启用模块而不受当前观察结果影响。 | [归档](archive/feat-stream-time-drive.md) |
| [x] | NPM TCP/UDP 会话性能分析 (`npm-session-analysis`) | P1 | 让流量分析用户在同一 `npm.basic` 任务内按 `features` 启用 Session 模块、以独立的 `observing` 选择是否前台查看类型化会话性能结果；模块复用唯一会话、方向、协议识别结果、生命周期与预算，提供带有效性依据的连接、传输时延、重传和吞吐指标，不重复会话化或把序列缺口直接宣称为网络丢包。 | [归档](archive/feat-npm-session-analysis.md) |
| [x] | NPM 通用参数入口 (`npm-basic-parameters`) | P1 | 让同一 `npm.basic` 任务既需要共享配置又组合多个模块的用户，不再为每类能力增加专用顶层 `WITH` 参数或承受配置归属冲突；交付单一 `parameters` JSON 字符串入口，以 `core` 承载流量标签化引用等任务共享配置、以扁平模块 ID 承载私有配置，严格校验信封、共享字段及已启用模块，自动忽略未启用、未加载或未知模块节点，并在任务打开时冻结拥有型配置；保留旧 Basic/Session SQL 兼容，不交付具体配置内容的业务解释。 | [归档](archive/feat-npm-basic-parameters.md) |
| [x] | 流量标签化 (`flow-labeling`) | P1 | 依赖 `config-channel` 与 `npm-basic-parameters`；让网络性能、安全等分析场景中需要按 MAC、VLAN、IP/CIDR、传输协议和端口确定会话流主归属的用户，不再由各消费模块重复匹配规则或面对多结果冲突；交付通用同进程能力插件 `libflowsql_flow_labeling.so`，当前由显式启用 `labeling` 的 `npm.basic` 任务通过固定 IID 绑定精确配置快照和任务私有 DPDK ACL matcher，为每个双向会话流产生零或一个唯一具名主标签；插件统一拥有进程级 EAL 与 DPDK 依赖，未部署插件或未启用标签化不影响其他任务，启用但 provider/运行环境不可用则在任务打开前明确失败。 | [归档](archive/feat-flow-labeling.md) |
| [x] | NPM Basic 与 Flow Labeling 可运维性及代码结构收敛 (`npm-flow-labeling-refinement`) | P1 | 依赖已交付的 `npm.basic` 与 Flow Labeling；让部署运维者和后续模块开发者不再混淆构建/启动开关、进程 EAL 内存与任务 matcher 预算，也不必在平铺源码中寻找模块；交付受限的插件启动 option、8～256 MiB 连续整数的可解释任务预算、按职责归位的 `npm_basic` 目录和 README 可达的部署/配置/性能文档，保持原有任务与分类结果不变。 | [归档](archive/feat-npm-flow-labeling-refinement.md) |
| [x] | NPM 协议模块运行时基础 (`npm-protocol-analysis`) | P1 | 为在同一 `npm.basic` 任务内扩展协议能力的模块开发者解决输入接入、事务维护与异构结果处理重复的问题；复用已有唯一解码、会话、NPI、`features`/`observing` 和任务预算，补齐模块输入与共享能力消费声明、packet/datagram/session 及独立控制事件分发、公共主标签传递、独立于会话的结果实体身份与事务时间推进、类型化结果消费者接入，使各模块按统一生命周期有界运行；依赖已交付的 `npm-basic-analysis`、`npm-session-analysis`、`npm-basic-parameters` 与 `stream-time-drive`，仅使用标签准入的模块要求 `flow-labeling`；不交付 TCP 重组、具体协议解析器或结果存储，协议关联与完成判定归各模块。 | [归档](archive/feat-npm-protocol-analysis.md) |
| [x] | NPM 多实体结果存储与查询 (`npm-result-query`) | P1 | 让需要留存和回查离线分析及后续持续分析结果的 NPM 用户，能够保存全部已启用结果模块的输出并正确查询同一实体的多个版本；交付不受 `observing` 影响的统一命名空间持久化，覆盖已有 Basic/Session 与后续协议结果，按任务运行实例、实体身份和 Schema 版本区分累计快照与独立事务，支持最新值、最终态、历史查询和保留策略，并明确有界待写与写入失败语义；依赖 `npm-protocol-analysis` 的公共结果与消费者契约，具体协议模块无需各自实现存储；生产实时场景另需 `npm-basic-realtime-integration` 与相应采集能力。 | [规格](archive/feat-npm-result-query.md) |
| [ ] | NPM 共享有界 TCP 字节流 (`npm-shared-tcp-stream`) | P1 | 为同一 `npm.basic` 任务内的多个 TCP 应用协议模块解决各自处理 sequence、重传、重叠、乱序与缺口造成的重复状态和结果分歧；交付仅为命中模块所选早期唯一主标签的会话按方向共享、按需创建且任务预算有界的 TCP 有序字节视图，统一多消费者读取与缓存回收，明确连续区间、缺口、截断及方向/会话终结事实；依赖 `npm-protocol-analysis` 的消费者与生命周期契约、`npm-basic-parameters` 和 `flow-labeling`，复用唯一会话核心且不要求启用 Session 性能模块；不默认重组全部 TCP，不模拟完整 TCP 端点，消息定界与事务关联归协议模块。 | 待创建 |
| [ ] | NPM DNS 事务分析 (`npm-dns-analysis`) | P1 | 为排查 DNS 响应异常的用户提供可见 UDP/TCP DNS 消息的请求/响应关联、观测时延、响应状态及明确的不完整原因；在同一 `npm.basic` 任务内保留 UDP datagram 边界、复用共享 TCP 字节流，以独立事务身份和超时维护有界状态，输出可独立观察并由统一结果存储消费的类型化结果；依赖 `npm-protocol-analysis` 与 `npm-shared-tcp-stream`，TCP 路径经共享流使用标签准入，持久化与查询由 `npm-result-query` 提供；不重复基础分析，不包含 DoT/DoH 解密与解析。 | 待创建 |
| [ ] | NPM HTTP/1 事务分析 (`npm-http1-analysis`) | P1 | 为排查明文 HTTP/1 服务响应异常的用户提供请求/响应关联、响应状态、观测时延及明确的不完整原因；在同一 `npm.basic` 任务内基于共享 TCP 字节流交付明确支持边界的消息定界、长连接事务身份与超时维护，输出可独立观察并由统一结果存储消费的类型化结果；依赖 `npm-protocol-analysis` 与 `npm-shared-tcp-stream`，经共享流使用标签准入，持久化与查询由 `npm-result-query` 提供；不解密 HTTPS、不包含 HTTP/2 或 HTTP/3，不存储无限正文或重复基础分析。 | 待创建 |
| [ ] | NPM TLS 握手分析 (`npm-tls-handshake-analysis`) | P1 | 为排查 TCP 上 TLS 连接建立异常的用户提供与 TLS 版本可见范围一致的握手元数据、有观测依据的失败事实及明确的不完整原因，避免将未观察到后续消息直接判为握手失败；在同一 `npm.basic` 任务内基于共享 TCP 字节流维护有界握手状态，输出可独立观察并由统一结果存储消费的类型化结果；依赖 `npm-protocol-analysis` 与 `npm-shared-tcp-stream`，经共享流使用标签准入，持久化与查询由 `npm-result-query` 提供；不解密握手或应用数据，不包含 QUIC 或重复基础分析。 | 待创建 |
| [ ] | NPM ICMP 控制消息分析 (`npm-icmp-analysis`) | P1 | 为排查网络连通性异常的用户提供 ICMP/ICMPv6 回显关联、观测时延、错误消息及明确的不完整原因；在同一 `npm.basic` 任务内以独立控制消息身份和有界关联状态处理，允许被引用的 TCP/UDP 会话关联缺失，输出可独立观察并由统一结果存储消费的类型化结果；依赖 `npm-protocol-analysis` 的独立 packet/control-event 路径，持久化与查询由 `npm-result-query` 提供；不依赖共享 TCP 流或会话标签准入，不把 ICMP 强行归入端口会话。 | 待创建 |
| [ ] | NPM 实时采集通道抽象 (`npm-capture-contract`) | P1 | 定义采集源生命周期、观测域与队列身份、批次包数/字节/等待上限、时间进度/空闲确认/积压、缓冲归还和丢包/背压统计，供不同后端统一接入 NPM。 | 待创建 |
| [ ] | NPM 基础分析生产实时接线 (`npm-basic-realtime-integration`) | P0 | 依赖 `npm-basic-analysis`、`stream-time-drive` 与 `npm-capture-contract`，将版本化时间通知和采集事实接入 `npm.basic`，验收生产实时 SQL 的无包/繁忙调度、EOF、取消、背压及任务隔离；不实现采集后端、结果持久化或分析算法。 | 待创建 |
| [ ] | NPM Linux 实时采集后端 (`npm-linux-capture-backends`) | P1 | 在统一采集契约下提供 AF_PACKET、PF_RING Classic、AF_XDP copy/generic-SKB 三种后端，统一配置、生命周期、过滤、时间戳和丢包/吞吐统计；不包含 PF_RING ZC 与 AF_XDP native zero-copy。 | 待创建 |
| [ ] | NPM AF_XDP Native Zero-Copy (`npm-af-xdp-native-zerocopy`) | P2 | 提供 native XDP + AF_XDP zero-copy 能力，覆盖驱动/内核能力探测、队列与 RSS、UMEM、显式降级策略和硬件性能验证。 | 待创建 |
| [ ] | NPM DPDK 运行环境与设备管理 (`npm-dpdk-runtime`) | P2 | 提供 DPDK EAL、hugepage、PCI/VFIO、NUMA、核心绑定、设备发现、能力探测和启动诊断；不包含 NPM 分析和跨进程数据面。 | 待创建 |
| [ ] | NPM DPDK 单进程采集 (`npm-dpdk-capture`) | P2 | 基于 DPDK PMD 实现端口/队列、mempool、rte_mbuf、RX burst、时间戳、背压/丢包统计，并接入统一采集契约；暂不包含 Primary/Secondary。 | 待创建 |
| [ ] | NPM DPDK 跨进程零拷贝接入 (`npm-dpdk-cross-process`) | P2 | 实现 DPDK Primary/Secondary、共享 mempool/ring、mbuf 所有权回收、Scheduler/NPM 服务接入以及重启清理语义。 | 待创建 |
| [ ] | 流式历史补算 (`stream-recompute`) | P2 | 对已落地存储按时间窗口或条件执行可幂等的批处理补算，不在流式数据面实现回放。 | [规格](specs/feat-stream-recompute.md) |

---

## 已完成能力

| 状态 | Feature | 优先级 | 目标 | 归档规格 |
| --- | --- | --- | --- | --- |
| [x] | C++ 框架核心 (`framework-core`) | P0 | 提供插件式进程框架、统一数据接口和批处理 Pipeline。 | [归档](archive/feat-framework-core.md) |
| [x] | Python 算子与 Web 管理 (`python-web`) | P0 | 打通 C++ 与 Python 算子桥接，并提供算子、任务和通道的 Web 管理入口。 | [归档](archive/feat-python-web.md) |
| [x] | 数据库平台 (`database-platform`) | P0 | 提供数据库插件、驱动、连接池、多数据库读写和 SQL 过滤闭环。 | [归档](archive/feat-database-platform.md) |
| [x] | 路由与服务对等化 (`routing-services`) | P1 | 统一插件路由、错误映射和跨进程服务调用边界。 | [归档](archive/feat-routing-services.md) |
| [x] | Web 控制台 (`web-console`) | P1 | 提供可操作的任务、通道和系统状态管理界面。 | [归档](archive/feat-web-console.md) |
| [x] | 通道与算子目录 (`operator-catalog`) | P1 | 建立通道、算子元信息和激活状态的统一目录与唯一真相。 | [归档](archive/feat-operator-catalog.md) |
| [x] | Pipeline 与异步任务 (`pipeline-async`) | P1 | 支持多算子编排、异步执行、取消、超时和结构化诊断。 | [归档](archive/feat-pipeline-async.md) |
| [x] | C++ 算子插件 (`cpp-operators`) | P1 | 支持 C++ 算子插件独立编译、动态激活、去激活和安全卸载。 | [归档](archive/feat-cpp-operators.md) |
| [x] | 流式运行时 (`stream-runtime`) | P2 | 提供流式通道、流式算子、共享 source 和 Group DAG 执行能力。 | [归档](archive/feat-stream-runtime.md) |
| [x] | 通用基线检测 (`baseline`) | P1 | 提供 Value、Ratio、Relation 三类基线的 bootstrap、在线 rolling、可信度和风险融合能力。 | [归档](archive/feat-baseline.md) |

---

优先级约定：P0 为核心能力，P1 为重要能力，P2 为增强能力，P3 为可选能力。
