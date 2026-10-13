# Baseliner 使用说明

`explore.baseliner` 从数据库表或已注册的 DataFrame 读取时间序列，评估当前桶的实际值、预期值、
上下界和偏离程度，并按配置生成未来预测。一个任务可以同时使用 Value、Ratio 和 Relation 模型。

## 运行准备与输入输出

执行任务的 Scheduler 进程需要加载 `libflowsql_baseline.so`；通过 Web「算子管理 → C++ 插件」
上传并激活 `build/output/libflowsql_baseliner.so`。Baseline 插件必须提供 Value、Ratio、Relation 的
V1 评估就绪能力，缺少任务所需能力会在打开任务时明确失败。

| 位置 | 支持范围 | 使用方式 |
| --- | --- | --- |
| 输入数据库 | SQLite、MySQL、PostgreSQL、ClickHouse | 两段通道名配置多个 dataset，或三段表名指定一张表 |
| 输入 DataFrame | `dataframe.<name>` | 有限快照；CSV 先上传并注册为 DataFrame |
| 两段数据库输出 | SQLite、MySQL、PostgreSQL | 托管结果、模型 checkpoint 与消费位置，支持联合恢复 |
| 三段结果表 / DataFrame | SQLite、MySQL、PostgreSQL，或 `dataframe.<name>` | 有限 snapshot，保存 evaluation 与 forecast |
| `model_output` | 三段 SQLite/MySQL/PostgreSQL 表，或 `dataframe.<name>` | 有限 snapshot，独立导出最终模型参数 |

配置使用 `WITH config='config.<name>@<revision>'` 的确切版本，或 `WITH parameters='<JSON 原文>'`，
两者选择其一。任务持有启动时的配置版本，不跟随 latest；SQL 字符串中的单引号按 SQL 规则转义。

## 单表和 DataFrame 快速开始

以下配置按分钟汇总每条链路的 bytes，并预测未来两个桶。将实际输入字段映射到 `fields`；
`series_keys` 决定独立建模的业务身份，`deduplicate.keys` 决定输入去重键。

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

将 JSON 放入 parameters，即可指定结果与最终模型的两个出口：

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

DataFrame 默认分析整个快照；限定桶范围时保留 begin/end，将 consistency 改为 `dataframe_snapshot`。
输入字段类型必须符合配置，支持整数无损扩宽及 float32→float64；缺列、溢出或不支持的类型明确失败。
快照先按时间和稳定键排序再分页，保留原通道的内容、Schema 和顺序。排序、转换、分页和结果缓冲受
`read_policy.max_pending_bytes` 约束，减小 page_rows 不会消除完整快照排序的内存成本。

两段数据库来源使用完整 `datasets` 配置；单数 `dataset` 是明确单表或 DataFrame 的简写。
普通业务表应选择固定不可变范围或一致快照；ClickHouse 输入要求已完成 run 或调用方保证的不可变范围。

## 从 NPM 周期结果建立基线

先运行独立的 NPM 任务，再分析其 Basic history。下例中的通道需提前配置，持续采集通过
`/scheduler/batch/submit` 异步提交；NPM 的完整使用方式见 [NPM 说明](npm.md)。

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

以下配置读取已完成运行的 IPv4 结果，按观测域汇总完整周期的字节增量。
将 `completed-run-id` 替换为 NPM 响应中的明确 run_id；IPv6 使用 `npm_basic_ipv6_history_v1`，
可在 datasets 中另配置一项并绑定同次运行。实际业务身份应按需要设置 series_keys。

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
    "table": "npm_basic_ipv4_history_v1",
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

通过 `POST /channels/config/publish` 发布配置，字段为 `name=link_baseline`、`expected_current_revision=0`、
`format=json`、`schema_id=baseliner.task.v1`，以及 JSON 原文的 `content_base64`。
执行 SQL 时使用响应的 exact_reference，例如 `config.link_baseline@1`。

有限 PCAP 落库可使用 snapshot。持续 poll 需要生产者提供真实的 epoch、已提交位置、不可变前缀和桶关闭证据；
单独完成有限 PCAP 落库不会自动获得该能力。各族关系独立推进至源确认的闭合边界，无包族也可推进；
写入或发布失败后运行为 incomplete，保留已提交前缀，不将旧事实作为新的成功进度。

## 理解评估、预测和模型输出

基线桶宽 B 必须为 NPM 周期 P 的正整数倍。按 `interval_*` 增量聚合，过滤不完整周期；
累计值和零增量终态不能作为新的学习事实。相同身份下的多会话需要汇总，数据库读取按稳定键分页，
由算子在有界内存中关闭桶；一次分页不等于一次模型更新。Ratio 分别汇总分子、分母后求比值，
Relation 按组分布评估并输出 routed 结果。

当前桶先用更新前模型评估，再按策略学习。冷启动或无有效预测时，observed 保留，expected/lower/upper 为 NULL；
从当前观测首次初始化成功仍属于该桶冷启动。bootstrap 或恢复的已有模型可使首条观测直接就绪。
评估就绪不代表允许学习或告警，应分别读取相应状态。forecast 没有 actual，并带有 `issued_after_bucket`
与 `model_basis_id`；同一未来桶在不同时间发布的预测可以并存。

两段托管出口的结果在独立版本表中，不给 NPM 源表追加预测列：

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

指定三段结果表或 DataFrame 时，仅写入 evaluation、forecast（含 Relation routed），不混入模型参数、
Relation fusion 或 maintenance。没有序列的 DataFrame 仍具有 results Schema。

可选 `model_output` 在正常 EOF 后导出每个 dataset/metric/series/epoch 的最终模型，包含 config_hash、
model_kind、model_basis_id、as_of_bucket、status、maturity、parameters_version 和 parameters_json。
未训练模型的 parameters_json 为 NULL，无序列时保留 model_parameters 空 Schema。
参数版本 1 提供滚动系数、尺度、协方差、季节/月位置、bootstrap 分量及 Relation basis/routed 参数。
导出只读，不重复训练，也不导出完整 checkpoint；最终参数不能代表每条历史预测使用的模型。
该选项不改变配置原文、确切版本或恢复兼容 hash，省略时不序列化最终参数。

## 发布结果与检查出口状态

指定结果和模型的目标表不存在时自动创建，已有表严格检查列、类型、可空性与主键；MySQL 要求 InnoDB。
按 logical_key 幂等写入，保留其他任务或逻辑键的数据。输入与输出、结果与模型不能指向同一实际表或 DataFrame，
即使通道名不同也会在处理前拒绝。模型目标不能覆盖托管保留表。

指定结果表与模型表在同一实际数据库时共用一次事务，任一失败共同回滚。
跨数据库、数据库与 DataFrame 混合、两个 DataFrame 分别提交或登记；仅在两个出口都完成后报告 completed。
DataFrame 在正常完成后登记，EOF 前失败或取消不替换已有同名通道；登记阶段按现有替换规则处理。

响应的 `result.results_output` 和可选 `result.model_output` 分别提供 target、status、rows_written。
status 为 pending/staged/committed/failed/cancelled/unknown，rows_written 只计确认提交或登记的行。
例如结果已提交、模型登记失败，任务会失败，同时如实保留结果的 committed 状态与行数。
unknown 表示提交结果不确定，任务失败且不会自动重试；已登记出口也不会因随后取消而改称未提交。
结果缓冲受 `read_policy.max_pending_bytes` 限制，最终参数还受 `persistence.max_checkpoint_bytes` 限制。

三段表和 DataFrame 输出不提供 checkpoint/消费位置联合恢复，published_generation 为 0；
重放重新计算，结果表按逻辑键写入。需要联合恢复、Relation fusion 和维护结果时使用两段托管出口。
两段托管出口的可选模型导出失败不撤销已经发布的 generation，可从响应分别核查。

## 恢复、持续运行与停止

两段数据库托管输出在一个 generation 内原子发布模型、消费位置和结果。
`checkpoint_every_buckets` 与 `checkpoint_interval_ms` 控制发布频率；完整 checkpoint 的序列化和提交是同步成本。
无数据时仍由框架时间驱动维护，TTL 默认禁用。输入索引和发布超时的配置建议见
[README 的基线输入说明](../README.md#数据库输入表的索引建议)。

恢复使用相同 task_key 与兼容配置：`require` 要求已有模型，`if_exists` 有模型则恢复，`fresh` 用于新的任务身份。
来源 epoch、发布位置、保留期或 Schema 不兼容时明确失败；重复输入不重复学习或发布。
同步 result、异步 managed_result 提供 task_key、run_id、配置 hash、generation、关系名与消费位置。
托管 rows_written 计本次确认提交的结果、Relation 融合和维护行；没有新输入的恢复可以为 0，历史结果仍可查询。

poll 通过异步入口运行，不把空轮询当 EOF。`POST /scheduler/batch/stop` 请求
`{"runtime_task_id":"...","mode":"stop"}` 会唤醒 reader 并正常提交尾批；cancel 或执行错误不发布成功尾批。
部署使用配套加载器，并声明 Scheduler 的 task runtime 角色，使其在插件卸载前停止接纳、取消并等待活动任务退出。
资源在所有在途调用退出后关闭；仅替换 Baseline 插件不能提供完整的停机顺序保证。

## 性能说明

以下是 2026-10-11 的固定负载样本，本次文档整理没有重新测量。环境为 Intel i9-13900H、20 逻辑 CPU、
WSL2/Linux x86_64，仓库普通构建，成本目标未启用优化 flags；固定 CPU 0，各五轮。
测量时存在其他构建和测试负载，并非独占机器；表中总耗时为中位数 [min, max]，CPU 与 RSS 为中位数。

第一组使用 960 条标准观测（40 桶 × 8 身份 × 3 类模型）、24 个模型身份，Relation 为 3 组/1 指标，
预测 horizon=2，Arrow 每批 24 条，checkpoint_every_buckets=7。各路径生成相同的 5,760 行结果。

| 路径 | 总耗时 ms | CPU ms | 峰值 RSS MiB |
| --- | ---: | ---: | ---: |
| 直接评估引擎 | 108.519 [103.342, 130.965] | 100.228 | 30.164 |
| Arrow 算子 | 308.408 [293.286, 312.148] | 302.745 | 32.766 |
| SQLite 托管 | 749.265 [739.379, 762.001] | 625.871 | 45.938 |
| SQLite 托管，每提交增加 2 ms | 775.012 [762.485, 814.487] | 627.478 | 45.980 |

第二组读取同一份 960 行原始 DataFrame，比较物理顺序、字段类型和出口；结果均为 5,760 行，
有模型出口时另输出 24 行最终模型。该组的输入及计时范围与标准观测组不同，应在组内比较。

| 原始 DataFrame 路径 | 总耗时 ms | 峰值 RSS MiB |
| --- | ---: | ---: |
| 有序宽类型 → DataFrame 结果 | 170.144 [165.554, 195.416] | 46.094 |
| 逆序宽类型 → DataFrame 结果 | 161.689 [159.179, 166.617] | 46.109 |
| 逆序窄类型 → DataFrame 结果 | 164.256 [160.784, 209.114] | 46.984 |
| 逆序窄类型 → DataFrame 结果及模型 | 169.171 [166.192, 246.674] | 47.305 |
| 逆序窄类型 → SQLite 结果及模型 | 385.314 [368.393, 686.152] | 41.105 |

原始 DataFrame 计时包括输入 Open/Next、聚合、评估、预测、编码、EOF 与输出，不包括 CSV 上传、HTTP 或源库扫描。
没有独立测量排序和类型转换，不能将两个总耗时相减作为它们的成本。托管路径还包含 checkpoint 与数据库发布，
较短的 checkpoint 间隔会增加同步成本；这些小样本不能作为大表扫描、实时吞吐或生产容量承诺。

详细数据契约见 [托管算子规格](../tasks/archive/feat-baseline-operator.md) 和
[单表与 DataFrame 规格](../tasks/archive/feat-baseliner-single-source.md)。
