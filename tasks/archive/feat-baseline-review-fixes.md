# baseline 检视修复

状态：已完成并归档（2026-10-08），T1～T5 全部验收；B06 的封装算子部分仍由独立后续 Feature 交付。

## 业务意图

让使用 Value、Ratio、Relation 做历史预热和在线检测的调用方，消除检视中确认的 P1/P2 状态、训练及检测口径问题；保持既有公开接口，交付可复现、可回归的正确行为及经确认后实施的可选状态管理能力。

## Non-Goals

不新增检测能力、配置、JSON 字段或 artifact 版本，不修改既有公共接口；新独立公共能力仅限下述已确认的 B06 方案，实施前冻结其新头文件契约。不做大文件拆分和泛化性能优化，不提交/推送。后续按 T1→T2→T3→T4→T5 顺序完成任务，B01～B11 为问题标识；用户已同意 B01 的内部游标及有限查找/存储成本、B04 合并投影校验和必要的前置扫描、B08 方案 A 的 band 语义变化及固定标量计算、B09 的日历预编译，以及 B11 方案 A 的最近提交快照语义和每 rolling 状态 +40 字节缓存/固定写入成本；其他接口或性能影响仍需先讨论。

B06 独立接口、释放/版本保留行为及索引成本已获用户“OK，实施吧”确认，按工作台逐切片实施。插件不接入具体产品、不判断业务不活跃、不创建定时线程；封装算子作为独立后续 Feature 登记，不纳入本 Feature 实施。

## 核心契约

- ibaseline_service.h、ibaseline_types.h 及既有序列化格式冻结。
- RollingState 的 last_seen_bucket 保持模型学习锚，内部 last_processed_bucket 控制合法观测消费顺序；跳过学习仍推进后者。
- Relation source 在 routed/basis/fusion 变更前做时间检查；拒绝重复/过期输入不改变运行状态。
- 历史和在线使用同一输入域、metric 身份和变换口径；单 series 训练只替换其运行状态。
- 不兼容 artifact 导入失败且原状态不变；导入成功时 runtime 与 seed 对应。
- Ratio Bootstrap band 保留先验/覆盖度概率不确定性带，与已有 logit 残差 sigma_ref 经 sigmoid 还原的区间取包络；mu/confidence 保持，单点/序列同口径，不承诺已校准覆盖率。
- 容量、event 消费、band 和 snapshot 口径逐项评估，未讨论的接口/性能影响不擅自实施。
- 事件日历在任务创建时按固定时区与 bucket 长度绑定；初始化处理夏令时转换点，运行查询只访问整数 bucket 区间索引，保持原本地墙钟端点匹配、事件去重和代码顺序，不逐 bucket 展开。
- snapshot 的 band、can_score/can_update/update_weight、can_alert 对应最近 kOk Submit；其他证据为当前学习状态，diagnostics 标记视图及消费 bucket。暂无在线提交时保留参数 band，控制和告警=false、weight=0；重建清除缓存，预测/拒绝/失败不覆盖。
- 性能影响重点评估算法复杂度、扫描范围、状态/数据结构及热路径结构变化，不以新增代码行数或测量噪声判定退化；发现算法或结构造成的额外成本时先讨论。

## 主链路

1. Submit：输入适配 → 时间闸门 → 检测与证据更新 → 条件学习 → 推进观测游标 → 返回既有类型结果。
2. Bootstrap / Load：验证输入或兼容性 → 构建 artifact/seed → 提交存储 → 重建目标运行状态。

## Feature Task

- [x] T1：交付一致的历史/在线准入与严格观测顺序，使重复、过期和域外输入不污染检测状态（B01、B04）。
- [x] T2：交付隔离且兼容性完整的预热状态替换，使单 series 训练不清除其他 series、导入按整份文档替换且不混用新 seed 与旧 runtime（B02、B03、B10）。
- [x] T3：交付一致的变换、band、event 与 snapshot 行为，使已支持配置的训练和在线结果使用相同口径（B05、B08、B09、B11）。
- [x] T4：交付经确认的显式状态管理、关联状态释放与实际容量约束，使调用方能完整回收目标身份并限制身份/历史版本累积（B06 插件部分；插件已验收，端到端超时由后续封装算子验收）。
- [x] T5：交付进入常规 CTest 的回归与修复证据，使 CI 能执行既有 baseline 测试并验证完整修复（B07 与 Feature 整体验收）。

## 问题清单

下表的“已修复”仅表示对应问题已达到其切片验收，不代表所属 Feature Task 或整个 Feature 已完成；具体证据及性能限制见“完成证据”。

| 编号 | 问题与预期修复结果 | 所属任务 | 当前状态 |
|---|---|---|---|
| B01 | 重复/过期观测及跳过学习后的重复提交可能修改状态；在状态变更前统一时间准入，合法消费仍推进游标。 | T1 | 已修复 |
| B02 | Relation 单 source Bootstrap 重建时清除其他 source 的在线状态；只替换目标 source，保留其他 source。 | T2 | 已修复 |
| B03 | Relation artifact 未完整校验 basis/fusion/routed 的身份与指标口径；拒绝不兼容导入且保持原状态。 | T2 | 已修复 |
| B04 | 历史与在线输入域不一致，Relation 非法输入可能产生部分状态更新；统一校验，非法提交不改变运行状态。 | T1 | 已修复 |
| B05 | Sampled Value 的 identity 与 Ratio 的 eps_logit 在训练、预测、在线检测中口径错位；贯通既有配置并校验变换兼容性。 | T3 | 已修复，性能结论限于已测条件 |
| B06 | 插件缺少按身份完整释放、实际容量约束与旧 basis 子状态退休；封装算子另负责业务活动、超时判断和时间触发。两部分分别验收，见下述方案和后续规格。 | T4（插件）及后续 baseline-operator | 插件已修复并验收；算子待办 |
| B07 | baseline 测试可执行程序未注册到 CTest，常规 CI 无法自动覆盖；注册 12 个常规回归目标，3 个评估/性能目标保持独立运行，并完成整体验收。 | T5 | 已修复，Feature 整体验收通过 |
| B08 | Ratio Bootstrap band 原来只用样本数近似概率方差，未消费历史残差尺度；按已授权方案 A 与 logit 残差带取包络，保留原带作为最低范围。 | T3 | 已修复，band 语义变化已获授权 |
| B09 | Bootstrap seed 导出 event_hint，但 Rolling 未消费，事件效果未贯通；日历区间预编译及 Value/Ratio/Relation routed 的检测、预测和去事件学习已接入并验收。 | T3 | 已修复，事件匹配零时区转换 |
| B10 | Value/Ratio 重训或导入新 artifact 后，warmup 跳过已有运行状态，形成新 seed 与旧 runtime 混用；按替换范围同步运行状态。 | T2 | 已修复 |
| B11 | snapshot 原为学习后 level±3sigma、has_seen 控制及恒零权重；按授权方案 A 缓存最近成功 Submit 的 band/控制/告警，标明时间口径并保持重建与拒绝语义。 | T3 | 已修复，方案 A 的语义和结构成本已获授权 |

## B06 责任拆分与插件方案

本节取代 /tmp/baseline-b06-evaluation/B06-proposal.md 中的统一 TTL/LRU A/B 提案；该目录的复现日志仍是事实依据，旧默认值、候选尺寸和活动维护成本不作为本方案依据。责任拆分文档完成后，用户已确认插件方案；实现与验收进度以工作台及完成证据为准，不将方案批准等同于修复完成。

### 责任与验收边界

| 部分 | 负责内容 | 交付与验收 |
|---|---|---|
| 当前算法插件，T4 | 提供显式按身份释放；统一清理 basis/routed/fusion/游标和派生缓存；区分模型资产与 runtime；管理历史版本；执行真实容量准入及 Close 释放。 | 经独立公开能力直接验证完整释放、其他身份不变、对象数量上限及失败原子性，不依赖 Scheduler 或时间触发。 |
| 后续封装算子 | 接入上游 Arrow 数据；定义成功观测活动、在线不活跃或事件时间过期；选择容量与淘汰策略；通过框架时间通知发起释放；管理重入水位和模型恢复。 | 独立 [baseline-operator 规格](../specs/feat-baseline-operator.md)，验证无数据/繁忙/背压时维护、策略与输出以及调用串行。 |

不活跃过期不等于观测 bucket 过期，也不等于旧 basis 子模型退休。插件不读取 wall clock、不保存业务活动 LRU；已有 fusion 局部清理仍限于其原契约，不可代替 source 整体回收。

### 可选能力与数据契约（已冻结并实现独立新头）

通过 IQuerier 的新独立 IID 暴露 IBaselineStateControlServiceV1，不向旧 IBaselineTask/IBaselineService 虚表加方法，不改既有配置、JSON 或 artifact 格式。

| 数据/能力 | 明确语义 |
|---|---|
| BaselineStateLimitsV1 | uint64_t 的 max_runtime_identities、max_model_identities，uint32_t 的 max_basis_versions_per_metric；受管任务显式提供有限正值，Relation 的版本上限至少 2，不设置通用 TTL 或产品默认值。 |
| Bind(task, limits) | 接受 shared_ptr<IBaselineTask> 与 limits，只绑定本服务创建、尚未 Submit/Bootstrap/Load 的空任务；返回 status 与 shared_ptr<IBaselineTaskStateControlV1>，控制句柄持有同一个任务；限额冻结在任务内部，原任务句柄的调用也不能绕过。未绑定的既有调用方保留原准入/历史行为，不宣称其自动有界。 |
| ReleaseIdentity(key, scope) | Value/Ratio 的 key 为 series，Relation 为 source；scope 为 RuntimeOnly 或 AllState。释放不存在身份幂等成功，拒绝将 routed 子 key 当 source 释放。 |
| QueryUsage() | 与 ReleaseIdentity 同属 IBaselineTaskStateControlV1；返回 status 与独立 usage 结构，四类 uint64_t 数量为 runtime 身份、模型身份、routed 子状态及保留版本；计数覆盖实际所有者，模型预热生成的 runtime 也计入限额，不扩写原 snapshot JSON。 |

控制句柄与任务共享生命周期，调用方仍须让插件库存续到句柄及所有插件产物销毁；所有管理调用沿用同任务串行契约。旧接口的 ABI 不变；新增 IID/头文件属于已确认的公共能力扩展，对应切片须先冻结接口再实施。

### 插件实现与行为边界

1. **显式释放**：RuntimeOnly 清除在线 rolling、消费游标与最近 Submit 视图；Relation 还按 source 清除 basis accumulator、fusion/persistence、routed rolling/spec/派生 seed 及内部索引，保留父级 artifact/seed 作为恢复材料。AllState 再清除该身份的父模型资产。其他身份不变；不能调用会立即重新预热的 RebuildRuntimeForSource 代替释放。
2. **重入语义**：RuntimeOnly 结束当前在线生命周期，查询不存在的 rolling 状态返回 kNotTrained；下一次合法 Submit 可从保留 seed 恢复，Value/Ratio 的 Bootstrap 预测仍有效，保留模型身份仍占模型额度。AllState 后按无模型冷启动。两种释放都会遗忘在线消费游标；插件不保留无限 tombstone，跨生命周期的旧数据拒绝由算子上游水位/恢复策略保证，不承诺永久重复桶拒绝。
3. **basis 版本退休**：仅受管 Relation 按 source×metric 的显式有限上限，在成功提交/refresh 的提交点退休最旧且不再被当前处理或 handover 使用的版本；active 版本保留，受保护版本占用额度，不能满足上限则在提交前拒绝、不进行部分更新。同时清理旧版本 routed rolling/spec/派生 seed/索引，不删除父 artifact；父 artifact 的训练 basis 不作为在线历史状态常驻理由，也不隐式复建已退休版本。退休版本的精确在线查询/预测返回 kNotTrained；未绑定任务保留原历史查询行为。
4. **容量与原子性**：runtime 身份与模型身份分别限量；runtime 身份包括任何仍持有在线子状态或消费游标的 series/source，不只是 rolling 表已有 key。满额不在插件内挑选其他身份淘汰，新增身份返回既有 kInvalidArgument，启用原 diagnostics 时说明容量原因；已有身份按限额内规则继续处理。Bootstrap/Load 在提交资产前校验身份数量及预热扇出，受管 Relation 导入同时核对 support/stable 大小及每 metric 的 universal≤4、每 basis 版本 scoped≤3+k_stable 子模型数。单目标重训只重建目标，不能复活其他已释放 runtime；不兼容/超限导入保持原状态。已有 fusion/persistence 配置声明的局部 cap 必须实际约束容器，不止输出诊断。
5. **Close**：三类任务实现 OnClosing，释放容器元素与已分配容量、runtime 索引及任务持有的编译日历引用，即使外部仍持有任务句柄也不保留模型/在线状态；Id/Name/Kind、ExportConfig 和重复 Close 语义保持。不承诺 allocator 立即将 RSS 归还操作系统。

数量上限按 runtime 身份×固定配置的 metric/summary 扇出×保留版本，加模型身份与已有单源 group/persistence 上限核对；对固定配置保证身份/版本不无限累积，不将对象数量限额宣称为精确字节预算。Close 的释放能力适用于全部任务；准入和有限历史模式仅显式绑定时启用。

### 线程、锁及算法/结构影响

| 位置 | 线程与同步标识 | 结构与调用成本 |
|---|---|---|
| 插件 Submit/Bootstrap/Load/Release/QueryUsage | **新增线程 0；新增 runtime 锁 0**。同任务非重叠、跨线程有 happens-before；不同任务仍可并行。 | 新身份数量校验及计数固定成本；Relation 增加 source→子状态/版本索引，只在创建/退休时维护，不增加每条活动 LRU 操作。 |
| 插件释放与版本退休 | **不创建后台线程，不与 Submit 并发**；在当前串行调用内完成。 | 按目标 source 的子状态数量删除，避免扫描其他 source 的全部 shards；同源版本检查随有限保留数变化。实际索引空间、释放延迟及完整 Submit 吞吐需实施时测量。 |
| 插件注册、配置和关闭 | **保留已有 TaskRegistry mutex 与配置 snapshot 原子发布，不新增锁**；registry 锁只保护任务表，不能保护 runtime。 | Bind 验证和 Close 注销是管理路径；调用 Close 前调用方必须停止同任务其他调用，Stop 不能绕过此条件。 |
| 后续封装算子 | **新增维护线程 0，算法状态 mutex 0**；复用 IBlockTransformTaskV2 的 ProcessBlock/OnTime/Flush 串行通知。 | 算子单独承担活动索引和有预算的过期遍历；一次释放的本源扇出应计入预算，不能仅限制调用次数就宣称固定延迟。 |
| 算子取消、查询和销毁 | **Cancel 可来自其他线程：仅已有取消机制/原子停止信号，不访问算法容器**；ReleaseTask 等调用结束再关闭。 | 外部查询消费已发布结果，不由 HTTP 线程直接调用 task；本方案不增加共享在线查询状态或后台并发维护。 |

因此本方案没有新增线程/锁的实现需求，但有已向用户说明并获确认的新增公开能力、限额拒绝/重入/旧版本不可查询行为及索引成本；实施时验证实际成本，不声称零性能影响，也不沿用旧 LRU 候选的 +16/+48 字节估算。超出本方案的影响仍须讨论。

### T4 验收锚点

- 公开可选能力验证三类任务 RuntimeOnly/AllState 完整释放、幂等和其他身份保持；持有关闭句柄仍无模型/runtime 容器容量。
- 小限额下 Submit/Bootstrap/Load 不超量且失败前后完整状态不变；单目标预热不复活其他已释放身份；fusion/persistence 实存不超过配置 cap。
- 单源多次真实 basis refresh 后子状态数量受配置版本数约束，active/handover 保持，已退休版本返回 kNotTrained，未绑定任务原历史查询保持。
- 重入明确验证消费游标重置和 seed 训练末闸门；无新增线程/runtime 锁；按相同身份数、扇出和输入序列比较完整 Submit 吞吐、索引内存和最坏单源释放延迟。

## 执行顺序与当前进度

此前实际按 B01→B02→B03→B04→B05 执行，对应 T1→T2→T1→T3；T2 未完整验收就进入 T3，未按任务顺序收口。保留实际完成记录，不重编号或将已完成的问题重复实施。

后续从最早未完成的任务继续，执行顺序固定为：

1. T1、T2 已完成；B02/B03/B10 的隔离与兼容状态替换保证已通过整体验收。
2. T3 已完成：B05/B08/B09/B11 已修复，12 个常规 baseline 目标构建和回归通过，快照与实际检测结果及当前状态的时点已明确。
3. T4/B06 插件部分已完成：可选 Bind/Release/Usage、关联状态和 Close 释放、容量/版本与导入扇出控制均通过回归和成本验收；业务超时仍由后续封装算子承担。
4. T5/B07 已完成：12 个常规目标及 Close 资源回归进入 CTest，全量构建和完整 61 项 CTest 通过，Feature 已收口归档。

当前 T1～T5 已完成。B06/T4 的完整验收记录在 /tmp/baseline-b06-t4/B06-T4-result.md，T5/B07 的注册、构建和 CTest 原始日志在 /tmp/baseline-b07-t5/，原复现证据保留于 /tmp/baseline-b06-evaluation/；旧 A/B 提案已取代，既有接口/配置/格式保持。后续 [baseline-operator](../specs/feat-baseline-operator.md) 全部待办，独立验收端到端不活跃处理；本 Feature 已完成全量验收和归档，未提交/推送。

## 验收

每个切片先补关键回归并观察失败，再最小修复、构建和测试；拒绝路径比较完整快照，训练/导入比较其他 series 保持与失败原子性。B01 使用当前插件副本对照完整提交路径的吞吐和内部状态存储；Feature 完成再全量构建和 CTest。

## 完成证据

- T5/B07 与 Feature 整体验收完成（2026-10-08）：原 CTest 49 项、baseline 注册 0；现有 12 个常规 baseline target 已按 target 名注册，统一 baseline 标签和 build/output 工作目录，CTest JSON 断言核对准确集合、命令与属性，三个评估/性能程序继续独立。test_baseline_task_headers 直接复用 flowsql_baseline 精确编译对象，启用已有 Close 资源回归宏，自动运行资源释放断言（8 PASS），保持 -UNDEBUG；未修改生产 target、算法、旧公共接口或 C++ 源码，没有新增运行时线程/锁或热路径成本。标准入口 cmake -B build src、12 个回归目标构建、cmake --build build -j4 均 exit 0；baseline CTest 12/12 通过（1.43 秒），完整 ctest --test-dir build --output-on-failure 61/61 通过（73.28 秒，JUnit failure/skipped 均 0）。首次全量构建有 make 毫秒时间戳、既有 Arrow nodiscard 和第三方头文件警告；一次增量构建 exit 0，无重新编译/链接或警告，未混入范围外修复。注册 red/green、详细输出、JUnit、构建与范围审计保留在 /tmp/baseline-b07-t5/；本轮只修改测试 CMake、README 和任务收口文件，既有生产修复 hashes 保持，git diff --check 通过，沿用 T1～T4 格式验收证据。T1～T5 已勾选，Backlog 完成并归档；封装算子仍待办，未提交/推送。

- T4/B06 插件部分完成：独立状态控制 IID/头提供 Bind、RuntimeOnly/AllState 和四类 usage；三类身份/model 准入、目标预热、原子导入、Relation source 定点索引/版本退休及 fusion 实存 cap 已实现。公开回归覆盖模型保留/清除、重入游标、训练末闸门、幂等/其他身份保持、独立容量与失败快照保持、受管超限版本/扇出拒绝及旧模式兼容；真实多次 refresh 验证 active/handover 保留、退休 kNotTrained 和未绑定历史查询保持。12 个常规 target 构建/测试全部通过，test_baseline 45 PASS、task_headers 7 PASS、fusion 8 PASS，精确当前对象资源回归 8 PASS；四种禁用模式、索引/所有者、foreign Bind 和 control/Stop 生命周期通过。Base/Value/Ratio task 各 +32 字节，Relation +88，RollingState 1024 不变；source index POD=48、ref=32 字节，另有节点/桶和 vector 开销。CPU 0、128 identities、最终同位置插件、六轮独立完整 Submit：旧→受管稳态中位 us/次为 Value 17.942→18.254、Ratio 18.611→18.428、Relation 120.853→121.471、256groups Relation 161.627→161.105；新身份中位额外约 0.677/0.678/6.371/5.779 us，属于已批准固定检查/索引成本，未优化构建和全部波动样本限制结论，不声称零开销/性能等价。8192 sources 的 32 次单源释放中位 8.063 us、观察最大 30.534 us；16 metrics/32 groups/32 versions/672 children 单源释放 980.875 us，无跨源全表扫描。新增线程/runtime 锁均为 0；18 份修改源码的 clang-format 行检查、diff 和允许范围 hash 审计通过，旧公共头/用户 Backlog/其他既有修改保持。全部方法、red/green、原始成本数据和最终库指纹见 /tmp/baseline-b06-t4/B06-T4-result.md；T5/封装算子未实施，未提交/推送。

- B06 Close 切片完成（2026-10-08，T4 未完成）：三类任务覆盖既有 OnClosing 虚 hook，Value/Ratio 交换释放 rolling、seed、artifact 容器容量并 reset 日历；Relation 同时释放全部 routed shards、basis、消费游标及 fusion/persistence。Stop 经 registry 的基类 Close 也触发清理，既有 Close/身份/ExportConfig/关闭后业务拒绝保持。新增同源资源回归在旧精确对象上因关闭后日历弱引用未失效而失败；修复后启用 FLOWSQL_BASELINE_CLOSE_RESOURCE_TEST 的精确对象构建 8 个 PASS，通过三类任务训练与多身份在线后直接/基类关闭、其他任务保持、重复关闭及保留句柄。内部探针另核对真实插件 Close 与 Stop：Value/Ratio 各 65 个 rolling、Relation 256 个 routed 及模型/seed/basis/cursor/fusion 实际释放，map bucket_count 回到默认空表、shard vector capacity=0、弱模型失效；Unload 后仍持有句柄重复关闭安全。最终两个 CMake target 构建及常规回归通过（test_baseline 41 PASS、task_headers 7 PASS），资源构建与内部探针均无编译错误/警告。新增资源回归需链接隐藏实现的精确对象，本轮未扩大生产符号可见性或改 CMake/CTest，自动 CI 收口留给 T5。七份 C++ 修改行 clang-format/版权及 diff 检查通过；核对 908 个非忽略文件，仅九份允许文件改变，旧公共头、Backlog 和其他既有修改 hash 保持。只在 Close 做随持有状态线性的析构/容量释放，无新增状态成员、热路径扫描/算法、线程或 runtime 锁，未测完整 Submit 吞吐；不承诺 RSS 立即归还 OS。证据与切片 diff 在 /tmp/baseline-b06-close/；验收后 WIP=0 停止，T4 不勾选，不进入后续切片/T5，不提交/推送。
- B06 责任拆分文档（2026-10-08，未修复）：按用户要求把插件显式状态管理/容量/版本/关闭与封装算子的上游/活动/超时/时间驱动分开，登记独立 baseline-operator 待办规格。候选独立 IID 保留旧虚表，受管模式显式启用；新增线程/runtime 锁为 0，已有 registry mutex/配置原子发布及 Cancel 的同步边界明确。旧统一 TTL/LRU 的 A/B 建议及尺寸不再作为当前方案依据；历史日志保留，不将候选设计声明为已批准。T4/T5 未勾选，未实施生产代码。
- B06 核查与影响讨论检查点（未修复、T4 未完成）：当前 .so 公开探针与精确 CMake 对象探针复现 Value/Ratio 各 1024 key 后远期 bucket 保留 1025 状态、evicted=0；Relation fusion cap=2/TTL=5 下有 129 个 basis、516 个 routed、129 个消费游标及 516 个 spec，fusion 已淘汰 128 只剩 1，关闭 fusion 同样增长。单源真实 refresh 到 version=17 后保留 48 个子状态，17 个历史 version 仍可精确查询/预测；关闭句柄仍持有 Value 的 1029 rolling/4 artifacts/4 seeds 和 Relation basis/cursor。合法 persistence cap=3 实际保存 5 键，仅诊断，固定 evidence universe 与 source/版本无界问题区分。私有删除模拟确认：清除 source/消费游标会让原拒绝重复 bucket 重新被接受；有 seed 保留训练末闸门与 Bootstrap 模型/预测，但丢失在线游标，Rolling 查询需重新 Submit；删除旧子模型使其精确查询/预测变 kNotTrained，active 保持。建议 A：可选容量/TTL配置、成功消费 LRU/有预算扫描、source 整体回收、最多两版本、模型数量准入和 Close 立即释放；B 可拒绝新身份以保留任务存续期消费游标。候选 RollingState=1040（现1024，+16）、source value=56（原8，+48）、child ref=16，含动态 capacity 成本；新增固定维护与被淘汰 source 的扇出删除，不做全表扫描/时区转换，尚未测候选完整 Submit 性能。三个探针最终编译无错误/警告，两套事实及模拟断言 exit=0，格式和范围检查通过，生产源码/公共接口/既有测试/用户 Backlog hash 保持；因新配置、淘汰语义和算法/结构成本超出现有授权，本轮停在具体方案讨论，不勾 T4，不开始 T5，不提交。详细方案、日志、链接输入/指纹及候选布局在 /tmp/baseline-b06-evaluation/。
- B11 完成、T3 收口：用户授权方案 A 后，内部 RollingLastSubmitView 保存最近成功 Submit 的三个 band 值、update_weight 和四个标志，bucket 复用消费游标；冷启动/普通成功出口缓存，查询读取，diagnostics 标记 last_submit 与 current_state。Bootstrap/Load 默认无在线结果，保留参数 band、控制/告警=false、weight=0；拒绝和失败不覆盖。新增公开回归在旧实现上失败；修复后首点/正常/降权/低支撑/异常跳过、Value/Ratio/Relation routed、配置/cap 和实际极端 z 告警、失败/只读保持、单目标重训及整份重载均通过。B09 分别核对实际事件/非事件检测带，保留全部当前状态字段比较并增加非事件未来序列中心/上下界一致性；B3 成熟度跨界仍保留提交与当前状态不同的时点。精确当前 CMake 对象的 16 组同状态 z/cap/multiplier/Fourier/trend/monthpos/event 对照及 LevelReady 极端告警缓存通过，POD 实测 40 字节、RollingState=1024（原984）。新增固定写入及已有状态复制、查询诊断字符串，无新模型计算/扫描/时区转换/动态缓存成员/锁；未实测完整 Submit 吞吐，不声称零耗时。12 个常规 baseline target 最终构建及测试全部 exit 0，test_baseline 41 个 PASS、task_headers 7 个 PASS，静默断言目标按 -UNDEBUG 执行；三份源码修改行格式、diff 和范围检查通过，公共头文件及其他既有修改 hash 保持。源码 diff、失败/成功日志、精确链接和结构证据见 /tmp/baseline-b11-evaluation/implementation/B11-result.md；本轮立即停止，未启动 T4/T5、未运行 Feature 全量 CTest、未提交/推送。
- B11 核查与方案检查点：当前 .so 公开接口复现 Value 首点 control 为 false/true/1、snapshot 为 true/true/0，Sampled/Ratio 低支撑为 false/false/0 而 snapshot 仍 true/true/0，异常跳过学习及 Relation 四个 routed 首点也存在差异。精确当前 CMake 对象验证同状态 band_z 改变检测带而 snapshot 不变，季节/trend/event 的预测中心与 level-only snapshot 不同，隔离 LevelReady 极端 z 的 can_alert 分支未体现在 snapshot；Bootstrap 无在线 Submit 也已有快照，预测/拒绝及 artifact 重载保持断言通过。B2/B3 仅定义字段结构，未明确统一时间点，因此不把不同视图的差异直接当作检测算法错误。建议 A：只缓存最近 kOk Submit 的 band、控制及告警判断，bucket 复用消费游标，既有 diagnostics 标明视图；无在线结果时保留参数 band 并明确未有决策。候选 POD 实测 40 字节，RollingState 984→1024，100,000 个 rolling 子状态约 +3.81 MiB；增加固定写入及已有状态复制，不重算模型/扫描/转换/动态分配/加锁，未测候选完整 Submit 吞吐。两个 probe 编译与事实断言通过，候选契约在现有 .so 首点 can_score 上失败；生产源码与公共接口及其他既有修改 hash 保持。具体视图、替代方案、B09 回归影响和验收锚点见 /tmp/baseline-b11-evaluation/B11-proposal.md；因 JSON 值语义和内部结构成本需按用户条件讨论，本轮停止，B11/T3 未完成，T4/T5 未启动。
- B09 完成：在已绑定日历基础上为 Value/Ratio/Relation routed 检测与预测加模型空间事件效应，学习观测扣除同一效应，原 observed/observed_model 保持；hint/task/calendar 身份版本保护、无日历和无可用 hint 保持。旧公开回归失败；修复后两种对齐下 Value/Ratio 同模型中心/上下界、边界、重复代码、单点/序列、完整去事件学习快照及重载通过，Relation 各七个事件子模型通过，纽约重复小时 Rolling 回归通过。实际 CMake 对象计数验证真实 Relation Submit 七个 routed 结果仅一次查询、事件时区转换 0。test_baseline、test_baseline_task_headers、test_baseline_rolling_state 最终构建和全部测试通过；六个源文件修改行格式 replacements=0，公共头文件与其他既有修改 hash 保持。新增工作为 seed 查找、身份比较、区间查询和各 seed 系数求和；临时集合随命中数增长，无 entries 全扫描、每 series 常驻状态或锁；未重测完整 Submit，不宣称零耗时。证据在 /tmp/baseline-b09-evaluation/rolling/B09-rolling-result.md；本轮已停止，T3 尚含 B11，T4/T5 未开始。
- B09 预编译基础完成（B09/T3 尚未完成）：用户要求时区固定、尽可能消除转换成本后，在 Value/Ratio/Relation 创建时绑定实际任务时钟，按时区转换点编译半开 bucket 区间并建立只读索引，同 code 重叠/相邻区间合并。旧实现的零转换契约 probe 失败；实际 CMake 对象的 24,100 组完整命中集合/指示行差分通过，绑定查询转换调用为 0，覆盖负 bucket、五档长度、四种时区及春跳/秋回。公开插件新增继承/显式时区及纽约重复小时回归；test_baseline、test_baseline_task_headers 构建和测试通过，修改行格式与 diff 检查通过，其他既有文件 hash 保持。固定 CPU 0、七轮串行交替、当前未优化匹配对象，256 条上海事件的旧/新区间查询中位 221.442/0.261 us，按新输出容器绑定中位 0.277 ms；4096 条为 3676.732/0.386 us、5.019 ms。每区间 32 字节，256 条索引 8 KiB，另有每任务日历副本；不新增每 series 状态或运行时锁。数据是匹配函数隔离测量，不代表完整 Submit 或 Release 性能；Rolling 尚未消费 hint，本轮在此停止。源码、计数 probe、原始性能 CSV、快照 diff 及说明见 /tmp/baseline-b09-evaluation/B09-precompile-result.md。
- B09 核查检查点（历史记录，随后预编译策略已获授权）：任务已持有日历，seed 已保存事件系数，但 Rolling runner 没有消费。公开接口 probe 链接当前 .so，Value/Ratio 实训事件 bucket 的 Bootstrap 中心为 493.189484190/0.671546834，Rolling 为 100.680760102/0.200776400；实际事件观测均判 outside=1。准确复用当前 CMake link.txt 的对象做同模型 hint 有/无对照，forecast/Submit/完整快照均相同；候选在模型空间加事件效应后与 Bootstrap 中心一致，扣除效应后的提交残差约为 0。全部调用状态、单点/序列及预测快照保持断言通过；Relation routed 未贯通路径已静态核查，尚无公开接口实测。原 B2 将 event_hint 定义为可选提示，本项是本次要求补齐的在线事件能力。最小方案仅为有事件 seed 且日历匹配的路径增加 O(entries+codes+coeffs) 匹配，同一次 Relation Submit 复用命中集合；不改公共接口/格式或每 series 常驻状态。CPU 0、五轮各 512 次、当前 -g 无 -O 的实际匹配对象，4/64/256 条目中位平均 us 为 UTC 0.229/1.350/4.821，上海本地时间 3.911/62.066/246.087；这是匹配函数隔离耗时，非完整 Submit 增幅或 Release 结论。新增扫描和时间转换属算法成本，按用户条件停止生产修改并讨论。具体方案、源码、原始/重复日志、精确链接输入及指纹在 /tmp/baseline-b09-evaluation/B09-proposal.md；B09/T3 不标完成，未启动 B11/T4/T5。
- B08 完成：用户已选择方案 A。单点/序列共用内部标量计算，保留原概率区间，与 sigmoid(logit(p)±z*max(sigma_ref,1e-3)) 取包络，既有 uncertainty_source 同时标明两种来源；mu/confidence、公开类型/配置/JSON/artifact 版本保持。新增实训噪声回归在旧实现上失败；修复后低噪声区间保持 [0.430278056,0.568183012]，高噪声区间为 [0.000006551,0.999990873]。固定同模型提高 sigma 扩张区间，accepted_count 200→2000 后仍包含残差范围；覆盖概率端点、sigma 下限/大尺度、四档 confidence_level、序列各点与单点一致、导出重载一致。test_baseline、test_baseline_task_headers 构建及全部测试通过，构建无警告/错误，修改行格式 replacements=0，diff 检查通过。每点增加固定 log/exp/min/max 与来源标记，训练、Submit、seed、状态布局、分块缓存、扫描及锁无本轮改动；仅作算法/结构评估，不宣称实测零耗时或已校准覆盖率。证据在 /tmp/baseline-b08-evaluation 的 test_red.log、build_green.log、*_green.log 和 *.slice.diff。T3 仍含 B09/B11，保持未勾选，本轮已停止。
- B08 修复前核查（历史检查点）：原 B1 设计与当前设计明确 Ratio 首版采用先验/覆盖度概率不确定性，而总体“正常波动范围”说明及 sigma_init 响应要求存在口径冲突。修复前插件实训低/高噪声及隔离同模型 sigma/coverage 对照通过：仅提高 logit sigma_ref 不改变区间；只将 accepted_count 从 200 增到 2000，原带从 [0.430278,0.568183] 收缩到 [0.477328,0.521133]。当时形成原区间与 logit 残差带取包络的候选并停下来讨论，未修改生产源码；随后用户授权方案 A，完成证据见上一项。具体设计依据、原始数据、数学方案和回归锚点在 /tmp/baseline-b08-evaluation/B08-proposal.md；原始 probe 状态及单点/序列一致性断言通过。
- B10 完成、T2 收口：Value/Ratio 回归各自在修复前复现旧 runtime bucket=500 未重建到训练末 199；成功 Bootstrap 后只替换目标 runtime，成功 Load 后按既有整份 artifact 替换语义重建运行状态。新增回归覆盖首次预热/重训、其他 series 完整保持、禁止替换/训练失败/导入失败的 artifact、seed、完整快照和配置保持、成功导入恢复及移除文档外旧状态、重新提交和重复拒绝。test_baseline、test_baseline_task_headers、test_baseline_rolling_state 构建及测试通过，既有 B02/B03 回归通过；修改行格式、版权头及 diff 检查通过，公共接口和格式无改动。Submit/预测算法、状态布局、锁及原 seed 扫描不改；显式模型替换承担必要的状态销毁/初始化，性能评估按算法和结构口径。证据在 /tmp/baseline-b10-evaluation/B10-result.md 与 *_green.log；本轮在 T2 完成后停止，未开始 T3/B08。
- B05 完成当前默认路径的性能检查：2026-10-07 12:47～12:49 UTC，复用同一 probe、CPU 0、128 source 和旧/新同构建插件，旧/新各预热一次后固定 8 轮串行交替，全程不启动构建/其他测试。Value/Sampled/Ratio/Relation 旧→新中位 us/次为 43.241→42.787、44.915→42.665、43.633→42.628、301.747→300.688（-1.049%/-5.011%/-2.302%/-0.351%），配对中位差异均非正，所有状态/checksum 通过；本组未观察到稳定退化，首轮总体中位变慢未复现。Value 配对噪声较大，几何均值 +2.374%，描述性 log-t 95% 区间 -6.859%～+12.523%，不能作为性能等价或任意场景零开销证明，也不宣称负中位差异为加速。B05 在已测条件下收口；详细方法、全部样本与指纹在 /tmp/baseline-b05-evaluation/B05-performance-clean.md、full_submit_comparison_clean.json，首轮日志保留。本轮生产源码未改，沿用 12 个已通过目标；T3 仍含 B08/B09/B11 保持未勾选，B06～B11 未开始。
- B05 上一轮功能检查点（当时未完成性能验收）：在创建/导入任务已有的 sampled profile 解析中缓存内部 identity 标志，在已读取的 RollingConfig snapshot 中带出 eps_logit，不增加 Submit profile 查询。identity/log1p 的在线正变换、Bootstrap 与 Rolling 逆变换、snapshot 及 seed model_space 使用一致口径；artifact 加载拒绝变换不匹配且原 artifact/snapshot 保持。Ratio 在线 logit/clip 诊断消费配置 eps，训练先验平滑保持。旧实现的 identity/Ratio 回归均失败；修复后 12 个常规 target 构建和测试通过，修改行格式和 diff 检查通过，公开接口/JSON/artifact 格式无修改。sizeof TaskSpec/RollingConfig 各 +8 字节，RollingState 不变。
- B05 首轮性能检查点（历史记录）：固定 CPU 0、128 source、8 轮交替，旧/新同为当前未优化构建，probe 为 -O2。Value/Sampled/Ratio/Relation 首轮中位差异 +12.592%/+6.084%/+13.284%/+5.390%；与构建重叠且轮次波动明显，既不能确认为稳定退化，也不能宣称无影响。时间盒到期停止于同一 T3/B05 检查点，未开始 B06；当时决定下一轮先无构建干扰复测，确认性能影响则按用户条件讨论。详细数据与旧插件在 /tmp/baseline-b05-evaluation/B05-checkpoint.md，复测收口见前述新证据。
- B04 完成：Value/Ratio 在聚合前使用与在线相同的输入域，sampled 拒绝零 sample_count；Value/Ratio/Relation 聚合后拒绝非 finite 输出。Relation 同时校验位置绑定的 metric 名称、finite 正 total、精确数组对齐和 finite 非负 masses；保留部分 total 覆盖及缺失指标进度。Submit 先准备全部指标，再修改 routed/basis/fusion，默认校验合并原投影循环，内部 ObserveValidated 避免重复扫描。新增回归在旧实现上失败，修复后五个相关目标构建及测试通过；多指标非法提交保持完整 task/source 快照，修正后同 bucket 可继续；basis-only、禁止 basis 更新、独立 basis/投影拒绝非法输入。修改行 clang-format replacements=0，diff 检查通过，公开头文件无 diff。
- B04 完整性能对照：固定 CPU 0、128 source、四轮中位；旧/新插件使用当前 -g 且未启用 -O 的构建，probe 使用 -O2，直接测 Submit。默认 3 groups 为 157.077 → 134.778 us/次，本组未观察到退化；basis-only 3 groups 为 1.088 → 1.354 us（+0.266 us）。关闭子状态 3/256/4096 groups 分别为 0.557 → 0.748、0.545 → 3.433、0.521 → 51.201 us，新增成本约 0.190/2.888/50.680 us。必要前置扫描属用户获准范围，但当前未优化插件实测明显高于旧 -O2 外置原型，不能沿用原型数字或外推默认检测/Release 性能。完整方法和日志在 /tmp/baseline-b04-evaluation/B04-result.md；T1 达到验收，B05～B11 尚未开始。
- B01 收口补充：Relation source 预热水位仅使用 coverage_report.train_end_bucket，不使用缺失的总 theta 默认 reference=0。负训练 bucket -10～-1 后在线 0 成功、重复 0 拒绝；回归修复前失败、修复后通过。最终六个相关目标构建及回归通过，修改行格式和 diff 检查通过；全量 Feature 尚未完成。
- B04 修复前评估：共享 Relation 群组输入校验增加数组读取；隔离原型无子状态配置 256/4096 groups 额外耗时约 0.201/2.684 us（约 +40%/+523%），不能外推为默认检测退化或最终融合实现结果。按用户性能条件停止生产修改，待讨论 /tmp/baseline-b04-evaluation/B04-proposal.md；B05～B11 尚未开始。
- B03 完成：导入校验 basis 与 fusion 的身份/指标口径及 routed 子模型身份/时钟/日历，拒绝不兼容 group id/version、metric、feature_base、k_head、other_group_idxs；失败保持 artifact/config/完整 source snapshot。原回归失败，修复后 test_baseline / test_baseline_task_headers 通过；不改格式和检测热路径。训练选择策略不要求重放，basis 按其已保存的 support/stable head 消费。
- B02 完成：单 source Bootstrap 仅重建目标 source；其他 online-only、已预热和带分隔符 source 保持完整快照和重复拒绝。强制替换目标后仍可正常提交。回归修复前失败、修复后 test_baseline 通过；全量 Load 行为保留，Submit 热路径无 diff。
- B01 修复前评估与用户继续授权：内部单游标候选 +8 字节/状态，source 水位表約 70 字节/源；隔离原型轻量提交中位 +4.98%，默认检测未观察到退化。此证据不替代完整修复验收。
- B01 完成：重复/过期 Value、Ratio、Relation 在证据修改前拒绝；跳过学习的合法观测推进内部游标，模型学习锚保持；basis accumulator 不重复累计。五个相关测试通过，新顺序回归在修复前插件上失败。完整对照固定 CPU、128 源，常规路径各八组未测到退化；轻量无子状态配置旧/新中位 408.2/506.8 ns（约 +24.2%，约 +98.6 ns/提交），属于获准内部水位检查的实际成本，不能沿用原型 +4.98% 数字。日志和基准在 /tmp/baseline-b01-evaluation。
