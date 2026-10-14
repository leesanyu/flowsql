# 即时工作台

WIP=0；Kafka 规格已补充第三方组件管理契约，Feature 与全部 Feature Task 保持待实施。

## 当前 Atomic Slice

- 关联：用户认可第三方组件管理方案并要求补充到 kafka-connection-auth 规格；仅补充 T1、T2、T5 的交付与验收边界，不实施这些任务。
- 业务意图：明确 Kafka 客户端的可复现构建、共享缓存、私有依赖及完整运行时交付，使原生和 Docker 获得可核验的相同认证能力。
- Non-Goals：不新增 Feature 或修改 Backlog，不修改代码、CMake、Dockerfile、配置或公共接口，不下载/安装/编译依赖，不执行认证实测，不暂存、提交或推送。
- 允许文件：tasks/active_task.md、tasks/specs/feat-kafka-connection-auth.md；保留全部既有工作区修改与前序工作台证据。
- 冻结契约：补充组件分类、固定来源/版本/校验与构建能力、主工作区缓存及指纹、librdkafka PIC 静态库私有链接和系统共享认证库、原生/Docker 运行时依赖检查；归入 T1/T2/T5，五任务均维持 [ ]。
- 本轮步骤：保存开始快照；补充依赖契约、相关任务与测试锚点；一次本轮 Diff 审查；核对规格尺寸/层级/状态、原契约及无关修改保持；记录证据并停止。
- 验收命令：awk 统计完成证据前非空行；Python 核对任务状态/关键契约/保留内容与摘要；git diff --name-only；git diff --check。
- 构建边界：仅 Markdown 文档修改，无对应 CMake target，不运行构建、CTest、前端测试或第三方组件实测。
- 时间盒：10 分钟，自 2026-10-14 08:39:41 UTC 起。
- 停止条件：组件管理方案完整纳入规格，文档、范围及保留检查通过后更新 WIP=0 并停止；不自动开始 T1。

### 本切片完成证据

- 规格新增“第三方组件与部署契约”，明确 librdkafka 固定源码/校验构建、系统共享认证库、压缩选项、已有组件复用与测试服务边界；具体版本和平台在 T1 冻结。
- 补充依赖清单、许可证与实际能力、主工作区预热及共享缓存指纹、离线与不匹配行为、Provider 私有 PIC 静态客户端链接、原生/Docker 运行包与插件/工具/CA 检查及依赖升级验收。
- T1/T2/T5 目标和两个测试锚点同步补充；完成证据前 189 个非空行，仍为五个待实施任务、两条主链路，无三级编号；原公共接口及其他契约逐字保持。
- 本轮 Diff 审查及 Python 契约/状态/空白/保留检查通过；Backlog 与其他五份既有修改文件的 SHA256、HEAD 和暂存区保持切片开始值，前序工作台完整保留；git diff --name-only 范围与 git diff --check 核对通过。
- 本轮只修改规格与工作台，未下载、安装、构建或实测第三方组件，未运行 CMake/CTest/前端测试，未暂存、提交或推送；完成后停止，不自动开始 T1。

## 上一切片：Kafka 连接与认证及通道 Web 管理规格撰写

- 关联：用户要求先编写 kafka-connection-auth 规格，明确 Kafka 可作为 source 和 sink，并纳入通道创建和管理的 Web 前端；本轮为规格切片，Feature Task 尚未实施。
- 业务意图：把可复用连接认证、具名 Kafka 通道、Web 管理与可观察就绪状态收口为可独立验收的 Feature。
- Non-Goals：不修改代码、配置或现有公共头文件，不安装依赖、实现四类数据适配器、执行 Kafka 连通/认证实测，不提交或推送。
- 允许文件：tasks/active_task.md、tasks/product_backlog.md、tasks/specs/feat-kafka-connection-auth.md；保留其他既有未提交修改及前序工作台证据。
- 冻结契约：规格包含业务意图、Non-Goals、连接/凭证/通道及会话契约、source/sink/both 角色、认证矩阵、Web CRUD 与验证、两条主链路、通常五个结果导向 Feature Task 和测试锚点；只登记规划状态，不勾选实施任务；Backlog 补充 Web 管理价值并链接规格，维持 [ ]。
- 本轮步骤：核对现有通道接口、Web 页面和认证资料；编写精益规格；一次契约与范围审查；核对行数、层级、链接、任务状态及无关内容保持；记录证据后停止。
- 验收命令：awk 统计完成证据前非空行；Python 核对任务编号/状态、关键契约、规格链接及保留摘要；git diff --name-only；git diff --check。
- 构建边界：仅规格与任务文档修改，不运行构建或 CTest；实现阶段验证目标写入规格，不将其记作本轮执行结果。
- 时间盒：原轮自 2026-10-14 07:42:20 UTC 起 30 分钟，已留下规格草案；用户要求“继续”后沿用相同 Feature 和允许范围，于 2026-10-14 08:24:28 UTC 启动 20 分钟续轮，完成规格检查与收口。
- 停止条件：规格覆盖连接认证和 Web 通道管理、两条主链路及客观验收边界，文档检查和一次审查通过后更新工作台并停止；不自动开始实施。

### 本切片完成证据

- 新建 tasks/specs/feat-kafka-connection-auth.md，明确可复用连接与 source/sink/both 具名通道、四类安全协议及标准 SASL 机制、凭证生命周期、纯虚会话接口、SQLite 持久化与 Web 创建/详情/编辑/验证/删除。
- 一次有界契约审查补清 both 的方向独立配置、OpenSession 不等待网络、异步验证/取消与总截止时间、Kerberos 私有票据及 GSSAPI 身份隔离、验证禁止 Topic 自动创建；实际客户端可行性与真实认证留待实施验收。
- Backlog 仅修改 kafka-connection-auth 一行，补充 Web 管理价值并链接规格，维持 P1、[ ]；四项数据 Feature 及其他 Backlog 内容与切片开始时逐字一致。
- 规格完成证据前 161 个非空行，T1～T5 共五个一级 Feature Task 全部未勾选，包含两条主链路，无三级编号；Python 核对关键契约、链接、任务状态、代码围栏及新文件空白通过。
- 前序工作台完整保留；其他五份既有修改文件的 SHA256、HEAD 与暂存区保持切片开始值；git diff --name-only、未跟踪规格范围及 git diff --check 核对通过。
- 本轮仅交付规格和任务文档，未实施代码、安装依赖、运行构建/CTest/前端测试或 Kafka 连通认证实测，未暂存、提交或推送；完成后停止，不自动开始 T1。

## 上一切片：Kafka 连接认证 Backlog 补充

- 关联：用户认可 Kafka Feature 价值与规模复检结论，要求将新 Feature 补充到 Backlog；尚无关联 Feature Task。
- 业务意图：为不同安全策略的 Kafka 接入建立可复用的连接认证目标，让四项数据通道共享明确的认证、凭证生命周期和诊断责任。
- Non-Goals：不创建规格或实施代码，不配置或测试 Kafka，不新增条件性的云厂商认证、Schema Registry、NPM 多实体直接发布或状态恢复 Feature，不改变既有优先级，不提交或推送。
- 允许文件：tasks/active_task.md、tasks/product_backlog.md；保留其他既有未提交修改及前序工作台证据。
- 冻结契约：新增 P1 kafka-connection-auth，覆盖四类安全协议、TLS 单向/双向及标准 SASL 机制、凭证生命周期、能力检查和诊断；原四项声明依赖，kafka-log-source 显示名称调整为结构化数据接入且保留标识，kafka-result-sink 明确标准结果批次发布及 NPM 全实体直接发布边界；五项状态均为 [ ]、规格均为待创建，结构化两项 P1、packet 两项 P2。
- 本轮步骤：核对现行条目及保留摘要；冻结工作台；新增连接认证目标并补充四项依赖与范围；核对表格、状态、编号唯一性及无关内容保持；审查 Diff 并记录证据。
- 验收命令：Python 核对五项标识、状态、优先级、规格占位、认证范围和四项依赖；核对其他 Backlog 条目、前序工作台及其他文件摘要保持；git diff --name-only；git diff --check。
- 构建边界：仅任务文档修改，不运行构建或 CTest。
- 时间盒：10 分钟，自 2026-10-14 07:26:10 UTC 起。
- 停止条件：五项符合已认可的价值与规模边界、文档与范围核对通过后更新工作台并停止；不自动启动规格或实现。

### 本切片完成证据

- 新增 P1 kafka-connection-auth，目标覆盖四类安全协议、TLS 服务端/双向认证、主机名校验及 PLAIN、SCRAM-SHA-256/512、GSSAPI、OAUTHBEARER，包含凭证引用、脱敏、适用的更新/续期、构建能力检查和错误诊断。
- 四项数据 Feature 均声明依赖 kafka-connection-auth；日志接入显示名称更新为结构化数据接入并覆盖日志、指标和业务记录，保留 kafka-log-source 标识；结果发布明确标准结果批次、有界发送和停止排空，排除 NPM 全部实体直接持续发布接线；packet 两项保持 P2 并明确状态恢复、探针管理和磁盘缓冲边界。
- 五项状态均为 [ ]、规格均为待创建；Python 核对表格列数、优先级、顺序、编号唯一性、认证范围和共享依赖通过；其他 Backlog 内容与前序工作台记录完整保持。
- 其他五份既有修改文件的 SHA256、HEAD 与暂存区保持本轮开始值；git diff --name-only 范围核对与 git diff --check 通过。
- 本轮仅修改 Backlog 与工作台，未创建规格、实施代码或新增条件性 Feature，未运行构建、CTest 或 Kafka 实测，未暂存、提交或推送；完成后停止。

## 上一切片：Kafka Feature 优先级调整

- 关联：用户要求结构化数据能力的优先级高于数据包能力；尚无关联 Feature Task。
- 业务意图：优先交付 Kafka 结构化日志接入与分析结果发布，将原始包接入和发布排在其后。
- Non-Goals：不调整四项能力的目标、边界、标识、待规划状态或规格状态，不创建规格、实施代码、运行服务或提交推送。
- 允许文件：tasks/active_task.md、tasks/product_backlog.md；保留其他既有未提交修改及前序工作台证据。
- 冻结契约：kafka-log-source、kafka-result-sink 均为 P1，kafka-packet-source、kafka-packet-sink 均为 P2；四项在 Backlog 中按日志接入、结果发布、原始包接入、原始包发布排列。
- 本轮步骤：核对现行条目；调整优先级与四项内部顺序；核对目标和状态保持；审查 Diff 并更新完成证据。
- 验收命令：Python 核对四项优先级、顺序、目标/状态及其他 Backlog 内容保持；核对其他文件摘要及 HEAD/暂存区保持；git diff --name-only；git diff --check。
- 构建边界：仅任务文档修改，不运行构建或 CTest。
- 时间盒：10 分钟，自 2026-10-14 07:05:40 UTC 起。
- 停止条件：结构化两项高于 packet 两项，文档与范围核对通过后更新工作台并停止。

### 本切片完成证据

- kafka-log-source 从 P2 调整为 P1，kafka-packet-source 从 P1 调整为 P2；kafka-result-sink 保持 P1，kafka-packet-sink 保持 P2。
- 四项按结构化日志接入、分析结果发布、原始包接入、原始包发布排列；Feature 目标、边界、标识、状态和规格占位均保持原样，其他 Backlog 条目逐字保持。
- Python 核对优先级、顺序、条目内容及前序工作台证据通过；其他五份既有修改文件的 SHA256、HEAD 与暂存区保持不变；git diff --name-only 范围核对及 git diff --check 通过。
- 本轮仅修改 Backlog 与工作台，未运行构建或 CTest，未创建规格、实施代码、暂存、提交或推送；完成后停止。

## 上一切片：Kafka 四项能力补充到 Backlog

- 关联：用户认可基于 docs/vision.md 的 Kafka 能力与边界分析，要求把四个运行场景补充到 Backlog；尚无关联 Feature Task。
- 业务意图：为远端 packet 接入、结构化日志接入、原始包发布和分析结果发布分别记录可独立交付的产品目标，供后续选择与规格化。
- Non-Goals：不创建规格、拆分 Feature Task、实现代码、配置 Kafka 或运行服务，不修改既有 Feature 状态，不提交或推送。
- 允许文件：tasks/active_task.md、tasks/product_backlog.md；保留既有部署配置、docs/npm.md 和 Python 样例/测试修改。
- 冻结契约：新增 kafka-packet-source、kafka-log-source、kafka-packet-sink、kafka-result-sink 四个 Feature，状态均为 [ ]、规格均为待创建；目标按运行场景、需求与可观察结果描述，并保留数据、位点、顺序/时间及发送保证的主要边界；packet 接入与结果发布暂列 P1，日志接入与原始包发布暂列 P2。
- 本轮步骤：核对愿景与 Backlog 现状；新增四行 Feature；核对状态、编号唯一性、表格结构及原条目保持；审查 Diff 并记录完成证据。
- 验收命令：Python 核对四个 Feature 的状态、优先级、规格、编号唯一性及原 Backlog 内容保持；核对前序工作台记录和其他文件摘要保持；git diff --name-only；git diff --check。
- 构建边界：本轮仅任务文档修改，无对应 CMake target，不运行构建或 CTest。
- 时间盒：10 分钟，自 2026-10-14 06:46:46 UTC 起。
- 停止条件：四个 Feature 覆盖已认可的运行场景和主要边界，文档与范围核对通过后更新工作台并停止；不自动启动规格或实施。

### 本切片完成证据

- tasks/product_backlog.md 当前工作集新增 kafka-packet-source、kafka-log-source、kafka-packet-sink、kafka-result-sink 四个独立 Feature，分别覆盖远端 packet 接入、结构化日志接入、原始包发布和分析结果发布。
- 四项状态均为 [ ]、规格均为待创建；packet 接入与结果发布暂列 P1，日志接入与原始包发布暂列 P2；目标记录各自运行场景、可观察结果与主要边界，未加入子任务或完整接口契约。
- Python 核对四行表格列数、状态、优先级、规格位置与全池编号唯一性通过；移除四行后 Backlog 与本轮开始时逐字一致，前序工作台任务与证据完整保留。
- 五份其他既有修改文件的 SHA256 与本轮开始时一致，HEAD 与暂存区保持不变；git diff --name-only 范围核对及 git diff --check 通过。
- 本轮仅修改 Backlog 与工作台，未创建规格或实现代码，未运行构建、CTest 或 Kafka 实测，未暂存、提交或推送；完成后停止。

## 上一切片：Baseline 部署服务加载项补齐

- 关联：用户确认补充 build/output/config/deploy-single.yaml 与 deploy-multi.yaml，服务由用户自行启动。
- 业务意图：Baseliner 在 Scheduler 同一进程内取得 Baseline 算法与状态控制服务，消除已复现的 invalid evaluation open。
- Non-Goals：不停止、重启或重新加载运行服务，不执行基线任务，不改算子、算法、Worker、数据库通道配置，不提交或推送。
- 允许文件：tasks/active_task.md、config/deploy-single.yaml、config/deploy-multi.yaml、build/output/config/deploy-single.yaml、build/output/config/deploy-multi.yaml；保留既有 docs/npm.md 与两份 Python 文件。
- 冻结契约：单进程 all.plugins、多进程 scheduler.plugins 各新增一次 libflowsql_baseline.so，与 libflowsql_scheduler.so 同进程；同步源模板以保证 flowsql_runtime_config 构建后保留加载项。
- 本轮步骤：核查运行配置与源模板；新增加载项；运行配置同步 CMake target；核对 YAML、插件归属、源/运行配置一致与部署回归；审查本轮 Diff。
- 验收命令：cmake --build build --target flowsql_runtime_config test_native_deploy_config -j2；ctest --test-dir build -R '^test_native_deploy_config$' --output-on-failure；YAML 插件归属/唯一性与其他配置摘要断言；git diff --name-only；git diff --check。
- 时间盒：10 分钟，自 2026-10-14 04:18:49 UTC 起。
- 停止条件：两种模式的 Baseline 插件归属正确、配置同步与部署回归通过后更新工作台并停止；不启动服务。

### 本切片完成证据

- config/deploy-single.yaml 的 all.plugins、config/deploy-multi.yaml 的 scheduler.plugins 各新增一次 libflowsql_baseline.so，紧邻 libflowsql_scheduler.so 之前；flowsql_runtime_config 已同步到用户指定的两份 build/output/config/ 运行配置。
- cmake --build build --target flowsql_runtime_config test_native_deploy_config -j2 通过；ctest --test-dir build -R '^test_native_deploy_config$' --output-on-failure 通过，1/1 测试成功（0.96 秒）。
- 四份 YAML 解析、Baseline 全部署唯一性、与 Scheduler 同进程及顺序断言通过；每种部署的源模板与运行文件字节一致，build/output/libflowsql_baseline.so 存在。
- build/output/config/flowsql.yml 与 protocols.yml 的 SHA256 保持修改前值；docs/npm.md 的既有 Diff SHA256 保持 387e34c98feac5be31ff77ae6de643d2730bfb5fd80e58b0a3d7797ca1f53489；保留前序 Python 文件。
- 本轮配置 Diff 仅为两份源模板各新增一行，工作台记录本轮证据；未启动、停止、重启或重载服务，未执行基线任务，运行验收由用户启动服务后进行；未暂存、提交或推送，完成后停止。

## 上一切片：链路 CSV 时间戳转换 Python 算子

- 关联：用户要求将 link_data_2_month.csv 的时间戳转换交付为 Python 算子；独立样例交付，不扩展既有 Baseliner Feature。
- 业务意图：原始 CSV 注册为 DataFrame 后，通过 SQL 生成可供 Baseliner 分析的整数时间与单链路身份。
- Non-Goals：不修改 Baseline/Baseliner、Python Worker 或桥接接口，不增加通用日期转换配置，不上传或激活运行中服务，不修改源 CSV，不提交或推送。
- 冻结契约：explore.link_timestamp 继承 OperatorBase，work(pl.DataFrame) -> pl.DataFrame；输入 time 为非空 utf8、格式 %Y/%m/%d %H:%M，按 Asia/Shanghai 解释；保留原列/行序，生成 time_s:int64（Unix 秒）；缺少 link 时添加 link:utf8=link_1，已有 link 保留；缺列、非文本、空时间和无效日期明确失败。
- 允许文件：tasks/active_task.md、samples/python_operator/explore_link_timestamp.py、src/python/tests/test_link_timestamp.py；保留 docs/npm.md 及前序工作台证据。
- 本轮步骤：先锚定时区、原始数据保持、错误输入与整份 CSV 的测试；实现独立可上传算子；验证 Worker 自动发现、Arrow IPC 往返及 SQL 使用方式；审查允许范围 Diff。
- 验收命令：PYTHONPATH=src/python python3 -m unittest discover -s src/python/tests -p test_link_timestamp.py -v；python3 -m py_compile samples/python_operator/explore_link_timestamp.py src/python/tests/test_link_timestamp.py；git diff --name-only；git diff --check。
- 构建边界：本切片只有 Python 样例与 Python 测试，无对应 CMake target，不变更 C++ 构建与测试入口。
- 时间盒：20 分钟，自 2026-10-14 03:53:39 UTC 起。
- 停止条件：全部测试与语法检查通过、整份 CSV 输出核验通过且范围审查完成后更新工作台并停止；不启动服务或执行基线分析。

### 本切片完成证据

- 已新增独立可上传的 explore.link_timestamp；输入与输出契约、导入后 SQL 写在算子文件说明中。
- 实现前 Worker 自动发现测试因算子不存在失败；实现后 unittest 8/8 通过（0.506 秒），包括时区、输入不变、已有链路保留、空表 Schema、错误输入、整份 CSV 和 Arrow IPC 往返。
- 整份 CSV 的 57,761 条时间与 datetime 标准库按 UTC+8 独立转换逐条相等，相邻时间间隔全部为 60 秒；time_s 为 int64，原始 time/Mbps 与行序保持。
- Python 语法编译与 git diff --check 通过；本轮仅两份新增 Python 文件及工作台，docs/npm.md 的既有 Diff 摘要保持 387e34c98feac5be31ff77ae6de643d2730bfb5fd80e58b0a3d7797ca1f53489。
- 用户明确自行导入；未上传、激活或执行在线任务，未暂存、提交或推送，不自动进入下一个切片。

## 上一切片：Basic 字段精简依赖核查

- 关联：用户要求删除 Basic 的 is_final、5 个 interval_* 及终结专用代码，明确保留 3 个 period_*，并要求先分析其他模块依赖与删除影响。
- 业务意图：区分 Basic 指标输出与算子共享核心，明确字段精简对分析模块、统一入库及 Baseliner 的实际影响。
- Non-Goals：本切片不修改代码、Schema、数据库或测试，不移除共享核心的会话生命周期，不实施尚未确定的 Baseliner 差分适配，不提交或推送。
- 允许文件：tasks/active_task.md、docs/npm.md；保留上一轮已交付的未提交字段字典。
- 本轮步骤：核查模块创建与输入分发、Session 独立结果构建、公共结果校验、数据库终态及周期发布、Baseliner 周期增量消费；记录可复核的依赖和实施边界。
- 验收命令：rg 核对源码依赖及字段消费位置；核对文档中的源码链接；git diff --name-only；git diff --check。
- 时间盒：10 分钟，自 2026-10-14 03:30 UTC 起。
- 停止条件：交付依赖分析并明确下游影响后停止；不自动实施跨模块契约变更。Baseliner 是否同步适配待用户产品范围选择。

### 当前切片完成证据

- 已核查生产模块目录、创建流程与输入适配：Session、HTTP/1、TLS、DNS、ICMP 不消费 Basic 结果类型或结果字段；它们复用算子的共享核心，Basic 结果输出可以不启用。
- 已核查 Session 独立构建 is_final/end_reason；连接关闭、超时、五元组复用与 EOF 仍须通过共享核心通知各模块，不能为精简 Basic 输出而删除。
- 已确认公共实体校验当前强制 is_final；统一入库无条件创建 final 关系，且周期发布校验用终态和方向增量识别 metadata-only 行，删除字段需要同步调整这些直接依赖。
- 已确认 5 个 interval_* 是周期增量，有独立于终态的统计用途；Baseliner snapshot/DataFrame 输入、桶聚合及生产示例直接依赖这些字段。累计量不能直接替换增量，否则重复统计；是否同轮交付差分适配待用户选择。
- 已在 docs/npm.md 补充依赖说明，保留当前 31 列 Basic 字典；用户明确保留 3 个 period_*，未实施字段删除。
- 类型依赖扫描、文档源码链接核对及六模块全部字段字典核对通过；本轮仅文档与工作台修改，未构建、运行测试、暂存、提交或推送。

## 上一切片：字段现状整理

- 关联：用户要求首先分析整理 basic/session/http1/tls/dns/icmp 等模块的分析及入库指标字段；本轮为现状分析，不新建实施规格。
- 当前 Atomic Slice：核对六类模块的分族结果 Schema、编码含义与托管入库映射，在现有 NPM 说明中交付完整字段字典。
- 业务意图：让字段优化有可复核的现行字段、类型、单位、有效性和存储类型依据。
- Non-Goals：不修改算子、Schema、数据库表、测试或配置，不实施删字段/改名/改类型，不作性能复测，不提交或推送。
- 允许文件：tasks/active_task.md、docs/npm.md。
- 本轮步骤：读取公共结果契约、各模块编码及生成路径和数据库消费者；按模块整理字段；区分分析字段与存储附加字段；核对重复与派生字段；审查文档 Diff。
- 验收命令：以 Python/正则提取现行 Schema 并核对字段数量、名称、类型及 nullable；核对数据库字段映射；git diff --name-only；git diff --check。
- 时间盒：30 分钟，自 2026-10-14 02:58 UTC 起。
- 停止条件：完整覆盖六模块和两族、分清累计与事件语义及入库附加字段、字段核对和文档 Diff 检查通过后停止；不自动进入优化实施。
- 起点：HEAD 8ecbd1477411aa4090c1d331a4fe776d8fbf367d，工作树与暂存区为空；现有说明为 docs/npm.md。

### 上一切片完成证据

- 已在 docs/npm.md 的“完整分析与入库字段字典”中按 Schema 顺序列出六个模块全部字段、Arrow 类型、nullable、单位及有效条件，同时整理两族地址编码、结果粒度、枚举原因和管理数据。
- 从当前分族 Schema 提取并逐项核对通过：basic 31 列、session 49 列、http1 22 列、tls 25 列、dns 25 列、icmp 27 列；单族合计 179 列定义，IPv4/IPv6 共 12 个实体；字段名称、类型、nullable 与顺序全部一致。
- 核对 Basic/Session 共有 22 个同名同类型字段；记录累计值与周期增量、独立 revision、派生指标、状态有效性及 observed_at 路径差异。
- 核对数据库消费者：分析列全量保存，物理表额外保存 __npm_run_id/__npm_task_id，公开关系关联 __npm_run_status；整理 SQLite/MySQL/PostgreSQL/ClickHouse 类型映射及三类管理表。
- 字段核对脚本、文档源码链接检查、范围检查和 git diff --check 通过；仅修改 docs/npm.md 与本工作台，暂存区为空、HEAD 未变。
- 本轮仅源码分析与文档整理，无代码或 Schema 修改，未运行构建、CTest 或数据库/性能实测；未暂存、提交或推送，不自动进入优化实施。
