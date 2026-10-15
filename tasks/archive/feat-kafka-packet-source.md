# Feature: Kafka 原始包接入与 NPM 分析

标识：`kafka-packet-source`；优先级 P2；状态：`[x]` 已完成，T1～T4 已按顺序验收。
前置：[连接与认证](../archive/feat-kafka-connection-auth.md)、[统一测试环境](../archive/feat-test-environment.md)、
现有 PacketFamily、Capture V2 与 NPM 实时托管输出；配套：原始包发布（`kafka-packet-sink`，独立待实施 Feature）。
本 Feature 拥有下述共享消息契约；可用独立测试生产者验收，不以配套 sink 完成为前提。

## Non-Goals

- 不从日志、五元组或统计指标恢复包，不自动识别 PCAP 文件、JSON、Avro/Protobuf 或其他原始包格式。
- 不实现多分区时间归并、跨探针观测域合并、按 flow 分区、消费组透明迁移、分析状态检查点或 Exactly-once。
- 不实现在线 seek、坏消息跳过/DLQ、自动去重、源配置热更新、Topic/ACL 管理或 NPM 结果 Kafka 直发。
- 首版不接入非 Ethernet LINKTYPE，不扩展 SQL 语法，不修改 NPM 会话、协议识别和结果表契约。

## 业务意图

通过 Kafka 汇聚远端探针流量的 NPM 用户，可把具名 `kafka.<name>` packet source 作为持续采集来源，
保留原始包、观测域和采集进度，经现有 `npm.basic` 输出会话、协议与性能事实到数据库托管目标。
用户能区分消息消费、位点确认和分析状态，重启后创建新的分析 run，明确识别重放与结果不连续的边界。

## 配置与 SQL 契约

复用 Kafka 连接/通道 CRUD、revision、租约和认证；仅 `payload_kind=packet`、角色 source/both 配置
`payload_options.source`。以下是已存在连接 `collector` 的非秘密通道示例：

```json
{
  "name":"remote_packets", "connection_name":"collector", "role":"source",
  "topic":"probe_a_packets", "consumer_group":"npm_probe_a", "payload_kind":"packet",
  "payload_options":{"source":{
    "encoding":"flowsql_packet_v1", "stream_id":"probe-a", "observation_domain_id":"7",
    "inputs":[{"source_id":0,"queue_id":0,"source_name":"probe-a/eth0"}],
    "offset_reset":"earliest", "commit_policy":"after_success",
    "max_packets_per_batch":256, "max_bytes_per_batch":1048576,
    "max_message_bytes":2097152, "max_wait_ms":100
  }}
}
```

- encoding、stream_id、observation_domain_id、inputs 必填；stream_id 匹配 `[A-Za-z0-9_.-]{1,64}`，
  观测域为 uint64 非零十进制字符串；inputs 1～32 项，source_id/queue_id 为 uint32，source_id 唯一，
  source_name 为 1～128 字节非空 UTF-8 且无 NUL。清单来自探针实际 Capture V2 描述，消息的观测域及输入编号须匹配；
  source_name 仅作本地诊断标签，不写入消息，也不用于替代数字身份。
- 默认值如示例；包数范围 1～4096，原始字节预算 1 KiB～16 MiB，消息上限 1 KiB～16 MiB，
  max_wait_ms 为 1～1000；消息上限包含 key 与完整 value，解码后的批次同时满足包数/原始字节预算。
  最多一个 outstanding 批次，超限整条失败，不拆包。
- offset_reset=earliest/latest/error，仅对没有有效已提交位点生效；commit_policy=none/after_success，默认 after_success。
  未知/重复键、错误类型和越界值拒绝；旧空配置可保存但为 configuration_required，未安装适配器为 not_installed。
  both 独立校验并保留两个方向；ready 不代表读 ACL 已验证，配置发现不连接 Broker 或消费数据。

```sql
SELECT * FROM kafka.remote_packets
USING npm.basic
WITH features='basic,session'
INTO mysql.npm;
```

通道、NPM 插件和目标数据库须已配置；经现有 submit API 以 async 提交。持续 NPM 仅接受既有两段数据库托管目标，
不将普通 DataFrame 或 `INTO kafka.*` 当作全实体持续结果出口。协议模块仍按现有 features/标签规则启用。
源阶段 WHERE 复用 PacketFamily 的精确 residual filter，首版不做过滤下推；不得删列破坏 NPM 输入 Schema。

## 共享原始包消息契约：flowsql_packet_v1

本节是 source/sink 和外部探针的唯一格式定义；消息承载一次有界采集 poll 的全部 packet 与同序 progress，
允许零包、有事实的空闲消息；两者都为零时不发送。Kafka key 是 stream_id 的原始 UTF-8 字节，不允许 null。
value 为二进制，不是 JSON/Base64、C++ 内存镜像或 Arrow IPC；全部整数按大端逐字段编码，有符号数为二补码。

| 区段 | 严格字段顺序与含义 |
| --- | --- |
| 固定头，56 字节 | magic 为四字节 ASCII `FSPK`；version:uint16=1；reserved:uint16=0；stream_epoch:16字节非零 UUID；observation_domain_id:uint64；capture_generation:uint64；poll_sequence:uint64；packet_count:uint32；fact_count:uint32。 |
| packet，32 字节头及原文 | source_id:uint32；timestamp_ns:int64；captured_len:uint32；wire_len:uint32；link_type:uint32；sequence:uint64；随后恰好 captured_len 个原始字节。按原采集顺序排列。 |
| progress，每项28字节 | source_id:uint32；queue_id:uint32；fact_sequence:uint64；capture_time_ns:int64；flags:uint8（bit0=packet_observed，bit1=source_idle_confirmed）；backlog:uint8（0=unknown，1=empty，2=present）；reserved:uint16=0。 |

- 总长度恰为 `56 + Σ(32 + captured_len) + 28 × fact_count`；包数最多4096、事实最多32。
  先核验长度、计数、溢出和配置预算，再分配输出；拒绝截断、尾随字节、未知版本/标志、null value/tombstone 和错误 key。
- timestamp_ns/capture_time_ns 是原采集端非负 UTC Unix ns，不使用 Kafka timestamp、接收墙钟或单调时钟替换。
  link_type 固定1（Ethernet）；captured_len 等于原文字节数，wire_len 不小于 captured_len，截断按长度如实保留。
- capture_generation、poll_sequence、packet sequence 和 fact_sequence 均非零；一个 writer 的 stream_epoch 唯一且重开更换，
  capture_generation 来自冻结的 CaptureSourceSetV2。poll_sequence 从1递增，每条加1；packet sequence 保留原 reader 全局顺序，
  可有采集/过滤缺口；每输入的 fact_sequence 严格递增。消费起点可在中段，首条建立基准，后续重复、回退或 poll 缺口报错。
- 包/事实来源必须在配置清单中，事实 source_id 不重复且 queue_id 匹配。packet_observed 与本消息对应输入存在包一致，
  为真时 capture_time_ns 不超过该输入本消息最大包时间，且不得同时 idle；无事实的输入保持 unknown/原有受阻状态。
  原始报文内部 malformed/truncated 由现有 layer status 表达，不因报文协议无法解码而跳过合法信封。
- 同一 stream_id/Topic 只部署一个活动发布者，整个观测域的全部输入共用一个预建、仅按保留期删除（cleanup.policy=delete）的
  单分区 Topic；源只接受 partition=0。禁止把双向会话按方向或输入队列分散到不同消费实例。
  Topic 保留期限制重放范围；采集缺包和时钟误差仍受原捕获点能力约束，不从顺序完好推断网络零丢包。
- 独立分析使用不同消费组；同组仅一个分析实例获得该分区。初次分配后撤销、丢失或重新分配即终止旧任务，
  不自动接回并沿用内存会话；发现 Topic 分区数变化也报错。Provider 对 packet 会话添加分配/元数据门禁，不改变结构化 source 行为。

## Reader、确认与 NPM 时间契约

- 复用 `IBlockStreamReaderFactoryV1` 创建任务独占 reader，由 `IQuerier` 获取 Provider；同进程同 source 限一个 reader。
  reader 实现 `ICaptureBlockStreamReaderV2`、`IBlockStreamSchemaV1` 和 `IBlockStreamAcknowledgementV1`；
  Kafka 会话沿用 `IKafkaConsumerSessionV1`，通过现有 session QueryInterface 获取，不暴露 librdkafka 类型或修改既有虚表。
- DescribeSources 在消费前由冻结配置给出全部输入、共同本地 generation、单一观测域及限额，run_mode 自动为 realtime；
  NPM 自动绑定来源，显式 source_domains/run_mode 冲突拒绝。第一条消息绑定远端 epoch/generation，后续变化报错，不能静默重绑。
- 一条消息转换为一个 PacketFamilySchema RecordBatch；原字节/PacketMeta 无损保留，NPI layer decode 在接收端一次完成，
  Identify 仍由 NPM 按采样策略执行，不传输或信任探针缓存的层偏移。不得追加 Kafka 列改变 PacketFamilySchema；
  Topic、partition、消息 offset、epoch、poll_sequence 与批次的对应关系由 reader 的有界确认状态保存并进入脱敏诊断。
- PollCapture 的 data 与 progress 同次返回；零包事实消息也返回 kData 和零行规范 Schema 批次，使事实处理与位点确认经过同一路径。
  普通超时为 kTimeout 且无事实。runner 先处理包、接受本批事实并完成当次输出，再确认，最后 ReleaseBlock；失败不确认。
  源过滤后的空批也保留进度/确认身份，不可丢弃控制消息或把下一批位点越过未处理批次。
- 远端 progress 仅在对应包处理后转为本地 generation 的采集事实；保留每输入事实序号与源时间。
  只接受远端明确确认的 empty/idle，不能用 Kafka lag=0、partition EOF、Poll 超时或消费者墙钟制造空闲水位。
  远端积压/未知、任一输入尚无事实或时间回拨时，共同候选暂停；沿用 CaptureProgressTrackerV2 的安全最小值和 NPM 乱序容忍。
  Kafka 传输延迟只延后事实到达，后续未处理消息不能由本地时间提前封闭周期；单调定时维护继续运行。
- after_success 仅由 AcknowledgeBlock 提交本条 offset+1；none 验证当前批次但不提交。ReleaseBlock、Close、Cancel、析构均不提交。
  提交超时/响应丢失可已在服务端生效，按不确定失败终止；提交不等于最终 Flush 或所有 NPM 状态/数据库事务已持久化。
- 每次重启产生新的 NPM run_id 和空会话状态；沿已提交位点继续会丢失此前内存上下文，不能拼接旧 session/revision。
  after_success 遇新 epoch 不确认该消息，重启可从该边界创建新 run；诊断记录本 run 实际起点、末次确认及提交不确定状态。
  完整重算单一发布段需保留期内的段首数据及新消费组/无有效提交位点；none 本身不清除组内旧位点，latest 不保证完整会话。
  重放可重复分析，同一原包不跨 run 自动去重；none/earliest 回放遇 epoch 边界仍失败，不自动跳过旧段，历史段独立重算应隔离 Topic。
- Kafka 分区末尾/无数据永不视为正常 EOF；stop 停止拉取，成功收尾后标 stopped；cancel/error 不成功 Flush、不制造 EOF。
  ReadInputCounters 仅报告本地实际消费/交付计数及可用性，远端采集丢包不可获知时为 unavailable，不能填零。
  网络/认证/续期、ACL、坏消息、拓扑/epoch、提交及输出失败均非零并标明阶段；诊断不包含原包内容或秘密。
- 有界 fetch/解码预算须覆盖一个当前消息、一个输出批次和有限客户端预取；已返回 Arrow 数据独立持有原文字节所有权。
  Cancel 可并发唤醒等待；借出批次归还、在途调用及回调结束后才释放 session/租约/reader，产物先于插件库销毁。

## 主链路

1. Web/API 保存 packet source → 只读发现 Schema/方向能力 → async 创建 reader/冻结配置 → 单分区分配 →
   解码包与同序事实 → npm.basic/托管输出 → 按策略确认 → 归还批次；所有模块复用现有 NPM 主路径。
2. 坏消息、输出/提交失败、所有权或 epoch 变化 → 失败并保留最后确认诊断 → 清理资源；用户重启后建立新 run，按组位点重放/续读。

## Feature Tasks（按顺序验收）

- [x] T1：交付严格 packet source 配置、共享二进制编解码及黄金样本，使外部探针和配套 sink 可按同一无损格式互操作。
- [x] T2：交付有界独占 Capture V2 reader 与位点/所有权门禁，使原包、进度、重放和异常生命周期具备可执行保证。
- [x] T3：交付 Kafka→NPM 的实时托管主链路与 Web 配置/能力发现，使会话和周期结果按远端观测域与安全时间产生。
- [x] T4：交付真实 Kafka、NPM 对照、部署/失败回归及用户说明，以完整验收证明接入和重启连续性边界。

## 验收锚点

- T1：GTest 覆盖配置双方向保留、全范围整数/纳秒、黄金字节序与长度、IPv4/IPv6/非IP/截断原文往返、坏格式与预算拒绝；
  独立实现的生产者生成固定样本，避免只用同一 codec 自编自解；合法协议坏包仍产生原包与 layer status。
- T2：librdkafka mock/假采集事实验证消费中段、严格顺序、单分区/首次分配/撤销失效、epoch 变化、队列预算、超时非EOF、
  零行事实确认、after_success/none、处理/进度/输出失败不提交、提交不确定、取消唤醒及 Arrow/插件卸载顺序，定向 Sanitizer。
- T3：生产 Scheduler + npm.basic 路径对照同一确定性本地采集序列，覆盖 basic/session 及配置规则的 dns/http1/tls/icmp，
  核对两族会话/协议/性能事实（忽略新 run/session 身份）；双输入双向会话关联、15/30/60秒周期、延迟/积压/回拨/空闲、
  过滤后空批、不完整尾部和 stop/cancel 均断言；Web 测试确认 packet/structured、source/sink/both、revision/租约不互相覆盖。
- T4：统一 environment.json/dependencies.json、隔离 Topic/组/身份与资源锁下执行原生/Docker 数据面，覆盖既有11种身份组合、
  GSSAPI/OAuth 更新后的继续消费、真实读ACL拒绝、缺Topic不创建、坏消息、重放/续读、新 run、不确定提交与重启部署；
  分区归属变化和远端重开不得延续旧状态；所需用例零环境跳过，不能用已完成认证或结构化接入结果代替。
- 完成门槛：标准 CMake 全量构建、无筛选 `tools/test.sh all --jobs 4`、选中/实际执行清单核对、前端测试/构建、
  格式/Diff/源码与产物摘要均通过；具体 targets/依赖/权限/命令在各实施切片工作台冻结。
  固定负载记录 packet/s、消息/字节吞吐、Kafka积压、CPU/RSS、端到端交付延迟及取消耗时，不承诺未经测量的生产容量。
- 用户说明维护 [docs/kafka.md](../../docs/kafka.md)，NPM 说明链接入口；本地日志/样本/摘要置 `evidence/kafka-packet-source/`，不入库。

## 完成证据

- T1（2026-10-15）：严格 packet 配置、共享大端 codec、独立 Python 黄金样本已通过；定向 selected/executed/passed=4，failed/skipped/not_executed=0。日志 evidence/kafka-packet-source/t1-*.log（未入库）。reader/NPM 尚待后续验收。

- T2（2026-10-15）：Capture V2 独占 reader、严格流/序列门禁、单分区首次分配/撤销失效、显式提交及不确定诊断已通过；稳定定向 4/4（20261015T130022Z-c6d56445），零失败/跳过，ASan/UBSan 无报告。沙箱 socket 拒绝及产物变化的失败轮次保留，不计入通过。

- T3（2026-10-15）：生产 Scheduler/NPM + SQLite 对照两族12实体非空结果，15/30/60秒周期、双输入安全最小时间、积压/未知/回拨暂停、过滤空批、stop不完整尾部/cancel不完整run、来源配置冲突均通过；标准回归10/10、零跳过、摘要稳定（20261015T133449Z-55008225）。前端8个测试文件、packet Vue双向编辑/revision保留及生产构建通过，格式/Diff检查通过；本地 evidence/kafka-packet-source/t3-* 未入库。

- T4（2026-10-16 Asia/Shanghai）：原生与Docker各11种真实身份均完成packet消费/确认；GSSAPI/OAuth更新后继续读取、读ACL拒绝、缺Topic不创建、坏消息、none重放/同组续读、真实提交失败不确定终止、分区增长/归属失效、epoch边界、独立进程重启新run/空会话及部署配置持久化均通过。固定256包/消息、64消息的成本样本及限制已写入docs/kafka.md；不表述为生产容量。
- 最终验收：标准全量构建、无筛选all（20261015T160222Z-c6564005）selected/executed/passed=148，failed/skipped/not_executed=0，清单一致且源码/产物前后摘要稳定；前端8个测试文件及build、ASan/UBSan、22个C++文件格式和Diff审查通过。最终配置从默认配置派生UUID隔离项目，未复用所有权不明的旧卷；所有子测试共用同一运行时配置。
- 全量发现并同步Scheduler旧时序回归：只读SQLite核验归还前已持久化3条完整非最终周期；精确断言cancel保留这3条并标incomplete，stop补尾部且与EOF一致。定向4/4及最终all均通过；旧断言失败轮（20261015T154457Z-c7252829）和环境准备失败轮保留，不计通过。完整日志/样本/摘要见本地evidence/kafka-packet-source/与evidence/test-environment/，未入库；未commit/push。
