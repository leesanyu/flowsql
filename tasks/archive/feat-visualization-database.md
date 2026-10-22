# Feature: 基础探索与 Web 入口（SQLite）

状态：`[x]` T1→T4 已按顺序实施、验收并逐项标记；按规格能力及实际受影响范围完成验收。
优先级：P1。前置能力：[数据库平台](../archive/feat-database-platform.md)、现有插件框架与 HTTP 路由。

## 业务意图

让探索 SQLite 数据表的用户从现有 Web 顶栏“数据探索”进入工作区，选择字段、过滤、分组并生成柱状、折线和散点图，直接利用 SQLite 原生 SQL 得到可解释的结果。
工作区采用 Graphic Walker，可直接访问且独立运行，强调轻量部署。Feature 标识保留 `visualization-database`；首个可交付版本只验收 SQLite。

## Non-Goals

- DataFrame 接入属于 `visualization-dataframe`；本 Feature 不把数据库原表自动复制到 Arrow、DuckDB 或浏览器后做统计聚合。
- MySQL、PostgreSQL、ClickHouse 探索适配属于 `visualization-database-connectors`；直方图、分箱、时间桶和去重计数属于 `visualization-statistics`。
- 不建设仪表盘门户、多人协作、权限体系、报表调度、语义模型、跨源 Join、SQL 编辑器或任意计算表达式。
- 不交付图表持久化、导入导出、嵌入分享及新数据库连接管理页面；首版探索状态保留在当前页面内存中。
- 首版不承诺箱线图、分位数、标准差、地理图和任意 Graphic Walker 功能；仅开放下文能力集合。
- 不承诺跨图表一致性快照、自动重试和实时订阅；查询反映执行时的源数据，刷新可得到不同结果。

## 已收口的架构边界

| 边界 | 决定与可观察结果 |
| --- | --- |
| 独立进程 | 使用现有 FlowSQL 主程序加载可视化 `.so`、数据库 `.so` 及必要框架插件；单独配置、端口和生命周期。直接访问探索工作区不依赖 Scheduler、Python Worker 或现有 Web 在线，顶栏入口随现有 Web 提供。 |
| 前端与入口 | 现有 Vue 顶栏增加“数据探索”，在内容区用同源 iframe 内嵌工作区；Graphic Walker 独立 React 构建和发布，其静态资源由可视化进程提供，也可直接访问，生产运行不新增 Node 服务、Python 服务或外部云 API。 |
| 数据库计算 | Graphic Walker 外部 `computation` → 服务端校验/SQL 编译 → 同进程数据库插件 → 源数据库；浏览器仅渲染已计算结果，Arrow 可作结果表示但不承担数据库聚合。 |
| 插件调用 | 经 `IQuerier` 获取现有数据库工厂/租约能力，保留通道、查询会话和插件的所有权；不直接依赖具体驱动类，不改变既有接口 ABI。 |
| 配置所有权 | 复用已有数据库通道配置源；可视化进程只读加载，不注册数据库管理写路由，也不写回共享配置。首版配置变更后重启可视化进程生效。 |
| 路由与数据面 | 浏览器请求和有界绘图结果走 HTTP；数据库驱动继续使用各自原生协议。服务间不经 HTTP 搬运整表，未来 DataFrame 批量传输遵循共享内存 / Arrow IPC。 |
| Feature 关系 | 本 Feature 交付可用的 SQLite 探索与 Web 入口；多库兼容、分布/时间统计、DataFrame 分别扩展来源或分析能力，各自设计及验收。只复用已验证契约，不预建通用执行引擎。 |

### Feature 边界与膨胀约束

SQLite 基础探索可独立使用并单独发布，因此提升为首个 Feature 的完整交付；四库覆盖仍是阶段目标，由后续兼容 Feature 完成。
多库兼容负责适配既有基础计算，统计 Feature 负责新增分箱/时间桶等分析语义，DataFrame Feature 负责另一种数据来源及其生命周期；三者只在 Backlog 记录目标和依赖。
首个 Feature 的验收矩阵固定为 SQLite × 柱状/折线/散点 × 下表基础计算；增加数据库、图表或运算能力须归属后续 Feature，不能以修复或补齐为名混入当前规格。
查询正确性、只读边界、限额、取消、资源回收和可运行交付属于本 Feature 的完成条件，不能留待后续“加固”。本次因验收边界拆分，不以规格行数或实现文件数作拆分依据。

### iframe 入口与独立访问契约

| 访问方式 | 路径与责任 |
| --- | --- |
| 主 Web | 顶栏“数据探索”进入 Vue 路由 `/explore`；内容区 iframe 加载同源 `/exploration/?embed=1`，保留产品顶栏，子页面隐藏独立入口的外壳。 |
| 资源代理 | Web 按配置的服务端 `visualization_upstream` 转发 `/exploration/` 和 `/api/visualization/v1/`，保留路径和查询参数；仅目标服务及这两个前缀，不建设任意 URL 代理。 |
| 独立访问 | 浏览器直接访问可视化服务的 `/exploration/`；使用同一份构建产物和查询 API，无父窗口时独立初始化主题/语言并显示必要的工作区外壳。 |

- React 构建固定资源 base 为 `/exploration/`，API 使用当前 origin 下的 `/api/visualization/v1/`；JS/CSS、动态模块和 Worker 均留在资源命名空间内，内嵌/独立访问无需改写打包后的代码。
- 未配置上游时入口显示“未配置数据探索服务”；上游不可达或握手超时时仅探索区显示错误与重试，不将 Gateway 状态当作可视化服务状态。
- 代理支持实际静态资源方法及所选 API，保留状态码、Content-Type 和适用缓存头，正确处理长度/编码等逐跳传输细节；探索 404/错误不得进入主 Web 的 SPA 回退。
- 内嵌响应允许同源父页面，例如 `frame-ancestors 'self'` 与兼容的 frame 响应头；同源 iframe 提供页面/样式隔离，不作为隔离不可信代码的安全边界。
- Web 直接连接可视化服务，不依赖 Scheduler/Gateway 路由发现；代理超时预算覆盖服务端计算期限及传输余量，保留显式取消路径，断开 HTTP 不等于数据库已取消。
- 首次进入才创建一个 iframe；切换菜单时保留其 DOM 实例并切换可见性，不反复修改 src 或依赖未验证的 KeepAlive 行为。离开时通知子页停止新查询并取消旧查询，保留当前图表编辑状态。
- 父页面拥有顶栏和外部布局，iframe 占满剩余内容区；父/子避免双重滚动。浏览器整页刷新及用户主动重新加载工作区可以重置内存状态，不增加持久化承诺。

### 最小桥接与独立升级契约

- 仅使用版本化 `postMessage`：`{version: 1, type, payload}`；子页发送 `ready`/`error`，父页发送 `context`（主题/语言/可见性）和 `visibility`。校验 origin 与 source，指定精确 targetOrigin；不传数据库凭证、原表或图表结果。
- `ready` 携带 `ui_version`、`bridge_version` 和支持的 `api_version`；父子及查询服务各自校验所需契约，版本不兼容明确提示。iframe load 事件不作为就绪证明，独立访问不等待父页握手。
- 探索前端有单独构建目标、静态产物目录和版本清单，不混入现有 Vue 的 static/assets。发布包自校验完整性，沿用构建身份的真实性要求，并明确独立产物的兼容检查，不能绕过既有发布校验。
- 版本兼容时仅更新探索静态包即可升级，不要求重新编译主 Web、更新 Vue 依赖或重启主 Web；用可配置静态目录或独立挂载实现，同一份包也供独立访问。
- HTML/版本清单避免陈旧缓存，资源采用内容哈希；升级过渡保留仍需支持的旧资源，避免已打开页面加载旧分块失败。新版本在重新加载工作区后生效，首版不做强制热替换或无损会话迁移。

## 首版能力与结果语义

| 能力 | 必须交付的范围 |
| --- | --- |
| 数据选择 | 仅从已配置 SQLite 通道发现数据库表和字段；每次探索绑定单表，展示字段类型及支持的操作，不在浏览器暴露凭证或连接串。 |
| 查询 | 字段投影；比较、集合及 NULL 过滤和 AND/OR 组合；多字段分组；聚合后排序；有界明细与显式 Top-N。过滤先于聚合，Top-N 不改变参与聚合的数据范围。 |
| 描述统计 | 行数、非 NULL 数、sum、mean、min、max；空值参与规则与下述断言一致，不沿用浏览器 Worker 的隐式转换。 |
| 图表 | 柱状图、折线图、散点图；字段切换、过滤和聚合变化重新请求计算；图表所需分组与统计都生成 SQLite 原生 SQL。 |
| 时间字段 | 允许规范 UTC ISO 8601 时间文本作为折线图横轴，字段类型由用户明确选择；格式校验失败时保持字符串。首版关闭自动时间分桶、格式猜测与派生时间维度。 |
| 明细与限制 | 散点图使用有界投影结果；超限要求增加过滤或显式选择受限明细模式，并在图上标示行数和限制，不把截断结果标为全量。 |
| 类型 | 字符串、布尔、可无损呈现的数值和时间进入对应图表。超出 JavaScript 安全数值范围的整数/Decimal 保留精确信息，禁止静默转成错误数值；首版拒绝不支持的定量绘图。 |

服务端按支持集合校验实际 workflow，并向 UI 返回能力；不支持的操作应禁用或返回明确错误，不自动切换到本地聚合。
`COUNT(*)` 与 `COUNT(field)` 明确区分；`mean([10, NULL, 20]) = 15`；全 NULL 的 sum/mean/min/max 为 NULL。
NULL 与空字符串为不同分组；多字段分组按元组处理，不靠分隔符拼接键。数值均值避免整数除法，SQLite 实际结果与原生 SQL 基准比较。
数据库排序规则随源库，首版不重写文本 collation，在数据源元信息中说明；SQLite 混合存储类型不得仅凭声明类型错误强制转换。

## 最小 HTTP 与数据契约

探索服务 API 入口为 `/api/visualization/v1`，独立工作区直接访问自身 API，不覆盖已有 `/channels/database` 等管理路由。
冻结以下线协议语义；实现时对应类型与公共声明先于业务代码落地。现有数据库接口不足时使用独立版本接口，不向旧虚表追加方法。

```typescript
interface SourceV1 {
  source_id: string;                 // 服务端解析到具名通道及单表，不接受客户端物理路径
  kind: "database";
  dialect: "sqlite";                // 后续多库 Feature 扩展，不提前声明已支持
  label: string;
  schema_revision: string;           // 字段映射的版本，不是数据快照版本
  fields: FieldV1[];
  capabilities: string[];
  workflow_version: string;          // 与锁定的 Graphic Walker 版本及适配器一致
  semantics: { timezone: "UTC"; collation: string };
}
interface FieldV1 {
  field_id: string;
  label: string;
  type: "string" | "boolean" | "number" | "temporal" | "unsupported";
  native_type: string;
  nullable: boolean;
}
interface ComputeRequestV1 {
  request_id: string;                // 客户端生成 UUID；服务端拒绝活动身份重复
  source_id: string;
  schema_revision: string;
  workflow_version: string;
  workflow: unknown;                 // 锁定版本的 GW payload；按首版操作集合严格解析
  detail_mode: "complete" | "limited";
}
interface ComputeResponseV1 {
  request_id: string;
  fields: FieldV1[];                 // 含聚合产生的输出字段
  rows: Record<string, string | number | boolean | null>[];
  execution: "database";
  scope: "complete" | "top_n" | "limited_detail";
  returned_rows: number;
  elapsed_ms: number;
}
interface QueryErrorV1 {
  request_id: string;
  code: string;
  message: string;                   // 脱敏，可供 UI 直接显示
}
```

| 方法与路径 | 契约 |
| --- | --- |
| `GET /capabilities` | 返回服务支持的 `api_version` 与 workflow 版本；独立前端在开始查询前核对兼容性，不以 UI 构建身份代替 API 兼容声明。 |
| `GET /sources` | 返回已配置、可探索的表描述列表；字段信息可按源惰性获取，失败明确归属通道。 |
| `GET /sources/{source_id}` | 返回 `SourceV1`，字段 ID 仅映射该源的已发现字段，能力与服务端校验规则一致。 |
| `POST /compute` | 接收 `ComputeRequestV1`，一次请求返回有界结果或错误；无需持久作业、消息队列或额外元数据库。 |
| `POST /cancel` | 请求体含 `request_id`；响应状态为 `cancel_requested` 或 `not_running`，前者不表示数据库已停止；原计算以取消结果或已先完成的结果结束。 |

GW 适配层将 `rows` 转成组件要求的返回值；排序、绘制和格式化不得暗中引入统计计算。同一视图中被替代的旧请求不得覆盖新图，组件的并行子查询分别跟踪。
错误码至少覆盖 `INVALID_WORKFLOW`、`DUPLICATE_REQUEST`、`UNSUPPORTED_OPERATION`、`UNSUPPORTED_TYPE`、`SOURCE_UNAVAILABLE`、`SCHEMA_CHANGED`、`RESULT_LIMIT`、`BUSY`、`TIMEOUT`、`CANCELLED`、`DATABASE_ERROR`。
字段/表标识符来自服务端映射并按方言引用，过滤值使用绑定参数；客户端不提交任意 SQL。拒绝未知操作、跨表引用和非只读语句。
schema_revision 不匹配时要求刷新元信息；数据库执行中发生 Schema 变化返回错误，不用旧字段映射猜测结果。

### 资源与生命周期保证

- 初始默认上限：每请求 10,000 行、8 MiB 序列化结果、30 秒计算期限，每进程 2 个活动查询；均由服务端配置，客户端不可抬高，忙时直接拒绝，不建设排队系统。
- 聚合扫描整个过滤范围后再检查输出规模，禁止先 `LIMIT` 原表再声称全量聚合。聚合结果超限返回 `RESULT_LIMIT`；显式 Top-N 则返回对应 scope。
- 取消/超时须传递到实际执行会话并在有界时间内释放资源，不能只停止 HTTP 等待。先停止接收、取消并等待查询结束，再释放会话、通道租约及插件库。
- 每次查询独占可取消会话；取消不影响其他请求。SQLite 复用已有只读会话能力并实际验证；其他数据库的取消保证属于后续兼容 Feature。
- 默认仅本机监听；远程访问复用部署侧访问控制。数据库读权限与原有管理职责分离，不在本 Feature 建设账号系统。

## 两条主链路

1. 进入/选择：可视化进程批次 Option → Load → Start，提供独立静态包及 API；Web 顶栏 → `/explore` → 同源 iframe → ready/context 握手 → 选 SQLite 表。独立访问 `/exploration/` 跳过握手，使用同一计算路径。
2. 计算/更新：拖放字段或修改过滤 → 外部 computation 提交版本化 workflow → 校验源、字段、操作和预算 → 编译原生 SQL → 租约内执行 → 返回有界结果 → 仅最新请求更新图表；切换/取消沿同一请求身份终止旧计算。

## 实现收敛点与证据边界

- 使用开源 Graphic Walker 与通用 React/HTTP/SQL；版本及许可证写入构建锁定记录。官方参考：[组件](https://github.com/Kanaries/graphic-walker)、[外部计算](https://github.com/Kanaries/graphic-walker/blob/main/computation.md)、[DSL 转 SQL](https://github.com/Kanaries/gw-dsl-parser)。
- T2 用真实浏览器产生的 workflow 验证组件契约及 SQLite SQL 路径；先评估现成 DSL 转换器对本 Feature 的覆盖、运行依赖和许可证，再确定复用或实现受限转换，不预设其可直接嵌入 C++。
- 该技术选择限于 T2，不能引入额外常驻运行时、原表本地聚合或云服务依赖；若约束内无法跑通，记录具体阻塞并复核规格，不扩大框架选型调研。
- 已核对现有 Web/Vite/HTTP/发布源码以及 Graphic Walker 0.5.3 发布包的主题、语言、外部 computation 能力；架构未发现必须新增常驻运行时的阻塞。浏览器夹具因本机监听 EPERM 未执行，提权被自动审批拒绝；未证明真实 iframe 或 Graphic Walker 运行通过。
- 已有通道预览接口不作为统计查询契约；本规格中的限额、能力和结果语义均是待实现与测试的目标。

## Feature Tasks 与验收锚点

- [x] **T1 — 用户可在现有 Web 内容区打开探索工作区，也能独立访问，保持一致的主题与会话体验。**
  交付顶栏入口、同源 iframe、受限资源/API 代理和独立 Graphic Walker 静态包；冻结桥接/配置/版本契约，提供独立进程启动。
  验收锚点：真实浏览器中模块/CSS/Worker/接口路径正确、主题同步、切换菜单状态保留且显隐通知正确；未配置/不可达/不兼容可见；探索 404 不变成主 Web 页面；关闭 Scheduler、Worker、原 Web 后仍可直接访问。
- [x] **T2 — 用户可选择 SQLite 表并通过真实图表交互获得原生 SQL 计算结果，建立可验证的探索闭环。**
  冻结并实现数据契约/适配器；选表、过滤、分组、基础聚合及三类图表接通外部 computation，能力集合与查询校验一致。
  验收锚点：真实 workflow/SQL/结果对应；NULL 均值为 15，全 NULL 聚合、复合分组键、SQLite 混合类型行为正确；原表不传给浏览器聚合；不支持分箱/时间桶时明确禁用。
- [x] **T3 — 连续探索与耗时查询具有明确的结果范围和资源边界，保证图表可信及 SQLite 会话可回收。**
  落实 Top-N/受限明细标识、精度保护、过期响应隔离、只读配置与查询、期限/并发/行字节上限、取消和停机顺序。
  验收锚点：超限不生成假全量图表；慢查询真实取消或超时，隐藏工作区可停止新查询并取消旧查询；第二请求不被误取消；停止时无悬空插件产物且配置未改；精确回收预算在首个 T3 切片冻结。
- [x] **T4 — 用户能按说明部署并复现完整 SQLite 探索流程，使首个 Feature 可独立交付。**
  提供原生/现有镜像体系中的启动配置、独立静态包及升级说明；记录同环境 RSS、启动时间、前端体积与测量口径。
  验收锚点：主 Web 产物摘要保持不变时升级兼容的探索静态包，内嵌/独立访问均正常，不兼容包明确拒绝；实际产物完成选表、三类图表及错误展示；构建、格式、SQLite/浏览器及受影响范围的回归全部通过。

## 测试与实施约束

- 契约/编译/结果语义使用 GTest 或适用的前端单测；真实浏览器检验 Graphic Walker 接线，不能以 mock computation 代替端到端结果。
- 本 Feature 定向测试使用隔离 SQLite 文件与真实浏览器；在 [依赖清单](../../config/testing/dependencies.json) 声明运行位置和资源锁，按 [测试说明](../../docs/testing.md) 检查所需依赖。不纳入无关 Kafka、其他数据库或镜像启动矩阵。
- 测试命令、允许文件、具体依赖版本及非秘密证据路径在每次 [工作台](../active_task.md) 冻结。Feature 完成验证探索全部能力，并按改动依据选择 SQLite/共享数据库契约、Web、部署配置和包完整性的回归；使用标准构建和带筛选的统一测试入口，不要求完整 CTest 或不带筛选的 `all`。扩大范围须先说明实际影响并更新工作台，新增测试进入统一入口。
- 必需清单须 selected=executed=passed，failed=skipped=not_executed=0；外部依赖缺失、尚未支持的必需能力和未运行浏览器均不能计为通过。
- 2026-10-20 用户授权顺序实施全部 Task；每个 Task 验收并标记后进入下一项。文档检查不替代运行时验收。
- 用户补充验收边界：不要求每个 Feature 进行镜像启动验证。本 Feature 交付原生及镜像体系配置，镜像部分验证打包内容、发布完整性及 Compose 静态配置；运行时通过实际原生探索进程、主 Web 和真实浏览器验收，不要求构建/启动新镜像。
- 2026-10-20 用户要求立即纠正验收范围扩大规则；上述受影响范围覆盖此前完整 CTest/全量回归要求，保留原失败与中断记录，不计为通过，也不作为无关完成门槛。

## 完成证据

2026-10-20：先完成 Feature 粒度修订，再按用户保留独立升级/访问的要求收口为 iframe 方案。规划记录位于 `evidence/visualization-database/planning/`、`planning-boundary-review/` 和 `iframe-feasibility/`（均 Git 忽略）；后者保存源码核查、隔离夹具及环境拒绝记录。浏览器验证未执行，尚无产品实现或运行时完成证据。

### T1 完成（2026-10-20）

- 独立 `.so`、React/Graphic Walker 0.5.3 静态包及独立启动配置已构建；现有 Vue 顶栏与单实例 iframe 接通，Web 仅代理规定前缀，HEAD/查询参数/缓存/状态码保持，探索 404 不回退主 SPA。
- 版本化桥接、主题与显隐通知、菜单切换保留 DOM、未配置/不可达/不兼容错误，以及主 Web 停止后独立访问均经实际产物验证。静态包含构建身份、兼容版本和 HTML/JS/CSS SHA256；组件的外部样式和品牌图改用本地/包内原始资源。
- `cmake -B build src` 与 `cmake --build build --target flowsql flowsql_web flowsql_visualization -j4` 成功。最终 CTest 2/2（含环境 fixture），浏览器 9/9，failed=skipped=not_executed=0；新增 C++ 全文件格式与既有文件改动行格式、git diff --check 通过。
- 本地证据：`evidence/visualization-database/t1/ctest.xml`、`browser.json`、`dependency-install.json`；失败轮保留，不计成功。前置通用夹具曾输出 9 项 PASS 后清理挂起，已精确终止其进程；它不作为最终验收依据。
- 边界：此处验收入口；尚无 SQLite 图表计算、T3 资源边界或全量回归完成结论。未提交或推送。

### T2 完成（2026-10-20）

- 新增独立版本的 SQLite 只读查询会话能力，保留每个值的真实存储类型；不改变既有虚表。来源经工厂/租约发现单表并返回服务器字段映射、Schema 摘要和能力；绑定值的受限编译器处理过滤、组合、投影、元组分组、基础聚合和排序。
- 真实 Graphic Walker 0.5.3 外部 computation 已接通选表、字段/图表/聚合切换与过滤；未知操作、分箱/时间桶和表达式明确返回错误，无浏览器原表聚合。当前 npm DSL 转换包是 JS/WASM 等运行形态，未引入 C++ 常驻转换运行时，采用本 Feature 受限 C++ 编译。
- 完整 T1+T2 定向 CTest 5/5，浏览器入口 9/9、真实计算 6/6，失败/跳过/未执行均 0。真实柱状/折线/散点/过滤 workflow 结果逐行等于原生 SQL；原生测试验证 NULL 均值 15、全 NULL 聚合、NULL/空串和复合键、混合类型保留、Top-N 与只读/绑定边界。
- 构建、格式与 diff 检查通过。本地证据：`evidence/visualization-database/t2/ctest.xml`、`api.json`、`browser.json`（含真实 workflows、结果和 SQL 对照）；浏览器测试选择器错误的失败轮不计通过。
- 边界：T3 期限/取消/并发/精度/过期响应及 T4 独立升级、部署与全量回归尚待验收；未提交/推送。

### T3 完成（2026-10-20）

- Top-N/受限明细范围、行/序列化字节预算、逐值精度与混合类型保护已落实；规范 UTC 时间横轴须用户明确选择，前端并行子查询分别跟踪，视图替代/隐藏取消且旧状态不覆盖新视图。
- 全请求期限传递到独占 SQLite 会话，取消与停机在锁外销毁会话/租约；默认 2 会话，可配置 1～8（保留现有连接池余量），HTTP 保留取消处理线程。真实取消、20ms 期限、并发隔离、SIGTERM 回收均在冻结的 2 秒预算内，原数据库与配置摘要不变。
- 最后构建明确退出 0 后顺序执行定向 CTest 8/8，通过/执行/选中一致，失败/跳过/未执行均 0；客户端代次与并行生命周期断言通过。证据：`evidence/visualization-database/t3/ctest.xml`、`limits.json`；此前构建/测试重叠造成 invalid ELF 的失败轮单独保留，不计成功。
- T3 已收口；T4 部署、独立升级、测量与完整回归尚待验收。未提交或推送。

### T4 完成（2026-10-20）

- 原生启动配置随标准产物 staging；Dockerfile/上下文包含探索包与双许可证，可选 Compose 提供独立服务、只读配置/SQLite 挂载及主 Web 上游。实际原生进程独立运行，无需 Scheduler/Gateway/Worker。
- 实际重建兼容的 0.1.1 临时静态包并升级，保留旧哈希资源；主 Web PID、主程序/Web 插件/Vue 产物摘要保持不变。内嵌与独立入口均完成三类图表、过滤及错误展示，不兼容包均明确拒绝，配置和源数据库摘要不变。
- 标准 CMake 配置成功，完整构建最终退出 0；原生交付检查 8/8，内嵌/独立升级浏览器各 7/7。启动/RSS/前端体积的同机单次测量口径和限制见 [用户说明](../../docs/exploration.md)。
- 按用户修订后的边界，统一入口选中=执行=通过 22，failed=skipped=not_executed=0，覆盖全部探索专项及受影响 SQLite/共享数据库消费者、Web/Vue、部署配置和发布打包回归；源码与产物在验收前后保持一致。产物身份、Compose 静态配置、新 C++/既有改动行格式及 git diff --check 通过。
- 本地证据（Git 忽略）：`evidence/test-environment/20261020T092520Z-2b44e334/summary.json`、`evidence/visualization-database/t4/final-acceptance.json`、`delivery.json`、`embedded-browser.json`、`standalone-browser.json`、`measurements.json`、`compose-final.json`、`build-checkpoint.json`。
- 覆盖边界：无关 Kafka/其他数据库矩阵及产品镜像启动不纳入本 Feature 门槛；原全量回归的环境失败与中断保留，不计成功。先前本地镜像构建只证明构建成功，未启动该镜像；未提交、推送或正式发布。
