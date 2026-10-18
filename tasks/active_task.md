# 即时工作台

WIP=0；2026-10-18 Asia/Shanghai。

## 最近完成：移除未使用的 Kafka Compose 覆盖入口 / Atomic Slice 1

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
