# Feature: NPM 实时采集通道抽象

状态：`[x]` 已完成
优先级：P1
前置 Feature：`npm-packet-contract`、`npm-offline-import` 的 BlockStream/独占 reader 契约（均已完成）
后续 Feature：`npm-basic-realtime-integration`、`npm-linux-capture-backends`、`npm-dpdk-capture`

## 业务意图

让生产实时 NPM 从不同采集后端取得相同的 packet block、采集进度和可观测运行事实。使用者能以一个任务独占的 source reader 识别观测域与队列、限制批次和等待、区分临时无包与确认空闲、归还底层缓冲，并解释采集丢包和背压；后端实现与分析算法各自独立。

## Non-Goals

- 不实现 AF_PACKET、PF_RING、AF_XDP、DPDK、PCAP 文件的新 reader，也不提供实际网卡包。
- 不修改已有 `IBlockStreamChannel`、`IBlockStreamReaderFactoryV1`、`BlockPollEvent`、`PacketSchema()` 或 V1 ABI；离线 reader 行为不变。
- 不推断网络丢包、TCP 重传或应用协议；采集丢包仅是源侧可报告事实。
- 不提供多队列/多主机时间归并、跨观测点会话合并、回放或结果持久化；一个 reader 对应一个已明确的观测域和 RX 队列。
- 不把单调时钟、Poll timeout 或背压等待直接解释成 packet 事件时间水位。

## 核心数据与接口契约

新增公共版本化接口，声明于 `src/framework/interfaces/`。下列是待实现时冻结的最小形状；GUID 与结构大小在 T0 固定，不能改动 V1 布局。`ICaptureBlockStreamReaderV1` 由 `IBlockStreamReaderFactoryV1` 创建的同一任务独占 reader 实现；runner 在 Open 前对该 reader 查询此扩展，实时任务缺失时明确拒绝。`PollCapture` 与继承的 `PollBlock` 不能在同一次运行中混用。

```cpp
struct CaptureReaderLimitsV1 {
    uint32_t struct_size;
    uint32_t contract_version;
    uint32_t max_packets_per_batch;
    uint64_t max_bytes_per_batch;
    uint32_t max_wait_ms;
    uint32_t max_outstanding_batches;
};

struct CaptureQueueIdentityV1 {
    uint32_t struct_size;
    uint32_t contract_version;
    uint64_t observation_domain_id;   // 与 NPM source_domains 映射一致
    const char* source_name;
    uint32_t source_id;               // 写入现有 PacketMeta.source_id
    uint32_t queue_id;
    uint64_t generation;              // 每次重新打开独立递增的源实例
    uint32_t link_type;               // 与 PacketMeta.link_type 一致
};

enum class CaptureBacklogV1 : uint8_t { kUnknown, kEmpty, kPresent };

struct CaptureProgressV1 {
    uint32_t struct_size;
    uint32_t contract_version;
    uint32_t source_id;
    uint32_t queue_id;
    uint64_t generation;
    uint64_t fact_sequence;           // 同一 generation 内严格递增
    int64_t capture_time_ns;          // 与 packet timestamp 相同的 Unix ns 时基
    bool packet_observed;             // 本次事实包含已交付的 packet
    bool source_idle_confirmed;       // 本队列完成一次检查且没有可交付 packet
    CaptureBacklogV1 backlog;         // 当前队列及 reader 待交付缓冲的状态
};

struct CaptureCountersV1 {
    uint32_t struct_size;
    uint32_t contract_version;
    uint64_t generation;           // 计数器重置须更换 generation
    uint64_t received_packets;       // 已观察到的源包
    uint64_t delivered_packets;      // 已交付 packet
    uint64_t delivered_bytes;
    uint64_t source_dropped_packets;  // 后端可报告的源丢包
    uint64_t queue_dropped_packets;   // reader 有界队列拒绝的包
    uint64_t backpressure_events;
    uint32_t available_mask;         // 不可获得的计数置 unavailable，不伪造 0
};

struct CapturePollEventV1 {
    uint32_t struct_size;
    uint32_t contract_version;
    BlockPollEvent block;             // kData/kTimeout/kEof/kCancelled/kError
    bool has_progress;
    CaptureProgressV1 progress;
};

interface ICaptureBlockStreamReaderV1 : IBlockStreamChannel {
    virtual int Describe(CaptureQueueIdentityV1* identity, CaptureReaderLimitsV1* limits) const = 0;
    virtual CapturePollEventV1 PollCapture(int timeout_ms) = 0;
    virtual int ReadCounters(CaptureCountersV1* counters) const = 0;
};
```

`CaptureReaderLimitsV1` 在具名通道配置中于 reader 创建前冻结，由 `Describe` 报告生效值，四个上限均为正；`PollCapture` 实际等待不超过调用方 timeout 与 `max_wait_ms` 的较小值。批次同时满足包数和捕获字节上限；单包超过字节上限时明示拒绝配置或计入可见丢包，不得静默越界。`PacketSchema()`、`PacketMeta`、`source_id` 和每 reader 严格递增的 `sequence` 复用既有契约；保留原始字节、captured/wire length、时间戳、LINKTYPE，采集端只做一次 layer decode，不调用 NPI `Identify()`。

`CaptureProgressV1` 是同一次 poll 的不可变事实，data 与其事实一起交付，空 poll 可独立产生空闲事实。`capture_time_ns` 是源时间域中的保守候选进度：有包时不得超过本次已交付包的最大时间，空闲时只能在确认当前队列和 reader 缓冲已检查为空后取同域采样时间；不能用消费者的墙钟补造。`backlog=kEmpty` 必须覆盖该 reader 的内核/用户缓冲中已可见的包，未知即 `kUnknown`。事实只对 `observation_domain_id + source_id + queue_id + generation` 有效，不能证明其他队列或未来不会有迟到包。时间回拨、队列切换或未知积压不得产生可前进的事实；消费者应用自己的乱序容忍量并拒绝回退。

`ReleaseBlock` 是必调的消费完成信号，且每个 data block 恰好一次；reader 在 release 和所有共享字节 owner 消亡前不得重用其缓冲。达到 outstanding 上限时有界等待或显式背压/丢包，不得无限申请内存。`ReadCounters` 同一 generation 内累计单调；可用性掩码区分不支持与零值，计数器复位必须更换 generation。正常主动关闭产生单次 EOF；`Cancel` 唤醒阻塞 poll 并产生 cancelled，故障产生 error，三者互斥。已借出 batch 在释放后才允许销毁 reader/卸载插件。

## 主链路

1. Scheduler 为任务独占创建 reader，读取已冻结的限额与身份；`PollCapture` 交付 packet batch 及同序进度，runner 完成处理和 `ReleaseBlock` 后转交事实，再继续 poll。
2. 无包时 reader 给出 timeout 或附带空闲确认的事实；背压或积压时给出 `kPresent/kUnknown`。任务正常关闭走 EOF；取消或错误分别走异常终止，reader 等待缓冲归还后释放。

## Feature Tasks

- [x] T0：交付版本化采集 reader、身份、限额、进度和统计公共契约及可执行断言，使不同后端与上层不依赖具体设备类型即可对齐。
- [x] T1：交付独占 reader 的协商、批次借用/归还与生命周期规则，使背压、取消和插件释放不会造成无界占用或悬空缓冲。
- [x] T2：交付同序采集事实与保守进度规则，使空闲、积压、时钟回拨及队列边界可被上层安全区分。
- [x] T3：交付假采集源的契约验证套件和诊断计数，使后续 Linux/DPDK 后端能复用同一组验收锚点。

## Feature 验收

- `test_framework` 断言：版本/结构大小、缺失扩展、非法限额、身份重开 generation、packet Schema/sequence 与批次包数/字节/等待上限。
- `test_framework` 断言：data 事实同序，timeout 不等于空闲；积压/未知、回拨、跨 queue 不产生虚假前进；累计计数的 unavailable 与零值可区分。
- `test_framework` 断言：release 前缓冲不复用、outstanding 上限生效、Cancel 唤醒 poll、EOF/取消/错误互斥、reader 释放等待最后借出 batch。
- 实施时运行对应 CMake target 与定向 CTest；Feature 收口运行标准全量构建和完整 CTest，C++ 文件通过 clang-format 与 diff 检查。

## 完成证据

- T0：新增 `ICaptureBlockStreamReaderV1`、版本化结构、独立 IID、描述校验与公共断言。`cmake -B build src`、`test_framework` 编译、定向 CTest 1/1、clang-format 检查通过；未改动原 V1 ABI。
- T1：新增任务私有 `CaptureReaderStateV1`，约束描述、批次上限、等待上限、借出/归还、缓冲 owner 与 EOF/取消/错误互斥；`test_framework` 编译、定向 CTest 1/1 和格式检查通过。真实设备唤醒和槽位所有权在后端 Feature 验收。
- T2：新增 `CaptureProgressTrackerV1`，以任务独占的 source/queue/generation 与序列门禁验证 packet/空闲事实，积压/未知/回拨不推进；`test_framework` 编译、定向 CTest 1/1 和格式检查通过。
- T3：新增供确定性 reader 调用的 `VerifyCaptureReaderContractV1` 探针及假源，覆盖版本、packet/事实同序、借还、背压、统计可用性、EOF/取消/错误和缺失扩展；`CaptureReaderStateV1` 提供累计统计快照。
- 总验收：标准全量构建通过；完整 CTest 40/40 通过；最终测试文件变更后的 `test_framework` 1/1 通过。新增 C++ 文件及 `main.cpp` 新增行的 clang-format 检查、`git diff --check` 通过。生产 reader 与实时 SQL 接线分别由 `npm-linux-capture-backends`、`npm-basic-realtime-integration` 验收。
