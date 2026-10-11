# NPI 报文布局与分析地址分离

状态：方案已形成，待一轮契约对齐后实施；T1～T4 均未开始。优先级：P1。
前置：[packet 契约](../archive/feat-npm-packet-contract.md)、[NPM Basic](../archive/feat-npm-basic-analysis.md)。
关联：[Baseliner 契约修复](../archive/feat-baseliner-contract-safety.md) 独立验收当前源码；本 Feature 负责新布局完成后的网络链路部署兼容回归。

## 业务意图与 Non-Goals

为通过 NPI 解码并由 NPM Basic 分析离线/实时流量的使用者，消除任意报文偏移下的未对齐访问，
并恢复具名协议字段的可读性；交付按协议字节布局读取的报文头、按适宜对齐存储的分析地址及明确的值转换，
使地址、分层、协议识别和会话归属可在同一条生产链路中独立验收。

本 Feature 不新增协议、采集后端或分析模块，不调整会话化、TCP 重组、应用事务、标签匹配或 Baseline 算法；
不改变 Arrow/SQL 结果 Schema、配置、持久化和 checkpoint 格式；不全局修改通用对齐宏；
不把任务状态、插件接口或分层结果等内存对象压为 1 字节；不扩展为一般网络库清理，不提交/推送。
只处理现有协议头布局与直接消费者；既有 parser/对象池/Regex 释放修复保留，不再重做资源框架。

## 已核查事实与责任边界

- `FAST_MEM_ALIGN(1)` 当前是 aligned(1)，没有消除成员自然对齐；现有 GRE/TCP/IPv4 分别要求 2/4/4 字节对齐。
- 真实报文 UBSan 已在 Ethernet 后偏移 14 的 IPv4 头报告未对齐访问；字节读取草稿虽通过定向检查，但用户不接受其可读性。
- 外层 packed 不能自动解决现有地址类型的嵌套访问；返回未对齐整数引用也会再次触发 UBSan。
- NPI 负责分层/识别和转换出拥有型 packet 地址；NPM Basic 的会话键路径直接读取网络头，需要同步适配。
- packet 地址别名指向公共分析类型；IPv6 对齐会传播到 packet、会话键和预算对象，涉及共享 C++ 布局。
- ICMP 引用报文、PCAP 过滤、packet 编解码和结果投影列入直接消费者核查；仅在新类型要求时修改，不扩展模块行为。

## 核心数据契约

### 1. 报文结构体与分析地址

现有网络协议 Header 名称继续代表报文布局；所有现有报文头及其嵌套报文子结构/union 均要求 alignof=1。
采用定义附近成对的 pragma pack(push, 1)/pop；包含系统头和分析类型定义时不处于 pack 区域。
保持具名字段访问，例如 gre->S、ipv4->protocol、tcp->src_port；不把固定字段展开成散落的裸偏移/位掩码。
变长选项、截断检查和协议字节序转换仍显式处理；语义位运算不等于布局偏移，按需保留。

范围覆盖 Ethernet/VLAN、IPv4/IPv6 及现有扩展头、TCP/UDP/SCTP、GRE/VXLAN/GENEVE、
MPLS/PPPoE/PPP/L2TP/GTP/TPKT/COTP，以及相关 flags、chunk、路由项和 AVP 子结构。
协议头只包含报文整数、位域和 wire 地址，不嵌入分析地址、字符串或有资源所有权的对象。

| 对象 | 大小 | 对齐 | 存储/责任 |
| --- | --- | --- | --- |
| wire::IPv4Address | 4 | 1 | 原始四字节地址 |
| wire::IPv6Address | 16 | 1 | 原始十六字节地址 |
| wire::EthernetAddress | 6 | 1 | 原始六字节地址 |
| ::IPv4Address | 4 | 4 | 保留现有整数/字节表示与分析行为 |
| ::IPv6Address | 16 | 16，推荐 | 保留现有 uint128_t 存储，使用自然对齐 |
| ::EtherAdderss | 6 | 2 | 保留三段 uint16_t/六字节表示，名称不顺手重命名 |

冻结的转换边界如下；wire 地址是可平凡复制的纯值，不提供引用到报文整数的接口。

```cpp
namespace flowsql::wire {
struct IPv4Address { uint8_t bytes[4]; };
struct IPv6Address { uint8_t bytes[16]; };
struct EthernetAddress { uint8_t bytes[6]; };
::IPv4Address ToAnalysis(const IPv4Address& value);
::IPv6Address ToAnalysis(const IPv6Address& value);
::EtherAdderss ToAnalysis(const EthernetAddress& value);
}  // namespace flowsql::wire
```

转换返回拥有型分析对象，先形成正确对齐的目的存储，再复制原始地址字节；保持现有网络字节存储约定，
不额外 ntohl、不改变比较/哈希/字符串输出；转换结果不借用报文生命周期，不返回报文字节的整数或分析对象引用。
所有 Header 的 size/align/关键 offset 按协议字节布局冻结；IPv6 首部/扩展头和隧道字段由独立字节夹具验证。
既有正确结果保持；因填充造成的错位按协议预期修正，不要求复现错误的 sizeof/offset 或错误解析结果。
不得将压紧后的 union 最大 sizeof 当作每一种 IPv6 扩展头的实际长度，也不得用当前错误填充制作验收夹具。
当前支持编译器/端序下的位域含义以机器断言冻结；不承诺任意编译器间的位域二进制布局。

### 2. 兼容范围与已测布局代价

这是公共 C++ 类型布局迁移。Header 地址成员类型改变，IPv6 分析地址对齐改变；
IProtocol/IQuerier 的虚接口、IID 和算法职责保持，但不承诺新旧相关 .so 混用。
所有交换 packet/NPM 公共值的 FlowSQL 调用方和插件必须使用同一版本公共头重新构建并一致部署。
Arrow、结果 Schema、地址字节、会话身份与序列化内容保持，不把编译期对象内存直接当持久化格式。

当前 GCC/Linux 候选探针采用 IPv6 align=16，测得如下；这些是 sizeof，不是实际 RSS、分配器或吞吐结果。

| 对象 | 当前字节数 | 候选字节数 | 增量 |
| --- | --- | --- | --- |
| IpAddress | 20 | 32 | +12 |
| PacketLayerInfo | 196 | 240 | +44 |
| PacketRecord | 272 | 320 | +48 |
| NpmEndpoint | 24 | 48 | +24 |
| NpmSessionKey | 96 | 144 | +48 |

IPv4/MAC 的当前大小和对齐保持；IPv6 地址本身仍为 16 字节，但 variant/包含对象增加填充。
推荐 16 是为了保留 uint128_t 表示；若该内存代价不被接受，须在唯一一轮契约对齐中选择双 uint64_t 的 8 字节表示，
不能仅用 alignas(8) 强行降低 uint128_t 的自然对齐。未对齐该选择前不实施分析类型修改。
预算必须按最终实际对象大小计账；既有三万会话容量保证要复核，不能只修改容量阈值让测试通过。

## 主链路

1. PCAP/采集原始字节及 captured_len → NPI 先验长度 → 1 字节对齐的具名 Header 分层/字段读取 →
   wire 地址按值转换 → PacketLayerInfo 持有对齐的分析地址 → NPM Basic 复用分层结果进行会话/模块分析。
2. NPM Basic 从既有 layer offset 读取需要的 wire Header，复核声明长度、分片和 TCP/UDP facts →
   会话键/比较/哈希使用分析地址 → 原有结果编解码/过滤/标签事实仍消费相同地址字节。

继续使用现有长度守卫和单次解码/有限识别契约；packed 不替代捕获边界检查。

## Feature Task 与验收锚点

- [ ] **T1 交付完整报文布局与分析地址分离契约，使任意报文偏移和分析对象分别满足自己的对齐要求。**
  先对齐公共头/struct；全部现有 Header 与子结构的 size/align/offset、位域、三类地址转换有机器断言；
  明确分析对象 ABI/内存代价，字节序、比较、哈希和地址格式保持，测试夹具使用独立协议字节。
- [ ] **T2 交付 NPI 具名字段解析与识别，使报文起始偏移变化不改变合法报文的层、端点和规则命中结果。**
  替换当前字节读取草稿，保留必要边界检查；覆盖 0～15 偏移、截断前缀、已有扩展/隧道可选字段，
  独立预期验证真实 Header 字节，真实 .so 规则命中和重复加载/停止/卸载通过 Sanitizer。
- [ ] **T3 交付 NPM Basic 及直接消费者的同步适配，使地址布局迁移保持会话归属、方向与结果一致。**
  会话键读取和 ICMP 引用报文按 wire/分析边界处理；IPv4/IPv6/MAC、双向键/哈希、过滤、标签事实、
  packet 编解码、结果 Schema/内容保持；实际对象预算与容量保证验证，未受影响的模块不重写。
- [ ] **T4 交付独立生产回归与成本证据，使新布局在完整网络分析链路中可安全部署并可评估代价。**
  当前源码全量构建/完整 CTest、定向 ASan/UBSan/LSan、格式/严格编译及同版 .so 加载通过；
  固定合法报文集、实际规则命中、CPU/包数/构建选项下重复测量分层、端点转换、识别、会话处理成本与内存，
  报告适用限制，保留必要旧/新对照；完成文档、范围/hash 和任务状态审查后归档本 Feature。

顺序 T1 → T2 → T3 → T4；Atomic Slice 的允许文件、命令、时间盒和停止条件进入工作台。
四个阶段共同验收 raw packet → wire Header → 分析地址 → NPM 的单一主链路；不按文件数/时间盒拆分 Feature。

## 与 Baseliner T4 的关系

网络头布局、NPI 可读性、NPM 消费者、网络字段覆盖和网络成本由本 Feature 交付。
Baseliner 的 T1/T2/T3 状态保留；T4 仅保留加载/会话/就绪契约自身的全量回归、成本和文档出口。
按用户后续授权，Baseliner T4 独立验收当前一致源码并收口，不等待本 Feature；本 Feature 完成新布局后，负责在最终一致构建上重跑 PCAP → NPM → Baseliner 的部署兼容回归。
前轮网络修复的原始日志仍可引用，但不是新布局验收；不把历史 12/12 与未运行的新布局拼接为完成。

## 完成证据

2026-10-11：完成方案与任务归属文档；本 Feature 的 T1～T4 未实施。
当前源码调用方核查、临时 packed/地址拆分探针及候选 ABI 测量保存在 `/tmp/npi-wire-layout-plan/`
与 `/tmp/npi-pack-review/`、`/tmp/npi-address-split-review/`。本轮没有修改 C++/CMake/测试或提交/推送。
