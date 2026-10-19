# 可重复测试环境

在仓库根目录执行 `tools/test.sh`。入口从运行时 JSON 读取地址、凭证引用、数据库角色和 Kafka 身份，按声明准备隔离服务，再从测试实际所在位置做认证与必要权限检查。默认数据库集成用例运行在 Docker 服务网络，连接服务名；原生调试显式选择发布端口。首次默认 Docker 准备不绑定宿主端口，原生集成准备才添加发布映射；已发布的受管映射继续复用。工具不修改 WSL 路由、用户容器或业务数据库。

```bash
cmake -B build src
cmake --build build -j2

# 本地注册用例，不启动数据库、Kafka 或 Docker
tools/test.sh unit
# 按需准备与执行全部数据库及 Kafka 真实矩阵/部署回归
tools/test.sh integration --jobs 1
# 全部注册用例，包括环境 fixture、普通 Kafka、两种真实矩阵/结构化接入及部署回归
tools/test.sh all --jobs 1
# 只读检查已有服务；首次准备前会明确报告不可用
tools/test.sh doctor
# 原生调试；Docker 默认可达不代表 WSL 发布端口也可达
tools/test.sh doctor --location native
tools/test.sh integration --location native --filter '^test_mysql$'
```

`--filter` 使用正则表达式；相关 CTest setup fixture 自动纳入选中清单。带筛选的通过只代表该清单，完整 Feature 验收使用不带筛选的 `all`。直接 `ctest --test-dir build --output-on-failure` 同样执行运行时 guard、fixture 和资源锁，不允许缺依赖后返回成功。

以上示例使用保守并行度，适合资源有限的开发环境。编译、镜像构建和完整回归依次执行，避免同时占用内存；
确认资源余量后可自行提高 `-j` 或 `--jobs`。验收运行期间保持源码与产物不变，重新编译后应重新验收。

本地 `test_docker_build_context` 使用 Docker SDK 7.1.0 的真实上下文打包器，检查镜像输入和私有 Kafka
配置的过滤规则。执行该单测只需 Python 依赖 `docker==7.1.0`，无需 Docker Engine、broker、数据库或
真实凭证；夹具在独立临时目录中创建占位文件。缺少 SDK 时测试失败，不跳过，可在开发 Python 环境中安装：

```bash
python3 -m pip install docker==7.1.0
```

集成测试首次运行需要 Docker CLI/Compose 和 socket 访问权限，以及当前构建产物。数据库镜像按 `config/testing/environment.json` 的固定摘要准备；运行镜像和身份夹具镜像使用配置声明的不可变镜像身份。当前工作区已有固定运行镜像，可通过以下命令为其他工作区准备运行依赖，然后将 inspect 返回的不可变 ID 写入自己的配置副本（runtime/harness）。

```bash
python3 tools/release.py image --tag flowsql:test-runtime
docker build --build-arg CLIENT_IMAGE=flowsql:test-runtime \
  -f src/tests/test_kafka_connection_auth/Dockerfile.auth-harness \
  -t flowsql:test-harness .
docker image inspect flowsql:test-runtime flowsql:test-harness --format '{{.Id}}'
```

Kafka 分发优先从固定摘要的官方 Kafka 4.1.0 镜像提取并核对版本，否则下载配置中声明的官方归档并校验 SHA512。原生真实矩阵还需 JRE 21、openssl、Python cryptography、MIT KDC/kadmin/kinit。缺镜像、工具、库或构建产物会报告 `artifact_missing`，不会借用某次历史 evidence 下的发行包。

默认配置是 [environment.json](../config/testing/environment.json)，每套用例的服务、库角色、运行位置与共享资源在 [dependencies.json](../config/testing/dependencies.json)。`--config /absolute/path/environment.json` 或 `FLOWSQL_TEST_CONFIG` 可切换运行时配置，无需重新 CMake/编译。严格拒绝未知、缺失、重复字段。`managed=false` 使用显式外部测试资源，只做预检与声明的业务测试，不建库或调整账号；Kafka 外部模式提供私有 `FLOWSQL_TEST_KAFKA_MANIFEST`，其结构与受管 profiles.json 相同，包含完整连接、凭证引用、Topic 和信任配置。

默认测试资源归属独立 `flowsql_test` 项目，数据库按 driver/npm/source/target/production 隔离；相同角色的共享表由清单中的资源锁串行保护。Kafka Broker、KDC、HTTPS token/JWKS 同处一个独立网络，证书 SAN 与 principal 匹配实际服务地址。真实矩阵的服务/Topic/ACL/票据使用独立运行身份，完成或取消释放本轮进程和客户端。已存在同名用户容器、网络或卷不能冒充受管资源。

临时数据库验收项目（`flowsql_empty_*`）在本轮成功或异常退出时自动移除其容器、卷和网络，准备部分失败也执行回收；清理错误使测试失败。可复用的主测试项目在普通测试后保留，避免每次重新初始化。显式清理 Docker 测试资源使用：

```bash
# 默认只预览；必须明确填写配置中的 project，示例为默认项目
tools/test.sh cleanup --project flowsql_test
# 确认后移除该测试项目的容器、卷（含测试数据）和网络
tools/test.sh cleanup --project flowsql_test --apply
```

清理不读取凭证或 Compose 文件，状态目录丢失后的孤立卷也可按精确 `flowsql.test.project` 标签识别。无该标签的卷、其他项目及业务资源保留；发现冲突 Compose 标签，或卷/网络被其他项目容器引用时整批拒绝删除。标准测试入口持有项目活动共享锁，清理使用非阻塞独占锁；同一项目的并发命令须使用同一 `FLOWSQL_TEST_STATE`。清理失败返回非零，重复清理空项目成功。该命令仅管理 Docker 资源，不终止原生 Kafka 进程；强制结束整个测试进程（如 SIGKILL）无法保证 finally 执行，遗留 Docker 资源需用此入口显式清理。

私有状态默认在 Linux 文件系统 `/tmp/flowsql-test-<uid>/`，父目录 0700、凭证 0600；可用 `FLOWSQL_TEST_STATE` 指向持久 Linux 路径。WSL 的 Windows 盘符挂载可能始终呈现 0777，此时应换私有状态位置，不能放宽凭证权限检查。默认凭证 `file:@state/secrets/<backend>` 由入口首次生成，以后复用；也支持 `file:/private/path` 和 `env:VARIABLE`。切勿把秘密写进 tracked JSON、日志或命令行。修改初始化密码不会改变已有卷里的账号；真实认证不匹配必须失败，不会自动清卷。

准备总预算默认 60 秒，单次连接最多 3 秒；认证、配置和明确权限错误立即失败。GSSAPI 双身份续期及 OAuth 刷新的长驻留时间是测试预算，独立于准备预算。`doctor` 只读查询数据库与 Kafka 元数据，不建表/Topic、不发布/订阅/消费、不加入组或提交位点；读写权限无法证明时显示 `unverified`。业务集成的建表/写入/清理权限只在专用测试库验证。

每轮入口在 `evidence/test-environment/<run_id>/` 保存脱敏 CTest 日志与 summary.json，列出选中、实际执行、通过、失败、跳过、未执行数和名单。任何必要依赖失败、环境跳过或未执行都会使验收非零；纯单测入口的排除不计为完整覆盖。私有配置/票据/原始服务日志留在私有状态，evidence 不提交。

| 返回类别 | 下一步 |
| --- | --- |
| config_missing / config_invalid | 核对配置版本、字段、凭证引用和 fixture 复用身份；配置改变时选择新的测试项目。 |
| execution_denied | 核对当前进程的本地 socket/Docker socket 权限或工具执行权限。 |
| artifact_missing | 构建对应目标或准备声明的固定镜像/运行工具。 |
| authentication_failed | 核对同一凭证引用与已有测试卷内身份；禁止猜密码或清卷绕过。 |
| database_missing / permission_denied | 核对测试库角色与授权；外部模式由测试资源管理员准备。 |
| network_unreachable / broker_address_unreachable | 从当前测试位置核对发布端点或服务名，以及 Kafka 返回的 broker 地址。 |
| tls_validation_failed / mechanism_unavailable | 核对 CA、SAN、安全协议、服务端机制与客户端能力；保持校验开启。 |
| kerberos_unavailable / token_service_unavailable / credential_expired | 核对独立身份服务、信任与续期，初次认证成功不能替代续期验收。 |
| preparation_timeout / test_failed | 查看该轮脱敏日志与明确失败阶段，修复后重跑相同选择。 |
| resource_busy | 等待该项目测试结束，核对卷/网络引用；不要强制清理正在使用的项目。 |

新增依赖测试时，在同一 Atomic Slice 中完成四件事：声明 dependencies.json 的服务/角色/位置/锁；接入 `tests/common/test_environment.h` 的参数/Options 或 Kafka 受管 manifest；用标准 `add_test(NAME ... COMMAND ...)` 注册；验证正常执行和至少一个必要依赖失效路径。共享 fixture 在 CTest 运行时从清单派生，CMake 不读取 MYSQL/PG/CH 密码或临时 Baseliner options。每轮临时文件用 `WorkspacePath`，不引用旧任务的 `/tmp` 目录。完整说明及约束见 [AGENTS.md](../AGENTS.md) 4.4。

Kafka 结构化数据面单独注册 `test_kafka_source_native/docker`，复用同一受管身份 manifest，从真实执行位置预检后在独立 Topic/组中发布与消费。`test_kafka_source_environment` 验证缺失配置必须非零。选中用例及位置由 dependencies.json 声明，不需要手工凭证或第二份连接配置。

## 产品版本回归

`test_product_version` 使用临时 Git/CMake 工程验证解析、PEP 440、dirty/未知、无变化及增量编译，并核对 Worker 源码分发包。
`test_product_release` 使用隔离 Git 和产物夹具验证发布通过与拒绝路径，不修改产品仓库标签。
`test_product_version_runtime` 查询实际 `build/output` 的 CLI 和 Web 两个 HTTP 监听入口，使用临时状态、动态本地端口及自身子进程，退出时回收。
`test_product_version_frontend` 需要已安装的本地 Node/npm/Vite，在临时复制树执行独立构建、真实 Vue 渲染及 dev 版本刷新，验证每个压缩 JS 块的身份；不执行 npm 安装，不连接后端。
以上用例按无外部服务测试注册，不使用数据库/Kafka 凭证；HTTP/Vite 需要本地监听权限。

```bash
./tools/test.sh unit --filter '^test_product_(version|version_runtime|version_frontend|release)$'
```

真实镜像验收需 Docker Engine 权限及通过身份校验的完整构建；使用专用本地镜像和容器，不挂载业务卷，检查 CLI、Worker、Web、静态版本与 OCI 标签。
这些定向检查不能替代不带筛选的 `./tools/test.sh all`。版本规则和发布操作见[版本说明](versioning.md)。
