# Baseliner 使用说明

`explore.baseliner` 读取数据库表或已注册的 DataFrame，计算当前桶的 actual、expected、上下界及偏离结果，并按配置生成未来预测。
Value、Ratio 和 Relation 可以在同一个配置中使用。当前支持 SQLite、MySQL、PostgreSQL、ClickHouse 来源；
模型和结果托管目标支持 SQLite、MySQL、PostgreSQL。

## NPM 落库与基线分析

先运行独立的 NPM 任务，再运行基线任务。下面的 `mysql.npm`、`mysql.baseline` 是已配置的数据库通道，
`netadapter.eth0` 是已配置的采集来源。持续来源需要通过 `/scheduler/batch/submit` 异步提交。

```sql
SELECT * FROM netadapter.eth0
USING npm.basic
WITH input_namespace='netadapter.eth0', source_domains='0:1',
     features='basic', output_interval_ns=60000000000
INTO mysql.npm

SELECT * FROM mysql.npm
USING explore.baseliner WITH config='config.link_baseline@1'
INTO mysql.baseline
```

有限 PCAP 的 NPM 结果可通过 snapshot 分析。poll 要求来源具备真实的已提交位置、epoch、不可变前缀和桶关闭证据；
单独运行有限 PCAP 离线落库不会自动提供采集进度能力，缺少进度时 poll 会明确拒绝。

下面的配置读取已经完成的 run，按采集域聚合完整周期的字节增量；将 `completed-run-id` 替换为 NPM 响应的 `run_id`。
`observation_domain_id` 是本例的稳定身份，实际部署应按业务对象配置 `series_keys`。

```json
{
  "schema_version": 1,
  "task_key": "link-byte-baseline",
  "source": "mysql.npm",
  "mode": "snapshot",
  "clock": {"bucket_seconds": 60, "timezone": "Asia/Shanghai"},
  "calendar": {"calendar_id": "cn-holiday", "calendar_version": "2026.1"},
  "forecast": {"horizon_buckets": 2},
  "read_policy": {"page_rows": 4096, "max_pending_bytes": 16777216},
  "persistence": {"checkpoint_every_buckets": 7, "retain_generations": 2, "restore": "if_exists"},
  "datasets": [{
    "id": "link",
    "table": "npm_basic_history_v1",
    "fields": {
      "__npm_run_id": "utf8", "session_id": "uint64", "revision": "uint64",
      "observation_domain_id": "uint64", "period_start_ns": "int64", "period_end_ns": "int64",
      "period_complete": "boolean", "interval_wire_bytes_total": "uint64"
    },
    "scope": {"run_ids": ["completed-run-id"]},
    "series_keys": ["observation_domain_id"],
    "row_semantics": "npm_period_increment",
    "deduplicate": {"keys": ["__npm_run_id", "session_id", "revision"], "on_duplicate": "require_equal"},
    "bucket": {"column": "period_start_ns", "unit": "ns"},
    "filter": [{"column": "period_complete", "op": "eq", "value": true}],
    "metrics": [{
      "id": "bytes", "kind": "value", "column": "interval_wire_bytes_total",
      "aggregate": "sum", "feature_type": "value_basic", "profile": "default"
    }]
  }]
}
```

通过 `POST /channels/config/publish` 发布 JSON 内容。请求包含 `name=link_baseline`、
`expected_current_revision=0`、`format=json`、`schema_id=baseliner.task.v1` 和 JSON 原文的 `content_base64`。
使用响应的 `exact_reference`，例如 `config.link_baseline@1`；在线任务持有该确切版本，不跟随 latest。
也可使用 `WITH parameters='<JSON 原文>'`，SQL 字符串中的单引号须按 SQL 规则转义。

普通业务表应显式配置关系名和逻辑字段类型。snapshot 可以使用固定不可变范围或一致快照；
ClickHouse 要求已完成 run 或调用方保证的不可变范围。一般业务表的 poll 还需要其生产者提供同一版本化发布能力。

## 指定单表与 CSV 离线输入

测试指定表可以直接使用 `FROM mysql.npm.samples`；CSV 上传后先按既有导入流程注册为
`dataframe.samples`，再使用同一算子。少量指标可在 `WITH parameters` 内使用单数 `dataset` 简写：

```json
{
  "task_key": "samples-baseline",
  "clock": {"bucket_seconds": 60, "timezone": "UTC"},
  "calendar": {"calendar_id": "cn-holiday", "calendar_version": "2026.1"},
  "forecast": {"horizon_buckets": 2},
  "dataset": {
    "fields": {"bucket": "int64", "link": "utf8", "bytes": "float64"},
    "scope": {"begin_bucket": 0, "end_bucket": 120, "consistency": "consistent_snapshot"},
    "series_keys": ["link"],
    "deduplicate": {"keys": ["bucket", "link"], "on_duplicate": "require_equal"},
    "bucket": {"column": "bucket", "unit": "bucket_id"},
    "metrics": [{"id": "bytes", "kind": "value", "column": "bytes", "aggregate": "sum",
                 "feature_type": "value_basic", "profile": "default"}]
  }
}
```

三段数据库来源使用上述 JSON；DataFrame 来源删除 `dataset.scope`，默认分析整个快照。
如果要限制 DataFrame 桶范围，可保留 begin/end 并把 consistency 改成 `dataframe_snapshot`。
完整 `datasets` 格式和 `config.<name>@<revision>` 精确引用继续支持；单数简写用于明确的单张表/DataFrame。

```sql
SELECT * FROM mysql.npm.samples
USING explore.baseliner WITH parameters='<上述 JSON>',
     model_output='mysql.analysis.final_models'
INTO mysql.analysis.predictions

SELECT * FROM dataframe.samples
USING explore.baseliner WITH parameters='<上述 JSON，删除 dataset.scope>',
     model_output='dataframe.final_models'
INTO dataframe.predictions
```

输入的实际字段类型必须符合逻辑配置。DataFrame 支持整数无损扩宽及 float32→float64，
溢出、缺列或不支持的类型明确失败。快照按时间和稳定键排序再分页，原通道的内容、Schema 和顺序保持。
排序、转换、分页和结果缓冲受 `read_policy.max_pending_bytes` 约束；减小 page_rows 不会消除完整快照排序的内存成本。
数据库新出口与模型参数导出只接受有限 snapshot；原两段数据库 poll 继续要求异步运行和真实发布进度。

## 独立导出最终模型参数

有限 snapshot 可以在顶层 WITH 增加 `model_output`，与 `config` 或 `parameters` 并列。
它只选择输出位置，不改变配置原文、精确配置版本、配置 hash 或恢复兼容 hash；两段数据库来源仍支持多个 dataset。
未指定时不遍历或序列化最终模型。三段数据库来源与上传 CSV 注册的 DataFrame 也支持该选项。

```sql
SELECT * FROM mysql.npm
USING explore.baseliner WITH config='config.link_baseline@1',
     model_output='mysql.baseline.final_models'
INTO mysql.baseline

SELECT * FROM dataframe.samples
USING explore.baseliner WITH parameters='<已归一的单表配置 JSON>',
     model_output='dataframe.final_models'
INTO dataframe.predictions
```

模型目标支持 SQLite/MySQL/PostgreSQL 三段表或 `dataframe.<name>`。正常 EOF 后，每个 dataset/metric/series/epoch
输出一行：身份及 config_hash、model_kind、model_basis_id、as_of_bucket、status、maturity、parameters_version 和 parameters_json。
`as_of_bucket` 是最后已处理桶；最终参数不代表每条历史预测所使用的模型。未训练模型的 parameters_json 为 NULL，
没有序列时仍输出带 model_parameters Schema 的空 DataFrame。参数版本 1 包含实际滚动系数、尺度、协方差、季节/月位置及
bootstrap 分量；Relation 包含明确身份、版本的 basis 和 routed 参数。导出只读，不重复训练，不导出完整 checkpoint。

目标表不存在则创建；已存在时校验列、类型、可空性及主键，保留其他逻辑键的记录，同一逻辑键重放幂等写入。
表包含用于幂等性的 logical_key；不能与来源表、主要结果或旧托管保留表重合。同数据库通道的不同表可以使用。
DataFrame 模型先暂存，在正常完成后按既有通道规则登记；EOF 前导出失败或取消时不改变已有同名模型。
登记阶段失败会使任务失败，响应保留各出口的实际状态；DataFrame 使用现有通道替换规则。
参数和缓冲分别受 max_checkpoint_bytes、max_pending_bytes 限制。
响应的 result.model_output 提供 target、status 和 rows_written；unknown 表示提交结果不确定，任务失败且不会自动重试。

该选项不支持 poll；省略它的旧 poll、托管多 Schema 输出及联合恢复保持。附加参数导出失败会使任务失败，
此前旧托管路径已经提交的 generation 仍有效，可从响应的 generation、rows_written 和模型输出状态核查。

## 指定预测结果表或 DataFrame

`INTO` 选择主要结果位置，`WITH model_output` 独立选择最终模型参数位置：

```sql
SELECT * FROM mysql.npm.samples
USING explore.baseliner WITH parameters='<单表任务 JSON>',
     model_output='mysql.analysis.final_models'
INTO mysql.analysis.predictions

SELECT * FROM dataframe.samples
USING explore.baseliner WITH parameters='<单表任务 JSON>',
     model_output='dataframe.final_models'
INTO dataframe.predictions
```

三段结果表支持 SQLite/MySQL/PostgreSQL，要求 snapshot。表不存在时创建，已存在时严格校验列、类型、可空性和主键；
MySQL 要求 InnoDB。表增加 `logical_key` 作为主键，按原结果逻辑键幂等写入，保留其他任务或逻辑键的记录。
evaluation 与 forecast 共用 results Schema；Relation routed 结果也写入该 Schema。模型参数、Relation fusion、
maintenance 不混入指定结果表或 DataFrame。没有序列时，DataFrame 仍保留 results Schema 的零行结果。

来源和目标可以位于同一实际数据库的不同表；覆盖来源表/DataFrame、模型与结果实际同目标会在处理输入前拒绝。
判定使用实际数据库身份，通道名称不同也适用。两段数据库输入仍可配置多个 dataset，其输出模式只由 INTO 决定。

新指定表模式下，两张输出表位于同一实际数据库时，结果和模型参数共用一次事务发布；任一写入失败则两表均回滚。
跨数据库、数据库与 DataFrame 混合、两个 DataFrame 使用独立提交/登记；任务只有两个出口均完成才报告 completed。
例如结果已经提交而模型登记失败，任务返回错误，`result.results_output.status=committed` 和实际行数仍保留，
`result.model_output.status=failed`、行数为 0，不宣称已提交数据被撤销。

响应中的 `result.results_output` 和可选 `result.model_output` 各包含 `target`、`status`、`rows_written`；
status 可为 pending/staged/committed/failed/cancelled/unknown，只有确认提交或登记的行计入 rows_written。
提交结果不确定时任务失败并报告 unknown，不自动重试。EOF 后若取消与登记确认交错，已登记的出口仍如实报告 committed。
输出缓冲受 `read_policy.max_pending_bytes` 限制；最终参数另受 `persistence.max_checkpoint_bytes` 限制。

新指定结果表和 DataFrame 不自动提供 checkpoint/消费位置的联合恢复，`published_generation` 为 0；
重放会重新计算并按逻辑键写入结果表。需要多 Schema 产品及联合恢复时，继续使用原 `INTO mysql.analysis` 两段托管出口。
原多 dataset、poll 异步、generation/checkpoint、TTL、Relation fusion/维护输出保持；可选模型导出失败也不撤销其已提交 generation。

## 时间桶、批量与模型更新

Baseline 桶宽 `B` 必须为 NPM 周期 `P` 的正整数倍。`B=P` 与同桶多会话汇总是常见路径；`B=2P` 等情况按完整桶合并，
没有拆桶、插值或非整数倍重采样。NPM 使用 `interval_*` 增量字段，累计字段和零增量终态不会作为新的学习事实。

数据库读取按稳定键分页，不执行 SQL `GROUP BY`。算子在有界内存中汇总并关闭目标桶：即使 `B=P`，
相同业务身份下的多个会话仍可能需要求和。分页/Arrow block 可以包含多个身份和时间桶；核心算法按身份逐桶、按时间顺序调用，
不会把一次数据库分页等同于一次模型更新。Ratio 使用分子、分母分别汇总后求比值；Relation 保留组分布并输出路由摘要。

当前桶先用更新前模型评估，再按策略学习。冷启动或没有有效预测时，保留 observed，expected/lower/upper 为 NULL。
forecast 不包含 actual，保留 `issued_after_bucket` 和 `model_basis_id`；同一未来桶的不同发布时间可以并存。

两段数据库托管模式下，`checkpoint_every_buckets` 和 `checkpoint_interval_ms` 控制持久发布频率。模型、消费位置、结果在一个 generation 内原子发布，
完整 checkpoint 的序列化和提交是同步成本。无数据时仍由框架时间驱动维护；TTL 默认禁用。

## 查询、恢复和关闭

结果写入独立的版本化表，不向 NPM 源表追加预测列：

```sql
SELECT metric_id, target_bucket, observed, expected, lower, upper, score, can_alert,
       band_kind, basis_id, model_basis_id, published_generation
FROM mysql.baseline.baseline_results_v1
WHERE task_key='link-byte-baseline' AND result_kind='evaluation'
ORDER BY target_bucket
INTO dataframe.current_baseline

SELECT metric_id, target_bucket, issued_after_bucket, expected, lower, upper, model_basis_id
FROM mysql.baseline.baseline_results_v1
WHERE task_key='link-byte-baseline' AND result_kind='forecast'
ORDER BY target_bucket, issued_after_bucket
INTO dataframe.future_baseline
```

同步响应的 `result`、异步状态响应的 `managed_result` 提供 task_key、run_id、配置 hash、generation、结果/模型关系及消费位置。
`rows_written` 是本次执行成功提交的结果、Relation 融合和维护行数。重启没有新输入时为 0，历史持久结果仍可查询。

两段数据库托管模式的恢复使用相同 task_key 和兼容配置。`require` 要求已有模型，`if_exists` 有模型则恢复；`fresh` 用于新的任务身份。
来源 epoch、发布位置、保留期或 Schema 不兼容时明确失败，不自动重试未知提交。重复输入不重复学习或发布。

poll 不把空轮询当 EOF。`POST /scheduler/batch/stop` 请求 `{"runtime_task_id":"...","mode":"stop"}`
会唤醒 reader 并正常提交尾批；`mode=cancel` 或执行错误不发布成功尾批。所有在途调用结束后才关闭资源或卸载库。

托管基础契约见 [原算子规格](../tasks/archive/feat-baseline-operator.md)，单表与两出口契约见
[单表与 DataFrame 规格](../tasks/archive/feat-baseliner-single-source.md)，生产条件、五轮排序/转换与模型导出测量见
[验收记录](baseliner-acceptance.md)。
