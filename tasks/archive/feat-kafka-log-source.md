# Feature: Kafka 结构化数据接入

标识：`kafka-log-source`；优先级 P1；状态：`[x]` 已完成。
前置：`kafka-connection-auth`、`test-environment`（已完成）。

## 业务意图

使用外部日志、指标或业务记录的分析用户，可将具名 `kafka.<name>` source 的 JSON 消息按固定实体
Schema 转为 Arrow RecordBatch，经过现有异步分析链路；保留消息身份，处理成功后才确认位点，失败可重放。

## Non-Goals

- 不实现 packet 接入、Kafka sink、产品专用日志解析、Schema Registry、Avro/Protobuf/CSV 或自动推断。
- 不实现跨数据库/Kafka 事务、Exactly-once、有状态算子崩溃恢复、DLQ、跳过坏消息或活动配置热更新。
- 不扩展 SQL 语法，不新增认证栈，不修改用户服务、路由、身份或业务数据，不提交/推送。

## 核心数据与接口契约

连接及通道沿用现有版本、revision、租约和受控凭证。`payload_kind=structured` 的 source/both 可配置
`payload_options.source`；未配置的旧定义可保存，但 source 不就绪。sink/packet 能力仍不可用。

```json
{"encoding":"json","entity":"metrics","fields":[
  {"name":"host","path":"/host","type":"utf8","nullable":false},
  {"name":"time_ms","path":"/time","type":"int64","nullable":false,"unit":"unix_ms"},
  {"name":"value","path":"/value","type":"float64","nullable":true,"unit":"bytes"}
],"offset_reset":"earliest","commit_policy":"after_success","max_batch_rows":256,"max_message_bytes":1048576}
```

- 每条消息为一个 UTF-8 JSON 对象；拒绝重复键、非法 UTF-8、非对象、tombstone、超限及类型错误；未知字段忽略。
- fields 1～128 个，输出名唯一且不以 `__kafka_` 开头；path 为 JSON Pointer；缺失/null 仅 nullable 可接受。
- 类型为 bool/int64/uint64/float64/utf8；整数须在范围内，浮点接受有限 JSON 数值，不接受字符串或布尔强转。
- unit 为可选非空说明，写入 Arrow field metadata；时间使用 int64，并显式指定 unix_s/unix_ms/unix_us/unix_ns，
  不解析日期字符串、不换算单位；实体名写入 Schema metadata。
- 追加非空 `__kafka_topic:utf8`、`__kafka_partition:int32`、`__kafka_offset:int64`，
  及可空 `__kafka_timestamp_ms:int64`（Kafka UTC Unix 毫秒）、`__kafka_key:binary`，不承诺跨分区顺序。
- offset_reset=earliest|latest|error 只对无有效已提交位点生效；commit_policy=after_success|none，默认 after_success。
  none 不提交位点，重新启动依照组已有位点/reset 规则重放。新消费组可独立重放；不提供在线 seek。
- 默认 max_batch_rows=256（1～4096）、max_message_bytes=1 MiB（1～16 MiB）；最多一个在途批次，客户端队列有界。
- 新增 IKafkaConsumerSessionV1：StartConsumer/PollMessage/CommitOffsets；通过现有 session QueryInterface 获取，
  不暴露 rd_kafka 指针；复用同会话身份更新与取消，网络/认证/提交失败绝不解释成 EOF。
- 新增可选 IBlockStreamAcknowledgementV1::AcknowledgeBlock；只在批次处理及当次结果投递成功后调用。
  ReleaseBlock 仅释放资源；Close/Cancel/析构无隐式确认。提交每分区本批次最后 offset+1；失败非零并终止。
  确认不等于算子终态或最终数据库事务落盘；提交超时/响应丢失时服务端可能已提交，非零不代表位点必定未变。
  崩溃窗口可重复，不能据此恢复有状态分析状态。
- source 由现有 IBlockStreamFactory 只读发现，通过 IBlockStreamReaderFactoryV1 创建任务独占 reader；
  reader 持有连接/通道租约；同一进程同一 source 只允许一个活动 reader，Kafka 组协调跨进程分区。
  无过滤下推；连续 source 必须 async；停止有界，已返回 Arrow 数据生命周期独立于 reader。

## 主链路

1. Web/API 保存结构化字段配置 → 严格校验 → 只读能力与实体 Schema → Scheduler async 解析 source →
   创建独占 reader/冻结租约 → 订阅 Topic → 拉取/转换 → 算子/结果投递 → 确认位点 → 释放批次。
2. 坏消息、认证、消费、结果投递或提交失败 → 错误终态 → 取消/释放 reader；未确认位点保留，按组/reset 重放。

## Feature Tasks（按顺序验收并勾选）

- [x] T1：交付严格结构化配置与确定性 JSON→Arrow 契约，使保存、Schema 和非法消息语义可以独立验证。
- [x] T2：交付认证复用的有界 Kafka 消费及任务独占 reader，使取消、元数据与显式位点确认可靠可用。
- [x] T3：接通 Scheduler 成功确认和 Web 配置/发现，使用户可用具名 source 启动异步分析且失败不误确认。
- [x] T4：交付真实 Kafka 接入、重放与失败回归及用户说明，通过全量验收后归档 Feature。

## 验收

- T1：配置正反例、缺失/null/数值边界/UTF-8/重复键/JSON Pointer/元数据/坏消息测试与 CMake target。
- T2：真实 librdkafka mock 的多分区、超时/取消、未确认不提交、新组重放、成功提交、提交错误和在途批次约束。
- T3：公共执行器成功/失败确认回归，插件 IID/租约/持续 source async 策略及 Vue 配置保存编辑回归。
- T4：统一配置、夹具、资源锁下真实 Kafka 原生与 Docker 数据面验收（包含同组未确认重开重放）；配置缺失/认证错误非零；
  全量构建、`tools/test.sh all --jobs 4`（无筛选）、前端测试/构建、格式与 Diff/保留摘要检查。
- 证据位于 `evidence/kafka-log-source/`，不入库；不以历史结果或普通 CTest 替代真实数据面覆盖。

## 完成证据

- T1：2026-10-15，test_kafka_structured 构建通过；统一 unit 入口 1/1、零跳过；格式/Diff/既有范围保护通过。证据 t1-*.log/json。

- T2：2026-10-15，reader/structured/connection_auth 构建与 CTest 3/3、零跳过；工厂独占/租约、两分区、未确认位点不变、新组重放、明确提交/提交失败与取消通过。真实同组重开在 T4 验收。证据 t2-*。

- T3：2026-10-15，执行器/控制面/Web/Scheduler/framework 当前 CTest 5/5；前端 37/37 与构建通过；确认顺序、失败不确认、真实 Schema 规划及配置编辑验证通过。格式/Diff/既有范围保护通过。证据 t3-*。

- T4：2026-10-15，原生与 Docker 的真实结构化数据面各 11/11 身份通过；同组未确认重开重放、两分区位点、确认后继续、none、坏 JSON/tombstone/超限、未知 Topic 不创建、错误认证、取消及 GSSAPI70秒/OAuth35秒驻留后消费通过。真实异步 SQL 过滤/Schema/DataFrame/成功提交和处理失败重放、持续复制停止/取消及租约释放通过。
- 最终构建：`cmake -B build src`、`cmake --build build -j4` 返回0；前端测试37/37、零跳过，生产构建通过；修改C++的 clang-format 与 git diff --check 通过。
- 最终完整回归：`tools/test.sh all --jobs 4 --config /tmp/flowsql-test-0/acceptance/kafka-log-source/environment.json` 返回0；selected134/executed134/passed134/failed0/skipped0/not_executed0。原生与Docker既有认证矩阵各21/21、部署回归及数据库回归均包含在本轮；源码/产物验收前后摘要一致。
- 配置来源仍为统一 `config/testing/environment.json`，运行时副本仅更换 project 为 `flowsql_kafka_source_acceptance`，避开无受管身份记录的旧同名数据库卷；未重置或接管旧卷，凭证不进证据。最终清单、配置非秘密差异、构建及保护检查见 `evidence/kafka-log-source/` 和 `evidence/test-environment/20261015T053828Z-540d2aa4/`（均未入库）。
- 用户说明统一维护 `docs/kafka.md`，README已链接；T4-final-scope核对97个初始路径，非本Feature文件摘要保持、HEAD/暂存区未变。未提交/推送。
