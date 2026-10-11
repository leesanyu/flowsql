# NPI 报文头布局与安全解析

状态：T1/T2/T3/T4 已完成并归档（2026-10-11）。优先级：P1。
前置：[packet 契约](../archive/feat-npm-packet-contract.md)。
后续：[NPM 按地址族分析与存储](../specs/feat-npm-ip-family-pipeline.md)。

## 业务意图与 Non-Goals

为通过 NPI 和 NPM 直接读取网络报文的调用方，消除任意报文偏移下的未对齐访问并恢复具名字段可读性；
交付符合协议字节布局的报文结构体、明确的地址值转换和经过真实报文验证的分层/识别结果。

本 Feature 不调整分析对象的 IPv4/IPv6 存储组织、会话容器、结果 Schema 或数据库关系；这些归后续 Feature。
不新增协议、采集后端或分析算法，不改通用对齐宏，不把有所有权的运行时对象压为 1 字节。
产品尚未发布，不设计旧 ABI、旧头文件或新旧实现并行支持，不创建兼容包装和迁移工具。
既有池/parser/Regex 生命周期修复作为工作树起点保留；不扩展为资源框架重写。

## 独立边界与核查依据

- `FAST_MEM_ALIGN(1)` 当前是 aligned(1)，不能消除成员自然对齐；Ethernet 后偏移 14 的 IPv4 访问已有 UBSan 证据。
- 外层 packed 不自动消除分析地址内部对齐要求；返回未对齐整数引用仍会破坏访问契约。
- NPI 的分层、协议引擎、packet decoder，以及 NPM 会话键、ICMP 引用报文和 PCAP 过滤中直接读 Header 的路径归本 Feature。
- 分族分析可以尚未实施，本 Feature 仍可用独立协议字节夹具和实际 NPI 解码/识别验收，具有可复用的安全解析价值。
- 本次将旧规格中“提高 IPv6 分析对齐及承担容器膨胀”的方案移交后续 Feature；不先把全部现有 variant 改大。

## 核心数据契约

现有协议 Header 名称代表报文布局；所有报文头及嵌套报文子结构/union 的 alignof 必须为 1。
在定义附近成对使用 pragma pack(push, 1)/pop；系统头、分析类型和普通运行时对象不在 pack 区域中。
Header 只包含协议整数、位域和 wire 地址，不嵌入字符串、分析地址或资源所有权对象。
保留 gre->S、ipv4->protocol、tcp->src_port 等具名字段；长度检查、变长选项和字节序转换显式处理。

| 报文地址类型 | sizeof | alignof | 责任 |
| --- | --- | --- | --- |
| wire::IPv4Address | 4 | 1 | 四字节网络地址 |
| wire::IPv6Address | 16 | 1 | 十六字节网络地址 |
| wire::EthernetAddress | 6 | 1 | 六字节以太网地址 |

wire 地址可平凡复制，不返回报文字节的整数引用；转换到分析地址时先形成合法目的对象，再按值复制 4/16/6 字节。
IPv4、IPv6 和 MAC 是三种独立转换；其最终分析存储对齐由后续 Feature 统一交付。
转换保持网络地址字节含义，不能把主机端序的整数内存直接当跨平台协议或持久化编码。

覆盖 Ethernet/VLAN、IPv4/IPv6 及现有扩展头、TCP/UDP/SCTP、GRE/VXLAN/GENEVE、
MPLS/PPPoE/PPP/L2TP/GTP/TPKT/COTP，以及相应 flags、chunk、路由项和 AVP。
每种头按协议定义冻结 size/align/关键 offset；变长 IPv6 扩展头不采用 union 最大 sizeof 作为实际长度。
位域在当前支持编译器与端序下用独立协议夹具验证；packed 不替代捕获长度和协议声明长度校验。
Header 经长度检查后直接 reinterpret_cast 为 const Header*，指向原始报文，不构造局部 Header 或整头 memcpy。
取头辅助函数只返回报文中的指针；指针及字段引用不得超过输入缓冲区生命周期。
该读取方式按项目当前 C++17、packed 与 -fno-strict-aliasing 工具链约定验证；不宣称任意编译器均可移植。
0～15 偏移、截断前缀及指针与输入地址一致性必须有机器断言。

## 主链路

1. 原始字节和 captured_len → 验证头长度 → 1 字节对齐的具名 Header → 分层路径与协议识别。
2. 已验证的 Header 地址字段 → 按值转换到调用方的具体 IPv4/IPv6/MAC 地址 → 直接消费者取得正确事实。

## Feature Task 与验收锚点

- [x] **T1 交付全部现有报文头和 wire 地址契约，使解析结构体在任意起始偏移下满足协议布局。**
  冻结公共头；size/align/offset、位域和三种地址转换有机器断言；夹具来自协议字节，不使用旧结构体填充构造。
- [x] **T2 交付 NPI 具名字段分层与识别，使合法报文在 0～15 起始偏移下得到相同解析和规则命中。**
  替换散落的固定字段裸偏移草稿；覆盖截断前缀、现有 IPv6 扩展及隧道可选字段、真实 .so 与真实规则。
- [x] **T3 交付全部直接读头消费者的安全访问，使 NPM、ICMP 和离线过滤取得符合协议的地址与传输事实。**
  覆盖会话键构建、引用报文、PCAP 筛选及其他检索确认的直接消费者；不在此任务重设计会话或结果存储。
- [x] **T4 交付完整解析回归和成本证据，使安全报文读取成为后续分族分析的已验收基础。**
  标准全量构建/完整 CTest、定向 ASan/UBSan/LSan、格式/严格编译通过；固定样本及 CPU 重复测量解析成本；
  实际规则命中和重复加载/停止/卸载通过，完成范围和文档审查后归档。

顺序 T1 → T2 → T3 → T4；实现步骤、允许文件和具体验收命令在每次 Atomic Slice 工作台冻结。
验收入口复用 packet contract、NPI alignment、NPM 会话/ICMP/过滤和真实生产集成测试，必要新增独立协议夹具。

## 与后续 Feature 及 Baseliner 的关系

本 Feature 交付 wire 安全；后续 Feature 统一拥有分析地址对齐、packet 分族存储、NPM 分族处理、结果和数据库契约。
[Baseliner 契约安全](../archive/feat-baseliner-contract-safety.md) 已完成归档，不重新打开其 T4。
后续新表作为 Baseliner 的新输入来源，由分族 Feature 更新示例和集成用例；不进行旧表兼容或重写基线算法。

## 完成证据

2026-10-11：根据用户要求重划方案和任务归属；本 Feature 实现任务均未验收。
既有偏移/释放修复日志仅作为历史问题依据，不作为本次 wire 契约的完成证明。
规划证据位于 `/tmp/npi-ip-family-feature-plan/`；本轮未修改生产源码、CMake、测试或运行数据库。

T1：39 类 wire/Header 布局断言、独立字节位域/地址转换及 0～15 偏移/截断夹具通过；标准 CMake target/CTest 通过。
直接 ASan/UBSan/LSan 通过；沙箱 ptrace 的 LSan 退出阻碍经同程序沙箱外复跑消除，未关闭检查器。
原始证据 `/tmp/npi-wire-layout-implementation/T1-*`；分析地址布局未改。

前次 T2（整头复制路线，已被新读取契约取代，不作为重新验收证据）：NPI layer/engine/decoder 已恢复具名 Header 字段，固定字段从已构造对象读取；修复 IPv6 扩展、VXLAN/GPE、
GRE 路由截断、L2TP/PPP/MPLS/GTP 变长定位。独立路径/地址/端口断言、0～15 偏移及全部截断前缀通过；
真实 HTTP 规则命中与三轮 .so 加载/停止/卸载通过。标准构建与 3/3 定向 CTest 通过，证据 `T2-build-v3/T2-test`。

前次 T3（整头复制路线，已被新读取契约取代，不作为重新验收证据）：NPM session/control 与 ICMP 具名 Header 读取及显式地址转换完成；会话和 ICMP 的 0～15 偏移、
引用地址/端口/MTU、引用截断断言通过。NPM Basic/ICMP/PCAP 导入 3 项通过；配置通道 E2E 在隔离网络命名空间通过。
主机最小 loopback connect 超时是外部环境阻碍；未修改主机网络，证据 `T3-build-regressions/T3-test/T3-e2e-isolated`。

T2 重新验收：固定头直接 reinterpret_cast 为 const Header*；变长头/decoder 的 View/HeaderAt 仅检查长度并返回输入指针。
布局夹具验证输入指针一致性；保留具名位域、截断与变长协议边界。当前标准构建和 3/3 定向 CTest 通过，
证据 `T2-direct-build/T2-direct-test`；整头复制 API 随 T3 消费者迁移后删除。

T3 重新验收：NPM session/control、ICMP outer/quote/port 全部使用 const Header*，整头 Read API 已删除。
当前标准 target 构建通过；NPM Basic/ICMP/PCAP 3/3 定向 CTest 与隔离配置通道 E2E 通过。
证据 `T3-direct-build/T3-direct-test/T3-direct-e2e`；保留 wire 到有所有权分析地址的明确值转换。

T4 历史阻塞检查点（2026-10-11，已被下述最终验收解除）：标准配置和最终全构建通过，12 个严格语法检查和修改代码格式/版权审查通过。
当前完整 CTest 83/84 通过，耗时 1220.15 秒；唯一失败为 test_baseliner_recovery 的 900.04 秒超时。
定向 ASan/UBSan/LSan 7/7 全部通过，耗时 434.78 秒；detect_leaks=1，含真实生产 NPM 四后端集成。
PCAP IPv6 packed 地址数组的 std::begin/end 会生成不同临时数组，已改为源 bytes 指针固定长度复制；
现有 IPv6 过滤用例在 ASan 下通过。协议 Header 读取仍是直接指针，不构造或复制整头。

临时 shell TCP 转发遗留事务已精确清理；最终 HTTP 使用独立网络命名空间，数据库经 Unix socket 连接主机 IPv6。
最终普通生产集成通过；恢复测试链接项不含 NPI/NPM，56 个相关 Baseliner/数据库源码保持本轮起点哈希。
该超时原因尚未完全定位，按跨任务隔离规则记录；不扩大本 Feature 到 Baseliner 恢复修复。

固定 CPU=0，相同合法协议样本与真实 HTTP 规则，五轮交替、每阶段 2000000 包，中位数：
分层 83.39→93.00 ns/包（+11.53%）；decoder 128.23→127.16 ns/包（-0.84%）；Identify 84.06→89.01 ns/包（+5.90%）。
两版各阶段 checksum 相同；driver/decoder 使用 C++17 -O2 -fno-strict-aliasing，NPI .so 使用相同标准构建条件。
成本包含长度检查及协议布局纠正；分层增加约 9.61 ns/包，不宣称性能无变化。
PacketLayerInfo sizeof=196、PacketRecord sizeof=272，改动前后一致；分析对象布局及分族 Schema 未实施。

该历史检查点尚未达到完整 CTest 全绿的 DoD，当时不勾选、不归档；最终状态以下述验收为准。
当前证据 `/tmp/npi-wire-layout-implementation/`：`T4-full-ctest-isolated.log/T4-full-tests-isolated.xml`、
`T4-san-ctest.log/T4-san-tests.xml`、`T4-final-cost.json/T4-cost-results.json`、
`T4-final-format.json/T4-final-strict-results.json/T4-independent-recovery-evidence.json`。


T4 最终验收（2026-10-11）：Baseliner 恢复测试超时已按独立修复完成，未放宽 TIMEOUT=900；
标准全构建通过，正式完整 CTest 84/84、零失败/错误/跳过，总耗时232.57秒，recovery45.66秒、production69.71秒。
最新报告 `/tmp/baseliner-recovery-fix/full.xml/full-ctest.log`；修复范围与额外存储/恢复Sanitizer2/2在
`/tmp/baseliner-recovery-fix/verification.json`，不将该修复混为本Feature实现。

最终核对15个wire/NPI/NPM/PCAP源码和测试文件与`T4-final-audit.json`哈希一致；
此前直接指针实现的定向ASan/UBSan/LSan7/7、12个严格编译、格式/版权和五轮固定CPU成本证据继续对应当前代码。
最新完整CTest也再次覆盖wire/packet/NPI alignment、NPM/ICMP、PCAP及真实生产链路；
真实HTTP规则与重复.so装卸断言通过。实现仍是align1 Header直接强转，未恢复整头复制。

最终核对与归档证据 `/tmp/npi-wire-layout-closeout/verification.json`。
T4完整目标与DoD已满足，T1～T4全部勾选，Feature移入archive、Backlog标记完成；
后续分族Feature仍待实施，不自动启动。原有其他工作树改动保持，未提交或推送。
