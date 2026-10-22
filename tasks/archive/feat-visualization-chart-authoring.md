# 图表选择与字段语义优化（visualization-chart-authoring）

## 业务意图

数据库探索用户在默认界面直接选择图表，并只填写该类型需要的字段；直方图选择一个数值字段即可观察源库全量分布。
本规格为用户确认的补充范围，保留 visualization-experience 归档的历史事实。

## Non-Goals

- 不实现地图、时间桶、去重计数、任意表达式、跨表关联、跨会话持久化或新的数据库驱动接口。
- 不下载截断明细计算直方图，不把高级编辑作为更多图表的前置步骤，不重启用户服务或修改业务数据。

## 核心契约

```ts
interface ChartKindV1 {
  id: string;
  label: string;
  roles: Array<{ key: string; label: string; kind: 'dimension' | 'measure' | 'any'; optional?: boolean }>;
}
interface HistogramConfigV1 { field: string; bins: number } // chart.config.flowsqlHistogram
// TableWorkspaceV1.chart 仍是唯一图表快照，类型、字段角色从快照推导。
// Compute workflow v1 的 view 增加：{op:'histogram',field:string,bins:number}
// 返回 bin_index、bin_lower、bin_upper、bin_count 四个数值字段；客户端只适配渲染字段，不重算频数。
```

- 默认图表目录：柱状、堆叠柱状、分组柱状、折线、面积、散点、圆点、饼图、热力图、表格、突出显示表格、箱线图、直方图。
- 字段不足时给出具体原因；类型可以先选再补字段，不要求用户进入高级编辑。
- 柱/线/面积为类别或时间与数值；散点为两个数值；饼图为分类与数值；热力为两个分类与数值或行数；
  箱线为数值和可选分组；表格使用所选分类/数值；直方图仅一个数值，纵向频数自动生成。
- 同类型编辑保留过滤、布局和高级配置；跨类型切换仅重建不兼容的编码，保留过滤与布局。
- 直方图默认 10 箱，允许整数 1～100；过滤先执行，再计算非 NULL 值的 min/max 和等宽分箱。
  普通箱左闭右开、最后一箱包含最大值；常量列返回一个箱，空集/全 NULL 返回零行；仅返回非空箱。
- 四库原生 SQL 同时完成边界与计数；输入范围、数值类型及非有限值不得绕过既有保护；边界近似遵循 Decimal 展示契约。
- 直方图排序按 bin_index；结果限制/偏移仍遵守已有 scope 与预算；取消、期限、只读、代次隔离不变。
- 高级编辑与默认字段设置使用同一快照；直方图字段和箱数由统一控件管理，高级拖拽改变类型后不保留过期分箱状态。

## 主链路

1. 默认选图→按类型出现字段角色→编辑唯一快照→渲染；推荐使用同一目录与配置；切页与高级往返恢复快照。
2. 单字段直方图→有界 workflow→原生过滤/分箱/计数→有序箱边界及频数→渲染；失败保留设置并允许恢复。

## Feature Tasks 与验收锚点

- [x] **T1 — 统一图表目录与字段角色契约，使全部非地图类型可由同一快照构造并安全切换。**
  验收：可执行单测覆盖 13 类型、单字段直方图、无 XY 饼图、缺字段提示、过滤/布局保留与类型往返。
- [x] **T2 — 四库提供受预算保护的源库直方图，使单字段分布统计有一致的边界和频数。**
  验收：SQLite/MySQL/PG/CH 原生 SQL 对照覆盖过滤、负值、端点、常量、空集/NULL、箱数校验及数值保护；既有 workflow 回归。
- [x] **T3 — 默认界面直接选图并按类型填写字段，用户无需进入高级即可完成直方图及其他图表。**
  验收：真实浏览器逐类绘图、无额外 Y 的直方图、动态字段、推荐/高级/多表恢复；既有体验与数值浏览器回归。
- [x] **T4 — 用户获得完整使用说明和受影响范围的可复现验收，默认交付产物包含本特性。**
  验收：全部 test_visualization_*（含新增场景）执行无跳过；产物、文档、规格状态一致。

## 测试与执行约束

- 用户已确认方案并授权依次完成；按 T1→T2→T3→T4，各任务验收勾选后重新冻结下一工作台，WIP=1。
- 每个 Atomic Slice 10～30 分钟；稳定任务可跨切片续行，不为实现步骤增加规格层级。
- 测试入口 tools/test.sh；三库测试复用统一运行时配置和受管测试项目，先从 native 执行位置 doctor，不打印凭证。
- 不涉及驱动/部署契约变更；不默认执行产品全量、Kafka、Docker 镜像或正式发布验证。

## 完成证据

本轮已完成；本地日志、基线和验收汇总缓存于 evidence/visualization-chart-authoring/，不入库。

- T1：Node 8/8；标准 test_visualization_chart 1/1，通过，零跳过；13 类型构造、角色及分箱桥接已验证。

- T2：标准清单 9/9（含 4 环境检查），evidence/visualization-chart-authoring/t2-final-tests.log；三库 E2E 21/21；ClickHouse Decimal scale 修复后完整重跑通过。

- T3：最终标准清单 7/7，20261020T165635Z-acc8afa5；状态单测 10/10，选图浏览器 36/36，既有体验 25/25，三库 E2E 21/21 与每库数值浏览器 5/5；默认产物已构建。

- T4：完整 test_visualization_* selected=executed=passed=21，failed/skipped/not_executed=0；标准证据 evidence/test-environment/20261020T170006Z-a778e5c6/summary.json。
- 最终内部场景：选图浏览器 36/36、既有体验 25/25、三库 E2E 21/21、每库数值浏览器 5/5、交付 8/8。
- 默认源码/产物 23 个指纹保持一致，manifest 7 文件校验及本轮 C++ 格式检查通过；范围外 1209 文件保持基线。
- 验收汇总 evidence/visualization-chart-authoring/acceptance.json，未入库；默认 build/output 已更新并由隔离服务验证，用户既有服务未重载。
- 未执行产品全量、Kafka、镜像/部署矩阵与正式发布；未提交推送。
