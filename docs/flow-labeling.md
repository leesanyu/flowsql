# Flow Labeling 构建、部署与容量指引

Flow Labeling 是 Scheduler 进程内的通用会话流标签能力。它用固定 IID
`IID_FLOW_LABELING_PROVIDER_V1` 暴露 `libflowsql_flow_labeling.so`，当前由显式启用 `labeling` 的
`npm.basic` 任务消费。一个任务在打开时解析一个不可变 `FlowLabelingSet` 快照并创建私有 matcher；后续同一
双向会话复用一个稳定的 `primary_label_id`。未命中值是 0，标签 ID 0 不得出现在配置中。

本文区分四个容易混淆的量：插件是否被构建、Scheduler 是否装载插件、进程级 DPDK EAL 内存，以及每个任务的
matcher 预算。任何 benchmark 数字都必须连同机器、DPDK 版本、规则布局和命令保存，不能当作规则数公式。

## 1. 系统依赖与构建三态

Flow Labeling 的真实实现依赖系统 DPDK 开发包。Ubuntu/Debian 构建环境至少需要：

```bash
sudo apt-get update
sudo apt-get install -y pkg-config libdpdk-dev
pkg-config --modversion libdpdk
test -f /usr/include/dpdk/rte_acl.h
```

当前验收环境快照是 Ubuntu 24.04.4 LTS、x86_64、`libdpdk-dev 23.11.4-0ubuntu0.24.04.2`，
`pkg-config` 报告 DPDK 23.11.4。它只是本次核查环境；在其他发行版、架构或升级后的节点上，应重新检查
`libdpdk.pc`、编译器 CPU flags、插件 `DT_NEEDED` 和运行库解析结果。

`FLOWSQL_FLOW_LABELING` 是 CMake cache 变量，不是服务运行时环境变量：

| 值 | 配置结果 |
| --- | --- |
| `AUTO` | 默认值；找到 `libdpdk.pc` 时构建真实插件，否则跳过插件 |
| `ON` | 必须找到 PkgConfig 和 `libdpdk.pc`，缺依赖时 CMake 配置失败 |
| `OFF` | 明确不构建插件 |

需要 Flow Labeling 时使用确定性的 `ON` 构建：

```bash
cmake -B build src -DFLOWSQL_FLOW_LABELING=ON
cmake --build build --target flowsql_flow_labeling test_flow_labeling test_npm_basic -j$(nproc)
```

插件通过 `PkgConfig::DPDK` 私有链接 DPDK，公共接口和 `libflowsql_npm_basic.so` 保持 DPDK-free。构建后复核：

```bash
readelf -d build/output/libflowsql_flow_labeling.so
ldd build/output/libflowsql_flow_labeling.so
readelf -d build/output/libflowsql_npm_basic.so
```

当前构建中真实插件直接声明 `librte_acl.so.24` 和 `librte_eal.so.24`，二者均由系统解析；NPM Basic 不直接
依赖 `librte_*`。Dockerfile 当前声明安装 `librte-acl24` 和 `librte-eal24`，但这只是文件快照，本 Feature
没有重新构建镜像。发布镜像前仍需在目标镜像中运行 `ldd` 并核对架构、ABI 和 CPU 能力。

## 2. Scheduler 装载与 EAL option

构建开关只决定 `.so` 是否存在；部署清单决定 Scheduler 是否必须装载它。原生单进程、Guardian 的 Scheduler
和 Docker Scheduler 示例都把插件列在 `libflowsql_config_channel.so` 之后、Scheduler 主插件之前：

```yaml
- name: libflowsql_flow_labeling.so
  option: "eal_memory_mib=512"
```

插件条目只属于 Scheduler。一个已列出的 `.so` 若缺失、`dlopen` 失败、option 非法或 EAL 启动失败，整个插件
批次必须失败，不会静默变成“无标签模式”。若以 `AUTO`/`OFF` 构建而没有该产物，应从相应部署清单中移除该
条目；如果任务显式请求 labeling 而 provider 未部署或未就绪，该任务会在打开阶段明确失败。

插件 option 使用分号分隔，只允许以下两个键：

| Option | 范围与缺省 | 含义 |
| --- | --- | --- |
| `eal_memory_mib` | 512～4096，默认 512 | Scheduler 进程的一套 DPDK EAL 内存预留 |
| `eal_lcore_cpu` | 可选，必须属于进程当前 CPU affinity | 把唯一 EAL lcore 固定到指定 CPU |

例如：

```yaml
option: "eal_memory_mib=768;eal_lcore_cpu=3"
```

CPU 3 只有在 `taskset -pc <pid>` 或容器 cpuset 显示它属于当前亲和集合时才有效。未知键、重复键、空值、
非十进制、溢出、越界内存和 affinity 外 CPU 都在 EAL 初始化前被拒绝；不接受原始 EAL argv。

插件从已验证 option 固定生成：

```text
--lcores=0@<cpu> --main-lcore=0 -m <eal_memory_mib>
--no-huge --no-pci --no-telemetry --no-shconf
```

未指定 CPU 时选用插件 `Start()` 当时的当前 CPU。所有插件仍遵循整批 `Option → Load → Start` 生命周期；
Flow Labeling 在 `Start()` 初始化一次 EAL，任务只能在此之后创建 matcher。停止或卸载前必须先释放所有任务的
matcher。

## 3. 发布 FlowLabelingSet

从 [严格配置模板](../config/flow-labeling-template.yaml) 复制并替换示例值。快照的关键元数据是：

```yaml
api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
```

配置是严格 UTF-8 YAML：未知字段、重复 mapping key、alias、自定义 tag，以及重复的 label ID、name 或
priority 都会在 matcher 发布前失败。标签 ID 0 保留给未命中；不同标签的 priority 必须唯一，值越大优先级
越高。所有规则只能是 `bidirectional`，每条非对称逻辑规则会展开成端点互换的两个物理规则，相同物理规则会
去重。规则内多个 match 是 AND；需要 OR 时写多条逻辑规则；全局规则必须显式写 `match_all: true`。

V1 只使用 NPM 唯一 Layer decode 产生的早期类型化事实：

- observation domain；
- 源/目的 MAC；
- 外层与内层两个 VLAN 的 TPID/VID；
- IPv4/IPv6 family 与源/目的地址；
- 传输层协议与源/目的端口。

模板列出了各字段支持的 `RANGE`、`MASK/prefix_bits` 和 `BITMASK` 形式。未出现的字段是 wildcard；带有效位的
MAC、IP、VLAN、协议和端口会自动增加 presence 条件，因此“事实缺失”不会误匹配数值 0。

V1 不消费原始 payload、DNS qname、HTTP Host、TLS SNI、NPI protocol 结果、TCP flags、TCP 重组内容或其他
解析后应用字段。Protocol 是 NPI 识别维度，Label 是网络自定义归属维度；二者在同一会话结果中并列存在，互不
改写。插件本身不解包、不建立会话、不访问 HTTP/SQLite，也不在逐包路径 Resolve 配置。

可在 Web 配置资源页面发布模板；也可通过统一 API 发布。以下示例假设当前 revision 为 0：

```bash
FLOW_LABEL_B64=$(base64 -w0 config/flow-labeling-template.yaml)
curl -sS http://127.0.0.1:8081/api/channels/config/publish \
  -H 'Content-Type: application/json' \
  --data-binary "{\"name\":\"corp-labels\",\"expected_current_revision\":0,\"format\":\"yaml\",\"schema_id\":\"flowsql.io/flow-labeling/v1alpha1\",\"content_base64\":\"${FLOW_LABEL_B64}\",\"original_filename\":\"flow-labeling-template.yaml\",\"change_note\":\"initial labels\"}"
```

响应包含类似 `"exact_reference":"config.corp-labels@1"`。后续更新必须带实际
`expected_current_revision`；发布产生新的不可变 revision，已打开任务继续使用原快照，不会自动漂移到 current。

## 4. 在 npm.basic 中启用

只有 `features` 显式包含 `labeling` 才会查询 provider、解析字段并 Resolve 配置。下面使用连续范围内的非历史档位
96 MiB，默认 `max_tracked_bytes=256 MiB` 足以满足两倍准入：

```sql
SELECT *
FROM pcapfile.capture
USING npm.basic
WITH input_namespace='pcapfile.capture',
     source_domains='0:77',
     features='basic,labeling',
     parameters='{"schema_version":1,"core":{"labeling":"config.corp-labels@1","labeling_memory_mib":96}}'
INTO dataframe.labeled_sessions
```

Basic 与 Session 结果在启用 labeling 时包含非空 `primary_label_id`。0 表示已分类但没有规则命中，不表示
“尚未分类”。主标签绑定在双向基础会话上，后续 packet 不重复调用 matcher；TCP tuple reuse 创建新会话实例并
重新分类。规则发布新 revision 也不会改变已经打开任务中的标签目录或 matcher。

`labeling_memory_mib` 默认 64，可接受 8～256 MiB 闭区间内任意整数。若选择上限 256 MiB，还必须显式把
`max_tracked_bytes` 提高到至少 512 MiB：

```json
{
  "schema_version": 1,
  "core": {
    "labeling": "config.corp-labels@1",
    "labeling_memory_mib": 256,
    "max_tracked_bytes": 536870912
  }
}
```

labeling 未启用时，`core.labeling` 和 `labeling_memory_mib` 按 optional-module 规则被忽略，不解析、不
Resolve，也不查询 provider。显式启用但引用不是 `config.<name>@<revision>`、快照不存在、provider 缺失或规则
编译失败时，任务打开失败，不会用旧规则或无标签模式继续。

## 5. 三种内存与 RSS

### 5.1 所有权

```text
一个 Scheduler 进程
└── 一个 Flow Labeling 插件实例 / 一套进程级 EAL
    ├── npm.basic 任务 A → 私有 matcher A → 任务 A 的 labeling_memory_mib
    └── npm.basic 任务 B → 私有 matcher B → 任务 B 的 labeling_memory_mib
```

多个任务共享进程 EAL，但不会共享 matcher、ACL context、标签目录或任务账面额度。任务关闭时释放自己的
matcher；进程停止时才清理 EAL。

### 5.2 四个量不能互换

| 量 | 所有者 | 能保证什么 | 不能保证什么 |
| --- | --- | --- | --- |
| plugin `eal_memory_mib` | Scheduler 进程 | DPDK EAL 的进程级内存预留 | 不是单任务额度或 RSS 上限 |
| `core.labeling_memory_mib` | 一个 labeling 任务 | matcher 的任务级 `kModuleState` 准入，默认 64 MiB | 不是独立物理子池 |
| `spec.engine.max_runtime_bytes` | 一个规则集的单 ACL context | 传给 `rte_acl_config.max_size` 的 runtime 上限 | 不是完整 matcher 或构建期峰值 |
| RSS/peak RSS | 整个进程 | 操作系统观测到的实际驻留/峰值 | 不等于上述配置量之和 |

每个任务在 matcher 发布前满足近似的稳态准入关系：

```text
max_runtime_bytes + retained physical rules + label directory + matcher fixed overhead
    <= labeling_memory_mib
```

此外，NPM 要求：

```text
max_tracked_bytes >= 2 * labeling_memory_mib * MiB
```

模板默认 `max_runtime_bytes=33554432`（32 MiB）。若任务只给 8 MiB，无论规则规模多小，runtime 上限本身已经
超过任务预算，会在构建前以 `kBudgetExceeded` 和 `/spec/engine/max_runtime_bytes` 失败；必须先降低 runtime
上限，并为物理规则、标签目录和固定开销留出余量。反过来，提高任务额度也不保证 EAL 有足够空间，尤其多个
任务并发编译时仍可能遇到 DPDK 分配失败，需要结合负载调整进程级 EAL 并监控实际内存。

不能使用下面的等式估算进程容量：

```text
process RSS != eal_memory_mib + sum(labeling_memory_mib)
```

`labeling_memory_mib` 是准入记账而非物理子池；RSS 还包含 EAL 实际驻留页、普通堆、ACL 构建临时对象、
Arrow/NPM 状态和共享库。归档测试中，一个 128 MiB 任务预算曾出现约 1 GiB 的进程 peak RSS，这正说明任务预算
不能被解释为 RSS ceiling。

## 6. Benchmark 复测与解释

构建并运行默认隔离矩阵：

```bash
cmake -B build src -DFLOWSQL_FLOW_LABELING=ON
cmake --build build --target benchmark_flow_labeling -j$(nproc)
build/output/benchmark_flow_labeling build/output/libflowsql_flow_labeling.so
```

默认矩阵在独立子进程中运行 `1K/64 MiB`、`10K/64 MiB`、`50K/64 MiB` 和 `50K/128 MiB`，
`max_runtime_bytes` 均为 40 MiB；预期第三项返回结构化 `budget_exceeded`，其余三项 build/classify 成功。
单例模式适合复核其他预算：

```bash
build/output/benchmark_flow_labeling build/output/libflowsql_flow_labeling.so \
  10000 96 40 success
build/output/benchmark_flow_labeling build/output/libflowsql_flow_labeling.so \
  50000 64 40 budget_exceeded
```

参数依次为逻辑规则数、任务预留 MiB、ACL runtime MiB 和预期结果。benchmark 给每条规则唯一
`observation_domain`，再加非对称 `source_port=443`，所以一条双向逻辑规则确实展开为两个不同物理规则；它还
验证正向、反向、未命中和 256 条 batch 分类。

程序输出固定 CSV：

```text
profile,logical_rules,physical_rules,config_bytes,reserved_bytes,max_runtime_bytes,result,error,path,build_ms,rss_before_config_kib,rss_before_build_kib,rss_after_build_kib,rss_build_delta_kib,rss_after_release_kib,peak_rss_kib,classifications,classify_ms,mpps
```

`build_ms` 只包围同步 `CreateMatcher`；RSS 来自 `/proc/self/status` 与 `getrusage(RUSAGE_SELF)`；分类吞吐经过
warm-up，计时至少约 250 ms，不包含配置生成、插件加载或 matcher 构建。默认矩阵以 fork 隔离 case。它不是
完整 Scheduler、Config Channel、PCAP I/O、NPM session 或 DataFrame 落盘 benchmark。

原始 Flow Labeling Feature 在当时同一代码布局、DPDK 23.11.4 和测试机器上归档了以下观察：

| 逻辑/物理规则 | 任务预算 / runtime | 结果 | build ms | RSS build delta KiB | peak RSS KiB | Mpps |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| 1K / 2K | 64 / 40 MiB | success | 294.766 | 41,244 | 62,880 | 16.152 |
| 10K / 20K | 64 / 40 MiB | success | 2,863.214 | 153,284 | 244,188 | 15.755 |
| 50K / 100K | 64 / 40 MiB | budget exceeded | — | — | — | — |
| 50K / 100K | 128 / 40 MiB | success | 14,619.722 | 650,796 | 1,061,768 | 15.256 |

归档还记录了 8→1K（4 MiB runtime）、16→5K（8 MiB）、32→10K（16 MiB）、64→20K（32 MiB）、
128→50K（40 MiB）、256→50K（128 MiB）这些成功代表点。它们不是合法档位：当前任务参数接受 8～256
任意整数，也不是“规则数对应内存”的公式。规则字段组合、标签字符串、DPDK 算法、CPU、构建选项、并发任务和
库版本都会改变结果。比较复测时至少保存以下信息：

- Git revision 与工作区差异；
- OS、CPU/flags、编译器、构建类型和 DPDK 包版本；
- 完整命令、CSV 原文、规则 profile 和并发条件；
- EAL option、任务预算、runtime 上限以及是否有其他任务共享进程。

## 7. 故障定位顺序

| 阶段 | 典型现象 | 首要检查 |
| --- | --- | --- |
| CMake | `ON requires ... libdpdk.pc` | `pkg-config --modversion libdpdk`、开发包和 `rte_acl.h` |
| 装载 | 找不到或无法 `dlopen` `.so` | 构建三态、部署文件、`readelf`/`ldd`、架构与 ABI |
| Option | 插件批次在 Start 前失败 | 重复/未知字段、512～4096、CPU affinity |
| EAL Start | Scheduler 启动失败 | CPU 绑定、EAL 内存、系统运行库；不得改为静默跳过 |
| 任务打开 | provider/config registry 不可用 | 插件是否同在 Scheduler、Config Channel 是否装载 |
| Resolve | 精确引用失败 | 必须是存在的 `config.<name>@<revision>`，不能写 `@latest` |
| Schema | `kInvalidConfig`/`kLimitExceeded` | 严格字段、唯一 ID/name/priority、模板 limits 和双向展开 |
| 预算 | `/spec/engine/max_runtime_bytes` | runtime + retained state 是否超任务额度；tracked 是否至少两倍 |
| DPDK build | `kDpdkFailure` | EAL 可用空间、算法/CPU 支持、并发构建和实际 RSS |

每次部署或容量调整都应先通过 `test_flow_labeling`、`test_npm_basic` 和部署配置测试，再在目标机器运行
benchmark；不要用 `AUTO` 跳过插件的成功配置代替真实功能验收。
