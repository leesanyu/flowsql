# Sprint 5 规划

## Sprint 信息

- **Sprint 周期**：Sprint 5
- **开始日期**：2026-03-12
- **预计工作量**：11 天
- **Sprint 目标**：实现 ClickHouse 数据库通道驱动，并将数据库通道配置从静态 gateway.yaml 迁移到 Web 动态管理

---

## Sprint 目标

### 主要目标

1. 实现 ClickHouseDriver，走 Arrow 原生路径（IArrowReadable/IArrowWritable），支持高效批量读写
2. 将数据库通道配置权威方迁移到 DatabasePlugin 自持久化，废弃 gateway.yaml 静态配置
3. 提供 Web UI 对数据库通道进行增删改，支持 SQLite/MySQL/ClickHouse 三种类型

### 成功标准

- ClickHouse 通道端到端测试通过（含 Arrow 类型矩阵覆盖）✅
- Web UI 新增通道后无需重启立即可用 ✅
- Scheduler 重启后从 flowsql.yml 自动恢复通道配置 ✅
- gateway.yaml 中不再有 `databases:` 配置项 ✅
- 密码 AES-256-GCM 加密存储，前端展示脱敏 ✅
- 所有现有测试回归通过 ✅

---

## Story 列表

---

### Story 5.1：ClickHouseDriver 核心实现（Epic 5）✅

**优先级**：P0（Epic 5 基础）
**工作量估算**：2 天
**依赖**：无
**完成日期**：2026-03-12

**验收标准**：
- [x] `ClickHouseDriver` 实现 `IDbDriver`，连接参数：host/port/user/password/database
- [x] `Connect()` 发 `GET /?query=SELECT+1` 健康检查，失败返回 -1
- [x] `Ping()` 同上，`IsConnected()` 基于最近 Ping 结果
- [x] `ClickHouseSession` 同时实现 `IDbSession` + `IArrowReadable` + `IArrowWritable`
- [x] `ExecuteQueryArrow()` 构造 `{sql} FORMAT ArrowStream`，POST 到 ClickHouse，用 `RecordBatchStreamReader` 解析响应体
- [x] `WriteArrowBatches()` 用 `MakeStreamWriter` 序列化，POST 到 `INSERT INTO {table} FORMAT ArrowStream`
- [x] `ExecuteSql()` 走普通 HTTP 文本响应（用于 DDL）
- [x] 事务方法返回 -1（ClickHouse 不支持传统事务）
- [x] `LastError()` 返回有意义的错误信息

**任务分解**：

#### Task 5.1.1：ClickHouseDriver 头文件（0.3 天）✅
- [x] 新建 `src/services/database/drivers/clickhouse_driver.h`
- [x] 声明 `ClickHouseDriver`、`ClickHouseSession`、`ClickHouseConnection` 结构体
- [x] 参照 `mysql_driver.h` 风格

#### Task 5.1.2：ClickHouseDriver 实现（1.2 天）✅
- [x] 新建 `src/services/database/drivers/clickhouse_driver.cpp`
- [x] 实现 `Connect()`：解析参数，发健康检查请求
- [x] 实现 `ExecuteQueryArrow()`：构造带 `FORMAT ArrowStream` 的 SQL，POST，解析 Arrow IPC Stream
- [x] 实现 `WriteArrowBatches()`：序列化 batches 为 Arrow IPC Stream，POST INSERT
- [x] 实现 `ExecuteSql()`：POST 普通 SQL，检查 HTTP 200
- [x] 实现 `CreateSession()`：返回 `ClickHouseSession`

#### Task 5.1.3：DatabasePlugin 集成（0.5 天）✅
- [x] 修改 `database_plugin.cpp` `CreateDriver()`：增加 `if (type == "clickhouse")` 分支
- [x] 修改 session_factory lambda：增加 `dynamic_cast<ClickHouseDriver*>` 分支
- [x] 修改 `database_channel.cpp` `CreateArrowReader/CreateArrowWriter`：从返回 -1 改为 `dynamic_cast<IArrowReadable/IArrowWritable>` 检查

---

### Story 5.2：ClickHouse 端到端测试（Epic 5）✅

**优先级**：P0
**工作量估算**：1 天
**依赖**：Story 5.1
**完成日期**：2026-03-12

**验收标准**：
- [x] 新建 `src/tests/test_database/test_clickhouse.cpp`
- [x] `IsClickHouseAvailable()` 检查，不可达时全部 SKIP
- [x] 测试覆盖：连接成功/失败、DDL（建表/删表）、写入、读取、Arrow 类型矩阵（INT32/INT64/FLOAT/DOUBLE/STRING/BOOLEAN）
- [x] 每个测试函数自建自清（时间戳后缀唯一表名）
- [x] `CMakeLists.txt` 加入 `test_clickhouse` 目标，加 `-UNDEBUG`
- [x] 所有现有 SQLite/MySQL 测试回归通过
- [x] `test_plugin_e2e.cpp` 补充 E1-E7（PluginLoader → IDatabaseFactory → IDatabaseChannel 完整路径）

**任务分解**：

#### Task 5.2.1：测试程序（0.7 天）✅
- [x] 新建 `src/tests/test_database/test_clickhouse.cpp`
- [x] 参照 `test_mysql.cpp` 结构

#### Task 5.2.2：CMake 集成（0.1 天）✅
- [x] 修改 `src/tests/test_database/CMakeLists.txt`

#### Task 5.2.3：代码审查 + 修复（0.2 天）✅
- [x] 审查 ClickHouseDriver 实现，修复发现的问题
- [x] T7 类型矩阵补全逐列逐行验证（c_int64/float32/float64/string/bool）
- [x] T8 大批量写入补全 count 值断言（count()=10000）
- [x] 16 个用例全部通过，0 跳过
- [x] E1-E7 插件层 E2E 补充实现，全部通过

---

### Story 6.1：DatabasePlugin 持久化与动态管理（Epic 6）

**优先级**：P0（Epic 6 基础）
**工作量估算**：2 天
**依赖**：无（与 Epic 5 并行可行，但建议串行）

**验收标准**：
- [ ] `IDatabaseFactory` 扩展管理方法（不新增 `IDatabaseManager` 接口，`plugin_register.cpp` 无需修改）
- [ ] `Option()` 解析 `config_file=config/flowsql.yml`
- [ ] `Start()` 加载 `flowsql.yml` 的 `channels.database_channels` 节点
- [ ] `AddChannel()`：type+name 已存在返回 -1（不覆盖，需用 UpdateChannel）
- [ ] `RemoveChannel()`：关闭连接 + 从内存移除 + `SaveToYaml()`
- [ ] `UpdateChannel()`：原子覆盖写 YAML（不走 Remove+Add）
- [ ] `List()`：扩展签名增加 config_json 参数，密码字段脱敏为 `"****"`
- [ ] 密码 AES-256-GCM 加密（OpenSSL），密钥从 `FLOWSQL_SECRET_KEY` 环境变量读取
- [ ] `config_file` 为空时：`Start()` 直接返回 0，`AddChannel` 返回 -1

**任务分解**：

#### Task 6.1.1：IDatabaseFactory 接口扩展（0.3 天）
- [ ] 修改 `src/framework/interfaces/idatabase_factory.h`，合并管理方法，扩展 `List` 签名

#### Task 6.1.2：DatabasePlugin YAML 持久化（1 天）
- [ ] 修改 `database_plugin.h`：新增 `config_file_`、`LoadFromYaml()`、`SaveToYaml()`、加解密方法
- [ ] 修改 `database_plugin.cpp`：`Option()` 解析 `config_file`；`Start()` 加载配置；`Stop()` 无需额外操作

#### Task 6.1.3：动态管理方法（0.5 天）
- [ ] 实现 `AddChannel()`、`RemoveChannel()`、`UpdateChannel()`、`List()`

#### Task 6.1.4：密码加解密（0.2 天）
- [ ] 实现 `EncryptPassword()` / `DecryptPassword()`（AES-256-GCM，OpenSSL）
- [ ] 确认 CMakeLists.txt 链接 OpenSSL

---

### Story 6.2：Scheduler 新增通道管理端点（Epic 6）

**优先级**：P1
**工作量估算**：0.5 天
**依赖**：Story 6.1

**验收标准**：
- [ ] 新增 `POST /db-channels/add`：解析 JSON body，调用 `IDatabaseFactory::AddChannel()`
- [ ] 新增 `POST /db-channels/remove`：调用 `RemoveChannel()`
- [ ] 新增 `POST /db-channels/update`：调用 `UpdateChannel()`
- [ ] 新增 `GET /db-channels`：调用 `List()`，返回 JSON（密码脱敏）
- [ ] 通过 `IQuerier` 查找 `IID_DATABASE_FACTORY`，找不到时返回 503

**任务分解**：

#### Task 6.2.1：Scheduler 端点实现（0.5 天）
- [ ] 修改 `scheduler_plugin.h`：新增 Handler 声明
- [ ] 修改 `scheduler_plugin.cpp`：`RegisterRoutes()` 注册新路由，实现 Handler

---

### Story 6.3：Web 服务 CRUD API（Epic 6）

**优先级**：P1
**工作量估算**：1 天
**依赖**：Story 6.2

**验收标准**：
- [ ] `GET /api/db-channels`：转发到 `GET /scheduler/db-channels`，返回通道列表
- [ ] `POST /api/db-channels`：验证参数，转发到 `POST /scheduler/db-channels/add`
- [ ] `PUT /api/db-channels/:id`：转发到 `POST /scheduler/db-channels/update`
- [ ] `DELETE /api/db-channels/:id`：转发到 `POST /scheduler/db-channels/remove`
- [ ] 密码字段在返回给前端时脱敏（替换为 `"****"`）

**任务分解**：

#### Task 6.3.1：Web API 实现（1 天）
- [ ] 修改 `web_server.h`：新增 Handler 声明，新增 `NotifyDatabasePlugin()` 方法
- [ ] 修改 `web_server.cpp`：实现 Handler，注册路由

---

### Story 6.4：废弃 gateway.yaml 静态配置（Epic 6）

**优先级**：P1
**工作量估算**：0.5 天
**依赖**：Story 6.1

**验收标准**：
- [ ] `config/gateway.yaml` 删除 `databases:` 数组，DatabasePlugin option 改为 `config_file=config/flowsql.yml`
- [ ] `config/gateway.example.yaml` 同步更新
- [ ] `src/services/gateway/service_manager.cpp` 不再拼接 `--databases` 参数，改为传递 `option`
- [ ] `config/flowsql.yml` 新建（初始为空）
- [ ] `config.h` 中 `ServiceConfig::databases` 字段可保留（向后兼容），但 `service_manager.cpp` 不再使用

**任务分解**：

#### Task 6.4.1：配置清理（0.5 天）
- [ ] 修改上述文件

---

### Story 6.5：前端通道管理 UI（Epic 6）

**优先级**：P1
**工作量估算**：2 天
**依赖**：Story 6.3

**验收标准**：
- [ ] `Channels.vue` 新增"新增数据库通道"按钮
- [ ] 对话框支持类型选择（SQLite/MySQL/ClickHouse），动态显示对应字段
  - SQLite：name, path
  - MySQL：name, host, port(3306), user, password, database, charset(utf8mb4)
  - ClickHouse：name, host, port(8123), user, password, database
- [ ] 密码字段 `type="password"`，列表展示时显示 `****`
- [ ] 支持编辑已有通道（点击编辑按钮，密码字段留空表示不修改）
- [ ] 支持删除通道（二次确认）
- [ ] 调用 `POST/PUT/DELETE /api/db-channels`

**任务分解**：

#### Task 6.5.1：前端 API 封装（0.3 天）
- [ ] 修改 `src/frontend/src/api/index.js`，新增 db-channels 相关调用

#### Task 6.5.2：Channels.vue 改造（1.5 天）
- [ ] 修改 `src/frontend/src/views/Channels.vue`
- [ ] 新增对话框组件，动态表单逻辑

#### Task 6.5.3：联调测试（0.2 天）
- [ ] 前后端联调，验证增删改流程

---

### Story 6.6：端到端测试与代码审查（Epic 6）

**优先级**：P1
**工作量估算**：1 天
**依赖**：Story 6.5

**验收标准**：
- [ ] 新建 `test_database_manager.cpp`（9 个用例 M1-M9）：持久化、密码加密、重启恢复、并发
- [ ] `test_plugin_e2e.cpp` 补充（P1-P3）：`AddChannel()` 后 `Get()` 成功；`RemoveChannel()` 后 `Get()` 返回 nullptr；AddChannel 后立即查询成功
- [ ] Scheduler 端点测试（S1-S7）：HTTP 接口验证，含 503 场景
- [ ] 手动验证完整流程：Web UI 新增通道 → SQL 立即可用 → 重启后配置持久化
- [ ] 所有测试回归通过

---

## 实施顺序

```
Week 1（Epic 5）:
  Day 1-2: Story 5.1（ClickHouseDriver 核心 + DatabasePlugin 集成）
  Day 3:   Story 5.2（测试 + 代码审查）

Week 2（Epic 6）:
  Day 4-5: Story 6.1（DatabasePlugin 持久化 + IDatabaseFactory 扩展）
  Day 6:   Story 6.2（Scheduler 端点）+ Story 6.3（Web API）
  Day 7:   Story 6.4（废弃 yaml 配置）
  Day 8-9: Story 6.5（前端 UI）
  Day 10:  Story 6.6（端到端测试 + 代码审查）
  Day 11:  缓冲（修复审查问题）
```

---

## 风险识别

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| ClickHouse `FORMAT ArrowStream` 响应格式与 Arrow C++ 库不完全兼容 | 中 | 高 | 优先实测验证，必要时手动解析响应头 |
| DatabasePlugin 引入 YAML 持久化（yaml-cpp 已有依赖） | 低 | 低 | 项目已有 yaml-cpp，零新依赖 |
| OpenSSL AES-256-GCM 在项目中未使用过 | 中 | 低 | 可降级为 XOR + Base64（开发环境），生产环境再接 OpenSSL |
| 前端动态表单复杂度超预期 | 低 | 中 | Element Plus 的 el-form 支持动态字段，有成熟方案 |

---

## 技术债务记录

- `arrow_db_session.h` 中 `ClickHouseTraits::ConnectionType` 目前为 `void*` 占位，Story 5.1 需要替换为实际结构体
- `DatabaseChannel::CreateArrowReader/CreateArrowWriter` 目前直接返回 -1，Story 5.1 激活
