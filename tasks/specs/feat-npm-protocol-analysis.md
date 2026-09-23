# Feature: NPM 协议模块运行时基础

状态：`[-]` 进行中（T0～T2 已完成，T3～T4 未实施）
优先级：P1
前置：`npm-basic-analysis`、`npm-session-analysis`、`npm-basic-parameters`、`stream-time-drive`（均已交付）。
条件依赖：显式启用标签化或模块声明标签准入时要求 `flow-labeling`。
后续：`npm-result-query`、`npm-shared-tcp-stream` 及 DNS、HTTP/1、TLS、ICMP 分析；后续 Feature 不互相反向依赖。

## Non-Goals

- 不实现 TCP/IP 重组、消息定界、协议请求/响应关联、ICMP 内容解析、TLS 解密或任何具体协议结果。
- 不实现结果数据库、历史查询、保留策略、跨消费者事务或投递重试；持久化归 `npm-result-query`。
- 不实现实时采集后端或生产实时 SQL 接线；本 Feature 只交付任务内时间契约和模拟采集事实下的验证。
- 不新增全局模块 IID、第二套插件加载器、动态模块 DAG、热插拔、模块并行或跨任务状态共享。
- 不重建会话、方向、NPI、标签 matcher 或预算；不改变已交付 Basic/Session SQL、Schema 和指标含义。

## 业务意图

为在同一 `npm.basic` 任务内扩展协议分析的模块开发者，统一输入、实体生命周期、时间推进与结果消费，
使多个模块复用一次基础处理，并能独立维护一个会话内的多个事务或没有端口会话的控制事件。

已有 `INpmAnalysisModule`、Basic/Session 类型化 writer 和任务预算作为兼容基础；本 Feature 补齐其
会话以外的输入与结果边界。交付可被后续能力直接接入的运行时，以现有模块和仅测试使用的模块/消费者验收；
生产 SQL 不提前接受 `dns`、`http1`、`tls`、`icmp` 等尚未交付的 feature。

## 核心契约

### 模块目录、配置与按需依赖

- 模块目录固定在 NPM 算子插件内，登记唯一模块 ID、配置解析/创建入口和结果描述；任务 Open 冻结拥有型计划，
  再创建任务私有模块。测试目录注入走相同校验路径，不进入生产插件注册或新增公共测试 SQL 开关。
- `features` 仍显式启用模块，未知、重复、空或不可用 ID 拒绝；至少启用一个结果模块。`labeling` 是能力项，
  不可被 `observing` 选择。模块配置或依赖声明不得隐式启用其他 feature。
- `observing` 精确选择一个已启用模块声明的实体；保留 `basic`、`session` 名称与旧默认值。实体 ID 在目录内唯一，
  Open 固定其 Arrow Schema，空结果也返回相同 Schema；多个实体不混合到一个前台 RecordBatch。
- 参数继续使用 `parameters.core` 与扁平模块 ID：信封/共享字段严格校验，仅解析已启用且可用模块的业务节点，
  未消费节点按既有规则忽略；旧 `framework` 拒绝、legacy 冲突及原子失败语义保持不变。
- 模块归一化计划声明输入种类、是否要求 labeling/TCP stream，以及可选的主标签 ID 集合。标签选择对应已冻结
  标签目录，非空、无重复且不含未匹配值 0；缺失能力、未显式启用 labeling 或未知标签均在 Open 失败。
- 声明 TCP stream 需求必须同时声明标签准入；本 Feature 没有生产 stream provider，显式请求应失败，
  不允许降级为逐 packet 应用解析。这里只冻结能力需求，字节事件、游标和缓存实现归共享流 Feature。
- 同一任务只创建一次 matcher；模块复用会话已有 `primary_label_id`，不重新匹配。无准入选择时不过滤会话；
  只有显式启用 labeling 才查询 provider/Resolve 配置，包和时间回调不得访问控制面。
- 工厂只在 Open 解析配置并生成拥有型模块状态；目录、计划和消费者绑定均在任务结束前保持有效。
  失败不发布半初始化 runtime；诊断携带模块/实体 ID 或 JSON 路径。

### 输入与会话事件

- 输入复用已有解码层路径；TCP/UDP 的 binding、方向、会话 Observe、标签和 NPI 采样均只有一条执行链。
  模块只解析所属应用/控制消息内容，不重新解析基础 IP/TCP/UDP 或调用 NPI。
- 每个可分发 packet 只形成一种输入事件：TCP packet、UDP datagram 或 ICMP/ICMPv6 control packet；
  按模块 input mask 分发一次，UDP 保留单 datagram 边界和捕获完整性，TCP 此处仍是 segment 事实。
- control 路径复用经校验的网络层范围和协议号，给出包含控制消息头的有界字节视图及截断事实；
  不解读 type/code、引用报文或 echo 身份，不创建 TCP/UDP session，不调用 NPI 或会话标签 matcher。
- 有 control 消费者时，合法控制报文进入独立路径且不使混合任务因缺少端口失败；没有消费者时保留既有
  unsupported 行为。未支持报文、非法层偏移、畸形网络头和非首片不伪装成控制事件，沿用明确失败语义。
- TCP/UDP 在完成唯一会话与识别处理后通知订阅者；标签准入只限制该模块，不影响 Basic/Session 全量输入。
  snapshot/end 同样按传输种类订阅和标签准入分发，control-only 模块不收会话事件；control 与标签准入组合拒绝。
- 保留 tuple reuse 的“旧实例 end → 新实例首包”及关闭的“终结包 → 当前实例 end”顺序；
  snapshot/end 复用会话只读视图。所有模块串行，SQL token 顺序不决定生命周期顺序。

### 核心数据结构

下列结构属于 `flowsql::npm` 的 NPM 内部共用头文件契约，T0 落地；不扩展框架、插件或算子 V1/V2 ABI。
除目录/计划的拥有型内容外，事件、RecordBatch 与结构内指针都只在同步回调期间借用。

| 结构 | 字段与语义 |
| --- | --- |
| `NpmModulePlanV1` | 拥有型 `module_id:string`、`input_mask:uint8`、`requires_labeling:bool`、`requires_tcp_stream:bool`、可选 `primary_label_ids:vector<uint32>`、实体描述列表。 |
| `NpmInputEventV1` | `kind`、`observation_domain_id:uint64`、原始 `packet::PacketView`、只读层信息指针、有界 `body:Span<const uint8_t>`、`body_complete:bool`、可空 `const NpmPacketView*`/`const NpmSessionView*`。 |
| `NpmModuleTimeV1` | 可选 `watermark_ns:int64`、`observed_at_ns:int64`；水位未建立时不可作事务到期判断，观测时间仅用于结果标记。 |
| `NpmMaintenancePlanV1` | 可选 `event_deadline_ns:int64`（会话/模块最早采集时间 deadline）、可选 `snapshot_deadline_monotonic_ns:int64`；两种时钟不混用。 |
| `NpmEntityDescriptorV1` | 拥有型实体 ID、所属模块 ID、Schema 版本、Arrow Schema、revision 语义（cumulative / event）及身份、revision、observed_at、is_final 列的名称映射。 |
| `NpmResultContextV1` | 任务拥有的 `task_id:string` 与每次 Open 唯一的 `run_id:string`；重复执行同一任务不得复用 run_id。 |

input mask 的三个位分别对应 TCP、UDP、control。TCP/UDP 的 packet/session 指针均非空，body 为经校验的
transport payload；control 两者为空，原始 packet、网络层信息和控制消息 body 有效，不伪造方向/主标签。
每条新结果至少含 `entity_instance_id:uint64`、`revision:uint64`、`observed_at:int64`、`is_final:bool`，
可选关联 `session_id`；模块 Schema 自行承载观测域与业务字段。Basic/Session 身份列映射到原 `session_id`，
保持原有列及 metadata，包括 labeling 启用时的 Schema 差异；新增公共上下文不注入旧 SQL 列。

```cpp
enum class NpmInputKindV1 : uint8_t { kTcpPacket = 1, kUdpDatagram = 2, kControlPacket = 4 };

interface INpmResultEmitterV1 {
    virtual ~INpmResultEmitterV1() = default;
    virtual int Emit(std::string_view entity_id, const arrow::RecordBatch& rows) = 0;
};

interface INpmProtocolModuleV1 {
    virtual ~INpmProtocolModuleV1() = default;
    virtual int OnInput(const NpmInputEventV1&, INpmResultEmitterV1&) = 0;
    virtual int OnSessionSnapshot(const NpmSessionView&, int64_t observed_at_ns, INpmResultEmitterV1&) = 0;
    virtual int OnSessionEnd(const NpmSessionView&, NpmSessionEndReason, int64_t observed_at_ns,
                             INpmResultEmitterV1&) = 0;
    virtual std::optional<int64_t> NextEventDeadlineNs() const = 0;
    virtual int OnTime(const NpmModuleTimeV1&, INpmResultEmitterV1&) = 0;
    virtual int Finish(int64_t observed_at_ns, INpmResultEmitterV1&) = 0;
    virtual void Abort() noexcept = 0;
};

interface INpmResultConsumerV1 {
    virtual ~INpmResultConsumerV1() = default;
    virtual int Consume(const NpmResultContextV1&, const NpmEntityDescriptorV1&,
                         const arrow::RecordBatch& rows) = 0;
    virtual int Finish() = 0;
    virtual void Cancel() noexcept = 0;
};
```

这些接口同步返回，int 为 0 表示成功，非零终止任务；emitter 绑定调用模块，只能写其已声明实体。
诊断区分配置/能力不可用、输入无效、实体/Schema 无效、预算不足、模块/消费者失败和取消，保留底层错误原因。
Basic/Session 可通过适配接入，现有纯虚接口及对应 typed validator 的业务语义保留；不引入 `void*` 业务载荷。
Abort 幂等且无结果输出，仅在在途模块回调结束后执行；consumer Cancel 可并发发出非阻塞取消信号，
销毁消费者前等待在途 Consume/Finish 退出，不在持有生命周期互斥锁时调用外部消费者。

### 实体身份、结果路由与资源

- 持久结果身份为 `(run_id, entity_id, schema_version, entity_instance_id)`；Basic/Session 复用 session ID，
  新协议实体由所属模块分配任务内单调递增的非零 ID，不以 DNS wire ID、TCP tuple 或 session ID 替代事务 ID。
- revision 从 1 严格递增；cumulative 表示同一实体累计快照，event 表示一次结果且仅 revision=1、is_final=true。
  is_final 仅表示实体生命周期结束，成功/失败/不完整由业务字段表达；终态后不再发出该实体记录或复用 ID。
- 实体状态和 revision 由所属模块维护并计费，终结即释放，不建立随历史结果无限增长的终态去重表；
  路由器验证实体归属、Schema、必要列和值，生命周期不变量由模块及其单元测试保证。
- 所有已启用实体（含 Basic 的周期/终态）经过同一 emitter/router；旧 `WriteBasic/WriteSession` 及
  Basic 直接投影的终态路径都必须纳入，不能只在前台 Drain 后附加消费者。
- Open 注入零或一个任务级附加 consumer，接收全部已启用实体；它是后续存储的接入口，不受 observing 或
  operator-stage WHERE 影响。前台只保留 observing；无附加 consumer 的非观察结果验证后即释放。
- Consume 先于该批前台交付，借用结果必须在返回前消费或复制到已计费的拥有型缓冲；无界排队或持有输入
  packet/batch owner 均禁止。consumer 的同步调用必须可终止，取消后不得无限阻塞。
- 每批先完成 Schema 校验及前台/路由缓冲预算预留，再调用 consumer；consumer 错误终止任务、不重试，
  当前算子调用的前台输出保持空。
  已成功消费的前缀不回滚，不承诺跨前台/附加 consumer 原子提交或持久化 exactly-once。
- 模块状态、实体关联和 deadline 容器计入 `kModuleState`，待交付/已交付仍存活的结果缓冲计入
  `kPendingOutput`；输入与会话沿用原预算。先预留后分配，失败释放已获资源，不静默驱逐状态或丢结果。
  返回后仍存活的 Arrow owner 继续持有预算租约；任务销毁不使其悬空，也不提前归还其字节。

### 时间与终结

- 事务超时使用采集事件时间：复用唯一 capture progress / out-of-order tolerance 形成单调 watermark；
  packet 成功分发后推进水位，control 输入也参与。时间戳早于已建立水位的包保持既有明确失败语义。
- 水位推进时串行通知全部 enabled 模块的 OnTime；模块在 deadline <= watermark 时结束对应实体，
  并撤销或更新最早 deadline。不得等 UDP/TCP 会话关闭才结束事务，observing 不改变 deadline 或回调。
- 离线按包时间推进，EOF 负责剩余状态。模拟实时仅在已知无 backlog 且满足 packet/idle 事实时推进水位；
  单调时钟/墙钟变化、Poll timeout 或 snapshot tick 均不能自行证明事务超时。
- NpmMaintenancePlanV1 聚合 session 与模块最早事件 deadline，周期输出另用单调时钟；不得直接把 epoch
  事件时间填入框架单调 deadline。生产适配器将其与采集事实接入 `stream-time-drive`，归后续实时接线。
- 正常 EOF：剩余 session end → 每个模块单次 Finish（含无会话实体）→ 附加 consumer 单次 Finish →
  完成前台终态排空。已终结实体不重放；无订阅会话的模块仍获得时间和任务结束通知。
- 创建失败、处理错误或取消：Abort/Cancel 并释放资源，不再启动正常 Finish、不伪造 EOF 或成功终态；
  在途 Finish 退出后按失败/取消收口。后续入口保持终止状态；取消信号可并发，模块回调始终互斥且不可重入。

## 主链路

1. **打开与多输入处理**：冻结 features/observing/参数/能力计划 → 创建唯一 core 和任务私有模块、绑定 consumer →
   复用 packet 层信息 → TCP/UDP 唯一会话/标签/NPI 或独立 control → 订阅分发 → 统一结果路由 →
   推进合法水位与模块事务 → 输出 observing 的固定 Schema。
2. **维护与退出**：数据进度或模拟实时事实推进水位/周期维护 → 全部 enabled 模块维护独立实体 →
   类型化结果进入附加 consumer 与前台选择；正常 EOF 有序 Finish，错误/取消只终止并释放全部任务状态。

## Feature Tasks 与测试锚点

- `[x]` T0：交付可编译的模块输入、实体、时间和消费接口及契约断言，使后续能力有可直接复用的接入边界。
  锚点：旧 Schema 映射、新实体最小 Schema、输入 kind/空会话关系、能力需求校验、非法实体/Schema 拒绝。
- `[x]` T1：交付任务打开与多输入分发，使启用模块共享一次基础处理且控制消息无需端口会话。
  锚点：默认/legacy/V1 等价，未知模块与缺失能力 Open 失败且无半成品；同包唯一 binding/Observe/NPI；
  UDP 边界与截断、control 与 TCP/UDP 混合、无消费者兼容失败、标签订阅隔离、参数节点不隐式启用。
- `[x]` T2：交付独立实体时间与终结管理，使事务在会话存活时可到期，EOF/取消/复用不会泄漏或重放。
  锚点：同 session 多实体、无 session 实体、deadline 边界、无包/积压/水位回退、切换 observing 不改变轨迹；
  终结包先处理、tuple reuse 隔离、EOF 恰好一次、重复取消、回调中取消、预算失败后资源归还。
- `[ ]` T3：交付统一结果消费与前台路由，使全部 enabled 实体可接入存储且消费失败保持明确的终止语义。
  锚点：Basic/Session 快照与终态均送达附加 consumer；仅 observing 进入前台；同任务再次 Open 的 run_id 不同；
  consumer 首批/中途/Finish 失败无重试、前缀不回滚、借用复制与 Arrow owner 预算、非观察结果无无限积压。
- `[ ]` T4：交付兼容回归与组合验收证据，使运行时可供后续 Feature 使用且未交付模块不会被误报为可用。
  锚点：现有 SQL/E2E 通过；两种测试实体与控制输入走生产 runtime 组合验证；生产目录无测试/未交付协议项；
  并发任务隔离、销毁后结果 owner 有效、定向 Sanitizer、格式检查及完整 CTest 通过。

依赖顺序：T0 → T1；T2、T3 基于 T1，分别交付生命周期与结果消费保证；T4 在全部前置完成后进行，保持 WIP=1。
测试模块只按输入计数/生成模拟事务与 deadline，不包含协议解析；测试 consumer 验证接入，不声称存储已交付。

## 完成出口

- 公共内部头文件可编译，现有 Basic/Session 及测试模块通过同一 runtime/consumer 路径，关键锚点均有可执行断言。
- 通过对应 CMake target、单元/E2E、定向内存生命周期检查；Feature 完成时全量构建及 CTest 全绿，符合格式规范。
- 不依赖 TCP 重组、真实协议解析器、结果数据库或生产实时源才能验收；所有实现任务完成后才归档及标记完成。

## 完成证据

- T0（2026-09-23）：新增 NPM 内部协议契约头文件与实现，落地输入/模块计划/实体/时间/上下文结构及三个纯虚
  接口；校验输入 kind、空会话和 body 范围、能力/标签需求、实体列映射及结果 Schema/行值。Basic/Session 描述
  直接复用已有 Schema 与 session_id，未改变旧 SQL 输出。跨调用 revision/final 生命周期仍由后续任务实现。
- T0 定向测试覆盖带/不带 labeling 的旧 Schema、新事务及无会话结果、非法映射/类型/null/零身份/revision、
  event 终态、缺失能力、无效标签、TCP/UDP/control 输入关系及截断；生产库与两个定向 target 构建通过，CTest
  `test_npm_basic`、`test_npm_protocol_contract` 2/2 通过（2.05 秒），新增 C++ 格式和 Diff 空白检查通过。
- T0 验收时仅完成契约与校验；当时未接入生产模块目录、时间或结果消费路径，未运行全量 CTest。

- T1（2026-09-23）：固定生产目录仅含 Basic/Session；配置复用 legacy/V1 信封及按需消费规则，内部测试目录
  使用同一解析/Open 校验路径。Open 冻结拥有型计划和工厂，校验实体/observing/能力/标签后创建任务私有模块；
  工厂失败释放先前实例，runtime/schema 不发布半成品，新实体空结果保持 Open Schema。
- T1 输入接线复用唯一 binding/Observe/NPI，以适配层按 TCP/UDP/control 与主标签订阅分发；独立 control
  路径复用网络边界校验，支持 IPv4/IPv6 及已解码扩展头，传递包含控制头的有界 body 与截断事实，不创建
  端口会话、不调用 NPI/matcher。control 时间同步到既有水位及标签准入预测，保持混合批次的会话重建一致。
- T1 新增三组集成测试覆盖目录/参数按需、混合输入、唯一会话计数及 NPI、UDP 截断、控制路径兼容失败、
  IPv6 扩展头和截断、非法层偏移/非首片、标签隔离、未知标签/缺失 stream 能力、迟到包及工厂失败资源释放；
  既有默认/legacy/V1、SQL、会话生命周期与标签回归一并通过。生产库及两个定向 target 构建成功，无新增
  编译警告/错误；最终 CTest 2/2 通过（0.49 秒），14 个 C++ 文件格式检查通过（既有大测试文件检查新增区域）。
- T1 边界：协议模块仅接入输入及已有 session snapshot/end；事务 OnTime/Finish 生命周期归 T2，统一结果
  emitter/consumer 归 T3。T3 接线前协议 Emit 明确返回 ENOTSUP；本轮测试模块不产生业务结果，不声称具体
  协议分析或统一结果消费已经交付。T1 验收时 T2～T4 未勾选，未运行全量 CTest，未 commit/push。
- T2（2026-09-23）：协议 adapter 接入 `NextEventDeadlineNs`、`OnTime`、`Finish` 和幂等 `Abort`；离线每个
  packet/control 成功分发并实际推进 watermark 后串行通知模块，实时仅在 backlog 已知为空且 packet/idle
  事实成立时推进，未建立水位、积压、无 idle 证明及水位不变/回退均不触发事务到期。
- T2 正常 EOF 顺序固定为剩余 session end → 每个协议模块单次 Finish → 前台 drain；完成模块析构不 Abort，
  Finish/处理失败或取消只 Abort 未完成模块且不重放。模块回调内取消在回调退出后停止后续处理，重复 Cancel
  保持幂等；`MaintenancePlan()` 独立聚合 session/module 事件 deadline 与初始化后的单调 snapshot deadline。
- T2 生命周期测试模块自行维护同 session 多实体及 control 无 session 实体，以 `kModuleState` 计费并在到期、
  session end、Finish 或 Abort 释放；覆盖 deadline 等号边界、无包/积压/水位回退、observing 轨迹不变、终结
  packet 先输入后 end、tuple reuse 隔离、EOF/取消恰好一次、回调内取消、Finish 失败及预算部分预留回滚。
  最终生产库和两个定向 target 构建通过，CTest 2/2、0 失败（0.80 秒），本轮 C++ 格式与 Diff 检查通过。
- T2 边界：实体身份/revision/finality/预算仍由模块拥有，runtime 不引入通用实体容器；协议 Emit 仍明确返回
  ENOTSUP，统一结果路由留给 T3。T3～T4 未勾选，未运行全量 CTest，未 commit/push。
