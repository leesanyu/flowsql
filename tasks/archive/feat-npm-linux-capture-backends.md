# Feature: NetAdapter 常规网卡采集通道

标识：`npm-linux-capture-backends`（沿用现有 Feature 标识）
状态：`[x]` 已完成；2026-10-05 T0～T6全部完成，Web通道配置补充通过完整DoD
优先级：P1
前置 Feature：`npm-capture-contract`、`npm-basic-realtime-integration`；本 Feature 统一升级采集契约，并同步迁移其单输入实现。
关联 Feature：[npm-basic-periodic-stats](../archive/feat-npm-basic-periodic-stats.md) 负责统一周期统计、默认模式与新结果 Schema；真实 SQL 周期联验需该 Feature 完成，采集契约/后端本身可独立实施。

## 业务意图

让 Linux 镜像口、网卡和虚拟网卡上的 NPM 用户，在Web通道管理页面创建、编辑、查看和删除具名 `netadapter` 通道，配置同一观测域中的一张或多张网卡，并交给同一个生产 SQL 分析任务。采集技术在创建配置中选择，SQL 无需暴露 AF_PACKET、PF_RING 或 AF_XDP 名称。运维者可以查询实际采集范围、生效配置、时间戳来源及逐输入丢包/吞吐，三种后端复用通道管理和有界 reader 运行逻辑。

## Non-Goals

- 不包含 PF_RING ZC、AF_XDP native zero-copy、DPDK、跨主机采集或硬件时间戳统一校准。
- 不支持同一通道混用不同后端、跨观测域会话合并、重复镜像包自动去重或跨输入严格时间戳排序。
- 不实现协议识别、会话/事务算法、结果存储、PCAP 回放或通用抓包过滤语言。
- 不实现会话周期分桶或输出频率配置；统计周期属于 npm.basic，采集批次、Poll 等待与 buffer_mib 不等于统计周期。
- 不提供用户指定 RX 队列、CPU/RSS 或底层 ring/UMEM 调优参数；必要队列覆盖由后端发现并保证。
- 不提供旧单输入采集接口的运行期兼容、版本回退或适配分支；相关组件同步升级、构建和部署。
- 不以通道类别承诺固定速度，不把过滤丢弃计作内核丢包，不用采集计数推断网络传输丢包。

## 核心数据与接口契约

### 通道定位与创建配置

三类网卡采集通道按运行方式和部署要求区分；本 Feature 只交付第一类，后两类名称未冻结。

| 定位 | 公开 Category | 采集方式与边界 |
| --- | --- | --- |
| 常规网卡采集 | `netadapter` | AF_PACKET、PF_RING Classic、AF_XDP copy/generic-SKB 三选一；按 Linux 网卡名选择输入。 |
| 快速直接采集 | 待定，候选 `netdirect` | AF_XDP Native Zero-Copy；由 `npm-af-xdp-native-zerocopy` 交付。 |
| 用户态高吞吐采集 | 待定，候选 `netengine` | DPDK；由相应 DPDK Feature 交付，设备选择不强制使用 Linux 网卡名。 |

`netadapter` 的 `Type()` 为 `block_stream`、Schema 为既有 `packet`；具名 source 为 `netadapter.<name>`。创建入口沿用现有通道管理协议，`type=netadapter`、`role=source`，options 只有以下五项，不增加用户填写的身份或调优字段。

| options 字段 | 必填 | 契约 |
| --- | --- | --- |
| `backend` | 是 | `af_packet`、`pfring_classic`、`af_xdp_copy_skb` 三选一；不可用时明确失败，不自动换后端。 |
| `interfaces` | 是 | 非空、无重复的网卡名称数组；每张网卡均需成功打开，并覆盖所声明的接收范围。 |
| `promiscuous` | 否 | 布尔值，默认 `true`；后端无法满足请求时明确失败，停止时归还本通道持有的设置。 |
| `snaplen` | 否 | 整数，范围 1～65535，默认 65535；截断时保留真实 wire length。 |
| `buffer_mib` | 否 | 正整数，默认 64；整个通道的采集缓冲预算，所有网卡输入共享。 |

```json
{
  "backend": "af_packet",
  "interfaces": ["eth1", "eth2"],
  "promiscuous": true,
  "snaplen": 65535,
  "buffer_mib": 64
}
```

```sql
SELECT * FROM netadapter.edge_mirror USING npm.basic
WITH output_interval_ns=30000000000 INTO sqlite.npm
```

示例采用关联统计 Feature 的默认能力选择与 30 秒周期，sqlite.npm 为已创建的托管目标；持续任务经既有后台入口执行并查询数据库周期历史，不将无限结果写入 DataFrame。单靠定时产生分析结果不意味着普通 HTTP 查询会持续返回数据。

配置严格校验并规范化；缺项、未知字段、非法类型/范围、重复网卡明确拒绝。网络接口在打开时解析 ifindex；非以太网链路明确拒绝，不伪造 Ethernet。一个通道代表一个观测域，适用于同一观测点的多个输入；不同观测域创建不同通道。系统为通道分配并持久化稳定观测域身份，为有效输入分配 source_id；网卡/队列覆盖、身份映射和 generation 在 reader 打开时冻结并可查询。修改配置需停止旧 reader；有任务租约或 outstanding batch 时修改/删除返回 busy，重开使用新 generation。

`buffer_mib` 覆盖采集端 ring/UMEM、待交付报文、采集封装的全部 Arrow 列及未归还输入 batch 的受控缓冲，不包含 NPM 分析或结果预算。按实际分配容量计费，包括元数据、offset、容量预留、构建/复制峰值和 release 后仍由共享 owner 保留的内存；捕获字节上限不等于实际分配上限。

source residual 的额外副本属于消费阶段，在既有 NPM 输入批次预算及端到端内存峰值中核查；采集预算不代表整个任务的内存上限。物理内存测量区分新增分配和共享引用，不能把共享 buffer 重复算作物理分配。

后端按实际分配粒度规范化，为每输入保留必要容量，并在后端可共享的范围内分配通道余量；不能因其他输入空闲而让繁忙输入无谓耗尽可共享容量。查询报告逐输入 ring/UMEM 容量、共享封装容量及预算占用；无法在预算内覆盖全部输入时失败，不跳过网卡或队列。64 MiB 是通道总预算，不按网卡或队列倍增，也不按 snaplen 为每个实际短包无谓预留等长副本。

公共逻辑生成批次包数、捕获字节、等待、outstanding 与每次 Poll 检查包数/字节、处理时间预算的有界默认值，并查询报告全部生效值。T0 冻结默认值、分配规则与测试边界：每批 256 包/1 MiB，最大等待 10 ms、outstanding=1；每 Poll 检查 1024 包/4 MiB、非阻塞工作 2 ms；每输入轮转 16 包/64 KiB。共享封装最低 2 MiB、每输入后端最低 1 MiB，按实际队列/分配粒度复检全部预算；兼顾输入服务间隔和批次处理耗时；它们不是额外创建参数，也不以增加 outstanding 默认值代替实际消费能力验证。

### 共享运行逻辑与后端职责

Web配置沿用`/api/channels/stream/{definitions/query,query,add,modify,remove}`及Scheduler对应路由。
新增独立`IBlockStreamChannelDescriptorV1`公共IID，描述类型、显示名称、source角色、五项配置Schema、有限性和重置能力；不修改既有管理器ABI。
NetAdapter描述使用现有`StreamOptionField`，配置校验仍以`ParseNetAdapterConfig`为准；页面预选AF_PACKET不改变backend必填契约。
页面显示后端枚举、网卡名称数组、混杂开关、snaplen和全通道buffer_mib，默认值与既有配置一致。
“新增网卡采集通道”与“新增Stream通道”并列，直接打开netadapter表单；通用Stream新增类型列表排除netadapter。
创建/编辑只提交Schema中的五项options；观测域、额度及reader诊断只读，不能回填为创建参数。
编辑固定通道类型和名称；使用中禁用编辑/删除并保留服务端busy校验，连续源不显示重置操作。
默认单/多进程部署在Scheduler进程加载NetAdapter及三后端provider；缺依赖仍明确失败，不自动切换后端。
保存配置不打开设备；实际设备、权限及驱动检查发生在采集任务启动时，页面保存失败保留草稿。

一个通道管理入口提供 `IBlockStreamFactory`、`IBlockStreamManager` 和 `IBlockStreamReaderFactoryV1`；Scheduler 按公共 IID 发现它。后端通过公共 IID 提供收包能力，可按依赖独立构建/加载，不分别复制完整通道管理器。所有 C++ 服务仍由框架加载 `IPlugin`，遵守批次 `Option/Load/Start` 生命周期。

公共逻辑负责配置/身份、任务独占 reader、多输入公平轮询、批次封装、限额、进度汇总和资源租约；后端负责设备打开、队列覆盖、收包、底层缓冲释放、精确过滤及原始计数。默认不为每张网卡另建分析任务；同一任务内跨输入的 packet 交替交付，以 source_id 映射回网卡/队列或明确标记的逻辑收包点。

一次 Poll 的累计阻塞等待不超过 timeout 与 max_wait_ms 的较小值，不能逐输入各等待一次而随输入数累加；采用统一就绪等待或等价有界机制，再按输入额度非阻塞消费。检查包数/字节包含被过滤报文，处理时间预算在报文或有界后端块边界检查；达到任一工作额度即返回已有 batch 或空事件，已可交付的数据不为凑满批次额外等待。公平轮询同时约束每输入的包数/字节份额，冷输入的服务间隔在验收工况中设上限；返回的进度只包含已实际检查的事实，未检查输入不能伪造为空闲。

| 后端 | 数据与生命周期 | 时间戳和依赖边界 |
| --- | --- | --- |
| AF_PACKET | 每接口使用有界 PACKET_RX_RING/mmap 块式收包，优先 TPACKET_V3；批量消费并控制块退休等待，逐包 recv 路径不能代替性能基线；单 socket 覆盖接口时以逻辑队列 0 描述；Cancel 唤醒 poll。 | 优先使用内核软件收包时间戳；不可用时明确报告用户态收包时钟。仅 Linux 构建。 |
| PF_RING Classic | 每接口的 Classic ring；使用所选版本支持的批量消费或有界批循环，明确等待与归还粒度；不调用 ZC API，不要求 ZC license；报告实际接收范围。 | 使用 PF_RING 可报告的时间戳与 drop counter；缺库/驱动时 unavailable，作为可选依赖。 |
| AF_XDP copy/generic-SKB | 明确请求 XDP_SKB + XDP_COPY；覆盖相关 RX 队列并逐队列管理 socket/UMEM；批量消费 RX descriptor、补充 FILL ring，按需唤醒，避免逐包分配；不能只打开 queue 0 冒充整网卡。 | RX 时采样同域用户态时钟；依赖可用内核/XDP/相关库，不满足时失败，不升级 native/zero-copy。 |

AF_XDP 的 XDP 挂载、redirect 范围及对主机收包路径的实际影响需可诊断；取消/关闭只回收本通道拥有的资源，不覆盖其他所有者的 XDP 程序。配置中的多网卡都必须被覆盖，某输入打开失败或运行故障使整个 reader 失败，不能静默缩小采集范围。单输入繁忙不得长期阻塞其他输入或时间驱动；每批后返回 runner 检查 deadline。

多队列覆盖不等于多队列并行处理。初始分析消费沿用单 runner 顺序处理；后端报告实际收包执行方式，内部采集并行仅在基线暴露瓶颈后按本契约有界实现，不预设每队列线程。性能比较包含 Layer 解码与 Arrow 封装成本，不预设 AF_XDP SKB/copy 快于 AF_PACKET 或 PF_RING Classic。

### 多输入身份与统一契约升级

现有 `ICaptureBlockStreamReaderV1` 和 `IBlockTransformCaptureFactTaskV1` 只支持单一采集身份。产品处于构建阶段，本 Feature 将它们统一升级为多输入契约，同步迁移 reader、Scheduler、runner、`npm.basic` 与相关测试；产品只实现升级后的采集运行路径，单输入是输入集合长度为 1 的情况。

接口使用 V2 标记此次契约修订，IID、结构大小及公共头文件在 T0 冻结；版本标记不要求同时实现旧接口。逐输入身份、事实、计数及限额沿用仍适用的 V1 数据结构，保留这些数据类型不代表保留旧 reader/任务接口。以下数据形状在 T0 公共头文件冻结。

```cpp
struct CaptureSourceSetV2 {
    uint32_t struct_size;
    uint32_t contract_version;
    std::vector<CaptureQueueIdentityV1> inputs;
    CaptureReaderLimitsV1 limits;  // 整个 reader 的批次/等待/outstanding 上限
    uint32_t max_inspected_packets_per_poll;  // 包含被过滤报文
    uint64_t max_inspected_bytes_per_poll;
    uint32_t max_poll_work_ms;  // 非阻塞处理预算，在有界工作单元边界检查
};

struct CapturePollEventV2 {
    uint32_t struct_size;
    uint32_t contract_version;
    BlockPollEvent block;
    std::vector<CaptureProgressV1> progress;  // 同次 poll，按输入分别报告
};

interface ICaptureBlockStreamReaderV2 : IBlockStreamChannel {
    virtual ~ICaptureBlockStreamReaderV2() = default;
    virtual int DescribeSources(CaptureSourceSetV2* sources) const = 0;
    virtual CapturePollEventV2 PollCapture(int timeout_ms) = 0;
    virtual int ReadInputCounters(uint32_t source_id, CaptureCountersV1* counters) const = 0;
};

interface IBlockTransformCaptureFactTaskV2 {
    virtual ~IBlockTransformCaptureFactTaskV2() = default;
    virtual int BindCaptureSources(const CaptureSourceSetV2& sources) = 0;
    virtual int AcceptCaptureFacts(const std::vector<CaptureProgressV1>& facts) = 0;
};
```

source set 新增的三项工作额度均为正，作用于整个 reader；检查字节按被检查报文的可见 captured_len 累计。阻塞等待预算与非阻塞处理预算分开，Poll 总延迟包含两者及有界工作单元耗时，不能把 max_wait_ms 当作硬实时返回保证。

同一 reader 的所有输入处于同一观测域、同一运行 generation，每个有效输入有唯一 source_id 和明确 queue_id。PacketSchema/PacketMeta 保持不变：报文 source_id 对应可查询的网卡/队列映射，sequence 按整个 reader 的交付顺序严格递增。AF_PACKET/PF_RING 无法报告物理 RX 队列时使用明确的逻辑收包点，不伪造物理队列身份。source set 在运行中不增删；接口/队列集合变化需停止重开。

V2 Describe 中的字符串借用期截止 ReleaseReader；任务绑定时复制所需身份与映射。Poll 的进度值独立持有，每输入事实序号严格递增；绑定后拒绝未知输入、重复/倒序事实和跨 generation 事实，事实集合校验失败时不能部分生效。

所有实时采集 reader 和任务均使用这一套输入集合/事实契约，不做旧单输入采集 V1 与新版 V2 的运行期选择或回退。相关二进制必须按新公共头文件重新构建并同步部署；不满足新版契约的旧插件明确拒绝绑定，不按新版布局调用旧接口。

Scheduler 绑定同一独占 reader 的完整 source set；实时 `npm.basic` 未显式配置 source_domains 时采用 reader 描述，省去重复输入数字映射。已有显式配置需与描述一致，包括显式 `all` 的旧含义；input_namespace 默认取具名 source，既有显式配置保留。单/多输入共用上述绑定规则，不得在每包路径访问控制面或通道配置。

本次统一升级只针对采集 reader 与任务事实绑定契约。既有 `IBlockTransformOperatorV1/V2` 的离线/时间驱动功能划分、非采集 BlockStream 路径与 PacketSchema 保持现有行为，并通过回归验证；默认统计模式与 Basic 结果 Schema 的变更由关联统计 Feature 负责，不在后端重复实现。

### 数据、进度与缓冲回收

每包保留 Ethernet L2 字节、Unix ns 时间戳、captured/wire length 和来源，满足 `captured_len <= wire_len`。既有 NPI Layer 解码一次，Identify 留给 `npm.basic`。source-stage WHERE 只下推能精确执行的规则，其余交给现有 residual filter，不用不等价 BPF/eBPF 近似改变结果；查询报告能力和实际生效计划。

Layer 解码使用底层报文视图，按批次预留 Arrow 列与原始字节容量，尽量直接封装到最终输入缓冲，避免逐包中间字节副本和反复扩容。实现报告从后端到输入 batch 的复制路径和预算归属；不把 mmap、UMEM 或 Arrow 名称当作端到端零拷贝证明。

每输入事实只证明本输入的进度：有包候选不超过本次已交付包的最大时间；空闲候选仅在该输入的内核/用户缓冲确认检查为空后采同域时钟。回拨、积压 present/unknown 或尚无有效事实均不产生可前进进度，不能用 Poll timeout 或消费者墙钟替代确认空闲。

runner 完成对应 batch 的 source residual、分析、下游输出和 ReleaseBlock 后，再提交同次 poll 的整组事实；空 poll 可直接提交。公共进度逻辑维护逐输入的已处理候选，以全体输入的保守最小值作为任务共同候选，再应用既有乱序容忍量；任一输入尚未确认、当前有积压或时钟异常时暂停共同进度。共同进度不回退，不使用最快输入或最大时间。

统一周期统计使用 packet 采集时间划分窗口，并以共同安全进度封闭窗口；单调 deadline 只触发维护，不能在进度未知时输出假完整周期或假零流量。reader 持续公平交付，runner 仍检查到期维护并排空已封闭结果，不因某输入繁忙而取消定时器；积压、进度停滞与时间戳来源可查询，统计输出允许晚于名义周期边界。

在声明可持续处理的负载范围内，必须验证各输入能持续形成安全事实、共同水位前进且窗口封闭延迟满足验收上限。长期非空队列即使零丢包也不能作为通过依据；共同水位停滞时需报告阻塞输入、积压状态及分析预算压力。过载时沿用保守延期与显式背压/丢包，恢复到可持续负载后验证进度恢复；不得直接用已处理最大包时间绕过积压检查。支持积压中推进需另行对齐可证明的安全时间边界，不在本 Feature 预先放宽规则。

多输入交替交付不要求全局时间排序，也不复制业务会话/协议实现。同一观测域内按既有会话键关联，独立 source_id 仍可诊断来源，不自动去重。批次同时受包数、捕获字节、等待及 outstanding 限制；达到预算上限时有界背压或显式丢包。

每 data block 恰好一次 ReleaseBlock。借用底层字节的 batch 在 release 和所有相关共享 owner 消亡前不得重用 ring/UMEM 槽位；复制到独立 Arrow 缓冲后，仅在底层视图/引用均已结束时可提前归还原槽位，独立 batch 仍需 release 并按 owner 生命周期核算预算。reader 与后端插件租约保留到全部 batch 和相关 owner 归还，不能提前销毁或卸载。统计逐输入报告 received/delivered/source dropped/reader dropped/backpressure/bytes、时间戳/计数来源与 unavailable；通道汇总不能将未知值当零，也不能重复计数。

正常停止在最后批次/事实完成后单次 EOF；取消唤醒所有输入并报告 cancelled，任一后端故障报告 error，三者互斥。取消/故障沿用异常回收，无伪造最终态。通道范围内的 reader、输入缓冲和插件租约全部释放后才允许卸载。

### 性能基线与持续运行边界

性能验收分别测量“采集 + Layer 解码 + Arrow 封装”和“完整生产 SQL”，前者使用及时归还 batch 的消费端，后者包含 source residual、分析及结果写入。先建立 AF_PACKET 基线，再在各后端可用环境复测；不同硬件/负载的结果不直接排名，不把采集封装吞吐当作完整 SQL 吞吐。

| 工况 | 可复核结果 |
| --- | --- |
| 单/双接口，64 字节短帧与 1500 字节帧，无过滤 | 明确帧长口径并阶梯提高发送速率，记录实际发送/received/delivered pps 与字节速率、CPU、采集预算峰值及逐输入丢包可用性，建立可持续处理区间。 |
| 一忙一闲、多 RX 队列、高过滤拒绝率 | 记录逐输入服务间隔、批次包数/字节分布、Poll 等待与处理耗时；持续就绪或全过滤时仍有界返回，全空闲时无无界忙轮询。 |
| 完整 SQL 的持续负载、慢消费者与过载恢复 | 每个选定持续负载工况至少跨三个统计周期，记录共同水位滞后、窗口封闭延迟、维护延迟、分析预算占用和丢包；恢复负载与消费者后验证进度及占用恢复。 |

每次验收前记录选定负载、时长、冷输入服务间隔与窗口/维护延迟上限、丢包判据及预算上限；可持续工况要求全部满足，不能只凭吞吐数字通过，也不能事后放宽界限掩盖停滞。过载恢复在分析预算未触发任务终止的有界工况验证，预算耗尽须明确失败，不承诺已终止任务自动恢复。

测量同时保存硬件/CPU、内核与库版本、网卡/队列数、实际收包执行方式、全部生效限额、snaplen、过滤规则、分析模块、统计周期、结果目标和复现命令。延迟报告 p50/p99/最大值；分开记录累计计数、采样区间速率及 unavailable，受控已知报文用于核对实际输入范围，不由这些计数推断网络传输丢包。批次上限限制工作量，不能单独证明维护延迟上限；慢分析与慢输出须纳入端到端测量。

## 主链路

1. 用户通过Web发现NetAdapter类型与五项配置，创建/查看/编辑具名通道；Web经HTTP转发Scheduler，由公共管理器校验并持久化，使用中修改/删除明确拒绝；Scheduler 创建任务独占 reader，后端打开全部接口及所需队列，返回 source set、限额与 PacketSchema；实时任务自动绑定描述后 Open，公平轮询交付 packet batch 及逐输入事实。
2. runner 处理并归还 batch，提交整组事实，按所有输入安全进度推进事件维护；关联统计 Feature 按采集时间分桶并在共同水位跨过边界后输出周期增量/累计记录，单调 deadline 负责唤醒；正常停止、取消或任一输入错误走对应终结路径，全部资源归还后释放租约。

## Feature Tasks

- [x] T0：交付 NetAdapter 五项配置、输入身份与统一升级的采集/工作额度契约，使用户通过一个具名通道选择后端和多网卡，并可查询范围、预算分配与全部生效限额。
- [x] T1：交付同步迁移、共享多输入 reader、有界公平 Poll、封装回收与保守进度绑定，使单/多输入在统一预算内消费，且可持续负载下共同水位前进而不提前超时其他输入的会话。
- [x] T2：交付 AF_PACKET 多网卡收包、缓冲限额、时间戳与逐输入统计，使标准 Linux 环境具备可运行的 NetAdapter 采集源。
- [x] T3：交付 PF_RING Classic 多网卡收包和依赖/故障诊断，使 Classic 部署可复用同一通道与分析链路。
- [x] T4：交付 AF_XDP copy/generic-SKB 的接口/队列覆盖、UMEM 管理与回收，使支持 XDP 的环境可按明确模式取得完整声明范围的流量。
- [x] T5：交付三后端契约测试、真实网卡 SQL 联验及分阶段性能基线，使身份与诊断可复核，并证明声明负载内的公平消费、水位活性、周期封闭延迟和过载恢复。
- [x] T6：交付Web通道配置管理与部署接线，使用户可创建、编辑、查看和删除可供生产SQL使用的NetAdapter源，且配置持久化、占用保护和失败反馈可验证。
  - [x] T6.1：交付可发现的配置描述、准确的连续源/占用状态和默认部署接线，使Web取得五项参数及真实操作能力。
  - [x] T6.2：交付五项参数表单、配置查看和操作保护，使用户可完成配置管理，保存仅包含可写参数且失败保留草稿。
  - [x] T6.3：交付Web到Scheduler到管理器的HTTP联验及前端行为回归，使创建、修改、持久化、占用拒绝和释放后删除形成可复核闭环。

## Feature 验收

- Web管理：HTTP查询提供NetAdapter五项Schema与source角色；创建/编辑/重启后配置与观测域一致；非法参数、后端未加载及busy明确失败且不修改原配置；释放后可删除。连续源状态和重置能力准确。
- 前端行为：类型可选、三后端枚举与默认值正确、多网卡数组及整数范围校验、编辑仅回填可写字段、busy保护、配置查看及失败草稿保留；既有Stream/PCAP操作回归，前端构建通过。

- 配置 GTest：两必填、三可选字段、默认值、未知/非法字段、重复网卡、非以太网、预算不足和后端 unavailable；确认仅公开 `netadapter` 类别，无静默后端切换或输入遗漏。
- 契约 GTest：source set 版本/身份、source_id 到网卡/队列映射、同域限制、reader 全局 sequence、PacketSchema、原始字节、截断、批次与缓冲上限；单/多输入共用接口与自动来源绑定，显式配置冲突和不满足新版采集契约的旧插件均明确失败。
- 迁移与回归：旧单输入 reader/事实绑定实现及测试同步迁移，采集运行路径无旧接口注册、版本回退或适配分支；分别覆盖长度为 1 与多输入集合，既有批次算子 V1/V2、离线 SQL 和非采集 BlockStream 测试继续通过。
- 假输入/时钟 GTest：一忙一闲、公平轮询、多路不同进度、未知积压、回拨、重复/跨 generation 事实、整组拒绝；共同候选取安全最小值，未知输入不前进，逐输入无包不等于整个通道空闲，定时维护仍运行但不伪造完整统计周期。
- Poll GTest：单/多输入全空闲的累计阻塞等待不随输入数倍增；一忙一闲满足冻结的轮询额度，高拒绝/全部过滤计入检查包数与字节且达到处理预算即让出，已有数据不等待凑批；未检查输入无空闲事实。假时钟/有界工作单元锚定检查点，真实延迟另由性能基线验收。
- 生命周期/预算 GTest：混合来源 batch 恰好一次 release；借用 owner 存活时槽位不复用，独立复制在底层引用结束后可提前归还槽位；覆盖全部列容量、复制/扩容峰值、release 后保留 owner、逐输入必要容量和共享余量，预算不足不绕过。继续覆盖 drop unavailable、逐输入/汇总计数、Cancel 全输入唤醒、部分打开失败回滚及终态互斥；AF_XDP 只进入 SKB/copy 并正确归还 UMEM。
- Linux 真实收包 smoke：三后端各在对应可用环境完成实际 L2 收包、时间戳/计数来源、过滤与取消；每后端覆盖至少两张 Ethernet 接口和各自已知包，AF_XDP 另验证多 RX 队列完整覆盖与 XDP 资源回收，不用 PCAP 或自动 skip 代替验收。
- 生产 SQL：同一 `netadapter` 源名称切换后端后仍可执行；多输入在无包/有包/积压时验证自动身份绑定与安全事件维护；联验关联 Feature 的 15/30/60 秒周期、增量与累计、同一长 TCP/UDP 会话多条记录和停止尾部，积压时不提前封闭周期；同域两接口的双向包关联同一会话，不同通道观测域隔离；某输入失败导致任务失败且全部资源释放。
- 性能与活性：三后端分别完成上述三类工况，保存环境、预设判据、命令与原始测量；在可持续负载内共同水位持续前进、封闭延迟和冷输入服务间隔达标、预算受控，零丢包但水位停滞仍不通过。慢消费者/有界过载后验证恢复，不以无积压 smoke、PCAP 回放或采集封装吞吐替代完整 SQL 持续验收。
- 实施时运行对应 CMake target/CTest；Feature 收口全量构建、完整 CTest、clang-format 与 diff 检查通过。缺运行条件记为待验，不能据编译成功标记完成。

## 完成证据

2026-10-03：规格已按 NetAdapter 定位、多网卡配置与产品构建阶段的统一契约升级方案修订，并对齐独立的会话周期统计 Feature；六项 Feature Task 均待实施，未执行真实采集或 SQL 验收。

2026-10-03：按用户确认的性能审视补齐共同水位活性、有界 Poll、复制/槽位回收、全通道预算分配、后端批处理和分阶段性能验收；未实现后端或执行性能测量，不据本次文档修订勾选任务。

2026-10-04 T0 契约检查点：V2 source set/poll/task、backend provider IID、严格五项配置与全 reader 额度已落入公共头文件；source set 同域/generation/唯一 source 与配置边界测试锚定。管理查询与真实设备有效预算需后续接线验收后收口，不提前勾选 T0。

2026-10-04 T1 检查点：单/多输入运行迁移、共享 reader/管理器、Arrow 实际容量及复制峰值预算、release 后 owner 租约落地。配置/reader/PacketSchema/框架/Scheduler/NPM Basic/periodic 7/7 回归通过（41.11 秒）；真实持续负载及后端仍待验，T0/T1 未提前勾选。

2026-10-04 T2 检查点：AF_PACKET 多接口 TPACKET_V3 RX_RING 后端构建及确定性测试通过；隔离 netns 双 Ethernet veth 的真实 L2/48 字节截断/64 字节 wire length/内核时间戳/计数/取消及 backend busy 租约通过。每接口 ring=4 MiB，received=1/drop=0；完整 SQL/持续性能仍待 T5，不据 smoke 勾选整个 Feature。

2026-10-04 T3 检查点：PF_RING Classic 非阻塞有界 scratch、多接口原子打开、实际 ring 容量与预分配峰值核查、可选依赖/驱动缺失诊断已实现。主工作区缓存 PF_RING 8.8.0 库（源码 SHA256 b7a8afa167f6449be1dfeb362e8e76a9c0116630bf80be3b2ba50369dac82f15），有库分支构建通过且动态依赖无未解析符号；当前内核无 pf_ring 模块，真实验收待验，T3 未勾选。

2026-10-04 T4 检查点：libxdp 1.4.2/libbpf 1.3.0 的 AF_XDP SKB/COPY 后端、完整 RX 队列发现、有界 UMEM、COPY 模式确认、FILL 归还与自有 XDP compare-detach 已实现并构建。隔离双 Ethernet veth 各有 2 RX 队列，全部 4 XSK 打开，真实收包/截断/用户时间戳/取消及 XDP/混杂模式回收通过。逐队列真实包与持续活性继续 T5；received kernel counter unavailable 不当作零。

2026-10-04 T5 网络恢复检查点：可复现的隔离双 veth capture/生产 SQL 记录工具已构建，运行中诊断、
实际窗口首次可见延迟、CPU/RSS 与采集容量峰值写入 JSON。AF_PACKET 受控 TCP/UDP 400 pps、64 字节帧、
15 秒周期运行 47 秒，通过 8 条完整周期、同域 2 会话与 Stop 2 条最终记录；窗口可见延迟 max=1068.205 ms，
采集容量峰值=8,394,560 字节，已采样源端与 reader 丢包均为 0。原始证据在 build/netadapter-validation/，
复现命令见 docs/netadapter.md。NetAdapter/三后端契约测试 5/5，通过全量构建；全量 CTest 首次 47/48，
恢复 ClickHouse 后失败项单独重跑 1/1。完整矩阵、AF_XDP 逐队列/生产 SQL、PF_RING 实机及 Feature 格式/回归仍待验。
无过滤 SQL 暴露 IPv6 Router Solicitation 的 Layer/session binding 问题，GDB 复现记录已隔离到工作台，
本次受控 SQL 明确使用 TCP/UDP source WHERE，不据该通过替代无过滤验收，T5 保持未勾选。

2026-10-04 T0 完成：管理查询和活动 reader 补齐每输入 16 包/64 KiB 额度、共享封装/逐输入最低预算，
活动 reader 报告实际 Arrow 可用预算；测试核对双输入身份/容量与 backend+Arrow=通道总预算，持久观测域保持稳定。
对应 flowsql_netadapter/test_netadapter_contract/test_netadapter_reader 构建通过，CTest 2/2（0.36 秒），
本任务修改 C++ 文件 clang-format-18 与 git diff --check 通过；公共采集 ABI/五项配置不变，按独立契约目标勾选 T0。

2026-10-04 T1 完成：同步 V2 迁移无旧采集 reader/事实绑定接口残留，统一等待与安全进度规则保持不变。
可控单调时钟测试锚定全过滤 1024 包、4 MiB 有界报文检查点、2 ms 让出及下一输入恢复；
单/32 输入空闲统一等待、32 轮忙闲持续安全候选、积压暂停/排空恢复、整组跨 generation 拒绝、重复/未知/回拨均通过。
对应 target 构建通过，NetAdapter/Packet/框架/Scheduler/NPM Basic/periodic 回归 8/8（36.35 秒）。
隔离 AF_PACKET 双接口 400 受控 pps/64 字节/5 秒通过，逐输入已知包 [1000,1002]，
共同候选滞后 max=8.544 ms，输入服务间隔 max=8.513 ms，采集容量峰值=8,394,560 字节；
原始记录 build/netadapter-validation/t1-af-packet-capture-64.json。全部 38 个 Feature 改动 C++ 文件格式与 diff 检查通过。
T1 按共享运行契约与独立验证目标勾选，完整三后端 SQL/性能矩阵仍属于 T5。

2026-10-04 T3 补齐检查点：test_pfring_backend 真实模式支持双接收接口和双发送 peer，
实际标记帧核对 L2、48 字节截断/64 字节 wire、时间戳/统计、borrow 回收、Cancel 和 provider 租约；
隔离复现命令已写 docs/netadapter.md。target 构建、失败诊断 CTest 1/1 与全 Feature 格式/diff 检查通过。
运行依赖预检查返回 errno=19，当前 WSL2 无 pf_ring 模块/对应 ko/内核构建目录，真实双接口与 SQL 仍待验。
用户要求依次完成，当前停在 T3，T3/T4/T5 未勾选；需要可用的 PF_RING Linux 验收入口继续。

2026-10-04 T2 完成：AF_PACKET 独立目标重验通过。目标构建、契约/reader/backend CTest 3/3（0.33 秒）、
后端/测试格式与 diff 检查通过。隔离双 Ethernet smoke 两输入各收到标记帧，48 字节截断、
64 字节 wire、内核时间戳、逐输入 received=1/drop=0、ring=4 MiB、Cancel 与 provider 租约回收通过。
共享 reader 真实双接口 capture/Cancel 5 秒通过，全部输入收到受控报文；原始证据
build/netadapter-validation/t2-af-packet-smoke.log 和 t2-af-packet-capture-cancel.json。
此前已有 AF_PACKET 15 秒周期生产 SQL 检查点；本次按后端独立功能目标勾选 T2，T5 完整性能矩阵仍待验。

2026-10-04 T4 完成：真实 smoke 遍历全部 source_id，两张双队列 TAP Ethernet 的 RX0/RX1
各实际核对 600 个标记包，逐包接口/队列/轮次、48/64 字节长度、用户 RX 时间戳、borrow 互斥/归还通过。
每队列配置 256 帧 UMEM，重复 FILL 回收超过两轮；实际 SKB/COPY、received unavailable/drop=0、
每队列容量 1,064,960 字节、总预算、全输入 Cancel、关闭重开、混杂引用恢复、XDP owner 拒绝、
compare-and-detach 保留替换 owner、最后 TAP fd 关闭后接口消失均通过。
隔离挂载/网络命名空间和临时 TUN 节点脚本可复现；当前内核 veth 未记录 skb RX 队列，
其 TX 定向命中而 generic XDP 落 RX0 的失败记录保留，未用它冒充完整 RX 队列验收。
原始四队列证据 build/netadapter-validation/t4-af-xdp-queues-smoke.log。
双 veth 共享 reader capture/Cancel 通过（用于通道接线，已知包 [1002,0,1002,0]，不代替四队列证据），
共同候选滞后 max=0.750 ms、逐输入服务间隔 max=10.393 ms、采集容量峰值=4,265,792 字节。
生产 SQL AF_XDP/TCP+UDP/400 受控 pps/64 字节/15 秒周期运行 47 秒，停止前完整周期 6 条、
同域会话 2 个、Stop 最终记录 2 条，窗口首次可见延迟 max=1059.137 ms，采集容量峰值=4,265,280 字节。
原始 JSON 为 t4-af-xdp-capture-cancel.json 和 t4-af-xdp-sql-15s-stop.json，均在 build/netadapter-validation/。
标准 CMake 配置/对应 target 构建通过，相关契约/reader/AF_PACKET/AF_XDP CTest 4/4（0.29 秒），
全部 38 个 Feature 改动 C++ 文件格式、脚本语法和 diff 检查通过。按独立范围/回收目标勾选 T4，
未将 veth SQL 当作物理网卡或 T5 全周期/持续性能矩阵，T3/T5 保留未完成。

2026-10-04 T3 完成：当前 WSL2 6.6.87.2 与 PF_RING Classic 8.8.0 上完成双 Ethernet 真实收包、
48/64字节长度、时间戳/计数、borrow互斥、Cancel全部输入、provider租约、重开、预算失败和第二接口缺失/非Ethernet回滚。
本轮复现并修复两处PF_RING 8.8.0上游生命周期缺陷：无Redis构建的条件编译误跳过socket close；
用户态socket被按内核socket分配而不保留netns引用。两份最小补丁保存在src/channels/netadapter，
同版本用户态库已重建，匹配运行内核的模块已替换，140项导入符号CRC和完整vermagic核对通过；原库/模块已备份。
正常/异常进程退出各3次，同netns socket proc节点/混杂状态归还，ring=0，dmesg增量无新增WARNING/Oops。
生产reader预算预检改用实际短包头与页面/SHMLBA容量上界，仍核查实际映射和全通道预算，1500字节/双接口/16MiB真实打开通过。
同一netadapter.live、snaplen=1500、16MiB、400受控pps联验：capture 64字节/5秒/Cancel每输入1002已知包，
共同候选滞后max=0.606ms、服务间隔max=10.599ms，预算峰值12,747,896字节；
生产SQL15秒周期/47秒：64字节Stop为6条完整周期、2会话、2最终记录，窗口可见延迟max=1059.033ms；
1500字节Cancel为6条完整周期、2会话、0最终记录，max=1055.890ms，两个SQL预算峰值分别12,747,512/12,757,752字节。
全部记录通过启动前固定的延迟/16MiB/可用丢包=0判据；额外背景流量计数不用于推断网络丢包。
每次生产退出同netns ring/proc=0、两接口promiscuity=0；生产dmesg增量无新增PF_RING WARNING/Oops。
对应targets构建通过；NetAdapter/Packet/框架/Scheduler/NPM Basic/periodic相关CTest8/8（38.22秒），
本轮C++ clang-format-18、shell语法、补丁反向核查、git diff --check通过。
原始JSON/log与module/library构建、安装、生命周期回归证据位于build/netadapter-validation/t3-*。
按T3完整独立目标勾选完成；T5完整性能/15/30/60秒矩阵、无过滤IPv6输入契约仍待验，未归档、未提交或推送。

2026-10-04 T5 本轮检查点：GDB复现合法Ethernet/IPv6 Router Solicitation（70字节，IPv6 offset=14、
版本首字节0x60、payload=16）被session/control binding误拒绝；根因为legacy32位C++位域不能读取线格式版本。
新增合法IPv6 UDP、非零traffic class/flow label、非法版本及Ethernet ICMPv6无端口控制输入断言，
先以合法线格式夹具复现旧实现失败，再将版本校验改为首字节高四位。未修改公共Ipv6Header/ABI或默认basic模块语义。
无过滤SQL显式启用features='basic,icmp'，Basic/ICMP复用单一managed目标。
对应三个targets构建通过，NetAdapter/Packet/NPM Basic/ICMP/periodic相关CTest6/6（0.92秒）；
修改C++的clang-format-18、shell/Python语法、git diff --check通过。

阶段工具补齐single/dual/busy_idle/reject、64/1500字节、100/500/1000 tick/s，
记录实际sent/errors、每200ms诊断样本、batch包数/字节分布、共同候选推进与不可用次数、CPU/RSS/容量。
48项隔离veth capture实际执行：AF_PACKET12/16、PF_RING16/16、AF_XDP16/16；
峰值容量分别8,482,112/12,846,712/4,308,032字节，最大输入服务间隔13.609/24.033/16.065ms。
AF_PACKET四项失败为dual64/500、dual1500/1000、reject64/1000、reject1500/1000（字节/tick/s）；
均共同候选推进=0且已采样丢包=0，不属于可持续范围，不调整backlog门禁或阈值绕过。

生产无WHERE/双接口400受控pps五项SQL全部通过，每项至少三周期：
AF_PACKET15秒/47秒/64字节Stop、PF_RING15秒/47秒/1500字节Cancel、
AF_XDP15秒/47秒/64字节Stop、30秒/92秒/1500字节Cancel、60秒/182秒/64字节Stop。
均停止前6条完整周期、同域2会话，Stop2最终记录/Cancel0最终记录；
窗口首次可见延迟最大值依次1065.430/1074.540/1066.290/1059.480/1060.424ms，预算/可用丢包判据均通过。
PF_RING退出仍核对同netns ring/proc=0和两接口promiscuity=0。
完整本轮53项49通过/4失败，矩阵退出码1、summary.passed=false，原始JSON/log在build/netadapter-validation/t5/。
工具入口/判据/限制见docs/netadapter.md；AF_XDP多队列实发仍引用T4双TAP证据，veth不代替物理网卡性能。
T5保持[ ]：慢分析/慢输出、有界过载恢复、实际维护延迟/分析预算（工具明确null）、
完整三后端生产SQL持续矩阵、观测域隔离/输入故障任务回收及Feature全量回归仍待验；本切片结束，未归档或提交。

2026-10-04 T5运行观测/恢复检查点：预算实际Reserve成功时同锁记录tracked/pending/input高水位，
managed_result.diagnostics报告真实占用、限额、共同候选及阻塞输入；runner每200ms在执行线程发布快照。
测试专用provider经现有C ABI/纯虚接口包装生产npm.basic，真实SQLite prepared输出受控延迟；
维护迟到与调用间隔分别测量，不用HTTP耗时代替。对应target构建通过，相关CTest7/7（14.46秒），
预算失败/释放后高水位保持及独立任务断言通过；C++格式、shell/Python语法、diff检查通过。

三后端15秒周期/47秒slow_analysis（5～35秒每批20ms）和slow_output（前35秒每次prepared写10ms）
六项全部通过；各6条完整周期、2会话、Stop2最终/Cancel0最终。
最大维护迟到AF_PACKET3.764/6.726ms、PF_RING3.791/4.876ms、AF_XDP4.116/3.993ms；
tracked峰值≤22392字节、pending峰值≤15552字节，均低于冻结16MiB/4MiB。
过载5～10秒5000tick/s+每批50ms，随后恢复100tick/s：PF_RING/AF_XDP首个新安全进度1.564/0.171秒，
至少3次新进度且周期输出恢复。AF_PACKET首次失败源于跨阶段窗口归类，原始记录保留；
最终独立复测通过，恢复0.276秒、最大维护迟到7.323ms，逐窗口边界/首次可见/影响归类完整保存。
压力窗口允许保守延期，持续判据未提高，已恢复阶段丢包不得继续增加。
九项首轮测量8通过/1失败+AF_PACKET最终复测1通过，启动/注册失败日志也保留，不合并成全矩阵全绿。

功能矩阵最终AF_PACKET前缀5项：single/busy_idle通过；reject的WHERE false被谓词语法拒绝，
domain_isolation未取得第二任务run_id，input_failure未实际触发第二输入EIO，均待修复/重验；
余下两后端未完整执行。15:27UTC时间盒停止，本轮状态当前错误待修复，T5保持[ ]。
证据build/netadapter-validation/t5-recovery-abi/、t5-af-packet-overload-final.{json,log}、t5-functional-final/。
剩余：功能入口修复与三后端隔离/故障回收、AF_PACKET/PF_RING30/60秒、完整SQL持续/高拒绝矩阵，
Feature全量build/CTest/格式及归档；未提交或推送。

2026-10-05（北京时间）T5功能联验检查点：三个后端single/busy_idle/reject/domain_isolation/input_failure
新矩阵15/15实际通过，退出码0、summary.passed=true；旧12/15及并发失败记录均保留。
全拒绝谓词改为wire_len<0，0会话/0最终记录、安全候选推进；测试probe注册顺序修正后，
三个后端均实际触发第二输入EIO、整任务failed/0最终记录、reader租约归还、分析预算归零。
观测域并发旧入口配置NPI concurrency=1不足以运行两任务；改2槽位后已创建runtime但仍有后续失败：
AF_PACKET/PF_RING任务失败或无结果原因未确认，AF_XDP同接口第二reader返回EBUSY。
沿用冻结的顺序隔离边界：相同受控双向TCP/UDP帧、两个具名通道、单一managed SQLite目标、concurrency=1。
各后端两个run_id不同、全库两观测域且每run单域/两会话/两条Stop最终记录，第二任务stopped；
JSON明确domain_execution=sequential，不以该通过声称同接口/同目标并发支持。
最大逐输入服务间隔22.676ms、维护迟到1.053ms，均低于冻结100ms；PF_RING关闭资源核查通过。
对应target构建、相关CTest7/7（22.66秒）、修改C++格式、shell/Python语法、git diff --check通过。
原始JSON/log：build/netadapter-validation/t5-functional-sequential/；
并发复测：t5-domain-npi-two/；构建/回归日志：t5-domain-sequential-build.log、t5-functional-sequential-ctest.log。
本轮仅功能联验到达通过检查点；AF_PACKET/PF_RING30/60秒、持续SQL/高拒绝矩阵、
增量/累计与revision断言、Feature全量build/CTest/格式仍待完成，T5保持[ ]。

2026-10-05 T5周期/结果联验：验证工具新增逐session revision连续、UTC网格/长度、
各方向interval前缀和=累计、字节方向和=total、≥3完整周期与方向关联断言。
high_reject独立于全拒绝，保留帧长、每100tick仅一组端口41000/41001，其余src_port41002，
精确WHERE src_port<41002，保存成功selected/rejected发送数及结果端口/字节约束。
候选blocked时仅测最近已确认候选的年龄，生产进度门禁不变，2000ms阈值不提高。
加强观测后AF_PACKET100tick/s的30/60秒失败，最大候选滞后5029.983/5495.104ms；
结果一致性正确、可用丢包0仍判失败，t5-periods-complete/原始记录保留，案例边界STOP退出130。
下一片启动前明确冻结25tick/s、双向100受控pps，AF_PACKET/PF_RING30/60秒实际4/4通过，
候选最大滞后586.225/579.413/264.706/430.476ms，窗口最大1074.914/1081.371/1072.148/1050.195ms。
四项每会话3完整周期、结果一致性0失败，Stop各2部分尾部最终、Cancel0最终；预算/可用丢包均通过。
原始JSON/log t5-periods-25/、相关CTest7/7（22.45秒）t5-periods-25-ctest.log。
AF_XDP旧15/30/60真实SQL原数据库只读一致性核对3/3通过，审计t5-af-xdp-period-consistency-audit.json。
两个周期切片先后收口，持续24项按25tick/s已冻结启动；T5待持续矩阵及Feature全量DoD，不提前勾选。

2026-10-05 T5完成：三后端×64/1500字节×single/dual/busy_idle/high_reject持续矩阵24/24实际通过，
每项47秒/15秒周期/25tick/s；全部每会话≥3完整周期、revision连续/UTC网格/interval前缀累计/方向关联正确。
high_reject实际拒绝率约98.98%，无拒绝端口/字节偏差，已交付前缀不超过真实selected发送量。
AF_PACKET/PF_RING/AF_XDP候选最大滞后603.013/257.044/247.579ms，窗口最大1078.569/1072.629/1067.526ms，
服务最大29.182/32.503/24.728ms，维护迟到7.103/4.687/4.153ms；采集峰值8412608/12757752/4276032字节。
tracked峰值≤9566字节、pending≤15552字节；可用source与reader丢包=0，全部冻结判据满足。
原始记录t5-sustained-25/，汇总t5-sustained-25-aggregate.json，预设负载与命令/全部环境/限额保留。

完整DoD全量build通过、无Error/Warning；42个Feature修改C++格式、shell/Python语法、diff检查通过。
首轮沙箱完整CTest44/48，四项socket创建/数据库连接权限失败；获准升级后完整48/48通过（66.83秒）。
原始完整日志t5-final-full-build.log、t5-final-format.log、t5-final-full-ctest.log及t5-final-full-ctest-unrestricted.log。
完成清单t5-completion-evidence.json机器核查功能15/15、剩余周期4/4、持续24/24、慢消费/最终恢复9/9、
AF_XDP旧三周期一致性3/3、后端smoke/四RX队列证据、完整DoD和工具/二进制SHA256。
历史负载/并发/权限失败保持原样；可持续声明限定本WSL2隔离Ethernet环境与25tick/s选定工况，
不声称物理网卡高速吞吐或同接口/同目标并发支持。T5完整目标已达，勾选完成、规格归档，未提交/推送。


2026-10-05 T6补充完成：新增独立IBlockStreamChannelDescriptorV1公共IID，NetAdapter提供source角色、
五项参数Schema及连续源/无reset操作能力；Scheduler沿既有definitions/query及管理路由发现描述，
管理查询正确报告busy占用和连续源状态，后端未加载返回unavailable。既有管理器/采集运行ABI保持原契约。
Web表单支持后端选择、多网卡、混杂、snaplen和buffer_mib；只提交可写Schema字段，观测域和reader诊断不进入保存请求；
配置查看、类型/名称固定、busy操作保护、失败保留草稿完成。默认单/多进程部署加载管理器及三provider。
HTTP管理联验验证创建/重复名称/非法输入/后端未加载/busy拒绝/释放后修改/重启持久化/稳定观测域/删除。
测试加载生产NetAdapter、Builtin、Scheduler、Web插件；受控provider仅提供管理租约，不代替真实收包性能证据。
前端专项4/4、完整前端行为回归26/26通过；Vite构建通过，现有Rollup纯注释及大chunk提示保留，未扩展打包优化范围。
配置及目标构建通过；首次沙箱目标CTest3/5（两项本地socket被禁止），按权限流程复验5/5（3.11秒）通过。
最终全量构建无Error/Warning，完整CTest49/49（66.04秒）通过，5个C++文件及Scheduler本片改动行格式检查通过。
规格完成证据前153个非空行、7个一级Feature Task、编号最多两级；Markdown链接/围栏及diff检查通过。
本片原始日志build/netadapter-validation/web-{configure,target-build,target-build-2,target-ctest,target-ctest-unrestricted,
frontend-tests,frontend-build,full-build,full-ctest-unrestricted}.log；失败构建与沙箱日志保留。
T6及子任务勾选完成，Feature重新归档；本片未提交或推送。


2026-10-05 T6.2入口调整完成：“新增网卡采集通道”与“新增Stream通道”并列，
网卡入口直接预设netadapter/source并打开五项参数表单；普通Stream新增类型列表排除netadapter。
网卡新增/编辑使用专用标题且不再展示类型选择；服务未提供NetAdapter元数据时提示并保持表单关闭。
已有页面行为验收覆盖两入口默认选择、source角色、编辑/保存/草稿与busy保护，前端完整回归26/26通过。
npm生产构建通过（25.52秒；保留既有打包提示）；日志web-entry-frontend-{tests,build}.log。
本片仅前端与入口文档，无后端变更；前序完整CTest49/49为历史证据，本片未重跑C++回归。
