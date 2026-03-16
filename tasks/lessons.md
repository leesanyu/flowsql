# 经验教训

## L1: 不要为了探测元数据而消费数据流

**来源**：DatabaseChannel Phase 7 — SqliteDriver::CreateReader bug 修复

**问题**：`CreateReader` 中为推断列的 Arrow 类型，先调用 `sqlite3_step` 读取第一行判断类型，再 `sqlite3_reset` 重置。导致两个 bug：
1. `sqlite3_reset` 后带 WHERE 的查询返回空结果集
2. `sqlite3_step` 在 SQLITE_DONE 后自动 reset 重新执行，数据被重复读取

**原则**：当需要从有状态的游标/迭代器中获取元数据（Schema、列类型等）时，使用声明式 API（如 `sqlite3_column_decltype`）而非命令式 API（如 step 一行再 reset）。后者会改变游标状态，而状态恢复在不同查询条件下行为不一致，是隐蔽 bug 的温床。

**推广**：有状态对象（游标、流、迭代器）到达终态后，不要假设"再次调用会安全返回终态"。必须在应用层显式跟踪终态（如 `done_` 标志）。

---

## L2: 接口设计要充分考虑职责分离

**来源**：Sprint 1 回顾 — Pipeline 和 IChannel 接口重构

**问题**：
1. Pipeline 职责过重：负责 Channel ↔ DataFrame 转换，代码复杂度高
2. IChannel 接口混杂：生命周期、元数据和数据操作混在一起，难以扩展

**原则**：
- **单一职责原则**：每个接口只负责一件事
- **接口分层**：生命周期、元数据和数据操作分离
- **职责下沉**：通用逻辑上提，特定逻辑下沉

**解决方案**：
- Sprint 2 中将 Pipeline 重构为纯连接器，数据转换职责下沉
- IChannel 基类只保留生命周期和元数据，数据读写下沉子类（IDataFrameChannel/IDatabaseChannel）

---

## L3: 静态库单例问题

**来源**：Sprint 2 回顾 — PluginRegistry 单例失效

**问题**：libflowsql_framework.a 静态库链接到多个 .so（主程序和 bridge.so）时，每个 .so 有独立的 PluginRegistry 单例实例，导致插件注册失效。

**原则**：
- **避免静态库单例**：静态库链接到多个 .so 时单例失效
- **优先使用共享库**：或回归纯插件架构

**解决方案**：
- Sprint 3 中删除 PluginRegistry 和 libflowsql_framework.so，回归纯 PluginLoader 架构
- Pipeline/ChannelAdapter 移入 scheduler.so，避免跨 .so 共享状态

---

## L4: 测试要与功能开发同步

**来源**：Sprint 1 回顾 — 测试覆盖率不足

**问题**：初期聚焦功能实现，忽略测试，导致代码质量无法保证。

**原则**：
- **TDD（测试驱动开发）**：先写测试，再写实现
- **测试覆盖率**：单元测试覆盖率 > 80%，集成测试覆盖核心路径
- **测试金字塔**：单元测试 > 集成测试 > 端到端测试

**改进措施**：
- 每个 Story 完成后立即补充测试
- 使用工具辅助（valgrind、AddressSanitizer）

---

## L5: 第三方依赖要隔离

**来源**：Sprint 1 回顾 — Arrow 依赖管理

**问题**：第三方依赖混在 build 目录中，每次清理重建耗时长。

**原则**：
- **依赖隔离**：第三方依赖隔离到 build 目录外
- **缓存编译产物**：避免重复编译
- **三级回退**：pyarrow/缓存/源码编译

**解决方案**：
- 将 .thirdparts_installed 和 .thirdparts_prefix 移到项目根目录
- arrow-config.cmake 支持三级回退

---

## L6: 资源泄漏要及时发现

**来源**：Sprint 2 回顾 — Python Worker 资源泄漏

**问题**：Python Worker 资源泄漏（文件描述符、socket、线程）在后期才发现，增加返工成本。

**原则**：
- **每个 Story 完成后立即进行代码审查**
- **重点关注资源泄漏、线程安全、错误处理**
- **使用工具辅助**（valgrind、AddressSanitizer、lsof）

**改进措施**：
- 建立代码审查流程
- 使用 RAII 管理资源（智能指针、守卫类）

---

## L7: 架构问题要及早发现和解决

**来源**：Sprint 3 回顾 — PluginRegistry 单例问题

**问题**：PluginRegistry 单例问题在 Sprint 2 就存在，但直到 Sprint 3 才解决，增加返工成本。

**原则**：
- **及早发现架构问题**：代码审查时重点关注架构设计
- **果断重构**：发现架构问题时，不要犹豫，果断重构
- **避免技术债务累积**：每个 Sprint 都要清理技术债务

**改进措施**：
- 在 Sprint Planning 时识别架构风险
- 在 Sprint Review 时识别技术债务
- 在 Sprint Retrospective 时制定改进计划

---

## L8: 文档要与代码同步

**来源**：Sprint 3 回顾 — 文档更新滞后

**问题**：文档更新滞后，与实际代码不一致，新人上手困难。

**原则**：
- **文档与代码同步**：每个 Story 完成后立即更新文档
- **文档要准确**：确保文档与实际代码一致
- **文档要完整**：用户手册、开发者指南、API 文档都要完善

**改进措施**：
- 将文档更新纳入 Story 的验收标准
- 在 Sprint Review 时检查文档完整性

---

## L9: 集成测试必须走完整生产路径，禁止绕过插件层

**来源**：Sprint 4 回顾 — 测试策略根本性错误

**问题**：`test_mysql_driver.cpp` 直接 `#include` 驱动头文件、`new MysqlDriver()` 调用底层 API，绕过了 `PluginLoader → IDatabaseFactory → IDatabaseChannel` 的完整生产路径。这类测试只验证了零件，无法发现插件加载、工厂注册、Channel 生命周期管理等集成层面的 bug。`test_plugin_e2e.cpp` 的 double-loading 堆损坏问题，正是因为测试直接链接了 `flowsql_database.so` 才暴露，而驱动层测试永远不会发现这个问题。

**原则**：集成测试必须走完整生产路径。对于插件架构，测试入口是 `PluginLoader::dlopen`，而非 `#include` 驱动头文件。

---

## L10: 测试数据必须覆盖所有声明支持的类型

**来源**：Sprint 4 回顾 — INT32 类型处理 bug 发现滞后

**问题**：`MysqlBatchWriter::BuildRowValues()` 对 `INT32` 没有处理分支，但测试数据只用了 `INT64`/`DOUBLE`/`STRING`，`INT32` 路径从未被覆盖，导致 bug 在 Sprint 主体开发完成后才被发现。

**原则**：写入路径的测试数据必须系统性覆盖所有声明支持的 Arrow 类型（INT32/INT64/FLOAT/DOUBLE/STRING/BOOL 等），不能只用"方便构造"的类型。类型分支覆盖是写入路径测试的最低要求。

---

## L11: 测试目标必须显式 -UNDEBUG，assert 是测试的最后防线

**来源**：Sprint 4 回顾 — assert 被 NDEBUG 静默禁用

**问题**：顶层 `CMakeLists.txt` 的 Release 配置传递了 `-DNDEBUG`，测试二进制中所有 `assert` 被预处理器删除，测试运行"通过"但断言从未执行。`batch` 为 null 时 `batch->num_columns()` 直接 SIGSEGV，而测试无法捕获。

**原则**：每个测试目标的 `CMakeLists.txt` 必须显式加 `target_compile_options(... PRIVATE -UNDEBUG)`。关键错误检查不依赖 assert，改用显式 `if` + `printf` + `exit(1)`，确保在任何编译配置下都能中止。

---

## L12: 涉及共享对象的接口必须有多线程并发测试

**来源**：Sprint 4 回顾 — 多线程并发测试完全缺失

**问题**：`DatabaseChannel::CreateReader/CreateWriter` 是多线程共享入口，整个 Sprint 4 没有一个并发读写测试。连接池并发安全有测，但上层 Channel 的并发行为无测。

**原则**：凡是可能被多线程并发调用的接口（Channel、Factory、Pool），Story 验收标准中必须包含多线程测试用例，验证：① 无 crash；② 无数据串扰；③ 资源无泄漏（in_use 归零）。

---

## L14: 密码/认证相关测试禁止用 SQLite 替代

**来源**：Sprint 5 TQ-B4 — 密码重启解密测试用 SQLite 代替 MySQL/ClickHouse

**问题**：TQ-B4 用 SQLite `:memory:` 测试"带密码通道重启后解密验证"。SQLite 完全忽略密码字段，`Get()` 成功只证明 SQLite 连接成功，与密码解密无关。这条测试无法验证"AES-256-GCM 解密 → 密码传给驱动 → 驱动用密码认证"的完整路径，制造虚假覆盖。

**原则**：凡是涉及密码、认证、加解密的测试，**必须使用真实校验密码的数据库**（MySQL 或 ClickHouse）。SQLite 不校验密码，用它测试认证路径等于没有测试。测试必须能在密码错误时失败，才能证明密码正确时的成功是有意义的。

**推广**：更一般地，测试所用的替代品必须与被测路径在关键行为上等价。如果替代品对被测特性无感知（如 SQLite 对密码无感知），则该替代品无效。

---



**来源**：Sprint 4 回顾 — 专家视角模式二

**问题**：Sprint 4 的测试策略存在系统性偏差——所有选择都在降低测试通过的阻力，而不是提升测试发现问题的能力：
- 用 SQLite `:memory:` 替代 MySQL：启动快、不依赖外部环境，但不是生产路径
- 直接 `new MysqlDriver()` 绕过插件层：代码简单，但集成层 bug 永远不会暴露
- `assert` 被 NDEBUG 删除：测试永远不会因断言失败而中止，制造虚假绿灯

这三个选择的共同动机是降低阻力。但**一个永远通过的测试套件，价值为零，甚至是负值**——它制造虚假安全感，让团队相信代码是正确的，而实际上什么都没有验证。

NDEBUG 问题尤其严重：它破坏的不是单个测试用例，而是整个测试体系的可信度基础。整个 Sprint 期间"测试全部通过"是一个谎言，而没有人察觉。

**原则**：
- 测试的价值在于它能在代码出错时**失败**，而不是在代码正确时通过
- 测试环境应尽量贴近生产环境，而不是选择最方便的替代品
- 任何让测试"更容易通过"的决策，都要先问：这会不会同时让测试"更难发现问题"？
- 测试基础设施（编译配置、断言机制）的可信度，优先于测试用例数量

---

## L15: IPlugin 生命周期必须批次调用，禁止逐个走完整生命周期

**来源**：Sprint 6 架构讨论 — RouterAgencyPlugin 方案前置条件

**问题**：`main.cpp` 的 `RunService()` 循环调用 `LoadPlugin()`，而 `LoadPlugin()` 内部是 `loader->Load()` + `loader->StartAll()` 一气呵成。导致每个插件加载后立即启动，后续插件的接口尚未注册。当 RouterAgencyPlugin 在 `Start()` 中 `Traverse(IID_ROUTER_HANDLE)` 收集路由时，排在它后面加载的业务插件还不存在。

**原则**：插件加载必须分阶段批次执行——先所有插件 Option()，再所有插件 Load()，最后所有插件 Start()。禁止逐个插件走完整个生命周期。这是 IPlugin 接口的设计本意：Load() 解决依赖绑定，Start() 启动运行，两个阶段之间所有插件的接口必须已注册完毕。

**修正**：`RunService()` 拆为两阶段——先循环加载所有插件（注册接口 + Load），再统一调用 `StartAll()`。

---

## L16: fnRouterHandler 精确匹配不支持路径参数，迁移前必须逐一检查

**来源**：Sprint 6 Phase 3 — WebPlugin 迁移

**问题**：`/api/operators/([^/]+)/activate` 依赖路径参数，fnRouterHandler 精确匹配无法处理，被迫改为 POST + body 传参，造成 API 破坏性变更。

**原则**：将业务插件迁移到 IRouterHandle 之前，必须逐一检查所有 handler 的路由模式：
1. 是否使用路径参数（`/resource/:id`）
2. 是否使用 GET + query string
3. 是否使用 multipart 上传

凡是依赖这些特性的 handler，需要提前设计替代方案（改为 POST + body），并评估对调用方的影响。

---

## L17: 守护进程与业务逻辑必须严格分离

**来源**：Sprint 6 Phase 4 — RunGuardian 设计

**原则**：守护进程（Guardian）只做 fork + waitpid + respawn，不加载任何插件，不开任何端口，不包含任何业务逻辑。业务逻辑全部在子进程中。这样守护进程可以被任何外部工具（docker-compose、systemd）完全替代，零代码改动。

---

## L18: Python 包必须在开发环境中显式安装，环境搭建步骤要写入文档

**来源**：Sprint 6 运行验证 — `python3 -m flowsql.worker` 报 ModuleNotFoundError

**问题**：`flowsql` Python 包位于 `src/python/`，但未安装到系统 Python 环境，导致 Guardian 拉起 pyworker 时报 `No module named 'flowsql'`。此问题已修复过一次，但换环境或重装系统后会重复出现。

**修复命令**：
```bash
pip3 install -e src/python/ --break-system-packages
```

**原则**：
1. 项目包含 Python 子包时，**环境搭建文档（README 或 CLAUDE.md）必须包含安装步骤**，不能依赖"之前装过"的隐性假设
2. 凡是"手动执行过一次"的环境配置，都要写入文档，否则换机器/换人必然重现
3. 开发环境检查脚本（或 Makefile target）应包含 `python3 -c "import flowsql"` 验证步骤

---

## L19: 前端 API 入口必须是 WebPlugin 的对外端口（8081），内部端口不对外暴露

**来源**：Sprint 7 前端 API 路由修复

**问题**：错误地将前端 baseURL 改为 Gateway 内部端口（18800），违反架构设计。

**正确架构**：
- 前端 → `http://localhost:8081`（WebPlugin 唯一对外入口）
- WebPlugin 收到请求后，内部转发给各服务插件（DatabasePlugin、SchedulerPlugin 等）
- 18800（Gateway）、18802/18803（RouterAgencyPlugin）等均为内部端口，前端不可见

**原则**：内部服务端口（Gateway、RouterAgencyPlugin）是服务间通信用的，不是前端直连的。前端只认一个入口：WebPlugin 的对外端口。

---

## L20: 服务间转发地址必须有独立配置项，不能复用其他地址，不能依赖隐式环境变量

**来源**：Sprint 6 后修复 — WebPlugin db_proxy 转发到 pyworker 而非 Gateway

**问题**：`web_plugin.cpp` 把内部转发地址（`SetSchedulerAddress`）设成了 `worker_host_:worker_port_`，注释说"Gateway 模式下两者相同"，但这依赖未文档化的环境变量 `FLOWSQL_GATEWAY_ADDR`。该变量未设置时，转发打到 pyworker（18900），pyworker 不认识 `/channels/database/*`，返回 404。

**原则**：
1. 凡是"服务间转发"的地址，必须有**独立的配置项**（如 `gateway=host:port`），不能复用其他服务的地址
2. 不能依赖隐式环境变量来修正错误的默认行为；默认值本身就应该是正确的
3. 设计文档必须明确写出每个转发目标的地址来源（配置项名称、默认值）
4. 转发失败时必须打印日志（目标地址、HTTP 状态码），便于快速定位

