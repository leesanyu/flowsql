# Baseline 插件说明

本文是 Baseline 插件的长期入口文档，面向调用方、调度层和后续维护者。阶段设计文档记录实现过程和取舍；本文只记录当前对外能力、基本使用方式和必须遵守的接口契约。

## 1. 能力概览

Baseline 插件通过 `IBaselineService` 向同进程内其他插件暴露能力。调用方通过 `IID_BASELINE_SERVICE` 获取服务接口，再创建具体 task。

当前 task 类型：

| 类型 | 接口 | 主要用途 |
| --- | --- | --- |
| Value | `IBaselineValueTask` | 对单值序列建立在线 rolling baseline，支持历史 bootstrap、在线提交、预测、快照和导出 |
| Ratio | `IBaselineRatioTask` | 对比例 / 份额类序列建立在线 rolling baseline，接口形态与 Value 对齐 |
| Relation | `IBaselineRelationTask` | 对关系分布 block 做 routed summary rolling、stream basis、relation fusion 和 source / routed snapshot |

核心能力：

1. 历史 bootstrap：从历史样本训练 artifact / seed，用于冷启动和恢复。
2. 在线 rolling：对新 bucket 提交观测，输出 baseline band、score、trust、maturity 和诊断字段。
3. Relation routed summary：将关系分布拆成有界 routed summary，复用 Value / Ratio rolling core。
4. Relation stream basis：无历史或历史不足时，在线积累并刷新 basis。
5. Relation fusion：将 routed summary evidence 合成为 source 级 relation risk 和 pattern 解释。
6. Snapshot / export：支持 task、series、routed summary、bootstrap artifact / seed 的 JSON 观测和导出。

## 2. 基本使用

### 2.1 获取服务

Baseline 是插件能力，不通过 HTTP 直接暴露。调用方应通过框架的 interface 查询机制获取：

```cpp
auto* service = static_cast<flowsql::IBaselineService*>(
    querier->First(flowsql::IID_BASELINE_SERVICE));
```

具体查询 API 以当前框架接口为准。调用方不应直接依赖 `flowsql::baseline::*` 下的实现类。

### 2.2 创建 task

通过 JSON 配置创建 task：

```cpp
auto [status, task] = service->CreateValueTask(
    config_json,
    flowsql::BaselineSerializationFormat::kJson);

if (status != flowsql::BaselineStatus::kOk || !task) {
    // 处理配置解析或创建失败。
}
```

配置模板参考：

- `src/plugins/baseline/config/baseline-config-template.yaml`

目前 public 序列化格式以 JSON 为主。接口保留 `BaselineSerializationFormat` 参数，调用方不应假设未来只存在 JSON。

### 2.3 Value / Ratio 主流程

典型流程：

1. `CreateValueTask()` / `CreateRatioTask()` 创建 task。
2. 可选：`Bootstrap()` 或 `LoadBootstrapArtifact()` 预热历史模型。
3. 调用 `SubmitObservation()` 提交在线 bucket。
4. 调用 `PredictRolling()` / `PredictBootstrap()` 做只读预测。
5. 调用 `QueryTaskSnapshot()` / `QuerySeriesSnapshot()` 观测运行时状态。
6. 调用 `ExportBootstrapArtifact()` / `ExportBootstrapSeed()` 导出恢复数据。
7. 调用 `Close()` 关闭 task。

`PredictRolling()` 是只读预测接口，不应触发状态初始化或在线学习。在线学习只通过 `SubmitObservation()` 推进。

### 2.4 Relation 主流程

典型流程：

1. `CreateRelationTask()` 创建 Relation task。
2. 可选：`Bootstrap()` 或 `LoadBootstrapArtifact()` 加载历史 relation basis 和 routed summary seed。
3. 调用 `SubmitObservation()` 提交在线 relation block。
4. 调用 `PredictRoutedSummary()` 预测某个 routed summary。
5. 调用 `QuerySeriesSnapshot(source_series_key)` 观测 source 级状态和 relation fusion。
6. 调用 `QueryRoutedSummarySnapshot(query)` 观测某个 routed summary 的底层 rolling 状态。
7. 调用 `ExportBootstrapArtifact()` / `ExportBootstrapSeed()` / `QueryBootstrapBasis()` 导出或观测 bootstrap 结果。
8. 调用 `Close()` 关闭 task。

Relation 的 `RelationRollingObservation.metrics` 必须与 task config 中的 `metrics` 数组同序对齐。`RelationBootstrapMetric.metric` 是可选名称校验字段；若非空，必须等于同下标的 task metric。

## 3. 接口契约

### 3.1 Task 所有权

Task 由 `std::shared_ptr<IBaseline*Task>` 持有。`Close()` 会关闭 task 并从 registry 移除，但调用方已经持有的 `shared_ptr` 不会立即失效。

调用方应将 `Close()` 视为 task 调用序列中的状态迁移点：

1. 排在 `Close()` 之前的调用先完成。
2. 排在 `Close()` 之后的调用应看到 closed 状态并拒绝。
3. `Close()` 后不应继续把该 task 暴露给新的业务调用链。

### 3.1.1 可选状态管理

可选状态管理通过独立头 `framework/interfaces/ibaseline_state_control.h` 和
`IID_BASELINE_STATE_CONTROL_SERVICE_V1` 提供，不改变原服务、task 虚表或序列化格式。
创建 task 后、首次 `SubmitObservation` / `Bootstrap` / `LoadBootstrapArtifact` 前可绑定：

```cpp
auto* management = static_cast<flowsql::IBaselineStateControlServiceV1*>(
    querier->First(flowsql::IID_BASELINE_STATE_CONTROL_SERVICE_V1));
auto [bind_status, control] = management->Bind(task, {1024, 1024, 2});
```

三个限额分别为 runtime 身份、父模型身份、每 source×metric 的 basis 版本数，必须为正值；
Relation 的版本数至少为 2，Value/Ratio 忽略该正值。限额绑定后固定，原 task 入口也执行容量检查。
未绑定任务保留原准入和历史行为。控制句柄拥有 task，任务、句柄及产物须先于插件库销毁。

`ReleaseIdentity(key, scope)` 的 key 在 Value/Ratio 中是 series，在 Relation 中是 source，
不能用已存在的 routed 子 key 代替 source。不存在的身份幂等成功；两种 scope 的行为为：

| scope | 释放结果 |
| --- | --- |
| `kRuntimeOnly` | 清除 rolling、消费游标和最近 Submit 视图；Relation 同时清除 basis、fusion/persistence、routed spec/seed/rolling 与 source 索引。保留父 artifact/seed，下一条合法观测可恢复模型。 |
| `kAllState` | 在上述基础上清除该身份的父 artifact/seed，下一条观测按无模型冷启动。 |

两种释放均结束当前在线生命周期，允许下一生命周期重新处理此前消费过的 bucket；
保留 seed 时仍拒绝不晚于其训练末尾的 bucket。跨生命周期的旧数据过滤由调用方的水位/恢复策略保证。
RuntimeOnly 后在线快照返回 `kNotTrained`，Value/Ratio 的 Bootstrap 预测继续有效。

`QueryUsage()` 独立返回 runtime 身份、父模型身份、routed 子身份和保留版本数量。
routed 子身份包括仅有 spec/seed、尚未生成 rolling 的预热对象，同一个子身份计一次；
Relation 的 cursor-only source 也占 runtime 额度。它是对象数量口径，不是精确字节预算。
容量满时拒绝新增身份，返回既有 `kInvalidArgument`；已有身份继续处理，不自动淘汰其他身份。
超限 Bootstrap / Load 在提交资产前拒绝，单源重训不复活其他已释放 runtime。
受管 Relation 导入同时核对 basis 的 support/stable 大小和每 metric、每版本的预热子模型扇出：
universal 至多 4 个，basis scoped 至多 `3 + k_stable` 个，防止导入文档绕过固定配置的对象数量边界。

受管 Relation 在成功提交点清理最旧且不受 active/handover 保护的在线历史版本，
同时删除对应 routed spec/seed/rolling；精确查询/预测退休版本返回 `kNotTrained`，父 artifact 保留。
fusion 的 `fusion_persistence_max_keys_per_source` 实际约束保存的历史键数；
超过上限时复用既有证据表的字典序保留前若干键，其余证据继续参与当条计算但不累计跨条 persistence。
内部 cap=0 保留原不限量语义，通常配置使用有限正值。

`Close()` / 插件 `Stop()` 释放全部模型、runtime 容器及其容量和日历引用，外部句柄仍存续也不保留这些资源。
关闭后的管理调用拒绝，重复 Close 仍幂等；不保证 allocator 立即向操作系统归还 RSS。

### 3.2 同 task 非并发调用契约

Baseline task 实例按外部串行化状态机理解。调用方 / 上游调度必须保证：

1. 同一个 task 的 public API 调用不会重叠执行。
2. 如果同一个 task 的连续调用发生在不同物理线程，上游调度必须在前一次调用结束与后一次调用开始之间建立 happens-before，保证 task 内部非 atomic 状态的可见性。
3. 同一个 task 不要求固定物理线程，Baseline 也不做线程身份检查。
4. 不同 task 可以并行调用。

当前实现按上述契约收敛 task 内部 runtime 锁。调用方不得直接并发访问同一个 task，也不得依赖 Baseline 在 task 内部为重叠调用提供互斥保护。

Bind、ReleaseIdentity 和 QueryUsage 也遵守同 task 的串行契约；本能力不增加维护线程或 runtime 锁。
插件不判断业务不活跃、不读取业务超时 wall clock，也不连接 Scheduler。
业务活动判断、时间触发和维护调用的串行化由后续封装算子承担。

### 3.3 Immutable identity getter

以下 getter 是 task identity 能力，在 task 对象生命周期内必须保持跨线程读取能力：

1. `Id()`
2. `Name()`
3. `Kind()`

这些字段构造后不可变，getter 不应依赖 task runtime mutex。除这 3 个 immutable identity getter 外，其他 task public API 默认都遵守“同 task 不重叠执行”的契约。

### 3.4 Service、registry 与生命周期边界

`IBaselineService`、`TaskRegistry`、plugin lifecycle 和 runtime config 是 task 契约之外的并发边界：

1. `TaskRegistry` 负责 task 表和 task id 分配，内部保留短临界区同步。
2. Runtime config 使用 immutable snapshot 方式整体替换。
3. Plugin lifecycle 是否可与 service API 并发，由框架生命周期契约决定；若框架不保证串行，Baseline 需要单独的 plugin lifecycle 保护。

### 3.5 Snapshot 与导出

Snapshot 用于观测和调试，不应被调用方当成热路径状态传递格式。进程内阶段交接应使用结构体，JSON 只服务于边界导出、审计、调试和恢复。

当前 public snapshot / export 的稳定入口：

| 能力 | 接口 |
| --- | --- |
| task 配置导出 | `ExportConfig()` |
| task 快照 | `QueryTaskSnapshot()` |
| series 快照 | `QuerySeriesSnapshot()` |
| bootstrap artifact 导出 / 导入 | `ExportBootstrapArtifact()` / `LoadBootstrapArtifact()` |
| bootstrap seed 导出 | `ExportBootstrapSeed()` |
| Relation routed summary 快照 | `QueryRoutedSummarySnapshot()` |
| Relation bootstrap basis 查询 | `QueryBootstrapBasis()` |

## 4. 维护要求

新增或修改 Baseline public 能力时，需要同步检查：

1. `src/framework/interfaces/ibaseline_service.h`
2. `src/framework/interfaces/ibaseline_types.h`
3. `src/plugins/baseline/config/baseline-config-template.yaml`
4. `src/tests/test_baseline/*`
5. 本 README

新增配置项必须同步更新 C++ 默认值、YAML 模板、strict schema 和配置测试。新增 public 字段必须保持 append-only 兼容策略，除非阶段设计明确允许破坏性迁移。

常规回归共 12 个 CTest，统一标记为 `baseline`，测试工作目录为 `build/output`：

```bash
cmake -B build src
cmake --build build -j4
ctest --test-dir build -L baseline --output-on-failure
ctest --test-dir build --output-on-failure
```

`test_baseline_task_headers` 复用插件的精确编译对象，自动运行隐藏 task 实现的 Close/Stop 资源回归；不需要手工链接。既有 `-UNDEBUG` 保证 Release 构建也执行测试断言。

`test_baseline_link_bootstrap_eval`、`test_baseline_link_rolling_eval` 和 `test_baseline_batch_prediction_perf` 用于独立评估或性能测量，不注册到常规 CTest。
