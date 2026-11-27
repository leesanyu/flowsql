# 受控数据与 MCP 接入

普通 HTTP 程序和 MCP 客户端使用同一组通用工具发现来源、理解字段、提交有界查询和固定方案分析，按操作身份读取结果。
授权、计算、取消、操作记录与结果均由 FlowSQL 持有。MCP 是独立 Python 服务，可与 FlowSQL 分机部署；不要求运行 Agent、模型或 Python Worker。

## 部署受控 HTTP 服务

构建入口及产物：

```bash
cmake -B build src
cmake --build build --target flowsql flowsql_data_access flowsql_config_channel flowsql_builtin flowsql_catalog flowsql_database -j8
```

构建会把工具 Schema 和部署样例复制到 `build/output/config/agent-data-access/`。部署目录需包含框架二进制及其 `.so` 依赖。
在该目录创建权限为 0700 的 `meta/`、`dataframes/`；复制 `policy.example.json` 为 `meta/data-access-policy.json`，只保留实际批准的主体和来源。
私有策略、数据库连接配置、状态及凭证不提交到 Git。运行时状态使用 POSIX 文件系统；同一状态文件只允许一个进程使用。

数据库通道从 `config/flowsql.yml` 读取；复用管理侧已经建立的当前配置，保留各通道的 `dataset_instance_id`。
以下是 SQLite 通道结构，路径由部署方选择，身份在首次登记时生成一次并保存：

```yaml
channels:
  database_channels:
    - type: sqlite
      name: business
      path: /srv/flowsql/business.db
      dataset_instance_id: '<部署方登记的稳定唯一身份>'
```

策略的数据库对象精确到表或视图。`filters` 的 `fid` 在策略中使用真实列名，在计算前与客户端条件共同生效。
例如只授权 `sales` 的 2026 年数据：

```json
{
  "principal": "analyst",
  "token_env": "FLOWSQL_ACCESS_ANALYST_TOKEN",
  "max_rows": 1000,
  "max_bytes": 8388608,
  "timeout_ms": 5000,
  "result_ttl_ms": 3600000,
  "sources": [{
    "locator": {"kind":"database_table","type":"sqlite","name":"business","schema":"main","table":"sales"},
    "operations": ["query"],
    "filters": [{"fid":"business_date","rule":{"type":"range","value":["2026-01-01","2026-12-31"]}}]
  }],
  "analyses": []
}
```

该主体放在策略根对象 `{"subjects":[...]}` 中。部署方通过私有环境文件或服务管理器提供至少 32 字符的随机凭证；凭证不写入命令行、工具参数或日志。
启动样例：

```bash
cd build/output
./flowsql --config config/agent-data-access/deploy.example.yaml
```

该样例仅加载 Config、Builtin、Catalog、Database 和受控插件，监听 `127.0.0.1:18805`；未加载 NPM 也可查询业务表及 Catalog 的 DataFrame/CSV。
`GET /health` 检查进程存活。带服务受众专用 `Authorization: Bearer …` 的 `GET /data-access/tools` 返回完整工具 Schema；
`POST /data-access/call` 接收 `{"name":"工具名","arguments":{...}}`。错误为非 2xx 和 `{"error":{"code":"…","message":"…"}}`。

受控客户端只获得此监听器的访问权。Gateway、Router、Web、Scheduler 管理接口及数据库端点属于管理网络，不能同时向该客户端开放。
专用监听器没有 `/scheduler/batch/execute` 等路由，受控插件也不将其业务接口注册到管理 Router。
远程部署使用 TLS 反向代理，并以网络规则限制上游管理端口；HTTP 上游仅适合本机或受保护网络。

## 来源和结构化查询

先调用 `list_sources`，用 `offset/limit` 分页（默认 32、最多 100）；再按返回的 `source_ref` 调用 `describe_source`。
描述返回真实 locator、结构、语义说明、修订及逐操作的 `supported/authorized/available/reason`。
目录只包含主体显式授权的对象及其获准分析实际发布的派生对象；身份不可由展示名或表名拼造。
目录过大返回 `RESULT_PAGE_LIMIT` 时减少页大小。

| 来源 | 可用操作及边界 |
| --- | --- |
| SQLite/MySQL/PostgreSQL/ClickHouse 表与授权视图 | 结构化只读查询；固定分析方案可将其作为输入 |
| 内存 DataFrame、Catalog CSV/算子结果 | 描述不触发延迟加载；查询/分析必须显式 `capture_snapshot=true` |
| 结构化流及 Hub | 描述已有 Schema、角色和有限性；不轮询共享队列或建立订阅 |
| 已注册 PCAP/PCAPNG | 描述有限性与大小；单独批准的有限分析，不开放任意路径和原始报文下载 |
| Kafka、NetAdapter | 描述载荷、source/sink/both 角色与可公开元数据；不消费、提交位点、发布或启动采集 |
| Config | 仅格式、Schema 及精确修订等元数据；配置使用需另授 `analyze`，不开放正文下载或修改 |

缺少 provider、结构或说明时返回明确未知/不可用状态。来源变化返回 `SOURCE_CHANGED`，重新发现后应使用新的请求身份；
同一请求身份不能改参数重试。分析方案变化同样返回 `OPERATION_CHANGED`。

查询只接受单来源结构化 workflow，不接受原生 SQL、连接串、文件路径和写出目标。
字段使用描述返回的 `field_id`，示例中的 `f0/f1` 必须按实际字段映射替换：

```json
{
  "name": "query",
  "arguments": {
    "request_id": "sales-by-date-001",
    "source_ref": {"source_id":"<发现返回的身份>","source_revision":"<发现返回的修订>"},
    "timeout_ms": 5000,
    "max_rows": 1000,
    "workflow": [
      {"type":"filter","filters":[{"fid":"f0","rule":{"type":"comparison","op":"gte","value":"2026-11-01"}}]},
      {"type":"view","query":[{"op":"aggregate","groupBy":["f0"],"measures":[{"field":"f1","agg":"sum","asFieldKey":"amount_sum"}]}]},
      {"type":"sort","by":["f0"],"sort":"ascending"}
    ]
  }
}
```

可用计算包括 raw 明细、count/sum/mean/min/max/distinctCount 聚合、day/month 时间分组、等宽直方图、过滤和排序；
具体输入约束以 [机器 Schema](../config/agent-data-access/api.schema.json) 为准，后端不支持的类型/计算明确拒绝。
服务端生成 SQL 并绑定值；DataFrame 在有界快照上计算。ClickHouse 不宣称跨查询事务一致性。
不隐式截断完整结果；超过 `max_rows/max_bytes` 失败，显式 `limit` 的结果在 `basis.scope` 中声明范围。
查询不提供采样、任意表达式或未声明分位数；上限不代表数据库扫描行数保证，执行由实际期限和取消控制。

## 操作与结果交接

提交先持久登记，立即返回 `operation_id/request_id/kind/status`。用 `get_operation` 按操作身份或请求身份查询；二者只提供一个。
同主体、同请求身份、同参数返回原操作；参数不同返回 `REQUEST_CONFLICT`。提交响应丢失时先按 `request_id` 核对。
MCP 断连、传输取消或 MCP 进程退出不取消业务任务。业务取消使用 `cancel_operation`，等待实际 `cancelled/failed/completed`。

查询服务重启时未完成查询为 `interrupted`。分析结果不明时为 `reconcile_required`，保留稳定底层任务身份；
后续读取会尝试核对，找不到底层任务仍待核对，不自动重投。管理员应按操作记录核查实际输出；不要用新请求身份盲目重复分析。

完成后按 `result_ref` 调用 `read_result`，使用 `offset/limit` 分页（最多 1000 行、单响应 1 MiB）。
结果内容不可变，含字段类型、执行依据、原生 SQL/绑定值或分析清单、行数、范围和过期时间。
查询依据记录精确来源、结构与字典/契约引用；完整语义说明通过来源描述读取。

| 单元格 | 编码 |
| --- | --- |
| NULL | JSON `null` |
| 整数、浮点、Decimal | `{"type":"int64/float64/decimal","value":"精确文本"}`；不要先经过 JavaScript Number |
| 文本、数据库时间、Arrow 时间 | `type=string`；字段 `native_type` 保留时间单位/时区，Arrow 时间保留声明精度 |
| 二进制 | `type=binary_hex`，十六进制；不冒充 UTF-8 |

先调用 `acquire_result_lease`（最长 300000 ms），分页读取时传相同 `lease_id`，完成或失败后调用 `release_result_lease`。
租约有效期间可读过期前取得的同份结果；失效为 `RESULT_EXPIRED/LEASE_UNAVAILABLE`，越权引用为 `NOT_FOUND`。
复制方逐页使用返回的原文，独立核对：

```text
content_sha256 = SHA256(UTF-8(integrity_header + LF + 每个 row_json + LF))
```

`integrity_header` 只拼一次，行按 offset 顺序拼接；不要重序列化原文。核对整份摘要成功后再发布永久证据。
服务不重新执行过期查询来补造数据；永久保存由调用方承担。
主体默认预算为 1000 行、8 MiB、5 秒、结果 1 小时；部署上限分别为 65536 行、64 MiB、30 秒、24 小时。
最多 4 个执行中操作、每主体 64 个租约；结果存储每主体 128 MiB，操作登记表最多 10000 条，满额明确拒绝。

## 固定方案有限分析

`list_operations/describe_operation` 返回部署方批准方案及真实算子身份、契约引用和可用原因。
`preflight_analysis` 使用 `operation_ref/source_ref` 检查实际契约及所有解析输入/配置的授权；`submit_analysis` 再带 `request_id` 与预算。
客户端不能改算法参数、配置引用、输入路径或输出目标。仅开放真实声明且具备块处理/取消契约的算子。
带策略行过滤的来源不进入算子路径，返回 `UNSUPPORTED_SCOPE`；这类来源仍可使用受控 workflow 查询。

[策略样例](../config/agent-data-access/policy.example.json) 中 `dns_capture` 选择真实 `npm.basic`，固定 `config.npm-labels@1` 和 DNS 标签 1002，
写入主体独占的 `sqlite.analyst_results`。部署前由管理侧完成 PCAP/PCAPNG 通道、标签配置、输出数据库和算子安装/激活，
在受控框架进程内加载 Scheduler、BinAddon、NPI、PCAP、Flow Labeling 等实际依赖；缺失时方案明确不可用。
只读最小部署不必加载这些依赖；其他声明算子（例如 Baseliner）使用同一工具和方案结构。

`output` 只允许自动命名的 `dataframe`，或部署方独占分配的 `managed_database` 通道。
托管数据库不得与任意主体的业务来源通道相同，也不能由不同主体共享。
分析成功结果是 `basis.scope=analysis_manifest` 的不可变清单，包含实际任务、查询、契约执行引用及 `derived_sources`；
从这些真实公开输出的 `source_ref` 继续描述和查询数据。NPM 关系来自实际执行发布的语义，客户端按 run_id 选择 history/latest/final，
不访问内部存储表或累加累计修订。分析清单零业务行不代表分析没有输出数据。

DNS 验收口径为每族固定 8 个事务；有效时延条件是 matched、latency_ns 非 NULL、response_tc=0，分母为 2，mean/min/max 均为 10000000 ns。
RCODE 分布为 3→1、0→4、NULL→3；query_at_ns 两箱直方图排除 NULL，边界为族起点、+56500000、+113000000 ns，计数 3/4，末箱右端闭合。
这些是固定夹具的独立断言，不是生产数据结论；领域说明仍以实际字典和契约为准。

## 独立 MCP 安装与启动

Python 3.10+，只安装独立包及其声明依赖：

```bash
python3 -m venv /opt/flowsql-mcp
/opt/flowsql-mcp/bin/pip install /absolute/path/to/flowsql/src/mcp
```

也可构建 wheel 后分发安装。MCP 依赖官方 SDK `>=1.30.0,<1.31`、httpx 和 uvicorn；
使用 Streamable HTTP `2025-11-25`，监听 `/mcp`。它不导入 Worker/NPM，不直连数据库，不挂载 FlowSQL 数据目录，不加载 `.so`。

复制 [MCP 配置样例](../config/agent-data-access/mcp.example.json) 到部署方私有目录：

- `upstream_url` 指向受控监听器；`public_url` 是客户端访问的完整 `/mcp` 地址。
- 每个 `principal` 明确绑定 `token_env` 与 `upstream_token_env`。MCP 入站和 FlowSQL 下游凭证必须不同，主体在两端配置一致。
- `host/port` 控制本地监听；`allowed_origins` 是精确 Origin 白名单，默认不允许浏览器 Origin；Host 依据 public_url 校验。
- 远程对外地址使用 HTTPS，反向代理保留批准的 Host/Origin。此部署使用管理员签发的静态服务凭证，未提供 OAuth 授权服务器或动态客户端注册。

```bash
cd /tmp
/opt/flowsql-mcp/bin/flowsql-mcp --config /etc/flowsql/mcp.json
```

`GET /health` 为进程存活，`GET /ready` 会逐个检查已配置主体能否访问上游工具契约，上游错误返回 503。
客户端连接 `public_url`，携带 MCP 专用 Bearer 凭证。工具由上游动态提供，业务错误放在 MCP `isError/structuredContent.error`，
与 HTTP 的业务错误码一致；不会把 MCP 入站令牌直接传给不同受众的下游。
请求和上游响应限 1 MiB，HTTP 等待 35 秒，最多 20 个上游连接；大目录/结果应分页。
重启 MCP 可按原请求/操作/结果身份重读，不影响 FlowSQL 任务归属。

## 验证范围

自动用例使用真实四库表/视图、无 NPM 的业务表/CSV/内存快照、真实来源目录、NPM PCAP/PCAPNG 与 Baseliner，
并使用官方 MCP 客户端连接仓库外启动的已安装包。来源目录验收包含 Kafka/网卡的无读取副作用边界，不声明已验证 Kafka 数据面或实际网卡采集。

统一入口与配置见 [测试说明](testing.md)。独立 MCP 用例另提供已安装测试客户端环境 `FLOWSQL_TEST_MCP_PYTHON`（含 pytest），
真实分析 fixture 复用系统 Python 的 YAML/JSONSchema 依赖。按清单选择 `test_access_*`、`test_mcp_contract`，共享计算回归为
`test_visualization_dataframe_workflow/test_visualization_workflow`。测试结果、原始日志和摘要缓存于本地 `evidence/agent-data-access/`，不提交仓库。
