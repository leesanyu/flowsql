# 数据库通道浏览功能设计文档

## 背景与目标

用户希望在数据库通道管理页面增加"浏览"能力，方便直接在 Web UI 中查看数据库内容，而无需切换到外部工具。

**三项能力：**
1. **列出表** — 查看某个通道下的所有表名
2. **表结构** — 查看某张表的字段定义（列名、类型、是否可空、是否主键）
3. **数据预览** — 查看前 100 条数据（固定 LIMIT，不支持自定义 SQL）

> 复杂查询请使用 SQL 工作台。

---

## 后端设计

### 新增路由（DatabasePlugin）

在现有 `/channels/database/*` 路由组下追加 3 个端点：

| 方法 | 路由 | 功能 |
|------|------|------|
| POST | `/channels/database/tables` | 列出所有表 |
| POST | `/channels/database/describe` | 获取表结构 |
| POST | `/channels/database/preview` | 预览数据（前 100 条） |

### 请求/响应格式

**tables**
```json
// 请求
{"type":"sqlite","name":"mydb"}

// 响应
{"tables":["users","orders","products"]}
```

**describe**
```json
// 请求
{"type":"sqlite","name":"mydb","table":"users"}

// 响应：直接透传数据库原始结果，不做字段映射
// SQLite 返回：cid/name/type/notnull/dflt_value/pk
// MySQL 返回：Field/Type/Null/Key/Default/Extra
// ClickHouse 返回：name/type/default_type/...
{
  "columns": ["cid","name","type","notnull","dflt_value","pk"],
  "types":   ["INT32","STRING","STRING","INT32","STRING","INT32"],
  "data":    [[0,"id","INTEGER",1,"",1],[1,"name","TEXT",0,"",0]],
  "rows":    2
}
```

> 前端直接以表格形式展示原始列，不做字段语义解析。各数据库返回的列名不同，前端无需感知差异。

**preview**
```json
// 请求
{"type":"sqlite","name":"mydb","table":"users"}

// 响应
{
  "columns": ["id","name","email"],
  "types":   ["INT32","STRING","STRING"],
  "data":    [[1,"Alice","alice@example.com"],[2,"Bob","bob@example.com"]],
  "rows":    2
}
```

### 元数据 SQL（按数据库类型分支）

| 操作 | SQLite | MySQL | ClickHouse |
|------|--------|-------|------------|
| 列表 | `SELECT name FROM sqlite_master WHERE type='table' ORDER BY name` | `SHOW TABLES` | `SHOW TABLES` |
| 表结构 | `PRAGMA table_info(<table>)` | `DESCRIBE <table>` | `DESCRIBE <table>` |
| 预览 | `SELECT * FROM <table> LIMIT 100` | 同左 | 同左 |

### 安全性

- `table` 参数校验：只允许 `^[a-zA-Z0-9_.]+$`，防止 SQL 注入
- 预览 SQL 固定为 `SELECT * FROM <table> LIMIT 100`，不接受用户输入 SQL

### 实现路径

- 文件：`src/services/database/database_plugin.h/cpp`
- 通过 `IDatabaseFactory::Get(type, name)` 获取 `IDatabaseChannel`，失败时返回 `NOT_FOUND`
- 调用 `IDatabaseChannel::CreateReader(sql, &reader)` 执行元数据 SQL
- IBatchReader 用 RAII 管理（`std::unique_ptr` + 自定义 deleter 调用 `Close()` + `Release()`），确保异常路径不泄漏
- Arrow IPC → JSON：DatabasePlugin 不依赖 `scheduler.so` 中的 `DataFrame`，在 `database_plugin.cpp` 内实现轻量的 `BatchReaderToJson()` 工具函数，直接操作 Arrow C++ API（`arrow::ipc::ReadRecordBatch` + `arrow::Array::GetScalar`），复用与 `DataFrame::ToJson()` 相同的逻辑

### WebPlugin 双通道注册（L20 教训）

新增的 3 个路由必须在 **两处** 同步注册，否则前端直连 8081 会 404：

| 注册位置 | 文件 | 说明 |
|---------|------|------|
| `Init()` httplib 直接注册 | `web_server.cpp` | 前端 → 8081 直连路径 |
| `EnumApiRoutes()` IRouterHandle 声明 | `web_server.cpp` | Gateway → RouterAgencyPlugin 转发路径 |

两处均代理到 `scheduler_host_:scheduler_port_`（即 DatabasePlugin 所在的 Scheduler 服务）。

---

## 前端设计

### 入口

`Channels.vue` 操作列新增"浏览"按钮，仅对 sqlite/mysql/clickhouse 类型显示（与"删除"按钮并列）。

### 交互流程

```
点击"浏览" → 打开 Drawer → 调用 listDbTables → 左侧显示表列表
点击表名   → 调用 describeDbTable + previewDbTable → 右侧显示表结构和数据
```

### Drawer 布局

```
┌─────────────────────────────────────────────────────────┐
│ 浏览：mysql.mydb                                  [关闭] │
├──────────────┬──────────────────────────────────────────┤
│ 表列表       │ [表结构] [数据预览]  ← Tab 切换           │
│ ─────────    │ ─────────────────────────────────────────│
│ users    ●   │ 表结构（el-table）                        │
│ orders       │ 列名 | 类型 | 可空 | 主键                 │
│ products     │ id   | INT  |  N   |  Y                  │
│              │ name | TEXT |  Y   |  N                  │
│              │ ─────────────────────────────────────────│
│              │ 数据预览（el-table，前 100 条）            │
│              │ id | name | ...                          │
│              │  1 | Alice| ...                          │
└──────────────┴──────────────────────────────────────────┘
```

- Drawer 宽度：60%
- 左侧表列表：固定宽度 180px，可滚动
- 右侧：Tab 组件，默认选中"表结构"
- 默认自动选中第一张表

### API 新增（api/index.js）

```js
listDbTables:    (type, name)        => api.post('/api/channels/database/tables',   { type, name }),
describeDbTable: (type, name, table) => api.post('/api/channels/database/describe', { type, name, table }),
previewDbTable:  (type, name, table) => api.post('/api/channels/database/preview',  { type, name, table }),
```

---

## 涉及文件

| 文件 | 变更 |
|------|------|
| `src/services/database/database_plugin.h` | 新增 3 个 Handler 声明 |
| `src/services/database/database_plugin.cpp` | EnumRoutes 追加 3 条路由 + Handler 实现 + BatchReaderToJson 工具函数 |
| `src/services/web/web_server.cpp` | Init() 和 EnumApiRoutes() 各补充 3 条代理路由 |
| `src/frontend/src/api/index.js` | 新增 3 个 API 方法 |
| `src/frontend/src/views/Channels.vue` | 新增"浏览"按钮 + Drawer 组件 |

---

## 设计决策

| 问题 | 决策 | 理由 |
|------|------|------|
| 是否给 IDatabaseChannel 增加 Tables/Describe/Preview/DropTable 接口 | **否** | 现有 CreateReader/ExecuteSql 已覆盖所有需求；数据库类型分支逻辑应在 Handler 层处理，不应下沉到接口；避免接口膨胀，所有驱动无需实现重复的 SQL 拼接逻辑 |
| DropTable 是否纳入浏览器功能 | **否** | 浏览器定位为只读预览，破坏性操作不在此范围内 |
| describe 响应是否做字段映射 | **否，直接透传原始结果** | 各数据库返回列名不同（cid/Field/name），映射会丢失信息（如 MySQL Key 列的 UNI/MUL 值）；原始列名对开发者本身有意义；前端直接渲染表格无需感知语义 |
| Arrow IPC → JSON 转换方式 | **DatabasePlugin 内实现 BatchReaderToJson()** | DatabasePlugin 不能依赖 scheduler.so 中的 DataFrame；在 database_plugin.cpp 内实现轻量工具函数，直接操作 Arrow C++ API，与 DataFrame::ToJson() 逻辑相同 |

---

## 验证方式

1. 编译通过
2. curl 直接测试 DatabasePlugin 端口（18803）：
   ```bash
   curl -X POST http://localhost:18803/channels/database/tables \
     -H 'Content-Type: application/json' \
     -d '{"type":"sqlite","name":"mydb"}'
   ```
3. curl 通过 WebPlugin 端口（8081）验证双通道注册正确：
   ```bash
   curl -X POST http://localhost:8081/api/channels/database/tables \
     -H 'Content-Type: application/json' \
     -d '{"type":"sqlite","name":"mydb"}'
   ```
4. 前端打开通道列表，点击"浏览"按钮，验证 Drawer 正常展示表列表、表结构、数据预览

### 测试用例（test_database_manager 或新增 test_db_browser）

| 用例 | 场景 | 断言 |
|------|------|------|
| T1 | SQLite tables | 返回表名列表，不为空 |
| T2 | SQLite describe | 返回原始 PRAGMA 列（cid/name/type/notnull/dflt_value/pk） |
| T3 | SQLite preview | 返回不超过 100 行数据 |
| T4 | table 参数注入防护 | `table="users; DROP TABLE users"` 返回 BAD_REQUEST |
| T5 | 通道不存在 | `type="sqlite",name="notexist"` 返回 NOT_FOUND |
| T6 | 空数据库 | tables 返回 `{"tables":[]}` 而非报错 |
