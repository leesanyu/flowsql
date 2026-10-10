# Baseliner 验收记录

## 单表与 DataFrame Feature：T6 本次验收

本次针对 `baseliner-single-source`，验收日期为 2026-10-10。原 `baseline-operator` 的历史验收保留在后文。
完整基线及本轮最终检查各有原始证据，不把多个测试运行拼成一次完整 CTest。

### 构建、回归与生产范围

T6 前一切片执行标准入口配置和全量构建，单次完整 CTest **78/78** 通过，**882.49s**；失败、错误、跳过数均为 0。
本轮只增加成本测试和文档；生产实现、公共接口及其他测试与该完整基线保持文件 hash 一致。
更新后的成本目标重新构建并五轮重复全绿；最终验收来源分别记录，不将旧四路径成本结果充当新夹具结果。

生产测试实测 56.04s，保留 43 组原用例及 43 次插件重载恢复。新增/保留检查包括：

- 两段数据库、三段指定表、真实 CSV 导入的 DataFrame 三种输入 × SQLite/MySQL/PostgreSQL/DataFrame 四类结果和模型目标，共 12 组。
- Value/Ratio/Relation、evaluation/forecast、最终模型参数；逐个数值、NULL、枚举及 Schema 对照，源内容与 Schema 保持。
- 两段来源单/多 dataset、完整 parameters 与精确 config、四数据库来源、snapshot/poll 异步及旧 generation/checkpoint。
- 三后端同实际数据库双表事务回滚、别名通道、逻辑键幂等和保留用户其他记录；混合出口失败如实报告状态及已提交行数。
- 空 typed 输出、来源覆盖、目标重合、预算、取消、失败与旧托管 Relation fusion/maintenance 路径。

独立旧恢复 30 组实测 716.70s，维护 3.77s、生命周期 16.19s，均在本次完整 CTest 中通过。
DataFrame 数值验收读取 owning channel 的 Arrow 快照；此前隔离的通用 DataFrame 条件查询和 CSV NULL/小数展示现象仍单独记录，
没有将展示输出当作 Arrow 值或 Schema 的验收依据。

### 当前源码的 Sanitizer 与编译检查

本轮重新构建 94 个翻译单元，包含完整 Baseline、Baseliner 核心及输入 reader、模型/结果 store/provider、数据库、
框架 common 和测试使用的 pipeline/ChannelAdapter。六项 ASan/UBSan/LSan 全绿：DataFrame、evaluation/model export、
operator、三真实目标后端 store、lifecycle、query ownership；查询项只运行生产二进制的拥有型查询专用入口。
`halt_on_error=1`、`detect_leaks=1`，在沙箱外运行。其他动态框架插件、Arrow/数据库客户端依赖仍用普通库，
该结果只覆盖明确仪器化的代码范围。

98 个 C++ 文件整文件格式检查通过；通用 dataframe.cpp 的本 Feature 三处修改区间单独检查通过。
该文件其余旧代码的整文件格式诊断保存在 format.log，没有顺手重排。54 个翻译单元和 11 个公共头独立严格 C++17 编译通过，
沿用五个旧翻译单元已核实的 dangling-reference/missing-field-initializers/type-limits 例外，结果和例外逐项留证。

### 五轮相同负载成本

两组夹具均以独立子进程测量峰值 RSS，未固定 CPU 亲和性，使用仓库普通构建（当前成本目标无优化开关）。
机器为 Intel i9-13900H、20 个逻辑 CPU、WSL2/Linux x86_64。下表是五轮中位数，方括号为最小值和最大值。

原观测组保留四条路径：40 桶 × 8 身份 × 三类任务 = 960 条标准观测，24 个模型身份、Relation 三组/一个指标、
forecast horizon=2、每次 Arrow 24 条观测、checkpoint_every_buckets=7。各路径每轮 5760 行，数值汇总一致，结束 task_count=0。

| 路径 | 总耗时 ms | CPU ms | 峰值 RSS MiB |
| --- | ---: | ---: | ---: |
| 直接计算引擎 | 103.59 [101.17, 107.92] | 99.27 [97.15, 101.50] | 30.07 [29.84, 30.14] |
| Arrow 算子 | 298.87 [292.84, 312.71] | 294.79 [286.00, 307.39] | 32.52 [32.45, 32.79] |
| SQLite 托管 | 760.15 [752.30, 785.87] | 623.72 [619.84, 651.52] | 45.87 [45.61, 45.94] |
| SQLite 托管，每提交 +2 ms | 769.09 [745.28, 829.85] | 621.78 [610.35, 680.78] | 45.91 [45.76, 46.05] |

新原始数据组为同一份 960 行：40 桶 × 8 身份 × 3 个组，分页 256 行，聚合成同样的 960 条观测。
宽类型为 uint64/int64/float64，窄类型为 uint32/int32/float32；所有浮点夹具值都可精确表示，逆序改变物理顺序，源快照保持。
五条路径各轮结果均为 5760 行且数值汇总一致；选择模型出口的两条路径各输出 24 行，as_of_bucket 全部为 39。
使用真实 DataFrameReader → BucketAggregator → EvaluationEngine → results 编码/目标以及只读模型导出/编码/目标，
数据库路径使用新 ModelOutputStore 的同库双表一次事务。该组没有测量 Scheduler HTTP、CSV 上传解析或源数据库扫描。
新组只编码/发布 results 与可选 model_parameters；旧 Arrow/托管组还涉及 fusion、维护和 checkpoint。
两组总耗时的测量范围不同，应分别比较各组内部路径。

| 路径 | 总耗时 ms | 输入 Open ms | 模型导出与编码 ms | 发布 ms | 峰值 RSS MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| 有序宽类型 → DataFrame 结果 | 162.72 [159.97, 169.15] | 3.40 [2.44, 3.64] | 0.00 [0.00, 0.00] | 3.59 [3.49, 3.76] | 45.88 [45.79, 46.06] |
| 逆序宽类型 → DataFrame 结果 | 158.67 [155.63, 168.62] | 1.91 [1.87, 2.02] | 0.00 [0.00, 0.00] | 3.83 [3.49, 4.25] | 45.97 [45.77, 46.05] |
| 逆序窄类型 → DataFrame 结果 | 159.82 [157.19, 162.93] | 2.34 [2.26, 2.47] | 0.00 [0.00, 0.00] | 3.76 [3.54, 4.36] | 46.82 [46.65, 47.02] |
| 逆序窄类型 → DataFrame 结果及模型 | 162.77 [159.54, 166.18] | 2.35 [2.22, 2.39] | 3.89 [3.79, 4.03] | 3.40 [3.24, 3.68] | 47.14 [46.96, 47.34] |
| 逆序窄类型 → SQLite 结果及模型 | 366.96 [364.57, 384.94] | 3.14 [2.26, 3.25] | 3.91 [3.76, 4.29] | 207.85 [200.00, 216.16] | 40.95 [40.72, 40.98] |

input_open_ms 是 DataFrameReader::Open 整体计时，包含内容/Schema 指纹、范围处理、类型转换和稳定排序；
没有给内部阶段加入计时探针，不能将不同场景的差值解释为独立排序或转换耗时。
model_export_encode_ms 包含实际最终参数导出与 Arrow 编码；publication_ms 是结果和可选模型写入目标的总时间，
数据库模式包含同库事务提交，DataFrame 模式包含 Append/Write，均不包含 Catalog 登记。
目标建表/校验与 Join 单列为 target_setup_ms，不计入表中总耗时；SQLite 双表中位数 31.28ms [30.09, 31.75]。
总耗时包含输入 Open/Next、聚合、评估/预测、结果编码、有限结束及可选模型导出与发布，排除夹具生成和数值检查。
进程峰值 RSS 包含夹具、结果/参数暂存、数据库/Arrow 依赖与检查的整个子进程；reader_peak_buffer_bytes 是另外记录的受预算 Arrow 缓冲。
宽类型源缓冲为 53760 字节，窄类型为 26880 字节。当前/峰值 RSS 的 /proc 与 getrusage 口径不同。
这些小夹具用于核对路径成本和等价性，不能推导大表扫描、网络采集吞吐或生产规模 SLA。

### 环境、证据与复现

完整基线首次 NPM MySQL 连接失败：WSL 表 127 把 127.0.0.1 转发到 loopback0；容器健康，IPv6 三端口连通、IPv4 超时。
仅临时将该地址指向 lo 后，从第 1 项重跑完整 CTest，结束后原路由已恢复且核对一致，首次失败日志保留。
完整 CTest 沿用 setpriv --bounding-set=-net_admin 条件，真实数据库用例没有跳过；该条件不证明特权 DPDK/NetVSC 清理问题已修复。
本轮 Sanitizer 使用 IPv6 测试数据库配置，成本仅使用隔离 SQLite，无需再改路由。

完整基线证据在 `/tmp/baseliner-t6-output/`：full-tests.xml、full-ctest.log、full-ctest-detail.log、before-hashes.json 和 route 前后记录。
本轮证据在 `/tmp/baseliner-t6-final/`：cost-repeat.log、cost-measurements.json、cost-summary.json、sanitizer-results.json、
逐项 Sanitizer 原始日志、strict-results.json、format-final.json、最终 build/Diff/hash 审查和 verification.json。
测试数据库凭据仅存在本机 0600 文件；文档和仓库不保存凭据。

```bash
BASELINER_TEST_DB_OPTIONS=/private/backend-options.txt \
BASELINER_PRODUCTION_DB_OPTIONS=/private/production-options.txt \
cmake -B build src
cmake --build build -j4
# 在本机数据库可连接的环境中；WSL 条件与上文保持一致
setpriv --bounding-set=-net_admin ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^test_baseliner_cost$' --repeat until-fail:5 -V
```

## 原 baseline-operator Feature 的历史验收

以下保持原 T7.3/T7 的运行条件和原始数字，仅描述该历史 Feature。

验收日期：2026-10-10。代码在当前工作树中，未提交或推送。该记录描述可执行测试的条件和结果，不宣称生产规模吞吐保证。

### 最终完成证据

完整构建通过，末次日志无 warning/error；独立完整 CTest **77/77** 通过，耗时 **716.78s**，JUnit failures/errors/skipped 均为 0。
本机使用下文明确的 WSL 能力隔离条件，四真实来源/三托管目标保持必跑。
生产 26 组及插件重载后的 26 task 恢复、另行 30 组联合恢复全部通过；五轮成本重复通过。
定向生命周期及查询 ASan/UBSan/LSan、87 个 C++ 文件格式检查、含明确例外的 49 个翻译单元严格语法编译、文档配置生产解析器验证、Diff/冻结 hash 范围审查通过。
临时 WSL 路由已按原条目恢复。T7.3/T7 与本 Feature 完成，规格已归档，工作台 WIP=0；未提交或推送。

### 生产入口和后端范围

`test_baseliner_production` 通过 Scheduler 路由执行真实 SQL、真实 NPM 算子和真实数据库消费者。
确定性 PCAP 包含 41 个 60 秒桶、每桶三个 UDP 会话；固定每个完整桶 195 字节、3 包。
分析范围为前 40 个完整桶，UDP 超时显式设为 120 秒，forecast horizon=2。

- 原生有限 PCAP → NPM 落库 → snapshot。
- 测试 V2 capture adapter 消费同一真实 PCAP reader：各批次提供已处理包事实，尾批在确认 EOF 后提供空 backlog；
  NPM 自行发布 epoch/position/closed boundary → poll。适配器只用于确定性验收，没有声明真实网卡负载或 PCAP 自带进度能力。
- SQLite/MySQL/PostgreSQL/ClickHouse 四来源 × SQLite/MySQL/PostgreSQL 三托管目标 × snapshot/poll 两模式，合计 24 组。
- 每组同时包含 Value、Ratio、Relation、当前评估和 forecast；另有 SQLite 的 B=2P 两种模式，共 26 组。
- 实际 StopAll/Unload/Load 后恢复全部 26 个任务；结果值与逻辑键、完整模型 checkpoint 保持，重启本次写入为 0。
- 当前/预测结果通过三个目标的 Scheduler SQL 查询；ClickHouse 来源指纹用其原生 Arrow 查询能力读取。
  逐项验证实际值、冷启动 NULL、上下界/依据，保留源 Schema/内容及 npm_result_entities 的规范 Schema 指纹。

普通业务表、恢复故障、缺进度/不可兼容恢复、容量/TTL、停止/取消等边界由其余 contract、recovery、maintenance、lifecycle 测试覆盖。

### 生产验收发现并修复的问题

1. Baseliner 托管摘要缺 `rows_written`，导致成功持久发布后同步 SQL 被 Scheduler 拒绝。
   现在只累计本次成功事务中的结果/融合/维护行，恢复后没有新输入为 0。
2. Scheduler 的行式查询适配器借用 reader IPC 内存，reader 复用/销毁后仍合并旧批次。
   现在每批 IPC 复制到拥有型 Buffer。两批 8 KB 文本夹具在 Next/Close 覆盖旧字节，结果仍须准确；旧实现触发 ASan use-after-free，修复通过。
   代价是每输入批次一次字节复制；这条修复作用于结果查询，不改变 Baseliner 的版本化分页读取路径。

### 相同负载成本测量

`test_baseliner_cost` 用独立子进程隔离峰值内存，每个场景输入完全相同：40 桶 × 8 业务身份 × 三类任务 = 960 条标准观测；
24 个模型身份，Relation 三组/一个指标，forecast horizon=2，每次 Arrow 输入 24 条观测，checkpoint_every_buckets=7。
环境为 Intel i9-13900H、20 个逻辑 CPU、WSL2/Linux x86_64；使用仓库默认 CMake 构建，本轮成本目标没有 `-O` 优化选项，未固定 CPU 亲和性。
四种路径的 5760 个结果行数与 observed/expected/lower/upper 汇总值一致，结束后 Baseline 服务 task_count=0。

测量包括核心计算、可用的输入解码/结果编码、同步发布、无数据时间回调及正常 Flush，排除测试断言读取标量的耗时。
SQLite 延迟场景仅在测试 session 的每次 Commit 注入 2 ms；同时记录 CPU、提交次数、完整 checkpoint 字节数、当前/峰值 RSS。
这是标准观测进入算子后的成本比较，不包含源库扫描、索引布局、大表分页或真实网络采集成本。

五轮重复全部通过。下列数字为中位数，方括号为五轮最小值、最大值；四路径每轮均生成 5760 行，数值汇总对照通过。

| 路径 | 总耗时 ms | CPU ms | 观测/秒 | 峰值 RSS MiB |
| --- | ---: | ---: | ---: | ---: |
| 直接计算引擎 | 107.35 [104.33, 123.23] | 101.00 [97.25, 116.34] | 8942.33 [7790.28, 9201.47] | 28.14 [27.87, 28.19] |
| Arrow 算子 | 277.60 [263.36, 312.75] | 268.37 [254.31, 303.29] | 3458.19 [3069.54, 3645.14] | 30.55 [30.52, 30.65] |
| SQLite 托管 | 737.27 [722.90, 785.34] | 587.05 [574.94, 649.80] | 1302.10 [1222.40, 1327.99] | 43.67 [43.39, 43.92] |
| SQLite 托管，每提交 +2 ms | 760.83 [722.13, 794.28] | 585.20 [569.85, 636.96] | 1261.78 [1208.65, 1329.41] | 43.72 [43.54, 44.01] |

| 路径 | 最大单批耗时 ms | 无数据维护 ms | 开始/结束 RSS MiB（中位数） | 提交数 | 最终完整 checkpoint 字节 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 直接计算引擎 | 7.35 [6.24, 9.28] | 0.00 [0.00, 0.00] | 21.59 / 28.32 | 0 | 0 |
| Arrow 算子 | 10.21 [9.51, 12.55] | 0.00 [0.00, 0.00] | 21.95 / 30.85 | 0 | 0 |
| SQLite 托管 | 86.79 [84.36, 92.35] | 63.13 [59.29, 68.21] | 21.96 / 44.03 | 11 | 374769 |
| SQLite 托管，每提交 +2 ms | 87.32 [82.06, 91.05] | 66.50 [59.83, 75.80] | 21.98 / 44.05 | 11 | 374769 |

直接引擎和 Arrow 非托管路径不发布 checkpoint，表中 0 表示该路径不执行持久提交。非托管无数据回调为 0.0014～0.0023 ms；表中保留两位小数显示为 0.00。
Arrow 路径包含输入解码和输出编码，SQLite 托管路径还包含完整状态序列化与同步事务。托管路径每轮提交 11 次，最终 checkpoint 374769 字节。
注入延迟路径每次 Commit 等待 2 ms，五轮总耗时中位数较无注入路径增加约 23.56 ms；两路径测量范围重叠，单轮差异受调度和缓存影响。
无数据维护可能同步发布待提交 checkpoint，本次托管路径单次约 59～76 ms。背压、模型身份和 Relation 扇出增大时应重新测量，不能据此推导生产规模容量。
测试按独立进程的 getrusage 记录峰值 RSS，/proc 记录当前 RSS；两种内核计量口径可能存在差异。

### 验收环境与检查边界

目录夹具由 `test_baseliner_workspace` 和 Baseline 的 CTest TEST_INCLUDE_FILES 自动准备，evaluation/poll 通过 fixture 依赖运行，checkpoint/旧 Baseline 在 CTest 加载其目录时准备；不再依赖前次切片遗留的目录。
恢复矩阵一次完整运行实测 466.55s，旧 480s harness 上限只剩 13.45s 余量；TIMEOUT 调为 900s，数据库调用/取消/事务截止时间和全部断言不变。
生产配置与普通四库配置分开：生产 MySQL 使用容器已有测试管理员创建隔离数据库，不修改数据库用户权限；凭据仅留本机 0600 文件。
本轮曾遇到 WSL 镜像网络表 127 将 TCP/UDP 到 127.0.0.1 转至 loopback0，导致已监听的数据库与 HTTP 自连超时。
核验 IPv6 及本机其他地址正常后，临时将该地址路由到 lo；验收后恢复原条目。该环境问题不计作产品实现故障。
续执行时还发现中断前遗留 CTest 与本轮使用同一日志路径；已终止两轮无效运行，最终以单独从头执行的完整日志/JUnit 为准。

跨任务问题记录：目标为 Flow Labeling/系统 DPDK 的 NetVSC/failsafe 初始化和清理。在具有 NET_ADMIN 的 WSL 镜像网络上，
`test_npm_basic`、`test_config_channel_e2e`、`test_flow_labeling` 在自动创建 TAP 后崩溃；子进程堆栈为
`rte_vdev_uninit → rte_dev_remove → net_failsafe → rte_eal_cleanup → FlowLabelingPlugin::Stop`。
本 Feature 不修改该模块。最终完整 CTest 仅通过 `setpriv --bounding-set=-net_admin` 移除测试进程的网卡管理能力，
使用其 CPU/算法路径；Flow Labeling 全部原断言仍执行。真实数据库、NPM PCAP 和 Baseliner 生产入口未跳过。
这个运行条件不能证明具有 NET_ADMIN 时的 NetVSC/TAP 清理已修复；堆栈、失败日志与单独隔离测试保留在 dpdk-child-gdb.log、privileged-dpdk-attempt-full-ctest.log、dpdk-isolated.log。

定向 ASan/UBSan/LSan 包含完整 Baseline 算法、checkpoint、Baseliner 核心/reader/目标/provider、数据库实现和生命周期测试使用的 pipeline；
查询所有权用例另外覆盖 ChannelAdapter。`halt_on_error=1`、`detect_leaks=1`，在沙箱外执行。
动态加载的其他框架/NPM/系统数据库和 Arrow 依赖仍使用普通库；不宣称完整进程的所有依赖均经过 Sanitizer 仪器化。

额外严格语法编译覆盖本 Feature 改动的 49 个翻译单元，使用 `-Wall -Wextra -Werror`，保留测试使用的 unused-parameter/pessimizing-move 例外。
初轮扩展检查出现旧接口/旧测试的 missing-field-initializers、旧 toolkit 的 type-limits，以及 JSON 解析器的 dangling-reference 诊断。
解析器的 Required 返回父 JSON 对象中的子对象引用，临时 path 只用于错误路径，引用不指向该临时字符串；核查后仅该翻译单元关闭此诊断。
四个旧框架/测试翻译单元另关闭已核实的 missing-field-initializers/type-limits；其余翻译单元无新增例外。
原始诊断和例外复编结果分别保存在 strict-compile.log、strict-exceptions.log，不将初轮红灯描述为通过。

### 复现

准备两份仅本机可读的配置文件，内容为 DatabasePlugin 支持的 `type=...;name=...;...|...`。
`BASELINER_TEST_DB_OPTIONS` 包含 t2sqlite/t2mysql/t2postgres/t2clickhouse。
`BASELINER_PRODUCTION_DB_OPTIONS` 提供同名后端连接，需有创建独立测试数据库的权限；凭据不写入仓库。
生产测试为每轮分配唯一数据库名，SQLite 放入临时目录。下面的全量命令采用本轮 WSL 的能力隔离条件。
重跑前须确保 127.0.0.1 能访问本机服务；若 WSL 仍将它转发到 loopback0，先处理该路由异常。本轮临时修复在验收后恢复，原条目/修复核验/恢复快照分别保留在 wsl-route-before.txt、wsl-route-check.log、wsl-route-restored.txt。

```bash
BASELINER_TEST_DB_OPTIONS=/private/backend-options.txt \
BASELINER_PRODUCTION_DB_OPTIONS=/private/production-options.txt \
cmake -B build src
cmake --build build -j4
setpriv --bounding-set=-net_admin ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^test_baseliner_cost$' --repeat until-fail:5 -V
```

本轮本机原始证据放在 `/tmp/baseline-operator-t73/`；工作树测试和本文保留可重新执行的条件。
CTest 的成功用例 JUnit 输出默认截断为 1024 字节；状态/数量取完整 JUnit，26 组生产与 30 组恢复的逐项输出取最终 `full-ctest-detail.log`（保存本轮 LastTest.log 原文）。
