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
| [-] | NPM 基础分析与模块组合 (`npm-basic-analysis`) | P0 | 同一 `npm.basic` 引擎消费离线/实时 packet，统一会话、有限采样识别和模块组合；离线 EOF 及时收口，实时依赖时间驱动输出周期快照，明确有界内存、过载和性能契约。 | [规格](specs/feat-npm-basic-analysis.md) |
| [ ] | 流式算子时间驱动 (`stream-time-drive`) | P0 | 在保留 V1 ABI 的前提下提供可选版本化时间通知，覆盖无包/繁忙调度、链式传播、输出背压与取消；正常 EOF 立即单次终结，支撑 NPM 等有状态算子。 | 待创建 |
| [ ] | NPM TCP/UDP 会话性能分析 (`npm-session-analysis`) | P1 | 依赖 `npm-basic-analysis`，作为同一 NPM 算子的可选模块复用会话 ID、方向、协议标签及生命周期，计算连接、时延、重传和吞吐等性能指标；丢包指标仅在具有可观测依据时提供，不重复建立会话。 | 待创建 |
| [ ] | NPM 应用协议分析 (`npm-protocol-analysis`) | P1 | 依赖 `npm-basic-analysis`，作为同一 NPM 算子的可选模块按需重组、增量解析 DNS、HTTP、TLS、ICMP 等协议/事务；独立于完整性能分析，约束跨包缓存并标记不完整结果，协议识别成功后仍持续解析。 | 待创建 |
| [ ] | NPM 结果存储与查询 (`npm-result-query`) | P1 | 将 packet、flow、session、protocol 结果写入存储通道；支持实时累计快照的版本更新/最新值查询、持续写入及保留策略，避免重复求和和内存结果无限增长。 | 待创建 |
| [ ] | NPM 实时采集通道抽象 (`npm-capture-contract`) | P1 | 定义采集源生命周期、观测域与队列身份、批次包数/字节/等待上限、时间进度/空闲确认/积压、缓冲归还和丢包/背压统计，供不同后端统一接入 NPM。 | 待创建 |
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
