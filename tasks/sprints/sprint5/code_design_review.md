# Sprint 5 代码设计评审报告

评审日期：2026-03-12
评审角色：资深测试专家
评审依据：`code.design.md` + `test_design.md`

---

## 总体评价

设计文档整体质量较高，Sprint 4 的教训基本都有对应措施。以下按严重度列出问题。

---

## P0 级问题（阻塞实现）

### P0-1：ClickHouseSession 的事务方法语义错误

设计文档第 138 行注释：
> 事务：ClickHouse 不支持，继承基类默认空实现（返回 0）

返回 0 表示"成功"，但 ClickHouse 根本不支持事务。调用方如果依赖返回值判断事务是否开启，会误认为事务已成功开启，后续 Commit/Rollback 也返回 0，整个事务语义完全失效。

测试设计文档 T9 明确要求：`BeginTransaction/Commit/Rollback 均返回 -1`。

**修改**：`ClickHouseSession` 必须显式覆盖三个事务方法，返回 -1，不能依赖基类默认实现。

---

### P0-2：`database_channel.cpp` 修改点描述自相矛盾

设计文档第 300 行：
> `database_channel.cpp` 的 `ExecuteQueryArrow/WriteArrowBatches` 已经正确委托给 Session，**不需要修改**。`CreateArrowReader/CreateArrowWriter` 保持返回 -1。

但测试设计文档 E2 要求：`CreateArrowReader()` 返回 0，读取 RecordBatch 正确。

如果 `CreateArrowReader` 保持返回 -1，E2/E3/E4/E6/E7 这五个插件层 E2E 测试全部无法通过。

需要明确：插件层 E2E 测试走的是 `CreateArrowReader/CreateArrowWriter` 还是 `ExecuteQueryArrow/WriteArrowBatches`？两者是不同接口，调用路径不同。如果 E2E 测试走 `ExecuteQueryArrow`，测试设计文档需要修正；如果走 `CreateArrowReader`，实现设计需要激活这两个方法。

**这是一个接口路径不一致的问题，必须在实现前对齐。**

---

## P1 级问题（影响测试有效性）

### P1-1：`WriteArrowBatches` 的 SQL 注入风险未处理

设计文档第 224 行：
```cpp
std::string query = "INSERT INTO `" + std::string(table) + "` FORMAT ArrowStream";
```

用反引号包裹表名，但没有转义表名内部的反引号。如果表名包含反引号（如 `` table`name ``），会产生 SQL 注入。

Sprint 4 代码审查 P0-1 就是这个问题（`CreateTable/InsertBatch` 表名注入），这里重蹈覆辙。

测试设计文档没有对 ClickHouse 写入路径设计注入场景测试，需要补充：
- `test_clickhouse.cpp` 增加 `test_quote_identifier_injection`，覆盖含反引号、含分号的表名

---

### P1-2：`ParseArrowStream` 对空结果集的处理未说明

当 ClickHouse 查询返回 0 行时（如 `SELECT * FROM t WHERE 1=0`），响应体是只有 Schema 消息、没有 RecordBatch 消息的 Arrow IPC Stream。`ParseArrowStream` 的 while 循环会直接 break，`batches` 为空，返回 0。

这个行为是正确的，但测试设计文档没有覆盖这个场景。需要在 `test_clickhouse.cpp` 增加：
- `test_empty_result_set`：查询返回 0 行，`ExecuteQueryArrow` 返回 0，`batches` 为空

---

### P1-3：`IDatabaseManager` 的 IID 定义不完整

设计文档第 393 行：
```cpp
const Guid IID_DATABASE_MANAGER = {0xb1c2d3e4, 0, 0, {0}};
```

对比项目中已有的 IID 定义风格（如 `IID_DATABASE_FACTORY = {0xa9b8c7d6, 0xef01, 0x2345, {0x67, 0x89, ...}}`），这里的 IID 只填了第一个字段，其余全为 0，与现有风格不一致，且 `{0}` 初始化 8 字节数组的方式在 C++ 中是合法的但容易误读。

建议按项目现有风格完整填写 16 字节 IID，避免与其他接口碰撞。

---

### P1-4：`AddChannel` 的幂等性语义未定义

测试设计文档 M7 要求：
> 重复 AddChannel 同 type+name，应返回错误或覆盖（明确语义）

实现设计文档没有说明这个行为。SQLite Schema 有 `UNIQUE(type, name)` 约束，重复插入会触发 `SQLITE_CONSTRAINT`，但 `AddChannel` 是返回 -1 还是执行 UPSERT？

这个语义直接影响前端"编辑通道"的实现——如果 `UpdateChannel` 是 Remove+Add，而 Add 对已存在的 type+name 返回 -1，那么 Update 会先删除成功、再添加失败，导致通道丢失。

**必须在实现前明确：AddChannel 对已存在的 type+name 是报错还是覆盖。**

---

## P2 级问题（测试覆盖缺口）

### P2-1：Epic 6 的测试设计几乎空白

实现设计文档 Story 6.6 只有两行：
> - `AddChannel()` 后 `Get()` 成功，`RemoveChannel()` 后 `Get()` 返回 nullptr
> - 重启后配置持久化验证

对照测试设计文档，Epic 6 需要 `test_database_manager.cpp`（9 个用例）+ `test_plugin_e2e.cpp` 补充（3 个用例）+ Scheduler 端点测试（7 个场景）。实现设计文档对这些测试的覆盖几乎为零，Story 6.6 的验收标准远低于测试设计文档的要求。

建议在 Story 6.6 中明确引用 `test_design.md` 第五节的用例清单，作为验收条件。

---

### P2-2：密码加密的测试验证方式未说明

测试设计文档 M5 要求：
> 直接读 SQLite 文件，验证 password 字段不是明文

实现设计文档没有说明如何在测试中验证这一点。建议在 `test_database_manager.cpp` 中，`AddChannel` 后直接用 `sqlite3_exec` 读取 `config_json` 字段，断言其中不包含原始密码字符串，且包含 `ENC:` 前缀。

---

### P2-3：`db_path` 为空时的行为未测试

`Option()` 解析 `db_path`，`Start()` 检查 `if (db_path_.empty()) return 0`。这意味着不配置 `db_path` 时，DatabasePlugin 以纯内存模式运行（无持久化）。

这个降级行为需要测试：
- 不传 `db_path` 时，`AddChannel` 是否仍然有效（内存中生效，重启后丢失）
- 还是 `AddChannel` 直接返回 -1（因为没有持久化后端）

---

## P3 级问题（建议改进）

### P3-1：`Connect()` 中 `last_error_` 信息不够丰富

```cpp
last_error_ = "ClickHouse unreachable at " + host_ + ":" + std::to_string(port_);
```

当 ClickHouse 可达但认证失败时（HTTP 401/403），这条错误信息会误导用户以为是网络问题。建议 `Ping()` 返回 HTTP 状态码和响应体，`Connect()` 根据实际错误类型生成不同的 `last_error_`。

---

### P3-2：`SerializeArrowStream` 对空 batches 的处理

如果 `batches` 为空，`MakeStreamWriter` 写完 Schema 后没有 RecordBatch，ClickHouse 收到只有 Schema 的 Arrow IPC Stream 会如何响应？需要在实现中明确处理，或在测试中验证。

---

## 问题汇总

| 编号 | 级别 | 位置 | 问题描述 | 状态 |
|------|------|------|---------|------|
| P0-1 | P0 | `clickhouse_driver.h` `ClickHouseSession` | 事务方法继承基类返回 0，语义错误，应显式覆盖返回 -1 | ✅ 已接受并修复：ClickHouseSession 显式覆盖三个事务方法，返回 -1 并填写错误信息 |
| P0-2 | P0 | `database_channel.cpp` + `test_design.md` | `CreateArrowReader/Writer` 保持 -1 与 E2E 测试要求矛盾，接口路径未对齐 | ✅ 已接受并修复：新增 `ArrowReaderAdapter`/`ArrowWriterAdapter`，激活 `CreateArrowReader/Writer`；`ClickHouseSession` 继承 `IArrowReadable`/`IArrowWritable` 供 dynamic_cast 检查 |
| P1-1 | P1 | `clickhouse_driver.cpp` `WriteArrowBatches` | 表名反引号未转义，SQL 注入风险，测试设计缺少注入场景 | ✅ 已接受并修复：新增 `QuoteIdentifier()` 函数转义反引号；测试清单补充 `test_quote_identifier_injection` |
| P1-2 | P1 | `clickhouse_driver.cpp` `ParseArrowStream` | 空结果集行为正确但未测试，需补充 `test_empty_result_set` | ✅ 已接受并修复：说明空结果集行为，测试清单补充 `test_empty_result_set` |
| P1-3 | P1 | `idatabase_factory.h` `IID_DATABASE_MANAGER` | IID 只填第一字段，与项目风格不一致，存在碰撞风险 | ✅ 已接受并修复：按项目风格完整填写 16 字节 IID `{0xb1c2d3e4, 0xf5a6, 0x7890, {0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89}}` |
| P1-4 | P1 | `database_plugin.cpp` `AddChannel` | 重复 type+name 的幂等性语义未定义，影响 UpdateChannel 正确性 | ✅ 已接受并修复：明确 `AddChannel` 对重复 type+name 返回 -1；`UpdateChannel` 改用 `INSERT OR REPLACE` 原子操作，不走 Remove+Add（避免 Remove 成功 Add 失败导致通道丢失） |
| P2-1 | P2 | `code.design.md` Story 6.6 | Epic 6 验收标准远低于 `test_design.md` 要求，需引用测试设计用例清单 | ✅ 已接受并修复：Story 6.6 完整引用 test_design.md 第五节 M1-M9、P1-P3、S1-S7 用例清单 |
| P2-2 | P2 | `test_database_manager.cpp`（待建） | 密码加密验证方式未说明，需明确测试手段 | ✅ 已接受并修复：Story 6.6 明确 M5 测试手段——直接用 `sqlite3_exec` 读取 `config_json`，断言含 `ENC:` 前缀且不含原始密码字符串 |
| P2-3 | P2 | `database_plugin.cpp` `Start()` | `db_path` 为空时 AddChannel 行为未定义，需测试降级场景 | ✅ 已接受并修复：明确 `db_path` 为空时 `AddChannel` 返回 -1（无持久化后端，拒绝动态管理），Story 6.1 已说明 |
| P3-1 | P3 | `clickhouse_driver.cpp` `Connect()` | 认证失败时 `last_error_` 误导为网络问题，建议区分错误类型 | ✅ 已接受并修复：`Ping()` 根据 HTTP 状态码区分错误类型（网络不可达 / 401-403 认证失败 / 其他错误），`last_error_` 包含 HTTP 状态码和响应体 |
| P3-2 | P3 | `clickhouse_driver.cpp` `SerializeArrowStream` | 空 batches 序列化行为未说明，需明确或测试 | ✅ 已接受并修复：`WriteArrowBatches` 对空 batches 提前返回 0，不发 HTTP 请求（避免 ClickHouse 收到只有 Schema 的 IPC Stream 行为未定义） |
