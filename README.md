# FlowSQL

基于 SQL 语法的网络流量分析共创平台

## 项目简介

FlowSQL 是一个全栈式网络流量分析平台，通过扩展的 SQL 语法提供从数据采集、流量分析到数据探索的完整能力。用户无需深入了解底层技术（如 DPDK、Hyperscan），只需使用熟悉的 SQL 语句即可构建自己的流量分析系统。

平台采用 Gateway + RouterAgency 插件架构：所有 C++ 服务共享同一个框架程序（加载不同 .so），业务插件通过 IRouterHandle 声明路由，RouterAgencyPlugin 统一收集并向 Gateway 注册，Python Worker 作为独立 FastAPI 进程运行。控制面统一走 HTTP + URI 路由，数据面通过共享内存 / Arrow IPC 实现零拷贝传输。

## 核心特性

- **插件化架构**：所有功能模块以 .so 插件形式加载，统一框架程序驱动
- **RouterAgency 路由**：业务插件实现 IRouterHandle 声明路由，RouterAgencyPlugin 收集后向 Gateway 注册，插件对 HTTP 完全无感知
- **Gateway 转发**：Trie 最长前缀匹配，KeepAlive 自动续期，服务故障自动重启
- **C++ ↔ Python 桥接**：共享内存 + Arrow IPC 零拷贝数据传输，HTTP 仅传控制指令
- **SQL 驱动**：扩展 SQL 语法统一数据采集、分析、探索操作
- **三类算子统一管理**：内置算子（builtin）+ Python 算子 + C++ 插件算子统一走 `/api/operators/*`
- **Web 管理**：Vue.js 前端 + REST API，支持通道/算子/任务管理

## 快速开始

### 开发依赖服务

**MySQL**

```bash
docker compose -f config/docker-compose-mysql.yml up -d
```

连接参数：`127.0.0.1:3306`，用户 `flowsql_user`，密码 `flowSQL@user`，库 `flowsql_db`

**ClickHouse**

```bash
docker compose -f config/docker-compose-clickhouse.yml up -d
```

连接参数：HTTP `127.0.0.1:8123`，TCP `127.0.0.1:9000`，用户 `flowsql_user`，密码 `flowSQL@user`，库 `flowsql_db`

**PostgreSQL**

```bash
docker compose -f config/docker-compose-postgres.yml up -d
```

连接参数：`127.0.0.1:5432`，用户 `flowsql_user`，密码 `flowSQL@user`，库 `flowsql_db`

### 环境要求

- CMake 3.12+，C++17 编译器（GCC 7+），Linux
- Python 3.8+（Python 算子运行时）

```bash
pip3 install -e src/python/ --break-system-packages
```

### 编译

```bash
cmake -B build src && cmake --build build -j$(nproc)
```

### 运行

**单进程模式（开发调试）**

```bash
cd build/output
LD_LIBRARY_PATH=. ./flowsql --config ../../config/deploy-single.yaml
```

**多进程模式（生产部署）**

```bash
cd build/output
LD_LIBRARY_PATH=. ./flowsql --config ../../config/deploy-multi.yaml
```

启动后浏览器访问 `http://127.0.0.1:8081` 进入管理界面。

### 前端构建

```bash
cd src/frontend && npm install && npm run build
# 构建产物由 CMake 自动同步到 build/output/static/
# 或手动：rm -rf build/output/static/assets && cp -r src/frontend/dist/* build/output/static/
```

### 测试

```bash
cd build/output
./test_framework && ./test_bridge
./test_sqlite
./test_connection_pool
export FLOWSQL_SECRET_KEY="your-32-byte-secret-key-here!!"
./test_database_manager
./test_mysql        # 需运行中的 MySQL
./test_postgres     # 需运行中的 PostgreSQL
./test_clickhouse   # 需运行中的 ClickHouse
./test_router       # 路由表单元测试
./test_builtin      # Catalog/BinAddon/算子管理链路
```

## 架构

### 服务拓扑

```
浏览器
  │
  ▼
WebPlugin (8081) ── 静态文件 / API 代理入口
  │  去掉 /api 前缀
  ▼
GatewayPlugin (18800) ── Trie 最长前缀匹配转发
  ├── /api      → RouterAgencyPlugin (18802) → WebPlugin 路由
  ├── /channels → RouterAgencyPlugin (18803) → DatabasePlugin / SchedulerPlugin 路由
  ├── /tasks    → RouterAgencyPlugin (18803) → SchedulerPlugin 路由
  └── /operators→ RouterAgencyPlugin (18803) → CatalogPlugin 统一入口
                                             → BinAddonHostPlugin（C++ 插件生命周期）
                                             → BridgePlugin / PyWorker（Python 算子发现与执行）
```

### 请求链路

```
前端 → POST /api/channels/database/add (8081)
  → WebPlugin 去掉 /api → POST /channels/database/add → Gateway (18800)
  → Trie 匹配 /channels → RouterAgencyPlugin (18803)
  → DatabasePlugin::HandleAdd
```

### 核心接口

```
IPlugin（生命周期）
├── IRouterHandle（声明 HTTP 路由，对 HTTP 无感知）
├── IChannel（数据通道）
│   ├── IDataFrameChannel（批处理）
│   ├── IDatabaseChannel（数据库 Reader/Writer 工厂）
│   └── IStreamChannel（流式，设计中）
└── IOperator（数据算子：Work(in, out)）
```

### URI 设计约束

详见 [架构设计](docs/framework.md)。

**概要：**

| 层 | 格式 | 示例 |
|---|---|---|
| 前端对外（WebPlugin 8081） | `/api/{资源}[/{动作}]` | `/api/channels`, `/api/tasks/result` |
| 内部服务间（RouterAgencyPlugin） | `/{资源类型}/{子类型}/{动作}` | `/channels/database/add`, `/tasks/instant/execute` |
| Gateway 管理（内部专用） | `/gateway/{动作}` | `/gateway/register`, `/gateway/routes` |

动作词汇：`query` / `add` / `remove` / `modify` / `execute` / `refresh` / `reload`

## SQL 语法示例

```sql
-- 数据采集
SELECT * FROM netcard USING npm INTO ts.db1

-- 探索性分析（Python 算子）
SELECT * FROM example.memory USING explore.chisquare WITH target='label'

-- 统计分析
SELECT bps FROM ts.npm.tcp_session USING statistic.hist
WHERE time = '[2024/07/14 00:00:00 - 2024/07/14 23:59:59]'

-- C++ 插件算子（示例）
SELECT * FROM dataframe.input USING sample.column_stats INTO dataframe.stats
```

## 算子扩展能力

### Python 算子扩展

Python 算子以 `.py` 文件形式扩展，运行在独立 Python Worker 进程中，通过 BridgePlugin 与 C++ 调度面协作。

开发方式：

1. 继承 `OperatorBase`。
2. 使用 `@register_operator(category, name, description, position)` 注册元数据。
3. 实现 `work(df_in)`，输入输出均为 DataFrame（默认 Polars）。

管理能力（统一 API）：

- 上传：`POST /api/operators/upload`（`type=python`，支持 `multipart file` 或 `content`）
- 激活：`POST /api/operators/activate`（`{"type":"python","name":"category.name"}`）
- 去激活：`POST /api/operators/deactivate`
- 详情/编辑/删除：`/api/operators/detail`、`/api/operators/update`、`/api/operators/delete`

### C++ 算子插件扩展（.so）

C++ 算子以“插件文件（`.so`）”为管理单元。一个插件可包含多个算子，统一由 BinAddonHostPlugin 托管生命周期。

插件必须导出 4 个符号（`extern "C"`）：

1. `flowsql_abi_version`
2. `flowsql_operator_count`
3. `flowsql_create_operator`
4. `flowsql_destroy_operator`

管理能力（统一 API）：

- 上传：`POST /api/operators/upload`（`type=cpp`）
- 激活/去激活：`POST /api/operators/activate|deactivate`（按 `plugin_id`）
- 列表/详情：`GET /api/operators/list?type=cpp`、`POST /api/operators/detail`
- 删除：`POST /api/operators/delete`

关键行为：

- 上传后生成稳定 `plugin_id`（基于文件指纹）并持久化。
- 激活时执行 ABI、导出符号、算子冲突检查；失败原因可在详情中查看。
- 运行时由调度器按 `category.name` 调用已激活的插件算子。

可参考开发样例：[C++ 算子插件 Sample](samples/cpp_operator/README.md)

## 项目结构

```
flowSQL/
├── build/output/           # 编译产物（.so、可执行文件、static/）
├── config/
│   ├── deploy-single.yaml  # 单进程部署配置（开发调试）
│   ├── deploy-multi.yaml   # 多进程部署配置（生产）
│   └── flowsql.yml         # 数据库通道持久化配置
├── src/
│   ├── common/             # 公共头文件（define.h、loader.hpp、error_code.h）
│   ├── framework/          # 框架核心（IPlugin、Pipeline、IRouterHandle 等）
│   ├── services/
│   │   ├── gateway/        # GatewayPlugin（Trie 路由转发）
│   │   ├── router/         # RouterAgencyPlugin（路由收集 + HTTP 分发）
│   │   ├── web/            # WebPlugin（静态文件 + API 代理）
│   │   ├── scheduler/      # SchedulerPlugin（SQL 执行 + 通道管理）
│   │   ├── database/       # DatabasePlugin（MySQL/SQLite/PostgreSQL/ClickHouse）
│   │   ├── catalog/        # CatalogPlugin（通道目录 + 算子目录 + /operators/*）
│   │   ├── binaddon/       # BinAddonHostPlugin（C++ 算子插件管理）
│   │   └── bridge/         # BridgePlugin（C++ ↔ Python 桥接）
│   ├── plugins/
│   │   ├── example/        # 示例插件（MemoryChannel）
│   │   └── npi/            # NPI 协议识别
│   ├── python/             # Python Worker（FastAPI）
│   ├── frontend/           # Vue.js 前端
│   └── tests/
├── docs/                   # 设计文档
├── samples/                # 开发者样例工程（如 C++ 算子插件）
└── tasks/                  # Sprint 任务管理
```

## 文档

- [项目愿景](docs/vision.md)
- [架构设计](docs/framework.md)
- [C++ 算子插件 Sample](samples/cpp_operator/README.md)

## 许可证

MIT License
