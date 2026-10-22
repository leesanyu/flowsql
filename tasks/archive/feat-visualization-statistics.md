# 时间统计探索（visualization-statistics）

## 业务意图
数据库探索用户可在已有工作区按日/月比较趋势，并统计字段的不同值数量；四库均在源库执行 SQL，过滤先于统计，输出继续遵守现有预算与取消契约。

## Non-Goals
- 不增加任意表达式、小时/周/季度桶、补齐缺失日期、累计/窗口统计或自动时区转换。
- 不修改直方图、地图、DataFrame、通道管理、插件 ABI 或部署方式。
- 不承担产品全量、Kafka、容器镜像矩阵或正式发布。

## 契约
- 扩展现有 aggregate.groupBy：字符串字段 ID 保持兼容，也接受 `{field, unit: "day"|"month", asFieldKey}`。
- 时间桶别名为结果字段，类型 string；day 返回 YYYY-MM-DD，month 返回 YYYY-MM-01。允许多个桶及普通字段共同分组，排序/Top-N 使用结果别名。
- 支持原生 DATE/DATETIME/TIMESTAMP（排除仅 TIME）；SQLite 支持 ISO 日期/日期时间文本，取前十位日期，不猜测 Unix 数值单位。
- 按源库日历分桶，不做时区转换：MySQL TIMESTAMP、PG timestamptz 使用会话时区，CH DateTime 使用列时区；无时区日期时间保留日历值。SQLite 偏移文本也保留所写日期；无效日期按 SQLite date 语义归 NULL 桶。
- NULL 时间保留为 NULL 分组；空集无桶，不补零。不支持字段、单位、未知属性及重复输出别名明确失败。
- 新增 measure.agg=`distinctCount`，执行 COUNT(DISTINCT field)，CH 使用精确 uniqExact；排除 NULL，空字符串仍为值，空集/全 NULL 返回 0。`*`/行数字段仅允许 count。
- count/distinctCount 允许文本、数值和布尔；文本相等遵循源列 collation；数值去重使用原生类型，不经过 JS 浮点转换。
- 来源元信息新增 time_bucket_fields、day/month/distinctCount 能力及 source_calendar 说明；客户端只提供服务声明可用的字段。
- 图表 config.flowsqlTimeBucket 保存 `{field,unit}`；仅在该字段作为 aggregate 分组时翻译为桶，别名保持原字段 ID。无聚合或不再使用该分类时清理设置。
- 简洁/高级编辑共享同一图表；去重可选真实字段（包括文本/布尔），行数仍固定 count；查询失败可调整字段恢复。

## 主链路
1. 选表 → 选折线/柱状及时间字段 → 选日/月 → aggregate 分组翻译 → 原生 SQL → 有界结果 → 图表。
2. 选择统计字段及去重计数 → 过滤 → 原生精确去重 → 显示结果；切页和编辑模式保存同一配置。

## Feature Tasks（依次实施）
- [x] T1：交付四库原生时间桶与精确去重的计算契约，让新增统计保持现有只读、有界和 NULL 保证。
  验收：真实 SQLite 结果、四方言 SQL/非法输入断言及现有 workflow 回归通过。
- [x] T2：交付日/月和去重的绘图入口，让文本去重、配置保留和错误恢复可在同一工作区使用。
  验收：状态单测、真实 SQLite 独立/内嵌浏览器、既有图表体验回归通过。
- [x] T3：完成四库实际统计与用户说明，让不同库的日期、空值与去重结果有可复核证据。
  验收：统一配置预检、三外部库原生日期/带时区数据与 API/浏览器结果比对、完整受影响探索回归通过，默认产物更新且指纹核对。

## 完成证据
证据仅写 evidence/visualization-statistics/（不入 Git）；逐 Task 验收后追加，全部完成后归档。

- T1：默认 test_visualization_workflow、flowsql_visualization 构建成功，格式检查通过；标准 unit selected=executed=passed=1，failed/skipped/not_executed=0，summary=evidence/test-environment/20261021T040458Z-ccf90270/summary.json。首轮因并发链接触发 artifact_changed 判失败，构建停止后完整重跑通过。

- T2：chart.test.mjs 12/12；标准入口 selected=executed=passed=5（chart、experience 环境/体验、statistics 环境/统计），failed/skipped/not_executed=0，summary=evidence/test-environment/20261021T043918Z-49f039c4；SQLite 日/月与去重浏览器 14/14。


- T3：四库隔离夹具新增 DATE、DATETIME/TIMESTAMP、TIMESTAMPTZ 和 ClickHouse DateTime64 列；标准 integration 在复用同版本受管项目配置后 selected=executed=passed=2，failed/skipped/not_executed=0，summary=evidence/test-environment/20261021T050244Z-06053081/summary.json；内部 API/浏览器核对 27/27，证据 evidence/visualization-statistics/connectors/e2e.json 和 statistics-native-baselines.json。
- T3 回归：标准 all 选中 chart、experience、statistics 及两个环境夹具共 5 项，selected=executed=passed=5，failed/skipped/not_executed=0，summary=evidence/test-environment/20261021T051320Z-5fc5901b/summary.json；workflow unit selected=executed=passed=1，summary=evidence/test-environment/20261021T051841Z-b724f7cd/summary.json。
- T3 交付：docs/exploration.md 增加时间统计、源日历、NULL、去重、精度和配置恢复说明；C++/fixture clang-format --dry-run --Werror 与 git diff --check 通过；build/output 的 flowsql_visualization、统计 fixture 和四库测试目标已更新。首次使用当前 flowsql_test 配置因 ClickHouse 同名卷 ownership 不匹配而未执行，未计入通过；随后复用已存在且同镜像摘要的受管 flowsql_vis_connectors_20261020 配置完成预检和验收，未修改卷或凭证。
