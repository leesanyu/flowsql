# Feature: 分析结果 Kafka 发布

标识：`kafka-result-sink`；优先级 P1；状态：`[x]` 已完成（2026-10-15）。
前置：[Kafka 连接与认证](../archive/feat-kafka-connection-auth.md)、[统一测试环境](../archive/feat-test-environment.md)（均已完成）。

## Non-Goals

- 不实现 packet sink、NPM 全部启用实体的直接持续发布、数据库 CDC、Topic/ACL 管理或任意 Topic 动态路由。
- 不实现跨 Kafka/数据库/算子状态事务、Exactly-once、持久 outbox、断点续发、业务级自动重试或服务端去重。
- 不新增 SQL 语法、多 INTO、隐式 DataFrame 副本、Schema Registry、Avro/Protobuf、压缩策略配置或任意 librdkafka 参数透传。
- 不扩展算子专有的托管、多 Schema、模型/结果双出口与恢复协议；不能走下述通用出口的算子先显式物化后发布。

## 业务意图与范围

需要向 SIEM、告警系统和其他订阅者交付分析结果的用户，可将数据库查询、DataFrame 或可对接的算子输出
通过单一 `INTO kafka.<name>` 发布为带实体、Schema 版本、稳定结果身份和适用 revision 的 JSON 消息。
任务成功表示本次应发布的全部消息已收到 Kafka 确认；失败、取消和响应不确定时保留可核查的发布摘要。

当前只有认证会话和结构化 source，sink 方向尚未安装；本 Feature 新增独立结果 writer，不把发布伪装成
具有替换语义的 `IDataFrameChannel::Write`，也不把 NPM 实体/表名规则放入通用框架或 Kafka Provider。

| 输入 | 首版交付与边界 |
| --- | --- |
| 数据库查询 | 读取一个明确关系的查询结果，按 reader 的 Arrow Schema/批次发布；不提供持续轮询、变更捕获或增量水位。 |
| DataFrame | 读取任务启动时的非破坏性快照；发布所选行，不清空、改写或消费原 DataFrame。 |
| 可对接算子 | 接收普通 DataFrame 结果或公共 block-transform 输出回调中的单一固定 Schema；不具备该出口能力的组合在读取输入前拒绝。 |

NPM/基线等专有结果可先写入已有数据库关系或 DataFrame，再走本 Feature；不据此宣称全部实体已直接接通。
支持的 block-transform 组合保留既有有限/持续与 async 策略；持续任务可 stop/cancel，但不会因安装 sink 自动扩展来源能力。

## 配置与 SQL 契约

复用现有连接和通道 CRUD、revision、租约与凭证。仅 `payload_kind=structured`、角色 sink/both 可配置
`payload_options.sink`；both 的 source/sink 分别校验并同时保留，消费组只服务 source。
旧空 sink 定义可保存，能力为 `configuration_required`；packet sink 仍为 `not_installed`。
只有安装 writer 且 sink 配置完整时 `sink_adapter_available=true`；ready 表示能力就绪，不代表写 ACL 已验证。

以下为 `channels/add` 的非秘密示例，连接 `delivery` 已存在：

```json
{
  "name": "alerts", "connection_name": "delivery", "role": "sink",
  "topic": "analysis_alerts", "payload_kind": "structured",
  "payload_options": {"sink": {
    "encoding": "json", "entity": "alert", "schema_version": 1,
    "record_kind": "upsert", "key_fields": ["tenant", "alert_id"], "revision_field": "revision",
    "max_message_bytes": 1048576, "queue_max_messages": 1024,
    "queue_max_bytes": 8388608, "delivery_timeout_ms": 10000
  }}
}
```

- entity 为 `[a-z][a-z0-9_]{0,63}`，schema_version 为 1～2147483647，均必填；encoding 仅 json，未知/重复配置键拒绝。
- record_kind 必填 event/upsert；key_fields 为无重复列名的有序数组，至多 8 列；upsert 要求非空且提供 revision_field。
  upsert 的 key_fields 不得包含 revision_field；event 可省略/空 key_fields，且不得配置 revision_field。
  字段是否存在、类型、NULL 和完整输出 Schema 在 writer Open/Write 校验，不能因 revision 变化而改变结果身份。
- 默认消息上限 1 MiB（1 KiB～16 MiB），包括 UTF-8 key 与完整 value，不含 Kafka 协议开销；key 最多 4096 字节。
  队列默认 1024 条（1～65536）、8 MiB（1～256 MiB，且不少于消息上限）；任一上限先达到即反压。
- delivery_timeout_ms 默认 10000（100～120000）；一次 Write 内的编码、排队、发送/重试与确认共享绝对截止时间，
  不按消息或内部重试重置；Finish 使用同一配置的一次独立预算。Broker 限额更小时明确失败，不拆分单条业务记录。

示例表/快照具有 tenant:utf8、alert_id:utf8、revision:uint64、severity:int32；引用的通道和关系须已存在：

```sql
SELECT * FROM mysql.analytics.alert_results WHERE severity >= 3 INTO kafka.alerts;
SELECT * FROM dataframe.alert_results INTO kafka.alerts;
```

沿用任务执行 API 的 `sql_text` 与 mode；有限任务可 sync/async，持续来源/算子仍必须 async。
SQL 投影/过滤按既有执行链路作用于实际发布结果；配置引用的 key/revision 列被投影删除时在发送前失败。
数据库保留既有查询语义，DataFrame/算子只接受现有执行器支持的语法；不静默忽略子句以凑成可执行组合。
`kafka.<name>` 作为 sink 单独解析并创建任务私有 writer，不进入数据库两段目标解析，也不复用 source reader。
source-only 返回 `KAFKA_ROLE_MISMATCH`；能力未安装/未配置或算子出口不支持时，在产生数据副作用前拒绝。

## 消息、Schema 与身份

每个 Arrow 业务行对应一条非 tombstone 消息；无单独 Schema 消息、批次控制消息或结束消息，空结果发布零条。
value 为 UTF-8 JSON 对象，包含下表全部字段；业务列只放在 data 内，避免与信封字段冲突。

| 字段 | 固定语义 |
| --- | --- |
| envelope_version | 整数 1；与业务 Schema 版本、通道配置 revision 独立。 |
| entity / schema_version | 来自冻结的 sink 配置；声明业务实体及版本，不从通道 revision 或数据类型猜测。 |
| schema / schema_fingerprint | 完整字段顺序、名称、Arrow 逻辑类型、nullability、字段/Schema metadata；指纹见下文。 |
| publication_id / task_id | 每次 writer 创建生成唯一 publication_id，task_id 标识本次执行；重新执行产生新的 publication_id。 |
| record_kind / result_id / revision | event/upsert、类型化身份元组、可空十进制字符串；event 的 revision 固定为 null。 |
| data | 该行全部选中业务字段及显式 null；不注入/修改业务 revision、时间或单位。 |

- 支持 bool、int8/16/32/64、uint8/16/32/64、float32/64、utf8、binary、fixed_size_binary、timestamp。
  int64/uint64 为十进制字符串，其他整数为 JSON number；浮点只接受有限值；binary 为标准带 padding 的 Base64。
  timestamp 为原始有符号 int64 十进制字符串，Schema 显式保留 s/ms/us/ns 与 timezone；不猜测时区或换算精度。
  UTF-8、字段唯一性、nullability 必须真实有效；decimal、nested、dictionary 及其他类型在发送前拒绝，不自动降精度/强转。
- schema 为 `{fields:[{name,type,nullable,metadata}],metadata}`；type 为上述逻辑名称，timestamp 另含 unit/timezone，
  fixed_size_binary 另含 byte_width。metadata 为 UTF-8 字符串映射，重复键拒绝；对象键按 UTF-8 字节序排序，
  fields 保持 Arrow 顺序，紧凑 JSON/标准字符串转义/非 ASCII 原文编码后的 SHA256 为 schema_fingerprint。
  一个 writer 只接受一个指纹，后续批次漂移在该批发送前失败；业务 Schema 改变须显式增加 schema_version，
  本 Feature 不建立跨任务注册中心来强制全局同版本一致性，接收方可依据随消息携带的指纹检测冲突。
- key_fields 仅支持非空 utf8、整数和 binary；result_id 为配置顺序的 `{type,value}` 数组，值按上述无损规则编码。
  无 key_fields 的 event 使用 `{type:"publication",value:publication_id}` 与从 0 起的 uint64 行序号构成身份；
  因此重执行不能凭此身份自动去重。显式业务 key 可供下游识别同一事件/结果，但不保证上游生成唯一业务值。
- Kafka key 是紧凑 JSON 数组 `[entity,schema_version,result_id]` 的 UTF-8 字节，嵌套对象键使用上述规范排序。
  不含 publication_id（无业务 key 的 event 除外）、task_id、revision 或指纹，确保同版本同身份的更新具有相同 key。
- upsert 的 revision_field 必须为非空整数且值在 1～UINT64_MAX，信封中归一为十进制字符串；每条是完整替换快照。
  不生成 revision、不按全量 key 缓存去重/判定单调性；接收方按稳定 key/revision 处理重复与乱序，不能把 upsert 视为增量累加。
  event 表示独立事件，upsert 表示同结果的新版本；二者均发布普通消息，不把 Kafka key 等同 broker 去重或业务更新。

## 冻结公共接口与生命周期

新增通用 `IID_RESULT_SINK_FACTORY_V1` 由 IQuerier 查找；具体实现仍在 `libflowsql_kafka.so`。
以下为 C++17 契约声明（省略 include/namespace）；实现前形成公共头文件，不改既有通道、算子与 Kafka session 虚表。

```cpp
struct ResultSinkContextV1 { uint32_t contract_version = 1; std::string target, task_id; };
struct ResultSinkReportV1 {
    std::string publication_id, state, code, stage, binding_json;
    uint64_t input_rows = 0, confirmed_rows = 0, not_sent_rows = 0, unknown_rows = 0;
};
interface IResultSinkWriterV1 {
    virtual ~IResultSinkWriterV1() = default;
    virtual int Open(std::shared_ptr<arrow::Schema> schema) = 0;
    virtual int Write(const std::shared_ptr<arrow::RecordBatch>& batch) = 0;
    virtual int Finish() = 0;
    virtual void Cancel() = 0;
    virtual ResultSinkReportV1 Report() const = 0;
};
interface IResultSinkFactoryV1 {
    virtual ~IResultSinkFactoryV1() = default;
    virtual int CreateWriter(const ResultSinkContextV1&, std::unique_ptr<IResultSinkWriterV1>*) = 0;
};
struct KafkaProduceMessageV1 { std::string key, value; };
struct KafkaDeliveryV1 { uint64_t confirmed = 0, not_sent = 0, unknown = 0; };
interface IKafkaProducerSessionV1 {
    virtual ~IKafkaProducerSessionV1() = default;
    virtual int Produce(const std::vector<KafkaProduceMessageV1>&, uint32_t timeout_ms, KafkaDeliveryV1*) = 0;
    virtual int Drain(uint32_t timeout_ms) = 0;
};
```

- CreateWriter 失败置空；成功冻结通道/连接 revision、sink 配置和凭证并持有租约。target 非本实现支持的类别返回 ENOTSUP。
  任务独占 writer/producer；不同任务可使用同一具名 sink，拥有独立会话，不共享队列或 publication_id。
- Open 在首次数据前恰好一次校验完整 Schema；Write 同步确认当前批次，0 仅表示该批每条均已收到 delivery 成功回调，
  enqueue/本地 flush 返回不作为成功证据。Produce 通过现有 session QueryInterface 的新 IID 获取，复用认证、续期与取消；
  其消息数组只在调用中借用，冻结的 sink 配置限制单次数组和内部队列，不能向其他 Topic 发消息或暴露 rd_kafka 指针。
- Open/Write/Finish 串行，Cancel 可并发、幂等并唤醒网络/反压等待；Report 可并发读取一致快照。
  writer 与会话、回调及 Arrow 产物必须先于插件库销毁；取消后先等待在途调用退出，再释放租约/会话。
  writer 析构只清理，不隐式 Finish、继续发送或将取消改为成功；任何失败/取消后的 writer 不得再次 Write。
- Report.state 为 created/open/writing/finished/failed/cancelled，仅描述 writer；任务是否 completed/stopped 另按上游终态判定。
  binding_json 为适配器的非秘密快照；Kafka 包含 target、channel_revision、connection_revision、schema_fingerprint，
  通用调度层原样附入结果，不据此解释业务实体。Produce 每次返回均满足 confirmed + not_sent + unknown = 输入消息数。

## 交付、反压与终态

- Producer 固定 `acks=all`、`enable.idempotence=true`，由 librdkafka 在单会话及截止时间内处理可重试传输错误，
  不回退弱确认、不在创建失败时关闭幂等；幂等所需权限不足明确失败。业务层不重放失败批次或重建 producer 重试。
  确认是 broker 的 ISR 确认，不承诺消费者已处理、跨系统原子性或 Kafka 集群超出自身配置的持久性。
- 分区由非空 key 经 librdkafka 内置一致 key 分区器选择；仅同一 producer、同一分区内保持提交顺序。
  分区器固定 murmur2；跨任务不排序，分区数变更可能改变相同 key 的路由；下游仍须按 revision 处理。
  Topic 必须预先存在，关闭自动创建；生产只验证元数据，不创建 Topic 或发送独立权限探测消息。
- 仅保留调用方当前 Arrow 批次；逐行编码成受条数/字节双限的窗口，队列满时等待 delivery/取消并计入同一预算，
  不丢消息、不无限扩容、不编码整份数据库结果或额外复制完整 DataFrame。编码窗口与客户端队列分别受队列预算约束，
  两者持有的 key/value 字节合计不超过两倍字节预算加一条编码消息；协议/TLS、当前上游批次/快照与驱动内存另行核算，
  不把 payload 预算表述为整个进程 RSS 上限。
- Write 非零可已有部分确认，既有消息不撤回；传输前校验失败或未被客户端接受计 not_sent，
  已接受但未取得明确成功的消息保守计 unknown（包括部分 delivery 错误）。迟到确认不能改写已返回的终态报告。
  已结束 Write 的 input_rows = confirmed_rows + not_sent_rows + unknown_rows；未读取/尚未调用 Write 的行不计入。
  input_rows 在进入 Write 时计整批行数，在途差额为 pending；终态差额必须归零，unknown 不得伪报确定未写入。
- 若来源具有显式批次确认能力，仅当前输出 Write 全部成功后才允许确认当前源批次；发送失败/未知不确认该批。
  源位点与 Kafka 发布仍非原子，确认前崩溃可重复，既有批次确认也不代表后续算子最终输出/状态已持久化。
- 只有上游正常结束、算子最终 Flush 成功且 writer Finish/Drain 成功，任务才 completed；空结果也校验 Schema，
  不发布探测消息、不把零条完成当成写 ACL 已验证。rows_affected 只统计 confirmed_rows，失败响应同样附发布摘要。
- stop 停止接收新输入，按既有算子正常收尾后在预算内排空，保留 stopped 终态；排空失败必须 failed。
  cancel 立即中断等待并停止新发送，不调用成功 Flush/Finish，终态为 cancelled；已确认可见、在途结果可能未知，
  取消/错误不发送 EOF 或完成消息。认证、续期、Topic/ACL、编码、消息超限、确认超时均非正常结束。
- 成功/失败/取消均暴露 target、publication_id、通道/连接 revision、Schema 指纹、上述计数和脱敏 code/stage；
  stage 固定为 sink_resolve、sink_open、sink_encode、sink_deliver、sink_finish，不能以原始消息/凭证充当错误详情。

## 主链路

1. Web/API 保存 sink 配置 → SQL 解析唯一目标/检查出口能力 → 创建任务私有 writer/冻结租约 → Open Schema →
   查询/快照/算子批次 → 编码窗口/反压/逐条确认 → 上游正常终结与算子 Flush → Finish/Drain → 发布成功摘要并释放资源。
2. 校验/认证/发送错误、超时或取消 → 停止新输入与发送 → 唤醒并等在途调用退出 → 固定确认/未发/未知摘要 →
   失败或取消终态 → 释放回调、会话与租约；保留已发布消息，不自动重试或回滚。

## Feature Tasks（按顺序验收）

- [x] T1：交付严格 sink 配置、无损 JSON 信封和确定性身份/Schema 契约，使事件、更新与非法结果可独立验证。
- [x] T2：交付认证复用的任务私有 producer/writer，使队列、确认、取消、部分发布和资源生命周期有界且可核查。
- [x] T3：交付单一 INTO 的查询、快照与兼容算子出口，使正常完成和 stop/cancel 都准确反映实际 Kafka 交付。
- [x] T4：交付 Web sink 配置和按方向的能力发现，使 source/sink/both 编辑、revision 与秘密处理可安全复用。
- [x] T5：交付真实认证/数据面/部署回归和统一用户说明，经完整验收证明发布、重执行、失败及源数据保护边界。

## 验收锚点

- T1：GTest/单测断言配置正反例、双方向保留、整数全域/NULL/UTF-8/Base64/时间单位/时区、指纹稳定与漂移、
  key 稳定及版本隔离、event 生成身份、upsert revision/完整替换、超限/不支持类型/投影丢 key；错误不得静默降精度。
- T2：真实 librdkafka mock 验证队列双限、反压绝对预算、逐条 delivery、部分确认/结果未知、超时不重发、取消唤醒、
  Finish 失败、租约冻结/并发 writer 隔离及回调/动态库销毁顺序；以 Sanitizer 检查在途取消及卸载生命周期。
- T3：生产 Scheduler 路径覆盖数据库/空结果/DataFrame/至少一个真实公共算子出口；核对过滤/投影、Schema 和源内容不变，
  不支持组合启动前失败；正常结束/stop 排空、取消不 Finish、最终 Flush 失败及 rows_affected/错误摘要均有断言；
  带确认来源的成功顺序、发送失败/未知不确认和确认前重放不得遗漏。
- T4：前端测试与构建覆盖保存/回读/编辑、双方向不互相覆盖、配置不完整、活动租约及 revision 冲突；
  不把 ready/元数据验证当写 ACL 成功，隐藏方向及秘密不进入请求/日志；既有 source 回归保持通过。
- T5：在统一 environment.json/dependencies.json 中注册原生与 Docker 发布测试及资源锁；从实际执行位置预检，
  覆盖既有 11 种协议/身份组合的真实发布，由独立校验消费者核对 key/value/数量；含 GSSAPI/OAuth 驻留更新后的继续发布。
  隔离夹具负责预建 Topic/组及读写身份；只读 doctor 不发布/消费。真实写 ACL 拒绝、错误认证、缺 Topic 不创建、
  超限/断连/部分发布、相同业务 key/revision 重执行可重复、取消/stop 与原生/Docker 部署恢复必须分别验收且零环境跳过。
- 完成门槛：标准入口全量配置/构建、无筛选 `tools/test.sh all --jobs 4`（核对 selected/executed/passed 等清单）、
  前端测试/构建、定向 Sanitizer、格式/Diff/源码与产物摘要检查均通过；成本记录固定负载下消息/字节吞吐、CPU/内存与取消耗时，
  不设未经测量的生产容量承诺。实施时确定各 target/命令及配置非秘密版本，冻结在对应工作台。
- 用户说明继续维护 [docs/kafka.md](../../docs/kafka.md) 并由 README 引用；原始日志/样本/摘要仅存 `evidence/kafka-result-sink/`，不入库。

## 完成证据

- T1：配置/无损编码/key/指纹契约已实现；本轮构建通过，定向测试 2/2、零跳过，证据 `evidence/kafka-result-sink/t1-*.log` 与 `evidence/test-environment/20261015T064922Z-55c3419a/`（本地未入库）。
- T2：任务私有认证 producer/writer、delivery 计数/预算/取消/租约已接通；构建通过，定向测试 3/3（20261015T065735Z-0f12bcc1），ASan/UBSan 含泄漏检测通过。沙箱 loopback 拒绝及夹具缺运行目录的失败保留，修正后重新执行通过；日志 `evidence/kafka-result-sink/t2-*`（未入库）。
- T3：生产 Scheduler 数据库分批/空结果、只读 DataFrame 过滤投影、builtin.passthrough 与公共 block-transform 输出、确认后 ack/失败重放、Flush 失败、stop/cancel 及摘要已接通；构建与 3/3 定向回归通过，审查后 1/1 复验通过（20261015T071118Z-ca5f53d2、20261015T071245Z-1ecca364）。测试请求字段/返回码错误修正后重新执行；证据 `evidence/kafka-result-sink/t3-*`（未入库）。
- T4：Web sink 配置/双方向编辑、能力发现及 revision/租约回读已交付；前端 39/39 零跳过、Vite 构建通过（现有 bundle 大小提示保留）；后端 2/2 初验和补充 both/revision/租约断言后的复验通过；证据 `evidence/kafka-result-sink/t4-*`（未入库）。
- T5：2026-10-15 重启后恢复匹配的受管私有凭证/夹具状态，未改账号或重置卷；原生/Docker 只读 doctor 各26项通过。当前完整构建退出0，无筛选 `tools/test.sh all --jobs 4`（20261015T095158Z-f3769946）selected/executed/passed 均140，failed/skipped/not_executed 均0，执行前后及最终复核的源码/产物摘要一致。
- 本轮两位置真实认证各21项、source/sink 各11种身份和部署恢复均通过；独立消费者核对发布内容/数量、重复、权限拒绝、错误认证、缺Topic不创建、超限/部分发布、GSSAPI/OAuth更新和真实受控断连。断连两位置均保留 confirmed=3/unknown=3，不误报完整成功。前端39/39及build、重新构建的codec/writer/control ASan/UBSan/leak、11个新增C++文件格式及Diff空白检查通过。
- 完成索引 `evidence/kafka-result-sink/t5-final-acceptance.json`，完整清单与源码/产物摘要 `evidence/test-environment/20261015T095158Z-f3769946/`，本轮两位置固定负载成本包含在完成索引；仅为未固定CPU亲和性的本地短样本，不承诺生产容量。先前失败/恢复日志保留，所有证据均本地未入库；用户说明为 `docs/kafka.md`，未提交/推送。
