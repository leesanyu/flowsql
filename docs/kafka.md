# Kafka 连接、结构化/原始包接入与通道管理

FlowSQL 提供可复用的 Kafka 连接配置与具名通道管理。连接负责 broker、TLS/SASL 与凭证生命周期；通道引用连接，并指定 `source`、`sink` 或 `both`、Topic、消费组和数据类别。

结构化 JSON source 可将外部日志、指标或业务记录转换为固定 Arrow Schema，供异步 SQL 分析使用；packet source 保留探针原始包和采集进度，供 NPM 实时分析。连接验证只证明认证和元数据访问；消费权限在实际任务中检查。结构化 sink 可将分析结果发布为逐行 JSON 信封；packet sink 尚未安装，返回 `KAFKA_DATA_ADAPTER_UNAVAILABLE`。source 只能用于 FROM、sink 只能用于 INTO，角色不符返回 `KAFKA_ROLE_MISMATCH`。

## 原生安装与启用

当前验收平台为 Ubuntu 24.04、Linux x86_64。固定客户端为 librdkafka 2.16.0，采用私有 PIC 静态链接；OpenSSL、libcurl、Cyrus SASL 与 MIT Kerberos 使用系统共享库。开发依赖和共享缓存由 `thirdparts/librdkafka/` 管理。

```bash
sudo apt-get install libssl-dev libcurl4-openssl-dev libsasl2-dev \
  libsasl2-modules-gssapi-mit libkrb5-dev krb5-user zlib1g-dev liblz4-dev libzstd-dev
cmake -B build src
cmake --build build -j4
```

缓存位于 Git 主工作区的 `.thirdparts_prefix/librdkafka` 与 `.thirdparts_installed/librdkafka`。指纹覆盖源码校验、私有 GSSAPI 补丁、依赖清单、构建脚本、平台、编译器及系统组件版本；匹配才复用，不同 worktree 对同一指纹的编译通过文件锁串行。`-DFLOWSQL_KAFKA_OFFLINE=ON` 禁止下载，必须有匹配安装缓存或已校验的源码包。源码校验和必需特性检查失败均终止，认证不降级。

运行环境需要 `libssl3t64`、`libcurl4t64`、`libsasl2-2`、`libsasl2-modules-gssapi-mit`、`libgssapi-krb5-2`、`libkrb5-3`、`krb5-user`、`ca-certificates`、`zlib1g`、`liblz4-1`、`libzstd1`，以及已有插件的运行包。检查共享库不能替代 SASL 插件、票据工具和实际认证检查。

从 `build/output` 运行时，先准备部署方自己的密钥与目录：

```bash
mkdir -p meta secrets runtime/kafka config/kafka
chmod 700 secrets runtime/kafka
openssl rand -out secrets/kafka-master.key 32
chmod 600 secrets/kafka-master.key
cp ../../config/kafka/provider.example.json config/kafka/provider.json
```

首次部署才生成密钥，已存在数据库必须继续使用原密钥；更换或丢失密钥会使加密凭证无法恢复。将密钥备份到部署方的秘密管理系统，不放入 Git、镜像或 SQLite。连接定义、通道和加密凭证在同一 SQLite 事务中保存，数据库目录必须持久化。

Provider 配置文件示例：

```json
{
  "db_path": "./meta/kafka.db",
  "master_key_file": "./secrets/kafka-master.key",
  "runtime_directory": "./runtime/kafka",
  "files": {
    "corp_ca": "./secrets/ca.pem",
    "client_certificate": "./secrets/client.pem",
    "client_key": "./secrets/client.key",
    "service_keytab": "./secrets/service.keytab"
  },
  "environments": {
    "broker_password": "FLOWSQL_KAFKA_PASSWORD"
  }
}
```

路径由部署方控制，均相对于服务工作目录；Web 只选择 `file:corp_ca`、`env:broker_password` 等别名，不接受任意路径、JAAS、客户端参数或 shell 命令。不要给多个进程配置同一个数据库；同一 Scheduler Provider 是唯一配置真相。

在 Scheduler 插件列表加入：

```yaml
- name: libflowsql_kafka.so
  option: "config_file=config/kafka/provider.json"
```

`config/deploy-single.yaml` 和 `config/deploy-multi.yaml` 提供注释入口；准备文件后启用。所有插件按 Option、Load、Start 三个批次加载。`config_file=` 也可用于命令行插件参数，避免 JSON 中的逗号被插件列表分隔器拆开。主密钥或数据库无法加载时启动失败；单个 broker 离线不会阻止合法定义保存。

## Docker 挂载

仓库 Dockerfile 安装相同的认证运行组件，并携带 `share/kafka/dependencies.json`、`build-info.json` 与 `LICENSES.txt`；源码、开发包和缓存留在构建环境。构建前完成原生全量编译和前端构建。

```bash
docker build -t flowsql:latest .
mkdir -p secrets
chmod 700 secrets
openssl rand -out secrets/kafka-master.key 32
chmod 600 secrets/kafka-master.key
cp config/kafka/provider.docker.example.json config/kafka/provider.json
cp config/kafka/deploy-scheduler.example.yaml config/kafka/deploy-scheduler.yaml
docker compose -f docker-compose.yml -f config/docker-compose-kafka.yml up -d
```

该覆盖文件保留现有 Scheduler 插件集合，并增加 Kafka Provider。实际部署按已有配置调整示例，保留已有通道/插件。`config/kafka` 与 `secrets` 只读挂载；`kafka-state` 持久卷保存数据库；`kafka-runtime` 为私有票据的临时目录。修改 Provider 的 `files`、`environments` 映射后重启 Scheduler。映射的文件必须为普通文件，拒绝符号链接；秘密文件限 64 KiB、证书/keytab 限 1 MiB。keytab、证书和密码文件由部署方挂载到 `secrets`；生产 Kafka、KDC 和身份服务由部署方提供。

## Web 操作

打开“通道管理 → Kafka 通道”，可在“通道”和“连接配置”两个页签操作：

1. 新建连接，填写名称、一个或多个 `host:port` 和安全协议，按机制填写字段。密码、私钥口令和 OAuth client_secret 支持输入新值或选择部署引用。
2. 新建通道，选择连接、角色、单个 Topic 和数据类别。source/both 必须填写消费组；sink 不提交消费组。选择 structured 后可分别启用 source 字段映射与 sink 结果信封；both 两个方向独立配置。sink 需指定实体、Schema 版本、event/upsert、业务 key 与适用的 revision 列。通道创建时也能就地创建连接。
3. 点击“验证”检查已保存配置，或在表单中“验证草稿”。保存不要求网络可用，也不要求事先验证。验证预算为 1～30 秒；可取消，页面离开会请求取消。
4. 查看独立状态：“配置已保存”、连接验证时间/阶段、每个方向的数据适配器状态。复制引用得到 `kafka.<name>`。
5. 编辑/删除携带当前 revision。冲突返回 409 并保留草稿，刷新后重新编辑；系统不会自动覆盖他人的修改。活动任务或验证期间不能改删；连接被通道引用时不能删除。

秘密默认保留、不回显，遮罩文本不能替换密码；替换需显式选择新值/引用，必需秘密不能清除。切换协议/机制只提交当前字段。未保存或失败的草稿只保存在页面内存，离开时清理，不进入 localStorage、URL 或浏览器日志。

## 支持的认证

| 协议/机制 | 配置与边界 |
| --- | --- |
| PLAINTEXT | 明文 Kafka，无 TLS 和 SASL 字段。 |
| SSL | 强制服务端证书链和主机名校验；CA 可使用系统信任或受控文件；客户端证书和私钥成对选择以启用 mTLS。 |
| SASL_PLAINTEXT / SASL_SSL | 可选 PLAIN、SCRAM-SHA-256、SCRAM-SHA-512、GSSAPI 或 OAUTHBEARER；TLS 字段只在 SASL_SSL 下提交。 |
| PLAIN / SCRAM | username 与 password。密码可加密保存或引用受控文件/环境变量。 |
| GSSAPI | principal、service_name、keytab_ref；需要可访问的 MIT KDC、部署的 krb5.conf、SASL GSSAPI 插件和 kinit。每会话拥有独立 cache 和 GSS 凭证上下文。 |
| OAUTHBEARER | HTTPS token_endpoint、client_id、client_secret，可选 scope 和独立 CA；使用 client_credentials 自动获取与刷新令牌。Broker 应验证签名 token。 |

不支持云厂商 IAM 扩展、任意 JAAS、浏览器交互 OAuth、静态一次性 token 或开发用未签名 JWT。OAuth token endpoint 禁止 URL 用户信息、查询参数、fragment 和跳转；服务端响应不记录原始 token。部署身份端应提供 Bearer access_token 与整数 expires_in。

会话打开时冻结配置 revision 和凭证内容。密码、证书、keytab/client_secret 文件轮换对新会话生效；活动会话不热换配置，OAuth 和 Kerberos 按期限更新。环境变量轮换需要重启进程。Kerberos 固定 argv 获取短期票据并周期重取，不修改全局 KRB5CCNAME；停止会取消续期、结束子进程并释放私有 cache。

## 验证含义与控制面

验证只查询网络、TLS/SASL 和元数据，不订阅、不消费、不提交位点、不发布消息。指定 Topic 在可访问元数据中不存在返回失败，验证不会自动创建 Topic；返回实际检查过的 broker。查询全部可见 Topic 元数据后确认目标存在，因此缺少元数据访问权限时会失败，不尝试创建指定 Topic。始终返回 `read_permission=unverified`、`write_permission=unverified`，元数据成功不是读写 ACL 已通过。后续数据操作须独立检查权限和方向能力。

异步验证同时最多八个，超限返回 429；启动返回 202 与 test_id，结果仅保留内存五分钟。草稿不落库、不增加 revision；已保存定义验证持有租约，终态前释放客户端与凭证资源。重启、修改定义或凭证文件变化会失去先前验证状态，必须重新验证。

所有管理 API 使用 POST；Scheduler 路径为 `/channels/kafka/`，Web 代理为 `/api/channels/kafka/`：

| 路径后缀 | 作用 |
| --- | --- |
| definitions/query | 实际版本、机制、字段条件、引用别名和方向数据能力。 |
| connections/query / add / modify / remove | 连接 CRUD；query 支持 name、after、limit（1～100）；修改/删除要求 expected_revision。 |
| channels/query / add / modify / remove | 通道 CRUD；structured source/sink、packet source 分别接受下文配置；packet sink 仅保留空方向对象。 |
| tests/start / query / cancel | target 为 connection/channel；已保存目标带 name，可带 expected_revision；草稿使用 draft。query/cancel 使用 test_id。 |

响应 code/stage 为脱敏固定值。HTTP 400 字段错误、404 未找到、409 冲突/引用/使用中、413 请求超限、429 验证并发、503 能力/Provider 不可用、500 持久化失败。认证、证书、超时和取消在验证终态中表达，不作为正常 EOF。

## 结构化 JSON source

在 Web 配置结构化 source 后，状态应为 `ready`，查询通道可见 `source_schema`。未填写字段的旧 source 定义仍可保存，状态为 `configuration_required`，执行前必须补齐。一个通道对应一个 Topic 和消费组，字段配置在 reader 创建时冻结，活动 reader 释放后才能修改。

下面是 `channels/add` 的完整非秘密配置；连接 `logs` 需先按前文创建：

```json
{
  "name": "metrics",
  "connection_name": "logs",
  "role": "source",
  "topic": "business_metrics",
  "consumer_group": "flowsql_metrics",
  "payload_kind": "structured",
  "payload_options": {
    "source": {
      "encoding": "json",
      "entity": "metrics",
      "fields": [
        {"name":"host","path":"/host","type":"utf8","nullable":false},
        {"name":"time_ms","path":"/time","type":"int64","nullable":false,"unit":"unix_ms"},
        {"name":"value","path":"/value","type":"float64","nullable":true,"unit":"bytes"}
      ],
      "offset_reset": "earliest",
      "commit_policy": "after_success",
      "max_batch_rows": 256,
      "max_message_bytes": 1048576
    }
  }
}
```

相应消息为 UTF-8 JSON 对象，如 `{"host":"api-1","time":1700000000000,"value":1024}`。每条消息对应一行；不支持数组包裹、多行 JSON、Schema 推断或字符串数值强转。字段路径使用 JSON Pointer，`/a~1b` 表示键 `a/b`，`/x~0y` 表示键 `x~y`。允许额外 JSON 字段；缺失或 null 只在 `nullable=true` 时接受。

| 配置 | 契约 |
| --- | --- |
| fields | 1～128 个；输出名唯一，不能以 `__kafka_` 开头；类型 bool/int64/uint64/float64/utf8。 |
| unit | 可选非空单位，写入 Arrow field metadata；时间用 int64 加 unix_s/unix_ms/unix_us/unix_ns 明示。时间值按 UTC Unix 纪元解释，不解析本地日期字符串、不换算单位。 |
| offset_reset | earliest（默认）/latest/error；仅在组无有效已提交位点时生效。已有组位点优先。 |
| commit_policy | after_success（默认）在当前批次处理及当次结果投递成功后提交；none 永不提交，适合显式重放分析。 |
| max_batch_rows | 默认256，范围1～4096；最多一个在途批次。 |
| max_message_bytes | 默认1 MiB，范围1～16 MiB；超限消息使任务失败。客户端队列上限32 MiB，reader 约16 MiB 累积消息后停止聚集；Arrow转换及单条消息可能增加瞬时内存，不能把该值视为进程RSS上限。 |

输出保留已配置字段，追加以下身份列：

| 列 | Arrow 类型与含义 |
| --- | --- |
| __kafka_topic | 非空 utf8，源 Topic。 |
| __kafka_partition | 非空 int32，分区。 |
| __kafka_offset | 非空 int64，分区内消息位点。 |
| __kafka_timestamp_ms | 可空 int64，Kafka UTC Unix 毫秒；未提供时为 null。 |
| __kafka_key | 可空 binary，保留任意二进制 key，含零字节。 |

非法 UTF-8、重复键、非对象、tombstone、超限、数值越界或字段类型错误均使任务进入错误终态；不跳过、不进入 DLQ、不当作 EOF。认证、网络、消费和位点提交错误也会终止任务。修复数据/权限或更换独立消费组后重新运行；系统不自动创建 Topic。

### 异步 SQL 与结果边界

Kafka 是持续 source。在任务执行 API `/api/tasks/batch/execute` 使用 `mode=async`；`mode=sync` 返回 `ASYNC_EXECUTION_REQUIRED`。SQL 使用具名通道和配置中的真实字段，例如直接接入 DataFrame：

```json
{
  "sql_text": "SELECT * FROM kafka.metrics WHERE value > 0 INTO dataframe.metric_samples",
  "mode": "async"
}
```

也可在 `FROM`/源过滤之后使用接受此实体 Schema 的已安装 `USING category.operator` 算子；算子由其自身契约决定输出及正常终止条件。packet 专用算子不适用于此实体。过滤由 Scheduler 根据真实 Schema 执行，未下推到 Kafka；不存在字段会在读取前失败。省略算子的复制任务持续读取，停止及结果可见性遵循任务服务和目标通道的契约；需要有限分析结果时选择具有正常终止条件的算子。

提交响应返回 task_id，使用 `/api/tasks/result` 查询，`/api/tasks/cancel` 取消，参数为 `{"task_id":"返回的ID"}`。同进程同一具名 source 同时只允许一个活动 reader；不同进程使用相同消费组时由 Kafka 协调分区，同组会分摊消费，独立分析应使用不同组。不承诺跨分区全局顺序。

`after_success` 提交每个分区当前成功批次最后位点加一。释放 Arrow 批次、Close、Cancel 或析构均不提交。未确认批次重开同组可重放，确认成功后按组位点继续。提交成功响应丢失/超时时，服务端可能已提交，客户端无法据此断定结果；崩溃窗口也可能重复处理。

确认只覆盖当前批次处理及当次结果投递，不代表算子最终 Flush、DataFrame 最终发布或数据库事务已经落盘。后续任务失败/取消可能使尚未发布的内存结果丢失；Kafka 位点不会与最终输出原子提交。此能力不提供 Exactly-once、有状态算子崩溃恢复或端到端事务保证。需要重做整段分析时使用 `commit_policy=none` 或独立消费组，并按目标系统契约管理最终结果；无在线 seek/reset 功能。

## 分析结果 JSON sink

适用于把一次数据库查询、DataFrame 快照或兼容算子输出交给告警系统、SIEM 或订阅者。先准备 Topic 及实际发布身份的权限，关闭自动创建；保存 sink 配置后能力应为 `ready`。未配置的旧 sink 可以保存，能力为 `configuration_required`。ready 和“连接验证通过”均不代表写权限已经验证；空结果也不会发布权限探测消息。

在 Web 的 Kafka 通道中选择 sink 或 both，启用“结果发布”；也可调用通道 CRUD：

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

entity 必须匹配 `[a-z][a-z0-9_]{0,63}`，schema_version 为 1～2147483647。key_fields 是最多 8 列的有序、唯一列表；upsert 必须有 key 和独立的 revision 列，event 可无 key 且不配置 revision。配置、Schema、消息不合法时明确失败，不自动转型或丢弃记录。

| 参数 | 默认值与范围 | 含义 |
| --- | --- | --- |
| max_message_bytes | 1048576；1024～16777216 | 完整 UTF-8 key + value 的字节上限；单独 key 最多 4096 字节；更小的 broker 限额仍可能拒绝。 |
| queue_max_messages | 1024；1～65536 | 编码窗口及客户端队列各自的条数上限。 |
| queue_max_bytes | 8388608；1048576～268435456，且不少于消息上限 | 每个窗口/队列的 payload 字节预算；两者合计不超过两倍预算加一条编码消息，不等同进程 RSS 上限。 |
| delivery_timeout_ms | 10000；100～120000 | 每次 Write 的编码、排队、重试和确认共用的绝对预算；Finish 有一次独立预算。 |

连接 `delivery`、通道 `alerts` 和结果关系/快照需要预先存在，结果含 tenant、alert_id、revision。使用已有任务 API `/api/tasks/batch/execute` 的 `sql_text` 和 mode：

```sql
SELECT * FROM mysql.analytics.alert_results WHERE severity >= 3 INTO kafka.alerts;
SELECT tenant, alert_id, revision FROM dataframe.alert_results INTO kafka.alerts;
```

有限任务支持 sync/async；持续来源仍需 async，并可通过任务 API stop/cancel。数据库沿用实际查询语义并按 reader 分批发布；DataFrame 从任务读取时的只读快照执行现有过滤/列选择，原始内容不清空也不修改。key/revision 被投影删除会在发送前失败。普通 DataFrame 算子（如 `builtin.passthrough`）及单一固定 Schema 的公共 block-transform 输出可发布；普通算子阶段过滤、专有托管/多 Schema/模型双出口协议等未支持组合明确拒绝。NPM/Baseliner 特有结果请先显式写入数据库关系或 DataFrame，再单独执行发布 SQL。

每行是一条非 tombstone 消息，value 的业务字段位于 data 内；无 Schema、EOF、完成等额外控制消息，空结果零消息。信封包含：

| 字段 | 接收方用途 |
| --- | --- |
| envelope_version=1、entity、schema_version | 识别信封和业务版本；与配置 revision 独立。 |
| schema、schema_fingerprint | 携带字段顺序、名称、类型、nullability、metadata；接收方可检测同版本 Schema 冲突。 |
| publication_id、task_id | 区分本次 writer/执行；重执行会产生新的 publication_id。 |
| record_kind、result_id、revision | 表示事件或完整替换快照；revision 在 event 中为 null，在 upsert 中为正整数的十进制字符串。 |
| data | 全部选择的业务列，保留显式 null，不生成或修改业务 revision。 |

支持 bool、int8/16/32/64、uint8/16/32/64、float32/64、utf8、binary、fixed_size_binary、timestamp。int64/uint64 用十进制字符串，其余整数为 JSON number；浮点必须有限，二进制为标准带 padding 的 Base64。timestamp 为原始有符号 int64 字符串，Schema 显式保留 s/ms/us/ns 和 timezone，不转换时区或单位。decimal、nested、dictionary 等不支持类型拒绝，UTF-8、重复字段和 nullability 必须有效。Schema 中对象键按 UTF-8 字节序排序、fields 保持原序，紧凑 JSON 的 SHA256 即指纹；单个 writer 拒绝批次漂移。业务 Schema 变更应增加 schema_version，未建立跨任务 Schema 注册中心。

业务 key 列只支持非空 utf8、整数或 binary。result_id 按 key_fields 顺序携带 `{type,value}`，Kafka key 为紧凑 JSON `[entity,schema_version,result_id]`，不含 revision、task_id 或 Schema 指纹。同一版本同一业务 key 的多次更新得到同一 Kafka key；event 无业务 key 时使用 publication_id 和从 0 开始的行号，因此重执行不能据此去重。upsert 每条是完整替换快照，sink 不生成 revision、不缓存全量 key 检查单调性，接收方应按 key/revision 处理重复和乱序。相同 key/revision 重执行仍可以出现重复消息。

Producer 固定 acks=all、enable.idempotence=true、murmur2 分区器；同 producer 同分区保持提交顺序，跨任务不排序，分区数变化可能改变路由。成功必须收到每条 delivery 成功回调，不把 enqueue 或本地 flush 当成确认。不提供 Kafka/源位点/算子状态之间的事务、Exactly-once、持久重试或断点续发。

公共任务 API `/api/tasks/batch/execute` 和 `/api/tasks/result` 的 `result.publication`（内部 Scheduler 异步查询为 `managed_result.publication`）包含 target、publication_id、writer state、脱敏 code/stage、binding 中的通道/连接 revision 与 Schema 指纹，以及 input_rows、confirmed_rows、not_sent_rows、unknown_rows。终态满足：

```text
input_rows = confirmed_rows + not_sent_rows + unknown_rows
rows_affected = confirmed_rows
```

未调用 Write 的行不计入 input_rows；not_sent 表示未被客户端接受，unknown 表示已接受但缺少明确成功，包括部分 delivery 错误。迟到确认不改写已返回的报告。失败可以已有部分可见结果，不自动重放批次。来源有确认能力时，仅当当前输出发布成功才确认源批次；确认前崩溃可能重放重复，不能视为跨系统原子提交。

正常结束要求上游完成、算子最终 Flush 和 writer Finish/Drain 均成功。stop 停止输入后正常收尾并排空，保留 stopped，排空失败为 failed；cancel 中断等待并停止新发送，不调用成功 Flush/Finish，保留 cancelled 和部分/未知计数。错误阶段为 sink_resolve、sink_open、sink_encode、sink_deliver、sink_finish，不返回原始记录或秘密。

## 回归与依赖升级

```bash
ctest --test-dir build -R '^test_kafka_' --output-on-failure
cd src/frontend
node --test --test-isolation=none src/api/*.test.js src/utils/*.test.js
npm run build
```

真实认证夹具使用固定 Kafka 4.1.0，统一接入 [测试环境说明](testing.md)。标准入口自动准备独立 Broker、KDC 和签名 HTTPS token/JWKS 服务；Docker 使用独立桥接服务网络，秘密与运行状态在 Linux 私有目录，脱敏结果在 evidence/test-environment/。无需引用历史发行包或手工猜测 WSL/Docker 地址。

```bash
tools/test.sh integration --filter '^test_kafka_real_(native|docker)$' --jobs 2
tools/test.sh integration --filter '^test_kafka_deployment_(native|docker)$' --jobs 2
tools/test.sh integration --filter '^test_kafka_source_(native|docker)$' --jobs 2
tools/test.sh integration --filter '^test_kafka_sink_(native|docker)$' --jobs 2
tools/test.sh doctor --filter '^test_kafka_' --location docker
```

普通 Kafka CTest、两种真实客户端/HTTP 认证矩阵、结构化 source/sink 数据面和部署回归分别注册与统计；结构化数据面使用从统一配置派生的独立 Topic/组，验证全部11种身份的消费、同组未确认重放、成功位点继续，另验坏消息、错误认证、取消、续期后消费及真实异步 SQL。sink 数据面由独立消费者核对全部 11 种身份的 key/value/数量、重执行重复，以及写 ACL/错误认证/缺 Topic/超限/部分发布/取消和驻留更新后的继续发布。固定负载成本样本只用于该次环境观察，不是生产容量保证。完整验收使用 `tools/test.sh all`，不能只运行普通单测后声称四协议/身份续期全覆盖。原生矩阵需要本机监听和 JRE21/MIT KDC 等工具；缺依赖返回非零。

部署回归使用实际 `flowsql --config` 入口，检查配置/秘密只读挂载、私有临时目录、加密秘密和连接/通道重启恢复，以及旧验证结果不会跨重启沿用。

依赖升级须同步源码校验、清单和缓存指纹，并重跑四协议、mTLS、PLAIN、两种 SCRAM、双身份 GSSAPI/续期、HTTPS OAuth/刷新、错误凭证、元数据副作用、真实 HTTP 与原生/Docker 回归。客户端编译和 ldd 通过均不能替代真实认证。

## 原始包 packet source

适用于同一远端探针的原始 Ethernet 包汇聚；先在探针侧保留原始字节和 Capture V2 的同次进度，再按 `flowsql_packet_v1` 发布。不能用日志、五元组或统计指标代替包，也不接收 PCAP 文件、JSON、Base64 或 Arrow IPC。接收端以 NPI 解码原始字节，输出规范 PacketFamily Schema，不信任远端层偏移缓存。

先预建单分区、`cleanup.policy=delete` 的 Topic；部署方负责核实保留期与删除策略，客户端会检查分区数，不查询或修改 Topic 配置。同一 stream_id/Topic 只允许一个活动发布者，全部输入队列共享此分区；完整双向会话不能按方向分散到不同实例。独立分析使用不同消费组。准备已存在的连接 `collector` 后，通过 Web「Kafka 通道 → 原始 packet」或 `channels/add` 保存：

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

encoding、stream_id、observation_domain_id、inputs 必填。stream_id 匹配 `[A-Za-z0-9_.-]{1,64}`；观测域为非零 uint64 十进制字符串，Web 不转换为浮点数。输入 1～32 项，source_id/queue_id 为 uint32，source_id 唯一；source_name 为 1～128 字节 UTF-8、无 NUL 的诊断标签。名称不参与包的数字身份判断。

| 参数 | 默认值；范围 | 实际约束 |
| --- | --- | --- |
| max_packets_per_batch | 256；1～4096 | 每消息最多一个批次，超限整条失败，不拆分。 |
| max_bytes_per_batch | 1048576；1024～16777216 | 全部 captured_len 的总和。 |
| max_message_bytes | 2097152；1024～16777216 | key 与完整二进制 value 的总字节数，需同时满足 broker/producer 限额。 |
| max_wait_ms | 100；1～1000 | 单次 poll 等待上限，超时无进度且永不当作 EOF。 |
| offset_reset | earliest；earliest/latest/error | 仅无有效已提交位点时生效。 |
| commit_policy | after_success；after_success/none | 当前消息处理、事实及当次结果交付成功后提交 offset+1；none 不提交。 |

创建后 `adapter_status.source=ready` 表示配置完整，可见 PacketFamily 的 `source_schema`；读 ACL 仍未验证。空方向配置为 configuration_required，运行前须补齐；both 独立保存两个方向，packet sink 仍不可执行。一个 source 在同一进程只允许一个活动 reader，期间修改/删除受租约保护；不同配置名不能让同组任务安全共享分析状态。

Scheduler 进程须加载 Kafka、NPI、数据库和 BinAddonHost，并激活 NPM 插件，参见 [NPM 准备步骤](npm.md#运行准备与快速开始)。使用既有 async 任务 API 提交：

```sql
SELECT * FROM kafka.remote_packets
USING npm.basic
WITH features='basic,session'
INTO mysql.npm;
```

持续分析只支持两段数据库托管目标；协议模块 DNS/HTTP1/TLS 仍需现有 labeling 配置，见 [NPM 模块参数](npm.md#共享与模块参数)。来源能力自动选择 realtime、绑定配置中的观测域；显式冲突的 run_mode/source_domains 拒绝。WHERE 使用 PacketFamily 精确 residual 过滤，不向 Kafka 下推；过滤后的空批仍传递同次进度与确认身份。

### flowsql_packet_v1 外部发布格式

Kafka key 必须是 stream_id 的原始 UTF-8 字节，不允许 null。value 所有整数为大端、逐字段编码，有符号数用二补码；不使用 C++ struct 内存布局。每消息对应一个采集 poll，允许零包但有事实，两者皆零不发送。

| 区段 | 字段顺序 |
| --- | --- |
| 固定头 56 bytes | `FSPK` 4 bytes；version:u16=1；reserved:u16=0；stream_epoch:16非零bytes；domain:u64；capture_generation:u64；poll_sequence:u64；packet_count:u32；fact_count:u32。 |
| 每包 32 bytes 头 + 原文 | source_id:u32；timestamp_ns:i64；captured_len:u32；wire_len:u32；link_type:u32=1；sequence:u64；恰好 captured_len 个 bytes。 |
| 每事实 28 bytes | source_id:u32；queue_id:u32；fact_sequence:u64；capture_time_ns:i64；flags:u8；backlog:u8；reserved:u16=0。 |

value 总长恰为 `56 + Σ(32+captured_len) + 28×fact_count`。flags bit0 表示 packet_observed，bit1 表示 source_idle_confirmed，不可同时为真；其他位为0。backlog 为 0 unknown、1 empty、2 present。包最多4096、事实最多32，并须满足通道预算；缺失或未知字段、尾随字节、截断信封、tombstone、错误 key 均使任务失败，不跳过或送 DLQ。

timestamp_ns/capture_time_ns 是采集端非负 UTC Unix ns，Kafka timestamp 和消费者墙钟不代替它。wire_len 不小于 captured_len；内部 malformed/truncated 报文按实际 layer status 保留。来源须匹配配置，事实 source_id 不重复且 queue_id 匹配。packet_observed 与本消息该输入是否有包一致，为真时事实时间不得超过本批该输入最大包时间。允许省略某输入的事实，接收端不会补造空闲。

writer 每次重开更换非零16字节 stream_epoch；generation、poll_sequence、包 sequence、事实 fact_sequence 均非零。poll 从1逐条加1；包 sequence 保留 reader 全局顺序，可有过滤缺口但必须严格递增；每输入事实序号严格递增。消费可从段中部建立基准，后续 poll 缺口/重复、epoch 或 generation 变化立即失败。下面是可直接运行的独立 Python 格式编码示例；实际发布时将 value 与 stream_id key 交给部署方的 Kafka producer：

```python
import struct
import uuid

stream_id = b"probe-a"
epoch = uuid.uuid4().bytes  # writer 重开时更换，后续 poll 保留
# 零包、单输入的显式 idle/empty 消息；时间必须来自真实采集事实
capture_time_ns = 1_700_000_000_000_000_000
value = struct.pack(
    ">4sHH16sQQQII", b"FSPK", 1, 0, epoch, 7, 1, 1, 0, 1
) + struct.pack(
    ">IIQqBBH", 0, 0, 1, capture_time_ns, 2, 1, 0
)
assert len(value) == 84
# producer.produce("probe_a_packets", key=stream_id, value=value)
```

### 进度、失败和重启

零包事实消息返回零行规范批次，仍经过处理→事实→本次结果交付→确认→归还。普通超时、partition EOF、lag=0 都不产生事实。只有明确 empty/idle 才推进采集时间，多个输入取安全最小值；积压、unknown、缺事实或时钟回拨暂停候选。传输延迟会延后周期交付，不允许本地墙钟提前关闭远端周期。

初次分配后撤销、丢失、重分配或 Topic 分区数变化都会终止旧任务；不会自动接回内存会话。错误诊断包含阶段、实际 first_offset、当前/末次确认 offset、poll、epoch/generation 和 commit_uncertain，不含包内容或秘密。提交失败可能已在 broker 生效，按不确定终止，不能根据本地错误推断一定重放；Release、Close、Cancel、析构不提交。提交成功只覆盖本批成功处理和当次输出，不能证明活动 NPM 状态或未来 Flush 已持久化。

| 操作 | 可观察结果和适用边界 |
| --- | --- |
| stop | 停止拉取并成功收尾，任务 stopped；末尾不完整周期标 period_complete=false，run completed。 |
| cancel / error | 无成功 Flush，run incomplete；已确认位点不回退，未确认批次可能重放。 |
| 同组重启 | 从有效已提交位点继续，新 run_id、空会话状态；不能与旧 session/revision 拼接。 |
| none + 新组/无有效提交位点 + earliest | 在保留期内从最早可用记录重算；none 不清除已有组位点。重放允许跨 run 重复分析，不自动去重。 |
| latest | 跳过已有数据，可能从会话中间开始，不保证完整会话。 |
| 新 epoch 边界 | 旧 run 失败且不确认边界消息；after_success 同组重启可从边界建立新 run。跨历史 epoch 重算应隔离 Topic，不自动跳过旧段。 |

远端采集丢包计数未知时为 unavailable，不能填零；消费者本地收到/交付计数只描述本进程。客户端预取、解码信封和一个 outstanding Arrow 批次分别有界；Arrow 原字节自持所有权。max_bytes/message 不等于 RSS 上限，NPM 还有自己的会话预算。部署重启须持久化 Provider 数据库及原 master key、重挂凭证和私有运行目录；分析续读仍遵循新 run/空状态约束。

### packet source 验证与性能口径

标准入口为 `tools/test.sh integration --filter '^test_kafka_packet_(native|docker)$' --jobs 2`。使用统一运行时测试配置、受管 Kafka 和隔离 Topic/组，在真实位置先验证认证/元数据，再执行实际消费；原生和 Docker 均覆盖11种身份组合，以及 GSSAPI/OAuth 更新后的继续读取。完整回归使用不带筛选的 `tools/test.sh all --jobs 4`，环境不可用为失败，不计跳过通过。

固定性能负载为每消息256个96字节 IPv4/UDP Ethernet 帧、64消息，共16384包、单分区、单消费实例、after_success。计时包含 reader 创建、分配、接收解码和逐消息提交；结果记录 packet/s、message/s、原始/完整信封 byte/s、初始/最终 lag、进程 CPU 秒和峰值 RSS、预装消息从捕获时间至交付的 p50/p95、取消耗时。预装排队延迟包含发布及消费前等待，峰值 RSS 包含进程插件；此口径不能替代含 NPM/数据库的端到端容量或生产承诺。每次结果带执行日期和原生/Docker位置，本地原始证据不入库。

2026-10-15 在 Linux x86_64、Kafka 4.1.0、librdkafka 2.16.0、PLAINTEXT 下的验收样本如下。两个位置并行验收，期间另有 Sanitizer 编译，未固定 CPU 亲和性；这是一轮运行成本记录，不能据此比较原生与 Docker 的性能优劣。完整信封字节数包含 key 和固定/逐包/事实头，不含 Kafka 协议、压缩或网络开销。

| 执行位置 | wall s | packet/s | message/s | 原始 bytes/s | 信封 bytes/s | lag 初始→最终 | CPU s | 峰值 RSS KiB | 预装交付 p50/p95 ms | cancel ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 原生 | 1.1764 | 13927 | 54.40 | 1337020 | 1787530 | 64→0 | 0.1143 | 91240 | 483.5 / 671.0 | 0.0153 |
| Docker | 1.1755 | 13938 | 54.44 | 1338020 | 1788870 | 64→0 | 0.1193 | 74896 | 471.5 / 676.1 | 0.0146 |
