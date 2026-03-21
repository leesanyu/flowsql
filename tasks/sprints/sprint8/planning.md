# Sprint 8 规划

## Sprint 信息

- **Sprint 周期**：Sprint 8
- **开始日期**：2026-03-19
- **结束日期**：待定
- **预计工作量**：6-8 天
- **Sprint 目标**：内置通道与算子注册中心（Epic 9）——清理架构债务、建立 CatalogPlugin、支持具名 DataFrame 通道跨 Pipeline 共享、CSV 导入

---

## Sprint 目标

### 主要目标

1. **架构清理**：删除 `plugins/example` 和 `plugins/testdata`，将 MemoryChannel / PassthroughOperator 移入 `framework/core/`
2. **CatalogPlugin**：实现 `IChannelRegistry` + `IOperatorRegistry` 接口，建立进程内注册中心
3. **Scheduler 集成**：`dataframe.<name>` 通道寻址，支持 SQL `INTO dataframe.xxx` / `FROM dataframe.xxx`
4. **Web 能力**：CSV 导入、通道预览、重命名、删除，Channels 页面新增 DataFrame 通道分组

### 成功标准

- [x] `plugins/example` 和 `plugins/testdata` 目录已删除，所有引用更新
- [x] `MemoryChannel` / `PassthroughOperator` 在 `framework/core/` 中可直接构造使用
- [x] `CatalogPlugin` 编译通过，单元测试覆盖注册/查找/注销/重命名流程
- [x] SQL `INTO dataframe.result` 执行后通道可被后续 SQL `FROM dataframe.result` 读取
- [x] 端到端链路：`FROM mysql.prod.t USING op INTO dataframe.out` → `FROM dataframe.out INTO sqlite.local.t2` 通过
- [x] CSV 上传后自动创建具名通道，名称冲突时追加时间戳
- [x] Channels 页面展示 DataFrame 通道列表（名称、行数、列定义）
- [x] 预览、重命名、删除操作均正常
- [x] `npm run build` 编译无报错
- [x] 所有现有功能回归正常

---

## 设计文档

- [CatalogPlugin 设计](design.md)

---

## Story 列表

---

### Story 9.1：清理 plugins/example 和 plugins/testdata

**优先级**：P0（后续 Story 依赖干净的代码基础）
**工作量估算**：0.5 天
**依赖**：无
**状态**：✅ 已完成

**验收标准**：
- [x] 删除 `plugins/example/` 目录（MemoryChannel + PassthroughOperator）
- [x] 删除 `plugins/testdata/` 目录
- [x] `MemoryChannel` 移入 `src/framework/core/memory_channel.h/cpp`，保留为公共类
- [x] `PassthroughOperator` 移入 `src/framework/core/passthrough_operator.h/cpp`
- [x] 所有引用这两个插件的测试和代码更新为直接构造，编译通过
- [x] `config/deploy-single.yaml` 和 `config/deploy-multi.yaml` 中删除 `libflowsql_example.so` / `libflowsql_testdata.so` 条目（占位，待 Story 9.2 完成后替换为 `libflowsql_catalog.so`）
- [x] 测试用例 T01-T03 通过（见 design.md §10.2）

**任务分解**：
- [x] 将 MemoryChannel 代码移入 `src/framework/core/`
- [x] 将 PassthroughOperator 代码移入 `src/framework/core/`
- [x] 更新所有 `#include` 和 CMakeLists.txt 引用
- [x] 删除 `plugins/example/` 和 `plugins/testdata/` 目录
- [x] 编译验证

---

### Story 9.2：IChannelRegistry 接口 + CatalogPlugin 骨架

**优先级**：P0
**工作量估算**：1.5 天
**依赖**：Story 9.1
**状态**：✅ 已完成

**验收标准**：
- [x] 新增 `src/framework/interfaces/ichannel_registry.h`（IChannelRegistry，shared_ptr 语义，含 Register/Get/Unregister/Rename/List）
- [x] 新增 `src/framework/interfaces/ioperator_registry.h`（IOperatorRegistry，含 Register/Create/List）
- [x] 新增 `src/services/catalog/` 目录，实现 CatalogPlugin（多继承 IPlugin + IChannelRegistry + IOperatorRegistry + IRouterHandle）
- [x] `Option()` 支持 `data_dir` 配置项（默认 `./dataframes/`）
- [x] `Register` 自动序列化通道数据到 `data_dir/<name>.csv`（具名即持久化）
- [x] `Unregister` 同步删除磁盘文件；`Rename` 同步重命名磁盘文件
- [x] `Start()` 扫描 `data_dir` 自动恢复所有具名通道
- [x] `Load()` 阶段注册内置算子类型（passthrough）
- [x] 编译通过
- [x] 单元测试覆盖：注册/查找/注销/重命名/重启恢复/并发安全
- [x] 测试用例 T04-T17 通过（见 design.md §10.3）

**任务分解**：
- [x] 新增 `ichannel_registry.h`
- [x] 新增 `ioperator_registry.h`
- [x] 新增 `src/services/catalog/catalog_plugin.h/cpp`（含 CSV 序列化/反序列化工具函数）
- [x] 新增 `src/services/catalog/plugin_register.cpp`（注册 IID_CHANNEL_REGISTRY / IID_OPERATOR_REGISTRY / IID_ROUTER_HANDLE）
- [x] 新增 `src/services/catalog/CMakeLists.txt`
- [x] 编写单元测试 `src/tests/test_builtin/`

---

### Story 9.3：Scheduler 集成 — dataframe. 通道寻址

**优先级**：P0
**工作量估算**：2 天
**依赖**：Story 9.2
**状态**：✅ 已完成

**验收标准**：
- [x] Scheduler `FindChannel()` 新增 `dataframe.` 分支，走 `IChannelRegistry::Get`
- [x] SQL `INTO dataframe.<name>` 执行后自动调用 `IChannelRegistry::Register`，通道留存供后续 SQL 读取
- [x] SQL `FROM dataframe.<name>` 可读取已注册的具名通道
- [x] 端到端测试：`INTO dataframe.result` → `FROM dataframe.result INTO sqlite.mydb.output` 链路通过
- [x] 匿名 DataFrame 通道（Pipeline 内部中间结果）行为不受影响
- [x] 测试用例 T18-T23 通过（见 design.md §10.4）

**任务分解**：
- [x] Scheduler `Load()` 阶段通过 IQuerier 获取 `IChannelRegistry`
- [x] `FindChannel()` 新增 `dataframe.` 前缀分支
- [x] `ExecuteTransfer()` / `ExecuteWithOperator()` 写入路径：`INTO dataframe.<name>` 时创建 DataFrameChannel 并 Register
- [x] 编写端到端测试

---

### Story 9.4：HTTP 端点 + Web UI 展示

**优先级**：P1
**工作量估算**：2.5 天
**依赖**：Story 9.2
**状态**：✅ 已完成

**验收标准**：
- [x] CatalogPlugin 实现 `GET /channels/dataframe`（列出具名通道，含 name/rows/schema）
- [x] CatalogPlugin 实现 `POST /channels/dataframe/import`（multipart 上传 CSV，自动推断类型，名称冲突时追加时间戳）
- [x] CatalogPlugin 实现 `POST /channels/dataframe/preview`（预览指定通道前 100 行，格式对齐 DatabasePlugin）
- [x] CatalogPlugin 实现 `POST /channels/dataframe/rename`（body: `{"name":"x","new_name":"y"}`，new_name 已存在返回 409）
- [x] CatalogPlugin 实现 `POST /channels/dataframe/delete`（body: `{"name":"x"}`，注销通道）
- [x] `web_server.cpp` 双通道注册（Init() httplib 代理 + EnumApiRoutes() IRouterHandle 代理）
- [x] `api/index.js` 新增 5 个 API 方法
- [x] `Channels.vue` 新增 DataFrame 通道分组展示（名称、行数、列定义）
- [x] 通道列表页顶部新增"导入 CSV"按钮，上传成功后刷新列表并高亮新通道
- [x] 每行操作列：预览（Drawer 展示前 100 行）| 重命名（弹窗编辑确认）| 删除（确认后注销）
- [x] 测试用例 T24-T40 通过（见 design.md §10.5）

**任务分解**：
- [x] `catalog_plugin.cpp` 实现 5 个 Handler + `EnumRoutes` 注册
- [x] CSV 解析工具函数（列名提取 + 类型推断：INT64 → DOUBLE → STRING）
- [x] `web_server.cpp` 新增 `upload_dir` Option 解析、multipart handler（接收文件写临时目录）、5 条 IRouterHandle 代理路由
- [x] `src/frontend/src/api/index.js` 新增 `listDfChannels` / `importCsv` / `previewDfChannel` / `renameDfChannel` / `deleteDfChannel`
- [x] `src/frontend/src/views/Channels.vue` 新增 DataFrame 通道区块 + Drawer + 重命名

---

## 风险与缓解

| 风险 | 可能性 | 缓解措施 |
|------|--------|---------|
| CSV 类型推断边界情况（混合类型列、空列、超大文件） | 中 | 限制上传文件大小（10MB，WebPlugin 层拦截），空值统一按 STRING，混合类型降级为 STRING |
| `INTO dataframe.xxx` 与现有匿名通道路径冲突 | 低 | `dataframe.` 前缀分支独立，不影响现有 IDataFrameChannel 匿名用法 |
| WebPlugin 临时文件未清理（进程崩溃） | 低 | CatalogPlugin Start() 时清理 `uploads/` 目录下残留临时文件 |
| Register 落盘失败（磁盘满、权限问题） | 低 | Register 返回错误码，调用方回滚，不将通道加入内存 Registry |

---

## Sprint 7 遗留行动项

| 行动项 | 状态 |
|--------|------|
| 为 Story 8.3 补充自动化测试（T41-T46） | 📋 延至 Sprint 8 末尾处理 |
| 验证 ClickHouse describe 响应格式兼容性 | 📋 延至 Sprint 8 末尾处理 |

---

## 测试计划

详细测试用例见 [design.md §10](design.md#10-测试设计)，共 47 个测试用例（T01-T40 + T27a + T41-T46）。

### 环境要求

| 测试范围 | 所需环境 |
|---------|---------|
| T01-T17（单元测试） | 纯内存，无外部依赖 |
| T18-T22（Scheduler 集成） | SQLite（本地文件） |
| T23（完整跨通道链路） | SQLite + MySQL |
| T24-T40（HTTP 端点） | 纯内存，无外部依赖 |
| T41-T46（Sprint 7 遗留） | SQLite + MySQL + ClickHouse |

### 关键约束

- 所有测试目标 CMakeLists.txt 必须加 `target_compile_options(... PRIVATE -UNDEBUG)`（L11）
- T18-T23 端到端测试必须通过 PluginLoader 加载 `libflowsql_catalog.so`，不能直接 `new CatalogPlugin()`（L9）
- T41-T46 中 ClickHouse 相关测试必须在真实 ClickHouse 实例上运行（L22）
