# Sprint 6 迭代评审

## Sprint 信息

- **Sprint 周期**：Sprint 6
- **开始日期**：2026-03-15
- **评审日期**：2026-03-17
- **Sprint 目标**：补齐 Epic 6 遗留的错误透传（Story 6.7），完成 Epic 7 路由代理与服务对等化架构改造（Story 7.1~7.8）

---

## Story 验收结果

### Story 6.7：数据库错误信息透传 ✅

**验收结论**：通过

- `IBatchReadable/IBatchWritable/IArrowReadable/IArrowWritable/ITransactional` 均暴露 `GetLastError()`
- `IDatabaseChannel` 增加 `GetLastError()`，`IBatchReader/IBatchWriter/IArrowReader/IArrowWriter` 同步增加
- `RelationDbSessionBase` 统一 `last_error_` 成员，`CreateReader/CreateWriter` 填充具体错误
- `ChannelAdapter` 传递 error 参数，错误信息直达调用方
- ClickHouse 路径给出明确提示
- 所有测试回归通过

---

### Story 7.1：基础设施 — IRouterHandle + ErrorCode + 两阶段加载 ✅

**验收结论**：通过

- 新增 `src/framework/interfaces/irouter_handle.h`：`fnRouterHandler`、`RouteItem`、`IRouterHandle`、`IID_ROUTER_HANDLE`
- 新增 `src/common/error_code.h`：OK/BAD_REQUEST/NOT_FOUND/CONFLICT/INTERNAL_ERROR/UNAVAILABLE
- `main.cpp` RunService() 修正为两阶段加载（先循环 Load，再统一 StartAll）
- 编译通过，现有测试回归通过

---

### Story 7.2：RouterAgencyPlugin 实现 ✅

**验收结论**：通过

- 新增 `src/services/router/`，构建 `libflowsql_router.so`
- `CollectRoutes()` 独立方法，通过 `Traverse(IID_ROUTER_HANDLE)` 收集路由，自动提取一级前缀
- `Dispatch()` catch-all 分发，统一 CORS，业务错误码映射 HTTP 状态码
- `KeepAliveThread` 定期向 Gateway 重注册，`Stop()` 主动注销
- 编译通过

---

### Story 7.3：Gateway 改造 ✅

**验收结论**：通过

- `RouteTable` 重写为 Trie，最长前缀匹配，`RouteEntry` 增加 `last_seen_ms`，`Register` 幂等
- `GatewayPlugin` 删除 `ServiceManager`、`HeartbeatThread`、`HandleHeartbeat`
- 新增 `CleanupThread`（定期清理过期路由）
- `HandleForward` 转发完整 URI，不剥离前缀
- `service_manager.h/cpp`、`service_client.h/cpp` 不再被编译（CMakeLists 排除）
- 编译通过

---

### Story 7.4：SchedulerPlugin 迁移 ✅

**验收结论**：通过

- 多继承 `IRouterHandle`，删除 `httplib::Server`、`RegisterRoutes()`、CORS 处理
- 4 个 handler 改为 `fnRouterHandler` 签名
- 删除 4 个 `/db-channels/*` handler（移交 DatabasePlugin）
- URI 重命名：`/execute` → `/tasks/instant/execute`，`/channels` → `/channels/dataframe/query`，`/operators` → `/operators/native/query`，`/refresh-operators` → `/operators/python/refresh`
- `plugin_register.cpp` 注册 `IID_ROUTER_HANDLE`

---

### Story 7.5：DatabasePlugin 迁移 ✅

**验收结论**：通过

- 多继承 `IRouterHandle`，新增 `HandleAdd/Remove/Modify/Query`
- `EnumRoutes()` 声明 `/channels/database/add|remove|modify|query`
- `plugin_register.cpp` 注册 `IID_ROUTER_HANDLE`
- 编译通过，DatabaseManager 测试 M1-M8 全部通过

---

### Story 7.6：WebPlugin 迁移 ✅

**验收结论**：通过

- `WebPlugin` 多继承 `IRouterHandle`，`EnumRoutes()` 委托 `WebServer::EnumApiRoutes()`
- 所有管理 API handler 改为 `fnRouterHandler` 签名
- 路径参数路由改为 POST + body 传参（`/api/operators/activate` + `{"name":"..."}`)
- 保留 `httplib::Server` 用于静态文件服务
- `plugin_register.cpp` 注册 `IID_ROUTER_HANDLE`

---

### Story 7.7：RunGuardian + Docker ✅

**验收结论**：通过

- `main.cpp` 新增 `RunGuardian()`：fork + waitpid + respawn，零业务逻辑
- 新增 `Dockerfile`（ubuntu:22.04，同一镜像多角色）
- 新增 `docker-compose.yml`（gateway + web + scheduler + pyworker，restart: always）
- 新增 `docker-compose.full.yml`（含 MySQL + ClickHouse）
- `config/gateway.yaml` 更新为 per-plugin option 格式
- 全量编译通过

---

### Story 7.8：端到端验证 ✅（部分）

**验收结论**：核心测试通过，集成测试暂跳过

- 新增 `src/tests/test_router/test_router.cpp`：11 个测试
  - Trie 路由表：注册/匹配/最长前缀/幂等/注销/过期清理/GetAll（T1-T7/T11）
  - 路由收集：多插件/冲突检测（T8/T9）
  - 错误码值验证（T10）
- 所有 11 个测试通过
- 全量编译零错误，test_sqlite/test_database_manager 回归通过
- KeepAlive 集成测试、docker-compose 部署验证需要真实运行环境，记录为技术债务

---

## 技术债务

| 项目 | 优先级 | 说明 |
|------|--------|------|
| KeepAlive 集成测试 | P2 | 需要真实 Gateway 进程，建议在 CI 环境中补充 |
| docker-compose 部署验证 | P2 | 需要 Docker 环境 |
| service_manager.h/cpp 物理删除 | P3 | 文件仍在磁盘，CMakeLists 已排除，可在下个 Sprint 清理 |
| WebPlugin 文件上传改为 JSON base64 | P3 | 原 multipart 上传改为 JSON，前端需同步适配 |

---

## Sprint 后修复（Post-Sprint Hotfix）

### 前端 API 路由修复（2026-03-17）

**问题**：前端添加数据库通道返回 404，排查耗时较长。

**根因链**：
1. `web_server.cpp` Init() 里直接注册了 `/api/*` httplib 路由（与 IRouterHandle 声明重复），但**缺少** `/api/channels/database/*` 的代理路由，导致前端请求直接 404
2. `web_plugin.cpp` Start() 里调用 `SetSchedulerAddress(worker_host_, worker_port_)`，把内部转发地址设成了 pyworker（18900），而非 Gateway（18800）；pyworker 不认识 `/channels/database/*`，返回 404
3. `api/index.js` baseURL 带了 `/api` 后缀，db channel 路径又加了 `/channels/database/*`，实际请求变成 `/api/channels/database/*`，与 WebPlugin 注册的路由不匹配

**修复**：
- `web_server.cpp` Init() 补充 `/api/channels/database/*` 代理路由，转发时去掉 `/api` 前缀
- `web_plugin.h/cpp` 新增 `gateway_host_/gateway_port_` 配置项，`SetSchedulerAddress` 改用 Gateway 地址
- `api/index.js` baseURL 改为 `http://localhost:8081`，所有路径带完整前缀
- `gateway.yaml` web 服务增加 `gateway=127.0.0.1:18800` option

---

## 功能演示

### 新架构请求流

```
Client → Gateway(18800) → RouterAgencyPlugin(18803) → fnRouterHandler → 业务逻辑
```

### 路由注册流

```
RouterAgencyPlugin.Start()
  → CollectRoutes(IQuerier)
  → Traverse(IID_ROUTER_HANDLE) → SchedulerPlugin/DatabasePlugin/WebPlugin.EnumRoutes()
  → 提取前缀 → RegisterOnce() → POST /gateway/register
  → KeepAliveThread 每 10s 续期
```

### 服务启动命令（新格式）

```bash
# Gateway
./flowsql --role gateway --port 18800 --plugins libflowsql_gateway.so:config/gateway.yaml

# Scheduler 服务（含 RouterAgencyPlugin）
./flowsql --role scheduler --port 18803 \
  --plugins libflowsql_router.so,libflowsql_scheduler.so,libflowsql_database.so \
  --option "gateway=127.0.0.1:18800"

# Guardian 模式（一键拉起所有服务）
./flowsql --config config/gateway.yaml
```
