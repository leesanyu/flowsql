# Feature: 流量标签化

状态：`[x]` 已完成；优先级：P1
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

- `[x]` T0：交付独立能力插件、公共 IID/接口、条件消费和进程级 EAL 生命周期，使未启用任务不依赖 DPDK，
  启用任务获得安全 matcher 租约和确定诊断。
  - `[x]` T0.1：以 mock provider 冻结 compile request、typed facts、batch classify、目录视图、显式 Release、
    provider 缺失/未就绪及普通任务不查询 IID。
  - `[x]` T0.2：以三态开关构建并部署 `libflowsql_flow_labeling.so`，验证 `ON` 真实链接、`AUTO/OFF` 隔离、最小 EAL、
    并发 matcher、租约先释放再卸载、ABI 完整及 `npm.basic` 无 DPDK 动态依赖。
- `[x]` T1：交付 10K 标签可发布的有界快照和 DPDK ACL 配置编译，使全量字段能力及失败原子性由测试锁定。
  - `[x]` T1.1：把 Config Channel 单快照及 Web/校验边界扩展为 8 MiB，锚定 8 MiB 成功、超 1 byte 失败和
    代表性 10K 标签发布/Resolve，不改变不可变 revision。
  - `[x]` T1.2：交付严格 Schema、双向展开、tuple 编码和单 category build，非法输入不发布 matcher。
- `[x]` T2：把 admission batch matcher 接入唯一解码/会话链，使新会话绑定稳定主标签、已有会话零次重复分类，并保持
  NPI 采样、packet/session-end 回调顺序和失败原子性。
  - `[x]` T2.1：验证 session hit 不调用 matcher、同一 window 同 key 只产生一个 candidate、多个 miss 合并一次
    `ClassifyBatch`、未命中 0 也被缓存、tuple reuse 重新分类及 ACL 失败不发布半成品 session。
  - `[x]` T2.2：验证 NPI 在 session 建立后按既有 pending/identified/unknown 采样运行；protocol 与自定义 label 并列输出、
    各自生命周期独立。
- `[x]` T3：交付真实插件部署 E2E、1K/10K/50K 规则的构建内存/时间与分类吞吐、资源诊断和完整回归。
  - `[x]` T3.1：交付单 context 无损规则压缩和可解释的 matcher 稳态预算，使全部既有规则组合保持等价的同时，
    每条物理规则由 44 fields/720 bytes 降至 32 fields/528 bytes，构建期不再保留完整规则字符串去重副本或依赖
    未受控的 vector 几何扩容。
  - `[x]` T3.2：交付任务级 matcher 内存参数化，使用户可在 8/16/32/64/128/256 MiB 六档中显式选择并保持
    默认 64 MiB，同时以任务总跟踪预算至少两倍于 matcher 预算的准入约束，为会话与输入状态保留确定余量。
  - `[x]` T3.3：交付 1K/10K/50K 真实规则容量画像和完整回归，使默认及可调预算的成功边界、失败诊断与最终
    Feature 完成状态具有可复核证据。

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

- T0.1 已冻结 DPDK-free 公共 ABI、mock matcher 生命周期和 `npm.basic` 条件 provider 查询；普通任务路径不查询
  Labeling IID，启用任务对 provider 缺失/未就绪给出确定失败。
- T2 已让启用任务精确 Resolve 一次不可变 Config Channel 快照，预留任务预算并创建任务私有 matcher；runtime 在所有
  成功、失败、取消和 Flush 资源释放路径持有并显式 Release matcher，普通任务的 schema 与热路径保持不变。
- 唯一 packet/layer decode 现在同时生成 host-order 数值和 network-order MAC/IP 的 `FlowLabelFactsV1`；离线 admission
  对同一窗口基础 key 去重，候选窗口按 256 有界批量 `ClassifyBatch`，整批成功后才按原 packet 顺序运行既有
  Observe、NPI 和 module callback。分类失败不创建候选 session，也不调用 NPI 或模块。
- session active view、owned snapshot 与 end event 都携带不可变 `primary_label_id`；0 被正常缓存，session hit 不再调用
  matcher，tuple reuse 创建新 instance 并重新分类，旧实例终结回调仍携带旧标签，标签不进入 session/partition key。
- 不发布的 admission planner 按原 row 顺序预测 RST、双 FIN、idle retirement 与 tuple reuse；这些同 block 生命周期边界
  后的同 key 新实例重新分类，跨 256 候选边界的同实例 hit 继续复用已绑定标签且不增加 matcher 调用。
- 显式启用 labeling 时，Basic 与 Session Arrow schema 增加非空 `primary_label_id`；NPI `protocol_status/id/name` 仍按
  pending/identified/unknown 独立采样和投影。定向测试同时验证 protocol 与 label 并列存在且互不改写。
- `cmake --build build --target test_framework test_npm_basic -j$(nproc)` 通过；定向 CTest 2/2 通过。257 个唯一候选测试锚定
  `256 + 1` 两次窗口 classify 和第二窗口失败零回放；格式化 diff、`git diff --check`、允许文件边界均通过。
- T0.2 以系统 DPDK 23.11.4 和 `PkgConfig::DPDK` 私有链接真实插件；`OFF`、缺依赖 `AUTO`、缺依赖 `ON` 与真实
  `ON` 的三态配置分别证明跳过、跳过、明确失败和成功构建。`readelf -d`/`ldd` 显示仅真实插件直接依赖
  `librte_acl.so.24`、`librte_eal.so.24`，`libflowsql_npm_basic.so` 与公共头仍无 DPDK/RTE 依赖。
- 真实插件通过固定 IID 暴露 provider，以无 NIC/巨页的最小 EAL 启动；T0.2 当时的两个任务私有 ACL
  context 可并发只读分类空规则 snapshot，活跃租约阻止直接 Stop/Unload，全部显式 Release 后完成
  Stop/Unload/cleanup；当时对非空配置的拒绝现已被 T1.2 真实编译链取代。
- 原生与 Docker 部署测试证明插件只进入包含 Scheduler 的进程，Docker 运行环境由 Ubuntu 24.04 系统包提供 DPDK 23.11
  ABI。五个定向目标构建通过，定向 CTest 5/5 通过；格式化 diff、`git diff --check` 和允许文件边界通过。
  该 T0.2 切片结束时 T1/T3 未勾选，未运行完整 CTest，未 commit/push。
- T1.1 已将 Provider、Config 控制面、Web 代理和前端原文上限统一为 8 MiB，Base64 上限按原文边界计算为
  11,184,812 bytes，Config 专用控制请求和 Router 请求体保持 12 MiB 有界入口。真实 Web→Gateway→Router→Config
  Channel 链路证明恰好 8 MiB 发布/精确 Resolve 成功，8 MiB + 1 byte 返回 413 且 current 与下一 revision
  不变；526,918 bytes 的 10,000 标签 YAML 保持 Schema ID、内容和 SHA-256 摘要一致。三个定向 CTest 3/3
  通过，前端定向测试通过，格式化 diff、`git diff --check` 和允许文件边界通过。该 T1.1 切片结束时
  T1/T1.2 保持未勾选，未运行完整 CTest，未 commit/push。
- T1.2 已在不修改 DPDK-free 公共 ABI 的前提下交付严格 `FlowLabelingSet` YAML Schema：根、metadata、
  engine/limits、labels、rules 和 matches 均拒绝未知/缺失/重复字段；标签 ID、name、priority 各自唯一，
  字符串由 matcher 拥有到 `Release`。配置限额不得超过 compile request，ACL runtime、物理规则与目录
  状态同时受 64 MiB 预留预算约束，未知/不可用算法、预算与数量越界返回区分诊断且 output 不变。
- T1.2 当时的编译器用 44 个固定 DPDK field definitions 和显式 presence 字段编码 observation domain、MAC、两层 VLAN、
  IPv4/IPv6、传输协议与端口；数值 facts 从 host order 编码为 network-order tuple。定向规则矩阵实际经过
  `rte_acl_add_rules`、`rte_acl_build` 和 `rte_acl_classify_alg`，覆盖 MASK/RANGE/BITMASK 的 1/2/4/8
  字节宽度、IPv6 四段 prefix、所有 presence 反例、无命中 0 和重叠命中最高 priority。
- 每条逻辑规则只允许 `bidirectional`，正向与端点互换反向物理规则在 build 前展开并去重；对称
  `match_all` 在 `max_compiled_rules=1` 下成功，非对称规则在同一上限下明确越界。正反首包的
  IPv4、IPv6、MAC 和类型操作矩阵均返回相同标签；四线程并发只读 `ClassifyBatch`/`FindLabel`
  稳定，分类实现仅使用固定栈上 batch 缓冲和 DPDK classify，无热路堆分配。
- 严格示例 `config/flow-labeling-template.yaml` 已由真实 provider 成功编译。
  `cmake --build build --target test_flow_labeling -j$(nproc)` 通过；定向 CTest 1/1 通过，0 失败，
  最后一次耗时 0.81 秒；格式化 dry-run/diff、`git diff --check` 与允许文件边界通过。T1/T1.2
  已勾选，T3 未勾选；未运行完整 CTest，未 commit/push。
- 规格尺寸复检为 `## 完成证据` 前 200 个非空行、4 个一级任务且无第三层编号；配置编译与
  admission 是同一“会话主标签”交付链的两个连续阶段，共享一个 Feature 完成出口，因此保持当前 Feature 边界。
- T3 非 50K 验收已在 `FLOWSQL_FLOW_LABELING=ON` 下完成：真实 Web/Config Channel revision 经 `npm.basic`
  和真实 DPDK matcher 输出稳定 `primary_label_id`；独立 benchmark 以方向无关、唯一 observation domain 规则
  成功测量 1K/10K 逻辑规则的配置大小、展开、构建耗时、实际 RSS 和分类吞吐。
- 当前最终工作树以系统 DPDK 23.11.4 完整构建通过；全量 CTest 16/16、0 失败，总耗时 56.03 秒，覆盖真实
  Flow Labeling、Config Channel E2E、`npm.basic`、Scheduler、原生/Docker 部署契约和既有全仓回归。
  50K 逻辑规则与 64 MiB 预留的容量合同按用户要求留到 T3 最后单独讨论，因此 T3 仍未勾选。
- T3.1 将私有单 context 布局无损压缩为 32 fields：九个 validity 条件合并为一个 32-bit presence bitmask，
  observation-domain prefix 归一为 arbitrary bitmask 后求交，`RANGE` 与 `BITMASK` 仍为独立通道；静态断言锁定
  `sizeof(FlowAclRule) == 528`，Schema、公共 ABI 和既有逻辑字段均未改变。
- 物理规则改为预留双向候选上界后按完整 rule data 与全部 fields 排序/精确去重，不再为每条规则构造 720-byte
  `std::string` 副本；同 predicate 不同 label/priority 不会误去重。预算按 DPDK context 长期保留的唯一 raw rules、
  runtime 上限、标签目录和固定开销准入，临时编译 vector 的多余 capacity 不再冒充 matcher 稳态所有权。
- 新增真实 DPDK 断言覆盖九个 presence 位逐一缺失、observation-domain range/prefix/bitmask 三重交集各自反例、
  prefix/bitmask 冲突永不命中、32-field 配置准入、不同 rule data 不去重及预算阈值两侧；原有全字段、正反向、
  priority、并发与模板矩阵继续通过。定向构建成功，CTest 1/1 通过，最后一次 0.67 秒。
- 最终 64 MiB/40 MiB runtime benchmark：1K 规则构建 197.951 ms、构建 RSS 增量 31,648 KiB、22.557 M/s；
  10K 规则构建 1,902.237 ms、构建 RSS 增量 105,636 KiB、22.936 M/s。两档均实际 build/classify 成功；这些是
  当前机器观察值，不是跨机器阈值或 50K 容量结论。clang-format dry-run、tracked/untracked Diff 检查通过。
- T3.2 在 V1 `framework` 中交付 `labeling_memory_mib`：默认 64 MiB，严格接受 8/16/32/64/128/256 六档，所选
  bytes 同时用于任务 `kModuleState` 预留和 provider compile request；`max_tracked_bytes` 必须至少为该值两倍，
  因而 256 MiB 档要求显式提供至少 512 MiB 总跟踪预算。labeling 未启用时字段保持 optional-module 忽略语义，
  显式启用但 provider 不可用时继续明确失败。
- 测试锚定缺省/六档、非法类型与数值、2 倍边界、解析失败原子性、任务配置归一化、128 MiB 精确传值及下一任务
  恢复 64 MiB；公共 Flow Labeling ABI、规则上限与 matcher 布局未修改。`FLOWSQL_FLOW_LABELING=ON` 配置成功，
  `test_npm_basic` 构建成功，定向 CTest 1/1 通过、0 失败（0.51 秒）；clang-format dry-run、`git diff --check` 通过。
  规格尺寸复检为 205 个非空行、4 个一级任务且无第三层编号；T3.2 与既有容量验收属于同一主链，因此不拆 Feature。
  父任务 T3 仍未勾选，节点级总预算、容量矩阵与 50K 最终验收未实施，未运行全量 CTest，未 commit/push。
- T3.3 使用 `observation_domain` 唯一值与非对称 `source_port=443` 组合，确保每条双向逻辑规则展开为两个不同物理
  规则。最终 40 MiB runtime 矩阵：1K/2K 物理规则在 64 MiB 下构建 294.766 ms、RSS 增量 41,244 KiB、
  峰值 62,880 KiB、16.152 M/s；10K/20K 在 64 MiB 下构建 2,863.214 ms、RSS 增量 153,284 KiB、
  峰值 244,188 KiB、15.755 M/s；50K/100K 在 64 MiB 下于 `/spec/engine/max_runtime_bytes` 明确返回
  `kBudgetExceeded`；改用 128 MiB 后实际 build/classify 成功，构建 14,619.722 ms、RSS 增量 650,796 KiB、
  峰值 1,061,768 KiB、15.256 M/s。RSS 含 512 MiB EAL 堆和构建期临时对象，不等同于 matcher 稳态预算。
- 50K/128 初次实测暴露 DPDK 无巨页模式默认仅预留 64 MiB，实际 ACL build 额外分配约 24.1 MiB 时失败；插件私有
  EAL 参数已显式设为 512 MiB，使最高 256 MiB 单任务 matcher 档位和构建期工作区可用，不改变公共 ABI、任务预算
  或规则语义。六档保守代表点均实际成功：8 MiB→1K（4 MiB runtime）、16 MiB→5K（8 MiB）、32 MiB→10K
  （16 MiB）、64 MiB→20K（32 MiB）、128 MiB→50K（40 MiB）、256 MiB→50K（128 MiB）；这些是当前布局与
  机器的建议值，不是任意规则组合硬上限，256 MiB 档受现有 50K 逻辑规则上限约束，主要为复杂 ACL runtime 留余量。
- 最终系统 DPDK 23.11.4 全量构建通过，完整 CTest 16/16、0 失败，总耗时 53.30 秒。`readelf`/`ldd` 复核仅
  `libflowsql_flow_labeling.so` 直接依赖且可解析 `librte_acl.so.24`、`librte_eal.so.24`，
  `libflowsql_npm_basic.so` 仍无 RTE/DPDK 依赖；clang-format dry-run 与 `git diff --check` 通过。规格完成前尺寸为
  207 个非空行、4 个一级任务、无第三层编号；所有任务和完成出口均满足，Feature 已完成并归档，未 commit/push。
