# Feature: NPM Basic 与 Flow Labeling 可运维性及代码结构收敛

状态：`[x]` 已完成；优先级：P1
前置 Feature：`npm-basic-parameters`、`flow-labeling`（均已完成）；本规格不改变其已归档验收事实。

## Non-Goals

- 不改变 `npm.basic` 的 SQL 参数/输出、会话主标签语义、DPDK-free 公共 IID/ABI、ACL 规则 Schema、算法或分类热路径。
- 不把 CMake 三态替换为插件 `Option()`：插件在产物存在且成功 `dlopen` 后才能收到 option；不为已装载插件新增
  `AUTO|ON|OFF` 运行时模式，也不允许装载或 EAL 失败后悄悄跳过插件。
- 不让任务级 `parameters.core`、Config Channel 规则文件或首个 `npm.basic` 任务决定进程级 EAL 参数；
  不按规则数量推导 EAL/matcher 内存，不开放任意 EAL argv、网卡、巨页或 PCI 配置。
- 不删除有实测需求的 `labeling_memory_mib` 参数，不改变 `max_tracked_bytes` 的两倍准入与默认值；
  不声称单任务预留能够限制进程级 RSS 或多任务合计，也不建立节点级总预算调度器。
- 不引入新模块框架、动态模块注册、头文件转发层或与目录迁移无关的业务逻辑重写；不重跑性能测试后
  把不同机器/配置的数字拼接为硬件无关吞吐承诺。

## 业务意图

运维人员需要明确可选标签插件何时被构建、何时在 Scheduler 装载、DPDK 进程资源如何调节，避免一个
`FLOWSQL_FLOW_LABELING` 名字被误解为运行时环境变量，也避免写死的 512 MiB EAL 堆无法按部署规模调节。
后续维护 `npm.basic` 的开发者需要按职责快速定位配置、共享核心、Basic/Session 模块和输出代码；
使用者需要可复核的安装、配置、内存与 benchmark 指引。本 Feature 交付部署级最小参数入口、保留且解释
任务级连续有界预算、纯目录归位和一份从 README 可达的 Flow Labeling 使用文档，保持既有任务结果不变。

## 已核查事实与取舍

- `src/CMakeLists.txt` 将 `FLOWSQL_FLOW_LABELING=AUTO|ON|OFF` 定义为 **CMake cache 变量**；`AUTO` 缺
  `libdpdk.pc` 跳过构建，`ON` 缺依赖配置失败，`OFF` 不构建。它不是进程启动环境变量；真实验收仍须 `ON`。
- 原生 `config/deploy-single.yaml` / `config/deploy-multi.yaml` 与 `docker-compose.yml` 的 Scheduler 已列出
  `libflowsql_flow_labeling.so`、没有 option；插件 `Option()` 只接受空值，`Start()` 固定用当前 CPU、`-m 512`
  和无巨页/无 PCI/无 telemetry/无共享配置的 EAL 参数。加载器在 `dlopen` 失败时终止整个批次。
- `FlowLabelingPlugin::Start()` 在批次装载后初始化唯一的进程级 EAL；`npm.basic` 的 `parameters.core`
  在任务 `Open()` 才解析，精确快照也在此时 Resolve，多任务各有 matcher。因此 EAL 参数由部署者通过
  `flow-labeling` 自己的 plugin option 指定，任务只决定自己的 matcher 预算。
- `labeling_memory_mib` 默认 64，现有代码只接受 8/16/32/64/128/256 MiB，选值同时用于任务预算
  `kModuleState` 预留及 provider 编译请求；`max_tracked_bytes` 至少是选值两倍。字节预算、
  DPDK `max_size` 和任务准入均无 2 的幂或固定档位约束，因此六档白名单没有技术必要：
  保留参数与现有 8～256 MiB 有界范围，改为接受范围内任意整数，不把代表点包装为规则数上限。
- 当前 `src/operators/npm_basic/` 的约 20 组实现/头文件并列平铺，`CMakeLists.txt` 直接枚举源文件，
  `test_npm_basic.cpp` 显式引用多个内部路径；目录归位必须同步构建清单和这些引用。

## 核心数据契约

### 构建、装载与参数所有权

| 层级 | 配置入口与决定时机 | 结果 / 失败语义 |
| --- | --- | --- |
| 构建 | `cmake -B build src -DFLOWSQL_FLOW_LABELING=AUTO\|ON\|OFF` | 保留三态与真实 `ON` 验收；不由进程 option 代替 |
| 部署 | Scheduler 插件列表有/无 `libflowsql_flow_labeling.so` | 列出即必须可装载并启动，否则服务启动失败；不列出则普通任务正常，显式请求 labeling 的任务失败 |
| 进程 | `flow-labeling` 插件 `option`，批次 `Option()` 冻结 | 只设置 EAL 内存与可选 CPU，`Start()` 初始化一次；无效 option 拒绝整批，不由任何任务改写 |
| 任务 | `npm.basic` 的 `core.labeling` 和 `labeling_memory_mib` | 精确快照 + 私有 matcher；未启用不消费，启用且 provider 不可用即明确失败 |

`FlowLabelingPlugin` 私有拥有型配置（不进入 `iflow_labeling.h` 或 task JSON）：

```cpp
struct FlowLabelingStartupOptions {
    uint32_t eal_memory_mib = 512;               // 插件所属进程的 EAL 内存预留。
    std::optional<uint32_t> eal_lcore_cpu;        // 缺省选择 Start 时进程当前 CPU。
};
```

- 原生 YAML 示例：`name: libflowsql_flow_labeling.so` 与
  `option: "eal_memory_mib=512"`；Docker Scheduler 插件项沿用既有 `libflowsql_flow_labeling.so:<option>`
  字符串协议。空 option 兼容现有默认行为，两个原生配置及 Docker 均显式展示默认 512 MiB。
- `Option()` 仅接受分号分隔的 `eal_memory_mib=<整数>`、`eal_lcore_cpu=<整数>`，不接受未知/重复键、空值、
  非十进制或溢出；内存范围 512～4096 MiB，CPU 必须属于启动进程的有效亲和集合。配置解析不调用 EAL；
  内存范围是插件允许的运维配置边界，不是任意规则/并发数量都能构建成功的保证。
- `--lcores=0@<cpu>`、`--main-lcore=0` 和 `-m <eal_memory_mib>` 由已验证配置生成；固定
  `--no-huge --no-pci --no-telemetry --no-shconf`。不接受原样 argv 字符串，以免任务或部署绕开安全边界。
  EAL 初始化失败须暴露启动失败；Stop/Unload 仍先释放全部 matcher，保持原批次生命周期顺序。
- `AUTO/OFF` 无产物时，部署列表仍写有 `.so` 会在加载时失败；文档明确要求部署者在非 DPDK 部署中
  移除该项，或在需要此项时以 `ON` 构建并部署匹配的系统运行库，不新增静默忽略缺件机制。

### 三种内存与任务预算的通俗语义

| 量 | 对谁生效 | 用途与不能保证的事 |
| --- | --- | --- |
| plugin `eal_memory_mib`（默认 512） | 一个 Scheduler 进程的 DPDK EAL | 提供进程共享内存池；不等于单任务额度，也不等于 RSS |
| `core.labeling_memory_mib`（默认 64） | 每个启用 labeling 的 `npm.basic` 任务 | 为 matcher 留出的任务预算，8～256 MiB 整数；超预算在任务打开阶段诊断失败 |
| FlowLabelingSet `spec.engine.max_runtime_bytes` | 该规则集的单个 ACL context | 限制 DPDK 分类结构大小；不是构建期峰值 RSS，也不代替上述两个额度 |

任务默认 64 MiB、`max_tracked_bytes` 默认 256 MiB；选择 256 MiB 上限时至少显式设置
`max_tracked_bytes=536870912`。可把该整数理解为“一个任务愿意留给规则匹配器多少空间”，而非
“多少条规则对应多少内存”；12、48、96、160 MiB 等中间值与旧六个值使用同一字节预算路径。
已归档的 8→1K、16→5K、32→10K、64→20K、128→50K、256→50K 只是特定非对称规则、
DPDK 23.11.4 与机器上的成功代表点。尤其 50K/64 失败、50K/128 成功，说明不能按规则条数盲算；
多个任务同时编译时还须由部署者监控进程内存与 EAL 空间，本 Feature 不承诺自动容量推导。

### `npm_basic` 目录目标

- 根目录保留 `CMakeLists.txt`、插件 ABI 出口、operator 入口及共享 `npm_analysis_contract.*`；
  `config/` 放 `npm_parameters.*` 和 `npm_basic_task_config.*`。
- `core/` 放任务 runtime/budget、packet batch/processor、session key/table、protocol context 与 EOF flusher；
  `modules/basic/` 放 Basic result projector；`modules/session/` 放 Session analysis module 与 TCP tracker；
  `output/` 放 result collector 及 Basic/Session result encoder。
- 只移动现有成对 `.h/.cpp` 并更新 CMake/内部 include/测试 include；不改命名空间、ABI 导出、算法、
  测试期望或模块归属。允许分次原子迁移，但每次以构建和 `test_npm_basic` 闭环，最终旧路径引用清零；
  在未来切片开始前按实际文件冻结允许列表，不为了迁移而拆分共享契约或新建转发层。

## 主链路

1. **部署与任务打开**：以 `ON` 生成真实插件 → Scheduler 根据部署列表装载并在 `Option()` 冻结进程参数
   → `Start()` 用受限参数初始化 EAL → 显式 labeling 的 `npm.basic` 任务解析 64 MiB 默认或范围内整数覆盖，
   Resolve 精确快照并按现有路径编译 matcher；未启用任务仍不消费 provider。
2. **维护与使用**：开发者按目录定位共享核心与 Basic/Session 模块，构建/测试语义不变；使用者从 README
   进入 `docs/flow-labeling.md`，按照系统依赖、部署 option、精确配置、预算与 benchmark 指引复现结果。

## Feature Tasks 与测试锚点

- `[x]` T0：交付可配置而受限的进程级 EAL 启动与部署示例，使运维人员能够调节资源并在无效配置或
  缺少真实插件时获得确定的启动失败，同时保持构建三态和无标签任务的隔离。
  - `[x]` T0.1：先冻结私有 startup struct 与 option 测试：空值/默认值、内存上下界、CPU 亲和、
    重复/未知/非法字段、`Option → Load → Start` 批次和失败不启动 EAL。
  - `[x]` T0.2：实现受限 EAL 参数映射及原生/Docker 配置接线，以真实 DPDK 证明默认和覆盖值、
    缺件/失败不可静默跳过，`AUTO/OFF/ON` 构建语义不变。
- `[x]` T1：删除 matcher 预算的六档白名单并交付 8～256 MiB 连续整数边界与回归锚点，
  使运维者能区分 EAL 池、任务额度、ACL runtime 限制和构建 RSS，非档位数值可用且既有 SQL 默认不变。
- `[x]` T2：交付 `npm_basic` 按配置、共享核心、Basic/Session 模块和输出职责的目录归位，
  使源码定位清晰而 ABI、SQL、Schema、生命周期和既有结果不变。
- `[x]` T3：交付 `docs/flow-labeling.md` 及 README 链接，使安装/配置/预算/benchmark 能由同一份文档
  复现，并完成真实插件、NPM 及全量回归验收。文档至少覆盖以下事实与操作：
  - 构建侧 `pkg-config`/`libdpdk-dev`/`rte_acl.h` 的检查及 `ON` 命令；当前已核查环境是
    Ubuntu 24.04.4 LTS / x86_64，`libdpdk-dev 23.11.4-0ubuntu0.24.04.2`，`pkg-config` 报告
    DPDK 23.11.4。`Dockerfile` 声明安装 `librte-acl24`/`librte-eal24`，但镜像未在本轮构建验证；
    文档应注明这些是本机/文件快照，部署时重查 ABI、CPU flags、插件 `DT_NEEDED` 和运行库解析。
  - 构建/部署/任务配置示例、`config/flow-labeling-template.yaml` 的具名标签、优先级、双向展开、
    MAC/VLAN/IP/协议/端口匹配与明确不支持的解析后字段；三种内存量、旧六个实测参考点与连续整数范围。
  - 用层级图明确运行时所有权：一个 Scheduler 进程装载一个 Flow Labeling 插件实例并初始化一套进程级 EAL；
    多个启用 labeling 的 `npm.basic` 执行任务共享该 EAL，但每个任务分别拥有、释放任务私有 matcher/ACL context，
    `labeling_memory_mib` 属于任务而不是插件实例，任务之间不共享或借用该账面额度。
    ```text
    一个 Scheduler 进程
    └── 一个 Flow Labeling 插件实例 / 一套进程级 EAL
        ├── npm.basic 任务 A → 私有 matcher A → 任务 A 的 labeling_memory_mib
        └── npm.basic 任务 B → 私有 matcher B → 任务 B 的 labeling_memory_mib
    ```
  - 给出每任务准入关系；说明 `max_runtime_bytes` 只是任务 matcher 总预算的一部分：
    ```text
    max_runtime_bytes + retained physical rules + label directory + matcher fixed overhead
        <= labeling_memory_mib
    ```
    例如模板仍配置
    32 MiB runtime 时，8 MiB 任务预算必然在构建前失败，必须同时降低 runtime 上限并为其余状态留空间。
  - 给出进程关系及反例：并发 matcher 的实际 DPDK 分配与构建工作区共享 `eal_memory_mib`，但
    `process RSS != eal_memory_mib + sum(labeling_memory_mib)`。任务预留是准入记账而非物理子池，RSS 还包含
    EAL 实际驻留页、普通堆、规则编译临时对象、Arrow/NPM 状态、共享库等；128 MiB 任务预算曾观测到约
    1 GiB 构建峰值 RSS，须标明它是特定机器/规则/DPDK 版本的反例而非新容量承诺。
  - `benchmark_flow_labeling` 的构建/运行命令、1K/10K/50K 输入、物理展开、CSV 字段、
    构建时间/RSS/峰值 RSS/吞吐的测量边界和归档结果来源；重复测量要记录机器与 DPDK 版本。

## 验收矩阵

| 验收面 | 可执行断言 / 检查 |
| --- | --- |
| 构建/装载 | `cmake -B build src -DFLOWSQL_FLOW_LABELING=ON` + 真实插件测试；OFF/AUTO 缺依赖行为不变；缺 `.so` 的显式部署启动失败 |
| 启动参数 | `test_flow_labeling` / 部署配置测试验证 Option 默认/覆盖/非法输入、CPU 亲和与固定禁用项；无效参数在 EAL 前失败 |
| 预算隔离 | `test_npm_basic` 验证默认值、8/256 边界、12/96/127 等中间值、7/257 拒绝、两倍准入、未启用忽略与下一任务恢复默认 |
| 目录/兼容 | `rg` 检查旧路径，编译 `flowsql_npm_basic`、`test_npm_basic` 和 E2E；动态加载 IID/ABI、SQL Schema 和结果保持不变 |
| 文档 | README 链接可达；安装、配置与 benchmark 可核查；层级图、每任务不等式、8 MiB/32 MiB 失败例和 RSS 非等式齐全，且不把任务额度写成插件实例或 RSS 上限 |
| 性能证据 | 保留 `benchmark_flow_labeling` 的 1K/10K/50K 代表点、CSV 列/运行命令；注明来源、机器/DPDK、RSS 与吞吐非硬保证 |

Feature 完成时运行 `cmake -B build src -DFLOWSQL_FLOW_LABELING=ON`、相关 target 构建与定向 CTest，
再运行 `cmake --build build -j$(nproc)`、`ctest --test-dir build --output-on-failure`、改动 C++ 的
`clang-format --dry-run --Werror`、仓库适用 lint 和 `git diff --check`。验收的真实 DPDK 依赖用
`pkg-config --modversion libdpdk`、`readelf -d`/`ldd` 复核，不把 `AUTO` 跳过当作完整成功。

## 完成出口

1. CMake 构建、部署装载、进程 EAL 和任务 matcher 的边界各有唯一配置入口及确定失败语义。
2. 六档人工限制已删除，8～256 MiB 任意整数均走同一预算契约；历史画像不被误称为容量公式。
3. `npm_basic` 目录可按功能查找，行为、公共 ABI、SQL、结果和全量回归保持不变。
4. Flow Labeling 文档可从 README 访问，并给出当前环境核查方式与可复核的 benchmark 方法。

## 完成证据

- T0.1 已交付私有 `FlowLabelingStartupOptions` 和严格原子 option 解析：空值恢复 512 MiB 默认且 CPU 未指定；
  只接受 `eal_memory_mib` 512～4096 MiB 与当前进程亲和集合内的 `eal_lcore_cpu`，拒绝未知、重复、空字段、
  非十进制、溢出和越界输入，失败不覆盖旧值，完成 `Load()` 后也不能重配。
- 真实插件测试先得到 parser 未实现的链接失败，再以 `PluginLoader` 证明非法 option 使整批注册失败、接口不发布，
  合法 option 才进入 `Load → Start`；T0.1 完成时尚未接入 EAL argv。系统 DPDK 23.11.4 下当时的
  `test_flow_labeling` 构建与定向 CTest 通过，格式化与 Diff 检查通过。
- T0.2 已从拥有型配置生成固定安全 EAL argv：缺省 CPU 取 Start 当前 CPU，显式 CPU 覆盖；内存映射到 `-m`，
  `--lcores=0@<cpu>`、`--main-lcore=0` 以及 `--no-huge/--no-pci/--no-telemetry/--no-shconf` 保持固定。
  原生 single/multi YAML 与 Docker Scheduler 均显式传入 `eal_memory_mib=512`，未给其他进程加载插件。
- 隔离子进程以真实 DPDK 分别启动空 option 的 512 MiB 默认和 768 MiB/显式 CPU 覆盖；两个独立插件副本触发
  第二次进程 EAL 初始化失败，`StartAll()` 回滚首个插件且 provider 保持 unavailable。缺失 `.so` 和非法 option
  都在启动前失败。真实 `ON` 构建与 Flow Labeling/原生部署/Docker 部署 CTest 3/3 通过；`test_npm_basic` 1/1
  通过，保留普通任务隔离。独立目录验证 `OFF` 与缺 PkgConfig 的 `AUTO` 不生成插件且原生部署测试通过，缺依赖
  的 `ON` 配置失败；仅真实插件直接依赖可解析的 `librte_acl.so.24`/`librte_eal.so.24`，NPM Basic 无 RTE 依赖。
  T0/T0.2 已完成；未运行完整 CTest，未 commit/push。
- T1 将 `labeling_memory_mib` 合法性从六档白名单收敛为 `kNpmMinLabelingMemoryMiB=8` 至
  `kNpmMaxLabelingMemoryMiB=256` 的连续整数闭区间，保留 `kNpmDefaultLabelingMemoryMiB=64` 和
  `max_tracked_bytes >= 2 * labeling_memory_mib * MiB`。`test_npm_basic` 锚定 8/256 边界、12/24/96/127
  中间值、7/257 拒绝（257 使用恰好 514 MiB tracked 额度以隔离上界）、类型错误、未启用时忽略字段、
  任务配置覆盖后恢复默认，以及 provider 收到 127 MiB 预留；生产代码未修改 EAL、matcher 或预算记账路径。
  重新配置 `FLOWSQL_FLOW_LABELING=ON`、构建 `flowsql_npm_basic test_npm_basic`、定向 CTest 1/1 和
  `clang-format-18 --dry-run --Werror` 均通过，`git diff --check` 通过。T2/T3 未开始；未 commit/push。
- T2 将 16 对既有实现/头文件归入 `config/`、`core/`、`modules/basic/`、`modules/session/` 和 `output/`；
  根目录只保留 CMake、插件 ABI 出口、operator 入口和 `npm_analysis_contract.*`。生产/测试 CMake 与跨目录
  include 已同步，旧平铺路径和引用清零，未增加转发层；`.gitignore` 仅豁免必须交付的
  `src/operators/npm_basic/output/` 源码目录，其他 `output/` 构建产物仍被忽略。
  移动文件统一通过 `clang-format-18`，格式化前后的规范化源码比较除 T1 预算改动与 include 路径外无语义差异。
  `FLOWSQL_FLOW_LABELING=ON` 配置、`flowsql_npm_basic`/`test_npm_basic`/`test_scheduler_e2e` 构建、定向 CTest 2/2、
  V2 动态插件 ABI/任务结果断言、格式与 Diff 检查均通过。T0–T2 已完成，T3 未开始；未运行完整 CTest，
  未 commit/push。
- T3 已交付 README 可达的 `docs/flow-labeling.md`，覆盖系统 DPDK 依赖、CMake `AUTO|ON|OFF` 三态、
  Scheduler 装载与受限 EAL option、Config Channel 精确快照、L2/L3/L4 规则、`npm.basic` 启用方法、三层
  内存所有权及 benchmark 复测边界；明确任务预算不是物理子池或 RSS 上限，8 MiB 任务预算无法容纳模板的
  32 MiB runtime 上限，历史与本轮数据都不作为跨机器容量公式。
- 当前 Ubuntu 24.04.4 LTS / x86_64 环境使用 `libdpdk-dev 23.11.4-0ubuntu0.24.04.2`，`pkg-config`
  报告 DPDK 23.11.4；真实插件直接依赖且可解析 `librte_acl.so.24` 与 `librte_eal.so.24`，
  `libflowsql_npm_basic.so` 没有直接 RTE 动态依赖。Dockerfile 依赖仅作为文件快照记录，本轮未构建镜像。
- `FLOWSQL_FLOW_LABELING=ON` 配置成功；六个 Feature 相关 target 全部构建成功，定向 CTest 5/5 通过。
  默认 benchmark 4/4 符合预期：1K/64 MiB、10K/64 MiB、50K/128 MiB 成功，50K/64 MiB 在
  `/spec/engine/max_runtime_bytes` 返回结构化 `budget_exceeded`；本轮三个成功点吞吐分别为 14.096、
  15.233、15.308 Mpps，属于当前机器观测而非保证。
- 全量构建成功；完整 CTest 16/16、0 失败，总耗时 57.84 秒。Feature 改动 C++ 的
  `clang-format-18 --dry-run --Werror`、跟踪及新增文档的空白检查均通过；全量构建仅出现未修改 Arrow 调用点
  忽略 `[[nodiscard]]` 返回值的既有 warning，未扩大 T3 范围处理。Feature 与 Backlog 已完成并归档；
  未 commit/push。
- 2026-09-22 后续契约修正将文中的任务共享配置入口从 `parameters.framework` 同步为
  `parameters.core`；Flow Labeling 字段、预算和运行语义未改变，本次验证见即时工作台。
