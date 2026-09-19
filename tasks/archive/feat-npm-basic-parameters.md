# Feature: NPM 通用参数入口

状态：`[x]` 已完成（2026-09-19）
优先级：P1
前置 Feature：`npm-basic-analysis`、`npm-session-analysis`（均已完成）
后续 Feature：`npm-labeling`、`npm-shared-tcp-stream`、`npm-protocol-analysis`

## Non-Goals

- 不改变 `features` 的模块启用语义、`observing` 的单一前台结果语义，也不把 `input_namespace`、
  `source_domains` 等输入绑定藏入 `parameters`。
- 不允许参数节点隐式启用模块；未启用、未加载或未知模块的节点只作为未消费配置忽略，不改变执行集合。
- 不实现流量标签化、TCP 字节流或 HTTP/DNS/TLS 等业务能力，不 Resolve 配置快照，也不解释 CIDR、VLAN、端口、
  协议或服务规则；本 Feature 只冻结 `labeling` 引用的参数位置和类型边界。
- 不删除旧 Basic/Session 顶层 `WITH` 参数，不改变未使用 `parameters` 的既有 SQL 行为。
- 不新增全局模块 IID、动态模块 DAG、Web 参数编辑器或跨算子通用配置框架；参数所有权限定在 `npm.basic` 任务内。

## 业务意图

组合 `npm.basic` 共享核心、Basic/Session 结果及后续协议模块的用户，不应为每个能力继续增加
`session_xxx`、`http_xxx` 等顶层 `WITH` 键，也不应面对同一配置由多个模块重复声明或按隐式优先级覆盖。

本 Feature 交付单一 `parameters` JSON 字符串入口：`framework` 承载任务共享运行配置，其余一级字段以
`features` 的精确模块 ID 承载模块私有配置。任务打开时一次性完成严格校验并冻结为拥有型结构；包、会话和时间
通知路径只读取类型化配置，不再解析 JSON。旧 SQL 保持兼容；信封、共享字段和已启用模块严格校验，其他模块配置
可随信封携带但不进入当前任务运行时。

## 核心契约

### SQL 参数角色

| `WITH` 参数 | 唯一职责 | V1 关系 |
| --- | --- | --- |
| `input_namespace`、`source_domains` | 输入源与 observation domain 绑定 | 保持顶层必填，不进入 `parameters` |
| `features` | 决定实际启用的结果/能力模块 | 保持顶层；参数节点不得改变其集合 |
| `observing` | 决定当前 Block Transform 的单一前台实体 | 保持顶层；不得决定模块是否执行 |
| `parameters` | 共享运行参数与模块私有参数 | 新增的单一严格 JSON 字符串入口 |

新格式示例：

```sql
SELECT *
FROM pcapfile.capture
USING npm.basic
WITH input_namespace='pcapfile.capture', source_domains='0:1',
     features='basic,session', observing='session',
     parameters='{"schema_version":1,"session":{"max_tcp_ranges_per_direction":1024}}'
INTO dataframe.session_metrics
```

`parameters` 的 SQL 外层值使用带引号字符串，内部必须是严格 JSON；Scheduler 仍把每个 `WITH` 值安全包装为
字符串字段后交给算子，
`npm.basic` 再解析 `parameters` 的拥有型副本。数字、布尔、数组和对象在内层使用原生 JSON 类型，不接受用字符串
伪装的数字。

### V1 信封与命名空间

```json
{
  "schema_version": 1,
  "framework": {
    "max_tracked_bytes": 268435456
  },
  "basic": {},
  "session": {
    "max_tcp_ranges_per_direction": 1024
  }
}
```

- `schema_version` 在使用 `parameters` 时必填，V1 只接受整数 `1`；缺失或未知版本明确失败。
- `framework` 可省略，表示全部使用共享默认值；若存在必须是 object。它只表示当前 `npm.basic` 任务的共享框架，
  不是 FlowSQL 全局配置。
- 除 `schema_version`、`framework` 外，一级键是模块参数命名空间且必须是 object。`features` 已启用且当前可用的
  模块严格校验其节点；节点缺失表示使用默认值，空 object 表示显式使用默认值。
- 未启用、未加载或未知模块节点自动忽略，不校验其内部业务字段，也不写入拥有型配置或因后续加载而热生效；
  `features` 显式请求不存在或未加载模块仍失败。规划中的 HTTP/1 使用 `http1`，不使用含混的 `http`。
- 所有 JSON object 拒绝重复键；信封、`framework` 和被消费模块拒绝未知字段、错误类型与 `null`。未消费模块仅要求
  节点为 object，并受合法 JSON、重复键、大小和深度约束；错误携带稳定错误类别及 JSON Pointer 风格字段路径。
- SQL 字面量解码后的 `parameters` JSON 文本最多 64 KiB，嵌套深度最多 64；各 SQL 入口现有的语句长度限制
  保持独立，不假定均为 64 KiB。解析、分配或校验失败时原输出配置不变。

### 共享与模块参数

V1 把现有共享参数按原名放入 `framework`：

| 所有者 | 字段 |
| --- | --- |
| `framework` | `run_mode`、`result_mode`、`overload_policy`、`output_interval_ns`、`payload_sample_packets` |
| `framework` | `tcp_idle_timeout_ns`、`udp_idle_timeout_ns`、`out_of_order_tolerance_ns` |
| `framework` | `max_active_sessions`、`max_tracked_bytes`、`max_pending_output_bytes` |
| `basic` | V1 暂无私有字段；启用时只接受 `{}`，未启用时整个节点忽略 |
| `session` | 启用时接受 `max_tcp_ranges_per_direction`，整数范围 8～65536，默认 1024；未启用时整个节点忽略 |

枚举仍使用 JSON string；计数、字节数和纳秒值使用 JSON integer。字段默认值、上下界以及 offline/realtime 间的
既有联动不变，旧字段 `session_max_tcp_ranges_per_direction` 只在 legacy 模式保留。

### 流量标签化引用槽位

`labeling` 表示依据当前处理阶段可观察的特征，为报文、会话或协议实体附加零个或多个稳定具名标签。标签可描述
网段、VLAN、端点、隧道或已解析的 HTTP/DNS/TLS 服务；它不是模块启用开关、互斥分类，也不等同于处理准入。
只有在某处理阶段之前已产生的标签可用于该阶段准入，解析后标签不能反向控制生成它所依赖的重组或解析链。

流量标签化已启用且可用时，`framework.labeling` 是单个精确不可变引用字符串 `config.<name>@<revision>`；空字符串、
裸名称、`@latest`、revision 0 和非 Config 引用无效。省略表示未配置，是否必填由 `npm-labeling` 契约决定。

后续能力交付后的扩展位置固定为：

```json
{
  "schema_version": 1,
  "framework": {
    "labeling": "config.corp-labels@7"
  },
  "http1": {}
}
```

该片段只展示保留位置，不是本 Feature 完成时即可执行的配置。

本 Feature 只冻结字段位置、条件消费和精确引用词法契约。在 `npm-labeling` 未启用或尚不可用时，出现该字段不报错，
不校验引用、不 Resolve，也不写入任务配置。后续 Feature 启用该能力时，必须在任务打开阶段校验并 Resolve 一次精确
快照，编译为任务私有结构；逐包或逐事件路径不得访问 Config Channel、HTTP 或 SQLite。

### 拥有型数据与错误契约

```cpp
enum class NpmParameterSourceV1 : uint8_t {
    kLegacyWith,
    kParametersV1,
};

struct NpmFrameworkParametersV1 {
    NpmAnalysisConfig analysis;
    std::optional<std::string> labeling_reference;
};

struct NpmBasicModuleParametersV1 {};

struct NpmSessionModuleParametersV1 {
    uint32_t max_tcp_ranges_per_direction = 1024;
};

struct NpmParameterConsumersV1 {
    bool basic_enabled;
    bool basic_available;
    bool session_enabled;
    bool session_available;
    bool labeling_enabled;
    bool labeling_available;
};

struct NpmTaskParametersV1 {
    uint32_t schema_version = 1;
    NpmParameterSourceV1 source = NpmParameterSourceV1::kLegacyWith;
    NpmFrameworkParametersV1 framework;
    std::optional<NpmBasicModuleParametersV1> basic;
    std::optional<NpmSessionModuleParametersV1> session;
};

struct NpmParameterStatusV1 {
    NpmParameterErrorV1 error;
    std::string path;
};
```

`NpmParameterErrorV1` 至少区分 invalid JSON/envelope、unsupported version、duplicate/unknown consumed field、
invalid type/range、legacy conflict、invalid exact reference 和 allocation failure。解析函数先构造临时
`NpmTaskParametersV1`，只返回共享配置和已启用且可用模块的拥有型结果；T1 再在任务打开路径把该结果归一到
`NpmBasicTaskConfig`。返回后的字符串和容器不借用输入 JSON。
这些结构属于 `npm.basic` 内部契约，不注册全局 IID，也不向模块传递 RapidJSON DOM 或原始字符串。

### 兼容与单一来源

- 未提供 `parameters` 时继续走 legacy 解析；所有既有默认、错误、Basic-only 和 Session SQL 行为不变。
- 提供 `parameters` 时，`features`、`observing`、`input_namespace`、`source_domains` 仍按现有规则和默认值共存；
  `parameters` 不替代这些参数，也不改变模块启用集合。
- 提供 `parameters` 时，任何 legacy 调优字段同时出现都以 configuration-source conflict 拒绝，即使值相同也不设
  覆盖优先级。用户必须一次性把共享和模块调优字段迁入新信封。
- 同一逻辑配置用 legacy 或 V1 表达时，冻结后的 `NpmAnalysisConfig`、模块启用集合、Schema、输出和预算行为相同。

### SQL 字符串与重复参数

- 保留现有不带引号、单引号和双引号的 `WITH` 值；带引号值分别将 `''`、`""` 解码为单个对应引号，
  反斜杠不作为 SQL 层转义。空值仍无效，未闭合引号必须失败；引号内的逗号、分号及 SQL 关键字均为值内容。
- 同一 `WITH` 列表内完全相同的键（保持大小写敏感）须在写入 `unordered_map` 前拒绝；不同 USING/THEN
  阶段允许同名键。逗号分隔和引号后续文本仍按现有语法校验，不能由后值覆盖前值。
- 此规则属于通用 SQL Parser，须保证 `SplitSqlText`、顶层关键字扫描和 `WITH` 值读取一致；直接单 SQL
  与经 `sql_text` 切分的入口均在创建算子任务前拒绝无效输入，旧算子的无转义值保持原样。

## 主链路

1. **V1 参数打开任务**：SQL Parser 严格读取单个 `parameters` 字符串并拒绝重复键 → Scheduler 安全包装外层
   `WITH` JSON → `npm.basic` 校验信封和 framework、只校验 `features` 已启用且可用的模块并丢弃其余模块节点 →
   原子冻结拥有型配置 → probe、Process、time notification 和 Flush 只读同一类型化快照。
2. **Legacy 兼容打开任务**：未提供 `parameters` → 按现有顶层字符串参数和默认值解析 → 归一到同一拥有型任务
   配置 → 运行时行为与改造前一致；一旦出现新旧调优字段混用，在创建任何 task/runtime 前明确失败。

## Feature Tasks 与测试锚点

- `[x]` T0：冻结并以测试锚定 V1 信封、扁平所有者命名空间、拥有型结构和稳定错误路径，使参数归属及失败结果
  不依赖模块实现顺序或 JSON 字段顺序。
  - `[x]` T0.1：覆盖 schema version、重复键、共享与活动模块的未知/null/类型校验、非活动/未知模块忽略及原子失败。
  - `[x]` T0.2：锚定三处 SQL 词法、stage 级重复键和两类 SQL 入口，验证 Scheduler 包装后算子收到的
    `parameters` 等于 SQL 字面量解码后的值，且不改变旧算子配置。
- `[x]` T1：交付单一 `parameters` 传输与严格解析，使共享配置、Basic 和 Session 私有配置在任务打开时形成一个
  拥有型快照，并让包/会话热路径不接触 JSON。
  - `[x]` T1.1：完成 `framework` 共享字段及活动 `basic/session` 节点的类型、默认、范围校验，并丢弃非活动模块节点。
  - `[x]` T1.2：把 V1 与 legacy 归一到同一 `NpmBasicTaskConfig`，拒绝任意新旧调优字段混用。
- `[x]` T2：把参数快照接入 probe、离线和模拟实时 runtime，使相同逻辑配置在 V1 与 legacy 下得到相同 Schema、
  生命周期、预算和 Basic/Session 输出。
- `[x]` T3：交付 SQL/E2E 与完整回归验收，使旧任务无修改运行、活动配置可诊断失败、非活动模块配置可安全忽略，
  并为流量标签化和协议模块提供稳定扩展入口。

## 验收矩阵

| 验收面 | 必测断言 |
| --- | --- |
| 信封 | 缺失/未知 `schema_version`、根或模块节点非 object、过深及任意 object 重复键失败；字段顺序不影响结果 |
| 原生类型 | `framework` 与活动模块中的数字只接受 JSON integer；字符串数字、负数、溢出、错误枚举及类型错均失败 |
| 模块消费 | enabled+available 节点缺失使用默认、`{}` 合法且错误字段失败；未启用/未加载/未知模块 object 被忽略且不进入运行时；`features` 显式请求不可用模块失败 |
| 流量标签化 | 未启用或不可用时忽略 `framework.labeling` 且不校验/Resolve；启用后严格校验精确引用并只 Resolve 一次 |
| 单一来源 | `parameters` 与任一 legacy 调优键混用失败；输入绑定、features、observing 可正常共存 |
| SQL 词法 | 单/双引号成对转义、引号内分号/逗号/关键字、未闭合引号及同 stage 重复键；跨 stage 同名键合法；普通算子回归 |
| SQL 传输 | 直接单 SQL 与 `sql_text` 切分入口一致；Scheduler 包装后算子收到字面量解码值，不要求外层 JSON 字节或字段顺序一致；无效输入不创建算子任务 |
| 大小约束 | `parameters` 解码后超过 64 KiB 失败；各入口原有 SQL 长度限制不被误当作统一上限 |
| 兼容 | 既有 Basic-only/Session SQL、默认值及错误不变；legacy/V1 等价配置产生相同 runtime 与结果 |
| 生命周期 | 解析后改写输入字符串不影响配置；失败不修改旧输出；Process/通知/Flush 不再解析 JSON |

Feature 完成时至少运行：

```bash
cmake -B build src
cmake --build build --target test_framework test_npm_basic test_scheduler_e2e -j$(nproc)
ctest --test-dir build -R '^(test_framework|test_npm_basic|test_scheduler_e2e)$' --output-on-failure
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
git diff --check
```

## 完成出口

1. V1 `parameters` 信封、`framework` 和扁平模块命名空间具有按需、原子、拥有型解析结果。
2. `features`、`observing`、输入绑定和参数配置职责保持正交；参数节点不能启用模块，未消费节点被确定性丢弃。
3. 旧 SQL 行为不变，新旧调优字段不能混用；等价 legacy/V1 配置生成相同任务配置和可观察结果。
4. SQL 引号、重复 `WITH`、JSON 重复键、已消费字段的未知/类型/范围及流量标签化预留边界均有确定性测试。
5. 定向测试、完整构建与 CTest 全部通过，后续模块无需新增顶层调优参数即可扩展自己的一级命名空间。

## 完成证据

- T0.1：新增 `NpmTaskParametersV1`、消费者可用性快照、稳定错误枚举与 `ParseNpmParametersV1` 独立契约；迭代式
  JSON 解析限制 64 KiB/深度 64，任意 object 重复键和 JSON Pointer 路径确定，活动 Basic/Session 与 framework
  严格校验，非活动/未知模块 object 丢弃；字段顺序、输入所有权及失败不改输出均有断言。T0 阶段该入口尚未接入
  `NpmBasicTaskConfig`、operator 或 runtime；任务配置接入已在下述 T1 证据中完成。
- T0.2：通用 SQL Parser 已锚定单/双引号成对转义、引号内逗号/分号/关键字、未闭合引号、同 stage 重复键拒绝
  与跨 stage 同名键；Scheduler E2E 证明直接单 SQL 和 `sql_text` 两个入口均把字面量解码值安全包装为算子收到的
  JSON string，重复键在建 task 前失败且旧 `mode` 参数不变。
- `test_framework`、`test_npm_basic`、`test_scheduler_e2e` 构建通过；定向 CTest 3/3 通过（37.96 秒）。
- `git diff --check`、新增文件空白检查和新增 C++ 120 列检查通过；环境无 `clang-format` 可执行文件，也无仓库
  format/lint target，已按 `src/.clang-format` 人工审查本轮 Diff。
- T1.1：任务打开配置先按顶层 `features`/`observing` 冻结消费者集合，再一次性解析 `parameters`；V1 framework
  分析配置和活动 Session 范围归一进现有拥有型 `NpmBasicTaskConfig`，未活动 Basic/Session 与未知模块不进入结果，
  原始 JSON 不保存到任务配置或热路径。外层 JSON string 的内嵌 NUL 也被明确拒绝，避免 C 字符串截断绕过严格解析。
- T1.2：V1 与 legacy 等价配置逐字段一致；12 个 legacy 调优键与 `parameters` 任意混用均返回
  `kParameterSourceConflict`，并通过 `NpmParameterStatusV1` 保留 `kLegacyConflict` 或 V1 类型/范围错误及 JSON Pointer
  子路径。`features`、`observing`、`input_namespace`、`source_domains` 仍可与 `parameters` 共存，所有失败保持旧输出。
- `test_npm_basic`、`test_scheduler_e2e` 构建通过；定向 CTest 2/2 通过（21.44 秒）；该检查点之后继续执行 T2/T3。
- T2：新增从独立 legacy/V1 WITH JSON 冻结配置开始的运行时等价锚点；真实 operator probe 在 Basic/Session 两种
  observing 下得到相同 Schema 和 pipeline 生命周期，离线 runtime 对相同 TCP 批次得到相同处理状态、预算、EOF
  Batch 与 `kFlushed` 终态，注入完整时间能力的模拟实时 runtime 对相同 UDP 会话得到相同周期快照、revision、预算
  与 Cancel 清理结果。原始 JSON 在运行前改写不影响快照；所有热路径只读取既有 `NpmBasicTaskConfig`，无需修改
  runtime/operator 生产代码或增加第二套参数通道。
- `cmake --build build --target test_npm_basic -j$(nproc)` 通过；`test_npm_basic` 定向 CTest 1/1 通过（0.40 秒）。
  T3 已在下述真实 SQL/E2E 与完整回归中完成。
- T3：复用动态加载的真实 `libflowsql_npm_basic.so`、真实 PCAP Source、Scheduler 和 DataFrame Sink，证明既有
  legacy Basic/Session SQL 无修改运行，等价 V1 SQL 的 Schema 与完整 Arrow RecordBatch 分别逐值相同；Basic-only
  V1 同时携带无效 Session 内部值、未启用 labeling 的非精确引用及未知 `http1` 节点仍成功，Session V1 携带未知
  future protocol 节点仍成功，锚定参数节点不隐式启用模块且未消费 object 被确定性忽略。
- 活动 V1 framework 类型错误、活动 Session 范围错误及 legacy/V1 来源冲突均在真实 Scheduler Schema probe 阶段
  失败，未注册目标 DataFrame、未进入 NPI 包处理；返回诊断分别包含稳定的 `invalid parameters` 或
  `configuration source conflict` 类别，以及 `/framework/max_active_sessions`、
  `/session/max_tcp_ranges_per_direction` 或 `/run_mode` JSON Pointer。算子只在 `Open()` 配置失败时构造一次任务私有
  诊断字符串，继续通过既有原子首错槽发布稳定指针，不改变 Cancel/runtime 并发路径。
- Feature 完成验收：`cmake -B build src` 通过；三个目标定向构建通过，定向 CTest 3/3 通过（38.97 秒）；完整
  `cmake --build build -j$(nproc)` 通过，完整 CTest 15/15 通过（52.99 秒）。`git diff --check`、未跟踪新增文件
  空白检查和新增/修改 C++ 120 列检查通过；环境无 `clang-format` 可执行文件，也无仓库 format/lint target，已按
  `src/.clang-format` 人工审查 T3 Diff。Feature 已完成并归档，未开始后续 Feature。
