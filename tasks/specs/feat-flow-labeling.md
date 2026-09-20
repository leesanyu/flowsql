# Feature: 流量标签化

状态：`[-]` 进行中（已冻结 MVS，尚未开始 T0）；优先级：P1
依赖关系：前置 `config-channel`、`npm-basic-parameters`（均已完成）；后续 `npm-shared-tcp-stream`、`npm-protocol-analysis`

## Non-Goals

- 不返回所有命中规则，不给一个双向会话流保留多标签或标签集合；重叠命中只取唯一最高优先级结果。
- 不提供单向 packet 标签，不匹配 HTTP Host、DNS qname、TLS SNI 等解析后字符串；Labeling V1 不调用或拥有 NPI，
  不启动 TCP 重组或协议解析。现有 `npm.basic` 的 NPI 采样仍可在会话建立后按既有契约运行。
- 不把 Labeling 做成新的 SQL operator、数据通道或独立进程；不复制 packet decode、方向判断、会话表、预算或结果链。
- 不让插件自行 Resolve Config Channel、访问 SQLite/HTTP 或逐包查询 `IQuerier`；配置内容只由任务打开路径传入一次。
- 不实现自研分类器、DPDK 网卡 PMD、运行中规则热更新或跨任务 ACL context 共享；当前不抽象通用
  `IDpdkRuntimeV1`，第二个真实 DPDK 消费者出现后再复检。
- 不允许配置任意内存 offset，不绑定 NIC、不要求巨页；最小 EAL 模式必须在 T0 的真实部署测试中被证明。

## 业务意图

网络性能、安全等流量分析用户需要用上万个具名主标签统一表达 MAC、VLAN、IP/CIDR、传输协议和端口归属，不应让
各消费模块重复匹配，也不应让可选标签能力把 DPDK 依赖强加给未启用它的任务。标签的产品归属单位是双向会话流，
不是逐 packet 结果；ACL 的执行输入是本次 admission window 内去重后的唯一新会话候选。

本 Feature 交付通用同进程能力插件 `libflowsql_flow_labeling.so`。插件通过固定 IID 创建任务私有、不可变的
DPDK ACL matcher，并统一拥有进程级 EAL；`npm.basic` 是首个消费者，继续拥有唯一解码、精确配置 Resolve、会话化、任务预算和
结果投影。每个双向会话流只得到零或一个稳定主标签，provider 或 DPDK 环境不可用时在节点启动或任务打开阶段明确失败。

## 核心契约

### 独立插件与职责边界

- `libflowsql_flow_labeling.so` 是 Scheduler 进程加载的通用系统能力插件，实现 `IPlugin` 并注册
  `IID_FLOW_LABELING_PROVIDER_V1`；它不注册 operator、channel 或 external-entry，不经过 BinAddon operator ABI。
- `npm.basic::Start()` 不要求该 provider。仅当 `features` 包含 `labeling` 时，任务打开路径查询 IID 一次；
  未找到或未就绪则该任务失败，未启用标签化的任务不查询且行为不变。
- `npm.basic` Resolve 一次 `config.<name>@<revision>`，预留任务预算并把 borrowed
  `ConfigChannelSnapshot` 交给 provider 同步编译；插件不得保存 registry、引用字符串或控制面句柄。
- provider 解析/校验快照、展开双向规则并构建任务私有 ACL context；成功返回 matcher 后不再保留原始配置内容，
  运行中发布新 revision 不改变已有任务。
- `npm.basic` 从唯一 packet/layer decode 生成基础 session key 和类型化 facts：先查基础 session；命中时直接复用
  已保存的 `primary_label_id`，不调用 matcher；未命中时在有界 admission window 内按基础 key 去重候选并批量调用
  matcher，随后创建 session 并绑定标签。插件不接收裸包、不建立会话。

| 所有者 | 唯一职责 |
| --- | --- |
| Config Channel | 持有不可变 revision 和拥有型快照，不解释 Labeling 业务规则 |
| Flow Labeling 插件 | DPDK/EAL、Schema 校验、ACL 编译、批量分类、标签目录和 matcher 租约 |
| `npm.basic` | 条件消费、精确 Resolve、类型化 facts、基础 session 身份、admission staging、任务预算、结果与错误传播 |

### 执行单位与 admission 语义

- 基础 session key 先于 Labeling lookup，包含既有的 input namespace、observation domain、IP family、传输协议和
  规范化双向 IP/port endpoint；`primary_label_id` 是 session 属性，不进入基础 key 或 partition key。
- session hit 直接读取不可变 `primary_label_id`，后续 packet 不调用 matcher；`label_id=0` 也表示已完成分类但未命中，
  必须写入 session，不能作为“尚未分类”哨兵。
- session miss 或 TCP tuple reuse 产生一个 admission candidate；同一 admission window 按基础 session key 去重，
  只保留一个代表 facts，再以 `ClassifyBatch` 一次提交多个候选。候选数为 1 时允许 `count=1` 退化。
- window 达到有界候选上限、输入 block 结束或实时最大等待时间到期即 flush；staging 不保存裸包指针，ACL 失败不发布
  半成品 session。分类完成后按原 packet 顺序 replay `Observe`、NPI 采样和模块回调。
- tuple reuse 是新的 session instance，必须重新分类；旧 instance 的标签不得继承。会话结束后再次出现的同一基础 key
  也作为新 admission candidate。

`npm.basic` 在 session 建立后仍按既有 `payload_sample_packets` 契约调用 NPI `Identify`。Protocol 是 NPI 提供的客观标准
协议维度；Label 是本 Feature 根据网络自定义规则产生的归属维度。两者在同一 session 中以独立字段并列保存、分别演进，
协议结果不写入 `primary_label_id`，主标签也不写入 protocol 字段。

### 公共接口冻结

公共头文件只依赖 FlowSQL 类型和 `ConfigChannelSnapshot`，不得出现任何 `rte_*` 类型。数值 facts 使用 host
byte order；MAC 与 IP 字节保持网络顺序，IPv4 放在 `ip[0..3]` 且其余字节为 0；所有可选事实均有显式 validity。

```cpp
const Guid IID_FLOW_LABELING_PROVIDER_V1 = /* fixed in the T0 public header */;

enum class FlowLabelingErrorV1 : uint8_t {
    kNone = 0,
    kUnavailable,
    kInvalidSnapshot,
    kInvalidConfig,
    kLimitExceeded,
    kUnsupportedAlgorithm,
    kBudgetExceeded,
    kDpdkFailure,
    kAllocationFailed,
};

struct FlowLabelEndpointFactsV1 {
    uint8_t mac[6]{};
    uint8_t mac_valid = 0;
    uint8_t ip[16]{};
    uint8_t ip_valid = 0;
    uint16_t port = 0;
    uint8_t port_valid = 0;
};

struct FlowLabelVlanFactsV1 {
    uint16_t tpid = 0;
    uint16_t vid = 0;
    uint8_t valid = 0;
};

struct FlowLabelFactsV1 {
    uint32_t struct_size = sizeof(FlowLabelFactsV1);
    uint64_t observation_domain_id = 0;
    uint8_t ip_family = 0;  // 4 or 6.
    uint8_t transport_protocol = 0;
    uint8_t transport_valid = 0;
    FlowLabelEndpointFactsV1 source;
    FlowLabelEndpointFactsV1 destination;
    FlowLabelVlanFactsV1 vlan[2];
};

struct FlowLabelingCompileRequestV1 {
    uint32_t struct_size = sizeof(FlowLabelingCompileRequestV1);
    const ConfigChannelSnapshot* snapshot = nullptr;  // Borrowed for CreateMatcher only.
    uint64_t reserved_module_state_bytes = 0;
    uint32_t max_labels = 0;
    uint32_t max_logical_rules = 0;
    uint32_t max_compiled_rules = 0;
};

struct FlowPrimaryLabelViewV1 {
    uint32_t label_id = 0;
    int32_t priority = 0;
    const char* name = nullptr;
    const char* display_name = nullptr;
    const char* description = nullptr;  // Matcher-owned until Release.
};

struct FlowLabelingDiagnosticV1 {
    FlowLabelingErrorV1 error = FlowLabelingErrorV1::kNone;
    const char* path = nullptr;
    const char* detail = nullptr;  // Call-borrowed; caller copies before return.
};

interface IFlowLabelMatcherV1 {
    virtual ~IFlowLabelMatcherV1() = default;
    virtual int ClassifyBatch(const FlowLabelFactsV1* facts,
                              uint32_t count,
                              uint32_t* primary_label_ids) const = 0;
    virtual bool FindLabel(uint32_t label_id, FlowPrimaryLabelViewV1* output) const = 0;
    virtual void Release() noexcept = 0;
};

interface IFlowLabelingProviderV1 {
    virtual ~IFlowLabelingProviderV1() = default;
    virtual FlowLabelingErrorV1 RuntimeStatus(FlowLabelingDiagnosticV1* diagnostic) const = 0;
    virtual FlowLabelingErrorV1 CreateMatcher(const FlowLabelingCompileRequestV1& request,
                                              IFlowLabelMatcherV1** output,
                                              FlowLabelingDiagnosticV1* diagnostic) = 0;
};
```

`CreateMatcher` 失败时 output 不变；snapshot、facts 和 diagnostic 字符串均为调用期 borrowed，调用方立即复制错误。
`ClassifyBatch` 的 facts 输入是 admission window 内唯一的新会话候选代表，不是所有 packet；返回 0 表示整批成功，
非 0 时输出数组内容未定义且不得提交部分 session。matcher 成功发布后，
`ClassifyBatch` 和 `FindLabel` 可并发只读调用且热路径不分配；`Release` 不得与调用并发，它销毁 context、
标签目录并归还插件活跃租约，调用方不得跨插件 Stop/Unload 保存 matcher 或字符串指针。

### 唯一主标签与 DPDK ACL

- 固定一个 DPDK category，所有规则 `category_mask=1`；`userdata=label_id`，0 保留为未命中。
- priority 属于标签且不同标签不得重复；同一标签的逻辑规则继承同一 priority。逻辑规则自动展开为正向和端点互换
  的反向物理规则并去重，保证任一方向首包得到相同标签。
- 规则字段为 AND，未出现字段为 wildcard，OR 用多条逻辑规则表达。Schema 覆盖
  `MASK/RANGE/BITMASK` × 1/2/4/8 字节；多 category 是因产品唯一结果语义而唯一关闭的 DPDK 返回能力。
- 配置只选择冻结的 observation domain、MAC、两层 VLAN、IPv4/IPv6、传输协议和端口；插件生成
  `field_index/input_index/offset`、presence 字段和网络字节序 tuple。逐包变化的 TCP flags 不进入 V1。
- `algorithm` 映射 DPDK classify 实现，`max_runtime_bytes` 映射 `rte_acl_config.max_size`；非法配置、
  不支持的 SIMD、规则/字段/预算越界都在 matcher 发布前失败。详例见 `config/flow-labeling-template.yaml`。
- 会话只保存不可变 `primary_label_id`，名称由 matcher 的任务级不可变目录解析；标签不参与基础 session key，协议状态和
  协议 ID 由 NPI 独立维护，两个维度使用独立字段和生命周期。

### DPDK 构建、EAL 生命周期与部署

- DPDK 是 Labeling 真实插件私有的系统依赖，不新增 `thirdparts/dpdk`、`ExternalProject` 或私有缓存副本；构建环境
  安装 `pkg-config`、DPDK 开发包和 `rte_acl.h`，部署环境由系统包管理器提供 ABI 兼容运行库。
- 插件局部 CMake 以 `pkg_check_modules(DPDK ... IMPORTED_TARGET libdpdk)` 发现依赖并 `PRIVATE` 链接
  `PkgConfig::DPDK`，完整保留 `libdpdk.pc` 的 include、含 `-march` 的 CFLAGS、链接选项和依赖库，不手写 `-ldpdk`。
- `FLOWSQL_FLOW_LABELING=AUTO|ON|OFF`：`AUTO` 缺 DPDK 时提示并跳过真实插件，`ON` 缺依赖时明确配置失败，`OFF`
  明确不构建；Feature 完整验收必须使用 `ON`，不得以 `AUTO` 跳过真实目标后宣称完成。
- 只有 `libflowsql_flow_labeling.so` 产生 DPDK `DT_NEEDED`，公共头、mock 和 `libflowsql_npm_basic.so` 保持 DPDK-free；
  发布物不复制 `librte_*.so`，插件仅部署到 Scheduler，构建与运行环境须使用兼容发行版、CPU 参数和 DPDK ABI。
- 插件按批次 `Option → Load → Start`，Start 在任何 matcher 前执行唯一一次无 NIC、网卡绑定和巨页依赖的最小 EAL；
  每任务 ACL context 只在打开时 add/build，宿主须先 Release 全部 matcher 再 Stop/Unload/cleanup，不能依赖 Stop 返回值阻止 dlclose。
- `npm.basic` 先预留 `kModuleState`；规则、context 和目录内存不得越界。Config Channel 由 512 KiB 有界扩至 8 MiB，
  以容纳实测 1,452,272 bytes 的代表性 10K 配置，并同步 Web Base64、校验和读取边界。

## 主链路

1. **节点与任务打开**：插件批次启动并初始化一次 EAL → labeling 任务查询 provider 一次 →
   `npm.basic` Resolve 精确快照并预留预算 → provider 同步校验/编译并返回 matcher lease →
   成功后才发布任务；普通 Basic/Session 不经过这些步骤。
2. **新会话 admission**：唯一 decoder 生成基础 session key 和 typed facts → 先查 session；hit 直接复用标签且不调用
   matcher，miss 按 key 去重进入有界 window → 一次 `ClassifyBatch` 返回候选 label IDs → 按原 packet 顺序创建/绑定
   session，再运行现有 NPI `Identify` 采样和模块回调。

## Feature Tasks 与测试锚点

- `[ ]` T0：交付独立能力插件、公共 IID/接口、条件消费和进程级 EAL 生命周期，使未启用任务不依赖 DPDK，
  启用任务获得安全 matcher 租约和确定诊断。
  - `[ ]` T0.1：以 mock provider 冻结 compile request、typed facts、batch classify、目录视图、显式 Release、
    provider 缺失/未就绪及普通任务不查询 IID。
  - `[ ]` T0.2：以三态开关构建并部署 `libflowsql_flow_labeling.so`，验证 `ON` 真实链接、`AUTO/OFF` 隔离、最小 EAL、
    并发 matcher、租约先释放再卸载、ABI 完整及 `npm.basic` 无 DPDK 动态依赖。
- `[ ]` T1：交付 10K 标签可发布的有界快照和 DPDK ACL 配置编译，使全量字段能力及失败原子性由测试锁定。
  - `[ ]` T1.1：把 Config Channel 单快照及 Web/校验边界扩展为 8 MiB，锚定 8 MiB 成功、超 1 byte 失败和
    代表性 10K 标签发布/Resolve，不改变不可变 revision。
  - `[ ]` T1.2：交付严格 Schema、双向展开、tuple 编码和单 category build，非法输入不发布 matcher。
- `[ ]` T2：把 admission batch matcher 接入唯一解码/会话链，使新会话绑定稳定主标签、已有会话零次重复分类，并保持
  NPI 采样、packet/session-end 回调顺序和失败原子性。
  - `[ ]` T2.1：验证 session hit 不调用 matcher、同一 window 同 key 只产生一个 candidate、多个 miss 合并一次
    `ClassifyBatch`、未命中 0 也被缓存、tuple reuse 重新分类及 ACL 失败不发布半成品 session。
  - `[ ]` T2.2：验证 NPI 在 session 建立后按既有 pending/identified/unknown 采样运行；protocol 与自定义 label 并列输出、
    各自生命周期独立。
- `[ ]` T3：交付真实插件部署 E2E、1K/10K/50K 规则的构建内存/时间与分类吞吐、资源诊断和完整回归。

## 验收矩阵

| 验收面 | 必测断言 |
| --- | --- |
| 条件依赖 | `AUTO/OFF` 不阻塞普通构建；`ON` 缺依赖即失败；普通任务不查询 provider 且无 DPDK 依赖 |
| 插件生命周期 | EAL 只初始化一次；并发 matcher 独立；所有 matcher Release 前不得 Stop/Unload/dlclose |
| 接口所有权 | snapshot/facts 为 call-borrowed；matcher/标签字符串活到 Release；失败 output 不变；无 `rte_*` 公共类型 |
| admission 与唯一 | session hit 不调用 matcher；同一 window 同 key 只一个 candidate；未命中 0 也缓存；tuple reuse 重新分类 |
| 唯一与双向 | 无命中为 0；重叠只取最高 priority；正反首包相同；不同标签重复 priority 失败；主标签在 session 内稳定 |
| ACL 能力 | 三种 field type × 四种 size 均有正反例；IPv4/IPv6、MAC、VLAN、协议、端口可组合 |
| 批量与顺序 | admission window 有界 flush；多个 miss 一次 `ClassifyBatch`；按原 packet 顺序 replay，不改变 NPI/模块回调 |
| 热路径 | session hit 不再 ACL；不再重复解码、逐包查 IID/配置或访问 HTTP/SQLite；matcher classify 不分配 |
| 双维度结果 | NPI protocol 与网络自定义 label 同时存在、分别更新；来源、字段和生命周期语义独立 |
| 构建与部署 | `pkg-config --cflags --libs libdpdk` 完整；`readelf -d`/`ldd` 证明插件 DPDK 依赖可解且 Basic 无依赖；ABI 兼容 |
| 预算与容量 | matcher 不越过预留；8 MiB 边界和 10K 发布明确；1K/10K/50K 记录展开、内存、时间和吞吐 |

## 完成出口

1. Labeling 作为独立能力插件经固定 IID 被条件消费，不成为 operator 或第二条解析/会话链。
2. 插件独占 DPDK/EAL、配置编译和 matcher 生命周期；`npm.basic` 独占 Resolve、facts、预算、会话和结果。
3. 每个双向会话流在 admission 批量 classify 后只有零或一个稳定主标签；后续 packet 复用该标签，热路径不触达控制面且无悬空插件对象。
4. 系统 DPDK 发现、三态构建、ABI 部署、接口、容量、算法、并发、预算、完整构建和 CTest 均有可复核证据。

## 完成证据

尚未开始实现。
