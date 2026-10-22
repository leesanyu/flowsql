# 数据库探索

FlowSQL 的“数据探索”使用 Graphic Walker 0.5.3，可选择已配置的 SQLite、MySQL、PostgreSQL、ClickHouse 单表，在默认界面直接选择 13 种非地图图表，并按图表需要选择字段，观察趋势、关系及数值分布。
过滤、分组和统计由所选源库的原生 SQL 执行，浏览器接收有界计算结果。探索服务是加载 `.so` 的独立 FlowSQL 进程；
直接访问和 Web 内容区使用同一份 React 静态包。界面默认采用以画布为主的简洁模式，可在同一会话中为多张表分别保留图表。
运行探索进程无需 Node 服务、Scheduler、Gateway 或 Python Worker。

## 原生启动

从仓库根目录构建：

```bash
cmake -B build src
cmake --build build -j4
```

产物包括 `build/output/libflowsql_visualization.so`、`libflowsql_database.so`、`exploration/` 和
`config/deploy-visualization.yaml`。Node/npm 仅参与构建，依赖由 lockfile 固定。

在 `build/output/config/flowsql.yml` 的 `channels.database_channels` 中配置要探索的库；保留文件中已有的其他配置。
例如使用绝对路径及只读参数：

```yaml
channels:
  database_channels:
    - type: sqlite
      name: analysis
      path: /srv/flowsql/data/analysis.db
      readonly: "true"
```

探索进程只读取通道配置，不注册数据库管理 HTTP 路由，也不修改通道文件。SQLite 文件及所在目录必须对运行用户可读。
只读挂载中的 WAL 数据库应由源库管理者提供可读取的一致快照，或正确提供现有 WAL/SHM 文件及相应权限；不要仅复制正在写入的主文件。

MySQL、PostgreSQL、ClickHouse 复用已有数据库通道，探索不需要创建另一套连接。配置形状如下；地址、库名和凭证填写部署实际值，避免将私有通道配置提交到 Git：

```yaml
channels:
  database_channels:
    - type: mysql
      name: sales_mysql
      host: mysql-service
      port: "3306"
      database: analytics
      user: exploration_reader
      password: <部署私有凭证>
    - type: postgres
      name: sales_pg
      host: postgres-service
      port: "5432"
      database: analytics
      user: exploration_reader
      password: <部署私有凭证>
    - type: clickhouse
      name: sales_ch
      host: clickhouse-service
      port: "8123"
      database: analytics
      user: exploration_reader
      password: <部署私有凭证>
```

服务进程须能访问配置端点；容器内用服务名。建议使用源库只读账号。MySQL/PG 每次探索使用独占只读事务，
ClickHouse 请求携带 `readonly=2` 并限制为服务端生成的单条 SELECT。ClickHouse 账号须允许读取所选表和元信息、
设置查询期限/结果限额，以及终止自己的查询；取消通过独立控制连接按唯一 `query_id` 执行 `KILL QUERY ... SYNC`。
MySQL 同样通过独立控制连接终止本账号的查询；PG 使用原生取消消息。部署应验证这些权限，取消失败不能当作资源已回收。

MySQL/ClickHouse 发现配置的当前数据库普通表；PostgreSQL 发现当前数据库中账号可 SELECT 的非系统 schema 普通表。
来源标签含通道、schema/数据库与表名，PG 同名表分别可选。每次请求独占会话，保留通道生命周期；四库均不承诺跨图表快照。
ClickHouse 不提供跨元信息/计算语句的一致性事务，界面刷新时源数据可能变化。

在产物目录运行：

```bash
cd build/output
./flowsql --config config/deploy-visualization.yaml
```

默认监听 `127.0.0.1:18910`。直接打开 `http://127.0.0.1:18910/exploration/`，或检查
`http://127.0.0.1:18910/api/visualization/v1/capabilities`。修改部署文件中 visualization 服务级 `option` 的 JSON 可配置：

```json
{"host":"127.0.0.1","port":18910,"static_dir":"exploration","limits":{"max_rows":10000,"max_bytes":8388608,"timeout_ms":30000,"max_active":2}}
```

`static_dir` 相对运行目录。行数允许 1～999999，期限允许 1～300000ms，会话允许 1～8，字节数为正整数；
客户端不能抬高部署限额。默认最多 2 个活动会话，元信息查询也占用配额，忙时直接返回 BUSY。

## 接入现有 Web

使用仓库默认 `./start.sh` 时，`deploy-single.yaml` 已配置 Web 上游并同时拉起独立探索进程；
浏览器访问 `http://127.0.0.1:8081/explore`，也可直接访问 `http://127.0.0.1:18910/exploration/`。
修改源码部署配置后，执行 `cmake --build build --target flowsql_runtime_config -j2` 同步运行配置，再重启 `./start.sh` 生效。
探索来源仍取自已有 `build/output/config/flowsql.yml`，没有数据库通道时需先配置；此操作不自动添加示例库或改动已有通道。

其他自定义启动配置需要在 `libflowsql_web.so` 的选项中追加：

```text
visualization_upstream=http://127.0.0.1:18910
```

若两服务位于不同容器，使用容器服务名并让探索服务监听 `0.0.0.0`。Web 直接代理
`/exploration/` 与 `/api/visualization/v1/`，无需 Gateway 注册。浏览器从 Web 顶栏“数据探索”进入 `/explore`，
内容区显示同源 iframe。首次进入创建工作区，切换菜单保留各表的图表状态；离开时取消活动查询并暂停计算，返回后可继续编辑。
整页刷新、主动重新加载工作区或服务重启后重新打开页面，会清空浏览器会话中的图表；不提供跨会话持久化。

未配置上游、服务不可达或版本不兼容时，入口显示具体状态与重试。服务默认仅本机可达；远程部署沿用部署侧访问控制。

## Docker / Compose

现有 Dockerfile 将探索插件和 `exploration/` 复制到同一 FlowSQL 镜像，并在构建期间校验实际产物。使用
[版本与镜像说明](versioning.md)构建本地镜像；不要仅复制 Vue `static/`。

可选 [Compose 配置](../docker-compose.exploration.yml) 提供独立 visualization 服务并配置 Web 上游。
先准备两个已存在的绝对路径：专用通道配置文件，以及包含 SQLite 文件的目录。通道 YAML 中的 `path` 应使用容器路径，
例如 `/opt/flowsql/exploration-data/analysis.db`。两者均以只读方式挂载，不复用或修改 Scheduler 的业务配置卷。

```bash
export FLOWSQL_IMAGE=flowsql:latest
export FLOWSQL_EXPLORATION_CONFIG=/srv/flowsql/exploration/flowsql.yml
export FLOWSQL_EXPLORATION_DATA=/srv/flowsql/exploration/data
docker compose -f docker-compose.yml -f docker-compose.exploration.yml config
docker compose -f docker-compose.yml -f docker-compose.exploration.yml up -d visualization web
```

默认直接入口只发布在宿主机 `127.0.0.1:18910`。可用 `FLOWSQL_EXPLORATION_PORT` 改宿主端口；Web 始终使用
`http://visualization:18910`。独立使用时只启动 `visualization` 即可，它不声明对其他服务的依赖。

## 选表与绘图

1. 在“数据来源”选择数据库表或 DataFrame，打开对应工作页；再次选择同一来源会定位已有工作页。
   DataFrame 来源首次打开即取得一份不可变快照，工作页顶部显示采集时间、行数与到期状态。
2. 在默认界面先选择“图表”，再填写该类型需要的字段，点击“绘图”。所有图表都可直接选择，无需先进入高级编辑。
   默认目录包含柱状图、堆叠柱状图、分组柱状图、折线图、面积图、散点图、圆点图、饼图、热力图、表格、突出显示表格、箱线图和直方图。
3. 从“更多设置”展开过滤、受限明细和适用的时间选项。规范 UTC 时间文本如 `2026-01-01T00:00:00Z` 可明确勾选“UTC 时间横轴”；
   若时间字段不适用，可取消该选项再绘图。日期时间默认保留源库文本和源时区语义。
4. 需要拖放字段、颜色、样式或更复杂的过滤时，进入“高级编辑”。默认控件和高级编辑共同编辑当前图表，切换模式会保留配置；
   修改单个字段时，其他高级编码、过滤和布局继续保留。切换图表类型时，会清理与新类型不兼容的编码。
5. “图表推荐”在默认和高级模式都可展开，使用同一个类型目录；点击类型会调整当前图表，不创建另一张图。
   它与“图表”下拉框作用相同，字段不足时显示具体缺少的字段，补齐后即可绘图。

字段控件随图表类型变化：

| 图表 | 需要选择的字段 |
| --- | --- |
| 柱状、折线、面积、圆点 | 类别或时间、数值 |
| 堆叠 / 分组柱状 | 类别或时间、数值、不同的系列分类 |
| 散点 | 两个不同的数值字段 |
| 饼图 | 分类、数值或行数；不显示横轴和纵轴 |
| 热力图 | 列分类、不同的行分类、颜色数值或行数 |
| 表格 / 突出显示表格 | 分类和数值；表格可以不选择分类 |
| 箱线图 | 数值、可选分组 |
| 直方图 | 一个数值字段；频数自动计算，不需要选择第二个字段 |

直方图默认 10 箱，箱数可改为 1～100 的整数。例如选择“响应时间”，系统会对过滤后的非 NULL 数据取最小值和最大值，
划分等宽区间，并在源库统计每个区间的记录数。普通区间左闭右开，最后一个区间包含最大值；常量列显示一个区间，空集和全 NULL 列不显示区间。
仅返回非空区间，结果状态中的行数是返回的区间数。频数由 SQLite、MySQL、PostgreSQL 或 ClickHouse 原生 SQL 计算，
不先截断明细再在浏览器统计；Decimal 边界沿用下述精度展示规则，源库仍按原生数值语义分箱。

散点、圆点及箱线图使用查询返回的明细；默认要求结果完整。数据过多时增加过滤，或明确勾选“受限明细”；
受限明细下的箱线图只描述返回部分数据，结果状态会标明范围。直方图始终对整个过滤范围进行聚合，受限明细设置不截断其输入。

来源方言、字段原生类型、时区/排序及能力说明位于 `ⓘ` 信息入口，支持鼠标悬浮、键盘聚焦和点击查看。
常驻区域保留绘图控件和结果范围，其他设置按需展开。

每张表的工作页分别保存字段、图表类型、聚合、过滤、UTC/明细选项和编辑模式。选择另一张表后再返回，配置仍在；
来源身份包含数据库通道和 schema，同名表会分开保存。一次只显示和计算当前页，不提供跨表 Join 或同屏多图拼版。
切页、关闭工作页或离开 Web 探索菜单时会取消该页的在途查询，旧响应不会更新其他页；关闭后再打开会从默认配置开始。
“取消查询”也会请求取消实际数据库会话。

“刷新来源”会核验已打开页的字段结构。来源消失时暂停该页并提示；结构变化时暂停旧图表，点击“重新选择字段”后
从新结构建立配置，避免将旧字段 ID 用于新列。来源未变化时保留原配置。

支持字段投影、比较/集合/范围/NULL 过滤及 AND/OR 组合、多字段元组分组、排序、显式 Top-N、
`count(*)`、非 NULL 计数、sum/mean/min/max。过滤先于聚合，Top-N 在聚合后裁剪输出。
NULL 与空字符串分组不同，`mean([10,NULL,20])=15`，全 NULL 的 sum/mean/min/max 为 NULL；文本排序遵循源列 collation。

图上“全量计算结果”“Top-N 结果”“受限明细，非全量”分别说明返回范围，同时显示行数和耗时。
聚合扫描整个过滤范围，输出超限返回 RESULT_LIMIT；不会先截断原表再标为全量统计。默认行数 10000、序列化结果 8MiB、期限 30s。
取消与超时传递到独占会话；本 Feature 的隔离慢查询验收要求取消、超时及停止过程在 2 秒内回收。

## DataFrame 快照探索

DataFrame 探索用于查看 FlowSQL 内部命名结果为 CSV 导入结果或算子/任务输出 `dataframe.<name>` 的中间数据。它与数据库探索
共用同一个工作区和同一套图表；差异在于数据来自一次采集的不可变快照，而不是实时查询表。

### 配置

数据面需要来源服务与探索服务都能访问的私有目录，两端配置必须一致，且都不由浏览器提交：

- 拥有 DataFrame 通道的服务（默认 `all` 中的 `libflowsql_catalog.so`）：`exploration_shared_dir=./exploration-shared`。
- 探索服务（`visualization`）：`dataframe_upstream=http://127.0.0.1:18803` 与 `dataframe_shared_dir=./exploration-shared`。

目录首次由来源服务创建为 `0700`，对象为 `0600`，并在其中持有 `flock` 排他锁，避免两个来源进程共用同一目录。
未配置数据面或上游不可达时，数据库探索照常可用，来源列表只显示数据库表；申请 DataFrame 快照会明确返回
`DATA_PLANE_UNAVAILABLE`，不会显示空数据。首版仅支持同机原生多进程，不提供跨主机或容器共享挂载。

### 使用步骤

1. 先让数据出现为命名 DataFrame：在任务中执行带 `INTO dataframe.<name>` 的 SQL，或通过 DataFrame 导入入口导入 CSV。
2. 打开数据探索，在“数据来源”的“DataFrame 快照”分组选择 `dataframe.<name>`。
3. 工作页状态行显示“DataFrame 快照”、采集时间、行数与到期状态；之后所有绘图都绑定这份版本。
4. 数据更新后点“获取最新快照”切换到新版本；“重新绘图”只在当前版本上重算，不重新采集。

版本语义：`schema_revision` 只描述字段顺序、名称、类型与可空性，不是数据版本；行数、采集时间和结果范围另行显示。
来源任务结束、通道被删除或重命名都不影响已取得的快照；重新采集会因来源不存在而失败，但工作页保留旧快照。
字段结构变化时提示重新选择字段；同一结构下刷新会保留图表配置。关闭工作页会释放快照；浏览器异常退出由租约兜底。

### 预算与时限

| 项目 | 默认值 |
| --- | --- |
| 单个快照 | 256 MiB（保留的 Arrow buffer 与 IPC 表示合计） |
| 来源进程快照总量 | 512 MiB |
| 同时存在的快照 | 8 个，同时采集 2 个 |
| 传输租约 | 60 秒（接收端建立映射并确认后立即释放） |
| 快照闲置有效期 | 15 分钟，可见工作页定期续期 |
| 计算 | 结果 10000 行、8 MiB、30 秒、2 个活动请求，额外工作内存 128 MiB |

这些是受管数据分配上限，不是操作系统 RSS 硬上限。超限返回 `SNAPSHOT_LIMIT` 或 `RESULT_LIMIT`，繁忙时直接拒绝而不排队。
取消、显式释放与停机应在 2 秒内结束相关工作并归还预算。快照过期后绘图返回 `SNAPSHOT_EXPIRED`，页面保留绘图设置并提示
重新获取；版本与请求不一致返回 `SNAPSHOT_MISMATCH`。

### 计算与时间语义

DataFrame 路径在 C++ 内用 Arrow 内核计算，不依赖 Python Worker：投影、比较/集合/范围/NULL 过滤及 AND/OR、多字段分组、
`count`/`distinctCount`/`sum`/`mean`/`min`/`max`、排序/Top-N、等宽直方图与日/月时间桶。统计口径与数据库路径一致：
`mean([10,NULL,20])=15`，空集或全 NULL 的 sum/mean/min/max 为 NULL、计数为 0，NULL 排序固定在最后。
二进制与嵌套字段标为不支持，其余字段可继续探索。

日/月桶只对 date 与 timestamp 开放：带时区的 timestamp 按其声明时区的日历分桶，无时区 timestamp 保留无时区日历值，
不使用宿主时区；文本保持分类，不猜测文本日期或整数 epoch 单位。箱线图入口在 DataFrame 页不可用，因为当前计算范围不含分位数。
大于 ±9007199254740991 的整数拒绝定量绘图，聚合不会先转浮点绕过；Decimal 汇总为精确文本，均值由内核返回浮点时标记为近似。

原生支持的边界：`documents` 中的独立探索服务与默认 `all` 服务均可用；Docker Compose 未声明共享目录挂载，因此
容器内不保证 DataFrame 数据面可用，数据库探索不受影响。

## 时间统计与去重

选择折线图、柱状图、面积图或其他聚合图表后，时间字段会显示“时间分桶”控件。选择“按日”返回 `YYYY-MM-DD`，选择“按月”返回该月第一天 `YYYY-MM-01`；过滤先执行，分组和去重计数由 SQLite、MySQL、PostgreSQL 或 ClickHouse 原生 SQL 完成。时间桶可以和普通分类字段一起分组，也可以排序和使用 Top-N。

时间桶使用源库日历值，不把时间转换成浏览器本地时区。MySQL `TIMESTAMP` 和 PostgreSQL `timestamptz` 使用数据库会话时区，ClickHouse `DateTime`/`DateTime64` 使用列时区；无时区的 `DATE`、`DATETIME`、`TIMESTAMP` 保留其日历值。SQLite 使用 ISO 日期或日期时间文本的前十位，不猜测 Unix 秒/毫秒单位。来源信息中的“源库日历”会显示这一语义；不支持的字段和周/小时等单位会明确报错。

聚合下拉框中的“去重计数”对应 `COUNT(DISTINCT 字段)`，ClickHouse 使用精确 `uniqExact`。NULL 不计入不同值，空字符串仍是一个值，空集或全 NULL 返回 0；文本比较遵循源列 collation，数值比较保留源库类型，因此相邻的超安全整数不会被浏览器浮点数合并。行数字段只能使用普通“计数”，去重计数可用于文本、数值和布尔字段。

时间分桶和去重计数配置属于当前图表，在简洁/高级编辑模式、切换其他工作页后继续保留；切换为明细图表或取消聚合会清除时间分桶。源字段结构变化后需要重新选择字段。查询失败会保留当前配置并显示原因，调整字段、单位或过滤后可重新绘图。

SQLite 混合存储类型、超出 JavaScript 安全范围（±9007199254740991）的整数、二进制及非有限数值无法定量绘图时明确报错，避免静默失真。
MySQL Decimal、PG Numeric、CH Decimal/UInt64 在驱动边界保留精确文本。常见 Decimal 先保留源库结果文本，再生成绘图数值：
`0.1`、`0.10000000000000000001` 可以绘图，并显示“≈ 近似数值”；`1.5` 等可精确表达的值显示“Decimal 精确数值”。
点击精度标记可查看每个结果字段的精确文本，顺序与返回结果一致，NULL 保持 NULL；任一非 NULL 值需要近似时，该字段标记为近似。
图形中的浮点刻度和悬浮信息用于观察趋势，精确结果应以精度信息中的源库文本为准。

投影、sum/min/max 和 MySQL/PG Decimal mean 使用源库精确结果。ClickHouse 的 Decimal mean 原生返回 Float64，允许近似展示，
精度信息会说明来源，且不提供虚构的精确均值文本。原生浮点字段沿用源库浮点语义，不将其标记为十进制精确值。
过滤、分组及统计仍使用源库原生类型，近似值仅供图形展示，不替代源库统计，也不写回数据源。

Decimal 转换为非有限值、非零值下溢为零或超出安全量级（±9007199254740991）时明确失败；
超安全整数的投影与定量聚合继续拒绝，不能通过先求均值再转浮点绕过保护。聚合输入检查使用实际过滤范围，
范围外的大整数不会导致安全查询失败。精确文本与精度元信息均计入 8MiB 结果字节预算，超过上限会报错，不会截断文本后返回成功。
布尔类型保留 true/false；数组、JSON、复合与二进制字段不支持绘图。日期/时间以源库文本和原生类型返回，默认字符串，
不猜测或自动转换源时区；规范 UTC 文本才适用“UTC 时间横轴”。NULL 排序、文本比较和 collation 遵循源库。
ClickHouse 使用 OrNull 聚合，使空集/全 NULL 的基础聚合与上述契约一致。
支持上述四库的单表、上述 13 类图表及源库数值分箱；时间桶、去重计数、计算表达式、地图、跨表/跨库及 DataFrame 属于后续能力，
不支持的 Graphic Walker 操作返回明确错误，不切换为浏览器本地聚合。

计算失败时，结果状态显示“查询失败”，并在原图表配置旁显示中文原因与调整建议；高级模式也使用这一提示，
避免重复弹出原生英文计算错误框。修改字段、聚合或过滤后重新绘图即可继续，无需清空工作页。
例如超安全大整数无法绘图时，可以改选可绘制的数值字段；若数据范围过大，可增加过滤或明确选择受限明细。

| 提示/错误 | 处理 |
| --- | --- |
| RESULT_LIMIT | 增加过滤，显式 Top-N，或对明细选择受限模式；聚合不会自动截断。 |
| BUSY | 等待其他查询结束或取消当前查询。 |
| SCHEMA_CHANGED | 刷新来源并重新选择字段。 |
| UNSUPPORTED_OPERATION / UNSUPPORTED_TYPE | 使用支持的操作或字段；超安全范围、非有限值、下溢或混合存储类型需要调整字段/过滤或检查源数据，普通 Decimal 近似绘图不再因非完全无损而失败。 |
| TIMEOUT / CANCELLED | 缩小查询范围后重试，或继续编辑图表。 |
| SOURCE_UNAVAILABLE / DATABASE_ERROR | 检查探索进程日志、只读配置及源文件权限；HTTP 不返回数据库物理路径或凭证。 |

## 独立静态包升级

探索构建目标为 `flowsql_exploration`，产物目录与 Vue `static/` 分开。manifest 记录 `ui_version`、API=1、桥接=1、
workflow=`graphic-walker-0.5.3`、构建身份及所有当前文件 SHA256。UI 包可有不同于主 Web 的构建身份，但包内所有 JS 必须一致。
静态包与 API 都经过兼容检查；`index.html` 和 manifest 不缓存，哈希资源长期缓存。

准备新包时在对应源码版本更新 `src/exploration/package.json` 与 lockfile 的版本，先生成当前真实构建身份，再单独构建：

```bash
python3 tools/product_version.py --root . --output /tmp/exploration-build-info.json
FLOWSQL_BUILD_INFO_FILE=/tmp/exploration-build-info.json npm --prefix src/exploration run build -- --outDir /srv/flowsql/packages/exploration-new
python3 tools/release.py exploration --package /srv/flowsql/packages/exploration-new
```

先将新包的哈希资源、许可证和 HTML 写入部署侧已配置的静态目录，最后以同文件系统原子替换 manifest。
升级期间保留旧 `assets/`，供已打开页面加载旧分块；不要执行清空输出目录的同步。短暂不完整包会返回 409，可重试。
兼容包在重新加载工作区后生效，无需更新 Vue 依赖、重新编译或重启主 Web。不承诺强制热替换或会话迁移。

容器部署可在附加 Compose 配置中将同一宿主静态目录只读绑定到 `/opt/flowsql/bin/exploration`，随后从宿主更新其内容；
应挂载目录以支持 manifest 原子替换。完整镜像交付仍用 `python3 tools/release.py artifacts` 校验 C++/Worker/Vue 及探索包。
当前脏工作区的开发构建不代表正式发布；正式发布还需版本说明中的 clean source/tag 校验。

## 测量口径

交付验收在同一机器、隔离 3 行 SQLite 数据集上测量：启动为创建进程到首次成功 capabilities HTTP 响应的墙钟时间，
每 10ms 探测；RSS 取 Linux `/proc/PID/status` 的 VmRSS，分别在 API 就绪和完成三类图表后采样。
前端体积只求和当前 manifest 引用文件，gzip 为每个文件独立压缩的和，不含保留的旧分块及 HTTP 开销。
2026-10-21 的开发机样本（Linux 6.18.54.1 WSL2、x86_64、20 个逻辑 CPU；标准开发构建；隔离 3 行 SQLite 表）：

| 指标 | 测量值 |
| --- | --- |
| 启动到 capabilities 可用（包括 Guardian 拉起服务） | 324 ms |
| API 就绪后的探索服务 RSS（不含 Guardian/浏览器） | 31.2 MiB |
| 完成内嵌/独立三类图表后的探索服务 RSS | 45.7 MiB |
| 当前静态包引用文件总量 | 4,549,665 bytes（4.34 MiB） |
| 同一文件集合独立 gzip 后总量 | 1,329,054 bytes（1.27 MiB） |

这是同一轮单次采样，未扣操作系统缓存、HTTP 探测与插件加载开销；图表后采样还包括类型错误展示路径。
一次开发机样本不能作为生产容量或延迟保证。

## 依赖与归属

工作区集成 Kanaries Graphic Walker 0.5.3，遵循其 Apache-2.0 和附带的 Graphic-Walker Branding License v1.0，
保留组件的 Kanaries 标志与归属。发布包在 `/exploration/licenses/` 提供原始 LICENSE、LICENSE2 和适配说明。
构建适配将组件的外部 Leaflet CSS、原始品牌 fallback 图片改为随包本地加载，没有移除品牌标志。
React、Graphic Walker、Vite 及传递依赖由 [lockfile](../src/exploration/package-lock.json) 固定；不依赖外部 CDN 或云端计算。
