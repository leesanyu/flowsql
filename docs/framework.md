# FlowSQL 架构演进方案

## Context

最近连续 3 个 bug 暴露了桥接层的架构复杂度问题（双通道、冗余消息类型、多层继承）。借此机会重新审视整体架构，形成统一的多服务通信方案。经过模块 A/B/C 的实施，架构已从 5 服务精简为 4 服务——Scheduler 吸收了 OperatorService 的职责，统一管理算子注册、执行和 Pipeline 调度。

Gateway/Manager 服务架构已实现并验证通过。

## 核心设计原则

1. **统一框架（Python Worker 例外）**：C++ 服务都是同一个框架程序加载不同 .so 运行起来的，彼此对等；Python Worker 是独立的 FastAPI 进程
2. **IPlugin 机制保留**：这是架构的显著特点，所有功能模块继续以 .so 插件形式加载
3. **控制面 HTTP 统一**：服务间控制面通信全部走 HTTP + URI 路由
4. **数据面独立通道**：高吞吐数据传输走共享内存 / Arrow IPC，不经过 HTTP
5. **Scheduler 统一屏蔽算子实现差异**：C++ 算子在 Scheduler 进程内直接函数调用，Python 算子通过 PythonOperatorBridge 代理执行，上层 Pipeline 无需感知差异

## 整体架构

### 服务拓扑：星型，Gateway + 管理服务合一

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

### 通信方式

- **服务注册**：各服务启动后向 Gateway 注册两级 URI 前缀路由，声明自己的能力
- **服务发现**：通过 `GET /gateway/routes` 查询路由表，确认目标服务就绪
- **控制面**：服务间通过 Gateway 转发 HTTP 请求，Gateway 剥离匹配前缀后转发
- **数据面**：Python 算子执行时，数据通过共享内存 / Arrow IPC 传输（C++ 算子直接操作 IChannel，不经过共享内存）

### Gateway/Manager 职责

- 维护 URI 路由表，提供注册、注销、查询接口
- 转发 HTTP 请求到目标服务（剥离匹配前缀后转发）
- 管理各服务进程的启动、停止、故障检测与重启
- 启动时通过命令行参数将 Gateway 地址传递给各服务
- 本身也是框架 + gateway.so，与其他服务对等

### URI 路由机制

**路由注册**

- 各服务启动后主动向 Gateway 注册路由，声明自己提供的能力
- 路由条目为两级 URI 前缀 → 服务地址的映射（级数可配置，默认 2 级）
- 同一个服务注册多条路由，每条对应一个具体能力
- 不允许重复注册：如果某个前缀已被其他服务注册，注册请求被拒绝

**路由匹配与转发**

- Gateway 收到请求后，取 URI 的前 N 级（默认 2 级）做精确匹配
- 匹配成功后，剥离前缀，将剩余路径转发给目标服务
- 示例：请求 `/pyworker/work/explore/chisquare`
  - 取前两级 `/pyworker/work` 查路由表 → 匹配到 Python Worker 地址
  - 剥离前缀，转发 `/explore/chisquare` 给 Python Worker

**路由查询**

- Gateway 提供 `GET /gateway/routes` 接口，返回当前所有已注册的路由条目
- 服务间存在依赖时，通过查询路由表确认目标服务就绪后再发起调用
- 示例：Scheduler 启动后轮询 `GET /gateway/routes`，确认 `/pyworker/operators` 已注册，再发 `GET /pyworker/operators`

**Gateway 自身接口（不经过路由匹配）**

| 接口 | 方法 | 说明 |
|------|------|------|
| `/gateway/register` | POST | 注册路由，body: `{ "prefix": "/pyworker/work", "address": "127.0.0.1:18900", "service": "pyworker" }` |
| `/gateway/unregister` | POST | 注销路由，body: `{ "prefix": "/pyworker/work" }` |
| `/gateway/routes` | GET | 查询所有已注册路由 |
| `/gateway/heartbeat` | POST | 服务心跳上报，body: `{ "service": "scheduler" }` |

**路由注册示例（实际实现）**

Web 服务启动后注册：
```
POST /gateway/register  { "prefix": "/web", "address": "127.0.0.1:18802", "service": "web" }
```

Scheduler 启动后注册：
```
POST /gateway/register  { "prefix": "/scheduler", "address": "127.0.0.1:18803", "service": "scheduler" }
```

Python Worker 启动后注册（每个端点一条路由）：
```
POST /gateway/register  { "prefix": "/pyworker/health",    "address": "127.0.0.1:18900", "service": "pyworker" }
POST /gateway/register  { "prefix": "/pyworker/operators",  "address": "127.0.0.1:18900", "service": "pyworker" }
POST /gateway/register  { "prefix": "/pyworker/work",       "address": "127.0.0.1:18900", "service": "pyworker" }
POST /gateway/register  { "prefix": "/pyworker/reload",     "address": "127.0.0.1:18900", "service": "pyworker" }
POST /gateway/register  { "prefix": "/pyworker/configure",  "address": "127.0.0.1:18900", "service": "pyworker" }
```

路由匹配优先取前 2 级精确匹配，未命中则回退到前 1 级。例如 `/web/api/health` 先查 `/web/api`（未注册），再查 `/web`（命中），剥离前缀后转发 `/api/health` 给 Web 服务。

### 各服务角色

| 服务 | 实现方式 | 注册路由 | 职责 |
|------|---------|---------|------|
| Gateway/Manager | 框架 + gateway.so | `/gateway/*`（内置） | 路由转发、服务注册/发现、心跳监控、生命周期管理 |
| Web 服务 | 框架 + web.so | `/web/...` | 前端交互、用户 API，收到 SQL 转发给 Scheduler |
| Scheduler | 框架 + scheduler.so + 算子插件 | `/scheduler/...` | SQL 解析、Pipeline 构建与执行、通道管理、算子注册/管理/执行、任务管理 |
| Python Worker | FastAPI 进程 | `/pyworker/...` | Python 算子的实际执行环境 |

### Pipeline 定义

Pipeline 是 Scheduler 进程内的运行器，负责串联算子执行：

- **接口**：`Pipeline::Run(operator, source_channel, sink_channel)`
- **C++ 算子**：Pipeline 直接调用 `operator->Work(source, sink)`，纯进程内函数调用，无跨进程开销
- **Python 算子**：Pipeline 调用的是 `PythonOperatorBridge->Work(source, sink)`，Bridge 内部完成：
  1. 从 source IChannel 读取数据
  2. Arrow IPC memcpy 写入共享内存 `flowsql_<uuid>_in`
  3. HTTP 控制指令发给 Python Worker：`POST /pyworker/work/<category>/<name> { "input": "路径" }`
  4. Python Worker 零拷贝读取 → 执行 → 写入 `flowsql_<uuid>_out`
  5. Bridge 从共享内存读取结果，写入 sink IChannel

Pipeline 不感知算子是 C++ 还是 Python，统一面向 IOperator 接口。差异由 PythonOperatorBridge 在内部屏蔽。

### 数据面设计

**设计目标**

C++ 算子直接操作 IChannel，零开销。Python 算子执行时通过共享内存实现跨进程零拷贝，HTTP 只传控制信息。

**两条数据路径**

```
C++ 算子路径（进程内，零开销）：
  Pipeline → operator->Work(source_channel, sink_channel)
  算子直接从 source IChannel 读取数据，处理后写入 sink IChannel
  无共享内存、无 memcpy、无 HTTP

Python 算子路径（跨进程，共享内存零拷贝）：
  Pipeline → PythonOperatorBridge->Work(source, sink)
    → 从 source IChannel 读取 Arrow RecordBatch
    → Arrow IPC memcpy 写入 /dev/shm/flowsql_<uuid>_in
    → HTTP 控制指令（仅传文件路径，几十字节）
    → Python Worker: pa.memory_map() → Arrow Table（零拷贝）
    → Arrow Table → Polars DataFrame（零拷贝）
    → 算子 operator.work(df_in) → df_out
    → Polars DataFrame → Arrow Table（零拷贝）
    → Arrow IPC memcpy 写入 flowsql_<uuid>_out
    → HTTP 响应返回输出路径
    → Bridge: memory_map 读取 → Arrow RecordBatch → 写入 sink IChannel
```

**关于 Arrow IPC "序列化"**

Arrow IPC 写入共享内存的过程本质是 memcpy（内存布局直接拷贝），不涉及编解码。读取端通过 memory_map 直接映射，实现零拷贝访问。这与 JSON/Protobuf 等需要编解码的序列化有本质区别。

**传输策略：共享内存 + 磁盘回退**

PythonOperatorBridge 根据数据量自动选择传输方式：
- 数据量 ≤ 阈值（可配置，默认如 256MB）：写入 `/dev/shm/flowsql_<uuid>_in`（共享内存，零拷贝）
- 数据量 > 阈值：写入 `/tmp/flowsql_<uuid>_in`（磁盘 Arrow IPC 文件）

Python Worker 侧不需要区分——都是通过 `pa.memory_map()` 打开路径读取，逻辑一致。区别只在于底层是内存映射还是磁盘 IO。

控制指令中传的是完整路径，Python Worker 按路径读取即可，无需感知传输策略。

**控制指令格式**

请求：
```
POST /pyworker/work/explore/chisquare
{ "input": "/dev/shm/flowsql_<uuid>_in" }
```

成功响应：
```
200 OK
{ "output": "/dev/shm/flowsql_<uuid>_out" }
```

失败响应（算子执行出错，不生成 `_out` 文件）：
```
500
{ "detail": "错误信息" }
```

**共享内存生命周期**

- 所有权归 PythonOperatorBridge（调用方）：它创建 `_in`，读取 `_out`，最终清理两者
- Python Worker 无状态：只读 `_in`，写 `_out`，不负责清理
- 命名规则：`/dev/shm/flowsql_<uuid>_in`、`/dev/shm/flowsql_<uuid>_out`，uuid 保证并发安全

**泄露防护（RAII + 启动清理）**

- RAII：PythonOperatorBridge 内部用 SharedMemoryGuard 对象持有文件路径，析构时自动 unlink，覆盖正常流程和 C++ 异常退出（栈展开）
- 启动清理：Scheduler 启动时扫描 `/dev/shm/flowsql_*`，清理上次残留文件，兜底 SIGKILL 等无法析构的场景

### Python 算子完整流程

**启动阶段：**
1. Gateway/Manager 启动，监听端口
2. Gateway/Manager spawn 各服务进程（包括 Python Worker），带上 Gateway 地址参数
3. 各服务启动后向 Gateway 注册路由
4. Scheduler 轮询 `GET /gateway/routes`，确认 `/pyworker/operators` 已注册
5. Scheduler 通过 Gateway 调用 `GET /pyworker/operators` 获取 Python 算子列表
6. Scheduler 将 Python 算子注册到 PluginRegistry（类型标记为 Python，关联 PythonOperatorBridge）

**执行阶段：**
1. 用户通过 Web 服务提交 SQL
2. Web 服务转发给 Scheduler：`POST /scheduler/execute { "sql": "..." }`
3. Scheduler 解析 SQL，构建 Pipeline（确定算子、source 通道、sink 通道）
4. Pipeline 调用 `operator->Work(source, sink)`：
   - 若为 C++ 算子：直接函数调用，算子操作 IChannel 完成计算
   - 若为 Python 算子（实际调用 PythonOperatorBridge）：
     a. Bridge 从 source IChannel 读取 Arrow RecordBatch
     b. Arrow IPC memcpy 写入共享内存 `flowsql_<uuid>_in`
     c. Bridge 发送 HTTP 控制指令：`POST /pyworker/work/<category>/<name> { "input": "路径" }`
     d. Python Worker 从共享内存零拷贝读取 → Polars DataFrame → 算子处理 → 结果写入 `flowsql_<uuid>_out`
     e. Bridge 从共享内存读取结果 → 写入 sink IChannel
     f. SharedMemoryGuard 析构，清理共享内存文件
5. Scheduler 返回执行结果给 Web 服务

**Reload 阶段：**
1. 用户在 Web 界面触发 reload
2. Web 服务 → Gateway → Scheduler：`POST /scheduler/reload`
3. Scheduler → Gateway → Python Worker：`POST /pyworker/reload`
4. Python Worker 重新扫描算子目录，返回变更列表
5. Scheduler 更新 PluginRegistry 中的 Python 算子元数据（新增/移除）

### 多服务部署与编排

**框架程序定位**

框架程序是通用的插件容器，本身不包含业务逻辑。启动时通过配置决定加载哪些 .so，一个服务进程可以加载多个插件（且是常见情况，单个插件应聚焦、尽可能小）。

**部署配置（config/gateway.yaml）**

```yaml
gateway:
  host: 127.0.0.1
  port: 18800
  heartbeat_interval_s: 5     # 心跳周期
  heartbeat_timeout_count: 3  # 超时判定次数

services:
  - name: web
    plugins:
      - libflowsql_web.so
    port: 8081

  - name: scheduler
    plugins:
      - libflowsql_scheduler.so
      - libflowsql_bridge.so
      - libflowsql_example.so
    port: 18803

  - name: pyworker
    type: python
    command: "python3 -m flowsql.worker"
    port: 18900
```

**启动编排（已实现）**

```
flowsql --config config/gateway.yaml
```

1. Gateway 进程启动，加载 `libflowsql_gateway.so`，Option 传入配置文件路径
2. GatewayPlugin 解析 YAML 配置，启动 HTTP 服务线程（监听 18800）
3. ServiceManager 按配置 `posix_spawn` 各子服务：
   - C++ 服务：`flowsql --role web --gateway 127.0.0.1:18800 --port 8081 --plugins libflowsql_web.so`
   - Python 服务：`python3 -m flowsql.worker --port 18900`（环境变量 `FLOWSQL_GATEWAY_ADDR=127.0.0.1:18800`）
4. 各子服务启动后：
   - C++ 服务：加载插件 → ServiceClient 向 Gateway 注册路由 → 启动心跳线程
   - Python Worker：发现算子 → 向 Gateway 注册路由 → 启动心跳线程
5. Scheduler（BridgePlugin）通过 `GET /gateway/routes` 发现 PyWorker 地址，再 `GET /pyworker/operators` 获取算子列表并注册到 PluginRegistry
6. Gateway 启动心跳检测线程，定期检查各服务心跳超时

**心跳机制**

- 各服务定期向 Gateway 发送心跳：`POST /gateway/heartbeat { "service": "scheduler" }`
- Gateway 维护每个服务的最后心跳时间
- 超过 N 个心跳周期未收到（默认 3 × 5s = 15s），判定服务异常
- 服务异常时 Gateway/Manager 自动重启该服务进程
- 心跳周期和超时次数均可通过配置文件设置

**停止编排（已实现）**

Gateway 收到 SIGINT/SIGTERM 后：
1. ServiceManager 逐个停止子服务：SIGTERM → 等待 2s → SIGKILL
2. 各子服务收到 SIGTERM 后：停止心跳 → StopModules（逆序停止插件）→ UnloadAll → 退出
3. Gateway HTTP 服务停止
4. Gateway 进程退出

## 流式架构（设计阶段）

以 `SELECT * FROM netcard USING npm INTO ts.db1` 为驱动场景，设计流式处理能力。

### Channel 的本质：描述符，不是数据管道

Channel 是框架层面的命名、受管理的数据源/汇描述符。框架只看到 IChannel（统一管理），算子通过 dynamic_cast 获取具体类型后操作原生数据管道。

```
Channel = 描述符
  ├── 身份（catelog, name）→ SQL 中 FROM/INTO 引用
  ├── 元数据（type, schema）→ 框架做类型检查和展示
  ├── 生命周期（open, close）→ 框架管理资源
  └── 数据管道的入口 → 算子通过 dynamic_cast 获取
       ├── DataFrameChannel: 入口就是自身（Read/Write）
       ├── DatabaseChannel: 入口是工厂方法（CreateReader/Writer）
       └── NetcardChannel: 入口是 GetRing()（返回 rte_ring*）
```

### 流式通道与流式算子

与批处理对称，新增流式层：

```
                    进程内                  跨进程
批处理通道      DataFrameChannel          —
批处理算子      C++ 算子（同步 Work）      PythonOperatorBridge
流式通道        进程内 IStreamChannel      netcard（独立进程，DPDK）
流式算子        进程内 IStreamOperator     npm（独立进程）/ StreamWorker（通用容器）
```

### 通道接口体系

```
IChannel（基类：生命周期 + 身份 + 元数据）
  ├── IDataFrameChannel（批处理 + DataFrame：快照 Read / 替换 Write）
  ├── IDatabaseChannel（批处理 + 数据库：Reader/Writer 工厂）
  └── IStreamChannel（流式行为基类，不绑定数据格式）
       ├── NetcardChannel（流式 + packet/rte_mbuf）
       ├── DataFrameStreamChannel（流式 + DataFrame）
       └── ...
```

IStreamChannel 不限定 IDataFrame 作为数据格式。数据格式由具体子类型决定，算子通过 dynamic_cast 获取所需类型——与批处理算子 cast 到 IDataFrameChannel 是同一模式。

### 部署模式：是否独立进程取决于复杂度

- 复杂流式算子（如 npm 网络性能分析）→ 独立进程部署
- 简单流式算子 → 放在通用 StreamWorker 里（定位类似 Python Worker：通用流式算子执行容器）

StreamWorker 是框架程序加载流式算子 .so 的实例，与其他 C++ 服务对等，符合"统一框架"原则。

### Scheduler 角色随任务类型变化

| 任务类型 | Scheduler 角色 | 数据路径 |
|---------|---------------|---------|
| 批处理 | 执行者（Pipeline 驱动） | 经过 Scheduler |
| 流式进程内 | 宿主（算子自管理线程） | 在 Scheduler 进程内 |
| 流式跨进程 | 编排者（HTTP 控制指令） | 不经过 Scheduler |

批处理时 Scheduler 在数据路径上（Pipeline 驱动 Work 调用）。流式跨进程时 Scheduler 只做控制面编排，数据直接在服务间流动（如 netcard → ring buffer → npm → ClickHouse）。

### DPDK 大页内存零拷贝

netcard 场景采用 DPDK Primary/Secondary 进程模型实现零拷贝：

- netcard 服务作为 DPDK Primary 进程，初始化大页内存，创建 rte_mempool 和 rte_ring
- npm/StreamWorker 作为 DPDK Secondary 进程，attach 到同一块大页内存
- NIC 通过 DMA 直接将数据包写入大页内存的 rte_mbuf
- rte_ring 传递 mbuf 指针（8 字节），不拷贝数据包本身
- 消费者（npm）通过指针直接访问大页内存中的数据包

整条链路零拷贝：NIC DMA → 大页内存 mbuf → ring 传指针 → 消费者原地读取。

### 流式任务执行流程

以 `SELECT * FROM netcard USING npm INTO ts.db1` 为例（netcard 和 npm 均为独立服务进程）：

```
1. SqlParser 解析 → source=netcard, op=npm, dest=ts.db1
2. Scheduler 查找组件：
   - netcard: Gateway 路由表 /netcard/* → 跨进程流式服务
   - npm: Gateway 路由表 /npm/* → 跨进程流式服务
   - ts.db1: 数据库连接配置
3. 判断为跨进程流式任务 → 创建 DistributedStreamingTask
4. Scheduler 编排：
   GET /netcard/ring_buffers → { "eth0": { "path": "/dev/shm/...", "schema": "..." } }
   POST /npm/start {
     "task_id": 42,
     "source": { "type": "ring_buffer", "path": "/dev/shm/flowsql_netcard_eth0" },
     "sink": { "type": "clickhouse", "database": "ts", "table": "db1" }
   }
5. 立即返回 { "task_id": 42, "status": "running", "type": "streaming" }
6. 数据面持续运行（Scheduler 不参与）：
   netcard → ring buffer → npm → ClickHouse
7. 控制面监控：Scheduler 定期 GET /npm/status/42
8. 停止：POST /npm/stop/42 → Flush → 任务结束
```

npm 算子内部的 Work 实现：

```cpp
int NpmOperator::Work(IChannel* in, IChannel* out) {
    // 从描述符获取实际数据管道
    auto* nc = dynamic_cast<NetcardChannel*>(in);
    auto* ring = nc->GetRing();                    // rte_ring*

    auto* db = dynamic_cast<IDatabaseChannel*>(out);
    db->CreateWriter("npm_results", &writer);      // IBatchWriter*

    // 启动工作线程，异步返回
    running_ = true;
    worker_ = std::thread([this, ring, writer] {
        while (running_) {
            rte_ring_dequeue_burst(ring, mbufs, ...);
            // 网络性能分析 → 结果写入数据库
        }
        writer->Flush();
        writer->Close(nullptr);
        writer->Release();
    });
    return 0;
}
```

### 流式服务的控制协议

Scheduler 通过 HTTP 对流式服务做 start/stop/status 编排：

```
启动：POST /npm/start { "task_id": 42, "source": {...}, "sink": {...} }
监控：GET /npm/status/42 → { "running": true, "batches": 5678, "rows": 1234567 }
停止：POST /npm/stop/42
```

netcard 服务提供 ring buffer 查询：

```
GET /netcard/ring_buffers → { "eth0": { "path": "/dev/shm/...", "schema": "..." } }
```

### 待深入设计

以下事项已识别但尚未展开设计：

- IStreamChannel / IStreamOperator 的具体接口方法
- ITask 统一任务抽象（BatchTask / InProcessStreamingTask / DistributedStreamingTask）
- StreamWorker 控制协议
- 服务发现与任务类型自动判断（PluginRegistry vs Gateway 路由表）
- ring buffer 多消费者策略、满时策略、背压机制
- 流式任务的错误恢复与生命周期管理
- 部署配置中流式服务的声明方式

## 当前实现状态

### 已实现

- **Gateway/Manager 服务**：`libflowsql_gateway.so`（配置解析、路由表、服务管理、HTTP 转发、心跳检测）
- **通用入口**：`flowsql` 可执行文件（Gateway 模式 `--config` / Service 模式 `--role`）
- **Web 插件化**：`libflowsql_web.so`（WebServer 包装为 IPlugin）
- **ServiceClient**：子服务向 Gateway 注册路由 + 心跳
- **BridgePlugin 适配**：移除 PythonProcessManager/ControlServer，改为通过 Gateway 路由发现 PyWorker
- **Python Worker 适配**：移除 ControlClient，改为向 Gateway 注册路由 + 心跳
- **PluginRegistry 增强**：LoadPlugin 支持 option 参数传递
- **IModule 合并到 IPlugin**：IPlugin 新增默认空 Start()/Stop()，删除 IModule/IID_MODULE，简化插件接口

### 项目目录结构

```
flowSQL/
├── thirdparts/                 # 第三方依赖构建配置（非源码）
│   ├── arrow/
│   ├── gflags/
│   ├── glog/
│   ├── httplib/
│   ├── hyperscan/
│   ├── rapidjson/
│   ├── sqlite/
│   └── yaml-cpp/
│
├── build/                      # cmake 构建产物（可随时 rm -rf）
│   └── output/                 # 编译产物（.so、可执行文件）
│
├── .thirdparts_installed/      # 第三方依赖安装缓存（独立于 build）
├── .thirdparts_prefix/         # 第三方依赖编译缓存（独立于 build）
│
├── src/
│   ├── CMakeLists.txt          # project(flowsql)
│   ├── subjects.cmake          # 构建宏定义
│   ├── .clang-format
│   │
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
│   │   ├── example/            # 示例插件（libflowsql_example.so）
│   │   └── npi/                # NPI 协议解析插件（libflowsql_npi.so）
│   │
│   ├── python/                 # Python Worker（FastAPI + 算子运行时）
│   ├── frontend/               # Vue.js 前端项目
│   │
│   └── tests/
│       ├── test_framework/
│       ├── test_bridge/
│       ├── test_npi/
│       └── data/               # 测试数据（NPI pcap 文件等）
│           └── packets/
│
├── config/                     # 运行配置（gateway.yaml）
├── docs/                       # 文档
└── ...
```

### 构建与运行

```bash
# 构建（从项目根目录）
cmake -B build src && cmake --build build -j$(nproc)

# 运行
cd build/output
LD_LIBRARY_PATH=. ./flowsql --config ../../config/gateway.yaml

# 测试
./test_framework
./test_bridge
```

### 验证结果

1. `flowsql --config config/gateway.yaml` 启动成功
2. Gateway(18800) 自动 spawn Web(8081) + Scheduler(18803) + PyWorker(18900)
3. `GET /gateway/routes` 返回 7 条路由
4. `GET /web/api/health` 通过 Gateway 转发成功
5. `GET /pyworker/operators` 通过 Gateway 转发返回 4 个 Python 算子
6. BridgePlugin 通过 Gateway 路由发现 PyWorker 并注册算子
7. Ctrl+C 优雅停止所有子服务