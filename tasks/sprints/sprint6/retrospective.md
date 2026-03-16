# Sprint 6 迭代回顾

## Sprint 信息

- **Sprint 周期**：Sprint 6
- **回顾日期**：2026-03-17

---

## Keep（做得好的地方）

1. **设计先行**：Sprint 6 有完整的设计文档（`design_router_agency.md`、`design_error_propagation.md`），实现时几乎没有返工，设计与实现高度一致
2. **分阶段交付**：5 个 Phase 顺序推进，每个 Phase 编译验证后再进入下一个，没有出现大规模集成失败
3. **接口隔离彻底**：业务插件（Scheduler/Database/Web）对 HTTP 完全无感知，fnRouterHandler 签名干净，未来替换传输层零改动
4. **Trie 实现正确**：最长前缀匹配、幂等注册、过期清理、并发安全（shared_mutex）一次写对，测试全部通过
5. **测试覆盖关键路径**：test_router 11 个用例覆盖了 Trie 的所有核心行为，包括边界和并发场景

---

## Problem（需要改进的地方）

### P1: WebPlugin 路径参数路由改造引入 API 破坏性变更

**问题**：`/api/operators/([^/]+)/activate` 改为 `POST /api/operators/activate + body`，是破坏性 API 变更，前端需要同步修改。

**根因**：fnRouterHandler 精确匹配不支持路径参数，但在 Phase 3 开始前没有提前评估所有 handler 的路由模式。

**改进**：Phase 3 前应逐一检查所有 handler 是否依赖路径参数（计划中有此风险项，但执行时未充分评估）。

### P2: service_manager.h/cpp 未物理删除

**问题**：文件仍在磁盘，只是 CMakeLists 排除了编译。遗留文件会造成混淆。

**根因**：删除文件需要用户确认（CLAUDE.md 规则），当时未主动提出。

**改进**：下个 Sprint 开始时清理。

### P4: WebPlugin 内部转发地址配置混乱（代码编写问题 + 设计缺口）

**问题**：前端添加数据库通道持续返回 404，排查耗时远超预期。

**根因分析**：

这是**代码编写问题**和**设计缺口**叠加导致的：

1. **设计缺口**：Sprint 6 设计文档（design_router_agency.md）明确了"前端 → Gateway → RouterAgencyPlugin → 业务插件"的链路，但**没有明确 WebPlugin 作为前端入口时如何做内部转发**。WebPlugin 既是静态文件服务器，又是 API 代理，这个双重角色的转发配置没有在设计阶段写清楚。

2. **代码编写问题（三处）**：
   - `web_plugin.cpp` 把 `SetSchedulerAddress` 设成了 `worker_host_:worker_port_`（pyworker 18900），注释写的是"Gateway 模式下两者相同"，但这个假设依赖未文档化的环境变量 `FLOWSQL_GATEWAY_ADDR`，实际运行时该变量未设置，导致转发打到 pyworker
   - `web_server.cpp` Init() 里注册了 `/api/*` httplib 路由，但**漏掉了** `/api/channels/database/*` 的代理路由，这是纯粹的遗漏
   - `api/index.js` baseURL 带了 `/api` 后缀，db channel 路径又加了 `/channels/database/*`，路径拼接错误

3. **排查困难的原因**：三个错误分布在三个不同层（C++ 后端配置、C++ 路由注册、前端 JS），每次修一处都还有另一处在报错，且 404 的来源（WebPlugin 本身 vs 内部服务）不直观，缺乏日志辅助定位。

**改进措施**：

- **设计层**：凡是涉及"服务间转发"的插件，设计文档必须明确写出转发目标地址的来源（配置项名称、默认值、是否依赖环境变量）
- **代码层**：内部转发地址必须有独立的配置项（`gateway`），不能复用 worker 地址，不能依赖隐式环境变量
- **调试层**：WebPlugin 的 db_proxy 转发失败时应打印日志（目标地址、HTTP 状态码），而不是静默返回 503/404

**经验教训**：见 L19、L20

**问题**：Story 7.8 的集成测试（KeepAlive 故障恢复、docker-compose 部署）未完成，记录为技术债务。

**根因**：需要真实运行环境，单元测试框架无法覆盖。

**改进**：在 CI 环境中补充，或在下个 Sprint 的验收标准中明确要求。

---

## Try（尝试的新做法）

1. **CollectRoutes 独立方法**：RouterAgencyPlugin 的路由收集逻辑抽为独立方法，可传入 mock IQuerier 单元测试，效果很好，后续继续保持
2. **Trie 替代 HashMap 前缀匹配**：比原来的两级前缀提取更通用，支持任意层级，且最长前缀匹配语义更清晰
3. **Guardian 模式**：fork 守护替代 ServiceManager，零业务逻辑，可被 docker-compose/systemd 完全替代，架构更正交

---

## Action Items

| 行动项 | 负责人 | 截止 | 状态 |
|--------|--------|------|------|
| 物理删除 service_manager.h/cpp 和 service_client.h/cpp | Claude | Sprint 7 开始 | 📋 待办 |
| 前端适配 WebPlugin API 变更（activate/deactivate 改为 body 传参） | 用户 | Sprint 7 | 📋 待办 |
| CI 环境补充 KeepAlive 集成测试 | Claude | Sprint 7 | 📋 待办 |

---

## 经验教训固化

### L16: fnRouterHandler 精确匹配不支持路径参数，迁移前必须逐一检查

**来源**：Sprint 6 Phase 3 — WebPlugin 迁移

**问题**：`/api/operators/([^/]+)/activate` 依赖路径参数，fnRouterHandler 精确匹配无法处理，被迫改为 POST + body 传参，造成 API 破坏性变更。

**原则**：将业务插件迁移到 IRouterHandle 之前，必须逐一检查所有 handler 的路由模式：
1. 是否使用路径参数（`/resource/:id`）
2. 是否使用 GET + query string
3. 是否使用 multipart 上传

凡是依赖这些特性的 handler，需要提前设计替代方案（改为 POST + body），并评估对调用方的影响。

### L17: 守护进程与业务逻辑必须严格分离

**来源**：Sprint 6 Phase 4 — RunGuardian 设计

**原则**：守护进程（Guardian）只做 fork + waitpid + respawn，不加载任何插件，不开任何端口，不包含任何业务逻辑。业务逻辑全部在子进程中。这样守护进程可以被任何外部工具（docker-compose、systemd）完全替代，零代码改动。
