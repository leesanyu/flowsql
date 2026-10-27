# SQL 采集、分析与输出指引

FlowSQL 使用 `SELECT … FROM … USING … THEN … INTO …` 把数据来源、分析算子和输出位置连接起来。
先准备来源和依赖，确认当前部署的能力，再选择任务入口。本文说明已有能力；产品方向见[愿景](vision.md)。

## 发现通道与算子

Web「通道管理」「算子管理」提供当前部署的资源入口。API 调用使用 Web 地址，例如 `http://127.0.0.1:8081`。
下面查询都是控制面操作；数据在通道与算子之间传输，不随目录查询返回。

| 对象 | 发现入口 | 如何判断 |
| --- | --- | --- |
| DataFrame / CSV | `GET /api/channels/list`，以及通道管理的 DataFrame 页 | 主要查询 DataFrame，失败时可回退本地记录，不能当作所有通道的实时清单。冷启动 CSV 可以未加载，rows/schema 未知不等于零行；首次实际读取才加载。 |
| 数据库 | `POST /api/channels/database/query`，同前缀的 `tables`、`describe` | 连接名与表分别查。已保存配置不证明真实认证、读写权限或目标表就绪。 |
| Stream / block 通道 | `POST /api/channels/stream/query` 与 `/api/channels/stream/definitions/query`，JSON body `{}` | 前者查实例，后者查可创建类型、选项、方向及提供时的 is_finite。PCAP、NetAdapter 和 Kafka block 通道出现在此管理入口，不因此使用 Stream 执行 API。 |
| 算子 | `GET /api/operators/list?type=builtin`，type 分别为 builtin、python、cpp | 按类型查询；category.name 是 SQL 名称。三类响应结构可不同。active 只说明激活，仍需对应插件、来源和依赖可用。 |
| 算子配置 | 通道管理中的配置资源，或 `/api/channels/config/query` | `config.<name>@<revision>` 是配置引用，不是通用 FROM 数据来源。任务固定 revision，不跟随 latest。 |

例如查看内置算子后，在 Web 激活 `builtin.passthrough`；C++ 算子先上传对应 `.so`，再激活。
NPM 需要同进程的 NPI、协议词典及来源插件；DNS/HTTP/TLS 还依赖标签能力。Baseliner 需要 Baseline 插件。
目录里有名字或 active=1，都不能单独证明这些依赖可用。C++ 上传返回 plugin_id，激活请求使用
`{"type":"cpp","plugin_id":"上传返回值"}`；builtin/python 的激活使用 `{"name":"category.name"}`。
PCAP 当前可通过上传或 stream/add 创建并在实例查询中发现，未提供类型定义描述；定义清单不是所有插件的全集。
通过 stream/add 创建 block 来源时显式填 `role=source`、`options` 对象；省略 role 的默认 both 不适用于只读来源。
NetAdapter 配置保存不打开设备，但仍要求已加载所选后端 provider、合法网卡名称和足够缓冲预算。

## 语句结构与结果出口

下面是结构示意，尖括号和花括号不是可复制的 SQL：

```text
SELECT <projection> FROM <source> [WHERE <source-filter>]
  [USING <operator> [WITH <params>] [WHERE <operator-filter>]
    {THEN <operator> [WITH <params>] [WHERE <operator-filter>]}]
  [INTO <destination>]
```

`THEN` 连接同一语句内的算子；分号切分多条语句，单纯换行不会切分。每个 WITH 属于紧邻的算子。
SQL 字符串中的单引号写成两个单引号，JSON 参数作为完整字符串传入。USING/INTO 是否可省略按路径决定，
不要把一种算子的选项或出口套用到其他算子。

WHERE 属于紧邻的生产阶段：FROM 后是来源过滤，USING/THEN 后是相应算子输出过滤。
类型化过滤只适用于已支持的 block 路径和提供可绑定输出 Schema 的 block transform；旧 DataFrame、
IStreamChannel 与数据库路径遵循各自能力。数据库原生 SQL 的 WHERE/GROUP BY/聚合由源库执行，
不能据此推断所有通道都支持 JOIN、聚合或同一套表达式。详见 [SQL 阶段过滤](../README.md#sql-过滤能力)。

`sqlite.work.samples` 表示表，`sqlite.work` 是数据库通道；两段目标的语义由算子决定。
`npm.basic` 的两段数据库出口保存多个启用实体，普通 DataFrame 只保存显式 observing 的一个实体。
持续 NPM 必须写两段托管数据库，不能直接写普通 DataFrame。
Baseliner 从独立的已发布快照或数据库结果读取；不要写 `npm.basic THEN explore.baseliner` 来替代两次任务。

## 选择执行入口

可以先调用 `POST /api/tasks/sql/classify`，body 为 `{"sql":"SELECT …"}`，查询单语句的 task_kind、requires_async。
多语句使用 `POST /api/tasks/sql/analyze`，body 为 `{"sql_text":"…;…;"}`，返回 statement_count、statement_kinds、task_kind、requires_async。
这里的分析只做拆句与类型判断，不验证全部算子参数、Schema、认证或写入能力。

| 来源与组织方式 | Web 对外执行入口 | 参数与限制 |
| --- | --- | --- |
| DataFrame、数据库、有限 PCAP | `POST /api/tasks/batch/execute` | sql_text；默认 mode=async，有限任务可显式 sync；多条 batch 顺序执行。 |
| NetAdapter、持续 Kafka block source | 同一 batch 入口 | mode=async；显式 sync 在创建任务前返回 ASYNC_EXECUTION_REQUIRED。 |
| IStreamChannel 单语句 | `POST /api/tasks/stream/execute` | execution_kind=single、sql_text；仅异步，必须包含流式算子。 |
| 多语句 Stream / mixed DAG | 同一 stream 入口 | execution_kind=group、group_mode=dag、sql_text；至少两条分号分隔；每条语句自身不能混用 stream 与非 stream source。 |

类型看实际 FROM 来源，不看 INTO，也不能只看名称是否带 stream。block_stream 与 IStreamChannel 是不同数据路径。
Web 对连续来源、Stream 和多语句自动选择异步；API 的 batch 多语句仍允许显式 sync，按顺序完成。
本文 `/api/` 路径面向 Web；`/scheduler/batch/*` 是内部运行入口，字段、标识和返回值不同，不能直接互换。

## 观察结果与结束任务

提交时保存返回的 `task_id` 和 `runtime_task_id`。Web batch 用 `POST /api/tasks/list`、`/api/tasks/result`
查询，result body 为 `{"task_id":"提交返回值"}`；Stream 用 `/api/tasks/stream/list`、`status`、`stop`。
Stream status/stop 同样使用其提交返回的 task_id；内部 Scheduler batch/status、batch/stop 使用 runtime_task_id。
托管 NPM 还要保存运行元数据中的 run_id 与关系名，再按运行查询 history/latest/final。
内部 Scheduler 的 managed_result 提供这些信息；Web 摘要不完整透传时按 E03 查询数据库运行元数据，
见 [NPM 结果查询](npm.md#保存所有实体并查询)。

有限来源等待 EOF 与 completed，再核验结果。连续来源保持运行，周期结果从托管数据库读取，普通同步查询不持续推送。
`POST /api/tasks/cancel` 的 task_id 对活动异步 Batch 请求正常 Stop；立即 Cancel 则使用内部
`POST /scheduler/batch/stop` 的 runtime_task_id 与 `mode=cancel`。正常 Stop 排空最后批次和终态；
Cancel 只保留已交付前缀。按钮名称不代表相同底层结束方式，也不意味着停止后能恢复会话。

同步 Web 对执行上游的读取等待为 30 秒，独立于任务 timeout_s。耗时任务优先异步。
若显示 `service unreachable`，先查询原任务及结果，确认状态后决定是否重试，避免重复写入。
错误时保留任务标识、失败阶段与非秘密配置；不要把密码、密钥、票据或完整私有配置贴进日志。

## 本地示例的准备与执行

E01～E06 是 executable 示例，下面的准备脚本供各例复用。请使用一个隔离的原生部署，避免覆盖同名输出；准备后启动服务。
下面的 SETUP 脚本只写 `FLOWSQL_EXAMPLE_DIR` 指定的专用目录，使用 Python 标准库。
先运行 `export FLOWSQL_EXAMPLE_DIR=$(mktemp -d /tmp/flowsql-example-XXXXXX)`，再将下方脚本保存为 setup.py，
执行 `python3 setup.py`（只执行一次）。配置 Catalog 的 `data_dir=<目录>/dataframes`、
Database 的 `type=sqlite;name=work;path=<目录>/work.db`。
Catalog 启动扫描 CSV 注册通道，首次读取时加载。Scheduler 进程加载 builtin、catalog、database、stream、
config_channel、binaddon、NPI（有效 protocols.yml）、pcapfile、baseline、flow_labeling 和 scheduler；
Web/Task/Router 按正常部署连接。上传并激活 npm_basic 与 baseliner 两个 C++ 算子；激活 builtin.passthrough。
E05/E06 还需激活 `builtin.passthrough_stream`；只运行这两例时无需上传 NPM/Baseliner 或加载其分析依赖。
Flow Labeling 的默认 EAL 预留为 512 MiB，需可用 DPDK 运行库，见[部署说明](flow-labeling.md)。

<!-- workflow:SETUP -->
```python
import os, socket, sqlite3, struct
from pathlib import Path

root = Path(os.environ['FLOWSQL_EXAMPLE_DIR'])
(root / 'dataframes').mkdir(parents=True, exist_ok=True)
(root / 'dataframes/samples.csv').write_text('name,value\nalpha,1\nbeta,2\ngamma,3\n')
(root / 'dataframes/series.csv').write_text('id,t,v\n' + ''.join(f'1,{t},{100 + t % 3}.0\n' for t in range(40)))
with sqlite3.connect(root / 'work.db') as db:
    db.execute('CREATE TABLE samples(name TEXT, value INTEGER)')
    db.executemany('INSERT INTO samples VALUES(?,?)', [('alpha', 1), ('beta', 2), ('gamma', 3)])

# 固定 Ethernet/IPv4/UDP DNS，一问一答，采集时间相差 10 ms。
question = b'\x07example\x03com\x00' + struct.pack('!HH', 1, 1)
query = struct.pack('!6H', 0x1234, 0x0100, 1, 0, 0, 0) + question
answer = struct.pack('!6H', 0x1234, 0x8180, 1, 1, 0, 0) + question
answer += b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 60, 4) + socket.inet_aton('192.0.2.20')
def packet(src, dst, sport, dport, payload):
    udp = struct.pack('!4H', sport, dport, 8 + len(payload), 0) + payload
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 1, 0, 64, 17, 0,
                     socket.inet_aton(src), socket.inet_aton(dst))
    checksum = sum(struct.unpack('!10H', ip))
    checksum = (checksum & 65535) + (checksum >> 16)
    checksum = (checksum & 65535) + (checksum >> 16)
    ip = ip[:10] + struct.pack('!H', (~checksum) & 65535) + ip[12:]
    return bytes.fromhex('0200000000020200000000010800') + ip + udp
with (root / 'dns.pcap').open('wb') as capture:
    capture.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
    for micros, frame in [(0, packet('10.0.0.1', '8.8.8.8', 53000, 53, query)),
                          (10000, packet('8.8.8.8', '10.0.0.1', 53, 53000, answer))]:
        capture.write(struct.pack('<IIII', 1, micros, len(frame), len(frame)) + frame)
```

以下每个 JSON 块都是请求记录：把 body 序列化后发送到 Web 的 path，method 为 POST。
只替换声明的 `${EXAMPLE_DIR}`（Scheduler 可读的绝对目录）、`${LABELS_B64}`（下面 YAML 的 UTF-8 base64）、
`${LABEL_REF}`（发布响应的 exact_reference）以及查询中的 `${RUN_ID}`（运行元数据的 run_id）。
SQL 在 body.sql_text 中，不能把请求记录整个作为 API body。
有限示例用 mode=sync，返回后查看任务结果与指定 DataFrame；耗时真实数据改用 async 并等待 completed。

### E01：CSV 注册与独立参数链

目的：验证两个算子的 WITH 独立绑定，输出三行原始数据。前置为 SETUP 和已激活的 passthrough。

<!-- workflow:E01 -->
```json
{"method":"POST","path":"/api/tasks/batch/execute","body":{"mode":"sync","sql_text":"SELECT * FROM dataframe.samples USING builtin.passthrough WITH delay_ms=1 THEN builtin.passthrough WITH delay_ms=0 INTO dataframe.csv_copy"}}
```

用 `POST /api/channels/dataframe/preview`，body `{"name":"csv_copy","page":1,"page_size":100}`。
预期 columns 为 name/value，rows=3，内容 alpha/1、beta/2、gamma/3。等待 EOF，不需 Stop。

### E02：SQLite 原生过滤与聚合

本例的 WHERE、SUM 由 SQLite 执行。聚合表达式在当前通道返回字符串列，结果为单行 `"5"`；
投影不使用 AS 别名。它不证明其他来源支持聚合。

<!-- workflow:E02 -->
```json
{"method":"POST","path":"/api/tasks/batch/execute","body":{"mode":"sync","sql_text":"SELECT SUM(value) FROM sqlite.work.samples WHERE value >= 2 INTO dataframe.sqlite_total"}}
```

按 E01 的 preview 方法查看 sqlite_total，预期 rows=1、columns=[SUM(value)]、data=[["5"]]。

### E03：固定 PCAP 的 Basic、Session 与 DNS

先创建文件来源；role 必须为 source。路径位于 Scheduler 所在机器。

<!-- workflow:E03-SOURCE -->
```json
{"method":"POST","path":"/api/channels/stream/add","body":{"type":"pcapfile","name":"dns_capture","role":"source","options":{"path":"${EXAMPLE_DIR}/dns.pcap","format":"pcap"}}}
```

发布此完整 FlowLabelingSet；规则按 UDP/53 双向匹配。订阅 ID 1002，并显式启用 labeling、dns。

<!-- workflow:DNS-LABELS -->
```yaml
api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
metadata:
  name: workflow-dns
spec:
  engine:
    type: dpdk-acl
    categories: 1
    algorithm: default
    numa_socket_id: any
    max_runtime_bytes: 33554432
    limits:
      max_labels: 10
      max_logical_rules: 10
      max_compiled_rules: 20
      max_expanded_fields: 64
      reject_duplicate_label_priorities: true
  labels:
    - id: 1002
      priority: 100
      name: dns
  rules:
    - id: dns-udp
      label_id: 1002
      direction: bidirectional
      matches:
        - field: transport_protocol
          value: 17
          mask: 255
        - field: destination_port
          min: 53
          max: 53
```

<!-- workflow:E03-CONFIG -->
```json
{"method":"POST","path":"/api/channels/config/publish","body":{"name":"workflow-dns","expected_current_revision":0,"format":"yaml","schema_id":"flowsql.io/flow-labeling/v1alpha1","content_base64":"${LABELS_B64}","original_filename":"workflow-dns.yaml","change_note":"isolated example"}}
```

先选 Basic 输出；相同有限文件可以在不同任务重新读取。final 只输出终态累计值。

<!-- workflow:E03-BASIC -->
```json
{"method":"POST","path":"/api/tasks/batch/execute","body":{"mode":"sync","sql_text":"SELECT * FROM pcapfile.dns_capture USING npm.basic WITH features='basic,session',observing='basic_ipv4',result_mode='final' INTO dataframe.dns_basic"}}
```

<!-- workflow:E03-SESSION -->
```json
{"method":"POST","path":"/api/tasks/batch/execute","body":{"mode":"sync","sql_text":"SELECT * FROM pcapfile.dns_capture USING npm.basic WITH features='basic,session',observing='session_ipv4',result_mode='final' INTO dataframe.dns_session"}}
```

<!-- workflow:E03-DNS -->
```json
{"method":"POST","path":"/api/tasks/batch/execute","body":{"mode":"sync","sql_text":"SELECT * FROM pcapfile.dns_capture USING npm.basic WITH features='basic,session,labeling,dns',observing='dns_transaction_ipv4',parameters='{\"schema_version\":1,\"core\":{\"labeling\":\"${LABEL_REF}\",\"result_mode\":\"final\"},\"dns\":{\"primary_label_ids\":[1002]}}' INTO sqlite.work"}}
```

dns_basic 与 dns_session 各一个 IPv4 UDP 会话，双向包数各 1，总包数 2；UDP 不承诺 TCP RTT。
最后一个任务的两段 sqlite.work 保存所有启用实体，同时 observing 选择 DNS 前台结果。
Web 任务摘要不直接返回完整 managed_result；先执行下面的元数据查询，在专用空库中应只有一个运行，保存 run_id。
内部 Scheduler 响应的 managed_result 也包含运行与关系信息。不要把 Web task_id 当 run_id。

<!-- workflow:E03-RUN -->
```json
{"method":"POST","path":"/api/tasks/batch/execute","body":{"mode":"sync","sql_text":"SELECT * FROM sqlite.work.npm_result_runs WHERE input_namespace='pcapfile.dns_capture' INTO dataframe.dns_runs"}}
```

preview dns_runs，核对 status=completed，再在数据库通道查询 npm_basic_ipv4_final_v1、npm_session_ipv4_final_v1、
npm_dns_transaction_ipv4_final_v1，以 `__npm_run_id` 精确筛选，DNS 预期一个 example.com 的成功响应事务。

<!-- workflow:E03-QUERY -->
```json
{"method":"POST","path":"/api/tasks/batch/execute","body":{"mode":"sync","sql_text":"SELECT * FROM sqlite.work.npm_dns_transaction_ipv4_final_v1 WHERE __npm_run_id='${RUN_ID}' INTO dataframe.dns_transaction"}}
```

preview dns_transaction 核验 qname、outcome、事务数；完整字段见 [NPM](npm.md)。本例只覆盖完整双向 UDP DNS。

### E04：独立快照的评估、预测与模型

此任务在前序任务结束后单独提交。series 是 SETUP 生成的 40 桶快照，值为 100+t%3；
真实 NPM 指标应先从已完成的结果抽取成独立的 bucket/series/value 快照，再配置字段与指标语义。
不要直接把 NPM 报文时间当桶编号。本例预测每次评估后两个桶，不仅包含最后两个桶。

<!-- workflow:E04 -->
```json
{"method":"POST","path":"/api/tasks/batch/execute","body":{"mode":"sync","sql_text":"SELECT * FROM dataframe.series USING explore.baseliner WITH parameters='{\"schema_version\":1,\"task_key\":\"workflow-baseline\",\"source\":\"dataframe.series\",\"mode\":\"snapshot\",\"clock\":{\"bucket_seconds\":60,\"timezone\":\"UTC\"},\"calendar\":{\"calendar_id\":\"cn-holiday\",\"calendar_version\":\"2026.1\"},\"forecast\":{\"horizon_buckets\":2},\"datasets\":[{\"id\":\"d\",\"fields\":{\"id\":\"uint64\",\"t\":\"int64\",\"v\":\"float64\"},\"scope\":{\"begin_bucket\":0,\"end_bucket\":40,\"consistency\":\"dataframe_snapshot\"},\"series_keys\":[\"id\"],\"deduplicate\":{\"keys\":[\"id\",\"t\"],\"on_duplicate\":\"require_equal\"},\"bucket\":{\"column\":\"t\",\"unit\":\"bucket_id\"},\"metrics\":[{\"id\":\"v\",\"kind\":\"value\",\"column\":\"v\",\"feature_type\":\"value_basic\",\"profile\":\"default\"}]}]}',model_output='dataframe.workflow_models' INTO sqlite.work.workflow_baseline"}}
```

使用 SQLite 查询 workflow_baseline（例如 `sqlite3 <目录>/work.db`）。预期 evaluation 40 行、forecast 80 行；
evaluation 覆盖桶 0～39，observed=100+桶%3；
forecast 的 target_bucket 比 issued_after_bucket 大 1 或 2，observed 为 NULL，最后覆盖到 41。
preview workflow_models，预期非空并带 config_hash。当前普通 DataFrame preview 会把数值 NULL 显示为零，
因此本例从 SQLite 核验预测 observed IS NULL；不能用 preview 的零解释缺失观测。预测值随模型成熟度变化，本例不承诺固定精度或告警阈值。
模型出口只在正常 EOF 完成后发布，详见 [Baseliner](baseliner.md)。

### E05：Stream single 与正常结束

本例使用内置 `tcp_session_mock` 作为隔离的有限 Stream 来源；它只用于验证 Stream API 的异步入口、状态和 EOF，
不代表真实设备或 Broker。创建来源后立即提交，来源产生 6 行并关闭；Stream 任务结束状态为 `stopped`，
结果写入 DataFrame。持续 block 来源必须使用 batch 的异步入口；持续 IStreamChannel 仍使用 Stream 入口。
任务页的 Stop 是正常排空结束，立即 Cancel 的语义见上文。

<!-- workflow:E05-SOURCE -->
```json
{"method":"POST","path":"/api/channels/stream/add","body":{"type":"tcp_session_mock","name":"workflow_stream_single","role":"source","options":{"mode":"none","total_records":6,"batch_rows":2,"emit_interval_ms":1,"partition_count":1,"ring_size":16,"overflow":"block","ring_mode":"spsc"}}}
```

<!-- workflow:E05 -->
```json
{"method":"POST","path":"/api/tasks/stream/execute","body":{"execution_kind":"single","sql_text":"SELECT * FROM tcp_session_mock.workflow_stream_single USING builtin.passthrough_stream INTO dataframe.workflow_stream_single_out"}}
```

保存响应中的 `task_id`，用 `POST /api/tasks/stream/status` 和
`{"task_id":"${E05_TASK_ID}"}` 轮询到 `status=stopped`，再 preview
`workflow_stream_single_out`。预期 6 行，字段为 `clientIP`、`clientPort`、`serverIP`、`serverPort`、`bps`、`pps`；
不把一次有限 EOF 结果解释为持续服务保证。

下面用无输入的持续 ring 验证正常停止：它是受控 `IStreamChannel`，不是持续 block 采集来源。
创建两个通道后提交，确认 status 为 running，再将提交返回的 task_id 替换到 Stop 请求。

<!-- workflow:E05-LIVE-SOURCE -->
```json
{"method":"POST","path":"/api/channels/stream/add","body":{"type":"ring","name":"workflow_idle_source","role":"source","options":{"ring_mode":"spsc","ring_size":16,"overflow":"block","finite":false}}}
```

<!-- workflow:E05-LIVE-SINK -->
```json
{"method":"POST","path":"/api/channels/stream/add","body":{"type":"ring","name":"workflow_idle_sink","role":"sink","options":{"ring_mode":"spsc","ring_size":16,"overflow":"block","finite":false}}}
```

<!-- workflow:E05-LIVE -->
```json
{"method":"POST","path":"/api/tasks/stream/execute","body":{"execution_kind":"single","sql_text":"SELECT * FROM ring.workflow_idle_source USING builtin.passthrough_stream INTO stream.workflow_idle_sink"}}
```

<!-- workflow:E05-STOP -->
```json
{"method":"POST","path":"/api/tasks/stream/stop","body":{"task_id":"${STREAM_TASK_ID}"}}
```

Stop 返回和后续 status 均应为 stopped；本例未发送数据，处理行数为零。

### E06：Stream Group DAG 与 mixed Group DAG

下面两组请求都使用同一个 `stream/execute` 入口，并明确声明 `execution_kind=group`、`group_mode=dag`。
每条语句用分号切分；换行只改善可读性。纯 Stream 组从有限 mock 来源经过中间 Stream 通道到 DataFrame，
mixed 组先由 DataFrame 批节点写入 Stream 通道，再由 Stream 节点写入 DataFrame。两组都以 `stopped` 收口并核验节点类型和行数。

<!-- workflow:E06-STREAM-SOURCE -->
```json
{"method":"POST","path":"/api/channels/stream/add","body":{"type":"tcp_session_mock","name":"workflow_stream_group_source","role":"source","options":{"mode":"none","total_records":6,"batch_rows":3,"emit_interval_ms":1,"partition_count":1,"ring_size":16,"overflow":"block","ring_mode":"spsc"}}}
```

<!-- workflow:E06-STREAM-MID -->
```json
{"method":"POST","path":"/api/channels/stream/add","body":{"type":"ring","name":"workflow_stream_group_mid","role":"both","options":{"ring_mode":"spsc","ring_size":16,"overflow":"block","finite":false}}}
```

<!-- workflow:E06-STREAM -->
```json
{"method":"POST","path":"/api/tasks/stream/execute","body":{"execution_kind":"group","group_mode":"dag","sql_text":"SELECT * FROM tcp_session_mock.workflow_stream_group_source USING builtin.passthrough_stream INTO stream.workflow_stream_group_mid; SELECT * FROM stream.workflow_stream_group_mid USING builtin.passthrough_stream INTO dataframe.workflow_stream_group_out;"}}
```

提交响应的 `node_count=2`。先查询响应中 task_id 的 status，确认 `nodes` 包含两个 `stream` 节点，
再 preview `workflow_stream_group_out` 到 6 行，clientPort 为 10000～10005，各节点 processed_rows 均为 6。
第一节点有限 EOF 后停止，但 `finite=false` 的中间 ring 继续等待输入，整个组仍为 running，不会由第一节点 EOF 自动关闭。
此时将组 task_id 替换为 `${STREAM_TASK_ID}`，请求正常停止，再查询 status 确认 stopped：

<!-- workflow:E06-STREAM-STOP -->
```json
{"method":"POST","path":"/api/tasks/stream/stop","body":{"task_id":"${STREAM_TASK_ID}"}}
```

Group 状态中的 `resolved_sources`、`share_sets` 是观测信息，不等同于跨任务补发历史；late join 从加入时刻开始消费。

<!-- workflow:E06-MIXED-MID -->
```json
{"method":"POST","path":"/api/channels/stream/add","body":{"type":"ring","name":"workflow_mixed_mid","role":"both","options":{"ring_mode":"spsc","ring_size":16,"overflow":"block","finite":false}}}
```

<!-- workflow:E06-MIXED -->
```json
{"method":"POST","path":"/api/tasks/stream/execute","body":{"execution_kind":"group","group_mode":"dag","sql_text":"SELECT * FROM dataframe.samples INTO stream.workflow_mixed_mid; SELECT * FROM stream.workflow_mixed_mid USING builtin.passthrough_stream INTO dataframe.workflow_mixed_out;"}}
```

轮询到 `status=stopped`，确认节点同时包含 `batch` 与 `stream`，`node_count=2`，再 preview
`workflow_mixed_out`，预期与 `samples.csv` 相同的 3 行。单条 SQL 若同时包含 stream 与非 stream source 仍应失败，
不能用 mixed group 绕过该限制。

## 持续 block 来源与 Kafka 条件模板

以下 C01～C05 均为 `conditional_template`，本 Feature 未执行真实网卡或 Broker。
先在目标 Scheduler 配置来源、出口、插件、身份与权限，并从实际执行位置验证连接和声明操作；模板使用具名通道，
占位符只替换为已存在的通道名称，不能直接原样提交。所有请求均为
`POST /api/tasks/batch/execute`，SQL 放入 `sql_text`，`mode` 按表选择。

| ID / 方向 | 已配置条件与 SQL 形状 | 提交及结果边界 |
| --- | --- | --- |
| C01 NetAdapter → NPM | `${CAPTURE}` 为可读网卡来源，`${DB}` 为两段托管数据库；加载后端、NPI 和已激活 NPM。`SELECT * FROM netadapter.${CAPTURE} USING npm.basic WITH features='basic,session' INTO ${DB}` | async；从 runtime_task_id 的 batch/status 取得 managed_result，再查询 history；正常 Stop 排空终态。后端权限、缓冲与接口可见性见 [NetAdapter](netadapter.md#通道与生产-sql)。 |
| C02 Kafka structured source → DataFrame | `${LOG_SOURCE}` 的 source 配置声明 JSON 字段 Schema，身份有 READ/组权限；`SELECT * FROM kafka.${LOG_SOURCE} INTO dataframe.log_samples` | async；结构化实体不能交给 packet 专用算子。最终 DataFrame 发布和 offset 确认不是同一事务，见 [JSON source](kafka.md#结构化-json-source)。 |
| C03 DataFrame → Kafka result sink | `${RESULT_SINK}` 的 sink 配置使用 JSON 结果信封，包含所需 key/revision 字段，身份有 WRITE；`SELECT * FROM dataframe.csv_copy INTO kafka.${RESULT_SINK}` | 有限来源可 sync/async；每行一条消息，检查 confirmed/not_sent/unknown，空结果不证明写权限，见 [结果 sink](kafka.md#分析结果-json-sink)。 |
| C04 Kafka packet source → NPM | `${PACKET_SOURCE}` 读取 `flowsql_packet_v1`；单分区 Topic、stream_id、观测域与 inputs 一致，`${DB}` 已创建；`SELECT * FROM kafka.${PACKET_SOURCE} USING npm.basic WITH features='basic,session' INTO ${DB}` | async；原始 Ethernet 包与同次 Capture V2 事实缺一不可，不接收 JSON/PCAP 文件；结果与新 run 规则见 [packet source](kafka.md#原始包-packet-source)。 |
| C05 NetAdapter → Kafka packet sink | `${CAPTURE}` 为 Capture V2 网卡来源，`${PACKET_SINK}` 是同流单发布者的单分区 packet sink；`SELECT * FROM netadapter.${CAPTURE} INTO kafka.${PACKET_SINK}` | async；仅 SELECT * 直通形状，不加 WHERE/USING/投影。逐批确认不等于零丢包，见 [packet sink](kafka.md#网卡原始包-packet-sink)。 |

Kafka 四类适配器的方向和数据契约独立，不能互换配置。通道 ready、active 或连接验证只证明相应准备层级；
不证明实际发布、消费或托管结果已经成功。持续 batch 的显式 sync 会在创建任务前返回 ASYNC_EXECUTION_REQUIRED。
本地标准 discovery 验证该准入，Scheduler 回归使用受控持续 Capture provider 验证运行状态、托管 history、Stop 与 Cancel；
该 provider 不打开设备或连接 Broker。

持续 batch 提交后保存 Web task_id/runtime_task_id；按上文 list/result 和内部 batch/status 观察，
`/api/tasks/cancel` 请求正常 Stop，等待 stopped 后核验 run completed 及终态结果；立即 Cancel 或错误会留下 incomplete 前缀。
Stream 的 stop 与 batch 的停止入口分别使用对应标识。停止后再清理专用通道，活动 reader/writer 期间不要修改或删除配置。

同源 Stream 的 late join 不补历史且要求 WHERE 签名一致。Kafka 断连、重平衡或确认不确定时不能宣称自动恢复了旧会话；
修复后重新提交是新任务/新 run，位点续读不恢复 NPM 内存状态，重放可能重复输出。packet 新 epoch 也建立新边界，
探针停止不发送远端 EOF；集中分析端须独立停止。详细重试和确认语义以链接中的适配器说明为准。

示例清理：先确认所有任务结束，停止这个专用部署，再删除专用输出通道和示例目录；不要删除业务通道或共享数据库。
仓库测试入口 `tools/test.sh all --location native --filter '^(test_sql_workflow_finite|test_sql_workflow_stream)$' --jobs 1`
会自动准备临时目录、启动隔离进程、执行上述原文请求并清理自身资源，可用于检查本机依赖。

## 验证范围

指引的本地验收使用隔离 SQLite/CSV/PCAP、真实原生插件和随机本地端口。
实际网卡、Kafka Broker、认证、数据库后端和容器部署的要求分别见
[NetAdapter](netadapter.md)、[Kafka](kafka.md)、[NPM](npm.md) 与 [Baseliner](baseliner.md)。
目录查询、SQL 分析、本地受控来源的通过结果均不能证明目标环境的真实数据面就绪。
