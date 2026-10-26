# Feature: DataFrame 启动恢复优化

状态：`[x]` 已完成；2026-10-26 T1→T2→T3 顺序验收通过并归档。
优先级：P1
前置能力：`operator-catalog`、`visualization-dataframe`（均已交付）。

## 业务意图

对保存了大量 CSV DataFrame 的单进程及 Scheduler 部署，服务启动和目录查询不再等待全部数据解析，
用户可以先访问服务、查看通道和探索来源，再在预览、SQL 读取或获取探索快照时加载所选 DataFrame。
启动成本只包含文件发现与目录登记，不随 CSV 总内容量执行全量类型推断和内存物化。

实施前核查：原 `CatalogPlugin::Start()` 串行调用 `LoadCsvFile()` 恢复全部 CSV，且插件 Start 批次依次执行，
因此后续 Database、Scheduler 和 Gateway 的启动会被阻塞；`HandleListChannels()` 也读取每个通道的数据。
仅缩短类型推断循环不能消除全量恢复随数据增长的成本。

2026-10-23 登记时隔离测量：23 个 CSV、273692299 bytes，Catalog Start 约 12.86 秒；空目录约 0.00002 秒
（完整探针进程启动分别约 12.98 秒、0.084 秒）。该数据仅是问题依据，不是本 Feature 的验收结果或性能承诺。
原始记录在本地 `evidence/startup-investigation/diagnosis.json`，未入库；实施时需在相同环境重新建立对照。

## Non-Goals

- 不更换 CSV 持久化格式，不修改现有类型推断和数据读写业务语义。
- 不引入后台全量预热、自动淘汰、内存缓存容量治理或分布式 DataFrame。
- 不以并行全量加载、增加超时或调整插件生命周期顺序替代按需加载。
- 不改变控制面 HTTP、数据面 Arrow IPC / 共享内存，以及公共通道和快照接口 ABI。
- 不迁移、删除或修改已有业务 CSV，不自动重启用户服务，不扩展外部数据库/Kafka/Docker 验收矩阵。

## 核心数据与接口契约

复用 `IChannelRegistry`、`IDataFrameChannel`、`IAppendableDataFrameChannel` 和 DataFrame Snapshot V1，
优先在 Catalog 内部实现持久化通道的加载状态，不新增跨插件纯虚方法。

| 数据或操作 | 预期契约 |
| --- | --- |
| 持久化通道目录项 | 保存名称、CSV 路径、source_id 和加载状态；启动只发现文件及登记目录，不解析 CSV 内容。 |
| 加载状态 | 至少区分未加载、加载中、已加载；加载失败必须可诊断，不伪装为空 DataFrame，重试策略在实现前冻结。 |
| 名称查询、通道列表、探索来源列表 | 不物化全部数据；列表中的未知行数/Schema 有明确表示，不把未加载显示为零行；兼顾 Scheduler 通用目录的 Schema 查询路径。 |
| 首次数据访问 | 预览、SQL 读取、追加和探索快照只加载目标 DataFrame；同一通道并发首次访问共享一次成功加载结果。 |
| 替换写入 | 保持替换语义，不为覆盖旧结果而先解析旧 CSV；追加必须保留原数据及 Schema 校验。 |
| 并发与生命周期 | CSV 解析不占用目录全局锁；重命名、删除、关闭、停止与在途加载协调，不死锁、不复活已删除目录项，不引用已销毁的 Catalog。 |
| 探索快照 | 保留 source_id、不可变快照、取消、截止时间、大小预算及所有权保证；加载等待与解析纳入请求预算。 |
| 未加载通道管理 | 重命名更新实际文件与后续加载位置并保留 source_id；删除不触发解析；重启后按现有持久化规则恢复。 |

内部 `LazyDataFrameChannel` 组合内存通道，持有路径、名称、状态、错误、加载代次、互斥量与条件变量；
加载器为不捕获 Catalog 的静态函数。公共头文件与虚表不变。
HTTP 通道列表新增 `load_state` 和 `load_error`；未物化时 `rows/schema` 为 JSON null，已加载保持原格式。
`Schema()` 未加载时返回 JSON `null`，不触发推断。失败记录为 failed，同批等待者收到失败，后续新请求可重试；
取消/超时回到 unloaded，不发布半成品。关闭清空缓存，重开按持久化 CSV 重新加载；删除/停止使旧句柄永久失效。
同一通道最多一个解析者；替换写入使在途旧加载失效。重命名保留 source_id 和在途读取的文件所有权。
快照请求的等待、CSV 扫描、推断和物化共用截止时间及取消信号，成功后沿用既有大小预算和不可变复制保证。
有限评审结论：保持公共 ABI、不在目录锁内解析、不捕获 Catalog；并发状态及退役路径通过 T2/T3 测试收口。

## 主链路

1. **启动与目录查询**：枚举持久化 CSV → 登记未加载通道和探索来源 → 后续插件继续启动并开放服务；
   用户查询通道或来源目录时仅返回已知元信息，未加载状态明确可见。
2. **首次访问与复用**：用户预览、运行 SQL 或申请快照 → 按目标通道协调一次加载 → 按既有规则解析并发布数据 →
   完成当前读取或快照；后续访问复用已加载结果，管理操作与请求取消遵循同一生命周期契约。

## Feature Task

- [x] T1：交付无需全量解析数据的启动恢复和目录查询，使服务及持久化 DataFrame 名称先于数据加载可用。
- [x] T2：交付并发安全的按需数据访问与快照生命周期，使首次读取、后续复用及失败/取消行为保持可解释且数据一致。
- [x] T3：交付完整的通道管理与用户状态展示，并通过聚焦回归和隔离性能对照证明多 DataFrame 场景的可用性与启动收益。

## Feature 验收边界

- 确定性测试证明启动、名称查询、通道列表及探索来源列表不预读 CSV 内容；不只依赖墙钟耗时断言。
- 覆盖并发首次读取、单次成功加载复用、数值/字符串 Schema、空表、加载失败，以及替换/追加行为。
- 覆盖未加载及加载中的重命名/删除、关闭/重开、停止和持有快照的数据所有权；取消、超时不发布半成品。
- 覆盖经默认主产物的恢复 → SQL/预览 → 探索快照 → 绘图路径；用户看到明确的未加载状态和加载失败反馈。
- 对空目录、固定文件数不同总字节量、固定单文件大小不同文件数进行隔离启动对照；记录计时口径、环境、
  文件数、总字节量及峰值内存，并单独记录首次访问代价，不能只证明等待被转移到目录查询。
- 实施工作台按实际影响冻结 CMake target、`tools/test.sh` 筛选用例与必要 fixture，核对
  selected=executed=passed 且 failed=skipped=not_executed=0；不默认跑全量或无关部署矩阵。

## 完成证据

- 2026-10-23：按用户“先不实现代码，先登记 Feature”的要求登记；T1～T3 均未开始，无实现或验收结论。

- 2026-10-26 T1：启动只登记延迟通道，列表明确 null 元信息；主产物及测试构建通过。`test_catalog_lazy_loading` 1/1 通过，IN_OPEN/IN_ACCESS 断言目录路径零 CSV 读取，首次目标读取保留 INT64/DOUBLE/STRING。证据：`evidence/test-environment/20261026T044027Z-7d4e78b6/summary.json`，未入库。

- 2026-10-26 T2：锁外解析、同批结果共享、独立等待预算与解析取消/期限、失败重试、替换失效、关闭/重开及停止退役完成。确定性 Gate 与真实 CSV/Schema/空表断言通过。主构建成功，标准原生入口 selected=executed=passed=3，failed=skipped=not_executed=0：`evidence/test-environment/20261026T044801Z-0730e4b1/summary.json`。首次沙箱运行仅共享快照用例本地监听失败，外层同清单复测全绿，未放宽断言。

- 2026-10-26 T3：未加载与在途 rename/delete、source_id、关闭/重开/停止退役及快照所有权验收完成；真实主产物覆盖恢复后的 SQL/预览、不可读旧 CSV 的替换、快照和 standalone/embedded 绘图。主通道页真实 Chromium 验证未加载、已加载、失败及未知元信息。
- T3 首轮 11 项完整执行，主通道页因共享 API 客户端硬编码 localhost:8081 失败；定位后仅将基址改为同源，保持端点/方法/数据契约，按原清单重建主 Web 并复测，未放宽断言。工作台 Slice 1 以“当前错误待修复”关闭，Slice 2 沿用 T3 完成。
- Feature 最终清单：selected=executed=passed=11（7 业务 + 4 必要环境 fixture），failed=skipped=not_executed=0；`evidence/test-environment/20261026T052611Z-c44b5f03/summary.json`。无外部 DB/Kafka/Docker，未运行全量/发布/镜像矩阵。
- 隔离性能：空目录、4×约64 KiB、4×约2 MiB、1×约2 MiB、8×约2 MiB，每组旧/新各三次独立进程；默认无优化构建、热缓存、无并行构建/验收。8 文件 16777320 bytes 的 Start 中位数 1473.99→27.379 ms，列表 0.099→0.186 ms，目标首次 Read 0.012→201.137 ms，启动/列表峰值 RSS 66920→23828 KiB。完整环境、原始记录及源码/主产物 SHA256 在 `evidence/dataframe-lazy-loading/performance.json`、`verification.json`，本地忽略不入库；使用说明与限制见 `docs/exploration.md`。
- 主 Catalog/Scheduler/Web/visualization、两套前端与相关测试构建成功；格式、Python/JS 语法及变更范围审查通过。沙箱内本地监听/esbuild EPERM 按原范围在外层执行通过；未重启用户服务，未提交或推送。
