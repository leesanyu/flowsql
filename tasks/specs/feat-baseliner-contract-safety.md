# Baseline/Baseliner 生命周期与评估契约修复

状态：规格切片已完成；T1～T4 均待实施。优先级：P1。
前置能力：[Baseline 算子](../archive/feat-baseline-operator.md)、[单表与 DataFrame 分析及输出](../archive/feat-baseliner-single-source.md)。

## 业务意图与 Non-Goals

为通过 Scheduler 加载 `.so` 并调度 Baseline 分析的用户，修复停机时算法状态被提前清理、
取消时输出会话被提前关闭，以及算子通过诊断文本猜测冷启动的问题；交付可验证的任务排空、
会话安全释放和类型化评估有效性保证，保持已有分析结果、源数据与持久化行为。

本 Feature 不调整算法公式、训练策略、阈值、结果表 Schema、配置或 checkpoint 格式；
不处理结果发布阶段的取消空窗、通用 DataFrame 查询/CSV 展示、一般去重或文件重构；
不引入通用插件依赖图、算法全局锁、专属工作线程或新的数据库公共接口。

责任边界：Baseline 插件拥有算法、模型状态和本次评估的就绪判定；Baseliner 算子负责数据适配、
任务封装与输出会话；加载器和 Scheduler 负责启动/停止顺序及活动任务排空。

## 已核查问题与证据

| 编号 | 已核查路径 | 修复后的可观察保证 |
| --- | --- | --- |
| P1-1 | [加载器](../../src/common/loader.hpp) 逆启动顺序 Stop；生产测试将 Scheduler 注册在 Baseline 前，导致 [Baseline Stop](../../src/plugins/baseline/baseline_plugin.cpp) 先 Close 活动任务并清空算法状态 | 不论这两者的配置排列，Scheduler 停止接纳并取消/join 活动任务后，Baseline 才开始清理 |
| P1-2 | [ModelOutputStore](../../src/operators/baseliner/model_output.cpp) 的 Join 显式 Close 旧会话；另一线程 Cancel 持有的 shared_ptr 不能阻止内部数据库句柄被 Close | 在途 Cancel 持有完整会话及通道租约，最后使用者退出后才关闭句柄 |
| C1 | [evaluation.cpp](../../src/operators/baseliner/evaluation.cpp) 通过 uncertainty_source 中的 `first_observation` 判断冷启动 | 算子只消费插件类型化状态，诊断内容不决定预测字段是否有效 |

前轮只读探针输出：`PROBE before Scheduler::Stop algorithm_task_closed=1`、
`PROBE join rc=0 Close_called_during_Cancel=1`；日志在 `/tmp/baseliner-code-review/stop.log`、`join.log`。
它们证明了错误顺序/调用重叠，未声称已经触发真实 UAF 崩溃；修复时将确定性交错转为仓库回归。

## 核心契约

### 1. 加载与退出

保持所有插件按批次 Option → Load → Start 的前置契约及 IPlugin 虚表。
在现有 external-entry 标记旁新增 `IID_PLUGIN_TASK_RUNTIME`；注册值仍为同一个 `IPlugin*`，
由 Scheduler 声明。没有角色标记的插件仍属普通能力插件，不按插件名称硬编码排序。

Start 按普通能力 → task runtime → external entry 三相位执行，同相位保持原注册顺序；
Stop 逆成功启动顺序执行。Scheduler::Stop 返回必须表示其工作线程/批任务已结束、
在途 task 调用及取消回调已退出，不再进入 Baseline 接口。随后普通能力插件方可 Stop。
Start 失败仅逆序回滚本批次已成功启动的插件；失败插件自身收回本次 Start 的半成品资源。
重复 Stop 不重复调用已停止插件；直接 Unload 先执行 StopAll，再进入原 Unload/释放库流程。
任务和插件产物依然遵守已有库租约，不能在持有插件产物时主动 dlclose。

这保证同一 Baseline task 的 Submit/Close 不重叠，保留其单调用线程契约，不往算法热路径加锁。
部署需要同时更新加载器与声明角色的 Scheduler；单独替换 Baseline `.so` 不提供此停机保证。

### 2. 输出会话与取消

Baseliner 内部使用一个共享会话所有者，统一保留数据库通道租约与 atomic session。
Store 和在途 Cancel 持有该所有者；仅最后所有者释放时调用 session::Close，且先于通道租约释放。
Join 交接所有者，不显式关闭仍被使用的旧会话；两个同库输出共用一次事务，任一 Store 释放不提前关库。
取消标志跨会话交接保持：取消发生在发布新会话之前、过程中或之后，都不能使新会话漏收取消，
更不能由后续 Open/Join 清掉已发生的取消并继续发布。重复 Cancel/Close 不造成重复关闭或悬空引用。
除 Cancel 可并发外，Open/Join/Write/Close 仍由任务执行线程串行；不扩展为任意方法并发安全。
该所有者只管理生命周期，不新增通用资源框架，不改数据库驱动 API 或原提交结果语义。

### 3. 插件显式输出本次评估就绪状态

新增独立公共头 `framework/interfaces/ibaseline_evaluation.h`，以下为冻结的 V1 契约。
旧 `ibaseline_service.h`/`ibaseline_types.h` 的虚表与结构布局保持；新 V1 类型发布后保持布局，
后续不兼容演进另建版本。不承诺跨编译器/STL 的二进制兼容。

```cpp
#include <framework/interfaces/ibaseline_service.h>
#include <cstdint>
#include <vector>

namespace flowsql {
enum class BaselineEvaluationReadinessV1 : uint32_t {
    kUnavailable = 0,
    kColdStart = 1,
    kReady = 2,
};
struct BaselineEvaluationResultV1 {
    RollingBaselineResult result;
    BaselineEvaluationReadinessV1 readiness = BaselineEvaluationReadinessV1::kUnavailable;
};
struct BaselineRelationEvaluationResultV1 {
    RelationRollingResult result;
    std::vector<BaselineEvaluationReadinessV1> routed_readiness;
};
interface IBaselineValueEvaluationV1 {
    virtual ~IBaselineValueEvaluationV1() = default;
    virtual BaselineEvaluationResultV1 SubmitObservationWithReadiness(
        const ValueRollingObservation& obs, const RollingSubmitOptions& options) = 0;
};
interface IBaselineRatioEvaluationV1 {
    virtual ~IBaselineRatioEvaluationV1() = default;
    virtual BaselineEvaluationResultV1 SubmitObservationWithReadiness(
        const RatioRollingObservation& obs, const RollingSubmitOptions& options) = 0;
};
interface IBaselineRelationEvaluationV1 {
    virtual ~IBaselineRelationEvaluationV1() = default;
    virtual BaselineRelationEvaluationResultV1 SubmitObservationWithReadiness(
        const RelationRollingObservation& obs, const RelationRollingSubmitOptions& options) = 0;
};
}  // namespace flowsql
```

沿用任务可选能力的 dynamic_cast 发现方式；服务仍通过 IQuerier/IID 取得，算子不依赖插件实现类。
新旧 Submit 复用同一算法执行，单次调用只校验、评估和学习一次；不能先调用旧 Submit 再重新提交。
输出是拥有型单次结果，生命周期受任务/插件库租约约束；线程约束与旧 task 相同。

| 状态 | 精确定义 | 算子当前桶输出 |
| --- | --- | --- |
| kUnavailable | 本次没有有效评估结果，包括拒绝输入、预测失败或未能初始化；错误状态仍由 result.status 表达 | 保留可用 actual 和原 status，预测字段为 NULL |
| kColdStart | 本次从当前观测首次初始化成功，但进入本次观测前没有可用基线 | 保留 actual，当前桶状态 kNotTrained，预测字段为 NULL |
| kReady | 本次以当前观测学习之前的有效基线完成评估，且该 rolling result.status 为 kOk | 输出原基线条带和偏离结果，告警仍由 can_alert 决定 |

状态在 runner 的初始化/预测分支直接赋值，不能解析诊断文本、事后查询模型或缓存“上次 readiness”。
kReady 与 can_score、can_update、can_alert、maturity 不等价；已有基线的低支持或禁止学习观测，
只要本次评估有效，仍可为 kReady。bootstrap/checkpoint 提供有效旧基线时，首次在线提交也可 kReady。
首条观测学习成功可以支持未来 forecast，但不能把当前桶评价标成 ready；forecast 保持原接口语义。

Relation 的 routed_readiness 与 result.routed_results 同次构造、同长度、同索引；每条对应其自身
routed_series_key/basis_version。关闭 routed 输出时两者均为空；basis 切换允许新子序列冷启动、
其他子序列继续 ready，不能用外层 Relation 状态或全局 bool 替代。算子在消费前验证长度一致。

Baseliner 在 Open 时检查对应的 V1 能力；缺失时在消费输入前明确报能力不兼容，不回退字符串推断。
旧调用方仍可调用新插件的旧 Submit；新旧入口在相同初始状态下必须产生等价旧结果和最终模型。
诊断可继续展示 `first_observation`，但它不再承担跨层控制语义；不往持久快照增加瞬时 readiness 字段。

## 两条主链路

1. 执行与结果：Scheduler 创建/绑定 Baseline task → 检查 V1 能力 → 每桶提交一次 → 插件返回旧结果及
   事前评估状态 → 算子按状态填预测/NULL → 按既有协议输出结果、模型及恢复位置。
2. 取消与退出：外部入口停止接纳 → Scheduler 取消任务；Cancel 持有会话所有者，Join 仅交接所有权 →
   任务/取消回调退出，最后所有者关闭会话 → Scheduler 完成 join → Baseline 清理 → Unload/释放库。

## Feature Task 与验收锚点

- [ ] **T1 交付配置顺序无关的任务排空保证，使停机和卸载不再提前清理活动算法状态。**
  用真实 loader fixture 验证三相位、Scheduler/Baseline 顺序排列、失败回滚、重复 Stop 和直接 Unload；
  用活动 Baseline task 的确定性屏障证明提交/取消先结束，插件清理后发生，真实 Scheduler `.so` 接线路径通过。
- [ ] **T2 交付取消与会话交接的安全释放保证，使同库双出口在并发取消时保持句柄有效和事务一致。**
  屏障停住旧 session::Cancel 后执行 Join/Close，断言 Close 尚未发生；释放屏障后恰好关闭一次，
  通道后于会话释放。覆盖 Join 前/中/后取消、重复取消、两个 Store 先后释放、无重复提交及同库别名双表回滚。
  实际 SQLite/MySQL/PostgreSQL 输出回归保留；不能只用 shared_ptr 引用计数断言代替内部 Close 时序。
- [ ] **T3 交付插件拥有的类型化评估就绪契约，使三类算法的有效预测与冷启动不再受诊断文案影响。**
  Value/Ratio 首条与后续、bootstrap 首条、恢复后首条、有效评估但禁止学习、无效/重复输入均有断言；
  Relation 验证 mixed ready/cold、basis 切换和关闭 routed 输出；诊断开关/文案变化不改变算子字段有效性。
  独立旧调用方与新插件兼容，新算子缺能力明确失败；双任务分别走新旧入口，核对旧结果、快照和学习次数。
- [ ] **T4 交付完整回归与可复核证据，使退出安全、结果语义及兼容范围在生产加载链路中同时成立。**
  当前源码全量构建/CTest、定向 ASan/UBSan/LSan、格式/严格编译和实际 `.so` 加载回归通过；
  并发保证以可控屏障和确定性断言验收，TSan 可运行时作为补充，环境阻碍须记录而非伪称通过。
  复核源数据、结果 Schema、checkpoint 和旧数值保持；报告固定负载下提交/会话交接的成本及限制。
  完成文档、增量 diff/hash 和任务状态审查后才归档 Feature，不以已有构建产物或历史日志代替验收。

顺序：T1 → T2 → T3 → T4，每次只执行工作台冻结的切片。具体文件/命令随切片进入工作台。
本 Feature 统一验收 Scheduler → Baseliner → Baseline 的调用和资源退出链路，不扩展为框架重构；
三个缺陷分别有结果保证，整体验收覆盖同一生产执行链路，保持四个一级任务、无第三层编号。

## 完成证据

2026-10-10：仅补充规格和验收锚点；运行实现、公共头与回归测试尚未修改，T1～T4 未完成。
文档链接/层级/空白及本轮增量范围检查通过；上方接口片段通过当前公共头下的独立 C++17 语法检查。
本轮未执行运行时构建/CTest，不将历史测试视为修复证据；详见工作台及 `/tmp/baseliner-contract-safety-spec/verification.json`。
