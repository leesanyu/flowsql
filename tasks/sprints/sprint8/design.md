# 架构设计：CatalogPlugin — 内置通道与算子注册中心

> 状态：讨论中
> 创建：2026-03-19
> 参与者：用户 + Claude

---

## 1. 问题陈述

当前架构存在两个结构性问题：

### 1.1 plugins/example 和 plugins/testdata 职责错位

`plugins/example`（MemoryChannel + PassthroughOperator）和 `plugins/testdata`（DataFrame 通道）以 IPlugin 形式存在，但这两类对象根本不需要插件生命周期管理：

- `MemoryChannel` 是一个内存队列，是纯数据结构
- `PassthroughOperator` 是一个无状态的执行类型
- `DataFrameChannel` 已经是 `src/framework/core/` 中的公共代码

让它们继承 IPlugin 是过度设计，增加了不必要的 .so 依赖和加载开销。

### 1.2 DataFrame 通道和内置算子缺少注册/发现主体

- `DatabasePlugin` 管理数据库通道（CRUD、持久化、懒加载）
- `BridgePlugin` 管理 Python 算子（跨进程发现、注册）
- **DataFrame 通道**：运行时大量产生，匿名通道在 Pipeline 内部流转，但具名通道（需要跨 Pipeline 共享）没有注册中心
- **内置 C++ 算子**（Passthrough 等）：没有类型注册表，无法按名创建

---

## 2. 设计目标

1. 删除 `plugins/example` 和 `plugins/testdata`，将 MemoryChannel / PassthroughOperator 移入 `framework/core/`
2. 引入 `CatalogPlugin`（加载在 scheduler 进程），提供两个接口：
   - `IChannelRegistry`：具名 DataFrame 通道的注册与发现
   - `IOperatorRegistry`：内置算子类型的注册与按名创建
3. 不破坏现有 IChannel / IOperator 查找链路（IQuerier::Traverse）

---

## 3. 通道命名规范

### 3.1 现有规范（数据库通道）

数据库通道采用两段式命名：

```
<type>.<instance>
```

示例：`sqlite.mydb`、`mysql.prod`

表名在 SQL 中显式指定，不属于通道名：

```sql
select * from mysql.prod.test_user ...
                         ^^^^^^^^^ 表名，不是通道名的一部分
```

### 3.2 DataFrame 通道命名

对齐现有规范，采用两段式：

```
dataframe.<name>
```

示例：`dataframe.test_user_processed`、`dataframe.daily_report`

在 SQL 中的用法：

```sql
-- 写入具名 DataFrame 通道
select * from mysql.prod.test_user using operator_xxx into dataframe.test_user_processed

-- 从具名 DataFrame 通道读取
select * from dataframe.test_user_processed into clickhouse.dw.result_table
```

`dataframe` 对应 `IChannel::Catelog()`，`<name>` 对应 `IChannel::Name()`，与现有接口天然对应，无需特殊处理。

### 3.3 匿名 vs 具名

| 类型 | 创建方式 | 生命周期 | 持久化 |
|------|---------|---------|--------|
| 匿名 DataFrame 通道 | `make_shared<DataFrameChannel>()` | Pipeline 内部，随 Pipeline 销毁 | 否 |
| 具名 DataFrame 通道 | 通过 `IChannelRegistry::Register` | 显式注销前一直存在，重启自动恢复 | 是（序列化到 `data_dir_`） |

**统一规则：具名 = 持久化，无论来源（SQL 写入或 CSV 导入）。**

`Register` 内部自动将通道数据序列化为 `data_dir_/<name>.csv`；`Unregister` 删除磁盘文件；`Rename` 同步重命名磁盘文件。调用方不感知磁盘操作。

---

## 4. 接口设计

### 4.1 IChannelRegistry

```cpp
// {新 GUID}
const Guid IID_CHANNEL_REGISTRY = { ... };

// IChannelRegistry — 具名 DataFrame 通道注册中心
// 具名即持久化：Register 自动落盘，Unregister 自动删文件，Rename 自动重命名文件
// Registry 通过 shared_ptr 共享通道所有权，避免悬空指针
interface IChannelRegistry {
    // 注册具名通道并持久化（name 不含 "dataframe." 前缀）
    // 同名已存在时返回 -1（CONFLICT），调用方若要覆盖，先 Unregister 再 Register
    virtual int Register(const char* name,
                         std::shared_ptr<IDataFrameChannel> channel) = 0;

    // 按名查找（未注册返回 nullptr）
    virtual std::shared_ptr<IDataFrameChannel> Get(const char* name) = 0;

    // 注销并删除持久化文件（未注册时返回 -1）
    virtual int Unregister(const char* name) = 0;

    // 枚举所有已注册的具名通道
    virtual void List(std::function<void(const char* name,
                                         std::shared_ptr<IDataFrameChannel>)> callback) = 0;

    // 重命名（原子操作：更新 Registry + 重命名磁盘文件）
    // new_name 已存在时返回 -1
    virtual int Rename(const char* old_name, const char* new_name) = 0;
};
```

**所有权说明**：Registry 通过 `shared_ptr` 共享通道所有权。`Unregister` 时 Registry 释放引用并删除磁盘文件；若 Pipeline 仍持有 `shared_ptr`，通道内存在 Pipeline 结束后自动销毁，不会出现悬空指针。

### 4.2 IOperatorRegistry

```cpp
// {新 GUID}
const Guid IID_OPERATOR_REGISTRY = { ... };

// 算子工厂函数类型：无参构造，调用方持有所有权（负责 delete）
using OperatorFactory = std::function<IOperator*()>;

// IOperatorRegistry — 内置算子类型注册中心
interface IOperatorRegistry {
    // 注册算子类型（name: "passthrough"、"filter" 等）
    // 同名已存在时覆盖，返回 0 成功
    virtual int Register(const char* name, OperatorFactory factory) = 0;

    // 按名创建算子实例（未注册返回 nullptr，调用方负责 delete）
    virtual IOperator* Create(const char* name) = 0;

    // 枚举所有已注册的算子类型名
    virtual void List(std::function<void(const char* name)> callback) = 0;
};
```

**设计说明**：算子是执行类型，不是数据实体。注册的是工厂函数，每次 `Create` 返回新实例。这与 `IChannelRegistry` 管理实例的语义不同。

---

## 5. CatalogPlugin 实现

### 5.1 文件结构

```
src/services/catalog/
├── CMakeLists.txt
├── catalog_plugin.h
├── catalog_plugin.cpp
└── plugin_register.cpp
```

### 5.2 类定义

```cpp
class CatalogPlugin : public IPlugin,
                      public IChannelRegistry,
                      public IOperatorRegistry,
                      public IRouterHandle {
public:
    // IPlugin 生命周期
    int Option(const char* key, const char* value) override;
    int Load(IQuerier* querier) override;
    int Start() override;
    int Stop() override;

    // IChannelRegistry
    int Register(const char* name, std::shared_ptr<IDataFrameChannel> channel) override;
    std::shared_ptr<IDataFrameChannel> Get(const char* name) override;
    int Unregister(const char* name) override;
    int Rename(const char* old_name, const char* new_name) override;
    void List(std::function<void(const char*, std::shared_ptr<IDataFrameChannel>)> cb) override;

    // IOperatorRegistry
    int Register(const char* name, OperatorFactory factory) override;
    IOperator* Create(const char* name) override;
    void List(std::function<void(const char*)> cb) override;

    // IRouterHandle：暴露 /channels/dataframe/* 管理端点
    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;

private:
    std::unordered_map<std::string, std::shared_ptr<IDataFrameChannel>> df_channels_;
    std::unordered_map<std::string, OperatorFactory> op_factories_;
    std::string data_dir_;   // CSV 持久化目录，Option() 配置
    mutable std::mutex mu_;
};
```

### 5.3 Load() 阶段

```cpp
int CatalogPlugin::Load(IQuerier* querier) {
    // 注册内置算子类型
    Register("passthrough", []() -> IOperator* {
        return new PassthroughOperator();
    });
    // 未来：filter、transform 等
    return 0;
}
```

### 5.3.1 Option() 阶段

```cpp
int CatalogPlugin::Option(const char* key, const char* value) {
    if (strcmp(key, "data_dir") == 0) {
        data_dir_ = value;  // 默认：./dataframes
    }
    return 0;
}
```

配置示例（deploy-single.yaml / deploy-multi.yaml scheduler 进程）：

```yaml
- name: libflowsql_catalog.so
  option: "data_dir=./dataframes"
```

WebPlugin 同步新增 `upload_dir` 配置项：

```yaml
- name: libflowsql_web.so
  option: "host=127.0.0.1;port=8081;gateway=127.0.0.1:18800;upload_dir=./uploads"
```

### 5.3.2 Start() 阶段 — 启动恢复

```cpp
int CatalogPlugin::Start() {
    // 确保目录存在
    fs::create_directories(data_dir_);

    // 扫描目录，恢复所有具名通道
    for (auto& entry : fs::directory_iterator(data_dir_)) {
        if (entry.path().extension() == ".csv") {
            std::string name = entry.path().stem().string();
            // 解析 CSV → 创建 DataFrameChannel → Register
            auto channel = LoadCsvFile(entry.path());
            if (channel) Register(name.c_str(), channel);
        }
    }
    return 0;
}
```

进程重启后自动恢复所有具名 DataFrame 通道，无需用户重新导入。

### 5.4 plugin_register.cpp

```cpp
// 注册三个接口 IID
REGISTER_INTERFACE(IID_PLUGIN, plugin)
REGISTER_INTERFACE(IID_CHANNEL_REGISTRY, plugin)
REGISTER_INTERFACE(IID_OPERATOR_REGISTRY, plugin)
REGISTER_INTERFACE(IID_ROUTER_HANDLE, plugin)
```

---

## 6. HTTP 端点设计（IRouterHandle）

CatalogPlugin 通过 `EnumRoutes` 声明以下端点，由 RouterAgencyPlugin 统一分发：

| 方法 | URI | 说明 |
|------|-----|------|
| GET | `/channels/dataframe` | 列出所有具名 DataFrame 通道 |
| POST | `/channels/dataframe/import` | 上传 CSV 文件，创建具名通道 |
| POST | `/channels/dataframe/preview` | 预览指定通道数据（固定前 100 行） |
| POST | `/channels/dataframe/rename` | 重命名通道 |
| POST | `/channels/dataframe/delete` | 注销指定具名通道 |

> **注意**：RouterAgencyPlugin 使用精确路径匹配（`unordered_map<method:path, handler>`），不支持路径参数（`{name}`）。所有需要传递通道名的操作均改为 POST + body 传参，避免 API 破坏性变更（L16）。

**GET /channels/dataframe 响应示例**：

```json
{
  "channels": [
    {
      "name": "test_user_processed",
      "rows": 1024,
      "schema": [{"name":"id","type":4},{"name":"name","type":7}]
    }
  ]
}
```

**POST /channels/dataframe/import — CSV 导入**

CSV 导入跨越两个进程（WebPlugin 和 CatalogPlugin），采用**临时目录 + 路径传递 + move**方案：

```
前端 multipart/form-data 上传 CSV
  → WebPlugin Init() httplib handler 接收文件内容
  → 写入 <upload_dir>/<uuid>.csv（WebPlugin 配置的临时目录）
  → 调用 POST /channels/dataframe/import（JSON，走 IRouterHandle）
    {"filename": "users.csv", "tmp_path": "/path/to/upload_dir/<uuid>.csv"}
  → CatalogPlugin handler：
      1. 生成通道名（filename 去掉 .csv；冲突时追加 _YYYYMMDDHHmmss）
      2. fs::rename(tmp_path, data_dir/<name>.csv)（move，原子操作）
      3. 解析 data_dir/<name>.csv，创建 DataFrameChannel
      4. Register(name, channel)
  → 返回 {"name": "users", "rows": 500, "schema": [...]}
```

**目录配置**：

| 配置项 | 所属插件 | 说明 |
|--------|---------|------|
| `upload_dir` | WebPlugin | 临时文件目录，接收前端上传的 CSV |
| `data_dir` | CatalogPlugin | 持久化目录，存储具名通道的 CSV 文件 |

两个目录职责完全解耦。**部署约束**：两个目录必须在同一文件系统挂载点，确保 `fs::rename` 为原子操作；若跨文件系统，实现需退化为 copy + delete。

- 通道名生成规则：`filename` 去掉 `.csv` 后缀；若名称已存在，追加 `_<timestamp>`（格式 `_YYYYMMDDHHmmss`）
- CSV 解析：第一行为列名，后续行为数据；类型自动推断（优先级：INT64 → DOUBLE → STRING，空值按 STRING）
- 文件大小限制：建议 10MB（WebPlugin 层拦截）

**POST /channels/dataframe/preview 请求/响应示例**：

```json
// 请求
{"name": "test_user_processed"}

// 响应（格式对齐 DatabasePlugin /preview）
{
  "columns": ["id", "name", "score"],
  "types": ["INT64", "STRING", "DOUBLE"],
  "data": [[1, "Alice", 99.5], [2, "Bob", 88.0]],
  "rows": 2
}
```

**POST /channels/dataframe/rename 请求/响应示例**：

```json
// 请求
{"name": "users", "new_name": "clean_users"}

// 响应
{"name": "clean_users"}
```

- `name` 不存在时返回 404
- `new_name` 已存在时返回 409 CONFLICT
- 同步重命名磁盘文件：`fs::rename(data_dir_/<name>.csv, data_dir_/<new_name>.csv)`

**POST /channels/dataframe/delete 请求/响应示例**：

```json
// 请求
{"name": "users"}

// 响应
{"ok": true}
```

- `name` 不存在时返回 404

**说明**：
- 所有具名通道均持久化，重启自动恢复，无论来源（SQL 写入或 CSV 导入）
- `Unregister` 同步删除 `data_dir_/<name>.csv`
- 不提供 PUT（更新数据）端点，数据只能通过 SQL 重新写入或重新导入 CSV 覆盖

---

## 7. Scheduler 集成

Scheduler 在解析 SQL 通道引用时，按 Catelog 分流：

```
ParseChannelRef("mysql.prod.test_user")
  → catelog = "mysql" → IDatabaseFactory::Get("mysql", "prod")

ParseChannelRef("dataframe.test_user_processed")
  → catelog = "dataframe" → IChannelRegistry::Get("test_user_processed")
```

写入 `dataframe.xxx` 时：

```
INTO dataframe.test_user_processed
  → 创建 DataFrameChannel 实例
  → 若名称已存在：先 Unregister（删旧通道 + 旧磁盘文件），再 Register（覆盖语义）
  → 若名称不存在：直接 Register
  → Pipeline 执行完毕后，通道留在 Registry 中供后续 SQL 读取
```

**覆盖语义说明**：SQL 写入路径（`INTO dataframe.xxx`）采用覆盖语义，每次执行都以最新结果为准，符合用户直觉。HTTP API（`/import`）采用冲突返回 409 语义，要求用户显式决策，避免误覆盖已导入的数据。两者语义不同，各有合理性。

---

## 8. 清理计划

| 现状 | 改后 |
|------|------|
| `plugins/example/` (MemoryChannel + PassthroughOperator) | 删除目录，代码移入 `framework/core/` |
| `plugins/testdata/` (DataFrame 通道) | 删除目录 |
| 无 DataFrame 通道注册中心 | `CatalogPlugin` 暴露 `IChannelRegistry` |
| 无内置算子类型注册表 | `CatalogPlugin` 暴露 `IOperatorRegistry` |
| `dataframe.xxx` 通道无法在 SQL 中寻址 | Scheduler 新增 `dataframe.` 分支 |
| 具名通道重启丢失 | `Register` 自动落盘，`Start()` 扫描恢复 |
| deploy-single.yaml / deploy-multi.yaml 引用旧插件 | 替换 `libflowsql_example.so` / `libflowsql_testdata.so` 为 `libflowsql_catalog.so`，补充 `data_dir` 和 `upload_dir` 配置 |

---

## 9. 补充说明

### MemoryChannel 定位

移入 `framework/core/` 后保留为公共类，与 `DataFrameChannel` 并列，供任何需要内存队列语义的场景使用。

### Schema 展示与 rows 字段

`DataFrameChannel::Schema()` 已返回 JSON 格式列定义：

```json
[{"name":"id","type":4},{"name":"name","type":7},{"name":"score","type":6}]
```

其中 `type` 为 `DataType` 枚举整数值。`GET /channels/dataframe` 响应中直接透传此字段，前端负责将枚举值映射为可读类型名（INT64、STRING、DOUBLE 等）。

`rows` 字段通过以下路径获取：`DataFrameChannel::Read(IDataFrame*)` → `IDataFrame::RowCount()`。Handler 实现时创建临时 `DataFrame` 对象，调用 `Read()` 后取行数，无需额外接口改动。完整响应格式：

```json
{
  "channels": [
    {
      "name": "test_user_processed",
      "rows": 1024,
      "schema": [{"name":"id","type":4},{"name":"name","type":7}]
    }
  ]
}
```

---

## 10. 测试设计

### 10.1 测试分层

| 层次 | 目标 | 位置 |
|------|------|------|
| 单元测试 | IChannelRegistry / IOperatorRegistry 接口行为 | `src/tests/test_builtin/` |
| 集成测试 | CatalogPlugin 通过 PluginLoader 加载后的完整路径 | `src/tests/test_builtin/` |
| 端到端测试 | Scheduler 跨通道链路（dataframe. 寻址） | `src/tests/test_scheduler_e2e/` |
| HTTP 接口测试 | 5 个 HTTP 端点的请求/响应/错误码 | `src/tests/test_builtin/` |

**CMakeLists.txt 要求**：所有测试目标必须显式加 `-UNDEBUG`（L11）：
```cmake
target_compile_options(test_builtin PRIVATE -UNDEBUG)
```

### 10.2 Story 9.1 — 清理验证测试

| ID | 测试场景 | 验证点 |
|----|---------|--------|
| T01 | MemoryChannel 在新路径直接构造 | 读写行为与移动前一致 |
| T02 | PassthroughOperator 在新路径直接构造 | 执行行为与移动前一致 |
| T03 | 所有引用旧路径的测试目标重新编译并运行 | 无编译错误，无运行时 crash |

### 10.3 Story 9.2 — IChannelRegistry 单元测试

| ID | 测试场景 | 验证点 |
|----|---------|--------|
| T04 | Register 新通道 → Get | 返回正确 shared_ptr，引用计数 +1 |
| T05 | Register 同名通道 | 返回 -1（CONFLICT），原通道不变，磁盘文件不变 |
| T06 | Get 不存在名称 | 返回 nullptr |
| T07 | Unregister 已注册通道 | Get 返回 nullptr，磁盘文件已删除 |
| T08 | Unregister 不存在名称 | 返回 -1，无副作用 |
| T09 | Rename 正常路径 | 新名可 Get，旧名返回 nullptr，磁盘文件已重命名 |
| T10 | Rename new_name 已存在 | 返回 -1，原通道不变，磁盘文件不变 |
| T11 | List 枚举 | 返回所有已注册通道名，数量正确 |
| T12 | 重启恢复：Register 后重新调用 Start() | 通道自动恢复，数据行数与列定义一致 |
| T13 | 并发安全：8 线程同时 Register/Get/Unregister | 无 crash，无数据串扰，最终状态一致（L12） |
| T14 | 落盘失败：data_dir_ 设为只读目录 | Register 返回错误码，通道未加入内存 Registry |

**IOperatorRegistry 测试**：

| ID | 测试场景 | 验证点 |
|----|---------|--------|
| T15 | Register "passthrough" → Create | 返回非 nullptr 实例，类型正确 |
| T16 | Create 未注册名称 | 返回 nullptr |
| T17 | List | 包含 "passthrough"，数量正确 |

### 10.4 Story 9.3 — Scheduler 集成测试

> **要求**：端到端测试必须通过 PluginLoader 加载 `libflowsql_catalog.so`，不能直接 `new CatalogPlugin()`（L9）。

| ID | 测试场景 | 验证点 |
|----|---------|--------|
| T18 | `INTO dataframe.result` | Registry::Get("result") 返回非 nullptr，行数正确 |
| T19 | `INTO dataframe.result` → `FROM dataframe.result INTO sqlite.local.t2` | 数据完整传输，行数一致 |
| T20 | `FROM dataframe.<不存在名称>` | 返回明确错误码（NOT_FOUND），不 crash |
| T21 | `INTO dataframe.result` 执行两次（覆盖） | 第二次覆盖第一次，磁盘文件更新，行数为第二次结果 |
| T22 | 匿名 DataFrame 通道（Pipeline 内部中间结果）行为不变 | 现有相关测试全部通过，无回归 |
| T23 | 完整跨通道链路：`FROM mysql.prod.t USING passthrough INTO dataframe.out` → `FROM dataframe.out INTO sqlite.local.t2` | 数据完整，无中间状态泄漏 |

### 10.5 Story 9.4 — HTTP 端点测试

| ID | 端点 | 测试场景 | 期望结果 |
|----|------|---------|---------|
| T24 | GET /channels/dataframe | 有通道时 | 返回正确 channels 列表（name/rows/schema） |
| T25 | GET /channels/dataframe | 无通道时 | 返回 `{"channels":[]}` |
| T26 | POST /channels/dataframe/import | 正常 CSV | 返回 name/rows/schema，通道可 Get，磁盘文件存在 |
| T27 | POST /channels/dataframe/import | 名称冲突 | 追加时间戳后缀，原通道不受影响 |
| T27a | POST /channels/dataframe/import | tmp_path 不可访问（路径不存在或无权限） | 返回 400，Registry 无新通道，无残留文件 |
| T28 | POST /channels/dataframe/import | 超过 10MB | 返回 400，无临时文件残留 |
| T29 | POST /channels/dataframe/preview | 通道存在 | 返回前 100 行，格式对齐 DatabasePlugin /preview |
| T30 | POST /channels/dataframe/preview | 通道不存在 | 返回 404 |
| T31 | POST /channels/dataframe/rename | 正常重命名 | 返回 200，新名可 Get，旧名返回 nullptr |
| T32 | POST /channels/dataframe/rename | name 不存在 | 返回 404 |
| T33 | POST /channels/dataframe/rename | new_name 已存在 | 返回 409，原通道不变 |
| T34 | POST /channels/dataframe/delete | 通道存在 | 返回 200，Registry 移除，磁盘文件删除 |
| T35 | POST /channels/dataframe/delete | 通道不存在 | 返回 404 |

**CSV 类型推断专项测试**：

| ID | 输入 | 期望推断类型 |
|----|------|------------|
| T36 | 全整数列（`1,2,3`） | INT64 |
| T37 | 含小数列（`1.5,2.0`） | DOUBLE |
| T38 | 混合整数+字符串列（`1,abc`） | STRING |
| T39 | 含空值列（`1,,3`） | STRING（不 crash） |
| T40 | 首行为列名，数据行为空 | 0 行，schema 按 STRING 推断 |

### 10.6 Sprint 7 遗留测试（Sprint 8 末尾必须落地）

| ID | 来源 | 测试场景 |
|----|------|---------|
| T41 | Story 8.3 T1 | GET /channels/database 返回正确通道列表（SQLite） |
| T42 | Story 8.3 T2 | GET /channels/database 返回正确通道列表（MySQL） |
| T43 | Story 8.3 T3 | GET /channels/database 返回正确通道列表（ClickHouse，L22） |
| T44 | Story 8.3 T4 | GET /channels/database/{type}/{name}/tables 返回表列表 |
| T45 | Story 8.3 T5 | GET /channels/database/{type}/{name}/tables/{table}/columns 返回列定义 |
| T46 | Story 8.3 T6 | ClickHouse describe 响应格式兼容性验证（L22） |
