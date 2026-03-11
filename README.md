# FlowSQL

基于 SQL 语法的网络流量分析共创平台

## 项目简介

FlowSQL 是一个全栈式网络流量分析平台，通过扩展的 SQL 语法提供从数据采集、流量分析到数据探索的完整能力。用户无需深入了解底层技术（如 DPDK、Hyperscan），只需使用熟悉的 SQL 语句即可构建自己的流量分析系统。

平台采用 Gateway + 多服务插件架构，C++ 服务共享同一个框架程序（加载不同 .so），Python Worker 作为独立 FastAPI 进程运行。控制面统一走 HTTP + URI 路由，数据面通过共享内存 / Arrow IPC 实现零拷贝传输。

## 核心特性

- **插件化架构**：所有功能模块以 .so 插件形式加载，统一框架程序驱动
- **Gateway 路由**：星型服务拓扑，URI 路由注册/发现/转发，心跳监控与自动重启
- **C++ ↔ Python 桥接**：共享内存 + Arrow IPC 零拷贝数据传输，HTTP 仅传控制指令
- **SQL 驱动**：扩展 SQL 语法统一数据采集、分析、探索操作
- **Web 管理**：Vue.js 前端 + REST API，支持通道/算子/任务管理和在线编写 Python 算子
- **流式处理（设计中）**：DPDK 大页内存零拷贝、IStreamChannel/IStreamOperator 接口

## 快速开始

### 开发依赖服务

使用 Docker Compose 启动 MySQL 和 ClickHouse（配置文件在 `config/` 目录）。

**MySQL**

```bash
# 启动
docker compose -f config/docker-compose-mysql.yml up -d

# 停止
docker compose -f config/docker-compose-mysql.yml down
```

连接参数：`127.0.0.1:3306`，用户 `flowsql_user`，密码 `flowSQL@user`，库 `flowsql_db`

**ClickHouse**

```bash
# 启动
docker compose -f config/docker-compose-clickhouse.yml up -d

# 停止
docker compose -f config/docker-compose-clickhouse.yml down

# 验证
curl http://localhost:8123/ping
```

连接参数：HTTP `127.0.0.1:8123`，TCP `127.0.0.1:9000`，用户 `flowsql_user`，密码 `flowSQL@user`，库 `flowsql_db`

**同时启动两个服务**

```bash
docker compose -f config/docker-compose-mysql.yml \
               -f config/docker-compose-clickhouse.yml up -d
```

### 环境要求

- CMake 3.12+
- C++17 编译器（GCC 7+ / Clang 5+）
- Linux 系统
- Python 3.8+（Python 算子运行时）

### 编译

```bash
cmake -B build src && cmake --build build -j$(nproc)
```

### 运行

```bash
cd build/output
LD_LIBRARY_PATH=. ./flowsql --config ../../config/gateway.yaml
```

启动后 Gateway(18800) 自动 spawn Web(8081) + Scheduler(18803) + PyWorker(18900)，浏览器访问 `http://127.0.0.1:8081` 进入管理界面。

### 测试

```bash
cd build/output
./test_framework
./test_bridge
```

## 架构

### 服务拓扑

```
                    ┌─────────────────────┐
                    │   Gateway/Manager   │
                    │ (框架 + gateway.so)  │
                    │  - 路由表（URI→地址） │
                    │  - 服务注册/发现     │
                    │  - 生命周期管理       │
                    └──────┬──────────────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
     ┌────────▼───┐  ┌────▼─────┐  ┌───▼──────────┐
     │ Web 服务   │  │Scheduler │  │ Python Worker │
     │(框架+web.so)│  │(框架+    │  │  (FastAPI)    │
     │ 前端+API   │  │sched.so+ │  │  算子执行     │
     └────────────┘  │算子插件) │  └───────────────┘
                     │SQL+执行  │
                     └──────────┘
```

### 核心接口

```
IPlugin（生命周期 + 启停控制）
├── IChannel（数据通道描述符）
│   ├── IDataFrameChannel（批处理：快照 Read / 替换 Write）
│   ├── IDatabaseChannel（数据库：Reader/Writer 工厂）
│   └── IStreamChannel（流式，设计中）
├── IOperator（数据算子：Work(IChannel*, IChannel*)）
└── ...（可扩展）
```

- **IPlugin**（`common/loader.hpp`）— 所有可加载组件的根接口，三阶段加载：`pluginregist()` 注册 → `Load()` 初始化 → `Start()` 启动
- **IChannel**（`framework/interfaces/ichannel.h`）— 数据源/汇描述符，`Catelog()` + `Name()` 构成唯一标识，算子通过 `dynamic_cast` 获取具体子类型操作数据
- **IOperator**（`framework/interfaces/ioperator.h`）— 数据处理单元，`Work(IChannel* in, IChannel* out)` 核心计算方法
- **IDataFrame**（`framework/interfaces/idataframe.h`）— 列式数据集，Apache Arrow RecordBatch 后端

### 数据面

```
C++ 算子路径（进程内，零开销）：
  Pipeline → operator->Work(source_channel, sink_channel)

Python 算子路径（跨进程，共享内存零拷贝）：
  Pipeline → PythonOperatorBridge->Work(source, sink)
    → Arrow IPC memcpy 写入 /dev/shm/flowsql_<uuid>_in
    → HTTP 控制指令（仅传文件路径）
    → Python Worker: memory_map → Polars DataFrame → 算子处理
    → 结果写入 /dev/shm/flowsql_<uuid>_out
    → Bridge 读取结果 → 写入 sink IChannel
```

## SQL 语法示例

```sql
-- 数据采集
SELECT * FROM netcard USING npm INTO ts.db1

-- 探索性分析（Python 算子）
SELECT * FROM example.memory USING explore.chisquare WITH target='label'

-- 统计分析
SELECT bps FROM ts.npm.tcp_session USING statistic.hist
WHERE time = '[2024/07/14 00:00:00 - 2024/07/14 23:59:59]'
```

## 项目结构

```
flowSQL/
├── thirdparts/                 # 第三方依赖构建配置（非源码）
├── build/                      # cmake 构建产物（可随时 rm -rf）
│   └── output/                 # 编译产物（.so、可执行文件）
├── .thirdparts_installed/      # 第三方依赖安装缓存（独立于 build）
├── .thirdparts_prefix/         # 第三方依赖编译缓存（独立于 build）
│
├── src/
│   ├── common/                 # 公共头文件（define.h、loader.hpp 等）
│   ├── framework/              # 框架核心（IPlugin、PluginRegistry、Pipeline 等）
│   │
│   ├── services/               # 服务插件
│   │   ├── bridge/             # C++ ↔ Python 桥接（libflowsql_bridge.so）
│   │   ├── scheduler/          # 调度服务（libflowsql_scheduler.so）
│   │   ├── gateway/            # 网关服务（libflowsql_gateway.so）
│   │   └── web/                # Web 管理系统（libflowsql_web.so + flowsql 可执行文件）
│   │
│   ├── plugins/
│   │   ├── example/            # 示例插件（MemoryChannel + PassthroughOperator）
│   │   └── npi/                # NPI 协议识别插件（Hyperscan 正则 + 位图 + 枚举匹配）
│   │
│   ├── python/                 # Python Worker（FastAPI + 算子运行时）
│   ├── frontend/               # Vue.js 前端项目
│   │
│   └── tests/
│       ├── test_framework/
│       ├── test_bridge/
│       ├── test_npi/
│       └── data/               # 测试数据（NPI pcap 文件等）
│
├── config/                     # 运行配置（gateway.yaml）
└── docs/                       # 设计文档
```

## 文档

- [架构演进方案](docs/framework.md) — 整体架构设计与实现状态
- [Stage 1 设计文档](docs/stage1.md) — C++ 框架核心
- [Stage 2 设计文档](docs/stage2.md) — C++ ↔ Python 桥接 + Web 管理系统
- [Stage 3 设计文档](docs/stage3.md) — 数据库闭环 + 流式架构 + 平台增强

## 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件

## 贡献

欢迎提交 Issue 和 Pull Request

## 联系方式

项目地址：https://github.com/lealiang/flowsql
