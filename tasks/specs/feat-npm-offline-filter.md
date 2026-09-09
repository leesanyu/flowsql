# Feature: NPM 离线导入过滤

状态：`[-]` 进行中
优先级：P0
前置 Feature：`npm-packet-contract`、`npm-offline-import`、`stage-filter-pipeline`（均已完成）
后续 Feature：`npm-basic-analysis`、`npm-session-analysis`、`npm-protocol-analysis`

## 业务意图

用户可以在 `pcapfile` source 阶段按采集事件时间、MAC、IP、传输层端口以及两个 endpoint 之间的 TCP/UDP
四元组过滤离线 pcap/pcapng。时间文本在规划期按显式时区转换为 epoch nanoseconds；地址、端口和双向
endpoint pair 在任务初始化时编译成不可变类型化规则，pcapfile 在构造 Arrow batch 前完成任务隔离的精确
下推，未下推部分继续由通用 Arrow residual 保证正确性。

主用例：

```sql
SELECT *
FROM pcapfile.rdp
WHERE timestamp_ns >= TIMESTAMP '2026-09-08T09:30:00.123456789+08:00'
  AND timestamp_ns <  TIMESTAMP '2026-09-08T10:00:00+08:00'
  AND tcp('192.0.2.10', 52314, '198.51.100.20', 3389)
INTO dataframe.rdp
```

## Non-Goals

- 离线导入仍只执行 `npi::Layer()`，不调用 `npi::Identify()`；不提供 `protocol_id/protocol_sub_id`、HTTP、DNS、
  TLS、RDP 等协议或应用识别过滤。
- `tcp(...)`/`udp(...)` 只依据 Layer 解码得到的 IANA 传输层编号 6/17 和 endpoint，不代表 TCP stream、会话、
  客户端/服务端方向、重组或事务分析；不提供 `tcp.stream`。
- 不支持 payload 内容、正则、BPF/tcpdump 语法，不建立持久化 PCAP 时间或四元组索引。
- 不支持无时区本地时间或 IANA zone name，不依赖服务器时区或时区数据库猜测用户意图。
- 不修改共享 `IBlockStreamChannel` 的全局过滤状态，不向既有接口追加虚函数。
- 不改变 `PacketSchema()`、raw bytes、sequence、行顺序、EOF/错误/取消和 exactly-once release 语义。

## 用户过滤契约

### SQL、通用过滤语法与领域语义的职责边界

`SqlParser` 只负责识别 SQL statement 的 `SELECT/FROM/USING/THEN/INTO` 结构，并将每个 stage 后的
`WHERE` 子句作为原文与该 stage 对齐保存；它不解析 `WHERE` 内部 AST，不认识 `TIMESTAMP`、`tcp`、`udp`
或任何通道/算子领域字段。数据库 source 的原生 SQL 继续作为 `sql_part` passthrough，其内部 `WHERE` 不得被
误识别为 FlowSQL stage filter。

独立 `FilterExpressionParser` 统一解析与领域无关的表达式语法：`AND/OR/NOT`、比较、`IN`、`BETWEEN`、
`IS NULL`、函数调用和 typed literal。它只生成通用 AST，不校验函数名、字段、地址、端口、时区或业务类型。
typed literal 采用可扩展语法：

```ebnf
typed_literal := identifier single_quoted_string
```

例如 `TIMESTAMP '2026-09-08T01:30:00Z'` 的 AST 是
`FilterLiteralKind::kTyped`、`type_name = "timestamp"`、`text = "2026-09-08T01:30:00Z"`。未来新增
`UUID '...'`、`IP '...'` 或 `DURATION '...'` 不得要求修改 statement parser 或 filter expression parser。
`tcp(...)`、`udp(...)`、`mac(...)`、`ip(...)`、`port(...)` 同样始终解析为普通 function call。

当前 stage 所属通道或算子的领域 resolver 负责解释领域字段、函数和 typed literal，并编译为不可变类型化规则；
通用 schema binder/Arrow residual 负责其余可执行表达式。provider 不得自行重复解析完整 `WHERE` 字符串，
`IFilterPushdownV1` 只协商已经完成领域绑定的精确候选计划，不承担字符串解析或领域语义识别。

### 时间语法、时区与精度

时间过滤使用通用 typed literal 语法；`TIMESTAMP` 只在 pcapfile stage 的领域 resolver 中获得时间含义：

```ebnf
timestamp_literal := identifier single_quoted_rfc3339
                    # identifier 经领域绑定后必须为 TIMESTAMP
```

合法示例：

```sql
timestamp_ns >= TIMESTAMP '2026-09-08T01:30:00Z'
timestamp_ns <  TIMESTAMP '2026-09-08T09:30:00.123456789+08:00'
```

契约：

- 文本必须携带 `Z` 或显式 `±HH:MM`；无 offset 的本地时间、IANA zone name 和 leap second 必须在绑定阶段失败。
- 小数秒允许 0～9 位，右补零到 ns；超过 9 位失败，不截断、不四舍五入。
- pcapfile 领域 resolver 只解析一次并转换为 UTC epoch ns，溢出 `int64_t` 时失败；数据面只比较
  `int64_t timestamp_ns`。
- pcap/pcapng 原始精度统一换算为 ns，但不凭空增加精度；比较以 reader 已归一化的 `timestamp_ns` 为准。
- 连续窗口推荐半开区间 `[start, end)`，即 `>= start AND < end`；既有 `BETWEEN` 仍表示两端包含。
- 继续允许机器生成的 epoch-ns 整数字面量直接与 `timestamp_ns` 比较。
- 文件时间戳允许回退；命中时间上界只能排除当前 record，首版不得据此提前返回整个文件 EOF。

### Wireshark 风格的方向中立过滤

常用过滤默认不区分 source/destination，领域函数均返回 boolean：

```sql
mac('00:11:22:33:44:55')  -- src_mac 或 dst_mac
ip('192.0.2.10')          -- src_ip 或 dst_ip，统一 IPv4/IPv6
port(3389)                 -- src_port 或 dst_port，要求 ports_valid
```

MAC/IP 文本只在绑定阶段规范化；MAC 固定为 6 字节，IPv4 使用与 `PacketSchema()` 一致的 network-order
`uint32_t`，IPv6 固定为 16 字节。端口必须位于 `[0, 65535]`，不得截断。需要明确方向时仍可组合现有
`src_*`/`dst_*` 字段；首版不再增加另一组单向快捷函数。

### 默认双向 TCP/UDP endpoint pair

只提供两个主四元组谓词，不引入 `flow`、`conversation`、`tuple` 或方向参数：

```ebnf
transport_pair := TCP "(" ip_literal "," port_literal "," ip_literal "," port_literal ")"
                | UDP "(" ip_literal "," port_literal "," ip_literal "," port_literal ")"
```

函数名大小写不敏感，四个参数依次命名为 `endpoint1_ip, endpoint1_port, endpoint2_ip, endpoint2_port`。
`tcp(A, a, B, b)` 精确等价于：

```text
transport_protocol == 6 AND ports_valid AND
((src == A:a AND dst == B:b) OR (src == B:b AND dst == A:a))
```

`udp(A, a, B, b)` 使用传输层编号 17，其余语义相同。两端地址必须属于同一 IP family；IPv4/IPv6 混用、
非法地址、参数数量错误和越界端口均在执行前失败。截断、非 TCP/UDP、无有效端口或无法取得完整 endpoint 的
报文不匹配。pcapfile 当前采用 `EndpointScope::kInnermost`，因此隧道报文匹配最内层成功解码的 endpoint。

两个 endpoint 在编译阶段按 `family + address bytes + port` 排序；报文运行时做相同规范化。因此
`A:a → B:b` 与 `B:b → A:a` 命中同一规则，而 `A:b → B:a` 不会误命中。多个规则继续使用 `AND/OR/NOT`；
SQL 三值逻辑保持不变，`WHERE` 只保留 `TRUE`。

## 类型化规则与接口边界

SQL/JSON 只属于规划控制面，数据面不得解释字符串。当前 stage owner 通过版本化
`IFilterDomainResolverV1` 认领领域语义；非 owner 返回 `ENOTSUP`，未命中 resolver 时由通用 schema binder
处理。resolver 成功时必须返回与输入等价、可交给通用 binder 的完整 lowered AST，不得修改输入或保留请求
指针；被替换子树的根保留原 `node_id`，新增节点使用高位 synthetic ID 区间，保证诊断和 pushdown 拆分稳定。

```cpp
struct FilterDomainResolveRequestV1 {
    uint32_t contract_version;
    FilterDomainTargetKindV1 target_kind;  // source or transform
    const char* target_category;
    const char* target_name;
    std::shared_ptr<arrow::Schema> output_schema;
    std::shared_ptr<const FilterExpr> expression;
};

struct FilterDomainResolveResultV1 {
    std::shared_ptr<FilterExpr> lowered_expression;
    std::string diagnostic;
};

interface IFilterDomainResolverV1 {
    virtual int Resolve(const FilterDomainResolveRequestV1& request,
                        FilterDomainResolveResultV1* result) const = 0;
};
```

pcapfile resolver 把 `TIMESTAMP` typed literal 和 `mac/ip/port/tcp/udp` function call 降低为类型化字段、scalar
及布尔节点，随后复用通用 binder、Arrow residual 和 `IFilterPushdownV1`。parser、Scheduler、通用 binder
均不得按函数名注册 packet 语义。

packet reader 的内部执行模型在 `packet_filter_plan.h` 中使用以下数值/定长字节数据契约：

```cpp
struct PacketIpKey {
    AddressFamily family;           // IPv4 or IPv6
    uint32_t ipv4_network_order;
    std::array<uint8_t, 16> ipv6;
};

struct PacketMacKey {
    std::array<uint8_t, 6> bytes;
};

struct PacketEndpointKey {
    PacketIpKey address;
    uint16_t port;
};

struct TransportPairKey {
    uint8_t transport_protocol;     // TCP=6, UDP=17
    PacketEndpointKey first;        // canonical min endpoint
    PacketEndpointKey second;       // canonical max endpoint
};

struct TimeRangeNs {
    std::optional<int64_t> lower_ns;
    std::optional<int64_t> upper_ns;
    bool lower_inclusive;
    bool upper_inclusive;
};

struct PacketUnsignedRange {
    PacketUnsignedField field;      // captured_len/wire_len/source_id/sequence
    std::optional<uint64_t> lower;
    std::optional<uint64_t> upper;
    bool lower_inclusive;
    bool upper_inclusive;
};

enum class PacketFilterRuleKind {
    kMatchAll, kMatchNone, kAnd, kOr, kNot, kTimeRange, kUnsignedRange,
    kMacAnyOf, kIpAnyOf, kPortAnyOf, kTransportPairAnyOf
};

struct PcapFilterPlan {
    EndpointScope endpoint_scope;   // fixed to kInnermost for this feature
    PacketFilterRule root;          // immutable typed boolean rule tree
};
```

`PacketFilterRule` 按 kind 使用对应 range/set/key 字段：逻辑节点只使用 operands；时间/无符号范围使用已规范化
边界；MAC/IP/port/pair 使用去重集合。`TransportPairKey.first/second` 必须按
`family + network-order address bytes + port` 排序，transport protocol 只保存 IANA 数字 6/17。canonical plan
可以作为插件间的版本化规划期传输格式，但 reader 创建时只能解析一次并深拷贝为 `PcapFilterPlan`。每包禁止
解析 RFC3339、调用 `inet_pton()`、格式化地址、解析 SQL/JSON 或比较 `"TCP"`、`"UDP"`、IP/MAC 文本。

`IFilterPushdownV1` 继续只负责无状态规划协商。为把 pushed plan 安全交给执行实例，新增版本化 source task
reader IID，不改变现有 `IBlockStreamFactory`/`IBlockStreamChannel` ABI；概念契约为：

```cpp
struct BlockStreamReaderConfigV1 {
    uint32_t contract_version;
    const char* task_id;
    const char* source_category;
    const char* source_name;
    const char* pushed_filter_plan_json;
};

interface IBlockStreamReaderFactoryV1 {
    virtual int CreateReader(const BlockStreamReaderConfigV1& config,
                             IBlockStreamChannel** reader) = 0;
    virtual void ReleaseReader(IBlockStreamChannel* reader) = 0;
};
```

每次查询必须得到独占 reader，provider 在 `CreateReader()` 返回前深拷贝配置并编译不可变规则，查询结束由同一
provider 释放。IID 遍历中非 owner 必须返回 `ENOTSUP` 且不产生 reader；成功必须返回非空 reader，其他错误
不得夹带 reader。不能建立任务隔离 reader 时必须保持 source 零下推并执行完整 Arrow residual，严禁通过
共享 channel 的 `SetFilter()` 或等价可变状态实现。

## 两条主链路

### 规划与任务隔离

```text
SQL statement → stage-aligned WHERE text → FilterExpressionParser → generic FilterExpr
    → current stage domain resolver + schema binder → typed/canonical plan
    → IFilterPushdownV1 精确候选协商 → pushed + residual 等价拆分
    → IBlockStreamReaderFactoryV1 创建任务独占 reader → 一次性编译 PcapFilterPlan
```

顶层 `AND` 可以按候选子项拆分；`OR/NOT` 只有整棵子树能被 pcapfile 精确执行时才能整体下推。planner 始终
保证 `original ≡ pushed AND residual`；provider 不支持、旧版本或无法隔离时不是查询错误，完整谓词留在
Arrow residual。协商/编译/执行错误则使任务失败，不能退化成恒真或静默丢弃 residual。

### pcapfile 两级执行

```text
读取 record header
  → HeaderPredicate：时间、captured_len、wire_len、source_id、sequence
      definite false：跳过当前 payload/PacketRecord/Layer/Arrow 构造
  → 读取 packet bytes，并且只执行一次 Layer
  → DecodedPredicate：MAC、IP、port、TCP/UDP endpoint pair
      false：不追加到 Arrow builder
  → PacketRecord/RecordBatch → Arrow residual → downstream/DataFrame
```

格式 reader 能从 header 获得长度时应直接跳过不匹配 payload；无论是否能物理跳过，都必须避免 Layer 和 Arrow
构造。零行 batch 不是 EOF，过滤不得改变原有 EOF、错误、取消、背压和 block release 生命周期。

## 测试锚点与完成定义

- statement/parser：stage-aligned `WHERE` 原文提取、数据库原生 SQL passthrough、通用 typed literal 与普通
  function call AST；malformed expression 在执行 task 创建前失败。
- resolver/binder：`Z` 与等价 offset、0/3/6/9 位小数、无时区、超过 9 位、非法 offset、溢出和半开区间边界。
- packet rule：MAC、IPv4、IPv6、端口边界；TCP/UDP 正反方向命中、交叉端口不命中、family 混用失败、
  `ports_valid=false` 不命中、innermost endpoint 语义。
- spy `IProtocol` 证明完整离线过滤链路对 `Identify()` 零调用。
- pushed 与纯 Arrow residual 结果完全一致；provider 不支持时结果仍正确，`OR/NOT` 不发生近似下推。
- header 不匹配时不执行 Layer；decoded 不匹配时不进入 Arrow builder；规则仅在 reader 创建时编译一次。
- 两个并发 reader 使用不同规则互不污染，正常/EOF/取消/失败路径均由 provider exactly-once 释放。
- 真实 classic pcap 微秒/纳秒和 pcapng 分辨率/offset E2E 通过；时间戳回退不提前 EOF。
- Feature 完成时执行标准全量构建、完整 CTest、格式和 diff 检查，全部绿色后归档。

## 原子任务拆分

- `[x]` T1：冻结通用 filter parser、packet domain resolver、类型化规则和 source task reader 公共接口；不实现
  pcapfile 数据面。
  - `[x]` T1.1：解耦 SQL statement 与 filter expression parser；实现通用 typed literal，并锚定 stage clause、
    `tcp(...)`/`udp(...)` 普通函数 AST、数据库 passthrough 与执行前语法错误边界。
  - `[x]` T1.2：冻结 packet domain resolver、类型化 packet rule 与 source task reader 公共头文件，不接入
    provider/runtime。
  - `[x]` T1.3：增加接口 ABI、配置深拷贝和两个独占 reader fixture，保持既有接口不变。
- `[ ]` T2：实现时间与 packet domain binder/compiler 及 Arrow residual，证明所有文本只解析一次且无下推时
  结果正确。
- `[ ]` T3：实现 `IBlockStreamReaderFactoryV1` 的 Scheduler 任务隔离创建/释放链路，保持旧 source 兼容与零下推
  fallback。
- `[ ]` T4：在 pcapfile 实现 header/decoded 两级精确下推、不可变规则与并发 reader 隔离，锚定
  `Identify()` 零调用和生命周期。
- `[ ]` T5：增加真实 pcap/pcapng pushed/residual 等价 E2E，执行全量回归并完成 Feature 归档。
