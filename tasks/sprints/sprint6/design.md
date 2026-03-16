# Sprint 6 设计文档

> 状态：已评审
> 覆盖：Story 6.7（错误透传）+ Epic 7（路由代理 + 服务对等化）

---

## Part 1：错误信息透传（Story 6.7）

### 1.1 问题

`IBatchReadable::CreateReader` 和 `IBatchWritable::CreateWriter` 没有错误机制，导致数据库错误在 `RelationDbSessionBase` 层被丢弃，调用方只能拿到 -1 返回码。

错误丢失路径：
```
SchedulerPlugin::HandleExecute
  → ChannelAdapter::ReadToDataFrame(db, query, df_out, error)
    → IDatabaseChannel::CreateReader(query, reader)     ← 无错误机制
      → IBatchReadable::CreateReader                    ← 无错误机制
        → RelationDbSessionBase::CreateReader           ← 捕获 error 但丢弃
```

### 1.2 方案：统一 GetLastError() 模式

移除所有接口的 `std::string* error` 参数，统一通过 `GetLastError()` 暴露错误。

### 1.3 接口变更

**capability_interfaces.h**：
```cpp
interface IBatchReadable {
    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;
    virtual const char* GetLastError() = 0;  // 新增
};
interface IBatchWritable {
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;
    virtual const char* GetLastError() = 0;  // 新增
};
interface IArrowReadable {
    virtual int ExecuteQueryArrow(const char* sql,
        std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) = 0;  // 移除 error
    virtual const char* GetLastError() = 0;
};
interface IArrowWritable {
    virtual int WriteArrowBatches(const char* table,
        const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) = 0;  // 移除 error
    virtual const char* GetLastError() = 0;
};
interface ITransactional {
    virtual int BeginTransaction() = 0;     // 移除 error
    virtual int CommitTransaction() = 0;
    virtual int RollbackTransaction() = 0;
    virtual const char* GetLastError() = 0;
};
```

**db_session.h**（IDbSession）：新增 `last_error_`（protected）和 `GetLastError()`，所有公共方法移除 error 参数。内部钩子方法（`PrepareStatement` 等）保持 `std::string* error` 不变，直接写入 `&last_error_`。

**idatabase_channel.h**（IDatabaseChannel）：新增 `GetLastError()`，`ExecuteQueryArrow`/`WriteArrowBatches`/`ExecuteSql` 移除 error 参数。

### 1.4 实现要点

**RelationDbSessionBase / ArrowDbSessionBase**：多继承时单一 override 满足所有基类的 `GetLastError()`，`last_error_` 定义在 `IDbSession` 中共享：
```
RelationDbSessionBase
  ├── IDbSession::GetLastError()
  ├── IBatchReadable::GetLastError()   ─── 单一 override 满足全部
  └── IBatchWritable::GetLastError()
```

**DatabaseChannel**：session 是局部变量，必须在销毁前拷贝错误：
```cpp
int ret = batch_readable->CreateReader(query, reader);
if (ret != 0) last_error_ = batch_readable->GetLastError();  // 必须在 session 销毁前
return ret;
```

每个公共方法入口 `last_error_.clear()`，确保成功调用后 `GetLastError()` 返回空字符串。

**ChannelAdapter**（边界层，保持 `std::string* error` 输出不变）：
```cpp
if (db->CreateReader(query, &reader) != 0) {
    if (error) *error = db->GetLastError();
    return -1;
}
```

### 1.5 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `framework/interfaces/idatabase_channel.h` | 新增 `GetLastError()`，移除 error 参数 |
| `services/database/capability_interfaces.h` | 各接口新增 `GetLastError()`，移除 error 参数 |
| `services/database/db_session.h` | 新增 `last_error_` + `GetLastError()`，移除 error 参数 |
| `services/database/relation_db_session.h` | 适配新签名，override `GetLastError()` |
| `services/database/arrow_db_session.h` | 适配新签名，override `GetLastError()` |
| `services/database/database_channel.h/.cpp` | 新增 `last_error_`，适配新签名 |
| `services/database/arrow_adapters.h` | 移除 error 参数，从 session 拷贝错误 |
| `services/database/relation_adapters.h` | 事务调用适配新签名 |
| `framework/core/channel_adapter.h/.cpp` | 使用 `GetLastError()` |
| `services/database/drivers/sqlite_driver.h/.cpp` | 适配新签名 |
| `services/database/drivers/mysql_driver.h/.cpp` | 适配新签名 |
| `services/database/drivers/clickhouse_driver.h/.cpp` | 适配新签名，override `GetLastError()` |
| `tests/test_database/test_*.cpp` | 适配新签名 + 新增错误透传测试 |

---

## Part 2：路由代理 + 服务对等化（Epic 7）

### 2.1 问题

1. GatewayPlugin 职责过重：路由转发 + 子进程管理 + 心跳检测
2. 各插件各自创建 `httplib::Server`，多插件无法组合在同一进程
3. Scheduler 直接依赖 `IDatabaseFactory` 处理通道路由，耦合过重

### 2.2 核心接口

**irouter_handle.h**：
```cpp
namespace flowsql {

typedef std::function<int32_t(const std::string& uri,
                               const std::string& req_json,
                               std::string& rsp_json)> fnRouterHandler;
struct RouteItem {
    std::string method;   // "POST" / "GET" 等
    std::string uri;      // 完整路径，如 "/channels/database/add"
    fnRouterHandler handler;
};

const Guid IID_ROUTER_HANDLE = { /* 新 GUID */ };

interface IRouterHandle {
    virtual void EnumRoutes(std::function<void(const RouteItem&)> callback) = 0;
};
}
```

**error_code.h**：
```cpp
namespace flowsql::error {
constexpr int32_t OK             =  0;
constexpr int32_t BAD_REQUEST    = -1;   // → HTTP 400
constexpr int32_t NOT_FOUND      = -2;   // → HTTP 404
constexpr int32_t CONFLICT       = -3;   // → HTTP 409
constexpr int32_t INTERNAL_ERROR = -4;   // → HTTP 500
constexpr int32_t UNAVAILABLE    = -5;   // → HTTP 503
}
```

### 2.3 RouterAgencyPlugin

`libflowsql_router.so`，统一收集进程内所有插件路由，对外暴露单一 HTTP 端口。

**启动流程**：
```
Option()  → 解析 host、port、gateway、keepalive_interval_s
Start()   → CollectRoutes(querier_)
          → 启动 httplib::Server
          → RegisterOnce()          ← 立即注册，消除首次窗口期
          → 启动 KeepAliveThread
Stop()    → running_ = false
          → UnregisterOnce()        ← 优雅关闭
```

**CollectRoutes**（可单元测试，接受 mock IQuerier）：
```cpp
int RouterAgencyPlugin::CollectRoutes(IQuerier* querier) {
    querier->Traverse(IID_ROUTER_HANDLE, [&](void* p) -> int {
        auto* h = static_cast<IRouterHandle*>(p);
        h->EnumRoutes([&](const RouteItem& item) {
            std::string key = item.method + ":" + item.uri;
            if (route_table_.count(key))
                LOG_WARN("RouterAgency: duplicate route %s, ignored", key.c_str());
            else
                route_table_[key] = item.handler;
        });
        return 0;
    });
    // 提取一级前缀用于 Gateway 注册
    for (auto& [key, _] : route_table_) {
        auto colon = key.find(':');
        if (colon == std::string::npos) continue;
        auto uri = key.substr(colon + 1);
        auto slash2 = uri.find('/', 1);
        prefixes_.insert(slash2 != std::string::npos ? uri.substr(0, slash2) : uri);
    }
    return 0;
}
```

**Dispatch**：
```cpp
void RouterAgencyPlugin::Dispatch(const httplib::Request& req, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    if (req.method == "OPTIONS") {
        res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
        return;
    }
    auto it = route_table_.find(req.method + ":" + req.path);
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
        rsp_json = std::string(R"({"error":")") + e.what() + "\"}";
    } catch (...) {
        rsp_json = R"({"error":"unknown exception"})";
    }
    if (rsp_json.empty())
        rsp_json = (rc == error::OK) ? R"({"ok":true})" : R"({"error":"internal error"})";
    res.status = HttpStatus(rc);
    res.set_content(rsp_json, "application/json");
}
```

**RegisterOnce / UnregisterOnce**（使用 JSON 库序列化，避免注入）：
```cpp
void RouterAgencyPlugin::RegisterOnce() {
    std::string addr = host_ + ":" + std::to_string(port_);
    for (auto& prefix : prefixes_) {
        httplib::Client cli(gateway_host_, gateway_port_);
        cli.set_connection_timeout(2);
        nlohmann::json body;
        body["prefix"] = prefix;
        body["address"] = addr;
        cli.Post("/gateway/register", body.dump(), "application/json");
    }
}
// UnregisterOnce 同理，调用 /gateway/unregister
```

**KeepAliveThread**：
```cpp
void RouterAgencyPlugin::KeepAliveThread() {
    while (running_) {
        RegisterOnce();
        for (int i = 0; i < keepalive_interval_s_ * 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
```

**内部模块划分**：

| 模块 | 职责 |
|------|------|
| RouteCollector | `CollectRoutes(IQuerier*)` + 冲突检测 + 前缀提取 |
| HttpServer | httplib::Server + Dispatch + CORS + 请求体限制（1MB） |
| GatewayRegistrar | `RegisterOnce()` + `UnregisterOnce()` + KeepAlive 线程 |
| ErrorMapper | 业务错误码 → HTTP 状态码 |

`running_` 声明为 `std::atomic<bool>`。

### 2.4 URI 设计

所有操作统一 `POST` + JSON body，资源定位通过 body 字段，不使用路径参数。

| URI | 说明 |
|-----|------|
| `POST /channels/database/add` | 新增数据库通道 |
| `POST /channels/database/remove` | 删除数据库通道 |
| `POST /channels/database/modify` | 修改数据库通道 |
| `POST /channels/database/query` | 查询数据库通道 |
| `POST /channels/dataframe/query` | 查询内存通道 |
| `POST /operators/python/query` | 查询 Python 算子 |
| `POST /operators/python/refresh` | 刷新 Python 算子列表 |
| `POST /operators/native/query` | 查询 C++ 算子 |
| `POST /tasks/instant/execute` | 即时执行 SQL |
| `POST /tasks/instant/query` | 查询即时执行记录 |
| `POST /tasks/scheduled/add` | 创建定时任务 |
| `POST /tasks/scheduled/remove` | 删除定时任务 |
| `POST /tasks/scheduled/modify` | 修改定时任务 |
| `POST /tasks/scheduled/query` | 查询定时任务 |

路由归属：

| 插件 | 声明的路由 |
|------|-----------|
| DatabasePlugin | `/channels/database/*` |
| SchedulerPlugin | `/channels/dataframe/*`、`/operators/native/*`、`/tasks/*` |
| BridgePlugin | `/operators/python/*` |
| WebPlugin | web 管理相关 `/api/*` |

### 2.5 Gateway 改造

保留四个职责：路由表管理、请求转发、路由注册 API、过期清理线程。删除：ServiceManager、HeartbeatThread、HandleHeartbeat。

**RouteTable**（路径段 Trie，`std::shared_mutex` 保护并发读写）：

```cpp
struct RouteEntry {
    std::string prefix;
    std::string address;
    int64_t last_seen_ms;
};

struct TrieNode {
    std::unordered_map<std::string, std::unique_ptr<TrieNode>> children;
    std::optional<RouteEntry> entry;
};

class RouteTable {
    mutable std::shared_mutex mutex_;
    TrieNode root_;
public:
    void Register(const std::string& prefix, const std::string& address);   // 写锁
    void Unregister(const std::string& prefix, const std::string& address); // 写锁，address 匹配才删
    const RouteEntry* Match(const std::string& uri) const;                  // 读锁，最长前缀匹配
    void RemoveExpired(int64_t before_ms);                                  // 写锁，递归清理
};
```

Match 算法：按路径段逐级走 Trie，记录最后一个有 entry 的节点。

**Gateway 管理端点**：

| 端点 | 请求 Body | 响应 | 说明 |
|------|-----------|------|------|
| `POST /gateway/register` | `{"prefix":"/channels","address":"127.0.0.1:18803"}` | `{"ok":true}` | 幂等，存在则更新时间戳 |
| `POST /gateway/unregister` | `{"prefix":"/channels","address":"127.0.0.1:18803"}` | `{"ok":true}` | 立即删除，address 不匹配则忽略 |
| `GET /gateway/routes` | — | `{"routes":[{"prefix":"/channels","address":"..."},...]}` | 返回当前有效路由，用于 healthcheck |

**HandleForward**（转发完整 URI，不剥离前缀）：
```cpp
void GatewayPlugin::HandleForward(const httplib::Request& req, httplib::Response& res) {
    auto* entry = route_table_.Match(req.path);
    if (!entry) { res.status = 404; res.set_content(R"({"error":"no route"})", "application/json"); return; }
    httplib::Client cli(entry->address);
    cli.set_connection_timeout(5);
    auto result = cli.Post(req.path, req.body, "application/json");
    if (!result) { res.status = 502; res.set_content(R"({"error":"upstream unavailable"})", "application/json"); return; }
    res.status = result->status;
    res.set_content(result->body, "application/json");
}
```

**过期清理**（3 倍 KeepAlive 间隔）：
```cpp
void GatewayPlugin::CleanupThread() {
    int64_t expire_ms = keepalive_interval_s_ * 3 * 1000;
    while (running_) {
        route_table_.RemoveExpired(NowMs() - expire_ms);
        for (int i = 0; i < keepalive_interval_s_ * 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
```

| 场景 | 行为 |
|------|------|
| 服务正常 | KeepAlive 每 5s 更新时间戳，永不过期 |
| 服务崩溃 | 15s 后路由被清理，Gateway 不再转发到死服务 |
| 服务重启 | 新 KeepAlive 重新注册，路由恢复 |

### 2.6 插件迁移模式

各业务插件迁移步骤：实现 `IRouterHandle` → handler 签名改为 `fnRouterHandler` → 删除内部 `httplib::Server`。

**DatabasePlugin 示例**：
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

**WebPlugin 双重身份**：
- Web 服务器（静态文件）：保留现有 `httplib::Server`，`0.0.0.0:8081` 对外
- 管理 API（`/api/*`）：通过 IRouterHandle 声明，走 RouterAgencyPlugin

### 2.7 IPlugin 生命周期修正

当前 `main.cpp` 逐个插件走完整生命周期，违反批次调用原则。修正为：
```cpp
// Phase 1: 所有插件 Option + Load
for (const auto& plugin : plugin_list)
    loader->Load(app_path, relapath, options, 1);
// Phase 2: 统一 Start（此时所有接口已注册完毕）
loader->StartAll();
```

### 2.8 进程管理

**服务自治**：RouterAgencyPlugin KeepAlive 线程负责路由注册和故障恢复，启动顺序无所谓，Gateway 不可达时静默重试。

**RunGuardian**（开发环境，极简 fork 守护）：
```cpp
static int RunGuardian(const std::string& config_path) {
    signal(SIGTERM, [](int) { running = false; });
    signal(SIGINT,  [](int) { running = false; });

    std::map<pid_t, ServiceConfig> children;
    for (auto& svc : config.services) {
        pid_t pid = SpawnService(svc);
        if (pid > 0) children[pid] = svc;
    }
    while (running) {
        pid_t died = waitpid(-1, &status, WNOHANG);
        if (died > 0) { /* 重启子进程 */ }
        else std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // 优雅关闭：SIGTERM 所有子进程，等待最多 5s
    for (auto& [pid, _] : children) kill(pid, SIGTERM);
    // ... waitpid with deadline
}
```

**docker-compose**（生产环境）：同一镜像，不同启动参数区分角色，`restart: always` 替代 fork 守护。容器内 RouterAgencyPlugin 监听 `0.0.0.0`（Gateway 跨容器），安全隔离靠 Docker 网络（`expose` 非 `ports`）。

### 2.9 修改文件清单

**新增**：

| 文件 | 说明 |
|------|------|
| `src/common/error_code.h` | 业务错误码 |
| `src/framework/interfaces/irouter_handle.h` | IRouterHandle 接口 |
| `src/services/router/router_agency_plugin.h/.cpp` | RouterAgencyPlugin |
| `src/services/router/plugin_register.cpp` | 插件注册 |
| `src/services/router/CMakeLists.txt` | 构建 libflowsql_router.so |

**修改**：

| 文件 | 改动 |
|------|------|
| `src/app/main.cpp` | 两阶段加载；RunGateway → RunGuardian |
| `src/services/gateway/gateway_plugin.h/.cpp` | 删除 ServiceManager/HeartbeatThread；HandleForward 不剥离前缀；新增 CleanupThread |
| `src/services/gateway/route_table.h/.cpp` | 路径段 Trie + 过期清理，废弃 ExtractPrefix/StripPrefix |
| `src/services/scheduler/scheduler_plugin.h/.cpp` | 实现 IRouterHandle，删除内部 httplib::Server |
| `src/services/scheduler/plugin_register.cpp` | 注册 IID_ROUTER_HANDLE |
| `src/services/web/web_plugin.h/.cpp` | 实现 IRouterHandle（管理 API 部分） |
| `src/services/database/database_plugin.h/.cpp` | 实现 IRouterHandle |
| `src/services/database/plugin_register.cpp` | 注册 IID_ROUTER_HANDLE |

**删除**：ServiceManager、HeartbeatThread、HandleHeartbeat、ServiceClient、各插件 CORS 代码、`ExtractPrefix`/`StripPrefix`。

---

## Part 3：测试矩阵

### 3.1 错误透传测试

- `CreateReader` 查询不存在的表 → `db->GetLastError()` 包含具体原因
- ClickHouse 调用 `CreateReader` → `GetLastError()` 返回 "session does not support batch reading"
- 成功调用后 `GetLastError()` 返回空字符串

### 3.2 路由匹配边界用例

| 用例 | 期望结果 |
|------|---------|
| 注册 `/channels`，匹配 `/channels/database/add` | 命中 `/channels` |
| 注册 `/channels` 和 `/channels/database`，匹配 `/channels/database/add` | 命中更长的 `/channels/database` |
| 匹配 `/unknown` | 未命中 → 404 |
| 注册空前缀 `""` 或 `/` | 拒绝，LOG_WARN |
| 并发注册 + 并发匹配 | 不崩溃，结果一致 |
| 同一前缀两个 address 注册 | 后注册覆盖（幂等） |

### 3.3 RouterAgencyPlugin 单元测试

```cpp
// 路由收集：mock IQuerier，不依赖真实插件环境
MockQuerier mock;
mock.Register(IID_ROUTER_HANDLE, &my_plugin);
plugin.CollectRoutes(&mock);
assert(plugin.RouteCount() == 4);

// 注册：直接调用 RegisterOnce()，不等待线程
plugin.RegisterOnce();
assert(mock_gateway.RegisterCallCount() == plugin.PrefixCount());
```
