# 架构改造设计：路由代理 + 服务对等化

> 状态：讨论中
> 创建：2026-03-15
> 参与者：用户 + Claude

---

## 1. 问题陈述

当前 flowSQL 架构存在三个核心问题：

1. **GatewayPlugin 职责过重**：同时承担 HTTP 路由转发、子进程管理（ServiceManager）、心跳检测，违反单一职责原则
2. **服务进程不对等**：Gateway 进程是"特权进程"（负责拉起和管理其他进程），与插件式架构的对等理念矛盾
3. **路由硬编码耦合**：
   - 每个插件（Scheduler、Web）各自创建 `httplib::Server` 并硬编码路由
   - Scheduler 直接调用 `IDatabaseFactory` 处理 `/db-channels/*` 路由，产生不必要的强依赖
   - 多插件无法自由组合在一个服务中（多个 HTTP server 冲突）

## 2. 已达成共识

### 2.1 RouterAgencyPlugin 路由代理机制 ✅

引入 `RouterAgencyPlugin`（libflowsql_router.so），统一收集和分发进程内所有插件的路由。

**IRouterHandle 接口**：业务插件实现此接口声明路由，对 HTTP 完全无感知。

```cpp
// ===== irouter_handle.h =====
namespace flowsql {

// 路由处理函数：纯业务逻辑，不感知 HTTP
typedef std::function<int32_t(const std::string& uri,
                               const std::string& req_json,
                               std::string& rsp_json)> fnRouterHandler;

// 路由条目
struct RouteItem {
    std::string method;   // "GET" / "POST" / "PUT" / "DELETE"
    std::string uri;      // "/execute", "/channels", "/db-channels/add"
    fnRouterHandler handler;
};

const Guid IID_ROUTER_HANDLE = { /* 新 GUID */ };

// 业务插件实现此接口，声明自己提供的路由
interface IRouterHandle {
    virtual void EnumRoutes(std::function<void(const RouteItem&)> callback) = 0;
};

}  // namespace flowsql
```

**RouterAgencyPlugin 工作流程**：

```
Option() 阶段：
  解析参数：host、port（本地 HTTP 监听）、gateway（Gateway 地址）、keepalive_interval_s

Start() 阶段：
  1. Traverse(IID_ROUTER_HANDLE) → 收集所有 RouteItem → 构建 routeTable_（冲突时日志告警，先到先得）
  2. 从 routeTable_ 提取去重的一级前缀列表（如 /channels、/operators、/tasks）
  3. 启动 httplib::Server，注册 catch-all handler
  4. 启动 KeepAlive 后台线程（定期向 Gateway 注册路由前缀）

请求处理流：
  Client → Gateway(转发完整URI) → RouterAgencyPlugin(HTTP↔JSON) → fnRouterHandler(纯业务)
```

```cpp
int RouterAgencyPlugin::Start() {
    // 1. 收集路由（依赖注入：CollectRoutes 可单独测试）
    CollectRoutes(querier_);

    // 2. 启动 HTTP 服务
    http_thread_ = std::thread(&RouterAgencyPlugin::HttpThread, this);

    // 3. 立即同步注册一次，消除首次注册窗口期
    if (!gateway_host_.empty()) {
        RegisterOnce();
        // 4. 启动 KeepAlive 线程（定期续期 + 故障恢复）
        keepalive_thread_ = std::thread(&RouterAgencyPlugin::KeepAliveThread, this);
    }
    return 0;
}

// 路由收集：独立方法，便于单元测试传入 mock IQuerier
int RouterAgencyPlugin::CollectRoutes(IQuerier* querier) {
    if (!querier) return 0;
    querier->Traverse(IID_ROUTER_HANDLE, [&](void* p) -> int {
        auto* handle = static_cast<IRouterHandle*>(p);
        handle->EnumRoutes([&](const RouteItem& item) {
            std::string key = item.method + ":" + item.uri;
            if (route_table_.count(key)) {
                LOG_WARN("RouterAgency: duplicate route %s, ignored", key.c_str());
            } else {
                route_table_[key] = item.handler;
            }
        });
        return 0;
    });
    // 提取一级前缀（用于 Gateway 注册），加格式校验防止边界 UB
    for (auto& [key, _] : route_table_) {
        auto colon = key.find(':');
        if (colon == std::string::npos) {
            LOG_WARN("RouterAgency: invalid route key: %s", key.c_str());
            continue;
        }
        auto uri = key.substr(colon + 1);
        auto second_slash = uri.find('/', 1);
        std::string prefix = (second_slash != std::string::npos) ? uri.substr(0, second_slash) : uri;
        prefixes_.insert(prefix);
    }
    return 0;
}

void RouterAgencyPlugin::Dispatch(const httplib::Request& req, httplib::Response& res) {
    // CORS 统一处理
    res.set_header("Access-Control-Allow-Origin", "*");

    // OPTIONS 预检请求（CORS Preflight）
    if (req.method == "OPTIONS") {
        res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
        return;
    }

    std::string key = req.method + ":" + req.path;
    auto it = route_table_.find(key);
    if (it == route_table_.end()) {
        res.status = 404;
        res.set_content(R"({"error":"route not found"})", "application/json");
        return;
    }
    std::string rsp_json;
    int32_t rc = error::INTERNAL_ERROR;
    try {
        rc = it->second(req.path, req.body, rsp_json);
    } catch (const std::exception& e) {
        LOG_ERROR("RouterAgency: handler exception: %s", e.what());
        rsp_json = R"({"error":"handler exception"})";
    } catch (...) {
        rsp_json = R"({"error":"unknown exception"})";
    }
    // handler 忘记设置 rsp_json 时兜底
    if (rsp_json.empty())
        rsp_json = (rc == error::OK) ? R"({"ok":true})" : R"({"error":"internal error"})";
    res.status = HttpStatus(rc);
    res.set_content(rsp_json, "application/json");
}

// 向 Gateway 注册所有路由前缀（幂等），Start() 同步调用一次，线程负责续期
void RouterAgencyPlugin::RegisterOnce() {
    std::string local_addr = host_ + ":" + std::to_string(port_);
    for (auto& prefix : prefixes_) {
        httplib::Client cli(gateway_host_, gateway_port_);
        cli.set_connection_timeout(2);
        // 使用 JSON 库序列化，避免 prefix 含特殊字符导致 JSON 注入
        nlohmann::json body;
        body["prefix"]  = prefix;
        body["address"] = local_addr;
        cli.Post("/gateway/register", body.dump(), "application/json");
    }
}

// 向 Gateway 注销所有路由前缀（优雅关闭时调用）
void RouterAgencyPlugin::UnregisterOnce() {
    std::string local_addr = host_ + ":" + std::to_string(port_);
    for (auto& prefix : prefixes_) {
        httplib::Client cli(gateway_host_, gateway_port_);
        cli.set_connection_timeout(2);
        nlohmann::json body;
        body["prefix"]  = prefix;
        body["address"] = local_addr;
        cli.Post("/gateway/unregister", body.dump(), "application/json");
    }
}

// 定期向 Gateway 注册路由前缀（幂等），兼做故障恢复
void RouterAgencyPlugin::KeepAliveThread() {
    while (running_) {
        RegisterOnce();
        for (int i = 0; i < keepalive_interval_s_ * 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
```

**RouterAgencyPlugin 内部模块化**：

实现时按职责拆分为内部模块，避免单文件膨胀：

| 模块 | 职责 |
|------|------|
| RouteCollector | Traverse 收集路由 + 冲突检测 + 前缀提取（`CollectRoutes(IQuerier*)`，可单元测试） |
| HttpServer | httplib::Server + Dispatch + CORS + OPTIONS 预检 + 请求体大小限制（1MB） |
| GatewayRegistrar | `RegisterOnce()` + `UnregisterOnce()` + KeepAlive 线程 |
| ErrorMapper | 业务错误码 → HTTP 状态码映射 |

**线程安全**：`running_` 声明为 `std::atomic<bool>`，Stop() 设置为 false 后 KeepAlive 线程安全退出，Stop() 末尾调用 `UnregisterOnce()` 主动注销路由。

### 2.2 fnRouterHandler 签名：纯 JSON ✅

handler 签名为 `int32_t(uri, req_json, rsp_json)`，纯 JSON 交互，不传递 HTTP headers、query params 等上下文。当前业务场景足够。

### 2.3 资源导向的 URI 设计 ✅

**原则**：以项目核心概念实体为一级前缀，不以服务名/插件名为前缀。

URI 三级结构：`/{实体}/{子类型}/{动作}`

**统一请求约定**：
- 所有操作统一使用 `POST` + JSON body
- 资源定位通过 body 中的标识字段（如 `task_id`、`type`+`name`），不使用路径参数
- 这与 `fnRouterHandler(uri, req_json, rsp_json)` 的精确匹配路由完全契合

**Gateway 不剥离前缀**：Gateway 转发完整 URI，RouterAgencyPlugin 匹配完整路径。Gateway 只做转发，不改写 URI。

**Gateway RouteTable 改为字典树匹配**：废弃当前的一级/二级前缀提取逻辑（`ExtractPrefix`），改用字典树（Trie）按路径段逐级匹配。如何注册就如何匹配，不限层级。

```cpp
// 注册：服务注册任意路径前缀
trie.Insert("/channels",  {"127.0.0.1:18803", "scheduler"});
trie.Insert("/operators", {"127.0.0.1:18803", "scheduler"});
trie.Insert("/tasks",     {"127.0.0.1:18803", "scheduler"});

// 匹配：按路径段逐级查找最长匹配
trie.Match("/channels/database/add")    → 命中 "/channels"  → scheduler
trie.Match("/tasks/instant/execute")    → 命中 "/tasks"     → scheduler
trie.Match("/unknown/path")             → 未命中 → 404
```

#### 路由总表

**channels — 数据通道**

| URI | 说明 | Body 示例 |
|-----|------|-----------|
| `POST /channels/database/add` | 新增数据库通道 | `{"config":"type=mysql;name=mydb;host=..."}` |
| `POST /channels/database/remove` | 删除数据库通道 | `{"type":"mysql","name":"mydb"}` |
| `POST /channels/database/modify` | 修改数据库通道 | `{"config":"type=mysql;name=mydb;host=..."}` |
| `POST /channels/database/query` | 查询数据库通道 | `{}` 全部 / `{"type":"mysql","name":"mydb"}` 单个 |
| `POST /channels/dataframe/query` | 查询内存通道 | `{}` |

**operators — 算子**

| URI | 说明 | Body 示例 |
|-----|------|-----------|
| `POST /operators/python/query` | 查询 Python 算子 | `{}` 全部 / `{"name":"anomaly_detect"}` 单个 |
| `POST /operators/python/refresh` | 刷新 Python 算子列表 | `{}` |
| `POST /operators/native/query` | 查询 C++ 算子 | `{}` |

**tasks — 任务**

| URI | 说明 | Body 示例 |
|-----|------|-----------|
| `POST /tasks/instant/execute` | 即时执行 SQL | `{"sql":"TRANSFER FROM ..."}` |
| `POST /tasks/instant/query` | 查询即时执行记录 | `{}` |
| `POST /tasks/scheduled/add` | 创建定时任务 | `{"sql":"...","cron":"0 */2 * * *"}` |
| `POST /tasks/scheduled/remove` | 删除定时任务 | `{"task_id":"t-20260315-001"}` |
| `POST /tasks/scheduled/modify` | 修改定时任务 | `{"task_id":"t-20260315-001","cron":"..."}` |
| `POST /tasks/scheduled/query` | 查询定时任务 | `{}` 全部 / `{"task_id":"..."}` 单个 |

#### Gateway 路由映射

一个服务可注册多个一级前缀（RouterAgencyPlugin 从收集到的路由中自动提取并注册）：

```
/channels/*   → scheduler 服务（DatabasePlugin、SchedulerPlugin）
/operators/*  → scheduler 服务（SchedulerPlugin、BridgePlugin）
/tasks/*      → scheduler 服务（SchedulerPlugin）
```

#### 路由归属（哪个插件声明哪些路由）

| 插件 | 声明的路由 |
|------|-----------|
| DatabasePlugin | `/channels/database/*` |
| SchedulerPlugin | `/channels/dataframe/*`、`/operators/native/*`、`/tasks/instant/*` |
| BridgePlugin（或 SchedulerPlugin 代理） | `/operators/python/*` |
| WebPlugin | web 管理相关的 `/api/*`（通过 RouterAgencyPlugin 内部端口） |

### 2.4 DatabasePlugin 自己声明路由 ✅

DatabasePlugin 通过 IRouterHandle 声明 `/channels/database/*` 路由。Scheduler 不再需要依赖 IDatabaseFactory 来处理通道管理类路由。

```cpp
class DatabasePlugin : public IPlugin, public IDatabaseFactory, public IRouterHandle {
    void EnumRoutes(std::function<void(const RouteItem&)> cb) override {
        cb({"POST", "/channels/database/add",    [this](auto& u, auto& req, auto& rsp) { return HandleAdd(req, rsp); }});
        cb({"POST", "/channels/database/remove", [this](auto& u, auto& req, auto& rsp) { return HandleRemove(req, rsp); }});
        cb({"POST", "/channels/database/modify", [this](auto& u, auto& req, auto& rsp) { return HandleModify(req, rsp); }});
        cb({"POST", "/channels/database/query",  [this](auto& u, auto& req, auto& rsp) { return HandleQuery(req, rsp); }});
    }
};
```

### 2.5 Gateway 故障恢复：定期重注册 ✅

RouterAgencyPlugin 的 KeepAlive 线程每隔 N 秒向 Gateway 重新注册路由（幂等操作）。

- 正常运行：Gateway 收到注册请求，路由已存在，更新时间戳，无事发生
- Gateway 崩溃重启：路由表为空，下一次 KeepAlive 到期时路由自动恢复
- 服务崩溃：KeepAlive 停止，Gateway 可根据超时自动清理过期路由

不需要 Gateway 做任何特殊恢复逻辑。Gateway 重启后就是空路由表，等服务来注册。

### 2.6 WebPlugin 双重身份拆分 ✅

WebPlugin 有两个职责，应分开处理：

- **Web 服务器**（对外）：静态文件、HTML 页面、前端资源 → 保持现有 `httplib::Server`，端口对外开放
- **管理 API**（内部）：`/api/*` 路由 → 通过 IRouterHandle 声明，走 RouterAgencyPlugin，端口仅监听 127.0.0.1

管理面走内部端口更安全，这些端口对外隐藏，只有 Gateway 能访问。

```
外部用户
  ├── 直接访问 → WebPlugin (0.0.0.0:8081) → 静态文件/Web UI
  └── Gateway (18800) → RouterAgencyPlugin (127.0.0.1:18802) → /api/* 管理路由
```

YAML 配置：
```yaml
- name: web
  plugins:
    - name: libflowsql_web.so
      option: "host=0.0.0.0;port=8081"        # web 服务，对外
    - name: libflowsql_router.so
      option: "host=127.0.0.1;port=18802"     # 管理 API，仅内部
```

### 2.7 IPlugin 生命周期批次调用 ✅

**已写入 CLAUDE.md 架构原则第 6 条，lessons.md L15。**

插件加载必须分阶段批次执行——先所有插件 Option()，再所有插件 Load()，最后所有插件 Start()。禁止逐个插件走完整个生命周期。

当前 `main.cpp` 的 `LoadPlugin()` 违反此原则（Load + StartAll 一气呵成），需修正为：

```cpp
// Phase 1: 加载所有插件（pluginregist → Option → Load）
for (const auto& plugin : plugin_list) {
    loader->Load(app_path, relapath, options, 1);
}
// Phase 2: 统一启动
loader->StartAll();
```

### 2.8 CORS 统一处理 ✅

当前每个插件都自己写 CORS headers，改由 RouterAgencyPlugin 统一处理，插件不再关心。

## 3. 进程管理方案：自治服务 + 轻量守护 ✅

### 3.1 设计哲学

将进程管理拆为两个正交关注点：

| 关注点 | 职责 | 实现位置 |
|--------|------|----------|
| **启动** | fork 进程 | 启动器（fork 守护 / docker-compose / systemd） |
| **自治** | 路由注册、断线重连、自动重试 | 每个服务自身（RouterAgencyPlugin KeepAlive 线程） |

启动器只需要知道"启动哪些进程"，不需要知道服务间关系、路由、健康状态。所有智能都在服务自身。启动器可以被任何外部工具替代（docker-compose、systemd），零代码改动。

### 3.2 服务自治机制

路由注册和故障恢复由 RouterAgencyPlugin 的 KeepAlive 线程统一负责（见 2.1 节），不再需要 ServiceClient。

- 启动顺序无所谓：Gateway 不可达时 KeepAlive 静默重试
- Gateway 重启后：下一次 KeepAlive 自动恢复路由
- 服务崩溃后：KeepAlive 停止，Gateway 可根据超时清理过期路由

### 3.3 GatewayPlugin 瘦身

改造后保留四个职责：

- 路由表管理（RouteTable，字典树 + 过期清理）
- 请求转发（HandleForward，不剥离前缀）
- 路由注册 API（/gateway/register、/gateway/unregister、/gateway/routes）
- 路由过期清理线程（CleanupThread）

删除：ServiceManager、HeartbeatThread、HandleHeartbeat。Gateway 不再管理任何进程。

#### 路由过期清理机制

RouteEntry 增加 `last_seen_ms` 时间戳，KeepAlive 每次注册时更新。Gateway 启动清理线程，定期移除过期条目。

RouteTable 使用 `std::shared_mutex` 保护 Trie，支持并发读（转发）和写（注册/注销）：

```cpp
struct RouteEntry {
    std::string prefix;
    std::string address;
    int64_t last_seen_ms;   // KeepAlive 每次注册时更新
};

class RouteTable {
    mutable std::shared_mutex mutex_;
    Trie<RouteEntry> trie_;

public:
    // /gateway/register：存在则更新 last_seen，不存在则新增
    void Register(const std::string& prefix, const std::string& address) {
        std::unique_lock lock(mutex_);
        auto* entry = trie_.Find(prefix);
        if (entry) {
            entry->last_seen_ms = NowMs();
            entry->address = address;
        } else {
            trie_.Insert(prefix, {prefix, address, NowMs()});
        }
    }

    // /gateway/unregister：立即删除（优雅关闭时调用）
    void Unregister(const std::string& prefix, const std::string& address) {
        std::unique_lock lock(mutex_);
        trie_.Remove(prefix);  // 仅当 address 匹配时删除，防止误删其他实例
    }

    // HandleForward：并发读
    const RouteEntry* Match(const std::string& uri) const {
        std::shared_lock lock(mutex_);
        return trie_.Match(uri);
    }

    void RemoveExpired(int64_t before_ms) {
        std::unique_lock lock(mutex_);
        trie_.RemoveIf([&](const RouteEntry& e) { return e.last_seen_ms < before_ms; });
    }
};

// 清理线程：移除超过 3 倍 KeepAlive 间隔未更新的路由
void GatewayPlugin::CleanupThread() {
    int64_t expire_ms = keepalive_interval_s_ * 3 * 1000;
    while (running_) {
        route_table_.RemoveExpired(NowMs() - expire_ms);
        for (int i = 0; i < keepalive_interval_s_ * 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
```

#### Gateway 管理端点语义

| 端点 | 方法 | 请求 Body | 响应 | 说明 |
|------|------|-----------|------|------|
| `/gateway/register` | POST | `{"prefix":"/channels","address":"127.0.0.1:18803"}` | `{"ok":true}` | 幂等，存在则更新时间戳 |
| `/gateway/unregister` | POST | `{"prefix":"/channels","address":"127.0.0.1:18803"}` | `{"ok":true}` | 立即删除，优雅关闭时调用；address 不匹配则忽略 |
| `/gateway/routes` | GET | — | `{"routes":[{"prefix":"/channels","address":"127.0.0.1:18803"},...]}` | 返回当前所有有效路由，用于 healthcheck 和运维排查 |

#### Gateway 转发失败处理

`HandleForward` 连接上游失败时，返回 `502 Bad Gateway`：

```cpp
void GatewayPlugin::HandleForward(const httplib::Request& req, httplib::Response& res) {
    auto* entry = route_table_.Match(req.path);
    if (!entry) {
        res.status = 404;
        res.set_content(R"({"error":"no route"})", "application/json");
        return;
    }
    httplib::Client cli(entry->address);
    cli.set_connection_timeout(5);
    auto result = cli.Post(req.path, req.body, "application/json");
    if (!result) {
        // 连接失败（connection refused、超时等）
        res.status = 502;
        res.set_content(R"({"error":"upstream unavailable"})", "application/json");
        return;
    }
    res.status = result->status;
    res.set_content(result->body, "application/json");
}
```

| 场景 | 行为 |
|------|------|
| 服务正常运行 | KeepAlive 每 5s 更新 last_seen，永不过期 |
| 服务崩溃 | KeepAlive 停止，15s 后路由被清理，Gateway 不再转发到死服务 |
| 服务重启 | 新的 KeepAlive 重新注册，路由恢复 |

### 3.4 部署方式一：fork 守护（开发环境）

`flowsql --config gateway.yaml` 改为极简 fork 守护，只做 fork + waitpid + respawn：

```cpp
static int RunGuardian(const std::string& config_path) {
    GatewayConfig config;
    if (LoadConfig(config_path, &config) != 0) return 1;

    // 捕获 SIGTERM/SIGINT，设置 running = false 触发优雅关闭
    signal(SIGTERM, [](int) { running = false; });
    signal(SIGINT,  [](int) { running = false; });

    // fork 所有服务（含 gateway 自身）
    std::map<pid_t, ServiceConfig> children;
    for (auto& svc : config.services) {
        pid_t pid = SpawnService(svc);
        if (pid > 0) children[pid] = svc;
    }

    // 守护循环：子进程退出就重启，仅此而已
    while (running) {
        int status;
        pid_t died = waitpid(-1, &status, WNOHANG);  // 非阻塞，配合 running 标志
        if (died > 0) {
            auto it = children.find(died);
            if (it != children.end()) {
                pid_t new_pid = SpawnService(it->second);
                children.erase(it);
                if (new_pid > 0) children[new_pid] = it->second;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // 优雅关闭：SIGTERM 所有子进程，等待退出（最多 5s）
    for (auto& [pid, _] : children) kill(pid, SIGTERM);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (auto& [pid, _] : children) {
        int status;
        while (waitpid(pid, &status, WNOHANG) == 0 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}
```

零轮询、零网络开销、零业务逻辑。不加载任何插件，不开任何端口。

### 3.5 部署方式二：docker-compose（生产环境）

所有服务使用同一个 Docker 镜像，通过不同启动参数区分角色。Docker 负责进程生命周期管理（restart: always），完全替代 fork 守护。

#### Dockerfile

```dockerfile
FROM ubuntu:22.04 AS runtime

RUN apt-get update && apt-get install -y \
    python3 python3-pip libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Python 依赖
RUN pip3 install fastapi uvicorn pyarrow

WORKDIR /opt/flowsql

# 复制构建产物
COPY build/output/flowsql          ./bin/
COPY build/output/lib*.so           ./bin/
COPY build/output/static/           ./bin/static/
COPY config/                        ./config/
COPY src/python/                    ./python/

ENV PATH="/opt/flowsql/bin:${PATH}"
ENV PYTHONPATH="/opt/flowsql/python"
```

#### docker-compose.yml

```yaml
version: '3.8'

services:
  # ---- Gateway：统一入口，路由转发 ----
  gateway:
    image: flowsql:latest
    command: >
      flowsql --role gateway --port 18800
      --plugins libflowsql_gateway.so
      --option "host=0.0.0.0;port=18800"
    ports:
      - "18800:18800"           # 对外暴露 API 入口
    networks:
      - flowsql-net
    restart: always
    healthcheck:
      test: ["CMD", "curl", "-sf", "http://localhost:18800/gateway/routes"]
      interval: 10s
      timeout: 3s
      retries: 3

  # ---- Web：前端静态资源 + 管理 API ----
  web:
    image: flowsql:latest
    command: >
      flowsql --role web --port 18802
      --gateway gateway:18800
      --plugins libflowsql_web.so,libflowsql_router.so
      --option "host=0.0.0.0;port=8081;router_host=0.0.0.0;router_port=18802"
    ports:
      - "8081:8081"             # 对外暴露 Web UI
    expose:
      - "18802"                 # 内部管理 API，仅容器网络可达
    networks:
      - flowsql-net
    depends_on:
      gateway:
        condition: service_healthy
    restart: always

  # ---- Scheduler：SQL 执行 + 通道管理 + 算子 ----
  scheduler:
    image: flowsql:latest
    command: >
      flowsql --role scheduler --port 18803
      --gateway gateway:18800
      --plugins libflowsql_router.so,libflowsql_scheduler.so,libflowsql_bridge.so,libflowsql_example.so,libflowsql_database.so
      --option "host=0.0.0.0;port=18803;config_file=config/flowsql.yml"
    expose:
      - "18803"                 # 仅内部可达
    volumes:
      - ./config:/opt/flowsql/config
    networks:
      - flowsql-net
    depends_on:
      gateway:
        condition: service_healthy
    restart: always

  # ---- Python Worker：Python 算子执行 ----
  pyworker:
    image: flowsql:latest
    command: >
      python3 -m flowsql.worker --port 18900
      --gateway gateway:18800
    expose:
      - "18900"
    networks:
      - flowsql-net
    depends_on:
      gateway:
        condition: service_healthy
    restart: always

networks:
  flowsql-net:
    driver: bridge
```

#### 与外部数据库组合

```yaml
# docker-compose.full.yml — 包含 MySQL + ClickHouse
# 使用: docker compose -f docker-compose.yml -f docker-compose.full.yml up

services:
  mysql:
    extends:
      file: config/docker-compose-mysql.yml
      service: mysql
    networks:
      - flowsql-net

  clickhouse:
    extends:
      file: config/docker-compose-clickhouse.yml
      service: clickhouse
    networks:
      - flowsql-net

  scheduler:
    depends_on:
      gateway:
        condition: service_healthy
      mysql:
        condition: service_healthy
      clickhouse:
        condition: service_healthy
```

#### docker-compose 关键设计

| 设计点 | 说明 |
|--------|------|
| 同一镜像 | 所有 C++ 服务共用 `flowsql:latest`，通过启动参数区分角色 |
| 网络隔离 | Gateway 和 Web 暴露端口，scheduler/pyworker 仅 `expose`（容器网络内可达）；RouterAgencyPlugin 监听 `0.0.0.0` 而非 `127.0.0.1`，因为 Gateway 在另一个容器，安全隔离靠 Docker 网络（`expose` 而非 `ports`）实现，而非监听地址 |
| 服务发现 | 容器名即主机名，`--gateway gateway:18800` 替代 `127.0.0.1:18800` |
| 进程管理 | `restart: always` 替代 fork 守护，Docker 负责重启 |
| 启动顺序 | `depends_on` + `service_healthy` 确保 Gateway 先就绪；服务自治重试兜底 |
| 配置挂载 | `flowsql.yml` 通过 volume 挂载，支持运行时修改数据库通道配置 |

> **裸机部署 vs 容器部署**：裸机部署时 RouterAgencyPlugin 监听 `127.0.0.1`（Gateway 同机），容器部署时监听 `0.0.0.0`（Gateway 跨容器）。两种模式通过 `option` 参数区分，代码无差异。

## 4. 业务错误码设计 ✅

### 4.1 方案

采用方案 B：handler 返回业务错误码，RouterAgencyPlugin 映射到 HTTP 状态码。handler 对 HTTP 无感知。

### 4.2 错误码定义

```cpp
// ===== src/common/error_code.h =====
namespace flowsql {
namespace error {

constexpr int32_t OK             =  0;   // 成功
constexpr int32_t BAD_REQUEST    = -1;   // 参数错误、格式非法、缺少必填字段
constexpr int32_t NOT_FOUND      = -2;   // 资源不存在（通道、算子、任务）
constexpr int32_t CONFLICT       = -3;   // 资源冲突（重复添加、状态不允许）
constexpr int32_t INTERNAL_ERROR = -4;   // 内部错误（异常、执行失败）
constexpr int32_t UNAVAILABLE    = -5;   // 依赖服务不可用（插件未加载、连接断开）

}  // namespace error
}  // namespace flowsql
```

### 4.3 映射关系

| 业务错误码 | 语义 | HTTP 状态码 | 典型场景 |
|-----------|------|------------|---------|
| `0` OK | 成功 | 200 | 正常响应 |
| `-1` BAD_REQUEST | 参数错误 | 400 | 缺少字段、JSON 解析失败、SQL 语法错误 |
| `-2` NOT_FOUND | 资源不存在 | 404 | 通道不存在、算子不存在、任务不存在 |
| `-3` CONFLICT | 状态冲突 | 409 | 通道名重复、任务正在执行不可删除 |
| `-4` INTERNAL_ERROR | 内部错误 | 500 | 数据库执行异常、算子运行崩溃 |
| `-5` UNAVAILABLE | 服务不可用 | 503 | DatabasePlugin 未加载、Python Worker 不可达 |

### 4.4 RouterAgencyPlugin 映射逻辑

```cpp
static int HttpStatus(int32_t code) {
    static const std::unordered_map<int32_t, int> mapping = {
        { error::OK,             200 },
        { error::BAD_REQUEST,    400 },
        { error::NOT_FOUND,      404 },
        { error::CONFLICT,       409 },
        { error::INTERNAL_ERROR, 500 },
        { error::UNAVAILABLE,    503 },
    };
    auto it = mapping.find(code);
    return (it != mapping.end()) ? it->second : 500;
}

void RouterAgencyPlugin::Dispatch(const httplib::Request& req, httplib::Response& res) {
    std::string key = req.method + ":" + req.path;
    auto it = route_table_.find(key);
    if (it == route_table_.end()) {
        res.status = 404;
        res.set_content(R"({"error":"route not found"})", "application/json");
        return;
    }
    std::string rsp_json;
    int32_t rc = it->second(req.path, req.body, rsp_json);
    res.status = HttpStatus(rc);
    res.set_content(rsp_json, "application/json");
}
```

### 4.5 handler 使用示例

```cpp
int32_t DatabasePlugin::HandleAdd(const std::string& req, std::string& rsp) {
    if (/* JSON 解析失败 */) {
        rsp = R"({"error":"invalid JSON"})";
        return error::BAD_REQUEST;
    }
    if (/* 通道已存在 */) {
        rsp = R"({"error":"channel already exists: mysql.mydb"})";
        return error::CONFLICT;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}
```

### 4.6 响应格式约定

```json
// 成功
{"ok": true, ...}              // 写操作
{"data": [...], ...}           // 查询操作

// 错误（所有错误响应都包含 error 字段）
{"error": "具体错误描述"}
```

## 5. 改造影响

### 5.1 新增文件

| 文件 | 说明 |
|------|------|
| `src/common/error_code.h` | 业务错误码定义 |
| `src/framework/interfaces/irouter_handle.h` | IRouterHandle 接口定义 |
| `src/services/router/router_agency_plugin.h/cpp` | RouterAgencyPlugin 实现 |
| `src/services/router/plugin_register.cpp` | 插件注册入口 |
| `src/services/router/CMakeLists.txt` | 构建 libflowsql_router.so |

### 5.2 修改文件

| 文件 | 改动 |
|------|------|
| `src/app/main.cpp` | RunService 两阶段加载；RunGateway → RunGuardian（极简 fork 守护）；去掉 ServiceClient 调用 |
| `src/services/gateway/gateway_plugin.h/cpp` | 删除 ServiceManager、HeartbeatThread、HandleHeartbeat，只保留路由转发；HandleForward 不再剥离前缀 |
| `src/services/gateway/route_table.h/cpp` | 改为字典树（Trie）匹配，废弃 ExtractPrefix/StripPrefix |
| `src/services/scheduler/scheduler_plugin.h/cpp` | 实现 IRouterHandle，删除内部 httplib::Server；路由改为 /channels/dataframe/*、/operators/native/*、/tasks/instant/* |
| `src/services/scheduler/plugin_register.cpp` | 增加 IID_ROUTER_HANDLE 注册 |
| `src/services/web/web_plugin.h/cpp` | 实现 IRouterHandle（管理 API 部分），保留 Web 服务器 |
| `src/services/database/database_plugin.h/cpp` | 实现 IRouterHandle，声明 /channels/database/* |
| `src/services/database/plugin_register.cpp` | 增加 IID_ROUTER_HANDLE 注册 |
| `config/gateway.yaml` | 插件独立 option；（Phase 2）增加 supervisor 段 |

### 5.3 可删除代码

- `SchedulerPlugin` 中的 `httplib::Server server_`、`RegisterRoutes()`、`/db-channels/*` Handler
- `WebPlugin` 中 `/api/*` 路由的 HTTP 注册代码（handler 逻辑保留，改为 IRouterHandle 声明）
- 各插件中重复的 CORS 处理代码
- `RouteTable::ExtractPrefix()`、`RouteTable::StripPrefix()` — 改为字典树匹配
- `GatewayPlugin::HeartbeatThread()`、`HandleHeartbeat()`、`ServiceManager` 成员
- `ServiceClient` 整个类（`service_client.h/cpp`）— 路由注册由 RouterAgencyPlugin 内置
- `service_manager.h/cpp` 从 gateway 目录移除（fork 守护在 main.cpp 中重写，极简版）

## 6. 测试矩阵

### 6.1 Trie 边界用例

| 用例 | 期望结果 |
|------|---------|
| 注册 `/channels`，匹配 `/channels/database/add` | 命中 `/channels` |
| 注册 `/channels` 和 `/channels/database`，匹配 `/channels/database/add` | 命中更长的 `/channels/database`（最长前缀匹配） |
| 匹配 `/unknown` | 未命中 → 404 |
| 注册空前缀 `""` 或 `/` | 拒绝注册，LOG_WARN |
| 并发注册 + 并发匹配 | 不崩溃，结果一致（shared_mutex 保护） |
| 同一前缀两个不同 address 注册 | 后注册覆盖前注册（幂等语义，address 更新） |

### 6.2 RouterAgencyPlugin 单元测试策略

```cpp
// 路由收集：传入 mock IQuerier，不依赖真实插件环境
MockQuerier mock;
mock.Register(IID_ROUTER_HANDLE, &my_plugin);
plugin.CollectRoutes(&mock);
assert(plugin.RouteCount() == 4);

// KeepAlive 注册：直接调用 RegisterOnce()，不等待线程
MockGateway mock_gw;
plugin.RegisterOnce();
assert(mock_gw.RegisterCallCount() == plugin.PrefixCount());

// 路由冲突：Traverse 按插件加载顺序遍历，先到先得是确定性行为
// 测试时控制 mock 的遍历顺序即可稳定断言
```
