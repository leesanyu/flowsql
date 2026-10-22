# Feature：DataFrame 探索可视化

标识：`visualization-dataframe`；优先级：P1；状态：`[x]` 已完成，T1～T5 依次验收并归档。
前置：[基础探索](../archive/feat-visualization-database.md)、[工作页体验](../archive/feat-visualization-experience.md)、
[图表与直方图](../archive/feat-visualization-chart-authoring.md)、[时间统计](../archive/feat-visualization-statistics.md)。

## 业务意图

让需要查看 FlowSQL 中间结果、CSV 导入结果或算子结果的用户，在现有数据探索工作区选择命名 DataFrame，
用过滤、分组、统计和图表观察趋势、分布及变量关系；同一工作页的计算绑定同一份数据，生产任务结束或源通道被删除后，
已取得的快照在有效期内仍可使用。用户主动获取新快照后才切换数据版本，界面明确采集时间、行数和结果范围。

## Non-Goals

- 不读取任务内部尚未注册的临时对象，不消费 stream/block stream，不实时订阅、自动追随写入或推断任务最终完成。
- 不增加跨源 Join、任意 SQL/表达式、地图、分位数/箱线图、窗口统计、数据写回或 DataFrame 编辑能力。
- 不交付跨刷新/重启的快照或图表持久化、共享仪表盘及多用户权限体系。
- 首版交付同机原生多进程路径；不支持跨主机 DataFrame 传输，不扩展 Docker 共享挂载/容器部署矩阵。
- 不通过预览分页、HTTP JSON 或浏览器下载整份 DataFrame 后统计，不引入 DuckDB 或新的 Python/Node 常驻服务。

## 架构与首版范围

当前 `IDataFrameChannel::Read` 提供单次快照读取，Catalog 持有具名通道；现有探索服务仅发现数据库来源。
本 Feature 补齐可复用的快照身份、拥有型读取及跨进程交付，不将既有预览 API 当作版本化数据源。

| 边界 | 冻结契约 |
| --- | --- |
| 来源 | 从拥有通道的 Catalog 发现已注册且可读取的 `dataframe.<name>`，包含 CSV 导入和命名任务输出；发现不搬运数据。 |
| 服务责任 | Catalog 提供快照能力及 HTTP 控制路由；独立 visualization 进程接收快照并执行 C++ Arrow 内存计算；浏览器沿用外部 computation。 |
| 进程内调用 | 经 `IQuerier` 与新 IID 取得快照 provider；保持既有 `IDataFrameChannel`、`IChannelRegistry` 和数据库探索接口虚表不变。 |
| 跨进程数据面 | HTTP 只交换元信息、租约与取消；Arrow IPC 文件位于两进程可访问的受管共享内存目录，由 visualization 只读映射。 |
| 配置 | visualization 可选 `dataframe.upstream` 与 `dataframe.shared_dir`，来源服务配置同一共享目录；由部署者配置，浏览器不能提交路径或上游地址。 |
| 独立运行 | 数据库路径保持原生 SQL；未启用 DataFrame 或其上游失联时数据库仍可用，来源列表分别显示不可用原因。DataFrame 计算不依赖 Python Worker。 |
| 计算 | 投影、比较/集合/NULL 过滤及 AND/OR、多字段分组、count/sum/mean/min/max/distinctCount、排序/Top-N、等宽直方图、日/月时间桶。 |
| 图表 | 复用柱状、堆叠柱状、分组柱状、折线、面积、散点、圆点、饼图、热力、表格、突出显示表格、直方图；DataFrame 的箱线图入口禁用并说明原因。 |

### 快照身份与所有权

- `source_id` 为不透明身份，绑定来源进程启动代次及通道注册实例；重命名保留身份，删除后同名重建、进程重启产生新身份。
- `snapshot_id` 是一次成功采集的不可变数据版本；即使内容相同，重新采集也可产生新 ID，不把它解释为源写入序号。
  `schema_revision` 只标识字段顺序、名称、Arrow 类型、可空性及相关时间元数据；不能代替 `snapshot_id`。
- Acquire 在通道读锁内取得一致的批次及 Schema，锁外有界导出；不得分别读取行数/Schema/数据再拼成快照。
  源通道和插件租约覆盖读取与导出；所有 Arrow buffer 必须有真实所有者，借用内存须在有效租约内复制到预算内的拥有型存储。
  发布后生产者不得原地改写 buffer；无法保证稳定读取或内存有效期的来源明确拒绝，不能只凭 RecordBatch 的 shared_ptr 判定安全。
- 发布给读取方的批次不可变；源 Write/Append/Close、注销及任务资源释放不能修改或释放已取得的数据。
  有 Schema 的零行数据可探索；无 Schema、通道不可读或注册实例已变化明确失败，不伪造空数据成功。
- 首版允许捕获正在追加的命名 DataFrame 的某个已发布前缀。`row_count` 表示该快照全部行数；
  `scope=complete` 只表示在该快照过滤范围上完整计算，不表示生产任务已结束，也不补齐后来写入的数据。

### 核心结构与接口

下列为待实施的数据契约；T1 先将其落入公共头文件并用测试锚定，再接入生产路径。

```cpp
struct DataFrameSourceV1 {
    std::string source_id, label;  // 不暴露物理路径；source_id 含进程代次及注册实例身份
};
struct DataFrameSnapshotV1 {
    std::string source_id, snapshot_id, schema_revision, captured_at;  // captured_at: UTC ISO 8601
    uint64_t row_count = 0, retained_bytes = 0;
    std::shared_ptr<void> lifetime;  // 持有必要的源/插件租约，声明在 batch 前以保证后析构
    std::shared_ptr<arrow::RecordBatch> batch;  // 只读使用；Schema 与全部 buffer 有拥有型生命周期
};
struct DataFrameSnapshotRequestV1 {
    std::string source_id;
    uint64_t max_bytes = 0;
    std::chrono::steady_clock::time_point deadline;
    std::shared_ptr<std::atomic_bool> cancelled;
};
interface IDataFrameSnapshotProviderV1 {
    virtual ~IDataFrameSnapshotProviderV1() = default;
    virtual int List(std::vector<DataFrameSourceV1>* output, std::string* error) = 0;
    virtual int Acquire(const DataFrameSnapshotRequestV1& request,
                        DataFrameSnapshotV1* output, std::string* error) = 0;
};
```

接口失败清空 output；快照是拥有型值，释放最后一个所有者才释放批次，不新增裸指针 Release 配对要求。
持有 batch 或派生结果时必须同时保留 lifetime；provider 调用方另保留 provider 插件租约。
退出时先销毁快照、计算结果和会话，再释放相关动态库；独立 IPC 映射建立后不再依赖源插件。

```ts
interface DataFrameSnapshotInfoV1 {
  source_id: string; snapshot_id: string; schema_revision: string;
  captured_at: string; row_count: number; ipc_bytes: number; expires_at: string;
  fields: FieldV1[]; capabilities: string[]; time_bucket_fields: string[];
}
// SourceV1.kind 增加 "dataframe"；DataFrame 不伪装成数据库 dialect。
// ComputeRequestV1 在 DataFrame 路径必须携带 snapshot_id；数据库请求保持兼容。
// DataFrame ComputeResponseV1 回显 source_id / snapshot_id / schema_revision，execution="dataframe"。
// 沿用 fields / rows / scope / returned_rows / elapsed_ms / numeric_display。
```

### HTTP 与 Arrow IPC 交接

浏览器沿用 `/api/visualization/v1/`；来源元信息按 `kind` 声明能力，数据字段以成功取得的快照为准。

| 方法与相对路径 | 契约 |
| --- | --- |
| `GET sources`、`GET sources/{source_id}` | 合并数据库与 DataFrame 发现；DataFrame 描述只包含身份、标签和可用状态，不暗中创建快照。 |
| `POST snapshots/acquire` | `{request_id,source_id}`；取得并验证完整 IPC 后返回 `DataFrameSnapshotInfoV1`，未就绪的传输对象不返回给浏览器。 |
| `POST snapshots/renew` | `{snapshot_id}`；续期已有快照，返回新的 expires_at，数据版本不变；过期后不得按名称偷偷重建。 |
| `POST snapshots/release` | `{snapshot_id}`；幂等释放，取消关联请求并拒绝新计算，待在途读取结束后回收映射和预算。 |
| `POST compute`、`POST cancel` | 沿用 workflow 与 request_id；cancel 同样覆盖 acquire，期限从受理开始覆盖导出、打开、计算与结果编码。 |

- 来源服务在 `/channels/dataframe/exploration/v1/` 提供发现、申请、取消和释放控制路由；由配置好的服务端调用，
  不加入浏览器公开代理路径，沿用部署侧访问控制。申请携带 source_id、request_id 及剩余期限/字节预算，不能在下游重置总期限。
  交接描述含 `transfer_id/source_id/snapshot_id/schema_revision/row_count/ipc_bytes/expires_at`，不含数据行。
- 来源服务原子发布完整、只读的 Arrow IPC；接收端用受管目录和不透明 transfer_id 定位，校验长度、Schema 和行数。
  拒绝目录穿越、符号链接及未完成对象；只在全部验证后发布可计算快照，导出失败不残留半成品。
- 接收端建立独立有效的只读映射后确认释放传输租约；来源端移除目录项不会使接收端映射失效。
  传输租约默认 60 秒；失联、取消、过期及进程退出回收所属对象，重启只清理可证明归属的过期对象。
- 接收成功后原任务、Catalog 或上游连接结束不影响现有快照；visualization 重启会使快照失效，返回 `SNAPSHOT_EXPIRED`。

### 计算与类型语义

- 在不可变 Arrow 批次上执行受限 workflow，复用已有 Arrow/compute 依赖；字段通过服务端映射绑定，拒绝未知操作和跨源字段。
  过滤后扫描全部符合条件的快照行，再分组/聚合/排序并应用结果限制；不能对预览页或先截断的输入声称完整统计。
- NULL 与空串、不同分组键元组保持区别；count(*) 计行，count(field) 和 distinctCount 排除 NULL；
  mean([10,NULL,20])=15，空集/全 NULL 的 sum/mean/min/max 为 NULL，计数为 0，分组空集返回零组。
- 支持 Arrow 整数、浮点、布尔、UTF-8、Decimal 和 date/timestamp；二进制及嵌套/复合字段标为 unsupported，
  可探索其余字段。计数/去重允许文本、布尔及数值；sum/mean 只接受数值，不把字符串隐式转成数字。
- 文本按 UTF-8 字节精确比较，区分大小写；NULL 排序固定在最后。无排序明细保持快照行序，排序并列保留原顺序；
  分组输出默认按分组键排序，Top-N 并列以分组键或原行序消歧，确保同一快照重复查询可复现。
- 过滤、分组和精确去重使用原始类型。大于安全量级 ±9007199254740991 的整数及非有限数值拒绝定量绘图，
  聚合不得先浮点转换以绕过保护；Decimal 沿用精确文本/近似绘图元信息，非零下溢为零须失败。
  Decimal 均值若由内核返回浮点，标记 `origin="arrow_decimal_mean"` 与 approximate，不伪造精确文本。
- 直方图沿用 1～100 箱、过滤后 min/max、左闭右开/末箱含最大值、常量一个箱、空集/全 NULL 零行及只返回非空箱。
  箱边界与频数在服务端计算；Decimal 边界近似须声明，数值溢出明确失败。
- 日/月桶仅对 Arrow date32/date64/timestamp 开放，输出 YYYY-MM-DD / YYYY-MM-01；按 timestamp 声明时区的日历分桶，
  无时区 timestamp 保留无时区日历值，不使用宿主时区。NULL 保留 NULL 桶，不补缺失日期。
  文本默认分类，可沿用用户显式选择的规范 UTC 时间轴；不猜文本格式或整数 epoch 单位。
- 聚合超行数/字节预算明确失败；显式 Top-N 标 `top_n`，受限明细标 `limited_detail`，其余标 `complete`。
  沿用现有错误结构，新增 `SNAPSHOT_EXPIRED`、`SNAPSHOT_MISMATCH`、`SNAPSHOT_LIMIT`、`DATA_PLANE_UNAVAILABLE`。

### 预算、取消与工作页

- 服务端默认单快照最多 256 MiB（保留的 Arrow buffer 和 IPC 表示均受限），每进程快照/待交接合计 512 MiB，
  最多 8 个快照、2 个同时采集；采集前预留预算，编码中持续计量，不能先无界复制再报超限。
- 单计算额外工作内存默认 128 MiB，分组/排序/去重状态纳入预算；结果沿用 10000 行、8 MiB、30 秒与 2 个活动查询。
  输入、传输、中间结果及输出有各自账目；预算是受管数据分配上限，不宣称操作系统 RSS 硬上限。繁忙直接拒绝，不排无界队列。
- 快照闲置有效期默认 15 分钟；成功计算或显式 renew 延长，活跃页面在到期前续期；隐藏页停止续期与计算。
  到期先拒绝新请求，已获准请求可在既定期限内结束后回收；取消、显式释放或停机应在 2 秒内结束相关工作并归还预算。
- 工作页按 source_id 保存，额外绑定 snapshot_id；同页编辑和所有 Graphic Walker 子查询都用该版本。
  界面显示“DataFrame 快照”、采集时间、行数及到期状态，图表范围另行标记；不将 schema_revision 显示成数据版本。
- “重新绘图”重用当前快照；“获取最新快照”先取消旧请求，再申请新版本，成功后原子切换并释放旧版本；
  相同 Schema 保留图表，Schema 改变要求重新选择字段。申请失败保留仍有效的旧快照并清晰显示失败，禁止静默混用。
- 切页、隐藏沿用取消和代次隔离；响应须匹配 source_id、snapshot_id、schema_revision 和请求代次。
  关闭页主动释放；浏览器异常退出由租约兜底。过期保留绘图设置并提示重新获取；来源删除后旧快照仍可用，重新采集明确失败。

## 两条主链路

1. 选择 DataFrame → visualization 通过 HTTP 请求来源 provider → 原子读取并持有数据 → 受管共享内存导出 Arrow IPC →
   visualization 只读映射与验证 → 确认释放传输租约 → 工作页绑定快照 → Arrow workflow 计算 → 有界结果绘图。
2. 编辑/切页重用版本 → 用户获取最新快照时取消旧请求 → 成功取得新版本后切换；关闭/过期/取消/停机按所有权顺序回收，
   源追加、替换、删除及生产任务退出均不会改变已发布快照，迟到响应不能覆盖新版本。

## Feature Tasks 与验收锚点（依次实施）

- [x] **T1 — 交付具有稳定来源身份和拥有型生命周期的快照，使源写入、删除及任务结束不能改变已取得的数据。**
  验收：真实 Catalog/通道覆盖 CSV 和命名算子输出、空 Schema/零行、Write/Append 并发采集、重命名与同名重建；
  生产者释放外部引用后仍可读取，库卸载延后到最后租约释放；ASan/UBSan 无悬空访问，现有注册/预览与 DataFrame 回归保持。
- [x] **T2 — 交付有界、可取消的同机 Arrow IPC 交接，使独立探索进程可靠取得并管理快照。**
  验收：真实两个进程通过 HTTP 控制与共享内存交接，确认后来源退出仍可读；无整表 HTTP，异常导出、取消、过期、
  内存/字节/数量上限及私有路径校验均有断言；释放和停机无遗留映射/租约，缺数据面明确失败且数据库仍可用。
- [x] **T3 — 交付绑定快照的 Arrow 统计计算，使用户获得可重复、类型可信且范围明确的探索结果。**
  验收：固定 Arrow fixture 与独立期望值比对全部冻结操作，覆盖 NULL、复合键、Decimal/大整数、UTC/带时区日界、
  直方图端点、空集和全 NULL；过滤先于聚合、Top-N、过期/错配版本、预算与独立请求取消可执行验证。
- [x] **T4 — 交付同一工作区中的 DataFrame 工作页，让用户识别版本、主动刷新并在失效后恢复绘图。**
  验收：真实浏览器独立/嵌入入口覆盖 12 类图表、统计与字段能力，数据库/DataFrame 切换、简洁/高级配置保留、
  追加前后旧版不变/刷新改变、Schema 变化、关闭/隐藏、过期恢复与迟到响应隔离；两种桌面尺寸维持既有画布契约。
- [x] **T5 — 交付默认原生产物、使用说明和聚焦验收证据，使用户按文档从导入或任务输出完成版本化探索。**
  验收：隔离原生部署走 CSV 导入、SQL 命名输出到探索的真实路径，核对源码/默认产物；完整执行 T1～T4 新增测试及
  受影响的通道、Catalog、workflow、工作页、取消和交付回归；说明共享目录、预算、时间语义及原生支持边界。

## 实施与验证约束

- 用户已授权依序完成全部任务；按工作台指定单切片执行，T1～T5 各自整体验收并标记后才进入下一项。
- 实施前在工作台冻结允许文件、CMake targets、标准 `tools/test.sh` 筛选清单、选择依据、fixture 和 10～30 分钟时间盒。
  新测试同步接入依赖声明与资源锁；单测使用合成 Arrow，集成使用本轮私有目录和真实 Catalog/探索进程。
- 基础回归覆盖探索 workflow/client/chart、SQLite 浏览器/资源/体验/统计/交付与实际受影响的 DataFrame/Catalog 用例；
  若改动共享数据库 workflow 或公共接口，按调用链增加四库或共享契约回归，不默认纳入所有驱动、Kafka、镜像和产品全量。
- 外部数据库仅在所选回归需要时按统一运行时配置从实际位置 doctor；配置、权限或数据面缺失均非零，不跳过计通过。
  冻结清单必须 selected=executed=passed，failed=skipped=not_executed=0；文档检查与历史通过记录不替代本 Feature 验收。
- 用户说明维护现有 [探索说明](../../docs/exploration.md)；实现/测试证据放 evidence/visualization-dataframe/，不入 Git。
  分别报告源码完成、默认产物更新、隔离运行验证和用户既有服务状态；全部任务完成后再归档。

## 完成证据

T1～T5 均已实施并验收；本 Feature 的构建、测试与运行时证据见下列条目及 evidence/visualization-dataframe/implementation/（本地缓存，未入库）。
规格阶段的结构、链接及范围检查记录放 evidence/visualization-dataframe/planning/（本地缓存，未入库）。
2026-10-21：一轮契约审查完成；正文 157 个非空行、5 个相对链接与任务状态检查通过，范围外 1227 个文件摘要不变。

- T5 完成：默认 `cmake -B build src` 与 `cmake --build build -j$(nproc)` 全量构建通过，产物含 flowsql、libflowsql_catalog.so、libflowsql_visualization.so 与 exploration/；标准 integration selected=executed=passed=18，unit selected=executed=passed=2，failed=skipped=not_executed=0，summary 分别为 evidence/test-environment/20261022T111047Z-125c2367 与 20261022T111508Z-b89f2067。native 端到端在独立临时私有目录中启动 catalog 来源与 visualization 两进程，真实执行 CSV 导入与 `SELECT ... INTO dataframe.sql_output` 到版本化探索，验证绑定快照在来源删除后仍可计算、停机不残留发布对象、缺数据面时数据库路径可用且申请返回 DATA_PLANE_UNAVAILABLE；`docs/exploration.md` 已说明共享目录、上游、预算时限、时间语义与原生边界。期间修复 3 处缺陷：test_builtin 漏链`dataframe_transfer.cpp` 导致默认构建失败、未配置数据面时申请错误码与文档契约不符、传输用例仍发送裸 workflow 数组。证据 evidence/visualization-dataframe/implementation/t5-acceptance.json（本地缓存）。未覆盖 Docker 数据面、跨主机与四库回归。
- T4 完成：探索前端新增快照客户端与 DataFrame 工作页（`src/exploration/src/dataframe.js`、`main.jsx`），来源按数据库表
  与 DataFrame 分组；打开即取得快照并在状态行显示采集时间、行数与到期状态，计算携带 snapshot_id/schema_revision。
  标准 integration selected=executed=passed=14，failed=skipped=not_executed=0，
  summary=evidence/test-environment/20261022T100135Z-6b2a8e7c/summary.json。真实浏览器覆盖 12 类图表、箱线图禁用说明、
  获取最新快照切换并释放旧版本、重新绘图复用当前版本、简洁/高级切换保持版本、数据库与 DataFrame 页共存、关闭释放后
  续期返回 SNAPSHOT_EXPIRED、1366×768 与 1920×1080 画布契约；既有入口/计算/图表/体验/统计/交付回归保持绿色。
  证据 evidence/visualization-dataframe/implementation/t4-acceptance.json 与 evidence/visualization-dataframe/ui/（均本地缓存）。
- T3 完成：新增 dataframe_workflow（Arrow 内核执行受限 workflow）并接入 `/api/visualization/v1/compute` 的
  DataFrame 分派；标准 integration selected=executed=passed=10，failed=skipped=not_executed=0，
  summary=evidence/test-environment/20261022T093020Z-1c004676/summary.json。固定 Arrow fixture 覆盖投影、过滤、
  多字段/复合键分组、count/sum/mean/min/max/distinctCount、Top-N、直方图端点、UTC 日/月桶、Decimal 精确文本与
  近似均值来源、空集/全 NULL、安全整数上限与期限；HTTP 路径覆盖 snapshot_id 绑定、SNAPSHOT_MISMATCH/EXPIRED 与
  execution=dataframe。期间修复 Decimal min/max 被求和分支吞掉、零与负数比较方向、Decimal 文本未规范化、执行器
  误收整个请求对象、该 Arrow 构建无 strftime 内核等 5 处缺陷。证据
  evidence/visualization-dataframe/implementation/t3-acceptance.json（本地缓存）。
- T2 完成：默认 flowsql_catalog、flowsql_visualization、test_visualization_dataframe_store 已构建；标准 integration
  selected=executed=passed=6，failed=skipped=not_executed=0，
  summary=evidence/test-environment/20261022T083706Z-abe488b7/summary.json。真实 catalog 与独立 visualization 两进程
  经 HTTP 控制与私有共享目录交接；接收端映射建立后确认释放，来源 Stop 后批次仍通过 ValidateFull 并可求和；
  字节预算（SNAPSHOT_LIMIT）、取消、租约过期、符号链接与目录穿越形态的 transfer_id、缺数据面时数据库可用均有断言。
  期间修复 10 处实现缺陷（C++17 编译、半成品清理长度、预算下溢、发布 fsync 顺序、接收端映射生命周期、客户端线程
  析构与过期锁等），清单见 evidence/visualization-dataframe/implementation/t2-acceptance.json（本地缓存）。
- T1 完成：默认 flowsql_catalog、test_builtin、test_framework 已构建；标准 unit selected=executed=passed=2，failed=skipped=not_executed=0，summary=evidence/test-environment/20261021T085600Z-cd7fc034/summary.json。快照身份、并发追加、重命名/同名重建、源释放、零行/无 Schema、预算/取消/期限及赋值释放顺序断言通过；实际快照路径的 DataFrame、通道与 Catalog 源码经 ASan/UBSan 编译运行通过，格式及范围外摘要核对通过。证据 evidence/visualization-dataframe/implementation/t1-acceptance.json；首轮头文件和并发链接失败保留，不计通过。
