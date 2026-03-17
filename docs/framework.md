# FlowSQL 架构文档

## 核心设计原则

1. **统一框架**：所有 C++ 服务都是同一个 `flowsql` 可执行文件加载不同 `.so` 插件运行，彼此对等；Python Worker 是独立的 FastAPI 进程
2. **IPlugin 机制**：所有功能模块以 `.so` 插件形式加载，通过 `IPlugin` 接口统一生命周期管理
3. **控制面 HTTP**：服务间控制面通信全部走 HTTP + URI 路由，Gateway 负责转发
4. **数据面独立**：高吞吐数据传输走共享内存 / Arrow IPC，不经过 HTTP
5. **Interface 思维**：Plugin 通过纯虚接口（`IID_*`）向进程内其他 Plugin 暴露能力，调用方通过 `IQuerier::Traverse` 按 IID 查找，不直接依赖具体实现类
6. **IPlugin 生命周期批次调用**：插件加载分阶段批次执行——先所有插件 `Option()`，再所有插件 `Load()`，最后所有插件 `Start()`。禁止逐个插件走完整生命周期，因为 `Start()` 时必须保证所有插件的接口已通过 `Load()` 注册完毕

---

## 服务拓扑

```
                    ┌─────────────────────┐
                    │       Gateway       │
                    │  (libflowsql_gateway.so)  │
                    │  Trie 路由表 / 过期清理   │
                    │  子进程生命周期管理        │
                    └──────┬──────────────┘
                           │ Guardian fork + HTTP 转发
              ┌────────────┼────────────┐
              │            │            │
     ┌────────▼───┐  ┌─────▼──────┐  ┌─▼─────────────┐
     │    Web     │  │ Scheduler  │  │ Python Worker  │
     │ web.so     │  │ sched.so   │  │   (FastAPI)    │
     │ router.so  │  │ router.so  │  │  Python 算子   │
     │ 前端 + API │  │ bridge.so  │  └────────────────┘
     └────────────┘  │ database.so│
                     │ example.so │
                     └────────────┘
```

**端口分配**：Gateway 18800 · Web 8081 + 18802 · Scheduler 18803 · PyWorker 18900

---

## Gateway

### 职责
- 维护 URI 前缀路由表（Trie 实现，最长前缀匹配）
- HTTP 请求转发：完整 URI 转发给目标服务（不剥离前缀）
- 过期路由清理：定期移除超过 TTL 未续期的路由条目
- 自身也是框架 + `gateway.so`，与其他服务对等

### 路由机制

两层路由，各司其职：

**第一层 — Gateway Trie 匹配（前缀路由）**：RouterAgencyPlugin 启动时从收集到的路由中自动提取一级前缀（如 `/channels`、`/operators`、`/tasks`），注册到 Gateway。Gateway 按路径段逐级做最长前缀匹配，将**完整 URI 原样转发**给目标服务，不剥离前缀、不改写路径。

**第二层 — RouterAgencyPlugin 精确匹配**：目标服务的 RouterAgencyPlugin 对 `METHOD:URI` 做精确匹配，分发给对应的 `fnRouterHandler`。

```
POST /channels/database/add
  ① Gateway Trie 匹配 /channels → 目标 127.0.0.1:18803
  ② 转发完整 URI：POST 127.0.0.1:18803/channels/database/add
  ③ Scheduler RouterAgencyPlugin 精确匹配 "POST:/channels/database/add" → DatabasePlugin::HandleAdd
```

路由条目带 `last_seen_ms` 时间戳，RouterAgencyPlugin 的 KeepAlive 线程定期重注册以续期；超过 TTL（`heartbeat_interval_s × heartbeat_timeout_count`）未续期的条目自动清理。

### Gateway 内置接口

| 接口 | 方法 | 说明 |
|------|------|------|
| `/gateway/register` | POST | 注册路由 `{ "prefix": "/channels", "address": "127.0.0.1:18803" }` |
| `/gateway/unregister` | POST | 注销路由 `{ "prefix": "/channels" }` |
| `/gateway/routes` | GET | 查询所有已注册路由 |

### 心跳机制（服务自治）

Sprint 6 起，心跳改为**路由续期**模式：各服务的 `RouterAgencyPlugin` 内置 KeepAlive 线程，定期向 Gateway 重注册路由前缀（幂等操作，更新 `last_seen_ms`）。Gateway 不再主动 ping 服务，而是通过路由过期被动感知服务下线。

---

## RouterAgencyPlugin（`libflowsql_router.so`）

Sprint 6 引入的核心组件，每个 C++ 服务（Web、Scheduler）都加载此插件。

### 职责
1. `Start()` 时通过 `Traverse(IID_ROUTER_HANDLE)` 收集进程内所有业务插件声明的路由
2. 启动 `httplib::Server`，catch-all 分发到对应 `fnRouterHandler`
3. 向 Gateway 注册路由前缀，并通过 KeepAlive 线程定期续期

### IRouterHandle 接口

业务插件实现 `IRouterHandle`，声明自己提供的路由，对 HTTP 完全无感知：

```cpp
interface IRouterHandle {
    virtual void EnumRoutes(std::function<void(const RouteItem&)> callback) = 0;
};

struct RouteItem {
    std::string method;   // "GET" / "POST" / "PUT" / "DELETE"
    std::string uri;      // 完整路径，如 "/tasks/instant/execute"
    fnRouterHandler handler;
};

// 路由处理函数签名：纯业务逻辑，不感知 HTTP
typedef std::function<int32_t(const std::string& uri,
                               const std::string& req_json,
                               std::string& rsp_json)> fnRouterHandler;
```

### 业务错误码

`fnRouterHandler` 返回 `flowsql::error::*`，RouterAgencyPlugin 自动映射为 HTTP 状态码：

| 错误码 | 值 | HTTP 状态码 |
|--------|-----|------------|
| `error::OK` | 0 | 200 |
| `error::BAD_REQUEST` | -1 | 400 |
| `error::NOT_FOUND` | -2 | 404 |
| `error::CONFLICT` | -3 | 409 |
| `error::INTERNAL_ERROR` | -4 | 500 |
| `error::UNAVAILABLE` | -5 | 503 |

---

## Scheduler

Scheduler 进程加载多个插件，通过 `IQuerier` 进行进程内插件间通信：

| 插件 | IID | 职责 |
|------|-----|------|
| `libflowsql_router.so` | — | HTTP 服务 + 路由代理 + Gateway KeepAlive |
| `libflowsql_scheduler.so` | `IID_SCHEDULER` | SQL 解析、Pipeline 执行、任务管理 |
| `libflowsql_bridge.so` | `IID_BRIDGE` | C++ ↔ Python 算子桥接，发现并注册 Python 算子 |
| `libflowsql_database.so` | `IID_DATABASE_FACTORY` | 数据库通道管理（MySQL / SQLite / ClickHouse） |
| `libflowsql_example.so` | `IID_OPERATOR` | 示例 C++ 算子 |

### SQL 执行流程

```
用户 SQL → Web POST /api/tasks
  → Web 转发 POST /tasks/instant/execute → Gateway → Scheduler
  → SqlParser 解析（source / operator / dest）
  → FindChannel(source)  ← 支持三段式 type.name.table
  → ExecuteTransfer / ExecuteWithOperator
  → 返回结果 JSON
```

**三段式寻址**：`mysql.prod.test_users`
- `mysql` → 通道类型（type）
- `prod` → 通道逻辑名（name，由用户添加通道时指定）
- `test_users` → 表名（BuildQuery 替换 FROM 子句）

### 数据库通道管理

`IDatabaseFactory`（由 `database.so` 实现）提供：
- `Get(type, name)` → 懒加载连接，断线自动重连
- `AddChannel / RemoveChannel / UpdateChannel` → 运行时动态管理
- `List` → 枚举通道，密码字段脱敏

通道配置持久化到 `config/flowsql.yml`，密码 AES-256-GCM 加密（`ENC:` 前缀）。Scheduler 重启后自动从 YAML 恢复通道。

支持的数据库类型：

| 类型 | 驱动 | 连接方式 |
|------|------|---------|
| `sqlite` | SqliteDriver | 文件 / `:memory:` |
| `mysql` | MysqlDriver | 连接池（libmysqlclient） |
| `clickhouse` | ClickHouseDriver | HTTP 无状态（httplib，无连接池） |

### Scheduler 端点

**tasks — 任务执行**（SchedulerPlugin）

| 端点 | 说明 |
|------|------|
| `POST /tasks/instant/execute` | 即时执行 SQL |

**channels — 数据通道**

| 端点 | 说明 |
|------|------|
| `POST /channels/dataframe/query` | 查询内存通道（SchedulerPlugin） |
| `POST /channels/database/add` | 新增数据库通道（DatabasePlugin） |
| `POST /channels/database/remove` | 删除数据库通道（DatabasePlugin） |
| `POST /channels/database/modify` | 修改数据库通道（DatabasePlugin） |
| `POST /channels/database/query` | 查询数据库通道（DatabasePlugin） |

**operators — 算子**（SchedulerPlugin）

| 端点 | 说明 |
|------|------|
| `POST /operators/native/query` | 查询 C++ 算子 |
| `POST /operators/python/refresh` | 刷新 Python 算子列表 |

---

## Python 算子

### 注册流程

```
Guardian spawn Gateway，Gateway spawn Scheduler 和 PyWorker
  ↓
BridgePlugin::Load（Scheduler 进程内）：
  轮询 GET /gateway/routes（最多 10 次，间隔 1s）
  → 找到 /pyworker 前缀对应的地址，记录 PyWorker host:port
  ↓
BridgePlugin::Start：
  直连 PyWorker，GET /operators（最多 30 次重试，间隔 1s）
  → 解析算子元数据列表
  → 为每个算子创建 PythonOperatorBridge，存入内部 registered_operators_
  （不注册到 PluginLoader，不走 IQuerier）
```

Scheduler 查找算子时两条路径：
1. `IQuerier::Traverse(IID_OPERATOR)` → C++ 算子（进程内）
2. `IBridge::FindOperator(catelog, name)` → Python 算子（Bridge 内部查找）

**Reload**：用户在 Web 触发 → `POST /operators/python/refresh` → Bridge 重新调用 `DiscoverOperators()` → 更新 `registered_operators_`。

### 执行数据面

**C++ 算子**：进程内直接函数调用，零开销。

**Python 算子**：跨进程，共享内存传数据，HTTP 只传控制指令。

```
Pipeline → operator->Work(source, sink)
  （实际调用 PythonOperatorProxy → PythonOperatorBridge）
  1. 从 source IChannel 读取 Arrow RecordBatch
  2. Arrow IPC 序列化写入 /dev/shm/flowsql_<uuid>_in
     （数据量超阈值时回退到 /tmp，Python 侧无感知）
  3. POST /pyworker/work/<catelog>/<name>  { "input": "/dev/shm/..." }
  4. Python Worker:
       pa.memory_map(path) → Arrow Table（零拷贝）
       → Polars DataFrame → operator.work(df) → df_out
       → Arrow IPC 写入 /dev/shm/flowsql_<uuid>_out
       → 返回 200 { "output": "/dev/shm/..." }
  5. Bridge: memory_map 读取 _out → Arrow RecordBatch → 写入 sink IChannel
  6. SharedMemoryGuard 析构，自动 unlink _in / _out
```

Pipeline 统一面向 `IOperator` 接口，不感知算子是 C++ 还是 Python。

**共享内存生命周期**：所有权归 Bridge（创建 _in，读取 _out，清理两者）；PyWorker 无状态，只读写不清理。Scheduler 启动时扫描 `/dev/shm/flowsql_*` 清理上次残留。

---

## 部署配置

### 多进程模式（`config/deploy-multi.yaml`）

```yaml
mode: guardian   # Guardian 进程 fork 子进程

services:
  - name: gateway
    plugins:
      - name: libflowsql_gateway.so
        option: "host=127.0.0.1;port=18800;heartbeat_interval_s=10;heartbeat_timeout_count=3"

  - name: web
    plugins:
      - name: libflowsql_web.so
        option: "host=127.0.0.1;port=8081;gateway=127.0.0.1:18800"
      - name: libflowsql_router.so
        option: "host=127.0.0.1;port=18802;gateway=127.0.0.1:18800"

  - name: scheduler
    plugins:
      - name: libflowsql_router.so
        option: "host=127.0.0.1;port=18803;gateway=127.0.0.1:18800"
      - libflowsql_scheduler.so
      - libflowsql_bridge.so
      - libflowsql_example.so
      - name: libflowsql_database.so
        option: "config_file=config/flowsql.yml"

  - name: pyworker
    type: python
    command: "python3 -m flowsql.worker --port 18900"
```

### 单进程模式（`config/deploy-single.yaml`）

所有 C++ 插件在同一进程内加载，pyworker 独立 fork：

```yaml
services:
  - name: all
    plugins:
      - name: libflowsql_gateway.so
        option: "host=127.0.0.1;port=18800;..."
      - name: libflowsql_web.so
        option: "host=127.0.0.1;port=8081;gateway=127.0.0.1:18800"
      - name: libflowsql_router.so
        option: "host=127.0.0.1;port=18803;gateway=127.0.0.1:18800"
      - libflowsql_scheduler.so
      - libflowsql_bridge.so
      - libflowsql_example.so
      - name: libflowsql_database.so
        option: "config_file=config/flowsql.yml"

  - name: pyworker
    type: python
    command: "python3 -m flowsql.worker --port 18900"
```

**`config/flowsql.yml`**（运行时通道配置，由 Web 动态管理）：

```yaml
channels:
  database_channels:
    - type: mysql
      name: prod
      host: 127.0.0.1
      port: 3306
      user: flowsql_user
      password: "ENC:..."
      database: flowsql_db
```

---

## 项目结构

```
flowSQL/
├── src/
│   ├── common/             # 公共头文件（define.h、error_code.h、loader.hpp、toolkit.hpp 等）
│   ├── framework/          # 框架核心（IPlugin、PluginLoader、SqlParser、Pipeline 等）
│   │   └── interfaces/     # 跨插件接口（IDatabaseFactory、IRouterHandle 等）
│   ├── services/
│   │   ├── gateway/        # libflowsql_gateway.so（Trie 路由表、转发、过期清理）
│   │   ├── router/         # libflowsql_router.so（RouterAgencyPlugin、KeepAlive）
│   │   ├── scheduler/      # libflowsql_scheduler.so
│   │   ├── database/       # libflowsql_database.so（含 MySQL/SQLite/ClickHouse 驱动）
│   │   ├── bridge/         # libflowsql_bridge.so
│   │   └── web/            # libflowsql_web.so + 前端静态文件
│   ├── plugins/
│   │   └── example/        # libflowsql_example.so
│   ├── python/             # Python Worker（FastAPI + 算子运行时）
│   ├── frontend/           # Vue.js 前端
│   ├── app/                # flowsql 可执行文件入口（main.cpp）
│   └── tests/
│       ├── test_framework/
│       ├── test_database/  # test_sqlite / test_mysql / test_clickhouse / test_database / test_database_manager
│       ├── test_bridge/
│       ├── test_router/    # RouterAgencyPlugin 单元测试（11 个用例）
│       └── test_npi/
├── config/
│   ├── deploy-multi.yaml   # 多进程部署配置（guardian 模式）
│   ├── deploy-single.yaml  # 单进程部署配置
│   └── flowsql.yml         # 运行时通道配置（自动生成）
├── build/output/           # 编译产物
├── .thirdparts_installed/  # 第三方依赖安装缓存
└── docs/
```

---

## 构建与运行

```bash
# 构建
cmake -B build src && cmake --build build -j$(nproc)

# 启动（多进程模式，从项目根目录）
cd build/output && LD_LIBRARY_PATH=. ./flowsql --config ../../config/deploy-multi.yaml

# 启动（单进程模式）
cd build/output && LD_LIBRARY_PATH=. ./flowsql --config ../../config/deploy-single.yaml

# 测试
cd build/output
./test_router          # RouterAgencyPlugin 路由单元测试（11 个用例）
./test_database        # 插件层 E2E（需 MySQL 环境）
./test_database_manager
./test_sqlite
./test_clickhouse      # ClickHouse 不可达时自动 SKIP
```
