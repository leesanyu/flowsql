# FlowSQL 产品待办列表

## Epic 1: C++ 框架核心能力
**优先级**: P0 | **状态**: ✅ 已完成 (Sprint 1)
**价值**: 建立插件式进程框架基础，支持 C++ 和 Python 算子统一数据交换

### Story 1.1: 统一数据接口设计
**状态**: ✅ 已完成 (Sprint 1)
**验收标准**: IDataEntity 和 IDataFrame 接口完成，支持 JSON 和 Arrow IPC 序列化

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 定义 DataType 枚举和 FieldValue variant
- ✅ 实现 IDataEntity 接口（GetSchema/GetFieldValue/SetFieldValue/ToJson/FromJson/Clone）
- ✅ 实现 IDataFrame 接口（行列操作/Arrow 互操作/序列化）
- ✅ 实现 DataFrame 类（两阶段模式：构建期 builders_ → 读取期 batch_）
</details>

---

### Story 1.2: 通道和算子接口
**状态**: ✅ 已完成 (Sprint 1)
**验收标准**: IChannel 和 IOperator 接口定义完成，支持插件化架构

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 定义 IChannel 接口（继承 IPlugin，含 Catelog/Name/Open/Close/Put/Get/Flush）
- ✅ 定义 IOperator 接口（继承 IPlugin，含 Work/Configure/Position）
- ✅ 定义 OperatorPosition 枚举（STORAGE/DATA）
- ✅ 实现注册宏（framework/macros.h）
</details>

---

### Story 1.3: 核心框架实现
**状态**: ✅ 已完成 (Sprint 1)
**验收标准**: PluginRegistry、Pipeline 和 Service 核心框架实现完成

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 PluginRegistry（LoadPlugin/UnloadAll/GetChannel/GetOperator/Traverse）
- ✅ 实现 Pipeline（Run 循环：批量读取 → Work → 批量写入）
- ✅ 实现 PipelineBuilder（链式构建：AddSource/SetOperator/SetSink/SetBatchSize/Build）
- ✅ 实现 Service 主框架
- ✅ Pipeline 状态机（IDLE/RUNNING/STOPPED/FAILED）
</details>

---

### Story 1.4: 示例插件和测试
**状态**: ✅ 已完成 (Sprint 1)
**验收标准**: MemoryChannel 和 PassthroughOperator 实现，test_framework 5 个用例全部通过

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 MemoryChannel（std::queue 存储，Put/Get/Open/Close）
- ✅ 实现 PassthroughOperator（直接复制输入到输出）
- ✅ 编写框架集成测试（插件加载/DataFrame 操作/Pipeline 数据流通）
- ✅ 验证现有 NPI 插件不受影响
</details>

---

### Story 1.5: Apache Arrow 集成
**状态**: ✅ 已完成 (Sprint 1)
**验收标准**: Arrow 依赖集成完成，支持 IPC 和 JSON 模块

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 编写 arrow-config.cmake（ExternalProject_Add）
- ✅ 配置编译选项（ARROW_BUILD_STATIC/ARROW_IPC/ARROW_JSON）
- ✅ CMakeLists.txt 集成（add_thirddepen 宏）
- ✅ 验证编译和链接
</details>

---

## Epic 2: Python 算子桥接与 Web 管理
**优先级**: P0 | **状态**: ✅ 已完成 (Sprint 2)
**价值**: 实现 C++ ↔ Python 桥接，提供 Web 管理界面，支持 Python 算子动态上传和执行

### Story 2.1: 插件系统增强（模块 P）
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: 三阶段加载、Start 失败回滚、动态注册/注销、单例模式

<details>
<summary>任务分解（点击展开）</summary>

- ✅ PluginLoader 改造（GetInterfaces/StartModules/StopModules/Unload 清理）
- ✅ PluginRegistry 重构（单例/双层索引/统一 Traverse/动态注册）
- ✅ 适配 Service 和 test_framework
- ✅ 新增 test_dynamic_register 验证
</details>

---

### Story 2.2: C++ ↔ Python 桥接（模块 A）
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: PythonOperatorBridge 实现，Python Worker 启动，算子自动发现和注册

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 ArrowIpcSerializer（使用 arrow::ipc::MakeStreamWriter）
- ✅ 实现 PythonOperatorBridge（Work 方法：Read → Serialize → HTTP POST → Deserialize → Write）
- ✅ 实现 PythonProcessManager（Start/WaitReady/Stop/IsAlive）
- ✅ 实现 BridgePlugin（IPlugin + IModule，动态注册 Python 算子）
- ✅ 实现 Python Worker（FastAPI + uvicorn）
- ✅ 实现 Python 算子框架（OperatorBase/OperatorRegistry/arrow_codec.py）
- ✅ 编写 test_bridge 验证
</details>

---

### Story 2.3: IChannel 重构
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: IChannel 基类只保留生命周期和元数据，数据读写下沉子类

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 重构 IChannel 接口（保留 Open/Close/Flush/Catelog/Name/Type/Schema）
- ✅ 定义 IDataFrameChannel 子接口
- ✅ 定义 IDatabaseChannel + IBatchReader/IBatchWriter 接口
- ✅ 实现 DataFrameChannel（std::shared_ptr<arrow::RecordBatch> + 互斥锁）
- ✅ 修改 IOperator::Work 签名为 Work(IChannel*, IChannel*)
- ✅ 适配所有现有插件和算子
- ✅ 删除 framework/macros.h（未使用的死代码）
</details>

---

### Story 2.4: SQL 解析器 + Pipeline 重构
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: SQL 解析器支持 SELECT...FROM...USING...WITH...INTO 语法

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 SqlParser（递归下降解析器）
- ✅ 支持 SELECT/FROM/USING/WITH/INTO 关键字解析
- ✅ 支持参数解析（key=value 格式）
- ✅ Pipeline 重构为纯连接器
- ✅ 集成测试验证
</details>

---

### Story 2.5: Web 后端 API
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: SQLite 数据库封装，WebServer 实现，全部 API 端点完成

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 SQLite 封装（Database 类：Open/Close/Execute/Query/Insert/ExecuteParams）
- ✅ 编写 schema.sql（建表 DDL）
- ✅ 实现 WebServer（httplib Server + 所有 Handler）
- ✅ 实现 GET /api/health
- ✅ 实现 GET /api/channels（从 PluginRegistry 同步）
- ✅ 实现 GET /api/operators
- ✅ 实现 POST /api/operators/upload（路径穿越防护）
- ✅ 实现 POST /api/operators/{name}/activate|deactivate
- ✅ 实现 GET /api/tasks
- ✅ 实现 POST /api/tasks（SQL 解析 → Pipeline 执行）
- ✅ 实现 GET /api/tasks/{id}/result
- ✅ 实现 flowsql_web 入口（main.cpp：加载插件 → 预填测试数据 → 启动 WebServer）
</details>

---

### Story 2.6: Vue.js 前端
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: Vue 3 + Vite + Element Plus，Dashboard/Channels/Operators/Tasks 页面完成

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 项目脚手架搭建（Vue 3 + Vite + Element Plus）
- ✅ 实现 Dashboard.vue
- ✅ 实现 Channels.vue
- ✅ 实现 Operators.vue
- ✅ 实现 Tasks.vue
- ✅ 实现 Sidebar.vue 导航组件
- ✅ 实现 API 封装（api/index.js）
- ✅ 实现 Vue Router 配置
- ✅ 编写构建脚本和测试脚本
</details>

---

### Story 2.7: 端到端集成测试
**状态**: ✅ 已完成 (Sprint 2)
**验收标准**: Python 算子完整执行链路验证，算子上传激活功能验证

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 编写端到端测试脚本
- ✅ 验证 Python 算子执行链路
- ✅ 验证多 SQL 串联
- ✅ 验证算子上传激活
- ✅ 回归测试现有功能
</details>

---

## Epic 3: 数据库闭环与架构清理
**优先级**: P0 | **状态**: ✅ 已完成 (Sprint 3)
**价值**: 补齐数据库读写闭环，清理架构债务，回归纯插件架构

### Story 3.1: 架构重构 - 纯插件架构回归
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 删除 PluginRegistry 和 libflowsql_framework.so，回归纯 PluginLoader 架构

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 拆分 common/iplugin.h（从 framework 拆出）
- ✅ 修改 common/loader.hpp（实现 IRegister + IQuerier）
- ✅ 新增 common/iquerier.hpp（插件查询接口）
- ✅ 删除 PluginRegistry 相关文件
- ✅ 移动 Pipeline/ChannelAdapter 到 scheduler.so
- ✅ 清理 framework 库依赖
</details>

---

### Story 3.2: 接口解耦
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: IChannel/IOperator 去掉 IPlugin 继承，纯接口设计

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 修改 framework/interfaces/ichannel.h（去掉 IPlugin 继承）
- ✅ 修改 framework/interfaces/ioperator.h（去掉 IPlugin 继承）
- ✅ 适配所有实现类（显式多继承）
- ✅ 验证编译和测试
</details>

---

### Story 3.3: IBridge 接口
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 替代 dynamic_cast<IRegister*> hack，提供 Python 算子查询和刷新能力

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 定义 framework/interfaces/ibridge.h
- ✅ 修改 services/bridge/bridge_plugin.h（多继承 IBridge）
- ✅ 实现 FindOperator/TraverseOperators/Refresh 方法
- ✅ Scheduler 集成 IBridge 查询
- ✅ Web 端实现算子刷新 API
</details>

---

### Story 3.4: arrow_codec.py 统一转换层
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 兼容 Polars/Pandas/Arrow Table，简化 Python 算子开发

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 _ensure_arrow_table() 函数
- ✅ 支持 Polars/Pandas/Arrow 自动检测和转换
- ✅ 集成到 Python Worker
- ✅ 更新示例算子
</details>

---

### Story 3.5: IDatabaseFactory 工厂接口 + DatabasePlugin
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 数据库通道工厂实现，支持多数据库实例管理

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 定义 framework/interfaces/idatabase_factory.h
- ✅ 实现 services/database/database_plugin.h/.cpp
- ✅ 实现配置解析（Option() 方法）
- ✅ 实现环境变量替换
- ✅ 实现 Get/List/Release 方法
</details>

---

### Story 3.6: IDbDriver 驱动抽象 + SQLite 驱动
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 数据库驱动抽象层和 SQLite 驱动实现完成

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 定义 services/database/idb_driver.h
- ✅ 实现 services/database/drivers/sqlite_driver.h/.cpp
- ✅ 实现 SqliteBatchReader（流式读取 + Arrow IPC 序列化）
- ✅ 实现 SqliteBatchWriter（IPC 反序列化 + 自动建表 + 批量写入）
- ✅ 支持只读模式和 WAL 模式
</details>

---

### Story 3.7: DatabaseChannel 通道实现
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: IDatabaseChannel 接口实现，委托所有数据库操作给 IDbDriver

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 services/database/database_channel.h/.cpp
- ✅ 实现 Open/Close/IsOpened 方法
- ✅ 实现 CreateReader/CreateWriter 委托
- ✅ 实现元数据方法（Type/Catelog/Name）
</details>

---

### Story 3.8: SQL WHERE 解析 + DataFrame Filter
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: WHERE 子句解析和 DataFrame 过滤能力实现

<details>
<summary>任务分解（点击展开）</summary>

- ✅ SqlParser 新增 WHERE 解析
- ✅ 实现 ValidateWhereClause()（SQL 注入防护）
- ✅ 实现 DataFrame::Filter()（支持 6 种操作符）
- ✅ 支持多种数据类型比较
- ✅ 集成测试验证
</details>

---

### Story 3.9: 安全基线
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: SQL 注入防护、只读模式和环境变量替换实现

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 WHERE 子句注入防护
- ✅ 实现 SQLite 只读模式
- ✅ 实现环境变量替换
- ✅ 安全测试验证
</details>

---

### Story 3.10: Scheduler 集成
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: Scheduler 集成数据库通道，支持四层查找和 SQL 生成

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 FindChannel() 四层查找
- ✅ 支持三段式和两段式通道引用
- ✅ 实现 BuildQuery()（SQL 生成）
- ✅ 集成测试验证
</details>

---

### Story 3.11: ChannelAdapter 自动适配
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 去掉显式存储/提取算子，Pipeline/Scheduler 层自动感知通道类型差异

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 实现 ChannelAdapter 工具类
- ✅ 实现 4 种无算子路径
- ✅ 实现有算子时的自动适配
- ✅ 修改 SQL 解析器（USING 可选）
- ✅ 集成测试验证
</details>

---

### Story 3.12: 端到端测试
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 14 项端到端测试全部通过

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 编写配置解析测试
- ✅ 编写 SQLite 连接测试
- ✅ 编写 Reader/Writer 测试
- ✅ 编写 SQL 解析器测试
- ✅ 编写 DataFrame Filter 测试
- ✅ 编写安全基线测试
- ✅ 编写 6 个 E2E 场景测试
</details>

---

### Story 3.13: 代码审查修复
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 修复代码审查中发现的 9 个问题（P1×6 + P2×3）

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 修复 P1 问题（6 项）
- ✅ 修复 P2 问题（3 项）
- ✅ 回归测试验证
</details>

---

### Story 3.14: 清理任务
**状态**: ✅ 已完成 (Sprint 3)
**验收标准**: 删除 IDataEntity 相关死代码

<details>
<summary>任务分解（点击展开）</summary>

- ✅ 删除 idata_entity.h
- ✅ DataType/FieldValue/Field 移入 idataframe.h
- ✅ 清理 DataFrame 中 AppendEntity/GetEntity
- ✅ 清理 test_bridge 无用 include
- ✅ 更新 stage3.md 文档
</details>

---

## Epic 4: 多数据库支持与 SQL 增强
**优先级**: P1 | **状态**: ✅ 已完成 (Sprint 4)
**价值**: 扩展数据库支持，实现 MySQL 驱动和连接池，增强 SQL 能力

### Story 4.1: MySQL 驱动支持
**状态**: ✅ 已完成 (Sprint 4)
**验收标准**:
- 实现 MysqlDriver（基于 libmysqlclient）✅
- 支持连接池管理 ✅
- 支持事务控制（COMMIT/ROLLBACK）✅
- 端到端测试通过（19 个测试用例）✅

---

### Story 4.2: PostgreSQL 驱动支持
**状态**: 📋 待规划
**验收标准**:
- 实现 PostgresDriver（基于 libpq）
- 支持连接池管理
- 支持预编译语句
- 支持事务控制
- 端到端测试通过

---

### Story 4.3: 数据库连接池基础实现
**状态**: ✅ 已完成 (Sprint 4)
**验收标准**:
- ConnectionPool<T> 泛型连接池 ✅
- 支持连接复用和空闲超时回收 ✅
- 支持最大连接数限制 ✅
- 支持健康检查（心跳机制）✅
- 连接池单元测试通过（7 个测试用例）✅

---

### Story 4.4: SQL 高级特性
**状态**: ✅ 已完成 (Sprint 4)
**验收标准**:
- 支持 GROUP BY 和聚合函数 ✅
- 支持 ORDER BY 和 LIMIT ✅
- 支持子查询（透传给数据库引擎）✅
- sql_part 提取与 BuildQuery 替换逻辑 ✅

---

## Epic 5: ClickHouse 数据库通道
**优先级**: P1 | **状态**: ✅ 已完成（2026-03-12）
**价值**: 支持列式数据库 ClickHouse，走 Arrow 原生路径实现高效批量读写

### Story 5.1: ClickHouseDriver 核心实现
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 实现 ClickHouseDriver（基于 httplib，HTTP 8123 端口，零新依赖）
- 实现 ClickHouseSession，同时实现 IArrowReadable + IArrowWritable
- 查询走 `FORMAT ArrowStream`，写入走 `INSERT INTO table FORMAT ArrowStream`
- 认证：`X-ClickHouse-User` / `X-ClickHouse-Key` header
- 配置格式：`type=clickhouse;name=ch1;host=...;port=8123;user=...;password=...;database=...`

---

### Story 5.2: DatabasePlugin 集成 ClickHouse
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- `CreateDriver()` 增加 clickhouse 分支
- `DatabaseChannel::CreateArrowReader/CreateArrowWriter` 从返回 -1 改为 dynamic_cast 检查 IArrowReadable/IArrowWritable
- 现有 SQLite/MySQL 测试不受影响

---

### Story 5.3: 端到端测试
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 新增 test_clickhouse.cpp，覆盖连接、DDL、写入、读取、Arrow 类型矩阵
- ClickHouse 不可达时自动 SKIP
- 所有现有测试回归通过（T1-T16 全部通过）
- test_plugin_e2e.cpp 补充 E1-E7 插件层 E2E（PluginLoader → IDatabaseFactory → IDatabaseChannel 完整路径），全部通过

---

## Epic 6: Web 管理数据库通道
**优先级**: P1 | **状态**: ✅ 已完成（2026-03-12）
**价值**: 将数据库通道配置从 gateway.yaml 静态配置迁移到 Web 动态管理，支持运行时增删改

### 设计决策
- **配置权威方**：DatabasePlugin 自持久化（写自己的 SQLite 文件），Web 服务只是操作入口
- **密码存储**：AES-256-GCM 加密后存储，密钥从环境变量 `FLOWSQL_SECRET_KEY` 读取
- **跨进程通信**：Web → Gateway → Scheduler → DatabasePlugin，与现有架构一致
- **废弃**：`gateway.yaml` 中的 `databases:` 数组

### Story 6.1: DatabasePlugin 持久化与动态管理
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 新增 `IDatabaseManager` 接口：`AddChannel` / `RemoveChannel` / `UpdateChannel` / `ListChannels`
- `Start()` 从 SQLite 文件加载已保存的通道配置
- 密码字段 AES-256-GCM 加密存储，读取时解密
- `AddChannel()` 后无需重启即可使用新通道

---

### Story 6.2: Scheduler 新增管理端点
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 新增 `POST /db-channels/add`、`/db-channels/remove`、`/db-channels/update`、`GET /db-channels`
- 通过 IQuerier 找到 IDatabaseManager 并调用对应方法

---

### Story 6.3: Web 服务 CRUD API
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 新增 `GET/POST/PUT/DELETE /api/db-channels` 端点
- 密码字段前端展示脱敏（显示 `****`）
- 操作后通过 Gateway 通知 Scheduler

---

### Story 6.4: 废弃 gateway.yaml 静态配置
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- 删除 `gateway.yaml` 中的 `databases:` 数组
- DatabasePlugin option 改为 `db_path=/tmp/flowsql_db_channels.db`
- 向后兼容：旧配置格式保留解析能力但不再生成

---

### Story 6.5: 前端通道管理 UI
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- `Channels.vue` 新增数据库通道增删改对话框
- 支持 SQLite / MySQL / ClickHouse 三种类型，动态显示对应字段
- 密码字段 `type="password"`，展示时脱敏

---

### Story 6.6: 端到端测试
**状态**: ✅ 已完成（2026-03-12）
**验收标准**:
- Web UI 新增通道 → Scheduler 立即可用 → 重启后配置持久化
- 删除通道后 SQL 执行返回 "channel not found"

---

### Story 6.7: 数据库错误信息透传架构改造
**状态**: ✅ 已完成 (Sprint 6)
**优先级**: P1
**背景**: `IBatchReadable::CreateReader` 接口无 `error*` 参数，底层驱动（MySQL/ClickHouse）的错误信息（如 `No database selected`、`Table doesn't exist`）在 `RelationDbSessionBase::CreateReader` 中被捕获后丢弃，调用方只能看到 `CreateReader failed`，调试成本高。
**验收标准**:
- `IBatchReadable::CreateReader` 接口增加错误输出参数（或等效机制），将底层错误字符串透传给调用方
- MySQL / ClickHouse / SQLite 三个驱动均实现透传
- Scheduler 层将具体错误信息返回给 HTTP 调用方（而非通用失败消息）
- 新增测试：构造"数据库不存在"、"表不存在"场景，断言错误消息包含具体原因

<details>
<summary>设计要点（点击展开）</summary>

- 方案 A：`CreateReader(error_out*)` 参数扩展——接口侵入性最小，但需修改所有实现
- 方案 B：`thread_local` 错误槽（类似 `errno`）——零接口变更，但跨线程语义需谨慎
- 方案 C：返回 `Result<Reader, Error>` 包装类型——最符合现代 C++ 风格，但改动面最大
- 推荐在规划时评估三种方案的影响范围后决策

</details>

---

## Epic 7: 路由代理与服务对等化架构改造
**优先级**: P1 | **状态**: ✅ 已完成 (Sprint 6)
**价值**: 解耦插件路由、实现服务对等、统一 API 设计，为后续扩展奠定架构基础
**设计文档**: `docs/design_router_agency.md`

### Story 7.1: 基础设施 — IRouterHandle 接口 + 错误码 + PluginLoader 批次调用
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 定义 `irouter_handle.h`（IRouterHandle + RouteItem + fnRouterHandler）
- 定义 `error_code.h`（6 个业务错误码 + HttpStatus 映射）
- 修正 `main.cpp` RunService 为两阶段加载（Load 全部 → StartAll）

<details>
<summary>任务分解（点击展开）</summary>

- 📋 新增 `src/framework/interfaces/irouter_handle.h`：IRouterHandle 接口 + RouteItem + fnRouterHandler 签名
- 📋 新增 `src/common/error_code.h`：OK/BAD_REQUEST/NOT_FOUND/CONFLICT/INTERNAL_ERROR/UNAVAILABLE
- 📋 修正 `src/app/main.cpp` RunService：分离 Load 阶段和 Start 阶段，确保所有插件 Load() 完成后再统一 StartAll()
- 📋 单元测试：验证两阶段加载顺序正确
</details>

---

### Story 7.2: RouterAgencyPlugin 实现
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 实现 libflowsql_router.so（RouteCollector + HttpServer + GatewayRegistrar + ErrorMapper）
- 路由收集（Traverse IRouterHandle）+ 冲突检测（先到先得 + 日志告警）
- HTTP Dispatch + CORS 统一处理 + 错误码→HTTP 状态码映射
- KeepAlive 线程（定期向 Gateway 注册路由前缀，幂等）

<details>
<summary>任务分解（点击展开）</summary>

- 📋 新增 `src/services/router/` 目录：router_agency_plugin.h/cpp + plugin_register.cpp + CMakeLists.txt
- 📋 实现 RouteCollector：Traverse(IID_ROUTER_HANDLE) 收集路由 + 冲突检测 + 前缀提取
- 📋 实现 HttpServer：httplib::Server + catch-all Dispatch + CORS 统一处理
- 📋 实现 GatewayRegistrar：KeepAlive 线程 + POST /gateway/register
- 📋 实现 ErrorMapper：业务错误码 → HTTP 状态码映射
- 📋 集成测试：路由收集、分发、CORS、错误码映射
</details>

---

### Story 7.3: Gateway 改造 — 字典树路由 + 过期清理 + 瘦身
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- RouteTable 改为字典树（Trie）匹配，废弃 ExtractPrefix/StripPrefix
- HandleForward 不再剥离前缀，转发完整 URI
- 增加路由过期清理线程（CleanupThread），移除超过 3 倍 KeepAlive 间隔未更新的路由
- 删除 ServiceManager、HeartbeatThread、HandleHeartbeat
- 删除 ServiceClient

<details>
<summary>任务分解（点击展开）</summary>

- 📋 重写 `route_table.h/cpp`：字典树实现（Insert/Match/RemoveExpired），RouteEntry 增加 last_seen_ms
- 📋 修改 `gateway_plugin.cpp`：HandleForward 转发完整 URI，不剥离前缀
- 📋 新增 CleanupThread：定期移除过期路由条目
- 📋 新增 `/gateway/register`、`/gateway/unregister`、`/gateway/routes` 端点
- 📋 删除 ServiceManager、HeartbeatThread、HandleHeartbeat、ServiceClient
- 📋 测试：字典树匹配、路由注册/过期清理、转发完整 URI
</details>

---

### Story 7.4: 业务插件迁移 — SchedulerPlugin
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 实现 IRouterHandle，声明 /channels/dataframe/*、/operators/*、/tasks/instant/*
- 删除内部 httplib::Server 和 RegisterRoutes()
- 删除 /db-channels/* 相关 Handler（移交 DatabasePlugin）

<details>
<summary>任务分解（点击展开）</summary>

- 📋 SchedulerPlugin 多继承 IRouterHandle，实现 EnumRoutes()
- 📋 将现有 Handler 改为 fnRouterHandler 签名（uri, req_json, rsp_json）
- 📋 删除 httplib::Server server_ 成员和 RegisterRoutes()
- 📋 删除 /db-channels/* 相关 Handler
- 📋 plugin_register.cpp 增加 IID_ROUTER_HANDLE 注册
- 📋 回归测试：所有现有 API 功能不受影响
</details>

---

### Story 7.5: 业务插件迁移 — DatabasePlugin
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 实现 IRouterHandle，声明 /channels/database/*
- Handler 逻辑从 SchedulerPlugin 迁移过来

<details>
<summary>任务分解（点击展开）</summary>

- 📋 DatabasePlugin 多继承 IRouterHandle，实现 EnumRoutes()
- 📋 实现 HandleAdd/HandleRemove/HandleModify/HandleQuery
- 📋 plugin_register.cpp 增加 IID_ROUTER_HANDLE 注册
- 📋 测试：数据库通道 CRUD 通过 RouterAgencyPlugin 分发正常工作
</details>

---

### Story 7.6: 业务插件迁移 — WebPlugin
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 实现 IRouterHandle（管理 API 部分）
- 保留 Web 服务器（静态文件），管理 API 走 RouterAgencyPlugin 内部端口

<details>
<summary>任务分解（点击展开）</summary>

- 📋 WebPlugin 多继承 IRouterHandle，实现 EnumRoutes()（管理 API）
- 📋 保留 httplib::Server 用于静态文件服务
- 📋 管理 API Handler 改为 fnRouterHandler 签名
- 📋 测试：Web UI 正常访问，管理 API 通过 RouterAgencyPlugin 正常工作
</details>

---

### Story 7.7: 进程管理改造 — fork 守护 + docker-compose
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- main.cpp RunGateway → RunGuardian（极简 fork + waitpid + respawn）
- 编写 Dockerfile + docker-compose.yml + docker-compose.full.yml
- 更新 gateway.yaml 配置格式（per-plugin option）

<details>
<summary>任务分解（点击展开）</summary>

- 📋 重写 main.cpp RunGateway 为 RunGuardian：fork + waitpid + respawn，零业务逻辑
- 📋 编写 Dockerfile（基于 ubuntu:22.04，同一镜像多角色）
- 📋 编写 docker-compose.yml（gateway + web + scheduler + pyworker）
- 📋 编写 docker-compose.full.yml（含 MySQL + ClickHouse）
- 📋 更新 gateway.yaml 配置格式
- 📋 测试：fork 守护进程管理、docker-compose 部署验证
</details>

---

### Story 7.8: 端到端验证
**状态**: ✅ 已完成 (Sprint 6)
**验收标准**:
- 路由收集和分发测试
- KeepAlive + Gateway 故障恢复测试
- 资源导向 URI 全路由回归测试
- docker-compose 部署验证

<details>
<summary>任务分解（点击展开）</summary>

- 📋 路由收集测试：多插件路由收集、冲突检测、前缀提取
- 📋 路由分发测试：完整 URI 转发、错误码映射、CORS
- 📋 KeepAlive 测试：正常注册、Gateway 重启恢复、服务崩溃路由过期
- 📋 全路由回归：channels/database/*、channels/dataframe/*、operators/*、tasks/*
- 📋 docker-compose 部署：多容器启动、服务发现、故障重启
</details>

---

## Epic 8: Pipeline 增强与异步任务
**优先级**: P1 | **状态**: 📋 待规划
**价值**: 增强 Pipeline 编排能力，支持异步任务执行，提升系统易用性

### Story 8.1: 多算子 Pipeline
**状态**: 📋 待规划
**验收标准**:
- 支持算子链式调用（USING op1 THEN op2 THEN op3）
- 支持算子间数据传递
- 支持算子并行执行
- 支持算子错误处理和重试

---

### Story 8.2: 异步任务执行
**状态**: 📋 待规划
**验收标准**:
- 任务队列实现（基于线程池）
- 任务状态跟踪（pending/running/completed/failed）
- 任务取消和超时控制
- 任务结果持久化
- WebSocket 实时推送任务状态

---

## Epic 9: 流式架构
**优先级**: P2 | **状态**: 📋 设计阶段
**价值**: 支持流式数据处理，满足网络性能分析等实时场景

### Story 9.1: IStreamChannel 接口设计
**状态**: 📋 设计阶段
**验收标准**:
- 定义 IStreamChannel 接口（基于描述符）
- 支持 DPDK 大页内存零拷贝
- 支持背压机制
- 支持流式数据分片

---

### Story 9.2: IStreamOperator 接口设计
**状态**: 📋 设计阶段
**验收标准**:
- 定义 IStreamOperator 接口
- 支持流式数据处理
- 支持窗口操作（滑动窗口/滚动窗口）
- 支持状态管理

---

### Story 9.3: StreamWorker 通用容器
**状态**: 📋 设计阶段
**验收标准**:
- 实现 StreamWorker 容器
- 支持算子动态加载
- 支持算子生命周期管理
- 支持算子间通信

---

### Story 9.4: Scheduler 流式调度
**状态**: 📋 设计阶段
**验收标准**:
- Scheduler 支持流式任务调度
- 支持三种角色（执行者/宿主/编排者）
- 支持任务拓扑管理
- 支持故障恢复

---

### Story 9.5: DPDK 网卡采集插件
**状态**: 📋 设计阶段
**验收标准**:
- 实现 netcard 插件（基于 DPDK）
- 支持网卡数据包采集
- 支持零拷贝传输
- 支持多队列并行

---

### Story 9.6: 网络性能分析算子
**状态**: 📋 设计阶段
**验收标准**:
- 实现 npm 算子（网络性能分析）
- 支持流量统计
- 支持协议解析
- 支持异常检测

---

## Epic 10: 平台增强与用户认证
**优先级**: P2 | **状态**: 📋 待规划
**价值**: 提升系统可观测性、可维护性、易用性和安全性

### Story 10.1: 用户认证与权限
**状态**: 📋 待规划
**验收标准**:
- 用户注册和登录（JWT Token）
- 基于角色的权限控制（RBAC）
- 通道和算子访问权限
- 操作审计日志
- Session 管理

---

### Story 10.2: 监控和告警
**状态**: 📋 待规划
**验收标准**:
- Prometheus 指标导出
- Grafana 仪表盘
- 告警规则配置
- 告警通知（邮件/钉钉/企业微信）

---

### Story 10.3: 日志聚合
**状态**: 📋 待规划
**验收标准**:
- 结构化日志输出（JSON 格式）
- 日志级别控制
- 日志轮转和归档
- ELK 集成

---

### Story 10.4: 配置中心
**状态**: 📋 待规划
**验收标准**:
- 配置热更新
- 配置版本管理
- 配置回滚
- 配置审计

---

### Story 10.5: 插件市场
**状态**: 📋 待规划
**验收标准**:
- 插件上传和下载
- 插件版本管理
- 插件依赖管理
- 插件评分和评论

---

### Story 10.6: 文档和示例
**状态**: 📋 待规划
**验收标准**:
- 用户手册
- 开发者指南
- API 文档
- 示例项目

## 优先级说明
- **P0**: 核心功能，必须实现
- **P1**: 重要功能，近期规划
- **P2**: 增强功能，中长期规划
- **P3**: 可选功能，按需实现

## 状态说明
- ✅ **已完成**: Story 已实施并验证通过
- 🚧 **进行中**: Story 正在实施
- 📋 **待规划**: Story 已识别，待排入 Sprint
- 💡 **设计阶段**: Story 需求明确，设计方案待定
