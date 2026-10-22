# Feature: 多数据库探索兼容

状态：`[x]`，T1 → T2 → T3 → T4 → T5 已按顺序实施、验收、逐项标记。
前置：[SQLite 基础探索](../archive/feat-visualization-database.md)。

## 业务意图

让 MySQL、PostgreSQL、ClickHouse 用户在现有探索工作区选择已有通道的单表，用源库原生 SQL 完成与 SQLite 一致的基础统计及柱状、折线、散点图，保留只读、类型可信、取消和有界回收保证。

## Non-Goals

- 不新增分箱、时间桶、去重计数、DataFrame、跨表/跨库 Join、SQL 编辑器或连接管理界面。
- 不迁移原表，不把整表交给浏览器/Arrow 做统计，不更改账号、业务数据或用户部署。
- 不承诺跨请求快照、自动重试、任意数据库类型或不受控的 SQL 函数；沿用受限 workflow。
- 不执行无关 Kafka/产品全量/镜像运行矩阵，不提交或推送。

## 契约与核心结构

沿用 `IDatabaseExplorationSourceV1` / `IDatabaseExplorationSessionV1`，不追加旧虚表方法。
`Query` 仅接受单条 SELECT、绑定参数、有界结果；失败清空 output；Cancel 可并发调用且不关闭正在使用的库句柄。
每请求独占会话、保留通道租约，先释放结果和会话再释放插件；取消不影响其他请求。

```cpp
struct SourceSchemaV1 {
    std::string table;
    std::vector<FieldV1> fields;
    std::string dialect = "sqlite";
    std::string schema;  // PG schema; MySQL/CH current database
};
```

- HTTP v1 保持兼容；SourceV1.dialect 扩展 mysql/postgres/clickhouse，来源 ID 包含方言、通道、schema、表；字段 ID 为元信息顺序映射，schema_revision 包含字段类型和可空性。
- 发现范围：MySQL/ClickHouse 配置的当前数据库；PG 当前数据库非系统 schema 中当前账号可 SELECT 的普通表。服务端分别引用 schema/table，不拼接未经引用的标识符；同名表不冲突。
- 操作沿用 projection/filter/AND/OR/group/count/sum/mean/min/max/sort/Top-N，过滤使用原生绑定值；分组按元组，NULL 不等于空串，COUNT(*) 不等于 COUNT(field)。
- mean([10,NULL,20])=15；空集/全 NULL 的 sum/mean/min/max 为 NULL。ClickHouse 显式使用 OrNull 聚合；排序/文本比较遵循源库 collation，NULL 排序按源库。
- 字符串/布尔/安全整数/浮点数可绘图；二进制、复合类型明确不支持。Decimal 与大整数不得静默损失精度；Decimal 作为原生精确文本保留于驱动边界，仅可证明安全的值转为图表数值，否则 UNSUPPORTED_TYPE。
- 日期/时间保留源库文本及 native_type，默认字符串，用户显式选择规范 UTC 时间轴；不猜测源时区、不隐式转 UTC。
- MySQL/PG 使用只读事务；ClickHouse 使用 readonly=2、单条 SELECT、query_id、服务端期限与取消，无跨语句一致快照承诺。
- 沿用 10000 行/8MiB/30s/2 活动会话，可按已有部署选项调整；取消/超时/停止目标 2s 内回收本地请求与独占连接，并验证服务端查询终止。
- 元信息与错误不泄露凭证；数据库失效必须非零、可见且脱敏。

## 主链路

1. 工作区 → 来源发现 → 工厂按方言取得租约 → 独占只读会话 → 原生元信息 → 单表字段/能力/语义。
2. 真实 Graphic Walker workflow → 校验字段/能力/预算 → 方言 SQL + 参数 → 原生有界查询 → 精度检查 → 图表；替代/取消/超时/停机沿同一会话终止。

## Feature Tasks 与验收锚点

- [x] **T1 — 建立兼容的多库来源与方言契约，确保同一基础 workflow 在各库生成正确且安全的原生 SQL。**
  验收：方言引用、schema 隔离、绑定值、空值聚合和未知操作拒绝的机器断言；SQLite 既有计算/资源边界回归通过。
- [x] **T2 — MySQL 用户可通过独占只读会话获得可信的基础统计，并在取消后回收查询资源。**
  验收：真实 MySQL 参数/NULL/复合分组/类型/只读/上限、取消与期限、独立请求不被误取消；现有 MySQL 驱动回归。
- [x] **T3 — PostgreSQL 用户可区分 schema 同名表并得到正确统计，保持只读与查询生命周期保证。**
  验收：真实 PG 原生参数、整数均值、schema 引用、布尔/精度/空值、只读/上限/取消/期限及现有驱动回归。
- [x] **T4 — ClickHouse 用户可得到与基础契约一致的 NULL 统计，并取消实际服务端查询。**
  验收：真实 CH OrNull、绑定、Nullable/UInt64/Decimal/不支持类型、readonly、行字节限额、query_id 取消与期限、既有驱动回归。
- [x] **T5 — 用户在同一工作区完成四库选表和三类图表，获得可复现的文档与受影响范围验收。**
  验收：实际插件/HTTP 与真实浏览器覆盖三个新增后端的柱状/折线/散点/过滤；错误与精度可见、配置不变；四库专项和共享驱动/会话回归全绿。

## 测试与实施约束

- 当前真实配置唯一来源 config/testing/environment.json，依赖声明 config/testing/dependencies.json；只使用受管专用 driver 库及独立表名、资源锁。
- 原生测试先从实际执行位置 doctor 验证三库认证与目标库；缺依赖非零，禁止 SKIP 或猜密码。
- 每个 Task 在工作台冻结允许文件、具体命令与 10～30 分钟切片；允许沿同一 Task 续轮，不改变任务语义。
- 本次用户明确授权顺序实施全部 Task，每项验收标记后可继续下一项。
- 统一入口 selected=executed=passed 且 failed=skipped=not_executed=0；未执行范围不声明通过。

## 完成证据

各项完成证据如下。证据缓存在 evidence/visualization-database-connectors/ 与 evidence/test-environment/，均不入库。

### T1 完成（2026-10-20）

- SourceSchemaV1 增加默认兼容的 dialect/schema；四库标识符引用、独立 schema 引用、绑定值及 CH OrNull SQL 断言通过。保持公开虚表与 HTTP v1 兼容。
- 定向构建退出 0，格式/diff 通过；统一入口选中=执行=通过 6，failed=skipped=not_executed=0，包含真实 SQLite 浏览器/资源回归。证据 evidence/test-environment/20261020T115050Z-80ab1832/。
- 新隔离测试项目 flowsql_vis_connectors_20261020 由统一运行时配置副本管理；三库预检/既有驱动准备检查 6/6，不操作缺归属标签的旧卷。新增后端尚未交付。

### T2 完成（2026-10-20）

- MySQL 当前库来源/字段、独占 prepared 只读查询与精确 Decimal 保留已接通。日期文本、NULL 均值 15/计数/全 NULL、复合分组、绑定注入值、大整数、行字节预算均通过；显式取消、服务端活动查询消失、第二会话独立、50ms 期限在 2s 预算内。
- 原生统一入口选中=执行=通过 5，failed=skipped=not_executed=0；源码/产物一致，构建与格式通过。证据 evidence/test-environment/20261020T122348Z-5fcd4790/，缺配置非零 evidence/visualization-database-connectors/t2-missing-config.json。
- 初轮数值读取和期限分类失败已修复并保留记录；T2 在同一任务沿续切片收口，无失败用例删减。

### T3 完成（2026-10-20）

- PG 来源按 schema/table 单独引用，普通表发现过滤系统 schema/无 SELECT 权限；OID 驱动的原生参数/动态结果读取、Numeric 精确保留、只读事务与单行有界读取已接通。
- 真实浮点/整数均值、NULL/计数/复合键、布尔、日期、带点号 schema 同名表、绑定、精度、只读/限额，以及取消后 pg_stat_activity 无活动查询、第二会话隔离/50ms 期限均通过。
- 统一入口选中=执行=通过 7，failed=skipped=not_executed=0，包含 PG 既有驱动、MySQL 探索及共享编译契约；构建/格式/diff 和缺配置非零检查通过。证据 evidence/test-environment/20261020T124339Z-a2d4033e/、evidence/visualization-database-connectors/t3-missing-config.json。

### T4 完成（2026-10-20）

- CH readonly=2、原生参数、受预算约束的 JSONCompact、OrNull 聚合、UInt64/Decimal 精确保留、非有限值保留而非 NULL、特殊 URL/NUL 绑定值已通过真实数据库验证。
- 取消使用独占 query_id 和独立 KILL QUERY SYNC；服务端查询消失、第二会话不受影响、50ms 期限及本地 2s 回收预算、只读/行字节预算/复杂类型拒绝全部通过。
- 最终统一入口选中=执行=通过 9，failed=skipped=not_executed=0，覆盖 CH 既有驱动和前两库/共享编译回归；构建、格式、diff、缺配置非零通过。证据 evidence/test-environment/20261020T130419Z-5e0d6094/。

### T5 完成（2026-10-20）

- 同一工作区提供四库来源与方言/源语义提示；实际插件/HTTP 验证 MySQL/PG/CH 来源、PG 同名表 schema 身份、字段类型、只读配置及精度错误。真实 Graphic Walker 在每库完成柱状/折线/散点、过滤、workflow 和原生 SQL 结果逐行/重复数对照，每库浏览器 7/7，多库 E2E 9/9。
- E2E 暴露的 ClickHouse AVG(Int64) 预先浮点转换已增加整数输入检查；Decimal mean 无法证明无损时明确拒绝，投影/sum/min/max 仍按精确值检查。源时区不猜测，复杂类型禁用。方言引用包含反斜杠/引号别名的真实三库断言通过。
- 标准 CMake 配置与全部冻结 target 构建成功；最终统一入口选中=执行=通过 29，failed=skipped=not_executed=0，覆盖全部探索专项、三库驱动、SQLite/连接池/数据库管理单测及共享快照回归。源码与产物前后摘要一致。
- 新/改 C++ 格式、JS/Python 语法、git diff --check、当前产物身份检查通过；40 个本次范围外的既有改动文件 SHA256 保持。配置与源表/PG schema 夹具已回收，原有用户实例和缺归属标签测试卷未改动。统一工具创建的专用隔离测试服务按标准保留复用。
- 统一证据：evidence/test-environment/20261020T132147Z-afe77045/summary.json；本地专项 evidence/visualization-database-connectors/t5/{e2e,mysql-browser,postgres-browser,clickhouse-browser}.json、final-acceptance.json、pre-existing-protection.json 及逐轮构建日志（均 Git 忽略）。前轮失败记录保留，不计通过。
- 用户说明 docs/exploration.md 已更新三库接入、权限、精度/时间/快照边界与现有启动方式。默认 build/output 产物已更新；未重启用户已有服务，不将运行进程重载计为完成。未执行无关 Kafka、镜像或产品全量检查，未提交或推送。
