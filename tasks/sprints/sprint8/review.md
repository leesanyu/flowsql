# Sprint 8 Review

日期：2026-03-21

## 1. 审查范围
1. Web 通道管理页交互与可扩展性
2. DataFrame 预览稳定性与性能
3. 注册中心架构命名与接口抽象
4. SQL 执行链路兼容性（大小写/表名重写）
5. 插件与目录命名一致性
6. 文档与实现一致性

## 2. 本轮核心意见（来自评审）
1. 通道列表“分区展示”别扭，建议改为左侧类型侧边栏（DataFrame/数据库），为未来类型扩展留空间。
2. `SchedulerPlugin::RefreshRegistryRefs` 封装无意义，应直接取接口；且注册接口生命周期固定，无需刷新。
3. `IDataFrameRegistry` 抽象层级不对，应统一为 `IChannelRegistry`，具体能力用 `dynamic_cast` 处理。
4. “builtin”命名不理想，改为 `catalog` 更贴近“通道+算子目录服务”定位。
5. 执行 `select * from mysql.prod.test_users` 报 SQL 语法错误，需定位大小写/SQL 重写链路问题。
6. 变更后需同步文档，避免实现与设计脱节。

## 3. 发现的问题与根因
1. Web 通道列表 UX 问题  
   根因：不同类型在同页堆叠，认知切换成本高，操作按钮与当前类型耦合不清晰。
2. DataFrame 预览失败（历史出现）  
   根因：预览接口对请求字段与通道定位兼容性不足（`catelog/name` 处理与查找路径不稳）。
3. DataFrame 预览性能差、浏览器卡顿  
   根因：大列数+大条数一次性渲染，前后端未分页协同。
4. 注册中心抽象错误  
   根因：把“通道注册中心”收窄为 DataFrame 专属接口，扩展性受限。
5. MySQL 查询语法错误 near `.test_users`  
   根因：SQL 重写中 `FROM` 替换原先大小写敏感，小写 `from` 未命中，三段式名直接落到 MySQL。
6. 其他大小写风险点  
   根因：`dataframe.` 前缀识别、`catelog` 匹配、`builtin` 算子分类匹配存在大小写敏感路径。

## 4. 已落地修复
1. Web 通道页改为“左侧类型侧边栏 + 右侧内容区”，并做移动端适配。
2. DataFrame 预览走分页模式（页码/每页条数/总数），降低大表渲染开销。
3. 架构统一：移除 `IDataFrameRegistry`，引入并落地 `IChannelRegistry`。
4. 插件重命名：`BuiltinPlugin` -> `CatalogPlugin`；库名改为 `libflowsql_catalog.so`。
5. 服务目录重构：`src/services/builtin/` -> `src/services/catalog/`，引用同步。
6. SQL 重写修复：`FROM` 替换改为大小写不敏感，兼容 `select ... from ...`。
7. 大小写兼容补齐：`dataframe.` 前缀、通道 `catelog`、`USING builtin.xxx` 分类匹配改为大小写不敏感（名称本身仍区分大小写）。
8. 文档同步：Sprint8 设计/计划与 backlog/lessons 已对齐新命名与新接口。

## 5. 验证结果
1. 前端构建通过（Vite build 成功）。
2. 后端构建通过（CMake build 成功）。
3. `test_builtin`（CatalogPlugin 语义）通过。
4. `test_scheduler_e2e` 通过。
5. 现场复测反馈：本轮发现问题已修复。

## 6. 结论
1. 本轮问题从“UI可用性、架构抽象、SQL兼容性、命名一致性、文档一致性”五条线已闭环。
2. 当前实现方向与评审结论一致：统一通道注册中心、目录服务命名清晰、能力通过类型/能力接口区分。
3. 后续可选收敛项：将 `test_builtin` 目录名进一步重命名为 `test_catalog`，增强命名一致性（非功能阻塞）。
