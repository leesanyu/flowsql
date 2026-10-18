# 即时工作台

WIP=0；2026-10-18 Asia/Shanghai。

## 最近完成：Web 同步任务等待 30 秒 / 本地提交 Atomic Slice

- 业务意图/授权：用户明确要求“提交代码”，将已验收的Web同步执行30秒等待及本会话诊断、主构建记录形成可审查的本地提交。
- Non-Goals：不修改业务源码或启动脚本，不重跑源码未变化的编译/行为测试，不重启服务或重复SQL，不提交构建产物、evidence、私有配置，不推送。
- 允许文件：本轮仅编辑tasks/active_task.md；精确暂存src/services/web/web_server.cpp、tasks/active_task.md两个文件。现有任务正文逐字保留，基线在evidence/web-sync-timeout-30s/commit/，不入库。
- 已核查/契约：HEAD=5939c06，暂存区空；源码及主库SHA与主构建和5/5行为验收记录一致，既有定向CTest 1/1与前端构建/15文件同步成功；修改行clang-format及git diff --check通过。
- 步骤/验收：复核本次两文件diff和原验收；精确git add后核对暂存路径及blob、git diff --cached --check；本地git commit；核验父提交、提交树与两文件摘要、空index及工作区。Git元数据受限时按标准审批处理，不绕过拒绝。
- 依赖/位置：仓库根目录，本机Git/Python/clang-format，无外部服务或网络依赖；仅Git索引/提交所需权限。源码/主库/原工作台正文摘要保持。
- 时间盒：23:54–00:04 Asia/Shanghai，10分钟；停止：本地提交与后置审计成功后WIP=0；若权限拒绝则记录实际状态并停止，不扩大修改范围。

### 本地提交验收记录

- 源码/主库与既有成功编译及5/5行为记录一致，新增行clang-format、工作区/暂存diff检查通过；既有定向CTest 1/1、前端构建和15文件同步证据已复核，本轮没有修改源码或重跑无变化的测试。
- 普通git add被只读Git元数据限制拒绝；标准审批已允许精确暂存两个文件。暂存路径及blob与允许清单、当前文件一致，原工作台正文逐字保留，无构建产物、evidence或私有配置。
- 本记录随本次本地提交落库，提交号见Git历史；仅提交上述两个文件，不推送。提交树/父提交、空index及工作区的后置核验写入evidence/web-sync-timeout-30s/commit/，不入库。

## 最近完成：启动前端构建 uv_cwd 诊断 / Atomic Slice 1

- 业务意图：用户执行start.sh --build-frontend时npm在process.cwd报ENOENT；核实构建环境并从有效绝对目录完成前端构建和静态同步，使用户可继续启动服务。
- 已核查事实：start.sh直接从调用者工作目录执行npm run build --prefix；该参数不代替npm启动时读取process.cwd。当前根目录Node 25.6.1/npm 11.9.0读取cwd及脚本列表均成功；flowSQL/flowsql为同一目录inode，未复现用户终端的失效cwd状态。
- Non-Goals：不改start.sh、Node/npm版本、业务源码或公共契约，不安装依赖、不删移目录，不停止/启动服务、不重复业务SQL，不提交推送。用户终端的实际失效目录无法从本执行进程直接验证，不将推断写成已复现。
- 允许文件：tasks/active_task.md；生成产物仅src/frontend/dist/、build/output/static/，本地证据仅evidence/start-frontend-cwd/。保留既有Web源码/主库、HEAD/index及原工作台正文。
- 契约与验收：在/mnt/d/working/flowSQL/src/frontend运行npm run build，退出0后用cmake -E copy_directory同步到build/output/static，核验dist/index.html引用的本地资源存在且与目标文件SHA一致；bash -n start.sh、git diff --check与范围/摘要审计。无需修改接口或新增测试。
- 依赖/执行位置：本机Node/npm/Vite与既有node_modules，已核实的绝对工作目录；无外部服务、网络下载、数据库或Kafka依赖。
- 时间盒：23:28–23:38 Asia/Shanghai，10分钟；停止：构建/同步及审计完成后WIP=0，提供用户终端重新进入目录的恢复命令，不自动运行完整start.sh。

### 诊断与构建完成证据

- 状态：已完成，WIP=0。用户堆栈停在npm配置构造的process.cwd；已核对当前安装的@npmcli/config/lib/index.js第79行默认参数确实读取process.cwd，尚未进入Vite。用户终端的具体目录为何失效未直接复现，不推断为目录已删除或Node版本问题。
- 从绝对目录/mnt/d/working/flowSQL/src/frontend执行npm run build退出0：Node v25.6.1/npm 11.9.0/Vite 7.3.1，1,532模块，构建34.41秒；仅有既存的大chunk体积提示，不是本次ENOENT失败。日志evidence/start-frontend-cwd/frontend-build.log。
- 使用cmake -E copy_directory同步src/frontend/dist到build/output/static退出0，未移除目标目录；15个生成文件SHA逐个一致，index.html引用的3个本地资源均存在且摘要一致。明细build-audit.json，不入库。
- start.sh语法检查、范围/diff及HEAD/index/原工作台审计通过；启动脚本、既有Web源码与主库摘要保持。本轮未运行完整start.sh、未启动/停止服务或重复业务SQL。恢复命令为先cd /再cd /mnt/d/working/flowSQL，随后./start.sh；如需再次构建可保留--build-frontend。

## 最近完成：Web 同步任务等待 30 秒 / Atomic Slice 2（主构建产物更新）

- 授权/业务意图：用户反馈重启仍失败并要求核实编译；将已验收源码真正编译到默认启动加载的build/output/libflowsql_web.so，使下次启动加载30秒版本。
- 已核查事实：主Web库修改时间仍为2026-10-15，摘要2c9e291c；仅隔离库为2026-10-18/7037bc83。最新任务59实际11,043ms成功，仍超过旧Web的10秒等待；本机Web健康接口正常。
- Non-Goals：不改业务代码、算法、公共契约或超时值，不重复用户SQL、不停止服务/当前测试、不改数据，不提交推送。
- 允许文件：tasks/active_task.md；输出仅主CMake flowsql_web target相关产物及evidence/web-sync-timeout-30s/main-build/证据，保持现有web_server.cpp改动不再编辑。
- 契约/验收：cmake --build build --target flowsql_web -j2；确认主库摘要/时间变化，链接本次主产物的真实Web路由模拟12秒成功/32秒响应30秒超时等5项；git diff --check、范围及HEAD/index/原工作台正文保持。运行服务是否加载新版独立核验，不能把磁盘编译等同于已加载。
- 依赖/位置：仓库根目录、既有CMake缓存和第三方、本机动态端口模拟上游，无业务数据库/Kafka依赖；只读访问健康接口和任务元数据。若进程视图不可用如实报告，不绕过审批。
- 时间盒：23:17–23:37 Asia/Shanghai，20分钟；停止：主库编译/行为核验及后置审计通过后WIP=0，明确当前服务是否已重载；不扩展修改或主动停止正在运行的测试。

### 主构建验收证据

- 状态：已完成，23:21关闭，WIP=0；本轮未再修改业务源码。
- cmake --build build --target flowsql_web -j2退出0，实际重新编译web_server.cpp并链接主库；build/output/libflowsql_web.so修改时间为2026-10-18 23:17:30 +08:00，SHA256由2c9e291c变为30f38590c2c42644f2551da39da3867f9940e5b17a85170f1541813031fece13。
- 独立探针通过RAII动态加载上述主库、枚举真实Web路由并连接本机随机端口模拟上游；5项选中/执行/通过，失败/跳过0：同步12秒响应在12,004ms成功、32秒响应在30,013ms超时、控制接口在10,010ms超时，异步提交及503错误透传均通过。
- 普通沙箱首次探针因不允许监听导致绑定断言失败（退出134）；按标准审批获准本机监听后重跑退出0，未改变断言或等待门槛。主构建及主库行为日志、摘要和审计位于evidence/web-sync-timeout-30s/main-build/，不入库。
- HEAD/index、既有Web源码及原工作台正文保持，git diff --check及两文件范围检查通过。未重复业务SQL、重启服务、停止当前测试、提交或推送；上述探针不代表运行服务已加载新库或业务SQL端到端复测。
- 宿主进程只读核验被自动审批拒绝，理由为审批执行环境禁止提权；普通进程视图不可见主服务，因此当前服务装载版本未验证。健康接口先前返回ok，但不能据此确认版本；需在此次主库编译完成后重新启动服务才会加载新库。

## 最近完成：Web 同步任务等待 30 秒 / Atomic Slice 1

- 业务意图/授权：用户明确要求“将 WEB 同步任务的超时时间上调到 30 s”；修复同步执行超过10秒而页面误报service unreachable、后台仍成功的现象。
- 已核查事实：Web ProxyPostJson读取超时10秒，任务执行接口复用该函数；任务58/56分别completed/57,761行，诊断耗时10,370ms/11,286ms，超过Web等待窗口。
- Non-Goals：不改Baseliner、Task/Gateway/前端、任务执行超时或错误协议；不重复业务SQL，不重启服务，不覆盖正在运行测试使用的build/output产物，不提交推送。
- 允许文件：tasks/active_task.md、src/services/web/web_server.cpp；证据仅evidence/web-sync-timeout-30s/，隔离产物仅build/web-sync-timeout-30s/，均不入库。
- 契约：/tasks/batch/execute的Web代理读取等待为30秒，其余ProxyPostJson调用保持10秒，连接等待5秒不变；成功/失败响应和任务状态不变。
- 实现/验收：内部代理增加默认10秒的参数，任务执行调用显式30秒；cmake -B build/web-sync-timeout-30s src；cmake --build build/web-sync-timeout-30s --target flowsql_web test_pcap_upload_contract -j2；隔离Web路由+动态端口模拟12秒成功、32秒响应触发30秒超时、控制接口仍10秒、错误透传；ctest --test-dir build/web-sync-timeout-30s -R '^test_pcap_upload_contract$' --output-on-failure；本轮修改行格式、git diff --check及范围/SHA审计。
- 依赖/位置：仓库根目录，本机CMake/C++17、已有第三方共享缓存和httplib/SQLite；模拟上游绑定127.0.0.1随机端口、内存数据库，无业务服务/数据库/Kafka依赖。按标准审批开放本机监听，不修改系统配置；不干扰正在运行的用户测试。
- 时间盒：22:01–22:21 Asia/Shanghai，20分钟。停止：编译与定向行为验收通过后WIP=0，记录运行服务尚未重启；失败在本切片修复，到期保留明确证据。前项Docker/Kafka剩余改动已由当前HEAD 5939c06提交，工作区起始干净，原工作台正文逐字保留。

### 完成证据

- 状态：已完成，WIP=0；代码仅改Web内部代理的等待参数及任务执行调用，未改响应协议、其他接口等待或任务执行策略。
- 隔离CMake配置、flowsql_web与test_pcap_upload_contract均构建通过；ctest定向1选中/执行/通过、0失败/跳过，使用同一运行时测试保护。
- 真实Web handler+模拟上游：旧版12秒响应在10,009ms超时；新版12秒响应在12,002ms成功，32秒响应在30,030ms超时，控制接口仍10,006ms超时；异步提交及上游503错误透传通过，总计5/5。
- 本轮修改行clang-format、git diff --check与两文件范围审计通过；HEAD/index和原工作台正文保持，主build/output/libflowsql_web.so摘要保持。源码、隔离产物、计时原始日志及审计保存在evidence/web-sync-timeout-30s/，不入库。
- 本机监听测试的权限等待超出原时间盒，仅在已冻结验收检查点收口，未扩大修改或测试范围。未覆盖主产物、重启服务、重复业务SQL、提交或推送；正在运行的服务仍需之后重建/加载新版Web插件才会生效。

## 最近完成：Docker 发布修复与 Kafka 手测指导 / 本地提交 Atomic Slice

- 授权/业务意图：用户明确要求“提交代码”；将本会话已完成的 Docker 修复、对应回归和 Kafka 手测帮助文档形成可审查的本地提交。
- Non-Goals：不改业务代码或公共契约，不重跑无变化的完整回归，不推送或发布镜像，不操作已有容器/卷，不提交 evidence、秘密及其他 Feature 内容。
- 允许文件：本轮仅编辑 tasks/active_task.md；精确暂存 .dockerignore、Dockerfile、docker-compose.yml、README.md、docs/kafka.md、docs/netadapter.md、docs/testing.md、src/channels/netadapter/CMakeLists.txt、src/tests/test_framework/CMakeLists.txt、src/tests/test_framework/test_docker_deploy_config.cpp、src/tests/test_framework/test_docker_build_context.py、tasks/active_task.md，共 12 路径。
- 初始状态/契约：HEAD=d9c1298，暂存区为空；当前 12 路径均属于本会话，工作台现有 Baseliner 已提交正文逐字保留。Kafka 文档摘要与已验收结果一致；基线及审计缓存于 evidence/docker-release-commit/，不入 Git。
- 步骤：复核本次 diff/原验收证据；执行部署配置与上下文定向检查、三套 Compose 解析、格式及文档静态检查；精确暂存并核对路径/blob；本地提交后核对提交树、空暂存区和工作区。
- 验收命令：tools/test.sh unit --filter '^(test_native_deploy_config|test_docker_deploy_config|test_docker_build_context)$' --jobs 2；三套 docker compose config --quiet；clang-format --dry-run --Werror src/tests/test_framework/test_docker_deploy_config.cpp；文档 JSON/Bash/链接与前端配置往返校验；git diff --check、git diff --cached --check、git show --stat/--name-only、git diff --cached --quiet，暂存 blob 与当前文件精确相等。
- 依赖/执行位置：仓库根目录的 Git、Python3/Docker SDK 7.1.0、Node、Bash、clang-format、Compose CLI 及现有构建产物；所选本地契约测试不访问 Engine、broker、数据库或真实网卡。完整新镜像 build/run 和真实收包仍属此前未验证项，不以本轮检查替代。
- 时间盒：21:52–22:12 Asia/Shanghai，20 分钟；用户再次明确要求“直接提交”，当前权限规则已更新，沿用原 12 路径和验收边界继续。停止条件：本地提交及后置审计通过后 WIP=0；若 Git 元数据权限拒绝，按标准审批机制处理，不绕过权限限制。

### 本地提交检查

- 本轮定向回归选中/执行/通过 3，失败/跳过/未执行 0；summary 为 evidence/test-environment/20261018T134535Z-5a9bceb4/summary.json。三套 Compose 解析、C++ 格式和 diff 检查通过。
- Kafka 指导复核通过：32 个代码块、9 个 JSON 和 15 个 Bash 示例、22 个本地链接/锚点、42 个 UI 标签及连接/source/sink 配置往返与异步 SQL。README/文档与原验收摘要相同，原工作台记录逐字保留。
- 前轮权限记录：普通 git add 无法创建 .git/index.lock（Read-only file system），标准提权被旧审批策略拒绝，未改变 HEAD/index；当前按更新后的标准权限机制继续，不绕过拒绝。
- 本地提交验收：更新后的标准权限机制已批准 Git 元数据写入；HEAD、空暂存区、11 个已验收文件及原工作台正文均与基线一致后，精确暂存上述 12 路径。本记录随本次本地提交落库，提交号见 Git 日志；不推送。
- 完成出口：暂存路径/blob、diff 及提交后树/index/工作区审计缓存在 evidence/docker-release-commit/，不入库；保留所有原工作台正文，本切片关闭后 WIP=0。
- 沿用原修复的构建和 10/10 定向回归记录；本次没有修改程序或重跑完整回归。完整新镜像 build/run、跨容器业务链路和真实收包仍未验证，不能据此宣称已通过 Docker 运行验收。

## 最近完成：baseliner-output-throughput / 本地提交 Atomic Slice

- 授权/业务意图：用户明确要求“直接提交”；将已完成验收的Baseliner批量输出Feature形成一次可审查的本地提交。
- Non-Goals：不修改业务代码、不重跑无变化的完整回归、不推送，不提交Docker/Kafka等无关修改、evidence或私有配置。
- 允许文件：本轮仅编辑tasks/active_task.md；暂存docs/baseliner.md、src/operators/baseliner/{baseliner_operator.cpp,result_codec.cpp,result_batch.h}、src/tests/test_baseliner_contract/{CMakeLists.txt,test_baseliner_operator.cpp,baseliner_batch_fixture.h,test_baseliner_output_batching.cpp,test_baseliner_output_cost.cpp}、tasks/{active_task.md,product_backlog.md,archive/feat-baseliner-output-throughput.md}共12路径。
- 契约/步骤：复核HEAD、空暂存区、验收摘要和原修改；精确暂存11个完整文件，工作台按内容排除其他任务记录；核对暂存路径和blob、格式/diff，创建本地提交；核对提交树、空index和原无关内容摘要。
- 验收：git diff --cached --check；暂存12路径和blob精确相等；clang-format --dry-run --Werror；git show --stat/--name-only HEAD；git diff --cached --quiet；无关文件及工作台原记录SHA保持。
- 依赖/执行位置：本机Git/Python/clang-format及当前会话已完成的验收证据，无外部服务/网络依赖。上两次Git写入被环境拒绝，未改变HEAD/index；本轮权限规则已更新，按标准提权机制请求必要的Git元数据写权限。
- 时间盒：21:38–21:58 Asia/Shanghai，20分钟。停止：提交和后置审计通过后WIP=0；若权限仍拒绝则记录证据并停止，不绕过拒绝。

### 本地提交验收

- Git元数据写权限按更新后的标准审批机制获准；940个源码文件、193个正式产物与已验收all摘要一致，全量all为164选中/执行/通过、失败/跳过/未执行为0；本Feature格式及暂存diff检查通过。
- 本记录随本次本地提交落库，提交号见Git日志。只纳入12个Feature路径，工作台按内容排除其他任务记录；原无关文件/记录摘要保持，evidence与私有配置不入库，不推送。
- 暂存路径/blob、提交树、空index及无关内容的前后审计位于evidence/baseliner-output-throughput/implementation/commit/，仅本地缓存；本切片关闭，WIP=0。

## 当前完成：baseliner-output-throughput / T4 / Atomic Slice 5（Feature 已完成）

- 业务意图/授权：性能全部门槛已通过，交付可复核的用户性能说明并完成Feature全量DoD，确认本次输出优化与既有系统回归兼容。
- Non-Goals：不改其他Feature/业务数据/服务/系统网络/凭证，不调超时或放宽断言，不提交推送；不接管默认归属不明旧卷。
- 允许文件：tasks/active_task.md、tasks/specs/feat-baseliner-output-throughput.md、tasks/archive/feat-baseliner-output-throughput.md（全DoD通过后文件归档）、tasks/product_backlog.md（仅本Feature）；docs/baseliner.md（现有性能章节）；本地证据implementation/和标准测试证据。当前无生产修改计划。
- 步骤/验收：cmake -B build src；cmake --build build -j4，真正完成后单独 tools/test.sh all --jobs 4（无filter），使用T3独立project的统一配置副本与FLOWSQL_TEST_STATE；核对selected=executed=passed、failed/skipped/not_executed=0及源码/产物一致。clang-format --dry-run --Werror（本Feature C++）、git diff --check、无关文件/暂存区SHA、一轮有界diff审查。
- 用户说明契约：测量日期、机器、同默认flags、完整SQL口径、预热1+五轮中位/范围；增长曲线、五类负载（Relation+2.84%）、分段/暂存/CPU/RSS与比较内存、适用限制和可复跑命令，不依赖evidence才能使用。
- 依赖/位置：标准入口默认docker（本机测试native）；配置副本implementation/test-environment.json仅project有别，FLOWSQL_TEST_STATE=/tmp/flowsql-baseliner-20261018；声明的数据库、Kafka/身份及部署矩阵、Docker socket和真实认证/测试资源读写权限。按用户续作授权标准夹具准备隔离资源；不删除用户容器/卷、不改证书/网络。
- 时间盒：21:00–21:30 Asia/Shanghai，30分钟。停止：完整DoD满足则T4勾选、规格文件归档、Backlog完成、WIP=0；依赖失败或到期记录明确阻塞/完整样本检查点，未验证不标完成，后续沿用T4。

### Feature 最终出口（21:20关闭）

- 状态：已完成；T1～T4全部验收并勾选，规格文件已归档至tasks/archive/feat-baseliner-output-throughput.md，Backlog[x]，WIP=0，本切片停止。
- 全量配置/构建退出0；无筛选all=164选中/164执行/164通过，失败/跳过/未执行=0，summary=20261018T130312Z-e7fccdc4，源码与产物摘要前后一致。
- 同配置完整SQL五轮中位179.490→4.922秒、降低97.26%；增长比2.045/1.674；生产编码+追加173.801→0.705秒、降低99.59%；16批/每批≤4096/57,761行/1模型、所有列精确相等；五代表负载退化均≤10%（Relation+2.84%）。
- 本Feature格式/diff、T4一轮有界审查、无关文件/暂存区SHA全部通过；此前本机20/20、外部17/17、Sanitizer11/11与当前未改生产实现对应。
- 现有docs/baseliner.md更新本次性能/口径/边界和独立复跑入口；本地所有原始日志/完整样本/摘要在implementation/，未入库。
- 未提交、未推送、未重启用户服务、未覆盖用户通道；旧卷保持，测试专用受管资源保留供复跑。任务无遗留验收项。

## 当前完成：baseliner-output-throughput / T4 / Atomic Slice 4

- 业务意图/授权：完成真实生产读取/算子/追加分段成本，证明编码与追加热点已消除并量化真实批次和暂存资源。
- Non-Goals：不改正式源码/产物/参数/结果参照；不并发构建/回归/其他基准；不操作用户服务/输出、不提交推送。
- 允许文件：tasks/active_task.md、tasks/specs/feat-baseliner-output-throughput.md；本地证据implementation/。无代码修改。
- 契约/验收：run_bench.py production；真实DataFrameReader/聚合→真实.so→原DataFrameChannel::Append，旧新诊断副本各预热1+正式5轮；codec仅加计时/内存观测，与已完成未插桩SQL独立报告。编码+追加中位降低≥90%；全部30列/模型等价；新≤16非空批、每批≤4096。记录rows/batches/Arrow字节、单回调暂存峰值、墙钟/CPU/RSS与比较参照内存，前后SHA一致。
- 依赖/位置：native默认flags、独立诊断DSO/旧IPC、仓库CSV、Arrow/Baseline/私有DataFrame；无外部服务/特殊权限。
- 时间盒：20:40–21:10 Asia/Shanghai，30分钟。停止：五轮与门槛通过后关闭切片再载全量DoD；到期保留完整样本检查点沿用T4，未完成不勾选Feature。

### T4 Slice 4 完成（20:59关闭）

- 真实生产读取/聚合/算子/追加诊断各预热1+正式5轮，编码+追加中位173.800553827→0.704984231秒，降低99.5944%；总分段中位180.046249561→3.386846059秒（与未插桩Scheduler SQL独立报告）。
- 编码中位5.764383538→0.603865955秒，追加168.036170289→0.105044876秒；结果57,761行/1模型精确一致。
- 新16批、每批≤4096，Arrow批次字节53,084,468，单回调暂存峰值4,425,600；旧57,761批。比较前RSS旧中位313340KiB、新189516KiB；参照53,117,628字节，比较后RSS另记。
- implementation/t4-production/{samples,summary,before,after}.json、t4-production-gates.json及日志；SHA保持。性能全部门槛满足，T4仍待完整构建/all和用户说明；当前切片关闭，另载DoD。

## 当前完成：baseliner-output-throughput / T4 / Atomic Slice 3

- 意图/授权：补齐五种固定代表负载的真实算子与DataFrame追加成本，保证小样本、多序列/指标、Ratio、Relation和forecast不因优化退化。
- Non-Goals：不改源码/产物/参数/夹具，不扩展负载，不并发其他基准/构建/回归，不操作用户服务/结果，不提交推送。
- 允许文件：tasks/active_task.md、tasks/specs/feat-baseliner-output-throughput.md；本地证据implementation/。无代码修改。
- 契约/验收：run_bench.py representative；small/value/ratio/relation/forecast各预热1+新旧各5轮；真实.so逐页回调+实际DataFrame追加，全部结果/fusion/模型逐列等价；各新中位≤旧中位1.1倍。CPU/RSS、比较内存及Arrow字节记录，输入/库/可执行前后SHA一致。
- 依赖/位置：native默认构建；固定T1夹具/旧库/参照，Arrow/Baseline/私有DataFrame；无外部依赖/特殊权限。
- 时间盒：20:22–20:52 Asia/Shanghai，30分钟。
- 停止：五类门槛完成即关闭切片；到期保留完整样本检查点沿用T4，不扩大范围。之后另载生产分段成本，未完整DoD不勾选Feature。

### T4 Slice 3 完成（20:39关闭）

- 五类新旧各预热1+正式5轮，全部结果/fusion/最终模型精确一致，测量产物和输入前后SHA一致。
- 旧→新中位秒：small 0.031488029→0.022028197；value 39.30166343→1.400782519；ratio 9.550301406→0.734068482；relation 52.545022159→54.037384187；forecast 1.988433899→0.320501585。
- Relation增加2.8402%，未超过10%；其余均降低，五类退化门槛通过。evidence implementation/t4-representative/及t4-representative-gates.json。
- 当前切片关闭，T4仍待生产编码/追加五轮、完整构建/all及用户说明；另行冻结分段成本切片。

## 当前完成：baseliner-output-throughput / T4 / Atomic Slice 2

- 业务意图/授权：顺序完成正式未插桩SQL及增长曲线的五轮成本，量化完整发布路径的秒级收益。
- Non-Goals：不改源码/正式产物、编译模式/参数、输入/模型、旧IPC；不并发构建/测试/其他基准，不操作用户输出，不提交推送。
- 允许文件：tasks/active_task.md、tasks/specs/feat-baseliner-output-throughput.md；本地证据 implementation/。无代码修改。
- 契约/步骤：运行已冻结 run_bench.py sql-full，再按剩余时间运行 sql-prefixes；各负载新旧预热1次+正式5轮，同机单并发，真实Scheduler handler至completed含发布，每轮新cold私有输出；全部列/模型与旧参照精确比较。CPU/RSS和比较内存单独记录。
- 验收：sql-full 中位数≤30秒且降低≥80%；前缀16,384→32,768→57,761中位比≤2.5/≤2.2；样本日志、源/产物前后SHA一致、git diff --check/name-only。未完成的前缀沿用T4下轮，不降门槛。
- 依赖/位置：native，默认无O2/O3 flags；既有真实插件、私有DataFrame/SQLite、仓库CSV和固定IPC，无外部依赖/特殊权限。
- 时间盒：19:51–20:21 Asia/Shanghai，30分钟。停止：本轮负载完成或时间盒届满在完整样本边界保存检查点；不混入代表负载或完整回归；达到检查点后更新并关闭，再冻结下轮。

### T4 Slice 2 完成（20:21 关闭）

- 完整SQL及前缀门槛全部通过，未插桩样本均预热1+新旧各5轮；等价/输入与正式产物SHA保持。
- 新16,384/32,768/57,761行中位1.437938131/2.940406057/4.922357173秒；增长比2.044876614/1.674039938，低于2.5/2.2。旧前缀中位16.904871845/67.621353272秒。
- 证据 implementation/t4-sql-full、t4-sql-prefixes、t4-sql-gates.json；T4代表负载/分段五轮及完整DoD仍待完成。当前边界停止，随后另载代表负载切片。

### T4 正式完整 SQL 阶段结果（当前切片仍进行）

- 同机同默认参数、预热1+各5轮：旧中位179.490327915秒[169.888599234,182.915171197]；新4.922357173秒[4.684642412,5.172186598]；降低97.2576%，约36.46倍，两项完整SQL门槛通过。
- 正式十轮与预热两轮全部30列及最终模型精确相等。比较前峰值RSS旧中位297624KiB、新194516KiB；输入/库/成本可执行前后SHA一致。
- 证据：implementation/t4-sql-full/{samples,summary,before,after}.json及全部原始日志，不入库。T4其余门槛仍待验收；继续当前切片的前缀比较。

## 当前检查点：baseliner-output-throughput / T4 / Atomic Slice 1（已完成）

- 业务意图/授权：用户授权顺序实施至完成；T3 已整体验收，补齐真实生产读取/算子/追加的成本观测与正式可复跑比较入口，使收益可量化核验。
- Non-Goals：不改生产算法/输出/ABI/配置或编译优化级别、不改共享 DataFrame；不并发跑基准/构建/回归、不覆盖旧参照、不操作用户结果/服务，不提交推送。
- 允许文件：tasks/active_task.md、tasks/specs/feat-baseliner-output-throughput.md；src/tests/test_baseliner_contract/test_baseliner_output_cost.cpp（仅测试观测）；本地证据 evidence/baseliner-output-throughput/implementation/。现阶段不改 docs。
- 观测契约：--production 使用真实 DataFrame input、真实 .so、原顺序 ProcessBlock/Flush、实际 DataFrameChannel::Append；墙钟计时不含参照载入，记录主结果批次/行/字节、追加、CPU/RSS及参照大小。编码分段仅用本地源码副本插桩 DSO，与未插桩 SQL 独立报告；新暂存峰值取真实 ResultBatchEncoder::Stats，旧插桩 ProxyMemoryPool 统计 encoder 同时容量，均不影响正式库。
- 步骤：补生产成本入口与计时/内存口径；在 evidence 构建同 flags 的旧/新 codec 诊断副本；重建测试 target，运行成本 smoke 和完整样本新路径等价，冻结批量测量脚本。
- 验收：cmake --build build --target test_baseliner_output_cost -j4（完成后才跑测试）；标准 unit --filter '^test_baseliner_output_cost$'；真实生产小样本/全样本与旧 IPC compare，分段观测自检；clang-format --dry-run --Werror、git diff --check/name-only。
- 依赖：native 本机 Arrow/Baseline/Scheduler/SQLite，仓库 CSV，私有测试输出及旧库/参照；无外部服务或特殊权限。默认构建参数不变。
- 时间盒：19:44–20:14 Asia/Shanghai，30 分钟。
- 停止条件：观测/入口和检查通过后关闭本切片，另行冻结 T4 五轮成本运行；到期保留明确检查点，沿用 T4；未满足全部量化门槛和完整 DoD 前不勾选/归档。

### T4 观测入口检查点

- 当前切片已完成；标准成本 smoke 1/1（20261018T114845Z-2c348eb3），零跳过，源码/产物摘要保持。
- 新真实生产全样本自检：57,761行/16批/1模型，所有字段与旧IPC精确等价；3.183秒，编码0.556秒、追加0.092秒，单回调暂存峰值4,425,600字节；不是五轮验收。
- 新旧本地诊断DSO保持原编译flags，只增加codec计时/容量统计；与未插桩SQL分开。旧统计池初次拥有期错误导致诊断副本崩溃，延长至完整测量进程后32行自检通过；失败日志保留，未计正式样本，正式生产库SHA不变。
- 真实成本入口统计比较前CPU/RSS、比较后的RSS及单独参照字节；分段不包含参照载入。测量脚本冻结预热1+正式5轮、顺序执行、每轮cold私有输出、全部列/模型精确比较。
- 格式/diff检查通过。T4仍未整体验收；按连续授权另载正式成本切片。

## 当前完成：baseliner-output-throughput / T3 / Atomic Slice 2

- 授权/业务意图：用户在数据库权限阻塞说明后明确回复“继续实施”；继续统一测试配置连接及隔离夹具验证，完成批量输出的发布、恢复与生命周期保证。
- Non-Goals：不改算法、ABI、SQL/Schema、测试凭证/地址、系统网络或用户服务/容器/业务数据；不提交推送；本切片不提前开始 T4。
- 允许文件：tasks/active_task.md、tasks/specs/feat-baseliner-output-throughput.md；本地证据 evidence/baseliner-output-throughput/implementation/ 及标准测试证据目录。当前无生产代码修改计划。
- 契约：沿用 T3 已冻结的分析、发布、恢复与资源保证；已有 20/20 本机和 11/11 Sanitizer 不重复冒充新验收，补齐真实外部数据库矩阵。
- 环境隔离补充：默认旧卷缺少受管归属且私有状态缺失，不接管、不删除；从统一配置复制非秘密运行副本，仅 project 改为 flowsql_baseliner_20261018，FLOWSQL_TEST_STATE=/tmp/flowsql-baseliner-20261018；其余字段保持，标准入口生成新测试身份。配置副本位于允许的 implementation/ 证据目录，供 T3/T4 同源复用。
- 步骤/验收：tools/test.sh doctor --filter '^(test_baseliner_.*|test_scheduler.*)$'；通过后 tools/test.sh integration --filter '^(test_baseliner_.*|test_scheduler.*)$' --jobs 4；核对 selected/executed/passed 及零跳过，源码/产物与无关文件摘要；git diff --check/name-only。
- 依赖/执行位置：标准入口默认 docker（纯本机测试仍在 native）；config/testing/environment.json 和 dependencies.json 声明的 MySQL/Postgres/ClickHouse，既有隔离夹具与资源锁；真实 socket 及测试专用资源读写权限，必要时按新增授权申请扩大执行权限；不输出秘密、不重置服务或卷。
- 时间盒：19:33–20:03 Asia/Shanghai，30 分钟。
- 停止条件：外部矩阵通过则补齐 T3 完成证据、勾选并关闭本切片；失败或到期记录明确错误/检查点，不扩大文件范围。T3 完成后依用户连续实施授权另行冻结 T4。

### T3 整体验收完成

- 状态：已完成，19:40 关闭当前切片并勾选 T3；T4 尚未开始。
- 默认旧项目因卷归属不明拒绝复用，未接管/修改/删除旧资源。独立项目沿用同一配置除 project 外的全部字段，标准入口创建测试身份。
- 外部矩阵：20261018T113455Z-c5fe4fb2，selected/executed/passed=17，failed/skipped/not_executed=0；8 项环境夹具、8 项业务矩阵与1项 workspace。源码/产物前后摘要一致。
- 合并验收：此前重建后本机 20/20、ASan/UBSan/LSan 11/11、真实CSV结构/结果等价通过；外部发布/恢复/托管/时间维护已补齐，满足 T3。
- 无关文件和暂存区摘要保持，git diff --check 通过；本轮无生产代码修改，未提交推送。证据 t3-integration-isolated.log、t3-resume-scope.json 及上述标准 summary。

## 前次检查点：baseliner-output-throughput / T3 / Atomic Slice 1（被明确问题阻塞）

- 意图/授权：按顺序覆盖 Value/Ratio/Relation、预测与发布/失败生命周期，使批量输出保留既有状态和资源保证。
- Non-Goals：不扩展算法、出口事务或公开接口；不改其他 Feature、不改测试凭证/地址、不管理用户服务/容器、不提交推送。
- 允许文件：tasks/active_task.md、tasks/specs/feat-baseliner-output-throughput.md；src/tests/test_baseliner_contract/baseliner_batch_fixture.h、test_baseliner_output_batching.cpp、test_baseliner_operator.cpp、CMakeLists.txt；生产缺陷修复仅 src/operators/baseliner/result_batch.h、result_codec.cpp、baseliner_operator.cpp；本地证据 implementation/。
- 冻结契约：三类结果、forecast/fusion 顺序、checkpoint/模型精确等价；取消或无效编码在已暂存结果后失败仍空 outputs；Arrow buffers 随产物拥有 pool，task/插件先后释放安全。
- 步骤：补三类回调/无效编码测试和测试内可控 Submit 取消门；依赖预检后执行完整 Baseliner/Scheduler 定向回归；独立 CMake sanitizer 构建复用依赖缓存运行资源与生命周期断言。
- 验收：cmake --build build --target test_baseliner_output_batching test_baseliner_operator -j4；tools/test.sh doctor --help 并按所选依赖预检；tools/test.sh unit/integration --filter '^(test_baseliner_.*|test_scheduler.*)$'；ASan/UBSan/LSan 新测试及 SQLite/任务/拥有期覆盖；clang-format/diff/SHA 审计。外部受限部分诚实未验证，不以局部通过勾选 T3。
- 依赖：本机 Arrow/Baseline/SQLite；外部矩阵需要 config/testing/environment.json 所声明 MySQL/Postgres/ClickHouse/Kafka（只准备选中依赖）、真实执行权限；标准入口/隔离资源锁，不更改系统环境。Sanitizer 使用独立 build 缓存及原依赖，不影响正式成本产物。
- 时间盒：18:37–19:07 Asia/Shanghai，30 分钟。停止：全部 T3 验收满足则勾选并另载 T4；环境阻塞或到期记明证据，沿用 T3 不提前开始后续任务。

### T3 检查点与停止证据

- 状态：被明确问题阻塞；本机部分已完成，当前切片停止，T3 未验收、T4 未开始、Feature 仍进行中。恢复时沿用 T3，先冻结后续切片，不跳过外部矩阵。
- 构建：全部相关 Baseliner/Scheduler targets 重建通过；独立 CMake Sanitizer 所选目标完成，复用原第三方缓存，正式编译参数保持。
- 最终本机回归：标准 unit 20/20，failed/skipped/not_executed=0，summary=20261018T110150Z-7a14b3b9；源码/正式产物前后摘要一致。
- 资源安全：ASan/UBSan/LSan 10 项业务测试+1 项夹具共 11/11，失败/跳过=0；覆盖新五组批量断言、既有算子/模型/资源安全。Arrow、外部客户端库未插桩。纯本机扩大权限执行获批。
- 真实输入结构：用户转换 CSV 经生产读取/聚合及 .so，57,761 行/16 个非空批次/1 模型；与旧 IPC 所有列/NULL/行序及模型精确一致。合成直接回调 15 批仅为另一结构夹具。
- 无效轮保留：测试在链接期间启动被 artifact_missing/changed 拒绝，未计入最终成功；首轮 Sanitizer 聚合测试超时、受限 LSan fatal 均保留。五个 CTest 分组保留全部断言，原 90 秒上限不变；获批环境最终全绿。
- 阻塞：所选外部数据库 doctor 在 socket 阶段 execution_denied；申请扩大权限被自动审批拒绝，异步请求用户明确授权统一测试配置连接和隔离夹具，尚无答复。外部发布/恢复/托管矩阵未执行，不计通过；T4 五轮成本/完整 all/用户文档收口留待顺序继续。
- 审计：C++ 格式、git diff --check、一轮有界 diff 审查通过；原无关文件、暂存区和旧库副本保持。未操作用户服务/业务结果，未提交推送。
- 证据：implementation/implementation-checkpoint.json、t3-unit-rebuilt-final.log、t3-sanitizer-final.log/xml、t3-production-structure.*、t3-scope-final.json、t3-bounded-review.json。当前本机代码和证据已形成可复核结果。

## 最近完成：baseliner-output-throughput / T2 / Atomic Slice 1

- 授权/意图：按用户连续实施授权，在 T1 真实红灯与旧库参照上实现有界拥有型批次，保持逐桶计算和发射顺序。
- Non-Goals：不改公式/模型/公共 ABI/SQL/Schema、DataFrame 通道、编译优化级别、其他 Feature；不操作用户服务或结果，不提交推送。
- 内部接口冻结：ResultBatchEncoder(config, max_bytes, limits)；Append(row, batches, error)、Flush(batches,error)、Stats()；立即将 EvaluationRow 复制入拥有型 Arrow builders，不借用行字符串，不另保留行对象；最多 4096 行。
- 预算契约：同回调所有已完成批次与 builders 共用一个受限 allocator，增长计入旧+新缓冲同时峰值；先扣除 pending_/result_bytes_；reserve 失败先排空旧行，再尝试当前行，不重复 Submit；Arrow buffer 独立拥有 allocator。Schema/回调边界先排空；失败保持调用者 outputs 为空。
- 允许文件：tasks/active_task.md、tasks/specs/feat-baseliner-output-throughput.md；src/operators/baseliner/result_batch.h、result_codec.h、result_codec.cpp、baseliner_operator.cpp；src/tests/test_baseliner_contract/test_baseliner_output_batching.cpp、test_baseliner_operator.cpp；本地证据 implementation/。
- 步骤：实现 streaming Arrow encoder/受限分配器与算子接入；将 observation 批次数断言改为逻辑行断言；补预算与拥有期断言；真实算子/完整样本等价及结构验收。
- 验收：cmake --build build --target test_baseliner_output_batching test_baseliner_output_cost test_baseliner_operator -j4；tools/test.sh unit --filter '^(test_baseliner_output_batching|test_baseliner_output_cost|test_baseliner_operator)$' --jobs 1；完整样本与 T1 IPC compare；clang-format、git diff --check/name-only。
- 依赖：本机既有 Arrow/Baseline/Scheduler/Catalog 插件，仓库 CSV、私有 DataFrame/SQLite；无外部服务、网络或特殊权限。串行测量，不并行构建与成本实验。
- 时间盒：18:30–19:00 Asia/Shanghai，30 分钟。停止：T2 验收完成即记录并关闭；不足则记录检查点沿用 T2，不混入 T3。随后按用户授权重新冻结下一切片。

### T2 完成证据

- 状态：已完成，本切片边界关闭。定向标准测试 selected/executed/passed=3，failed/skipped/not_executed=0；summary=20261018T103522Z-08bfc263。
- 实现：Arrow streaming builders 直接拥有结果字段；4096 行与回调/Schema 边界排空，受限 allocator 同时计入已编码批次和扩容峰值，无新增结果对象容器。
- 等价：同规模合成输入真实算子 57,761 行/15 批；真实 SQL 4.702 秒/1 模型，与 T1 全列/NULL/行序/模型精确等价。仅首轮，不提前声明 T4 门槛通过。
- 边界：精确峰值/少一字节、字节切批、累计超限空 outputs、变长字段及独立列拥有期通过；格式/diff/无关文件 SHA 审计通过；初次编译缺 config 参数已修复并重新编译验收。
- 本地证据：implementation/t2-*；一轮 diff 审查未发现 P0/P1 阻塞；原业务结果/服务、无关修改和暂存区保持。按授权另载 T3。

## 最近完成：baseliner-output-throughput / T1 / Atomic Slice 1

- 授权：用户要求“按任务顺序进行实施，直至完成”；依次完成 T1→T2→T3→T4，每个切片验收并停止当前边界、更新工作台后再显式载入下一切片；不并行推进任务、不提交/推送。
- 业务意图：以真实算子结构断言、精确结果/模型参照和可重跑原实现成本，确保优化命中生产热点且保持结果。
- Non-Goals：本切片不改生产算法或输出实现，不改公共 ABI/SQL/Schema/构建优化等级，不操作业务结果、服务、容器或无关文件。
- 冻结契约：内部 ResultBatchLimits.max_rows=4096；Stats 记录行数/批数/Arrow 字节/暂存峰值；相同回调/Schema/时间戳内拥有型合批、预算扣除已保留结果、失败不交付半批，逐桶先评估后学习不变。
- 允许文件：tasks/active_task.md、tasks/product_backlog.md（仅本 Feature 状态）、tasks/specs/feat-baseliner-output-throughput.md；src/operators/baseliner/result_batch.h（新增内部数据声明）；src/tests/test_baseliner_contract/baseliner_batch_fixture.h（新增）、test_baseliner_output_batching.cpp（新增）、test_baseliner_output_cost.cpp（新增）、CMakeLists.txt；本地证据仅 evidence/baseliner-output-throughput/implementation/。
- 初始范围：保留已有 Docker/README/测试说明等修改；文件/暂存区摘要和生产库副本见 implementation/baseline.json、before-lib*，工作台旧文保存 active-task-before.md。
- 步骤：创建真实 .so RAII 夹具与批次结构红灯、参照等价和原成本入口；保存完整样本、前缀及五个固定代表负载的基线入口/编译环境；冻结内部批次数据契约。
- 验收命令：cmake -B build src；cmake --build build --target test_baseliner_output_batching test_baseliner_output_cost -j4；tools/test.sh unit --filter '^test_baseliner_output_batching$'（先保留结构红灯）；基线模式结构/等价运行；真实 Scheduler 成本入口 smoke；clang-format --dry-run --Werror；git diff --check；git diff --name-only。
- 依赖：本机 C++17/Arrow/既有 Baseline/Catalog/Scheduler 插件，私有 DataFrame/SQLite 与仓库 CSV；无外部数据库/Kafka/采集权限或网络依赖，不改变统一运行时测试配置。全量回归在 T4 用标准 all，依赖失败不计通过。
- 时间盒：18:14–18:44 Asia/Shanghai，30 分钟；若未完成沿用 T1，在有证据的检查点更新，不扩大到 T2。
- 停止条件：结构热点真实复现、结果模型精确参照、成本入口可复跑、契约对齐且定向构建/必要断言通过后勾选 T1，记录 WIP=0；随后依据用户顺序授权重新冻结 T2，不把预实验当作正式性能验收。

### T1 完成证据

- 状态：已完成；10 组原实现结构/精确结果及模型参照通过，结构红灯真实复现，非失败修复验收。
- 成本：真实 Scheduler 完整 SQL 173.306 秒；16,384/32,768 行 14.598/64.346 秒；五个代表入口成功，全部保存 IPC 参照。仅一次基线，不充当 T4 五轮验收。
- 构建/检查：两个新 target 编译通过；标准成本 smoke 选中/执行/通过 1；新增 C++ 格式、diff、无关文件/暂存区 SHA 审计通过。一轮内部契约审查无 P0/P1 阻塞。
- 证据：implementation/t1-context.json、t1-cost-baselines.json、t1-audit.json、t1-*.log 及 references/；均不入库。T1 本边界关闭，按授权另载 T2。

## 最近完成：baseliner-output-throughput / T1 前置可行性测试 Atomic Slice

- 关联：用户要求先用测试代码验证优化可以达到多少；本轮只测原路径、仅批量追加和批量编码+追加，不实施生产修复。
- 业务意图：对同一 57,761 行样本给出可复跑的秒级优化预期，并以所有结果列和最终模型等价排除计算缩水。
- Non-Goals：不改 src/、构建配置、算法、公共接口、服务、业务结果、Docker 或容器；不执行生产 SQL，不提交/推送，不将原型计时算作完整 SQL 验收。
- 数据契约：4096 行页内批次；原逐桶评估/更新顺序、原结果 Schema/NULL/行序、source fingerprint/config hash 及最终模型保持；使用同一当前生产库和编译参数，cold 新进程。
- 允许修改文件：tasks/active_task.md；tasks/specs/feat-baseliner-output-throughput.md 仅追加本轮证据；独立诊断源码/脚本/构建/日志仅写 evidence/baseliner-output-throughput/feasibility/。
- 初始范围：工作台 WIP=0，已有 Docker/文档/Feature 立项修改；初始文件及产物摘要、暂存区摘要和工作台原文保存于本轮 baseline.json、active-task-before.md。
- 步骤：沿用现有 CMake target 编译/链接参数构建独立程序；三路径单进程逐轮运行，记录读/聚合、求值、编码、追加、模型、总耗时和批次；计时外逐列与模型比对；聚合重复样本并审查范围。
- 验收命令：python3 evidence/baseliner-output-throughput/feasibility/run.py；python3 evidence/baseliner-output-throughput/feasibility/real_reference.py；clang-format --style=file:src/.clang-format --dry-run --Werror（本轮 C++）；python3 evidence/baseliner-output-throughput/feasibility/verify.py；git diff --check；git diff --name-only。
- 测试依赖：本机 C++17、Python3、当前 build 中 Baseline/Common/数据库/Arrow 库与 Baseliner contract 静态库、目标 CSV；直接复用 test_baseliner_cost 的 CMake 参数，不重配/覆盖当前 build。无外部数据库/Kafka/网络/采集权限，所有输出在本地证据目录，不注册用户通道。
- 时间盒：17:43–18:13 Asia/Shanghai，30 分钟。
- 停止条件：可比重复样本、结果与最终模型等价断言、产物/无关文件摘要及格式审计通过后 WIP=0 停止；完整 SQL 和 T1 其余正式验收留待实施，不自动开始 T2。时间盒到期按实际检查点收口。

### 本轮完成证据

- 状态：本轮可行性测试已完成，本轮收口，WIP=0；正式 T1 的其他交付和 T2～T4 未验收，不勾选任务，不自动开始生产修改。
- 构建/运行：独立 C++ 原型与真实算子核验程序使用当前 test_baseliner_cost 的 CMake 编译/链接参数构建成功；三方案各预热 1 次、正式 5 次，共 18 个隔离进程，全部成功；另运行真实 .so 核验 1 次通过。
- 成本：原单行路径总耗时中位数 175.076 秒（171.857～193.953）；仅批量追加 10.492 秒（10.401～10.987）；批量编码+追加 2.924 秒（2.901～3.115），相对原路径 59.88 倍、耗时降低 98.33%。
- 热点：编码/合并/追加合计中位数 170.748 → 0.452 秒（降低 99.735%）；编码/追加均由 57,761 次降至 16 次，每批 ≤4,096 行；批量路径读取/聚合/输入编解码约 0.620 秒、逐桶评估约 1.779 秒。
- 等价：17 次重复进程精确比较完整 30 列（含 NULL/行序/身份）和最终模型；真实 .so 再次证明 57,761 个单行批次及全部结果/模型一致；全部保持 57,761 行结果、1 行模型和相同 source fingerprint/config hash。
- 内存：比对参照加载前 RSS 中位数 287.83 → 187.71 MiB；仅属诊断程序，不作为生产预算保证；本轮暂存仅保留当前页及接收器完整输出。
- 目标建议：本样本完整 SQL 以 ≤10 秒为实施目标、争取 5～10 秒有依据；2.924 秒仅为核心链路实测。正式规格原 ≤30 秒且改善 ≥80% 的冻结门槛未改，尚未执行完整 SQL 或证明该门槛。
- 验证：本地 verify.py 54 项通过、0 失败；两份 C++ 格式、Python AST、git diff --check 通过，现有无关修改/Backlog/暂存区、四个生产产物和输入摘要保持；完成一轮原型生命周期/拥有期有界审查，无 P0/P1 阻塞。
- 边界：未修改生产源码、编译参数或服务，未覆盖用户输出，未运行常规 CTest/全量回归。完整预算同时峰值、失败发布、checkpoint 与 Scheduler/持久化/三类分析矩阵仍按正式规格实施，不将本轮当作修复验收。
- 本地证据：evidence/baseliner-output-throughput/feasibility/ 下 probe.cpp、operator_reference.cpp、run.py、real_reference.py、verify.py、measurements.json、summary.json、operator-reference.json、verification.json、report.md 及逐进程原始日志；均由 Git 忽略，未提交/推送。

## 最近完成：Baseliner 批量结果输出与性能验收立项 / Atomic Slice 1

- 关联：用户明确要求优化已核实的 Baseliner 分钟级延迟，本轮新增 Feature 并编写具体规格；不实施生产代码。
- 业务意图：为上传 CSV/DataFrame 做分钟级基线分析的用户，冻结输出批量化、结果等价与真实 SQL 性能目标，使后续优化有可执行的验收边界。
- Non-Goals：不修改算子、算法、共享 DataFrame 通道、SQL、公共 ABI 或构建配置；不运行业务 SQL、不覆盖原结果、不操作服务/容器、不提交或推送。
- 冻结契约：新增 P1 Feature `baseliner-output-throughput`，状态 `[ ]` 待实施；规格包含 Non-Goals、拥有型批次/预算契约、主链路、结果导向 T1～T4、真实算子测试与同配置成本门槛；诊断数据不得当作修复验收。
- 允许修改文件：tasks/active_task.md、tasks/product_backlog.md、tasks/specs/feat-baseliner-output-throughput.md（新增）；本地证据仅写 evidence/baseliner-output-throughput/。
- 初始范围：已有 Docker 发布修复及 README.md、docs/kafka.md 等未提交修改；原工作台为 WIP=0。初始摘要、暂存区摘要和原工作台缓存见 evidence/baseliner-output-throughput/planning-baseline.json 与 active-task-before-planning.md；保留全部无关修改。
- 实现步骤：复核生产单行输出/追加路径和既有测试；登记 Backlog；冻结极简规格、量化门槛与复现负载；做一次有界契约审查和文档/范围验证。
- 验收命令：Python 校验 Feature 唯一性、Backlog/规格相对链接、T1～T4 结果/状态、SQL parameters JSON 与源数据摘要、工作台历史/无关文件摘要和暂存区保持；awk '/^## 完成证据/{exit} NF{n++} END{print n+0}' tasks/specs/feat-baseliner-output-throughput.md；git diff --check、git diff --name-only。
- 测试依赖：本机 Python3、Git、已有 CSV、当前源码与统一测试入口的只读信息；纯文档切片无外部数据库/Kafka、Docker 权限或 C++ 构建依赖，不将未来验收命令记为本轮已执行。
- 时间盒：17:30–17:55 Asia/Shanghai，25 分钟。
- 停止条件：Backlog 与规格内容一致、契约和性能门槛可验证、文档与范围审计通过后更新 WIP=0 并停止；不自动开始 T1。

### 本轮完成证据

- 状态：立项与规格编写已完成，WIP=0；Backlog 新增 P1 `baseliner-output-throughput` 为 `[ ]` 待实施，T1～T4 均未开始。
- 规格：冻结每批最多 4,096 行、标准负载主结果非空批次 ≤16、本回调内按预算排空、逐桶学习顺序、拥有期与原发布保证；包含完整样本 SQL、原始 CSV 摘要、边界/错误/三类分析矩阵及四项结果导向任务。
- 性能门槛：新旧同构建、相同机器/执行位置，预热后各 5 次取中位数；完整 SQL 降低至少 80% 且 ≤30 秒，编码+追加降低至少 90%，前缀增长比及五组代表负载退化 ≤10% 同时验收；均为后续目标，未声称已达成。
- 文档验收：93 项本地检查通过，失败 0；规格完成证据前 141 个非空行、4 个一级任务、55 个去重相对链接有效；SQL JSON、57,761 行样本/端点/连续性、Backlog 状态与规格一致。
- 有界审查：完成一轮契约审查，未发现 P0/P1 级规格阻塞；实现需由真实算子与 Scheduler 链路测试收敛，不以本轮诊断对照作为修复验收。
- 范围：本轮仅修改工作台、Backlog 和新增规格；原 Docker/文档等无关文件摘要、原工作台历史与暂存区保持，git diff --check 通过；未编译、运行 SQL/CTest、改程序或提交/推送。
- 本地证据：evidence/baseliner-output-throughput/ 的 planning-baseline.json、planning-validation.json、verify_planning.py、bounded-review.json 与 diagnosis/ 原始诊断缓存；均被 Git 忽略。完成本轮后停止，不自动开始 T1。

## 最近完成：Docker 发布与运行修复 / Atomic Slice 1

- 关联：用户已确认修复上一轮 Docker 审计的四项问题；交付默认部署的功能插件、运行依赖和私有配置打包隔离保证。
- 业务意图：使用 Docker Compose 的用户能使用与原生相同的 Baseline 与 NetAdapter 框架能力，所启用采集插件不因缺少用户态运行库加载失败，实际 Kafka Provider 配置不能混入发布镜像。
- Non-Goals：不修改公共 ABI、Kafka 认证/读写语义和用户通道，不实现宿主机网卡自动透传/内核安装，不使用 privileged，不操作已有容器/卷/网络，不提交/推送或发布镜像。
- 冻结契约：Scheduler 默认加载 Baseline、NetAdapter 与三种采集 provider；NetAdapter 元数据放 /opt/flowsql/uploads/.meta/netadapter.db。PF_RING 动态运行库仅在构建启用时随 output 产物交付；镜像安装 libpcap/libxdp/libbpf 并生成 SONAME 链接。config/kafka 仅携带三个安全示例，实际 provider.json、私有子目录和密钥不进入上下文；不改动此前 README.md 和 docs/kafka.md。
- 允许修改文件：tasks/active_task.md、Dockerfile、.dockerignore、docker-compose.yml、src/channels/netadapter/CMakeLists.txt、src/tests/test_framework/CMakeLists.txt、src/tests/test_framework/test_docker_deploy_config.cpp、src/tests/test_framework/test_docker_build_context.py（新增）、docs/netadapter.md、docs/testing.md。本地证据仅写 evidence/docker-release-fix/。
- 文件数说明：同一部署修复必须同步构建产物、镜像、插件清单、回归注册及使用/依赖说明；不按实现文件拆成独立 Feature，不混入其他改动。
- 初始范围：HEAD=6c34ba4；既有 README.md、docs/kafka.md、工作台修改且暂存区为空；摘要及工作台历史缓存见 evidence/docker-release-fix/baseline.json。
- 实现步骤：先增加插件/依赖/真实上下文打包回归并确认失败；补部署与运行库分发；更新 Docker 采集边界/测试依赖说明；运行定向构建及标准回归，核对 ELF/SONAME、Compose 和未改文件摘要。
- 验收命令：cmake -B build src；cmake --build build --target test_docker_deploy_config test_native_deploy_config -j4；tools/test.sh unit --filter '^(test_docker_deploy_config|test_docker_build_context|test_native_deploy_config)$' --jobs 2；三套 docker compose config --quiet；clang-format --dry-run --Werror（本轮 C++）；本地无秘密上下文 tar、PF_RING staging/ldconfig/ELF 检查；git diff --check。按构建影响运行既有 NetAdapter/backend 与 Baseline 本地回归。
- 测试依赖：本机 CMake、C++17、Python3、Docker SDK 7.1.0（本地上下文打包，无 Engine）、Compose CLI、现有 DPDK/PF_RING/libxdp/libbpf 构建依赖；纯回归不依赖 broker/数据库/采集权限。所有新增夹具在独立临时目录，不读写真实凭证/业务资源。真实镜像验收依赖 Engine；本轮再次预检 socket permission denied，不绕过权限或将未运行项记为通过。
- 时间盒：17:02–17:32 Asia/Shanghai，30 分钟。
- 停止条件：四项修复及对应可运行回归闭环后更新工作台停止；真实 Docker build/run 受环境阻断时保留证据与未验证项；时间盒到期按实际检查点收口，不扩大文件或自动进入下一切片。

### 本轮修复证据与边界

- 状态：代码修复和可运行定向验收已完成，17:13 前收口；真实新镜像 build/完整 Compose 运行仍被 Engine socket 权限阻断，不作为发布运行已验收。
- 修复：Scheduler 补 Baseline、NetAdapter 与三 provider，网卡通道数据库放入持久卷；PF_RING 动态库在启用时随 build/output 准备，Docker 支持版本化库并由 ldconfig 建 SONAME 链接；libpcap/libxdp/libbpf 运行包齐备；Kafka/Docker 配置目录精确白名单，排除实际配置和私有目录。
- 红灯证明：新增两个回归先于修复执行，选中/执行 2、失败 2，跳过/未执行 0；20261018T090307Z-9f09c57a，复现插件/库缺失、私有文件被包含和版本化库被排除。
- 构建/绿灯：cmake 配置及九个定向 targets 编译退出 0；标准 unit 选中/执行/通过 10，失败/跳过/未执行 0，源码/产物摘要稳定；summary 为 evidence/test-environment/20261018T090950Z-85baf858/summary.json。包含部署/上下文、NetAdapter 三后端及 Baseline/Baseliner 本地回归。
- 静态/本地运行库：三套 Compose 解析、COPY 输入、PF_RING 安装源/构建产物摘要、本地隔离目录 ldconfig 链接和 dlopen 检查均通过；未调用采集 API，不将其当作容器或真实收包验证。C++ 格式、Python/文档 Bash 语法和 git diff --check 通过。
- 未验证：新镜像构建、完整服务启动、跨容器认证/SQL/Kafka 读写、重建恢复及真实采集；未跑全量 all。Engine 预检错误保存在 evidence/docker-release-fix/docker-engine.txt；没有绕过权限或操作原有资源。
- 范围：本轮十个允许文件；README.md、docs/kafka.md 与基线相同，既有工作台历史正文保留，暂存区为空；未提交/推送。证据 verify.py、verification.json、红绿灯日志及 report.md 均在被 Git 忽略的 evidence/docker-release-fix/。

## 前项完成：Docker 发布与运行核查 / Atomic Slice 1

- 关联：用户要求检查 Docker 方式发布和运行是否仍有问题；本轮为当前部署入口的审计，不实施未经确认的修复。
- 业务意图：确认当前镜像打包、基础/数据库叠加 Compose、Kafka 配置持久化和容器连接条件是否可支持发布运行，给出可复核的问题及验证边界。
- Non-Goals：不改 Dockerfile、Compose、配置或程序；不修改用户通道/任务，不提交/推送或发布镜像，不重置已有容器/卷/网络，不绕过 Docker socket 的权限限制。
- 冻结契约：保留原有 README.md、docs/kafka.md 与工作台历史；只更新工作台和本地证据。配置解析通过不等于真实镜像运行通过，历史 Docker 验收不得算作本轮执行。
- 允许修改文件：tasks/active_task.md；本地记录与审计脚本仅写 evidence/docker-release-audit/，构建/测试缓存按现有工具生成，不入 Git。
- 初始范围：HEAD=6c34ba4，既有 README.md、docs/kafka.md、tasks/active_task.md 修改，暂存区为空；摘要及原工作台缓存到 evidence/docker-release-audit/。
- 实现步骤：核对 Dockerfile/.dockerignore 打包闭包、Compose 启动/依赖/卷/网络及文档；检查当前 ELF 动态依赖和 Python Worker 路径；运行部署配置 target/定向回归；可访问 Engine 时以隔离项目验证，否则准确记录未执行项；审查本轮范围并输出最小修复方案。
- 验收命令：docker compose config --quiet（基础、数据库叠加、手测）；Dockerfile COPY/context 与插件/运行库审计；cmake --build build --target test_native_deploy_config test_docker_deploy_config -j4；tools/test.sh unit --filter '^(test_native_deploy_config|test_docker_deploy_config)$' --jobs 2；Docker Engine/version 预检、可用时隔离 build/run；git diff --check 及初始摘要核对。
- 测试依赖：本机 Python3、Bash、CMake、Node（如需前端产物核验）、Docker Compose CLI；本地配置契约不依赖数据库或 Kafka。真实镜像/容器验证依赖当前用户可访问 Docker Engine，当前 socket 返回 permission denied；不重复已被自动审批拒绝的提权方式。
- 时间盒：16:40–17:10 Asia/Shanghai，30 分钟。
- 停止条件：本轮核查和可运行检查完成，问题证据/影响/最小方案及未验证项齐全后 WIP=0；发现缺陷不自行扩大允许文件，真实运行受权限阻断则保留明确错误证据。

### 本轮核查证据与边界

- 状态：本轮静态核查与可执行定向回归已完成；真实镜像构建与容器运行验收被 Docker socket permission denied 阻塞，未伪记通过；发现的问题等待用户确认修复方案，不扩大允许文件。
- 确认缺口：默认 Docker Scheduler 未加载 Baseline 服务、NetAdapter 与 capture provider；当前 PF_RING/AF_XDP 产物需要的 libpfring.so.8、libxdp.so.1、libbpf.so.1 未纳入镜像运行依赖；.dockerignore 重新包含整个 config/kafka/，本地显式 provider.json 也可进入上下文。当前无真实配置/密钥泄漏证据。
- 构建与回归：两个部署配置 targets 编译退出 0；标准入口 unit 选中/执行/通过均 2，失败/跳过/未执行均 0，summary 为 evidence/test-environment/20261018T084318Z-8541d455/summary.json。这是本机配置契约测试，不是容器运行验证。
- 静态检查：基础、数据库叠加、手测三套 Compose 解析均退出 0；COPY 当前输入、声明插件文件、Arrow ABI、Kafka 持久状态卷、Python IPC/挂载及叠加 pyworker 依赖均通过；Docker SDK 本地打包器使用无秘密占位夹具复现 provider.json 进入构建上下文。
- 未验证：新发布镜像 build、完整 Compose 启动、业务链路、跨容器认证/读写和重建恢复；不引用历史最小 Docker smoke 充当本轮完整发布验收。
- 范围：仅工作台和被 Git 忽略的 evidence/docker-release-audit/ 有本轮修改；原 README.md、docs/kafka.md 摘要和暂存区保持；未修改程序/部署配置，未操作现有容器/卷/用户通道，未提交或推送。
- 本地证据：audit.py、audit.json、report.md、elf.json、apt-dependencies.txt、context-probe.json、docker-engine.txt 和初始摘要；问题影响、最小修复及验收建议均写入 report.md。修复需按 AGENTS.md 第 6 节先确认后开新切片。

## 前项完成：Kafka 手动测试用户指导 / Atomic Slice 1

- 关联：既有 Kafka source/sink 与手动测试部署的使用说明补全；用户明确要求详细指导和帮助文档。
- 业务意图：使用 ./start.sh 的用户能够按真实 Web 字段完成连接、source/sink 配置，发送样例、异步运行 SQL、辨认输出和重复测试。
- Non-Goals：不修改页面、插件、脚本或配置，不自动改用户连接/通道/任务，不发送消息或重置消费位点，不新建重复能力文档，不提交或推送。
- 冻结契约：唯一说明 docs/kafka.md 的手测章节保留既有入口锚点，按当前 KafkaChannels.vue / Tasks.vue 的实际标签及字段契约组织步骤；连接 manual、通道 manual_source/manual_sink、Topic flowsql.manual.source/flowsql.manual.sink、消费组 flowsql_manual，说明截图中通道名 manual 的 SQL 差异。
- 允许修改文件：tasks/active_task.md、docs/kafka.md、README.md；本地验证记录写 evidence/kafka-user-guide/。
- 初始范围：工作区和暂存区均干净，HEAD 为 6c34ba4；初始摘要见 evidence/kafka-user-guide/baseline.json。
- 实现步骤：核对 UI 与运行命令；编写逐项表单、字段映射、双端 SQL、结果与重测/停机说明；保留等价 API；检查示例、链接、命令语法与改动范围。
- 验收命令：文档代码块 JSON 解析、所有 bash 代码块 bash -n、相对文件链接与章节锚点检查；使用当前前端 buildConnection/buildChannel 对文档配置做断言；核对 UI 标签及任务停止 API；git diff --check、范围/暂存区检查。
- 测试依赖：本机 Python3、Node.js、Bash，静态读取当前源码/脚本/Compose；无 broker/数据库或其他外部依赖，不执行文档中的部署和写入命令。纯文档不编译 C++，不运行无关回归。
- 时间盒：14:22–14:47 Asia/Shanghai，25 分钟。
- 停止条件：帮助文档覆盖从首次启动到重测/退出及常见错误，示例与实际契约一致且静态验证通过后更新 WIP=0 并停止。

### 本轮完成证据

- 状态：已完成；沿用 docs/kafka.md 补全十步手测教程及导航，README 指向同一使用入口；本轮只修改三个允许文件，暂存区保持为空，未提交或推送。
- 内容：原生 FlowSQL + Docker Kafka 启动、Web 连接及 source/sink 逐字段配置、默认 message 字段替换、异步 SQL、供数与新增信封辨认、消费组续读与新组重放、取消任务与持久化恢复、错误排查及等价 API。
- 页面核对：42 个实际标签与当前组件一致；停止按钮为“取消”。当前结果弹窗不展示完整 publication，教程以实际公共任务 ID 查询 result.publication，不用信封中的运行时 ID；说明历史消息、int64 字符串及 read 从头读取的含义。
- 静态验收：32 个代码块围栏平衡；9 个 JSON 代码块、4 个静态 API 请求体及 1 个 stdin JSON 可解析；15 个 Bash 代码块及手测脚本 bash -n 均通过；22 处本地链接/锚点有效，原 README 手测锚点保留。
- 契约验收：文档连接/source/sink 三个配置经当前前端 buildConnection/buildChannel 往返与原配置一致；Web 与 API 的 SQL 一致且为 async；Topic、端口、网络、脚本命令、消费位点与结果计数说明均与当前实现核对。
- 范围审计：既有“原生安装与启用”及其后章节逐字保持；git diff --check 通过。本轮不启动/停止服务、不提交 SQL、不生产/消费消息，不声称重新运行 C++ 构建、Kafka 端到端或全量回归。
- 本地证据：evidence/kafka-user-guide/ 的 HEAD 基线、文档配置、verify.py 与 validation.json；均由 Git 忽略，不入库。

## 前项完成：移除未使用的 Kafka Compose 覆盖入口 / Atomic Slice 1

- 关联：Kafka 默认部署与手动测试部署配套清理；用户已授权全面核查后删除未使用文件。
- 业务意图：消除旧客户端覆盖文件与手测 broker 配置的命名混淆，默认原生、Docker 和手测入口继续使用现有配置。
- Non-Goals：不重命名手测配置，不删除显式 Provider/部署示例，不修改插件、认证与数据面，不启动或重建服务，不删除目录、密钥、数据库或 Docker 资源，不提交或推送。
- 核查证据：跟踪文件及非忽略/隐藏配置搜索仅找到文档和一行注释；启动脚本、构建、测试无旧入口引用。当前全部四个 Docker 容器（含停止容器清单）均未使用旧覆盖配置，当前 COMPOSE_FILE 未设置，本地显式部署文件未生成。
- 冻结契约：删除 config/docker-compose-kafka.yml，清理用户文档旧命令及手测 Compose 注释；保留默认 Kafka 持久状态、只读显式配置能力及其独立示例。
- 允许修改文件：tasks/active_task.md、config/docker-compose-kafka.yml（删除）、config/docker-compose-kafka-manual.yml（仅注释）、docs/kafka.md；证据写 evidence/kafka-compose-cleanup/。
- 初始范围：已有十三个跟踪文件修改和两个新增手测文件；本轮重叠修改仅工作台、说明和手测配置注释；其他文件摘要及暂存区保持，快照见 evidence/kafka-compose-cleanup/baseline.json。
- 验收命令：全面引用搜索；docker compose config --quiet（基础、数据库叠加、手测）；tools/test.sh unit --filter '^(test_native_deploy_config|test_docker_deploy_config)$' --jobs 2；bash -n tools/kafka-manual.sh；手测 broker status；git diff --check 及本轮范围/摘要审计。
- 测试依赖：部署配置契约为本机纯测，使用既有构建产物和统一 tools/test.sh；Compose 解析和容器来源检查仅使用当前 Docker Engine；status 只读访问已运行手测 broker，不生产/消费/提交位点，无外部认证依赖。
- 时间盒：12:08–12:28 Asia/Shanghai，20 分钟。
- 停止条件：旧文件无执行依赖且删除、引用清理、配置契约及范围审计通过后更新 WIP=0 并停止；发现实际使用则保留文件并报告证据。

### 清理完成证据

- 状态：已完成；旧覆盖文件已删除，用户说明与手测 Compose 注释已清理，保留显式配置示例及通用部署说明。
- 全面核查：Git 跟踪文件及隐藏/忽略的源码配置搜索均无剩余执行或用户文档引用；仅本工作台保留删除记录，构建、证据及依赖缓存不作为运行入口。当前四个 Docker 容器均未引用旧配置，显式本地配置/密钥文件未生成。
- 定向回归：标准入口 unit 选中/执行/通过均 2，失败/跳过/未执行均 0；覆盖 test_native_deploy_config、test_docker_deploy_config，summary 为 evidence/test-environment/20261018T040833Z-b18497b4/summary.json。
- 配置检查：基础 Compose、数据库叠加 Compose、独立手测 Compose 三套 config --quiet 均退出 0；脚本 bash -n、git diff --check 通过。手测 broker status 正常，Kafka 及三个数据库容器均 healthy，全程未启动/停止/重建任何容器。
- 范围审计：手测 Compose 只改首行注释；本轮允许列表外的原有修改摘要保持、暂存区不变；未编译或声称完整 CTest/数据面复测，未提交或推送。
- 本地证据：evidence/kafka-compose-cleanup/ 的 baseline.json、scope.json、compose.json、containers.json 和 broker-status.log，均未入库。

## 前项完成：Kafka 手动测试部署工具 / Atomic Slice 1

- 关联：已归档 kafka-log-source / kafka-result-sink 的开发部署配套；用户授权固化 Docker 环境供手动测试，不改变数据面能力或公共接口。
- 业务意图：原生 ./start.sh 和 Docker FlowSQL 用户能重复启动一个持久 Kafka，以固定样例向 source 供数，并直接观察 sink 输出。
- Non-Goals：不扩展认证/集群/packet 功能，不修改自动集成测试夹具，不修改用户通道定义，不重建生产镜像，不触碰已有数据库容器及数据，不提交或推送。
- 冻结契约：独立 Compose project flowsql-kafka-manual；固定官方 Kafka 4.1.0 镜像摘要、单节点 KRaft、单分区 source/sink Topic，禁止自动建 Topic；原生地址 127.0.0.1:19092，Docker 同网络地址 manual-kafka:9092；持久卷及网络独立。脚本提供 up/status/seed/read/produce/logs/down/help，up 幂等建 Topic，seed 产生合法三条 JSON，read 观察 source 或 sink，down 保留卷，produce 从 stdin 写入 source。固定 Schema 为 host:utf8、time_ms:int64（路径 /time）、value:float64；sink 使用 event 信封。
- 主链路：up → Web/API 创建连接及 source/sink → async SELECT INTO kafka sink → seed → read sink；down → up 后 Topic、数据和消费组保留。
- 允许修改文件：tasks/active_task.md、config/docker-compose-kafka-manual.yml、tools/kafka-manual.sh、docs/kafka.md、README.md；本轮证据仅写 evidence/kafka-manual/。
- 初始范围：已有 13 个脏文件是上一项默认部署修复；仅 docs/kafka.md、README.md、工作台在本轮继续修改，其余摘要保持；快照见 evidence/kafka-manual/baseline.json。
- 验收命令：bash -n tools/kafka-manual.sh；Docker Compose config --quiet；从不同 cwd 执行脚本、重复 up；原生及 Docker 实际元数据地址检查、三条消息生产/消费及 FlowSQL source→sink HTTP 端到端；down/up 后数据保持；git diff --check。无新增 C++，不重新编译或扩展自动认证矩阵。
- 测试依赖：Docker Engine/Compose、Python3、已有 build/output 的 FlowSQL 插件；使用本轮专用 PLAINTEXT 开发 broker，无外部账号；元数据预检和读写均从实际原生进程/隔离容器验证。手测参数由本轮 Compose/脚本管理，不引入自动集成测试配置的另一来源；隔离验证状态和日志放 evidence/kafka-manual/。
- 时间盒：11:43–12:13 Asia/Shanghai，30 分钟。
- 停止条件：工具、使用说明和上述真实链路/持久化验证完成后更新 WIP=0 并停止；若时间盒到期，记录检查点或具体错误及未验证项，不扩充范围。broker 保留运行供用户手测。

### 本轮完成证据

- 状态：已完成；11:54 前完成验收，broker 保留 healthy 运行，隔离 FlowSQL 实例已停止；未提交或推送。
- 交付：独立 Compose + 可从任意 cwd 调用的脚本；固定版本/镜像摘要、原生/容器双地址、专用持久卷和网络、幂等 Topic 初始化、三条样例供数、stdin 供数、source/sink 查看及保留数据的停止/恢复。用户说明有完整连接、Schema、sink event 信封配置和异步 SQL。
- CLI：语法/帮助/错误参数 5/5；真实 source 读取 3 条退出 0、请求 10 条 sink 但仅有 9 条时退出 1，两项均符合断言。Compose config 和 git diff --check 通过。
- 端到端：使用说明中的原始 JSON/SQL，从原生 127.0.0.1:19092、Docker manual-kafka:9092 分别查询实际 broker 元数据并执行 source→sink；两轮均 input_rows=confirmed_rows=3、not_sent_rows=unknown_rows=0。逐条核对 host/value、int64 字符串及版本化信封。
- 持久化：down/up 和 --force-recreate 两项检查均通过；Topic ID、source 3 条/sink 9 条完整消息和三个验收专用消费组的已提交 offset=3/lag=0 前后相同。普通手测组 flowsql_manual 未占用。broker 留有本轮样例/验收数据，查看 sink 会包含此前信封。
- 失败检查点保留：首轮隔离状态误放 Windows 挂载目录，私有文件系统检查拒绝；改用 Linux 状态路径。第二轮误等运行中公共任务结果的终态发布摘要而超时，但实际已发布三条；最终改为实际读取新增消息后停止任务，再检查终态报告。两轮未计入成功，原始日志和 HTTP 响应保留。
- 范围：其余前项修改文件 SHA256 与初始快照相同，暂存区未变化；未修改自动测试配置/夹具、C++ 源码和生产 Compose，未重新编译或声称执行完整 CTest/认证矩阵；这是开发部署工具的定向验收。
- 证据：evidence/kafka-manual/ 下 baseline.json、cli.json、read-count.json、e2e.json、persistence.json 及逐轮原始日志；均未入库。

## 前项完成：Kafka 默认部署修复 / Atomic Slice 1

- 关联：已归档 kafka-connection-auth 的部署缺陷修复；用户已确认固化默认配置并覆盖各种启动方式。
- 业务意图：使用 ./start.sh、原生 single/multi 配置、直接 --role/--plugins 和 Docker Compose 时，Kafka 管理接口默认可用；首次初始化后配置、密钥及定义跨重启复用。
- Non-Goals：不修改 Kafka 数据面、认证机制、Web 表单和公共接口；不启动生产 broker，不自动迁移已有显式部署，不提交或推送。
- 冻结契约：空插件参数启用统一默认初始化；原生状态根目录依次取 FLOWSQL_KAFKA_STATE_DIR、XDG_STATE_HOME/flowsql/kafka、HOME/.local/state/flowsql/kafka，必须是绝对路径；Docker 指定 /opt/flowsql/kafka-state 并挂持久卷。根目录内固定 config/kafka/provider.json、secrets/kafka-master.key、meta/kafka.db、runtime/kafka；配置记录绝对路径，首次随机生成 32 字节密钥，已存在文件不覆盖。已有数据库缺密钥、密钥损坏、目录权限不合格及重复占用明确失败；显式 JSON/config_file= 保留原严格加载契约，不兜底生成。
- 实现步骤：先锚定首次启动/跨 cwd 恢复/密钥丢失与损坏/显式配置回归；实现插件初始化；默认开启原生及 Compose 插件；验证真实 HTTP 和部署资产契约，更新用户文档。
- 允许修改文件：
  - tasks/active_task.md
  - src/channels/kafka/kafka_plugin.cpp
  - src/channels/kafka/kafka_plugin.h
  - src/tests/test_kafka_connection_auth/test_kafka_control.cpp
  - src/tests/test_kafka_connection_auth/deployment_smoke.py
  - src/tests/test_framework/test_native_deploy_config.cpp
  - src/tests/test_framework/test_docker_deploy_config.cpp
  - config/deploy-single.yaml
  - config/deploy-multi.yaml
  - docker-compose.yml
  - Dockerfile
  - docs/kafka.md
  - README.md
- 文件数说明：这是跨启动入口的一项部署保证，配置与对应回归必须同步；不按入口另建 Feature Task。本轮不扩充上述清单。
- 测试依赖：C++ 控制面和配置契约为本机纯测；真实 HTTP 原生/Docker 使用隔离临时状态、随机端口和现有运行镜像，无数据库/Kafka broker 依赖。显式部署烟测沿用统一测试入口配置及资源锁；Docker 仅操作本轮新建的受管测试容器/卷，原容器和数据保留。
- 验收命令：cmake --build build --target flowsql flowsql_kafka test_kafka_control test_native_deploy_config test_docker_deploy_config test_kafka_web_e2e -j4；tools/test.sh unit --filter '^(test_kafka_control|test_native_deploy_config|test_docker_deploy_config|test_kafka_web_e2e)$'；clang-format --dry-run --Werror（本轮 C++ 文件）；git diff --check；隔离原生/Docker真实启动与重启 HTTP 检查，显式部署 smoke 原生/Docker回归。
- 时间盒：11:24–11:54 Asia/Shanghai，30 分钟。
- 停止条件：上述入口及数据保护验证完成即更新工作台并停止；时间盒到期则记录已完成证据和明确未验证项，不自动进入下一切片；外部环境阻塞不伪记通过。

### 前项检查点

- 初始 Git 工作区干净；已复现未加载 Provider 时三个管理接口返回 404，加载后均 200。
- 已完成全量构建（退出 0）及原生 10 / Docker 8 个真实启动代次检查；覆盖默认配置、直接插件、多进程、start.sh 隔离运行与显式只读配置恢复。
- 验证范围补充：私有 Plugin 生命周期增加默认目录锁，最终执行全部 Kafka 本地回归及原生/Docker部署配置契约：`tools/test.sh unit --filter '^(test_kafka_.*|test_native_deploy_config|test_docker_deploy_config)$' --jobs 4`；不启动外部 broker/数据库。
- 无效轮如实保留：一轮 Web socket 被沙箱拒绝；一轮 4/4 用例通过但同时重新链接产物，被标准入口以 artifact_changed 判为无效。最终验收等待全部编译结束后独立运行，不计上述轮为通过。

### 前项完成证据

- 状态：已完成，11:35 前完成本轮闭环；未开始其他切片，未提交或推送。
- 交付：默认初始化统一放在 Kafka 插件中，原生 single/multi 与 Compose 默认加载；固定私有持久路径、每实例首次随机密钥、跨 cwd/重启恢复、既有状态缺配置/密钥及损坏时拒绝替换、默认状态单进程独占；显式部署契约保持。
- 编译：指定 targets 和全量 `cmake --build build -j4` 均退出 0；运行目录 single/multi 配置已同步。
- 最终本地回归：`20261018T033351Z-9879c58a`，选中/执行/通过均 24，失败/跳过/未执行均 0；源码与产物前后摘要相同。涵盖默认初始化与密钥保护、控制面、Web HTTP、各数据适配器本地回归和原生/Docker部署配置契约。
- 真实部署：标准入口 `tools/test.sh deployment --location native|docker` 均退出 0。原生 10 个代次（显式配置、默认 config/role/multi/start 各首次及重启），Docker 8 个代次（显式只读配置、默认 config/role/multi 各首次及重启）；验证加密凭证恢复、定义持久化、原密钥复用和私有权限。start 使用原脚本副本与隔离的最小 staged runtime；Compose 生产插件集合由部署契约和 `docker compose config --quiet` 验证，本轮未启动完整生产 Compose 项目。
- 格式/检查：5 个 C++ 文件 clang-format --dry-run --Werror、Python py_compile、git diff --check 通过；全部 13 个修改文件在允许列表内。
- 证据：`evidence/kafka-default-deployment/`、`evidence/test-environment/20261018T033351Z-9879c58a/`、`evidence/test-environment/direct/deployment-native-808c603d/`、`evidence/test-environment/direct/deployment-docker-d52df098/`，均未入库。
- 验收边界：这是部署修复的定向验收，未运行不带筛选的 all、真实 broker 认证/读写矩阵；未变更该契约。Docker 使用统一配置的固定运行镜像加本次构建产物进行隔离回归，发布镜像尚未重新构建，使用 Docker 时需先按 README 重建镜像。
