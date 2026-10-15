# Feature: 原始包 Kafka 发布

标识：`kafka-packet-sink`；优先级 P2；状态：`[x]` 已完成（2026-10-16）。
前置：[连接与认证](../archive/feat-kafka-connection-auth.md)、[统一测试环境](../archive/feat-test-environment.md)、
现有 PacketFamily/Capture V2；共享格式依照 [packet source](../archive/feat-kafka-packet-source.md#共享原始包消息契约flowsql_packet_v1)。
不依赖 source 适配器先实现，可用独立消费者验收；结果 sink 的交付不等同本 Feature 已接通。

## Non-Goals

- 不管理探针部署、Topic/ACL 或集群，不提供磁盘缓冲/outbox、断点续发、Exactly-once 或采集零丢包保证。
- 不实现多分区/按 flow 路由、跨探针合并、非 Ethernet、PCAP/数据库/DataFrame 发布、Kafka 转发或 NPM 分析结果发布。
- 首版只接受完整 Capture V2 的 netadapter 来源及 SELECT *，不支持投影、WHERE、USING、多 INTO 或活动源热切换。
- 不新增认证栈，不重定义 PacketFamilySchema、采集后端、统计/丢包计数与现有 SQL 语法。

## 业务意图

需要把网卡采集数据送往集中分析端的分布式部署，可通过 `SELECT * FROM netadapter.<name> INTO kafka.<name>`
将一次采集 poll 的原始包和同序采集事实批量发布，使集中 NPM 能恢复原文与安全时间进度。
部署者可核查 Broker 确认、未发送和结果未知的包数，区分 Kafka 发送失败、背压和捕获点实际报告的丢包。

## 配置与 SQL 契约

复用连接/通道 CRUD、revision、认证和租约；仅 `payload_kind=packet`、角色 sink/both 配置
`payload_options.sink`。连接 `delivery` 与专用 Topic 已存在：

```json
{
  "name":"packet_upload", "connection_name":"delivery", "role":"sink",
  "topic":"probe_a_packets", "payload_kind":"packet",
  "payload_options":{"sink":{
    "encoding":"flowsql_packet_v1", "stream_id":"probe-a",
    "max_message_bytes":2097152, "queue_max_messages":64,
    "queue_max_bytes":8388608, "delivery_timeout_ms":10000
  }}
}
```

- encoding/stream_id 必填且遵循共享格式；默认值如示例。消息上限 1 KiB～16 MiB，包含 key 与完整 value，
  不含 Kafka 协议开销；队列条数 1～65536、字节预算 1～256 MiB 且不小于消息上限，任一先满即背压。
  delivery_timeout_ms 为100～120000；一次 Write 的校验/编码、排队、重试和确认共用绝对预算，Finish 另有一次同值预算。
- 未知/重复键、非法类型/范围拒绝；source/sink 配置分别保存，消费组仅服务 source。
  只有 packet writer 安装且配置完整才 ready；空配置为 configuration_required、未安装为 not_installed，ready 不表示写ACL已验证。

```sql
SELECT * FROM netadapter.edge_capture INTO kafka.packet_upload;
```

经现有 submit API 以 async 执行。启动前解析角色、源/目标能力与完整 SQL；不支持的组合在打开网卡或发送前拒绝，
不静默忽略子句。源的 backend/接口/snaplen 等继续由既有 netadapter 通道管理；不在 Kafka 表单重复配置网卡。
来源须提供 PacketFamilySchema、有效 CaptureSourceSetV2 和逐输入事实；一个任务独占源 reader 与 packet writer。

## 消息、路由与采集事实

- 只使用 [共享 flowsql_packet_v1 契约](../archive/feat-kafka-packet-source.md#共享原始包消息契约flowsql_packet_v1)，不复制第二套格式。
  一个 kData poll 的全部包/事实编码成一条 Kafka 消息；kTimeout 带事实时编码零包消息，无事实时不发送。
  按原采集顺序保留原文、PacketMeta、原 generation/sequence 与每输入事实；不序列化 layer/NPI 指针或调用 Identify。
- writer 从 Open 的 source set 固定观测域、输入清单和 capture_generation，生成唯一 stream_epoch；
  source_id/queue_id/观测域保持原值，source_name 作为绑定诊断供接收端配置 inputs，不从本机名称猜远端身份。
  每条实际提交的消息使用同一 stream_id key，poll_sequence 从1递增；失败后不继续创建后续消息，不换 epoch 自动续发。
- 一个观测域的全部输入只写一个预建的、cleanup.policy=delete 的单分区 Topic，实际 partition 固定0；
  启动验证唯一分区，运行发现拓扑变化即失败，禁止动态分区/分摊双向流。部署须保证同 Topic/stream_id 单发布者；
  同进程同具名 packet sink 限一个活动 writer，跨进程不提供发布者选主或分布式锁。
- Open 校验源最大 poll 可装入消息：`key_bytes + 56 + 32 × max_packets_per_batch + max_bytes_per_batch + 28 × input_count`
  不超过 max_message_bytes；不足在首包前失败。Broker 消息限额更小时发送明确失败，不切断原包、不丢事实、不拆分本 poll。
  接收端包数/字节/消息预算须覆盖发送端生效上限，使用说明给出两端匹配方式。
- 事实必须来自该 poll 的原采集端检查；不以 Kafka 确认时刻、发送端墙钟或队列暂时为空补造 idle。
  积压/未知/回拨如实传输；仅对已取得原始源事实的输入发布事实，静默输入不能凭其他输入的包提前推进。
  阻塞发送期间不获取新事实；恢复后先继续按采集顺序交付，不用延迟后的当前时间回填缺失周期。

## 冻结公共接口与生命周期

新增通用 packet sink IID，由 IQuerier 查找，具体 writer 仍属于 Kafka `.so`；不借用结果 sink 的行编码/
固定 Schema 结果语义承载采集控制事实。以下为 C++17 公共声明，省略 include/namespace；实现前落入公共头文件。

```cpp
struct PacketSinkContextV1 {
    uint32_t contract_version = 1;
    std::string target, task_id;
};
struct PacketSinkCountsV1 { uint64_t input = 0, confirmed = 0, not_sent = 0, unknown = 0; };
struct PacketSinkReportV1 {
    std::string state, code, stage, binding_json;
    PacketSinkCountsV1 messages, packets;
};
interface IPacketSinkWriterV1 {
    virtual ~IPacketSinkWriterV1() = default;
    virtual int Open(const CaptureSourceSetV2&, std::shared_ptr<arrow::Schema>) = 0;
    virtual int Write(const CapturePollEventV2&) = 0;
    virtual int Finish() = 0;
    virtual void Cancel() = 0;
    virtual PacketSinkReportV1 Report() const = 0;
};
interface IPacketSinkFactoryV1 {
    virtual ~IPacketSinkFactoryV1() = default;
    virtual int CreateWriter(const PacketSinkContextV1&, std::unique_ptr<IPacketSinkWriterV1>*) = 0;
};
```

- CreateWriter 成功冻结连接/通道 revision、凭证和目标并持有租约，失败置空；Open 一次复制完整 source set/字符串，
  校验 Schema、身份与额度。Write 仅接受 kData 或 kTimeout 的合法采集事件；EOF、cancelled、error 由执行器分流。
  每个源批次对应一次 Write，0 表示整条消息已获 delivery 成功回调；不是 enqueue 成功，也不表示下游 NPM 已处理。
- 复用 `IKafkaProducerSessionV1::Produce/Drain` 及现有会话认证/续期/取消；packet 会话启用单分区/partition0 门禁，
  不改既有 producer 虚表，不向其他 Topic 写出。Provider 内部实现 packet 特有校验，不将 Kafka 细节塞入通用采集接口。
- Open/Write/Finish 串行；Cancel 并发幂等、唤醒编码/反压/网络等待；Report 返回一致快照。
  Write 期间借用 CapturePollEventV2，返回后才可 ReleaseBlock；若底层异步持有数据，须持有独立 owner 至回调退出。
  Cancel 后等在途调用/回调退出，再销毁 writer/session/租约与源 reader；所有产物先于动态库销毁，析构不隐式 Finish/发送。

## 确认、背压、失败与可观察行为

- Producer 固定 acks=all、enable.idempotence=true，失败不降级；librdkafka 只在同一 producer 与调用预算内重试。
  业务层不重放未知消息或重建 producer 续发；Broker 确认只表示其 ISR 条件满足，不承诺消费者处理或跨系统原子性。
- 应用最多一个同步在途 poll；当前采集批次、单条编码消息和客户端队列分别有界，队列按条数/字节双限等待，
  不积累后续 poll 或复制全程抓包。报告各预算及实际队列峰值；这些预算不等于含驱动/协议/TLS 的整个进程RSS上限。
- 发送阻塞即暂停获取新包；网卡/内核仍可能丢包，使用源 ReadInputCounters 的 available_mask 与原 generation 报告
  source_dropped_packets、queue_dropped_packets、received/delivered 及背压事件，无法获取的计数为 unavailable。
  不能把传输 unknown、序号缺口或TCP重传当成已测采集丢包，不能把 Kafka 持久化表述为端到端零丢包。
- 每个已结束 Write 的消息及包计数分别满足 input=confirmed+not_sent+unknown；一条消息承载的包同获该消息结果，
  零包事实消息只增加 messages。校验失败/客户端未接受为 not_sent，已接受但无明确成功为 unknown；迟到回调不改终态。
  Write 失败时此前已确认消息保留；本次不可撤回，不自动重试。未拉取的包不计 input，采集丢包另计。
- 正常 stop 停止接收新输入，在预算内完成当前发送和 Finish/Drain，成功后为 stopped，排空失败为 failed；
  cancel 立即停止新发送/等待，不 Finish、不发送成功EOF，终态 cancelled，已确认可见、在途可未知。
  任务错误直接失败并清理，探针停止/崩溃不发送虚假 idle 或分析完成标记；接收 NPM 需自行 stop/cancel，不推断远端 EOF。
- 任务成功/失败/取消均提供脱敏 code/stage、target、冻结 revision、stream_id/epoch、观测域、输入映射、生效限额、
  消息/包分类计数与末次采集计数可用性；rows_affected 统计 confirmed packets，不能把消息数当包数。
  stage 区分 sink_resolve/open/encode/deliver/finish 与 source_capture；不回显原始包、凭证或私有文件内容。

## 主链路

1. Web/API 保存 packet sink → async SQL 解析/能力校验 → 创建私有 reader/writer并冻结源描述 →
   PollCapture → 同 poll 包/事实编码 → 队列背压/逐消息确认 → 归还批次；stop 有界排空后释放。
2. 源故障、认证/续期、Topic/ACL、编码/超限或发送不确定 → 停止拉取和发送 → 固定交付/采集摘要 →
   失败/取消并等待回调退出 → 释放全部资源；已发布数据保留，重启采用新 epoch。

## Feature Tasks（按顺序验收）

- [x] T1：交付严格 packet sink 配置与共享格式编码，使采集身份、原文和同序进度可由独立消费者无损核对。
- [x] T2：交付有界、独占且复用认证的 packet writer，使确认、背压、未知交付和资源释放可测量且可诊断。
- [x] T3：交付 netadapter→SQL INTO→Kafka 的异步链路与 Web 配置，使部署者可操作发布并区分发送失败和实际采集丢包。
- [x] T4：交付真实原生/Docker 发布、独立消费对照及部署/失败回归，经完整验收后给出可复现使用说明。

## 验收锚点

- T1：配置正反例、both 方向保留、共享黄金字节、两族/非IP/截断/最大长度原文、身份和同 poll 零包事实、额度拒绝；
  独立解码器核对格式，不以同一 codec 往返替代互操作证明；既有结构化 source/结果 sink 回归保持通过。
- T2：librdkafka mock + 假采集验证单分区/固定partition0、原文与事实顺序、单 writer 租约、队列双限/绝对预算、
  部分批次已确认/当前未知、取消唤醒/迟到回调、Finish失败和不隐式重试，Sanitizer 覆盖在途借用及动态库卸载。
- T3：生产 Scheduler 覆盖合法 SELECT */拒绝子句、缺扩展/错误Schema/额度不足先失败、空闲事实发布、stop排空/取消、
  实际网卡慢发送背压与计数可用性；前端保存/回读/编辑 packet/structured、角色与 revision/活动租约错误有断言。
- T4：统一 environment.json/dependencies.json 注册原生/Docker 和共享锁；在隔离Topic/身份下完成既有11种认证组合的发布，
  含 GSSAPI/OAuth 更新后继续发送；独立消费者逐字节/元数据/事实/顺序/数量对照，不依赖配套 source 才能通过。
  真实写ACL拒绝、缺Topic不创建、Broker超限/断连/未知交付、源故障、背压采集丢包、stop/cancel、部署重启新epoch均须验证；
  所需采集权限、隔离网卡与数据面依赖在工作台声明，不修改用户系统网络或复用业务流量，不把权限缺失计为通过。
- 配套 source 已交付时追加真实网卡→Kafka→NPM 的同负载本地对照；两项仍各自独立验收，不循环阻塞归档。
  固定负载记录 packet/s、消息/字节吞吐、确认延迟、CPU/RSS、队列峰值、采集丢包及取消耗时，区分无丢包区间和过载区间。
- 完成门槛：标准全量 CMake 构建、无筛选 `tools/test.sh all --jobs 4` 与选中/执行清单核对、前端测试/构建、
  定向 Sanitizer、格式/Diff/源码与产物摘要均通过；实施时在工作台冻结对应 target、配置来源、权限与命令。
- 使用说明维护 [docs/kafka.md](../../docs/kafka.md)，明确探针/集中端配对配置与上述限制；日志/性能原样本/摘要仅存
  `evidence/kafka-packet-sink/`，不入库；只读 doctor 不生产/消费，数据权限由本 Feature 真实数据面独立验证。

## 完成证据

- T1：公共 packet sink IID、严格配置与共享格式采集编码已完成；定向构建、格式及 Diff 检查通过。标准 unit 选中/执行/通过=5，失败/跳过/未执行=0；证据 `evidence/kafka-packet-sink/t1-unit-final.log`（未入库）。

- T2：独占 writer/租约、固定partition0、Topic门禁、绝对预算/双限、确认/未知/未发送计数、取消与释放完成。标准 unit 6/6，ASan/UBSan 2/2；动态库生命周期与迟到回调已覆盖；证据 `evidence/kafka-packet-sink/t2-unit.log`、`t2-asan-test.log`（未入库）。

- T3：生产Scheduler异步packet路径/Web配置完成；定向5/5，netadapter/调度追加2/2，前端8文件/build、格式/Diff通过。隔离AF_PACKET正常100/100且零采集丢包，慢确认过载测得源丢包59284，取消约53ms；这些为受控假producer背压样本，真实Kafka在T4验收。证据 `evidence/kafka-packet-sink/t3-capture-final.log` 等（未入库）。

- T4：原生/Docker各11身份的真实发布、GSSAPI 70秒/OAuth 35秒后继续发送、独立消费者逐字段/原文/事实/顺序核对通过；写ACL、缺Topic、Broker超限/真实断连、部分确认、取消及两个真实进程持久配置/原密钥恢复、新epoch/poll从1回归通过。
- T4：真实AF_PACKET→生产Scheduler→Kafka→packet source→NPM与本地NPM对照同批100个64字节帧，IPv4/IPv6 basic/session各50包/3200字节；确认100、采集丢包0、162条包/事实消息。探针停止不终止远端，源down正确失败于source_capture。两个NPM run分别收尾，未扩修同SQLite并发维护问题。
- T4：受控慢producer过载60000个1500字节帧、每消息约150ms确认，实测确认80、源丢包59320，取消约42ms；该样本只验证采集背压，不作为真实Kafka吞吐。真实writer成本样本及其环境/口径/限制已写入使用说明。
- 最终验收：`cmake -B build src`、`cmake --build build -j4`成功；无筛选`tools/test.sh all --config <本轮统一配置> --jobs 4`轮次`20261015T190756Z-8e9526d7`，选中/执行/通过均157，失败/跳过/未执行均0；源码与产物前后SHA256清单一致。前端8/8和生产build、定向ASan/UBSan 2/2、28个C++文件格式/版权、Diff检查通过。
- 文档与证据：README和`docs/kafka.md`已提供部署、配对预算、SQL/API、交付/采集计数、stop/cancel与重启边界。验收汇总为`evidence/kafka-packet-sink/t4-acceptance.json`；完整清单/摘要为`accepted-all/`，单例日志为`t4-accepted-{native,docker,capture,e2e}.log`，配置位置见`final-environment-location.json`，均在该本地证据目录且未入库。此前失败轮保留，未计入通过。
