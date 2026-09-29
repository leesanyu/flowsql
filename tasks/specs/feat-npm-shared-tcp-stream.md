# Feature: NPM 共享有界 TCP 字节流

状态：`[-]` 实施中；T0～T2 已完成，T3～T4 待实施（2026-09-29）
优先级：P1
前置：[协议模块运行时](../archive/feat-npm-protocol-analysis.md)、[通用参数](../archive/feat-npm-basic-parameters.md)、[流量标签](../archive/feat-flow-labeling.md)
后续：DNS TCP、HTTP/1、TLS 握手分析；结果持久化复用已完成的 `npm-result-query`。

## Non-Goals

- 不交付 DNS/HTTP/TLS 解析器、消息定界、事务关联、应用正文存储或 TLS 解密。
- 不模拟 TCP 端点、发送 ACK/请求重传、重组 IP 分片或推断真实网络丢包，不保证重叠选择与目标主机一致。
- 不交付实时采集、生产实时接入、多租户或 Web 配置，不改变产品定位。
- 不默认重组全部 TCP，不新增应用过滤规则体系，不通过 NPI/L7 识别结果准入，不改变 Basic/Session 指标算法。
- 不新增全局 IID、独立插件、SQL 结果实体或 `tcp_stream` feature；共享流是已启用协议模块请求的内部能力。

## 业务意图

同一 `npm.basic` 任务内的多个 TCP 协议模块需要读取相同的有序 payload，独立维护重传、重叠、乱序和缺口会
重复占用内存并产生不同解释。本 Feature 交付按会话、方向共享的被动捕获字节视图：仅为命中订阅主标签的会话
创建状态，每个消费者独立读取，缓存统一回收，缺失事实可见且所有状态受任务预算约束。
无需启用 Session 性能模块；通过确定性 TCP 报文和测试消费者独立验收，为后续协议分析提供共同输入。

## 核心契约

### 准入、身份与兼容

- 复用唯一解码、观测域绑定、会话 Observe、A/B 方向、NPI、主标签和 capture watermark；不建立第二套五元组会话表。
- Open 冻结模块的 `requires_tcp_stream=true`、TCP input mask、`requires_labeling=true` 与非空 `primary_label_ids`。
  标签必须来自同一次冻结的标签目录，不重复、不含 0；缺少 labeling、合法标签或消费接口时 Open 失败，不隐式启用模块。
- 会话首次分类得到的 `primary_label_id` 决定消费者集合，任务期间不热更新。无订阅或未命中时不创建重组缓存、游标或计时器。
  有消费者时，方向状态在该方向首次有效 TCP 输入时创建；反向从未出现不创建反向状态，也不伪造字节/方向结束事件。
- 流身份为任务内的 `(session_id, direction)`；观测域已由会话身份隔离。tuple reuse 必须先终结旧实例，再接收新实例首包。
  方向是规范化的 A→B/B→A，不代表客户端/服务端。label 0 不准入；Basic/Session 仍处理原有全量输入。
- 保留 `INpmProtocolModuleV1`、`INpmAnalysisModule` 及框架/算子 V1/V2 虚表和现有事件布局。
  新增独立内部消费接口；内部模块工厂实例增加可空、非拥有型 `tcp_stream_consumer` 指针，由同一 `protocol` 对象拥有。
  `requires_tcp_stream` 与该指针必须一致，Open 验证后才发布 runtime；不将此内部工厂结构作为跨 `.so` 扩展 ABI。
- 模块原有 `OnInput(kTcpPacket)` 继续表示 segment facts；流消费者通过新接口读取字节，UDP/control 路径不变。
  已有生产目录仍只有 Basic/Session；本 Feature 的测试模块不得进入生产 SQL catalog。

### 参数与预算

新增可省略的 `parameters.core.tcp_stream` object；配置本身不创建消费者或启用模块，省略/空对象取默认值。
沿用 V1 信封，严格拒绝重复/未知字段、错误类型、null 和越界值，错误携带 JSON Pointer；不新增顶层 `WITH` 参数。

| 字段 | 默认值与范围 | 含义 |
| --- | --- | --- |
| `max_buffered_bytes_per_direction` | 1048576（1 MiB）；65536～67108864 | 单方向共享缓存、索引、事件和游标的计费上限；与任务总预算同时满足 |
| `gap_timeout_ns` | 1000000000（1 秒）；0～60000000000 | 从首次观察到尚未补齐区间起，等待 capture watermark 推进的时长；0 仍需水位到达缺口证明时间 |

共享流配置在 Open 拥有化并冻结；以上数值是 V1 初始默认值，不是性能承诺。
`out_of_order_tolerance_ns` 仍只控制包时间戳水位，`session.max_tcp_ranges_per_direction` 仍只属于 Session 性能统计。
共享流不复用这两个参数解释 TCP sequence 乱序。

- payload 副本、容器容量、区间/截断索引、共享事件、订阅和游标均先 Reserve 后分配，统一计入 `kModuleState`；
  共享字节只计一次，消费者私有解析副本另计。任务级订阅与调度元数据同样计费；这是逻辑预算，不承诺 RSS 等于预算值。
- 跨包保留的数据必须复制到已计费缓存，不持有输入 batch owner；输入批次和输出继续分别计入 `kInputBatch`/`kPendingOutput`。
- 可回收内容先回收再申请；单方向限额、任务总额或实际分配失败均明确终止任务，不静默丢字节、驱逐慢消费者或降级成功。
  错误可区分限额类别，携带会话/方向及保留缓存的消费者身份；禁止靠缺口事件掩盖资源不足。
- 错误/取消执行原有 Abort/Cancel，回收全部流预算；正常结束必须完成消费者排空后释放。不回滚已成功输出的结果前缀。

### 新增数据与消费接口

以下为内部公共头 `npm_tcp_stream_contract.h` 的数据与接口契约；T0 已落为可编译头文件及契约断言。
现有 `NpmSessionView`、`Span` 和 emitter 直接复用；重组与消费分发在后续任务实现。

```cpp
enum class NpmTcpStreamOriginV1 : uint8_t { kUnknown, kSyn, kMidstream };
enum class NpmTcpStreamEventKindV1 : uint8_t { kData, kGap, kEnd };
enum class NpmTcpStreamGapReasonV1 : uint8_t { kWaitExpired, kTermination };
enum class NpmTcpStreamEndReasonV1 : uint8_t { kFin, kReset, kSessionEnd, kSequenceAmbiguous };
struct NpmTcpStreamConfigV1 {
    uint64_t max_buffered_bytes_per_direction = 1048576;
    int64_t gap_timeout_ns = 1000000000;
};
struct NpmTcpStreamEventV1 {
    NpmTcpStreamEventKindV1 kind;
    uint64_t begin = 0;
    uint64_t end = 0;  // 半开字节区间；End 的 begin == end。
    Span<const uint8_t> bytes;  // 仅 Data 非空，size == end - begin。
    std::optional<int64_t> captured_at_ns;  // 仅 Data：提供这些字节的包的捕获时间。
    std::optional<NpmTcpStreamGapReasonV1> gap_reason;
    bool includes_capture_truncation = false;  // 仅 Gap：至少部分缺失区间有捕获截断依据。
    std::optional<NpmTcpStreamEndReasonV1> end_reason;
    std::optional<NpmSessionEndReason> session_end_reason;  // 仅会话终结产生的 End。
};
struct NpmTcpStreamContextV1 {
    const NpmSessionView* session = nullptr;
    NpmPacketDirection direction;
    NpmTcpStreamOriginV1 origin = NpmTcpStreamOriginV1::kUnknown;
    bool capture_truncation_seen = false;  // 捕获事实；即使后续补齐也为 true。
    bool final_drain = false;
    int64_t observed_at_ns = 0;  // 本次通知的触发时间，不替代字节捕获时间。
};
interface INpmTcpStreamCursorV1 {
    virtual ~INpmTcpStreamCursorV1() = default;
    virtual bool Peek(NpmTcpStreamEventV1* event) const = 0;
    virtual int Consume(uint64_t bytes) = 0;
};
interface INpmTcpStreamConsumerV1 {
    virtual ~INpmTcpStreamConsumerV1() = default;
    virtual int OnTcpStreamReadable(const NpmTcpStreamContextV1& context,
                                   INpmTcpStreamCursorV1& cursor, INpmResultEmitterV1& emitter) = 0;
};
```

- context/session、cursor、event.bytes 只在当前同步、串行、不可重入的回调内借用；bytes 至 Consume 或回调返回即失效。
  provider 必须拥有时间通知所需的会话元数据，不保存包路径借用的 `NpmSessionView` 指针。
  复用既有 runtime 操作门；外部回调与 Cancel 的等待/资源释放规则保持一致，不在持有流容器锁时调用模块或 emitter。
- `Peek` 输出当前事件，false 表示暂不可读；不会隐式推进。Data 可 `Consume(1..bytes.size)`，部分消费后 Peek 返回后缀；
  Gap/End 仅以 `Consume(0)` 确认整个事件。无事件、Data 消费 0、越界或错误事件操作返回非零，provider 记录失败供 runtime 终止任务。
- Data 分块不等于消息边界，不保证合并为连续大 buffer；模块可复制未完成消息片段到自己的有界状态后推进游标。
  每个 Data 片段保留提供字节的捕获时间，合并不得丢失该归属；事务时间语义由协议模块定义。
- 每次输入/时间推进中有可读事件时通知消费者一次，由消费者循环 Peek/Consume；允许暂不推进，不立即反复回调忙等。
  新事件或后续 watermark 推进可再次通知。`final_drain=true` 时必须消费至 End 并确认，否则明确报消费者未排空，禁止挂住 EOF。
- 消费者集合在 Open/会话准入时固定，各有独立事件位置与 Data 偏移；共享事件内容一经可读即不可改写。
  仅所有消费者已越过的前缀可回收；一个消费者的推进不得使另一个消费者的 Peek 丢失数据。
  非零回调或 emitter 错误沿用现有任务失败语义；provider 解除订阅后才销毁模块，Abort 期间不再发正常流回调。

### 字节、缺口与终结

| 输入事实 | 统一可观察结果 |
| --- | --- |
| 起点 | 首个序号锚点含 SYN 时以 ISN+1 为字节偏移 0；SYN 可携带数据。否则以首个有声明 payload 或 FIN 的 sequence 为偏移 0，origin=midstream，未知前缀不伪造长度；纯 ACK 不确定起点。起点确定后不因晚到 SYN 回改。 |
| sequence | 按 TCP 32 位 serial arithmetic 映射到 64 位逻辑偏移，输出位置单调；支持正常 wrap，SYN/FIN 占 sequence 但不是 payload。支持窗口小于半序号空间；恰好半空间、无法唯一展开或偏移溢出时排空此前已知范围并以 SequenceAmbiguous 结束该方向，不猜测大缺口。 |
| 重传/重叠 | 同一偏移采用捕获输入顺序中的首个可见字节，重复不再次输出，重叠只补未占有部分。已发布/回收的偏移不回改；起点之前的晚到前缀裁去，不把本视图声称为端点接收结果。 |
| 乱序 | 保存不连续区间；缺口前连续前缀可读，缺口后的片段等待补齐或明确 Gap，消费者不得跨缺口当连续字节解析。 |
| 捕获截断 | 使用校验后的 wire/captured payload 长度，记录缺失尾部的已知范围；仅复制实际捕获字节，不补零。重传在提交 Gap 前可补齐，补齐后不产生 Gap；截断事实仍保留。 |
| 缺口期限 | 缺口由后续 sequence 或声明但未捕获的 payload 证明；期限为首次证明该未决缺口的捕获时间加 gap_timeout_ns，由唯一 watermark 驱动。重传不重置期限；部分补齐后剩余部分沿用期限。 |
| 缺口提交 | watermark 到期或终结时，在可证实范围内输出 Gap，再输出后续 Data。仅等待 watermark，不用墙钟；已发布 Gap 不因晚到数据回填，也不制造未知末尾缺口。 |
| FIN | 记录 FIN 所在的 payload 末端，先排出其前的 Data/Gap，再产生一次 Fin End；若前面有洞则等待补齐/期限或会话终结。半关闭不结束反向；FIN 之后的数据不作为该方向有效 payload。 |
| RST/会话终结 | RST 先处理该包可用 payload，再对已创建方向排出已知 Data/Gap/End；idle/tuple reuse/EOF 同样排空，保留既有会话 end reason。已结束方向不重复 End；新会话 ID 的流独立。 |

Gap 只说明本捕获视图缺少某段字节，不代表网络丢包或应用失败；End 只说明本方向不再提供字节，不代表完整应用消息。
不同消费者获得的 Data/Gap/End 序列在字节内容和偏移上相同，回调时机、分块读取量可以不同。
纯 ACK 不从 acknowledgment 值推断对向字节，亦不凭 ACK 释放其他消费者尚未读取的缓存。

## 主链路时序

1. **打开与输入**：解析 parameters → 冻结模块/标签计划、预算与消费接口 → 创建任务唯一共享 provider →
   唯一解码/绑定/会话分类/NPI → 模块原有 packet 回调 → 单次共享 TCP ingestion → 匹配消费者读取 → 最小游标回收。
   runtime 显式协调 ingestion/分发顺序，不能在每个模块适配器中重复喂包；SQL features 顺序不决定生命周期。
2. **时间与收尾**：capture watermark 前进 → provider 到期 Gap/可读通知 → 协议模块 OnTime；provider deadline 纳入现有维护计划。
   终结包按输入链处理 → 该会话所有剩余流 final drain → 模块 OnSessionEnd →（任务 EOF 时）各模块 Finish 一次 →
   结果消费者 Finish → 前台输出 drain。保持旧实例 end 先于新实例输入；错误/取消只 Abort/Cancel，不伪造正常 EOF。

## Feature Tasks 与测试锚点

| 任务 | 交付结果与可观察保证 | 验收锚点 |
| --- | --- | --- |
| [x] T0 | 交付可编译的流消费契约与冻结配置，让非法能力组合和参数在任务发布前失败，既有模块接口保持兼容。 | A1、A2 |
| [x] T1 | 交付单方向确定性字节视图，使重传、重叠、乱序、截断、wrap 和终结具有统一且不伪造字节的解释。 | A3～A6 |
| [x] T2 | 交付共享缓存、独立游标与统一计费，使多个消费者读取一致内容，慢消费者和预算不足可控且资源完整释放。 | A7、A8 |
| [ ] T3 | 交付标签准入与现有运行时集成，使测试协议模块经唯一主链路获得字节、时间和会话终结通知，保持原有输出语义。 | A9、A10 |
| [ ] T4 | 交付端到端回归与接入说明，证明批次边界、失败/取消和既有 Basic/Session 使用方式不破坏共享流保证。 | A11、A12；全部前序锚点 |

以下是实现时必须落成的自动断言；以任务勾选及完成证据为准，未完成任务的锚点不视为已通过。

| 锚点 | 输入与必须断言的结果 |
| --- | --- |
| A1 | 合法/缺失/不匹配 stream consumer、无 labeling、空/0/重复/不存在标签；非法计划 Open 失败且无已发布 runtime，旧 V1 测试模块仍可编译运行。 |
| A2 | 默认、上下界、越界、未知/重复键、null、字符串数字；合法配置精确冻结，失败不改原配置，core.tcp_stream 不启用模块，错误路径稳定。 |
| A3 | SYN(seq=100) 后 seq=101 的 ABC、seq=106 的 FG、seq=104 的 DE，含重复与跨 batch：字节恰为 ABCDEFG；反向独立，SYN payload、正常 wrap 与无 SYN 起点正确。 |
| A4 | ABCDE 后 seq 重叠 CxyFG：重叠保留原 CDE、补 FG，结果 ABCDEFG；同输入不同消费速度不改字节选择，Data 捕获时间对应实际字节来源。 |
| A5 | wire=5/captured=3 的 ABC 后重传 DE：期限前补齐无 Gap、截断事实为真；不补齐则 Gap[3,5)，拒绝虚构 DE；watermark 不动不超时、恰到期限提交、不重置剩余缺口期限。 |
| A6 | 缺口后 FIN、半关闭、双 FIN、RST、idle、tuple reuse、EOF、晚到前缀和 Gap 回填、半空间歧义：偏移有序、End 一次、未知前后缀无伪造；歧义方向明确终结，其他方向不受影响。 |
| A7 | 两消费者快慢交错，部分 Consume、重复 Peek、非法 Consume、终结未排空：独立游标和共享内容一致，只有最小游标前缀可回收，无忙循环或 EOF 等待。 |
| A8 | 单方向限额、任务预算耗尽、分配失败和 emitter 失败；无未计费增长/静默驱逐/伪造 Gap，已接收输出前缀语义不变，EOF/失败/取消后计费回基线。 |
| A9 | 无订阅、未命中、label 0、同标签两个消费者、不同观测域、features 换序：分别零重组状态/一次共享 ingestion/互相隔离；不启用 session 仍可消费。 |
| A10 | 终结包、旧实例复用、新实例输入、时间推进、模块 Finish/consumer Finish/Abort：记录回调序列并断言流排空先于模块会话终结，provider deadline 参与维护，取消不发送成功终态。 |
| A11 | 同一捕获输入切为不同 RecordBatch，测试消费者产出类型化实体并走现有结果 router：规范化字节/缺口/终结和结果一致，observing 只影响前台；输入 owner 可及时释放。 |
| A12 | 原有 NPM/参数/标签/结果回归与完整 CTest；共享流及 runtime 路径 ASan/UBSan 无越界、悬垂引用、溢出或泄漏；测试消费者不出现在生产 SQL catalog。 |

每个实现切片先在工作台列出允许文件、对应 CMake target、测试命令与停止条件；复用现有测试框架，不引入新依赖。
Feature 完成时运行标准 CMake 构建、完整 CTest 与相关 Sanitizer，记录环境、实际结果和不可用的外部依赖，不能以跳过代替验收。

## 完成证据

- 2026-09-29：完成规格草案及 T0～T4 拆分；尚未实现共享流、修改接口或运行新增测试。
- 完成一轮 P0/P1 设计审查：明确独立接口避免改动 V1 虚表、借用视图不可跨回调、计费先于分配、取消与回调互斥及终结排空顺序；未留有已识别的 P0/P1 阻塞项。
- 2026-09-29 T0：新增 `npm_tcp_stream_contract.h`，落地 Data/Gap/End、上下文、配置与独立 cursor/consumer 接口，未改原有 V1 虚表及事件布局。
- `parameters.core.tcp_stream` 严格解析并通过拥有型 task config 冻结到 runtime；默认值、范围、错误路径、失败不改原配置与不隐式启用模块均有断言。
- 模块实例增加非拥有型 consumer 指针，Open 发布前校验声明一致和同一 protocol 对象归属；失败中止已创建实例且保留调用方输出。
- A1/A2 通过新增的 3 组测试函数及既有计划/标签校验回归验证；合法消费接口通过实例校验，实际 provider 尚未交付，stream 计划 Open 仍明确返回 `kUnavailableCapability`，不提前启用流处理。
- 验证：`cmake --build build --target test_npm_basic test_npm_protocol_contract -j8` 成功；对应 CTest 2/2 通过、0 失败，最终一轮 0.68 秒。修改行 clang-format-18、9 个 C++ 文件版权头与 `git diff --check` 通过。
- T0 阶段未运行完整 CTest 或流路径 Sanitizer；该阶段结束时 T1～T4 尚未开始，未提交或推送。
- 2026-09-29 T1：新增 `NpmTcpStreamDirection` 单方向重组核心，通过同步事件 sink 输出 T0 的 Data/Gap/End；输入使用既有 TCP facts 和 payload，不另建会话/方向识别链。
- A3～A6 的核心语义已由 `test_npm_tcp_stream` 的 9 组测试验证：重传/重叠首字节优先、乱序补齐、捕获时间归属、截断修复/缺口、期限保持、水位推进、正常多次 wrap/半空间歧义、FIN/RST/各类会话终结及方向隔离。另有 200 组固定种子捕获序列与独立逐字节参考结果对照。
- 单方向未决状态与 payload 复制先预留预算再分配；大缺口只存区间，不按缺失字节数分配内存。限额/任务预算失败、sink 错误、Abort 和析构释放有断言；共享缓存与多消费者协调仍归 T2。
- T1 验证：标准 CMake 配置和 `test_npm_tcp_stream`、`test_npm_basic`、`test_npm_protocol_contract` 构建通过；对应 CTest 3/3 通过、0 失败，最终一轮 1.12 秒。
- 独立核心 ASan/UBSan（启用泄漏检测）CTest 1/1 通过，0.05 秒；首次执行受沙箱 ptrace 限制，获准在沙箱外重跑通过。最终构建日志无 Warning/Error，新增 C++ 文件 clang-format-18、版权头及 `git diff --check` 通过。
- T1 完成后仍未接入生产 runtime；跨 RecordBatch 的生产分发、共享消费者及完整 Feature 回归按 T2～T4 验收，未提前标记完成。未提交或推送。

- 2026-09-29 T2：新增 `NpmTcpStreamSharedDirection`、固定订阅和独立消费游标；一个事件/payload 由全部消费者共享，重复 Peek/部分 Consume 不影响其他消费者。只有全体确认的事件前缀回收，部分读取保留原分配容量并继续计费。
- 重组核心通过内部 `EmitOwned` 转交 payload 所有权和已有计费，不再复制共享事件字节；原借用 sink 兼容。方向预算统一覆盖核心未决区间、共享缓存、固定对象和订阅/游标，同时扣减任务 `kModuleState`；对象/数组/节点/payload 均先 Reserve 后分配。
- 输入或实际水位推进最多通知每个可读消费者一次；非法 Consume、非零消费者回调、emitter 错误锁存，即使调用方忽略返回值也失败。final drain 必须确认 End，未排空明确报错；重入/回调内 Abort 等回调返回后清理，不释放在用游标。
- 失败报告区分方向限额/任务预算/分配/消费者/emitter/未排空，带 session_id、方向、首个保留最早缓存的消费者及失败消费者；诊断名借用冻结目录，生命周期契约已写入内部头。无订阅不创建组件。
- A7/A8 已由新增 `test_npm_tcp_stream_shared` 的 7 组测试验证：快慢交错、重复 Peek、部分消费、缺口和 FIN/EOF 排空、非法消费、未排空、两级预算、单份 payload、两方向共享任务预算隔离、实际 new/new[] 失败遍历（含已转交前缀后的失败）、emitter 成功前缀保留及 Abort/析构清理。所有释放路径计费回基线。
- T2 标准 CMake 配置和四个相关目标构建成功（生产 NPM 插件同步编译）；`test_npm_basic`、`test_npm_protocol_contract`、`test_npm_tcp_stream`、`test_npm_tcp_stream_shared` CTest 4/4 通过，0 失败，0.79 秒。
- 单方向核心/共享方向 ASan+UBSan（detect_leaks=1）CTest 2/2 通过，0 失败，0.07 秒；沙箱 ptrace 阻止 LeakSanitizer 后获准沙箱外重跑。普通/消毒器构建无 Warning/Error，五个 C++ 文件格式/版权、diff 和允许文件基线检查通过。
- 首轮任务预算测试同时越过两个限额，优先报方向限额与断言不符；已调整测试输入只耗尽任务预算，两类错误均独立验证。未改变限额优先级来迎合测试。
- T2 完成后没有启用生产 stream provider；T3 承接标签准入、会话元数据/任务级调度计费及既有 Cancel 操作门集成，T4 承接完整 CTest 和跨批次结果链路。未提交或推送，保留全部前序未提交改动。
