# Baseliner 验收记录

## baseliner-contract-safety T4 最终验收（2026-10-11）

本 Feature 的 T1～T4 已完成，规格已归档。按用户本轮授权，新网络头/分析地址布局及其 NPI/NPM
适配由独立 [npi-wire-layout](../tasks/archive/feat-npi-wire-layout.md) 交付，不作为本 Feature 的完成前置。
本次直接验收当前一致源码；本轮只修改任务/验收文档，所有既有生产源码、CMake 和测试与切片起点 SHA256 一致。
下文其他 T4 检查点均为历史记录，未将它们拼接为本次完整回归，也未将当前通过当作未来新布局的验收。

### 当前构建、回归与契约

- 标准 `cmake -B build src` 和 `cmake --build build -j4` 通过；本次构建日志没有 warning/error。
- 从第 1 项开始的一次完整 CTest **83/83** 通过，**1050.02s**；
  failures/errors/skipped 均为 0，真实数据库用例没有跳过。包含全部 Baseline/Baseliner、加载器及网络/框架既有回归。
- 当前定向 ASan/UBSan/LSan **11/11**、**144.50s**（10 个测试及 1 个目录 fixture），
  覆盖 loader、checkpoint、safety、operator、evaluation、Store、lifecycle、readiness、独立旧调用方和生产链路。
  保持 `detect_leaks=1`、halt_on_error，不添加抑制；无内存访问、未定义行为或泄漏报告。
- 真实 PCAP → NPM → Baseliner → 数据库/模型出口完成 **43 组分析 + 43 次插件重载恢复**；
  源 Schema/内容、结果值/NULL/逻辑键及恢复输出断言通过。普通完整回归和上述仪器化生产回归分别保留原始证据。
- 两种 Scheduler/Baseline 注册顺序 × Stop/直接 Unload，以及 Cancel 在途时 Join/Close、最后使用者退出后恰好 Close 一次，
  均由真实 `.so` 与可控屏障断言验收；低支持、禁止学习、bootstrap/checkpoint 首条、Relation mixed ready/cold、
  basis 切换、关闭 routed 输出、缺 V1 能力和诊断文案变化回归通过。
- 新旧 Submit 的完整旧结果、模型快照/学习次数和 checkpoint 等价；独立只使用旧接口的 dlopen 调用方通过。
  旧 service/types/checkpoint 公共头、结果 codec 与配置契约逐字节等于 HEAD；瞬时 readiness 不进入持久格式。
- 修改区间/新增文件 clang-format-18 **20 个文件**、统一版权检查通过；**12 个翻译单元**严格 C++17 语法编译通过。
  沿用仓库既有 unused-parameter 等例外；Scheduler 的既有 PollEvent 聚合初始化保留 missing-field-initializers 例外，
  安全测试的既有公共头保留 type-limits/pessimizing-move 例外，未将带例外检查表述为无例外严格编译。
- 本次核对 **213 个翻译单元 / 202 个不同源文件**的仪器化构建及 **15 个 FlowSQL `.so`** 的 ASan/UBSan 符号。
  Arrow、DPDK、数据库客户端、ICU/OpenSSL 等第三方仍使用普通依赖缓存；没有声明第三方库完整仪器化。
- TSan 最小探针 exit 66，`unexpected memory mapping`，记录为本机不支持；未声明 TSan race 检查通过。

### 当前五轮固定负载成本

Intel i9-13900H、20 逻辑 CPU、WSL2/Linux x86_64；仓库空 build type，成本目标无优化 flags。
两项测试固定 `taskset -c 0`，各五轮通过；不是独占系统，测量期间存在构建/测试负载，原始范围保留。
下表为五轮中位数 [min, max]；第一表单位 μs。

| 测量 | 中位数 [min, max] |
| --- | ---: |
| 旧 Submit 墙钟 / 观测 | 20.964 [19.913, 33.168] |
| V1 Submit 墙钟 / 观测 | 20.327 [19.929, 34.027] |
| 旧 Submit CPU / 观测 | 20.773 [19.882, 32.604] |
| V1 Submit CPU / 观测 | 19.934 [19.897, 34.165] |
| Join 每轮墙钟中位数 | 8.117 [7.867, 10.507] |
| Join 每轮墙钟 p95 | 12.455 [11.348, 26.017] |
| Join 每轮墙钟平均值 | 9.097 [8.770, 13.358] |
| Join 每轮 CPU 平均值 | 9.810 [9.680, 14.653] |
| 两个 Store Open/Schema 每轮平均值 | 640.882 [624.794, 884.928] |
| 两个 Store 最终 Close 每轮平均值 | 6.119 [5.992, 7.635] |

| 路径 | 总耗时 ms | CPU ms | 峰值 RSS MiB |
| --- | ---: | ---: | ---: |
| 直接评估引擎 | 108.519 [103.342, 130.965] | 100.228 [98.111, 102.276] | 30.164 [30.051, 30.207] |
| Arrow 算子 | 308.408 [293.286, 312.148] | 302.745 [290.381, 309.679] | 32.766 [32.680, 32.809] |
| SQLite 托管 | 749.265 [739.379, 762.001] | 625.871 [612.880, 644.058] | 45.938 [45.797, 46.066] |
| SQLite 托管，每提交 +2 ms | 775.012 [762.485, 814.487] | 627.478 [620.223, 672.272] | 45.980 [45.934, 46.219] |

| DataFrame 原始数据路径 | 总耗时 ms | CPU ms | 峰值 RSS MiB |
| --- | ---: | ---: | ---: |
| 有序宽类型 → DataFrame 结果 | 170.144 [165.554, 195.416] | 164.962 [163.616, 183.550] | 46.094 [45.988, 46.270] |
| 逆序宽类型 → DataFrame 结果 | 161.689 [159.179, 166.617] | 162.319 [159.903, 167.225] | 46.109 [46.039, 46.215] |
| 逆序窄类型 → DataFrame 结果 | 164.256 [160.784, 209.114] | 165.070 [161.382, 207.213] | 46.984 [46.941, 47.148] |
| 逆序窄类型 → DataFrame 结果及模型 | 169.171 [166.192, 246.674] | 169.942 [164.696, 247.514] | 47.305 [47.258, 47.461] |
| 逆序窄类型 → SQLite 结果及模型 | 385.314 [368.393, 686.152] | 348.835 [332.559, 568.085] | 41.105 [41.004, 41.281] |

Submit 固定一个 Value 身份、10000 条观测，轮次交替新旧入口顺序；checksum/完整快照相同，cold=1、ready=9999。
旧入口和 V1 使用当前同一算法实现，不能据此推导修复前后 overhead。计时包含观测构造、结果消费和夹具断言，
排除 Open、快照查询及环境析构。Join 每轮 200 对 SQLite Store，仅测 Join，Open/Schema 与最终 Close 单列；
Join 含被替换会话最后所有者的同步关闭，不能解释为一次 shared_ptr 交换的成本。

既有成本测试的四条标准观测路径和五条原始 DataFrame 路径，各轮均输出 5760 行并通过数值汇总对照；
三类算法、960 条观测、24 个模型身份、Relation 3 组/1 指标、horizon=2，Arrow 每批 24 条、checkpoint_every_buckets=7。
模型出口场景各轮 24 行；原始 DataFrame 为同一份 960 行，宽/窄类型、物理排序、出口组合按既有夹具固定。
原始 DataFrame 与标准观测路径的计时范围不同，分别比较各组；逐组 CPU、RSS、Open、模型导出编码、发布、
目标初始化和维护延迟见机器可读汇总。这些小夹具不提供大表扫描、真实采集吞吐或生产规模 SLA。

### 环境与可复核证据

初次沙箱启动被 `setpriv: apply bounding set: Operation not permitted` 阻止；保留日志，未运行测试，
有效验收在沙箱外执行并沿用 `setpriv --bounding-set=-net_admin` 的 WSL 条件，不据此声称特权 DPDK 清理已修复。
完整回归首次又确认本机 IPv4 数据库超时，WSL 表 127 将 127.0.0.1 指向 loopback0；保留失败/中断日志。
仅在完整 CTest 期间临时改为 lo，三个数据库端口连通后从第 1 项重跑；结束后原路由已恢复，前后原文完全相同。
Baseliner 普通/仪器化用例使用已有 IPv6 私有配置文件，真实后端必跑；没有把连接串误当文件路径。

证据 `/tmp/baseliner-contract-safety-closeout/`：`full-tests.xml`、`full-ctest.log`、`full-test-detail.log`、
`sanitizer-tests.xml`、`sanitizer-detail.log`、`sanitizer-coverage.json`、构建日志、
`cost-runs.json`、`cost-measurements.json`、`cost-summary.json`、`format-results.json`、`strict-results.json`、
`compatibility.json`、`accepted-artifact-hashes.json`、路由前/中/后记录、起点/增量 diff/hash 和 `verification.json`。
凭据仅使用已有本机私有文件，不进入仓库。工作台 WIP=0，完成即停，未提交/推送；NPI 新布局 T1～T4 状态保持。


## 当前任务归属调整（2026-10-11）

协议报文布局、分析地址分离及 NPI/NPM Basic 适配已独立为 [npi-wire-layout](../tasks/archive/feat-npi-wire-layout.md)。
所有网络协议报文头及嵌套报文结构按 1 字节布局，分析用 IPv4/IPv6/MAC 采用各自适宜对齐；
本轮仅交付方案和任务文档，原有源码改动保留，新布局尚未实施或验收。

按用户后续授权，Baseliner T1/T2/T3 状态保持，T4 独立验收当前一致源码的 PCAP → NPM → Baseliner 集成链路，
完成自身完整 CTest、契约成本和文档出口；新网络布局的同版部署回归、网络字段/识别成本由新 Feature 承接。
下文的普通/仪器化通过和已修复泄漏仍是对应源码快照的有效记录；不能作为尚未实施的新布局证据。
Baseliner 的提交/会话交接五轮成本已记录，后续核对适用范围；不将其重新表述为尚未测量。

## baseliner-contract-safety T4 Hyperscan 数据库交接修复（2026-10-11）

用户确认扩大范围后，本切片修复 `RegexRecognizer::Ready` 重编译覆盖旧数据库指针的泄漏。
新数据库编译到局部指针；失败保留旧数据库，成功后释放旧数据库并交接，再执行既有 scratch 分配。
公共头、规则、scratch 分配流程和识别热路径保持；此前池/parser 修复与其余工作树改动全部保留。
本切片已验收，T4 仍未整体完成；以下当前证据优先于后文历史检查点。

- 标准 `cmake -B build src` 及既有仪器化 build 配置通过，使用已有数据库 options 文件路径；
  普通 packet/alignment targets 与全部 11 个定向 Sanitizer targets 构建通过。
- 普通 packet/alignment CTest **2/2**；ASan/UBSan/LSan 定向 CTest **12/12**，**144.14s**（含目录 fixture），
  没有跳过测试；loader、checkpoint、safety、operator、evaluation、Store、lifecycle、readiness 与生产均通过。
- 真实 NPI `.so` 空配置与已有规则配置各三次加载/停止/卸载，HTTP 规则在 0～15 偏移均命中；
  前轮同规则重载 **343,200 bytes / 4 allocations** 泄漏已消除。
- 当前生产 **43 组分析 + 43 次任务重载恢复** 通过，源 Schema/内容保持，退出无 Sanitizer 报告；
  detect_leaks=1，未添加抑制；有效测试在沙箱外去除 net_admin 执行，第三方缓存库仍为普通构建。
- 独立旧/新实现 ASan/UBSan/LSan 探针：旧实现连续 Ready 后编译无效正则，旧数据库被丢弃且泄漏，exit 1；
  修复后连续三次 Ready、两个 scratch 的实际匹配、失败保留旧数据库与匹配全部通过，exit 0、无泄漏。
  仓库已有真实重载回归保持；临时探针与原始日志作为补充证据保留。
- 修改区间 clang-format-18 与版权检查通过；regex 翻译单元严格 C++17 编译通过，沿用既有公共头/回调
  unused-parameter、reorder、pessimizing-move、type-limits 告警例外，未宣称无例外严格编译。
- 八个相关公共头与切片起点逐字节一致；其中六个旧公共头仍等于 HEAD；所有切片外文件与起点一致。
- 本轮不修改识别热路径，不重新外推前轮成本结果；完整普通 CTest、额外隧道字段和实际规则成本证据仍待 T4 最终收口。
  工作台 WIP=0，完成即停；T4 未勾选，Feature 未归档，未提交/推送。

证据目录 `/tmp/npi-regex-t4/`：`verification.json`、`normal-tests.xml/log`、`sanitizer-tests.xml/log`、
`sanitizer-detail.log`（含生产与 HTTP 命中完整输出）、`before-leak.log`、`probe-results.json` 与旧/新探针日志，
构建/格式/严格编译日志、兼容检查、切片增量 diff、范围与最终 hash。

## baseliner-contract-safety T4 生命周期释放修复（2026-10-11）

本切片已实施获准的 ObjectsPool/BufferPool 当前块和 NPI parser 析构释放；公共结构、分配路径与识别规则保持。
当前 T4 仍未完成：新同库重载回归又确认 Hyperscan 数据库覆盖泄漏，尚未获准扩大到 regexmatch.cpp。
以下新证据优先于后文前轮检查点，不将历史完整 CTest 作为当前完整验收。

- 标准 src 配置、全量普通构建通过；头文件实际调用方 config/engine/model/npi 均重新编译。
- 普通 packet/alignment CTest 2/2 通过；回归覆盖空池、部分块、整块、多块、非平凡对象析构计数、
  BufferPool 零初始化与跨块数据保持，真实 .so 空配置和仓库已有规则配置各三次加载/停止/卸载。
  已有规则确实加载：查询 HTTP 条目并验证 0～15 起始偏移下 HTTP payload 命中 2000。
- 新 ObjectsPool 断言在修复前稳定失败（SIGABRT，销毁后仍有存活对象）；原块释放修复后通过。
  BufferPool 旧/新独立 LSan 对照：旧实现泄漏 exit 1，修复后无泄漏 exit 0；原始程序和日志保留。
- 独立池测试和空配置 NPI 重载在 ASan/UBSan/LSan 下通过。保留 detect_leaks=1；沙箱 ptrace 失败原日志保留，
  有效结果来自沙箱外执行，未添加抑制或关闭检查。
- 完整本切片定向 Sanitizer **11/12**，147.39s（含一个目录 fixture）：loader、checkpoint、safety、operator、
  evaluation、Store、lifecycle、readiness、packet 和生产通过；生产 **43 组分析 + 43 次任务重载恢复**，
  源内容/Schema 保持，退出无 LSan 报告。第三方缓存库仍为普通构建，FlowSQL 依赖沿用仪器化构建。
- 唯一失败为新增规则配置反复加载的 alignment 测试：**343,200 bytes / 4 allocations**，栈指向
  `RegexRecognizer::Ready → hs_compile_multi`。Ready 将新数据库直接写入 hyper_database_，
  重新 Load 时覆盖旧指针；当前析构只能释放最后一个数据库。此问题与已修复的池/parser 泄漏有独立栈证据。
- 最小待确认方案：增加允许 regexmatch.cpp，编译新数据库到局部指针；成功后释放旧数据库并交接指针，
  失败保留旧数据库，scratch 分配和规则/公共接口保持。沿用同库重载、HTTP 命中与 LSan 回归，不减弱断言。
- 修改区间 clang-format-18 通过；六个相关翻译单元严格 C++17 检查通过，既有公共头告警例外和 Config
  具体类型删除 Dictionary 的 delete-non-virtual-dtor 告警例外均单列，未越界改公共头。
- 本切片没有修改热路径，不重测或改变前轮成本结论；完整普通 CTest、更多隧道字段和成本限制仍属 T4 最终验收待项。
- 非阻塞旁路发现：Dictionary::Count 的计数没有在 Insert 更新，真实规则查询成功但 Count 为零；
  测试已改为实际条目/规则命中断言，本切片不修改该独立计数问题。

证据目录 `/tmp/npi-lifecycle-t4/`：旧实现断言、BufferPool 对照、标准/仪器化构建，normal-final.xml，
sanitizer-tests.xml/log、sanitizer-detail.log（生产 43 组完整输出）、pool-after.log、npi-empty-sanitizer.log，
格式/严格编译原始与例外日志、incremental scope/diff/hash。后续状态以工作台为准，未提交/推送。

## baseliner-contract-safety T4 续执行检查点（2026-10-11）

T4 仍未完成。本次续执行保留已有 NPI 对齐读取草稿，未新增实现代码；实际刷新了构建、定向测试、
Sanitizer、成本及兼容核验。以下结果优先于后文前轮检查点，不把历史完整 CTest 拼接为本次全绿。

- 标准 src 配置和全量构建通过；普通 packet/alignment 定向 CTest 2/2 通过。
- 新对齐测试在真实 NPI .so 下完成 0～15 偏移及截断前缀断言；ASan/UBSan 未再次报告未对齐访问，
  但退出 LSan 报告 **16,752 bytes / 23 allocations**，因此该测试和本切片均不能宣称验收通过。
- 泄漏证据：`NetworkLayer::~NetworkLayer()` 为空，22 个 parser 对象未释放；
  `ObjectsPool<Recognized,1024>` 析构仅清理 used 链表，当前 `using_objects_` 块未释放。
  通用池文件不在当前允许清单；未关闭 detect_leaks、未添加抑制，也未越界修复。
- 初次定向 Sanitizer 8/12 通过；另外三项数据库测试受生成配置错误阻碍：CTest 将连接串写入了
  本应为文件路径的 BASELINER_*_DB_OPTIONS。重新以已有 options.txt 路径配置后复跑，Store/lifecycle 通过；生产 43 组分析与 43 次重载恢复
  功能断言通过，随后退出 LSan 报 **1,239,101 bytes / 70 allocations**，三项最终 2/3（147.19s）。
  符号核查同时指向 ObjectsPool 的 Definition/Item/Recognized 当前块、BufferPool 当前块及 NPI parser，
  原始退出栈部分因 .so 已卸载显示 unknown module；地址按已知 NPI 基址还原，符号结果单独保存。
- 本次普通完整 CTest 未通过且已终止（exit 143）：NPM 段错误、MySQL 连接失败，随后 PCAP upload 挂起。
  GDB 定位 NPM 为 DPDK `rte_vdev_uninit → rte_eal_cleanup → FlowLabelingPlugin::Stop`；
  按既有 WSL 条件 `setpriv --bounding-set=-net_admin` 复跑该项通过（12.19s）。
  MySQL 在沙箱外仍连接失败；未改路由，完整 CTest 待环境恢复后从头验收。
- clang-format-18 对修改区间/新增文件检查通过；五个相关翻译单元严格编译通过，但沿用已有
  公共头 warning 例外（unused-parameter/reorder/pessimizing-move/type-limits），不宣称无例外严格编译。
- 公共 netbase/netaddress/iprotocol 与旧 Baseline service/types 头逐字节等于 HEAD。
- 固定 CPU 0、八个既有报文夹具、每阶段 50,000 包，旧/新交替五轮：分层中位数 58.4332 → 59.0839 ns/包
  （+1.1%），端点解码 234.864 → 259.849（+10.6%），识别入口 20.2795 → 21.0327（+3.7%）。
  普通构建为 -g、未启用优化，测量并非独占系统；识别夹具未加载协议规则、checksum=0，不能外推完整识别成本。
  尚需补齐实际规则命中、其他改动隧道字段与有效负载的验收；未为降低成本扩大代码范围。

证据在 `/tmp/npi-alignment-t4/resume-*`：初次 Sanitizer XML/log、配置修正复跑、GDB 栈、
WSL 条件复跑、五轮成本、格式、严格编译（原失败和例外复跑分别保留）、公共头兼容和范围/hash。
本次不归档、不修改 Backlog、不提交/推送。

## baseliner-contract-safety T4 前轮验收检查点

验收日期：2026-10-11。本次 T4 尚未完成：生产 Sanitizer 在既有 NPI IPv4 未对齐访问处失败，
完整普通 CTest 正在运行。T1～T3 保持完成；T4 未勾选，Feature 未归档，未提交或推送。
以下是本轮当前源码的结果，后文历史 Feature 的完成记录不作为本 Feature 验收依据。

### 当前验证与阻塞

标准 `cmake -B build src`、`cmake --build build -j4` 完成，无编译 Error。
全量日志有 9 条未修改 `test_bridge/main.cpp` 的既有 Arrow nodiscard 返回值告警；本轮没有修改该测试。
Feature 的 C++ 修改区间/新增文件格式检查、12 个实现/测试翻译单元严格 C++17 编译通过。
Scheduler 注册单元保留已有公共头告警例外；本轮额外的 `PollEvent::err_msg` 聚合初始化告警例外与原失败日志单列保存。
旧 Baseline service/types/checkpoint 公共头、结果 codec 和配置契约与 HEAD 及切片起点逐字节一致。

普通完整 CTest 已启动，真实数据库用例必跑，沿用 `setpriv --bounding-set=-net_admin` 的 WSL 条件。
截至 2026-10-11T11:53:58+08:00，前 74/82 项通过，完整命令尚未结束；test_baseliner_recovery 仍运行。本检查点不宣称完整回归通过。
沙箱外的真实数据库 Store 探针通过；沙箱网络失败日志保留。

定向 ASan/UBSan/LSan 使用独立构建目录、标准 src 入口、`-fsanitize=address,undefined -fno-omit-frame-pointer -O0`，
复用主工作区第三方缓存。核对 208 个实际翻译单元、199 个不同源文件和 15 个 FlowSQL 动态库已仪器化，
覆盖完整 Baseline/Baseliner、Scheduler 及本次加载的 FlowSQL 依赖；Arrow、DPDK、数据库客户端、ICU/OpenSSL 保持普通缓存库。
沙箱内 LSan 遇到 ptrace 限制后，保持 `detect_leaks=1` 在沙箱外重跑；最终 **9/10 项通过**，包含一个目录 fixture。
loader、safety、checkpoint、operator、evaluation、Store、lifecycle、readiness 均通过，生产测试失败，合计 34.90s。

生产失败报告为 `src/plugins/npi/layer.cpp:91` 对 `Ipv4Header` 的 misaligned member access：以太网头偏移 14 字节，
而类型要求 4 字节对齐。栈为 PCAP reader → NPI packet decoder → network layer → ipv4_delamination，
发生在生产 PCAP → NPM 导入阶段，不能据此宣称完整生产链路的 Sanitizer 通过。
layer/packet_decoder/engine/netbase 四文件 hash 与 HEAD、切片起点均相同；这些文件不在本轮允许清单，未越界修复。
继续验收需要先用单独冻结的切片修复网络头未对齐读取，并重跑生产 Sanitizer。
TSan 的 PIE/非 PIE 最小探针均因 `unexpected memory mapping` 退出 66，记录为环境不支持，未声明 race 检查通过。

### 固定负载成本

Intel i9-13900H、20 逻辑 CPU、WSL2/Linux x86_64；默认空 CMake build type，测量目标没有优化 flags。
两项成本测试均以 `taskset -c 0` 固定 CPU，各运行五轮；测量时完整 CTest 也在运行，存在并行系统负载。
下表为五轮中位数和各轮 min/max，时间单位 μs。

| 测量 | 中位数 [min, max] |
| --- | --- |
| 旧 Submit 墙钟 / 观测 | 20.047 [19.999, 22.019] |
| V1 Submit 墙钟 / 观测 | 19.710 [19.557, 19.859] |
| 旧 Submit CPU / 观测 | 19.945 [19.898, 21.048] |
| V1 Submit CPU / 观测 | 19.606 [19.465, 19.786] |
| Join 每轮墙钟中位数 | 8.228 [7.775, 8.354] |
| Join 每轮墙钟 p95 | 13.956 [11.205, 15.160] |
| Join 每轮墙钟平均值 | 9.181 [8.459, 9.627] |
| Join 每轮 CPU 平均值 | 9.914 [9.183, 10.415] |
| 两个 Store Open/Schema 每轮平均值 | 606.424 [587.691, 631.366] |
| 两个 Store 最终 Close 每轮平均值 | 6.406 [6.042, 7.224] |

Submit 固定一个 Value 身份、10000 条观测，按轮交替新旧入口顺序，断言完整序列快照和 checksum 一致，
V1 cold=1、ready=9999。这是当前旧入口与 V1 入口比较，两者共享新算法实现，不能视为修复前后 overhead。
计时包含观测构造、结果消费及断言等夹具开销；Open/建表、快照查询与环境析构不计入 Submit。
Join 固定 200 对 SQLite Store，只计 Join，Open/Schema 和最后 Close 单列；Join 内部释放被替换会话的最后所有者，
因此包含其同步关闭成本，不能将这个数字解释为单次 shared_ptr 交换成本。
每轮 Join p50/p95 取排序后下标 100/190 的样本（下标从 0 开始），表中再汇总五轮分位数；计时调用本身也有开销。

既有 `test_baseliner_cost` 保持每轮 960 条标准观测、三类算法、24 个模型身份、5760 个结果，值汇总一致，五轮全部通过。
四条路径的墙钟中位数 [min, max]，单位 ms：

| 路径 | 墙钟耗时 |
| --- | --- |
| Evaluation engine | 105.92 [103.25, 110.19] |
| Arrow operator | 303.01 [299.48, 316.43] |
| 托管 SQLite | 732.92 [728.96, 815.09] |
| 托管 SQLite，每次 Commit 注入 2 ms | 793.67 [747.69, 830.44] |

原始 DataFrame 五路径总耗时中位数分别为 164.02、164.77、166.51、167.15、386.41 ms，
对应排序/逆序、窄类型转换、模型导出及 SQLite 双表输出，场景定义和计时边界沿用既有成本测试。
逐组 CPU、RSS、input Open、模型导出编码、发布、目标初始化等中位数和范围保存在机器可读汇总，
不得将这些小夹具推广为生产 SLA、网络吞吐或大表扫描结论。

本轮证据目录 `/tmp/baseliner-contract-safety-t4/`：普通构建/CTest 日志和 XML、`sanitizer-tests.log/xml`、
`sanitizer-coverage.json`、`npi-blocker.json`、`cost-runs.json`、`cost-measurements.json`、`cost-summary.json`、
兼容/格式/严格编译和起点/最终范围 hash；凭据仅在本机 0600 文件中，未写入文档或仓库。

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
