# Epic 7 设计文档评审意见

> 评审对象：`design_router_agency.md`
> 评审日期：2026-03-16
> 评审维度：功能流程完整性 / 可测试性 / 逻辑边界安全性

---

## 一、功能流程完整性

### 缺口 1：首次注册窗口期【严重】

`Start()` 启动 KeepAlive 线程后，线程第一次执行有延迟（`keepalive_interval_s` 秒后才注册）。
这段时间内 Gateway 没有路由，所有请求返回 404。

**建议**：Start() 中在启动线程前先同步注册一次，线程负责后续续期。

```cpp
int RouterAgencyPlugin::Start() {
    // ... 收集路由、启动 HTTP ...
    RegisterOnce();  // 立即注册一次，消除窗口期
    if (!gateway_host_.empty())
        keepalive_thread_ = std::thread(&RouterAgencyPlugin::KeepAliveThread, this);
    return 0;
}
```

### 缺口 2：Gateway 转发失败未定义【严重】

路由过期前（最长 `3 × keepalive_interval` 秒），服务已崩溃，Gateway 转发会 `connection refused`。
设计完全没有说明此时返回什么给客户端（502？503？超时多少？）。这是用户可见的错误路径，必须明确。

**建议**：在 `HandleForward` 中捕获连接失败，返回 `502 Bad Gateway` + `{"error":"upstream unavailable"}`。

### 缺口 3：OPTIONS 预检请求（CORS Preflight）【中】

设计说 CORS 统一处理，但 `Dispatch` 只处理路由表中存在的方法。
浏览器发出的 `OPTIONS` 预检请求不在路由表里，会返回 404，导致跨域请求全部失败。

**建议**：在 catch-all handler 中专门处理 `OPTIONS` 方法：

```cpp
if (req.method == "OPTIONS") {
    res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    res.status = 204;
    return;
}
```

### 缺口 4：`rsp_json` 为空时的行为【中】

```cpp
int32_t rc = it->second(req.path, req.body, rsp_json);
res.set_content(rsp_json, "application/json");  // rsp_json 可能为空
```

handler 返回错误码但忘记设置 `rsp_json` 时，响应体为空，客户端无法解析。

**建议**：Dispatch 中兜底处理：

```cpp
if (rsp_json.empty() && rc != error::OK)
    rsp_json = R"({"error":"internal error"})";
```

### 缺口 5：`/gateway/unregister` 和 `/gateway/routes` 语义未定义【中】

3.3 节列出了这两个端点，但均未定义：

- `/gateway/unregister`：何时调用？谁调用？调用后立即删除还是标记过期？服务优雅关闭时是否主动调用？
- `/gateway/routes`：docker-compose healthcheck 依赖它，但返回格式未定义。

**建议**：补充两个端点的请求/响应格式和调用时机说明。

### 缺口 6：`service_name_` 来源未定义【低】

KeepAlive 注册时发送 `service_name_`，但 `Option()` 解析的参数列表里没有这个字段，来源不明。
需明确是从配置参数读取还是由框架注入。

### 缺口 7：docker-compose 与设计文档矛盾【低】

设计文档 2.6 节说管理 API 应监听 `127.0.0.1:18802`（仅内部），
但 docker-compose 示例写的是 `router_host=0.0.0.0`，导致管理 API 对外暴露。两处需要统一。

---

## 二、可测试性

### 问题 1：RouterAgencyPlugin 路由收集无法单元测试【严重】

`Start()` 中的路由收集依赖 `IQuerier::Traverse`，需要真实的插件加载环境。
Story 7.2 验收标准写了"先用 mock 测试"，但没有定义 mock 策略。

**建议**：将路由收集逻辑提取为独立方法，测试时传入 mock IQuerier：

```cpp
// 可测试的接口
int CollectRoutes(IQuerier* querier);

// 测试代码
MockQuerier mock;
mock.Register(IID_ROUTER_HANDLE, &my_plugin);
plugin.CollectRoutes(&mock);
assert(plugin.RouteCount() == 4);
```

### 问题 2：KeepAlive 线程不可确定性测试【中】

线程依赖真实时间（sleep），测试需要等待 `keepalive_interval_s` 秒。
Story 7.8 的"Gateway 重启恢复测试"如果是集成测试，耗时不可控。

**建议**：将注册逻辑提取为 `RegisterOnce()`，测试直接调用该方法，不依赖线程调度：

```cpp
// 单元测试
plugin.RegisterOnce();  // 直接触发，不等待线程
assert(mock_gateway.RegisterCallCount() == prefixes_.size());
```

### 问题 3：路由冲突检测结果不可断言【中】

"先到先得"依赖 `Traverse` 遍历顺序，而顺序取决于插件加载顺序，测试无法稳定断言"哪个 handler 赢了"。

**建议**：明确 Traverse 遍历顺序（按插件加载顺序），或在冲突时 fail-fast（`Start()` 返回 -1 拒绝启动），而不是静默忽略。

### 问题 4：Trie 边界用例缺失【中】

planning.md 提到"Trie 单元测试覆盖所有路由模式"，但设计文档没有列出边界用例。
建议补充以下测试矩阵：

| 用例 | 期望结果 |
|------|---------|
| 注册 `/channels`，匹配 `/channels/database/add` | 命中 `/channels` |
| 注册 `/channels` 和 `/channels/database`，匹配 `/channels/database/add` | 命中更长的 `/channels/database` |
| 匹配 `/unknown` | 未命中 → 404 |
| 注册空前缀 `""` 或 `/` | 应拒绝或明确行为 |
| 并发注册 + 并发匹配 | 不崩溃，结果一致 |
| 同一前缀两个服务注册 | 明确覆盖/报错/负载均衡策略 |

---

## 三、逻辑边界安全性

### 问题 1：JSON 注入【高危】

```cpp
// 当前实现（危险）
std::string body = R"({"prefix":")" + prefix +
                   R"(","address":")" + local_addr +
                   R"(","service":")" + service_name_ + R"("})";
```

`prefix` 来自插件声明的路由，若包含 `"` 或 `\`，产生非法 JSON，Gateway 解析失败，注册静默丢失。

**建议**：必须用 JSON 库序列化，不能字符串拼接：

```cpp
// 使用 nlohmann/json 或项目已有的 JSON 库
nlohmann::json body;
body["prefix"]  = prefix;
body["address"] = local_addr;
body["service"] = service_name_;
cli.Post("/gateway/register", body.dump(), "application/json");
```

### 问题 2：`running_` 标志原子性【高危】

KeepAlive 线程读 `running_`，`Stop()` 写 `running_`，跨线程访问必须是 `std::atomic<bool>`。
设计代码中未体现，实现时容易遗漏导致数据竞争。

```cpp
// 必须声明为原子类型
std::atomic<bool> running_{false};
```

### 问题 3：Gateway Trie 并发读写【高危】

`/gateway/register` 写 Trie，`HandleForward` 读 Trie，两者都在 HTTP 线程池中并发执行。
设计没有提及任何锁保护。

**建议**：使用读写锁保护 Trie：

```cpp
mutable std::shared_mutex trie_mutex_;

// 写（注册）
std::unique_lock lock(trie_mutex_);
trie_.Insert(prefix, entry);

// 读（转发）
std::shared_lock lock(trie_mutex_);
auto* entry = trie_.Match(uri);
```

### 问题 4：handler 异常安全【中】

```cpp
// 当前实现（危险）
int32_t rc = it->second(req.path, req.body, rsp_json);
```

handler 抛异常会导致 HTTP 线程崩溃，进而整个服务崩溃。

**建议**：Dispatch 包裹 try-catch：

```cpp
try {
    rc = it->second(req.path, req.body, rsp_json);
} catch (const std::exception& e) {
    LOG_ERROR("handler exception: %s", e.what());
    rsp_json = R"({"error":"handler exception"})";
    rc = error::INTERNAL_ERROR;
} catch (...) {
    rsp_json = R"({"error":"unknown exception"})";
    rc = error::INTERNAL_ERROR;
}
```

### 问题 5：前缀提取的边界 UB【中】

```cpp
auto uri = key.substr(key.find(':') + 1);
```

若 `key` 中没有 `:`（格式非法），`find` 返回 `npos`，`npos + 1 = 0`，`substr(0)` 返回整个字符串，语义错误。

**建议**：加格式校验：

```cpp
auto colon = key.find(':');
if (colon == std::string::npos) {
    LOG_WARN("invalid route key: %s", key.c_str());
    continue;
}
auto uri = key.substr(colon + 1);
```

### 问题 6：fork 守护的信号处理【中】

`RunGuardian` 中 `while (running)` 依赖 `running` 标志，但没有说明如何设置 `running = false`。
若 SIGTERM 直接终止守护进程，子进程变成孤儿进程。

**建议**：明确信号处理流程：
1. 守护进程捕获 SIGTERM → 设置 `running = false`
2. 退出 waitpid 循环
3. 向所有子进程发 SIGTERM
4. 等待子进程退出（带超时）

### 问题 7：请求体无大小限制【低】

没有 `max_body_size` 限制，恶意客户端可发送超大请求体导致 OOM。

**建议**：httplib 支持 `set_payload_max_length()`，建议设置合理上限（如 1MB）：

```cpp
server_.set_payload_max_length(1 * 1024 * 1024);  // 1MB
```

---

## 四、问题汇总

| # | 维度 | 问题 | 严重度 | 建议处理阶段 |
|---|------|------|--------|------------|
| 1 | 功能完整性 | 首次注册窗口期 | 严重 | Story 7.2 实现前补充设计 |
| 2 | 功能完整性 | Gateway 转发失败未定义 | 严重 | Story 7.3 实现前补充设计 |
| 3 | 可测试性 | 路由收集无法单元测试 | 严重 | Story 7.2 实现前补充设计 |
| 4 | 安全性 | JSON 注入 | 高危 | Story 7.2 实现时必须修复 |
| 5 | 安全性 | `running_` 原子性 | 高危 | Story 7.2 实现时必须修复 |
| 6 | 安全性 | Trie 并发读写 | 高危 | Story 7.3 实现时必须修复 |
| 7 | 功能完整性 | OPTIONS 预检请求 | 中 | Story 7.2 实现时处理 |
| 8 | 功能完整性 | `rsp_json` 为空兜底 | 中 | Story 7.2 实现时处理 |
| 9 | 功能完整性 | 端点语义未定义 | 中 | Story 7.3 实现前补充设计 |
| 10 | 可测试性 | KeepAlive 不可确定性测试 | 中 | Story 7.2 实现时处理 |
| 11 | 可测试性 | 冲突检测不可断言 | 中 | Story 7.2 实现前明确策略 |
| 12 | 可测试性 | Trie 边界用例缺失 | 中 | Story 7.3 测试前补充 |
| 13 | 安全性 | handler 异常安全 | 中 | Story 7.2 实现时处理 |
| 14 | 安全性 | 前缀提取边界 UB | 中 | Story 7.2 实现时处理 |
| 15 | 安全性 | fork 守护信号处理 | 中 | Story 7.7 实现前补充设计 |
| 16 | 功能完整性 | `service_name_` 来源 | 低 | Story 7.2 实现时明确 |
| 17 | 功能完整性 | docker-compose 配置矛盾 | 低 | Story 7.7 实现时统一 |
| 18 | 安全性 | 请求体无大小限制 | 低 | Story 7.2 实现时处理 |
