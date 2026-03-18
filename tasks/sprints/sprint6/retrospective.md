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

### P3: GatewayPlugin 仍然特殊处理，与"服务对等"目标表面矛盾

**问题**：设计目标是"服务进程对等"，但 GatewayPlugin 不实现 IRouterHandle，不通过 RouterAgencyPlugin 分发请求，没有 KeepAlive 机制，看起来仍然是"特权插件"。

**根因分析**：

**这是设计的合理边界，不是设计不到位，也不是代码编写问题。**

Gateway 天然是路由基础设施层：RouterAgencyPlugin 向 Gateway 注册前缀，如果 Gateway 自己也要向上游注册，会形成无限递归。设计文档 §3.3 明确保留了 GatewayPlugin 的四个职责，这是有意为之。

真正的问题是**"服务进程对等"表述有歧义**：
- **原意**：进程管理权对等——废除 ServiceManager，Gateway 不再 fork 其他进程（✅ 已实现）
- **误解**：所有插件都应该走同一套 IRouterHandle + RouterAgencyPlugin 机制

Gateway 在"进程管理权"上已经对等，但在"路由机制"上它是基础设施层，不适用业务插件的路由机制。

**改进措施**：在架构文档中明确区分两个层次的"对等"，避免歧义：
- 进程管理对等（✅ 已实现：Guardian 替代 ServiceManager）
- 路由机制对等（⚠️ 不适用于 Gateway 本身，Gateway 是路由基础设施，不是业务服务）

### P4: WebPlugin 双 HTTP 通道混淆（设计缺口为主因）

**问题**：前端添加数据库通道持续返回 404，排查耗时远超预期，先后出现：1. 对外 web 服务不可用；2. web 服务的 URI 转内部管理面 URI 不正确导致 404。

**根因分析**：

**设计缺口是主因，代码编写错误是次因，两者叠加放大了排查难度。**

1. **设计缺口（主因）**：设计文档（§2.6）描述了 WebPlugin 的双重身份，但没有回答三个关键问题：
   - WebPlugin 内部转发的目标是谁？`/api/channels/database/*` 需要转发到 Scheduler，但设计文档只说"通过 RouterAgencyPlugin 分发"，没有说 WebPlugin 应该直连 Scheduler 还是经过 Gateway，也没有给出配置项名称
   - 两个 httplib::Server 的路由注册策略是什么？`/api/*` 路由在 Init() 里直接绑定了一遍，EnumApiRoutes() 又通过 IRouterHandle 声明了一遍，这个重复注册是有意为之还是遗漏，设计文档没有说清楚
   - `db_proxy` 的配置项叫什么？设计文档 YAML 示例里没有 `scheduler` 配置项的说明

2. **代码编写问题（次因，三处）**：
   - `web_plugin.cpp` 把 `SetSchedulerAddress` 设成了 `worker_host_:worker_port_`（pyworker 18900），注释写的是"Gateway 模式下两者相同"，但这个假设依赖未文档化的环境变量，实际运行时未设置，导致转发打到 pyworker
   - `web_server.cpp` Init() 里注册了 `/api/*` httplib 路由，但**漏掉了** `/api/channels/database/*` 的代理路由，纯粹的遗漏
   - `api/index.js` baseURL 带了 `/api` 后缀，db channel 路径又加了 `/channels/database/*`，路径拼接错误

3. **排查困难的原因**：三个错误分布在三个不同层（C++ 后端配置、C++ 路由注册、前端 JS），每次修一处都还有另一处在报错，且 404 的来源（WebPlugin 本身 vs 内部服务）不直观，缺乏日志辅助定位。

**改进措施**：

- **设计层**：凡是涉及"服务间转发"的插件，设计文档必须包含"转发配置表"（转发目标、配置项名称、默认值），不能只描述架构拓扑图
- **设计层**：双 server 架构必须明确说明路由注册策略（哪些路由只在外部 server 注册，哪些只通过 IRouterHandle 声明，哪些两者都有及原因）
- **代码层**：内部转发地址必须有独立的配置项（`scheduler`），不能复用 worker 地址，不能依赖隐式环境变量
- **调试层**：WebPlugin 的 db_proxy 转发失败时应打印日志（目标地址、HTTP 状态码），而不是静默返回 503/404

**经验教训**：见 L18、L19

### P5: Story 7.8 集成测试未完成

**问题**：KeepAlive 故障恢复、docker-compose 部署的集成测试未完成，记录为技术债务。

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

### L18: "服务对等"的表述必须区分两个层次

**来源**：Sprint 6 回顾 — GatewayPlugin 特殊处理问题

**原则**：架构文档中"服务对等"必须明确区分：
- **进程管理对等**：没有特权进程负责 fork/管理其他进程（Gateway 不再是 ServiceManager）
- **路由机制对等**：业务服务都通过 IRouterHandle + RouterAgencyPlugin 暴露路由

Gateway 是路由基础设施，不是业务服务，不适用路由机制对等。混淆这两个层次会导致对架构目标的误解。

### L19: 涉及服务间转发的插件，设计文档必须包含"转发配置表"

**来源**：Sprint 6 回顾 — WebPlugin 双通道混淆问题

**原则**：凡是插件内部需要调用其他服务的，设计文档必须明确写出：

| 转发目标 | 配置项名称 | 默认值 | 说明 |
|---------|-----------|--------|------|
| Scheduler | `scheduler` | `127.0.0.1:18803` | 内部 API 转发 |
| PyWorker | `worker` | `127.0.0.1:18900` | 算子执行通知 |

不能只描述架构拓扑图，不能依赖隐式环境变量，不能复用其他服务的地址配置项。

### L20: 双 server 架构必须明确路由注册策略

**来源**：Sprint 6 回顾 — WebPlugin 双通道混淆问题

**原则**：当一个进程内有多个 httplib::Server（如 WebPlugin 的 8081 外部 server + RouterAgencyPlugin 的 18802 内部 server），设计文档必须明确每条路由的注册位置：

| 路由 | 外部 server (8081) | 内部 server (18802) | 说明 |
|------|-------------------|---------------------|------|
| 静态文件 | ✅ | ❌ | 只对外 |
| `/api/*` | ✅ | ✅ | 两者都注册（外部直接访问 + Gateway 转发） |
| `/api/channels/database/*` | ✅（代理） | ✅（代理） | 代理到 Scheduler |

不明确这个策略，容易出现"漏注册"或"重复注册语义不一致"的问题。
