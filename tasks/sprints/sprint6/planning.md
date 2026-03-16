# Sprint 6 规划

## Sprint 信息

- **Sprint 周期**：Sprint 6
- **开始日期**：2026-03-15
- **预计工作量**：13-15 天
- **Sprint 目标**：补齐 Epic 6 遗留的错误透传（Story 6.7），并完成 Epic 7 路由代理与服务对等化架构改造（Story 7.1~7.8）

---

## Sprint 目标

### 主要目标

1. 数据库错误信息透传：`IBatchReadable/IBatchWritable/IDatabaseChannel` 接口增加 `error*` 参数，底层驱动错误信息直达调用方
2. 路由代理架构：引入 `RouterAgencyPlugin`，统一收集和分发进程内所有插件路由，业务插件对 HTTP 无感知
3. Gateway 瘦身：删除 ServiceManager/HeartbeatThread，改为 Trie 路由表 + 注册/转发/过期清理
4. 业务插件迁移：SchedulerPlugin/DatabasePlugin/WebPlugin 实现 IRouterHandle，删除内部 HTTP server
5. 进程管理改造：RunGateway → RunGuardian（极简 fork 守护）+ docker-compose 部署方案

### 成功标准

- [ ] 数据库错误信息透传：查询不存在的表时，error 包含具体原因（如 "no such table"）而非通用 "CreateReader failed"
- [ ] IRouterHandle 接口就绪，业务错误码 → HTTP 状态码映射正确
- [ ] main.cpp 两阶段加载（先 Load 后 StartAll）正常工作
- [ ] RouterAgencyPlugin 能收集路由、分发请求、向 Gateway 注册
- [ ] Gateway Trie 路由表正确匹配，过期清理工作正常
- [ ] 所有现有 API 通过 RouterAgencyPlugin 分发后功能不变
- [ ] RunGuardian fork 守护 + docker-compose 部署方案可用
- [ ] 全路由回归测试通过
- [ ] 所有现有测试回归通过（test_sqlite、test_mysql、test_plugin_e2e）

---

## 设计文档

- [错误信息透传设计](design_error_propagation.md)
- [路由代理架构设计](design_router_agency.md)

---

## Story 列表

---

### Story 6.7：数据库错误信息透传（Epic 6 遗留）

**优先级**：P0（与 Epic 7 独立，先做建立节奏）
**工作量估算**：2 天
**依赖**：无
**阶段**：Phase 0（Day 1-2）

**验收标准**：
- [ ] `IBatchReadable::CreateReader` / `IBatchWritable::CreateWriter` 增加 `std::string* error = nullptr`
- [ ] `IDatabaseChannel::CreateReader/CreateWriter` 同步增加 error 参数
- [ ] `DatabaseChannel` 透传 error 参数，各失败路径填充具体错误
- [ ] `RelationDbSessionBase::CreateReader/CreateWriter` 填充 error（MySQL/SQLite 共用）
- [ ] `RelationBatchReader::last_error_` 死代码修复
- [ ] `ChannelAdapter::ReadToDataFrame/WriteFromDataFrame` 传递 error 并使用具体错误信息
- [ ] ClickHouse 路径给出明确提示（"session does not support batch reading"）
- [ ] 测试：构造"表不存在"场景，断言错误消息包含具体原因
- [ ] test_sqlite、test_mysql、test_plugin_e2e 全部通过

**任务分解**：

#### Task 6.7.1：接口签名扩展（0.3 天）
- [ ] 修改 `src/services/database/capability_interfaces.h`：`CreateReader/CreateWriter` 增加 `std::string* error = nullptr`
- [ ] 修改 `src/framework/interfaces/idatabase_channel.h`：同步增加 error 参数
- [ ] 修改 `src/services/database/database_channel.h`：声明同步

#### Task 6.7.2：核心透传实现（0.7 天）
- [ ] 修改 `src/services/database/relation_db_session.h`：`CreateReader/CreateWriter` 填充 error
- [ ] 修改 `src/services/database/database_channel.cpp`：各失败路径填充 error，透传给底层
- [ ] 修改 `src/framework/core/channel_adapter.cpp`：`ReadToDataFrame/WriteFromDataFrame` 传递 error

#### Task 6.7.3：死代码修复（0.2 天）
- [ ] 修改 `src/services/database/relation_adapters.h`：`RelationBatchReader::last_error_` 死代码处理

#### Task 6.7.4：测试（0.8 天）
- [ ] `test_sqlite.cpp` 新增 Test 16：CreateReader 错误透传（表不存在、SQL 语法错误、nullptr 兼容、成功不污染）
- [ ] `test_mysql.cpp` 新增类似测试
- [ ] `test_plugin_e2e.cpp` 新增 E2E 错误透传测试

---

### Story 7.1：基础设施 — IRouterHandle + ErrorCode + 两阶段加载（Epic 7）

**优先级**：P0（Epic 7 地基，所有后续 Story 依赖此）
**工作量估算**：2 天
**依赖**：无
**阶段**：Phase 1（Day 3-4）

**验收标准**：
- [x] 新增 `src/framework/interfaces/irouter_handle.h`：IRouterHandle + RouteItem + fnRouterHandler + IID_ROUTER_HANDLE
- [x] 新增 `src/common/error_code.h`：OK/BAD_REQUEST/NOT_FOUND/CONFLICT/INTERNAL_ERROR/UNAVAILABLE
- [x] `src/app/main.cpp` RunService() 修正：循环内只 `loader->Load()`，循环结束后统一 `loader->StartAll()`
- [x] 现有 Gateway 模式和 Service 模式正常启动
- [x] 新头文件编译通过

**任务分解**：

#### Task 7.1.1：IRouterHandle 接口（0.5 天）
- [x] 新建 `src/framework/interfaces/irouter_handle.h`
- [x] 定义 fnRouterHandler、RouteItem、IRouterHandle、IID_ROUTER_HANDLE

#### Task 7.1.2：ErrorCode 定义（0.3 天）
- [x] 新建 `src/common/error_code.h`
- [x] 定义 OK(0)/BAD_REQUEST(-1)/NOT_FOUND(-2)/CONFLICT(-3)/INTERNAL_ERROR(-4)/UNAVAILABLE(-5)

#### Task 7.1.3：main.cpp 两阶段加载修正（1 天）
- [x] 修改 RunService()：循环内只 Load()，循环结束后统一 StartAll()
- [x] 验证 Gateway 模式不受影响（只加载一个插件）
- [x] 验证 Service 模式正常启动

#### Task 7.1.4：编译验证（0.2 天）
- [x] 全量编译通过
- [x] 现有测试回归通过

---

### Story 7.2：RouterAgencyPlugin 实现（Epic 7）

**优先级**：P0
**工作量估算**：2 天
**依赖**：Story 7.1
**阶段**：Phase 2（Day 5-6，与 Story 7.3 可并行）

**验收标准**：
- [x] 新增 `src/services/router/` 目录
- [x] `RouterAgencyPlugin` 实现：RouteCollector（Traverse + 冲突检测 + 前缀提取）、HttpServer（catch-all Dispatch + CORS）、GatewayRegistrar（KeepAlive 线程）、ErrorMapper（error_code → HTTP status）
- [x] 插件能加载，Traverse 收集到路由
- [x] 构建 `libflowsql_router.so`

**任务分解**：

#### Task 7.2.1：目录结构与 CMake（0.2 天）
- [ ] 新建 `src/services/router/CMakeLists.txt`
- [ ] 新建 `src/services/router/plugin_register.cpp`

#### Task 7.2.2：RouterAgencyPlugin 主体（1.5 天）
- [ ] 新建 `src/services/router/router_agency_plugin.h`
- [ ] 新建 `src/services/router/router_agency_plugin.cpp`
- [ ] 实现 Option()：解析 host/port/gateway/keepalive_interval_s
- [ ] 实现 Start()：Traverse 收集路由 → 冲突检测 → 前缀提取 → 启动 HTTP → 启动 KeepAlive
- [ ] 实现 Dispatch()：catch-all handler + CORS + ErrorMapper
- [ ] 实现 KeepAliveThread()：定期向 Gateway 注册路由前缀

#### Task 7.2.3：测试验证（0.3 天）
- [ ] RouterAgencyPlugin 能加载
- [ ] Traverse 收集到路由（先用 mock 测试）
- [ ] 编译通过

---

### Story 7.3：Gateway 改造 — Trie 路由表 + 瘦身（Epic 7）

**优先级**：P0
**工作量估算**：2 天
**依赖**：Story 7.1
**阶段**：Phase 2（Day 7-8，与 Story 7.2 可并行）
**风险**：改造现有代码，风险较高

**验收标准**：
- [x] `RouteTable` 重写为 Trie 实现（Insert/Match/RemoveExpired），RouteEntry 增加 `last_seen_ms`
- [x] `HandleForward` 转发完整 URI，不再 StripPrefix
- [x] 新增 `/gateway/register`、`/gateway/unregister`、`/gateway/routes` 端点
- [x] 新增 CleanupThread（过期路由清理）
- [x] 删除 HeartbeatThread、HandleHeartbeat
- [x] 删除 `service_manager.h/cpp`（333 行）、`service_client.h/cpp`（159 行）引用
- [x] Gateway 启动正常，注册/转发/过期清理工作正确

**任务分解**：

#### Task 7.3.1：Trie RouteTable 实现（1 天）
- [ ] 重写 `src/services/gateway/route_table.h/cpp`
- [ ] Trie 实现：Insert/Match/RemoveExpired
- [ ] RouteEntry 增加 last_seen_ms
- [ ] 单元测试覆盖所有路由模式

#### Task 7.3.2：GatewayPlugin 改造（0.7 天）
- [ ] 修改 `src/services/gateway/gateway_plugin.h/cpp`
- [ ] HandleForward 转发完整 URI
- [ ] 新增 /gateway/register、/gateway/unregister、/gateway/routes 端点
- [ ] 新增 CleanupThread
- [ ] 删除 HeartbeatThread、HandleHeartbeat

#### Task 7.3.3：删除废弃代码（0.3 天）
- [ ] 删除 `service_manager.h/cpp`
- [ ] 删除 `service_client.h/cpp`
- [ ] 更新 CMakeLists.txt

---

### Story 7.4：SchedulerPlugin 迁移（Epic 7）

**优先级**：P1
**工作量估算**：2 天
**依赖**：Story 7.2
**阶段**：Phase 3（Day 9-10，与 Story 7.5/7.6 可并行）
**风险**：最大改造点（801 行），需逐个 handler 改造；检查是否有 handler 依赖 req.params/req.path

**验收标准**：
- [x] `SchedulerPlugin` 多继承 IRouterHandle，实现 EnumRoutes()
- [x] 4 个 Handler 从 `(httplib::Request&, httplib::Response&)` 改为 `fnRouterHandler(uri, req_json, rsp_json)` 签名
- [x] 删除 `httplib::Server server_` 和 `RegisterRoutes()`
- [x] 删除 `/db-channels/*` 相关 Handler（移交 DatabasePlugin）
- [x] `plugin_register.cpp` 增加 `____INTERFACE(IID_ROUTER_HANDLE, IRouterHandle)`
- [x] URI 重命名：`/execute` → `/tasks/instant/execute`，`/channels` → `/channels/dataframe/query` 等
- [x] 所有 handler 功能不变

**任务分解**：

#### Task 7.4.1：IRouterHandle 集成（0.3 天）
- [ ] 修改 `scheduler_plugin.h`：多继承 IRouterHandle
- [ ] 修改 `plugin_register.cpp`：增加 IID_ROUTER_HANDLE 注册

#### Task 7.4.2：Handler 签名改造（1.2 天）
- [ ] 逐个改造 8+ 个 Handler 为 fnRouterHandler 签名
- [ ] 每改一个编译一次，确保不遗漏
- [ ] URI 重命名

#### Task 7.4.3：清理废弃代码（0.5 天）
- [ ] 删除 httplib::Server server_ 和 RegisterRoutes()
- [ ] 删除 /db-channels/* 相关 Handler
- [ ] 删除 CORS 处理代码

---

### Story 7.5：DatabasePlugin 迁移（Epic 7）

**优先级**：P1
**工作量估算**：1 天
**依赖**：Story 7.2
**阶段**：Phase 3（Day 10，与 Story 7.4/7.6 可并行）

**验收标准**：
- [x] `DatabasePlugin` 多继承 IRouterHandle，实现 EnumRoutes()
- [x] 新增 HandleAdd/HandleRemove/HandleModify/HandleQuery（从 SchedulerPlugin 迁移逻辑）
- [x] `plugin_register.cpp` 增加 IID_ROUTER_HANDLE 注册
- [x] `/channels/database/*` 路由功能正常

**任务分解**：

#### Task 7.5.1：IRouterHandle 集成（0.3 天）
- [ ] 修改 `database_plugin.h`：多继承 IRouterHandle
- [ ] 修改 `plugin_register.cpp`：增加 IID_ROUTER_HANDLE 注册

#### Task 7.5.2：Handler 实现（0.7 天）
- [ ] 实现 HandleAdd/HandleRemove/HandleModify/HandleQuery
- [ ] 实现 EnumRoutes() 声明 /channels/database/* 路由

---

### Story 7.6：WebPlugin 迁移（Epic 7）

**优先级**：P1
**工作量估算**：1 天
**依赖**：Story 7.2
**阶段**：Phase 3（Day 10-11，与 Story 7.4/7.5 可并行）

**验收标准**：
- [x] `WebPlugin` 多继承 IRouterHandle，实现 EnumRoutes()（管理 API 部分）
- [x] 保留 httplib::Server 用于静态文件服务
- [x] 管理 API Handler 改为 fnRouterHandler 签名
- [x] 管理 API 功能不变（路径参数路由改为 POST + body 传参）

**任务分解**：

#### Task 7.6.1：IRouterHandle 集成（0.3 天）
- [ ] 修改 `web_plugin.h` 或 `web_server.h`：多继承 IRouterHandle
- [ ] 实现 EnumRoutes()

#### Task 7.6.2：管理 API Handler 改造（0.7 天）
- [ ] 管理 API Handler 改为 fnRouterHandler 签名
- [ ] 保留静态文件服务的 httplib::Server

---

### Story 7.7：进程管理改造 — RunGuardian + Docker（Epic 7）

**优先级**：P2
**工作量估算**：2 天
**依赖**：Story 7.3（Gateway 已瘦身，ServiceManager 已删除）
**阶段**：Phase 4（Day 12-13）

**验收标准**：
- [x] `main.cpp` RunGuardian：极简 fork + waitpid + respawn，零业务逻辑，不加载任何插件
- [x] 新增 `Dockerfile`（基于 ubuntu:22.04，同一镜像多角色）
- [x] 新增 `docker-compose.yml`（gateway + web + scheduler + pyworker，restart: always）
- [x] 新增 `docker-compose.full.yml`（含 MySQL + ClickHouse）
- [x] 更新 `config/gateway.yaml` 配置格式（per-plugin option）
- [x] fork 守护进程管理正常（子进程崩溃自动重启）
- [x] 全量编译通过，现有测试回归通过

**任务分解**：

#### Task 7.7.1：RunGuardian 实现（0.7 天）
- [ ] 修改 `src/app/main.cpp`：RunGateway → RunGuardian
- [ ] 实现极简 fork + waitpid + respawn

#### Task 7.7.2：Docker 部署方案（1 天）
- [ ] 新建 `Dockerfile`
- [ ] 新建 `docker-compose.yml`
- [ ] 新建 `docker-compose.full.yml`

#### Task 7.7.3：配置更新（0.3 天）
- [ ] 更新 `config/gateway.yaml` 配置格式（per-plugin option）

---

### Story 7.8：端到端验证（Epic 7）

**优先级**：P1
**工作量估算**：2 天
**依赖**：Story 7.4, 7.5, 7.6, 7.7
**阶段**：Phase 5（Day 14-15）

**验收标准**：
- [x] 路由收集测试：多插件路由收集、冲突检测、前缀提取（T8/T9）
- [x] Trie 匹配测试：基本匹配、最长前缀、幂等注册、注销、过期清理（T1-T7/T11）
- [x] 错误码映射验证（T10）
- [x] 所有现有测试回归通过（test_sqlite M1-M8、test_database_manager）
- [ ] KeepAlive 测试：需要真实 Gateway 进程（集成测试，暂跳过）
- [ ] docker-compose 部署验证（需要 Docker 环境，暂跳过）

**任务分解**：

#### Task 7.8.1：路由测试（0.7 天）
- [ ] 路由收集测试
- [ ] 路由分发测试
- [ ] 冲突检测测试

#### Task 7.8.2：KeepAlive 与故障恢复测试（0.5 天）
- [ ] 正常注册测试
- [ ] Gateway 重启恢复测试
- [ ] 服务崩溃路由过期测试

#### Task 7.8.3：全路由回归（0.5 天）
- [ ] channels/database/*、channels/dataframe/*、operators/*、tasks/* 全路由验证

#### Task 7.8.4：部署验证（0.3 天）
- [ ] docker-compose 多容器启动
- [ ] 服务发现、故障重启验证

---

## 实施顺序

```
Phase 0（Day 1-2）— 独立，先做建立节奏:
  Story 6.7: 错误信息透传

Phase 1（Day 3-4）— Epic 7 地基:
  Story 7.1: IRouterHandle + ErrorCode + 两阶段加载

Phase 2（Day 5-8）— 核心组件，可并行:
  Story 7.2: RouterAgencyPlugin（Day 5-6）
  Story 7.3: Gateway 改造（Day 7-8）

Phase 3（Day 9-11）— 业务插件迁移，可高度并行:
  Story 7.4: SchedulerPlugin 迁移（Day 9-10）
  Story 7.5: DatabasePlugin 迁移（Day 10）
  Story 7.6: WebPlugin 迁移（Day 10-11）

Phase 4（Day 12-13）— 进程管理:
  Story 7.7: RunGuardian + Docker

Phase 5（Day 14-15）— 端到端验证:
  Story 7.8: 全路由回归 + 部署验证
```

---

## 风险识别

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| SchedulerPlugin 8+ handler 签名改造遗漏 | 中 | 高 | 逐个改造，每改一个编译一次 |
| 删除 ServiceManager 后开发环境启动不便 | 高 | 低 | Phase 4 提供 RunGuardian + docker-compose 替代 |
| Trie 匹配与现有前缀匹配行为不一致 | 中 | 中 | 先写 Trie 单元测试覆盖所有现有路由 |
| handler 依赖 req.params/req.path 路径参数 | 中 | 中 | Phase 3 前逐一检查所有 handler |

---

## 关键文件索引

| 文件 | 涉及 Story |
|------|-----------|
| `src/services/database/capability_interfaces.h` | 6.7 |
| `src/framework/interfaces/idatabase_channel.h` | 6.7 |
| `src/services/database/database_channel.cpp` | 6.7 |
| `src/services/database/relation_db_session.h` | 6.7 |
| `src/services/database/relation_adapters.h` | 6.7 |
| `src/framework/core/channel_adapter.cpp` | 6.7 |
| `src/framework/interfaces/irouter_handle.h` | 7.1（新增） |
| `src/common/error_code.h` | 7.1（新增） |
| `src/app/main.cpp` | 7.1, 7.7 |
| `src/services/router/*` | 7.2（新增） |
| `src/services/gateway/route_table.h/cpp` | 7.3 |
| `src/services/gateway/gateway_plugin.h/cpp` | 7.3 |
| `src/services/gateway/service_manager.h/cpp` | 7.3（删除） |
| `src/services/gateway/service_client.h/cpp` | 7.3（删除） |
| `src/services/scheduler/scheduler_plugin.h/cpp` | 7.4 |
| `src/services/database/database_plugin.h/cpp` | 7.5 |
| `src/services/web/web_plugin.h/cpp` 或 `web_server.h/cpp` | 7.6 |
| `Dockerfile` | 7.7（新增） |
| `docker-compose.yml` | 7.7（新增） |
| `docker-compose.full.yml` | 7.7（新增） |
| `config/gateway.yaml` | 7.7 |
