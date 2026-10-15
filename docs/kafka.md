# Kafka 连接、结构化接入与通道管理

FlowSQL 提供可复用的 Kafka 连接配置与具名通道管理。连接负责 broker、TLS/SASL 与凭证生命周期；通道引用连接，并指定 `source`、`sink` 或 `both`、Topic、消费组和数据类别。

结构化 JSON source 可将外部日志、指标或业务记录转换为固定 Arrow Schema，供异步 SQL 分析使用。连接验证只证明认证和元数据访问；消费权限在实际任务中检查。packet 和 Kafka sink 尚未安装，对应方向返回 `KAFKA_DATA_ADAPTER_UNAVAILABLE`。source 只能用于 FROM、sink 只能用于 INTO，角色不符返回 `KAFKA_ROLE_MISMATCH`。

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
2. 新建通道，选择连接、角色、单个 Topic 和数据类别。source/both 必须填写消费组；sink 不提交消费组。选择 structured 后启用结构化 source，配置实体、字段与消费策略。通道创建时也能就地创建连接。
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
| channels/query / add / modify / remove | 通道 CRUD；structured source 接受下文配置；packet/sink 的 payload_options 只允许空方向对象。 |
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
tools/test.sh doctor --filter '^test_kafka_' --location docker
```

普通 Kafka CTest、两种真实客户端/HTTP 认证矩阵、结构化数据面和部署回归分别注册与统计；结构化数据面使用从统一配置派生的独立 Topic/组，验证全部11种身份的消费、同组未确认重放、成功位点继续，另验坏消息、错误认证、取消、续期后消费及真实异步 SQL。完整验收使用 `tools/test.sh all`，不能只运行普通单测后声称四协议/身份续期全覆盖。原生矩阵需要本机监听和 JRE21/MIT KDC 等工具；缺依赖返回非零。

部署回归使用实际 `flowsql --config` 入口，检查配置/秘密只读挂载、私有临时目录、加密秘密和连接/通道重启恢复，以及旧验证结果不会跨重启沿用。

依赖升级须同步源码校验、清单和缓存指纹，并重跑四协议、mTLS、PLAIN、两种 SCRAM、双身份 GSSAPI/续期、HTTPS OAuth/刷新、错误凭证、元数据副作用、真实 HTTP 与原生/Docker 回归。客户端编译和 ldd 通过均不能替代真实认证。
