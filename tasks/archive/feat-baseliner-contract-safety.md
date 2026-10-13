# Baseline/Baseliner 生命周期与评估契约修复

状态：T1 / T2 / T3 / T4 已完成，Feature 已归档（2026-10-11）。网络新布局由 [npi-wire-layout](feat-npi-wire-layout.md) 独立交付。优先级：P1。
前置能力：[Baseline 算子](feat-baseline-operator.md)、[单表与 DataFrame 分析及输出](feat-baseliner-single-source.md)。

## 业务意图与 Non-Goals

为通过 Scheduler 加载 `.so` 并调度 Baseline 分析的用户，修复停机时算法状态被提前清理、
取消时输出会话被提前关闭，以及算子通过诊断文本猜测冷启动的问题；交付可验证的任务排空、
会话安全释放和类型化评估有效性保证，保持已有分析结果、源数据与持久化行为。

本 Feature 不调整算法公式、训练策略、阈值、结果表 Schema、配置或 checkpoint 格式；
不处理结果发布阶段的取消空窗、通用 DataFrame 查询/CSV 展示、一般去重或文件重构；
不引入通用插件依赖图、算法全局锁、专属工作线程或新的数据库公共接口。

网络协议头/分析地址布局及 NPI/NPM Basic 适配归独立的 [npi-wire-layout](feat-npi-wire-layout.md) Feature。
本 Feature 不承接其字段覆盖、网络成本或公共布局迁移；T4 验收当前一致源码的生产链路，未来新布局的同版部署回归由该独立 Feature 负责。

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

- [x] **T1 交付配置顺序无关的任务排空保证，使停机和卸载不再提前清理活动算法状态。**
  用真实 loader fixture 验证三相位、Scheduler/Baseline 顺序排列、失败回滚、重复 Stop 和直接 Unload；
  用活动 Baseline task 的确定性屏障证明提交/取消先结束，插件清理后发生，真实 Scheduler `.so` 接线路径通过。
- [x] **T2 交付取消与会话交接的安全释放保证，使同库双出口在并发取消时保持句柄有效和事务一致。**
  屏障停住旧 session::Cancel 后执行 Join/Close，断言 Close 尚未发生；释放屏障后恰好关闭一次，
  通道后于会话释放。覆盖 Join 前/中/后取消、重复取消、两个 Store 先后释放、无重复提交及同库别名双表回滚。
  实际 SQLite/MySQL/PostgreSQL 输出回归保留；不能只用 shared_ptr 引用计数断言代替内部 Close 时序。
- [x] **T3 交付插件拥有的类型化评估就绪契约，使三类算法的有效预测与冷启动不再受诊断文案影响。**
  Value/Ratio 首条与后续、bootstrap 首条、恢复后首条、有效评估但禁止学习、无效/重复输入均有断言；
  Relation 验证 mixed ready/cold、basis 切换和关闭 routed 输出；诊断开关/文案变化不改变算子字段有效性。
  独立旧调用方与新插件兼容，新算子缺能力明确失败；双任务分别走新旧入口，核对旧结果、快照和学习次数。
- [x] **T4 交付完整回归与可复核证据，使退出安全、结果语义及兼容范围在生产加载链路中同时成立。**
  当前源码全量构建/CTest、定向 ASan/UBSan/LSan、格式/严格编译和实际 `.so` 加载回归通过；
  并发保证以可控屏障和确定性断言验收，TSan 可运行时作为补充，环境阻碍须记录而非伪称通过。
  复核源数据、结果 Schema、checkpoint 和旧数值保持；报告固定负载下提交/会话交接的成本及限制。
  完成文档、增量 diff/hash 和任务状态审查后才归档 Feature，不以已有构建产物或历史日志代替验收。
  网络头新布局和 NPI/NPM 适配由独立 Feature 验收；本任务保留自身完整回归及当前版本 PCAP → NPM → Baseliner 集成验收，不等待未来布局实施。

顺序：T1 → T2 → T3 → T4，每次只执行工作台冻结的切片。具体文件/命令随切片进入工作台。
本 Feature 统一验收 Scheduler → Baseliner → Baseline 的调用和资源退出链路，不扩展为框架重构；
三个缺陷分别有结果保证，整体验收覆盖同一生产执行链路，保持四个一级任务、无第三层编号。

## 完成证据

2026-10-10：仅补充规格和验收锚点；运行实现、公共头与回归测试尚未修改，T1～T4 未完成。
文档链接/层级/空白及本轮增量范围检查通过；上方接口片段通过当前公共头下的独立 C++17 语法检查。
本轮未执行运行时构建/CTest，不将历史测试视为修复证据；详见工作台及 `/tmp/baseliner-contract-safety-spec/verification.json`。


2026-10-11：T1 / P1-1、T2 / P1-2 完成。加载器采用角色 IID 的三相位启动与逆成功顺序停止，
Scheduler 声明运行时角色，直接 Unload 先排空。输出 Store/在途 Cancel 共持会话与通道所有者，
Join 只交接所有权并保留/补发取消，最后所有者先关闭会话再释放通道。
真实 Scheduler/Baseline `.so` 的两种排列 × Stop/直接 Unload 屏障回归、三相位/同相位顺序、
各相位 Start 失败/增量回滚/重复 Stop、会话交接前中后取消/两个 Store 释放顺序/取消期间 Close 均通过。
修改前加载器使屏障后的 Baseline 提交失败，修改前 Store 在 Cancel 在途时调用 Close；回归旧实现
在 /tmp 覆盖编译，未撤销工作区修复，不把断言中止描述成实际 UAF 崩溃。
当前标准配置和五个目标构建通过，定向五项全部通过；来源为基础四项与生产复跑一项，
`combined-tests.xml` / `test-provenance.json` 明确来源，原配置错误与普通 MySQL 账号缺少建库权限的日志保留。
生产使用测试容器已有管理员凭据创建隔离数据库，未修改账号授权；43 组分析与 43 次重载恢复、
三后端同库别名双表回滚及源内容/Schema 保持通过，安全回归另连续五轮通过。
定向 ASan/UBSan/LSan（本次会话实现与安全测试/加载器头，依赖插件为普通构建）在沙箱外通过；
沙箱 ptrace 导致的 LSan 阻碍原日志保留，不据此宣称 T4 完整仪器化/全量 CTest 或成本验收。
修改区间/文件格式和严格编译通过；公共头既有 warning 例外单列，无越界修复。
证据 `/tmp/baseliner-contract-safety-p1/verification.json`、`scope-final.json`、构建/原始测试/回归旧实现/Sanitizer 日志。
本轮完成即停，工作台 WIP=0，T3 / T4 未开始，Feature 尚未归档，未提交/推送。


2026-10-11：T3 / C1 完成。新增独立 `ibaseline_evaluation.h` 三类 V1 task 能力，旧 service/types/checkpoint
公共头逐字节保持。runner 在每次调用开始置 unavailable，在成功从当前观测初始化/成功使用旧基线
评估的分支返回 cold/ready；Value/Ratio/Relation 新旧入口共享单次执行，不持久化瞬时 readiness。
Relation 同时构建 routed 结果及 readiness，Baseliner Open 检查 V1，Submit 消费前校验向量长度，
当前桶预测有效性不再依赖诊断；forecast 仍沿用原接口。
新增回归覆盖 Value/Ratio 首条/后续、bootstrap 首条、checkpoint 恢复首条、低支持与异常禁止学习、
无效/重复输入及无状态突变；Relation mixed ready/cold、basis 切换、关闭 routed 输出及诊断开关。
双任务分别走新旧入口，完整旧结果、序列快照（含学习次数）和 checkpoint 保持一致；三类诊断清空/
任意文案（包含误导性的 first_observation）均保持字段有效性，三类缺能力在 Open 失败且提交/绑定计数为零；
向量长度不一致明确失败。仅包含旧任务接口的独立 dlopen 调用方通过，实施前保存的旧 Value 客户端
二进制也成功调用新插件。
标准配置和 12 个定向目标构建通过；一次终端 CTest 13/13（12 个测试 + 1 个工作目录 fixture），4.51s，
包含既有 Baseline/checkpoint/Relation/B3、Baseliner evaluation/operator 与 T1/T2 safety 回归。
修改 C++ 区间/新文件格式检查、五个实现源和公共头 C++17 严格编译、版权/diff/hash 范围审查通过。
原始夹具配置错误与缺 V1 的测试锚点日志保留，最终结果只引用 acceptance.xml。
证据 `/tmp/baseliner-contract-safety-c1/verification.json`、`acceptance.xml`、`scope-final.json`。
本轮 WIP=0 并停止；T4 全量构建/CTest、完整 Sanitizer、生产/成本验收未执行，Feature 未归档，未提交/推送。

2026-10-11：T4 本轮检查点。标准全量构建完成；完整普通 CTest 已启动，最终结果待收取。
新增 Submit/Join 固定负载成本及五轮重复、既有九场景成本五轮、格式/严格编译/版权/兼容检查通过。
独立完整定向 Sanitizer 构建核对 208 个翻译单元和 15 个 FlowSQL 动态库已仪器化；沙箱外最终 9/10 通过，
生产 PCAP → NPM 路径在既有 NPI `layer.cpp:91` 触发 IPv4 头未对齐成员访问。
该依赖源码与 HEAD/切片起点相同且不在本轮允许列表，未越界修复，未关闭检查器。
T4 保留未勾选、Feature 未归档；后续须先冻结 NPI 对齐安全读取及回归切片，再完成生产 Sanitizer 验收。
实际数值、限制和原始证据见 [本地验收记录，未入库](../../evidence/baseliner-acceptance.md) 与 `/tmp/baseliner-contract-safety-t4/`。

2026-10-11 续执行 T4：当前 NPI 对齐修复草稿全量构建、普通 packet/alignment 两项通过；
定向 Sanitizer 在 alignment 测试退出时报告 16,752 bytes / 23 allocations，源为 NPI parser
与 ObjectsPool 当前块未释放。通用池不在允许清单，未越界处理、未关闭泄漏检查。
完整普通 CTest 本次未通过且已停止；DPDK 退出崩溃由 GDB 定位，按已有 WSL 能力限制复跑 NPM 通过。
T4 保留未勾选；当前成本、兼容、格式和严格编译例外及后续边界详见验收记录的续执行检查点。
生产定向 Sanitizer 配置修正后，43 组与 43 次恢复功能断言通过，但 LSan 最终报告
1,239,101 bytes / 70 allocations；同时确认 BufferPool 当前块泄漏，后续修复需另行冻结边界。

2026-10-11：获准的 ObjectsPool/BufferPool 当前块、NPI parser 析构释放已实施；旧对象析构断言失败、
旧 BufferPool LSan 泄漏与修复后通过均有对照。普通两项回归通过，真实 NPI 重载覆盖空配置/已有协议规则、
HTTP 实际命中和 0～15 偏移。定向 Sanitizer 11/12、147.39s，生产 43 组与 43 次恢复退出无泄漏；
新增同库规则重载发现 RegexRecognizer::Ready 覆盖旧 Hyperscan 数据库指针，343,200 bytes / 4 allocations。
该文件尚未获准纳入允许范围，T4 保留未勾选，工作台记录最小后续方案与原始证据。

2026-10-11 14:32：用户确认扩大到 regexmatch.cpp 后，Ready 新库局部编译、成功释放旧库并交接、失败保留旧库已实施。
普通 packet/alignment 2/2；当前完整定向 ASan/UBSan/LSan 12/12、144.14s（含目录 fixture），
真实 NPI 规则重载三次及 HTTP 0～15 偏移命中通过，前轮 343,200 bytes / 4 allocations 泄漏消除；
生产 43 组与 43 次恢复通过，源 Schema/内容保持，退出无 Sanitizer 报告。
临时旧/新实现探针验证重复 Ready、双 scratch 匹配及编译失败保留旧库，旧实现失败而修复后通过。
修改区间格式、版权、带既有告警例外的严格编译、公共头兼容与切片范围检查通过；证据 `/tmp/npi-regex-t4/`。
当前切片 WIP=0、完成即停；T4 仍未勾选，完整普通 CTest、额外字段/规则成本等最终验收未执行，未归档/提交/推送。

2026-10-11 任务归属调整：用户要求协议报文结构体统一 1 字节对齐、分析地址按适宜对齐，并拒绝当前大量字节读取写法。
新增独立 npi-wire-layout Feature，承接全部网络布局、地址分离、NPI 与 NPM Basic 消费者和网络成本验收。
T1/T2/T3 保持完成，T4 保持未勾选；NPI 新方案实施后再验收最终生产链路，不因任务分流提前归档。
源码和前轮原始证据全部保留；本轮只调整方案/任务文档，没有执行新布局或最终 CTest。

2026-10-11：按用户授权聚焦本 Feature，T4 最终验收完成。当前源码标准配置/全量构建通过；
一次完整 CTest 83/83、1050.02s，failures/errors/skipped 均为 0；定向 ASan/UBSan/LSan 11/11、144.50s，
保持 detect_leaks=1，无抑制，真实生产 43 组与 43 次恢复、源 Schema/内容保持通过。
核对 213 个仪器化翻译单元、202 个不同源文件及 15 个 FlowSQL .so；第三方仍为普通缓存库。
格式/版权 20 文件、严格编译 12 单元通过，既有告警例外单列；旧 service/types/checkpoint、codec/配置保持 HEAD。
两项固定 CPU 成本各五轮通过；旧/V1 Submit 墙钟中位数 20.964/20.327 μs 每观测，Join 中位数 8.117 μs，
为当前入口/固定夹具测量，不声明修复前后 overhead 或生产规模性能。TSan 探针受 unexpected memory mapping 阻碍，未声明通过。
WSL IPv4 路由问题经端口与路由核查；仅在完整 CTest 期间临时改 lo，结束后恢复并核对原路由。
本轮所有生产/CMake/测试文件与起点 SHA256 一致；新增网络布局及未来同版部署回归归独立 npi-wire-layout。
完成文档、增量 diff/hash 与范围审查后勾选 T4，规格归档、Backlog 完成、工作台 WIP=0；未提交/推送。
证据 [本地验收记录，未入库](../../evidence/baseliner-acceptance.md) 与 /tmp/baseliner-contract-safety-closeout/verification.json。
