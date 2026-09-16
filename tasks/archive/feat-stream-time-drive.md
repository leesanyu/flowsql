# Feature: 流式算子时间驱动

状态：`[x]` 已完成
优先级：P0
前置 Feature：`stage-filter-pipeline`、`npm-basic-operator-plugin-lifecycle`（均已完成）
后续 Feature：`npm-basic-realtime-integration`

## 业务意图

让使用 Block Transform 数据路径的实时有状态算子可以声明一个最早维护 deadline，并在暂时无包、持续繁忙
以及输出背压解除后收到串行时间通知。通知产生的结果继续走既有 Schema、过滤、链式传播和 consumer
背压边界；现有 V1 算子无需修改即可保持原行为。

本 Feature 提供通用调度能力。后续 `npm.basic` 适配器负责把全部 enabled feature 的 session、snapshot 和
transaction deadline 聚合为任务级最早 deadline；`observing` 只选择前台结果，不参与调度。

## Non-Goals

- 不修改 `IBlockTransformOperatorV1 / IBlockTransformTaskV1` 的布局、虚函数或语义，不要求 V1 provider
  实现空时间回调。
- 不修改旧 `IStreamOperator::Tick()` 路径，不合并两套算子 ABI 或重写 StreamRuntime。
- 不在时间通知中携带 capture progress、source idle confirmation、backlog、事件水位或丢包事实；这些属于
  `npm-capture-contract` 和后续 NPM 实时接线。
- 不实现 NPM 模块调度、周期快照、会话超时、协议事务超时、结果持久化或新的输出 Schema。
- 不提供专用 timer 线程、并发 `ProcessBlock/OnTime/Flush`、硬实时延迟保证或任意多定时器框架；每个任务只
  向框架暴露一个最早 deadline。
- 不绕过同步 output consumer 的背压，不承诺在 consumer 正阻塞时并发执行时间回调；解除阻塞后立即检查
  已到期工作。
- 不自动重试失败的时间回调，不在 Cancel、source/transform/consumer 错误后调用正常 Flush。

## 核心契约

### 可选版本化 ABI

V1 公共类型保持原样。新增 `IID_BLOCK_TRANSFORM_OPERATOR_V2`；Scheduler 对每个功能算子先解析唯一 V2，
没有 V2 时回退唯一 V1。V2 provider 仍由现有 C++ 插件 ABI V2 描述符导出，BinAddon 将其识别为
`block_transform_v2`；动态能力 lease 必须持有到 `ReleaseTask()` 完成。

公共头文件冻结以下最小形状；所有带 `struct_size` 的结构由调用方零初始化并填写大小，错误不得发布半初始化
task 或输出：

```cpp
constexpr uint32_t kBlockTransformContractVersionV2 = 2;
constexpr uint32_t kBlockTransformTimeDriveVersionV1 = 1;

struct BlockTransformTaskConfigV2 {
    uint32_t struct_size;
    uint32_t contract_version;
    const char* task_id;
    const char* with_params_json;
    const char* pushed_filter_plan_json;
};

struct BlockTransformTimeDriveStateV1 {
    uint32_t struct_size;
    uint32_t contract_version;
    uint8_t armed;
    uint8_t reserved[7];
    int64_t deadline_ns;
};

struct BlockTransformTimeEventV1 {
    uint32_t struct_size;
    uint32_t contract_version;
    int64_t monotonic_now_ns;
    int64_t wall_now_ns;
};

interface IBlockTransformTimeDrivenTaskV1 {
    virtual ~IBlockTransformTimeDrivenTaskV1() = default;
    virtual int GetTimeDriveState(BlockTransformTimeDriveStateV1* state) = 0;
    virtual int OnTime(const BlockTransformTimeEventV1& event,
                       std::vector<BlockTransformOutputV1>* outputs) = 0;
};

interface IBlockTransformTaskV2 : public IBlockTransformTaskV1,
                                  public IBlockTransformTimeDrivenTaskV1 {};

interface IBlockTransformOperatorV2 {
    virtual ~IBlockTransformOperatorV2() = default;
    virtual std::string Category() const = 0;
    virtual std::string Name() const = 0;
    virtual std::string Description() const = 0;
    virtual int CreateTask(const BlockTransformTaskConfigV2& config,
                           IBlockTransformTaskV2** task) = 0;
    virtual void ReleaseTask(IBlockTransformTaskV2* task) = 0;
};
```

V2 config 的字符串仍只在 `CreateTask()` 调用期间借用，provider 必须在返回前复制所需内容；
`contract_version` 必须为 V2，未知版本或过小 `struct_size` 直接拒绝且不得发布 task。

V2 task 继承全部 V1 数据、终结和取消语义。`GetTimeDriveState()` 只能在执行 task 成功 `Open()` 后调用：
`armed=0` 时忽略 `deadline_ns`；`armed=1` 时 deadline 是非负、绝对的单调时钟纳秒。需要在收到首个 packet
前取得时间原点的 task 可以先声明 `deadline_ns=0`，由 runtime 立即通知一次。状态查询必须是无副作用的
幂等观察，在下一次 ProcessBlock、OnTime、Flush 或 Cancel 前重复查询应返回相同状态；`armed` 只接受 0/1，
reserved 字节必须为零。

`OnTime()` 的 `monotonic_now_ns` 只用于 deadline、间隔和调度，必须非负且不回退；`wall_now_ns` 是 Unix
epoch 纳秒，只用于结果观察时间，允许因系统校时变化，禁止反过来驱动 deadline。时间源由 runtime 提供，
测试必须可注入确定性时钟而不依赖真实 sleep。

`OnTime()` 与 `ProcessBlock()` 使用相同状态码：`kContinue`、`kStop` 或负错误；outputs 必须在入口为空，
失败时也必须为空。成功可以产生零到多个与 `Open()` 输出 Schema 一致的 batch。一次通知只处理有界且有
进展的到期工作切片；仍有维护积压时由 task 重新 arm。返回 Continue 后必须 disarm，或声明严格大于本次
`monotonic_now_ns` 的 deadline，使 runtime 在再次 poll/取消检查后继续调度，禁止无进展忙循环。

Schema probe task 只执行 `Open → Cancel → ReleaseTask`，不得收到 `GetTimeDriveState()` 或 `OnTime()`；
只有 execution task 参与时间调度。V2/V1 同名解析规则是“唯一 V2 优先，否则唯一 V1”；同一候选版本内多匹配、
非法 lease、未知结构版本或过小结构均明确失败，不静默降级。

### Runtime 调度与输出

`BlockTransformPipelineRunner` 在 Open 后和每次完整处理输入、时间输出或 consumer 返回后读取单调时钟并检查
deadline。到期时先调用 `OnTime()`，校验状态、输出及重新 arm 的进展，再把结果交给现有输出过滤和 consumer；
没有到期任务时，`PollBlock()` 等待上限取配置 poll timeout 与距离最早 deadline 的较小值。

持续有数据时，当前 batch 的 `ProcessBlock()`、输出交付和 `ReleaseBlock()` 先完成，runtime 在下一次 poll 前
检查到期时间，保证定时工作不会只依赖 Poll timeout。consumer 阻塞期间不并发回调；consumer 返回后用新的
当前时间立即补做已到期维护。调度是 best-effort 串行保证，不承诺操作系统级硬实时上界。

单个 task 的 Process、OnTime、Flush 均由 runner 线程串行调用；并发 Cancel 继续使用既有幂等契约，并必须
使正在执行的阻塞 OnTime 尽快返回。时间输出不得旁路 transform residual、Schema 校验、consumer 或计数；
null batch、失败同时返回输出、单调时钟回退、deadline 无进展及时间状态查询失败均为 transform contract
error。

### 多级链与终结

同步 Block Transform chain 对所有 V2 stage 的 armed deadline 取最小值；V1 stage 不参与时间调度。到期时按
stage 顺序重新检查各 stage，只通知 `deadline <= now` 的 stage。第 `i` 级 `OnTime()` 输出先经过第 `i` 级
residual filter，再作为普通输入依次调用 `i+1...N` 的 `ProcessBlock()` 和 residual filter，最终才交给
consumer；不得回送本级 `ProcessBlock()`，也不得跳过下游。

上游时间输出改变下游状态后，下游在轮到自身时按最新 deadline 决定是否仍需通知。任一级 OnTime 或下游
ProcessBlock 返回 Stop，整条 chain 在已产生输出交付后正常停止并单次 Flush；任一级失败则取消全部 stage，
丢弃该失败调用的输出且不 Flush。

| 终止事件 | 时间与终结行为 |
| --- | --- |
| source 正常 EOF | 不再发新的时间通知，立即按既有顺序单次 Flush 并交付结果。 |
| ProcessBlock / OnTime 返回 Stop | 交付成功输出；若有关联输入先释放输入，然后单次 Flush，终态为 stopped。 |
| 外部 Cancel / source cancelled | 幂等 Cancel source 和全部 task；不 OnTime、不 Flush。 |
| source、transform、time state、filter 或 consumer 错误 | 任务 failed，Cancel 清理；不做正常 Flush。 |

## 主链路

1. **单 task 实时执行**：Open execution task → 查询 deadline → 按最早 deadline 限制 PollBlock；有数据时完成
   Process/交付/释放后检查时间，无数据 timeout 时直接检查时间 → OnTime 结果走同一过滤和 consumer → EOF
   或 Stop 单次 Flush，Cancel/错误异常清理。
2. **多 stage chain**：Open 各 stage → 聚合 V2 stage 最早 deadline → 按 stage 顺序通知到期项 → 时间输出从
   产生级的 residual 开始向下游传播 → 最终结果进入唯一 consumer，终结规则与单 task 一致。

## Feature Tasks

- [x] T0：交付 V2 provider/task、时间状态/事件和插件能力识别的冻结契约与测试夹具，使既有 V1 二进制及执行
  行为保持兼容，并为后续 runtime 实现建立可执行锚点。
- [x] T1：交付单 task 的 deadline-aware poll、空闲/繁忙/背压后补驱动及统一输出/失败路径，使有状态算子在
  不新增并发线程的前提下获得不会被输入模式饿死的时间维护保证。
- [x] T2：交付混合 V1/V2 同步 chain 的最早 deadline、逐级通知和时间输出下游传播，使多算子 SQL 保持过滤、
  Schema、Stop、Flush 与错误语义一致。
- [x] T3：交付 Scheduler 的 V2 优先/V1 回退、动态 capability lease 和端到端生命周期验收，使静态与上传算子
  都能安全使用新能力且旧算子零迁移继续运行。

## Feature 验收

- 头文件 ABI 测试证明 V1 IID、类型尺寸/虚函数契约和现有 fixture 未修改；V2 小结构、未知版本、重复候选和
  非法动态 lease 明确失败，唯一 V2 优先、无 V2 时唯一 V1 正常执行。
- 确定性 fake clock 覆盖 deadline 限制 poll、连续 timeout、持续数据、consumer 推进时间后的立即补驱动、
  wall clock 回退不影响调度、monotonic 回退及无进展 rearm 失败，全程不使用真实 sleep。
- 时间输出经过 Schema 校验、对应 stage residual、下游 ProcessBlock 和最终 consumer；混合 V1/V2 chain
  使用最早 deadline，stage 顺序、Stop 和错误传播可复核。
- 正常 EOF/Stop 只 Flush 一次；Cancel 及 source、Process、OnTime、time state、filter、consumer 错误均不
  Flush，失败调用不得泄漏输出，全部 task 和动态插件 lease 按序释放。
- `test_framework`、`test_builtin`、`test_scheduler_e2e` 定向通过；Feature 收口时全量构建和 CTest 通过。

Feature 完成时至少执行：

```bash
cmake -B build src
cmake --build build --target test_framework test_builtin test_scheduler_e2e -j$(nproc)
ctest --test-dir build -R '^(test_framework|test_builtin|test_scheduler_e2e)$' --output-on-failure
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
git diff --check
```

## 完成证据

- T0 冻结 V2 provider/task、时间状态/事件与 BinAddon `block_transform_v2` 能力契约；V1 IID、接口布局和行为
  保持不变，动态插件 fixture 可独立导出 V1 与 V2 Block Transform 能力。
- T1 的单 task runner 使用可注入单调钟限制 poll，并在 timeout、持续数据完整处理、输入释放和 consumer 返回后
  串行补驱动；时间输出复用 Schema、residual、consumer、计数、Stop/Flush/Cancel 和错误边界。
- T2 的同步 chain 聚合混合 V1/V2 stage 最早 deadline；按 stage 顺序重新查询最新状态，时间输出从产生级
  residual 开始向下游传播，覆盖状态变化、Stop、失败不泄漏输出和全部 stage Cancel。
- T3 的 Scheduler 按“唯一 V2 优先，否则唯一 V1”发现 provider；V2 歧义、遍历错误或非法动态 lease 明确失败
  且不降级。V2 config 填写结构大小/版本，probe 不查询时间，execution task 的 data/time 接口进入单 task runner
  或混合 chain；动态 lease 保持到所有 `ReleaseTask()` 完成。
- `cmake -B build src`：通过。
- `cmake --build build --target test_framework test_builtin test_scheduler_e2e test_scheduler_mutation_guard
  -j$(nproc)`：通过。
- 定向 CTest：`test_framework`、`test_builtin`、`test_scheduler_e2e`、`test_scheduler_mutation_guard` 4/4 通过。
- `cmake --build build -j$(nproc)`：全量构建通过。
- 完整 CTest：在允许 loopback socket 的执行环境中 13/13 通过。受限沙箱内两个 Web 测试在创建本地 socket 时
  失败，未进入业务代码；相同二进制和命令在解除该权限限制后通过。
- 本 Feature 允许文件 `git diff --check` 通过；环境未提供 `clang-format` 可执行文件，新增 C++ diff 无超过
  120 列的新增行。
