# Feature：通道说明默认初始化（channel-description-default-initialization）

## 业务意图

在通道说明页面配置关联时，用户选择一个已有且可读取结构的目标对象即可获得一份对象本地的空白默认说明和精确字典版本，直接选择目标字段并保存关联；用户不需要先离开关联流程手工填写目标说明。

字段表将名称和编辑控件、类型和时间/关联摘要分别水平对齐，方便在同一字段内核对设置。

## Non-Goals

- 不自动加载 pending DataFrame 结构或读取业务数据行。
- 不改变关联 JSON、精确目标字典引用、来源结构匹配和版本冲突校验。
- 不自动推断字段语义、JOIN、外键、基数或业务数据正确性。
- 不修改 SQL 工作流、算子说明或其他页面。

## 核心契约

- 目标结构为 `known` 且尚无绑定说明时，关联编辑器复用 `channels/save-description` 的空 `editor_changes` 创建对象本地默认字典并应用绑定。
- 默认说明的字段身份、结构和来源快照由现有服务端保存契约生成；关联仍引用返回的精确 `dictionary_ref`。
- 目标结构为 `pending`、`unsupported`、来源不可用或保存默认说明失败时，目标字段保持不可选并显示可复核错误；不得伪造目标字段或精确引用。
- 打开字段关联弹框时，优先恢复该字段未完成的关联草稿；没有草稿且该字段已有关联时先展示关联列表，点击“新增关联”或“编辑关联”后才显示表单；该字段没有关联时直接显示新增表单。
- 字段名、单位/时间控件和关联按钮共用主行；字段类型、时间戳基准和首条关联摘要共用辅助行；字段说明保留独立的跨列编辑框。

## 主链路

1. 关联弹框选择目标对象 → describe 目标结构 → 已知且缺少字典时提交空默认说明 → 重读精确目标说明 → 启用目标字段下拉框。
2. 目标已有关联说明时直接 resolve；目标 pending 时保留现有“先加载结构”边界。

## Feature Tasks

- [x] **T1：为已知结构目标建立默认说明并让目标字段可选，保证关联仍引用可解析的精确字典版本。**
  - 验收：真实浏览器覆盖无目标说明的已知数据库目标自动初始化、目标字段可选、`describe` 返回非空精确 `dictionary_ref`，并保留 pending 目标的边界提示。
  - 证据：`evidence/test-environment/20261030T175605Z-b9442463/summary.json`（selected=executed=passed=2）；`evidence/channel-description-refinement/default-init-build.log`；`evidence/channel-description-refinement/default-init-frontend-tests.log`。
- [x] **T2：完成受影响页面和语义消费者回归，证明默认版本不会改变既有关联、时间和 SQL 工作流契约。**
  - 验收：通道说明浏览器、语义 API/客户端和 SQL 指引相关受影响回归通过；未修改的服务端保存 JSON 和 SQL 执行行为有证据。
  - 证据：`evidence/test-environment/20261030T180028Z-aa14348f/summary.json`（selected=executed=passed=4）；公共语义单测 5/5，范围与 diff 检查通过；仅复用既有通道说明保存接口，未修改 SQL 执行入口及 JSON 契约。
- [x] **T3：已有字段关联优先展示可管理的关联列表，减少误入新增表单并保留草稿恢复和首次新增体验。**
  - 验收：真实浏览器验证已有单字段/联合关系的列表首屏、关闭后重开、主动新增、编辑、取消与删除；无该字段关联时直接新增，未完成草稿仍可恢复，打开列表不触发目标目录读取或创建新增草稿。
  - 证据：`evidence/test-environment/20261031T051046Z-1f60971d/summary.json`（selected=executed=passed=2，failed=skipped=not_executed=0）；前端构建与范围检查通过，列表截图及红绿验证见 `evidence/channel-description-refinement/relation-list-first/acceptance.json`。

- [x] **T4：对齐字段的主内容与辅助信息，让用户在不同窗口宽度和主题下清楚核对时间与关联设置。**
  - 验收：字段名/单位/时间/关联操作水平对齐，字段类型/时间戳基准/关联首条摘要水平对齐；字段说明仍可编辑；三种宽度、深浅主题及既有时间和关联浏览器流程通过。
  - 证据：`evidence/test-environment/20261031T051452Z-e0571e7e/summary.json`（selected=executed=passed=2，failed=skipped=not_executed=0）；1280/1440/1920 × 深浅主题 6 组实际 DOM 测量，两排最大偏差均为 0 像素，见 `evidence/channel-description-refinement/field-alignment/geometry.json`；前端构建、截图复核、范围与格式检查通过。

## 完成证据

- T1–T4 均完成；默认目标说明初始化、已有关联先列表与字段布局分别按序验收后勾选。
- 本次最终前端产物由 `cmake --build build --target flowsql_frontend -j4` 更新到 `build/output/static`；最终浏览器使用该产物及隔离插件宿主，未重载用户常驻服务。
- 本地构建、浏览器、坐标测量、截图与源码/产物摘要统一见 `evidence/channel-description-refinement/field-alignment/acceptance.json`（未入库）。服务端 JSON 与 SQL 执行契约未改动；未 commit、未 push。
