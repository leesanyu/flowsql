# Feature: NPM 多实体结果存储与查询

状态：`[x]` 已完成，T0～T4 已验收
优先级：P1
前置：`npm-protocol-analysis`、`database-platform`（均已交付）。
后续：DNS、HTTP/1、TLS、ICMP 等结果实体直接复用本 Feature，不各自实现存储。

## Non-Goals

- 不支持一条 SQL 的双 `INTO`、逗号分隔多 sink 或隐式 DataFrame 副本；实时增量预览/订阅另行立项。
- 不把多个异构实体塞入同一宽表、JSON/BLOB 万能表或同一个 Arrow Schema，不修改各模块的业务字段含义。
- 不提供跨数据库复制、分布式事务、跨批次 exactly-once、自动重试或已提交前缀回滚。
- 不实现 DNS、HTTP/1、TLS、ICMP 解析、TCP 重组、生产实时采集或协议模块私有存储逻辑。
- 不让数据库通道理解 NPM；普通算子和三段式 `<db_type>.<db_name>.<table>` 的单表语义保持不变。
- 不默认删除历史数据；不处理数据库备份恢复、进程崩溃后的 writer fencing 或人工改坏托管表的自动修复。

## 业务意图

让需要留存和回查离线分析及后续持续分析结果的 NPM 用户，以一个数据库通道保存一次运行中全部已启用实体，
并能通过既有三段式数据库关系查询某个运行、实体及 Schema 版本的历史、最新快照或终态，再按需物化为 DataFrame。

本 Feature 复用 `npm-protocol-analysis` 已交付的 `NpmResultContextV1`、`NpmEntityDescriptorV1` 和
`INpmResultConsumerV1`。`features` 决定计算/持久化实体，`observing` 只选择普通前台实体；数据库持久化不受
`observing`、前台投影或 operator-stage 过滤影响，具体协议模块不依赖任何数据库驱动。

## SQL 与执行边界

### 单一目标语义

```sql
SELECT *
FROM pcapfile.xxx
USING npm.basic
WITH input_namespace='pcapfile.xxx',
     source_domains='0:1',
     features='basic,session',
     observing='session'
INTO mysql.npm
```

- 对 `npm.basic`，两段式数据库目标 `<db_type>.<db_name>` 表示托管多实体结果命名空间；示例中的通道是
  `mysql.npm`。该通道必须已存在、可写，并由 Scheduler 在任务存活期间持有租约。
- 托管模式只有一个 SQL 目标。Scheduler 不注册 DataFrame，也不把 observing 结果另写默认表；任务完成响应
  返回 `run_id`、每个实体的版本/关系名/写入行数和总写入行数。
- 两段式特殊语义仅由声明“托管 sink”能力的 block transform task 接受；其他算子继续按现有规则处理或拒绝
  缺少表名的数据库目标，不按算子名称在 Scheduler 中硬编码 NPM 分支。
- 三段式 `INTO mysql.npm.user_table` 仍是一个明确 Schema 的普通单表目标，不表示多实体托管，不自动生成
  NPM 元数据或其他实体表；具体算子是否支持直接写该表继续遵循既有能力，本 Feature 不把它改造成第二种
  多实体入口，也不顺带为 block transform 增加通用单表 sink。
- 托管写入首版要求 `SELECT *`。source-stage `WHERE` 可以缩小输入并相应改变全部实体；NPM 之后的投影/
  operator-stage 过滤没有持久化含义，在托管模式下明确拒绝，避免用户误以为它过滤了数据库结果。

### 查询与 DataFrame 物化

既有三段式关系查询保持原样有效；例如用户已经建好 `session_table` 时：

```sql
SELECT *
FROM mysql.npm.session_table
INTO dataframe.session_table
```

这条语句只读取一个明确的数据库关系，不表示 NPM 双输出，也不改变普通数据库通道的单表语义。
当上游使用两段式 `INTO mysql.npm` 托管模式时，NPM 返回的实体摘要会给出可查询的托管关系名；客户端仍用
同一条三段式 `FROM <db_type>.<db_name>.<relation> INTO dataframe.<name>` 链路读取它。

每个实体/Schema 版本暴露三个只读关系，名称固定为：

```text
npm_<entity_id>_history_v<schema_version>
npm_<entity_id>_latest_v<schema_version>
npm_<entity_id>_final_v<schema_version>
```

例如读取某次 Session 运行的每个实体最新版本：

```sql
SELECT *
FROM mysql.npm.npm_session_latest_v1
WHERE __npm_run_id='run-id-from-analysis-result'
INTO dataframe.session_latest
```

- `history` 返回未到期且非清理中运行的全部 revision；`latest` 对
  `(__npm_run_id, identity_column)` 选择最大 revision；`final` 只返回业务行中 `is_final=true` 的 revision。
- `latest` 不表示“最新运行”或“最新 Schema”。查询必须显式选择带版本关系；用户以返回的 `run_id` 精确选择
  本次运行，避免并发/重复执行时依赖易漂移的 latest-run 含义。
- 三类关系都暴露 `__npm_run_status`；`writing`/`incomplete` 的可见前缀不会冒充成功结果。event 实体固定
  revision=1 且 final=true，因此三个关系可返回同一业务行。
- 关系是数据库内的表/视图或等价只读关系，继续走现有 database → DataFrame 查询链；不增加第二个分析 sink。

## 冻结接口与数据结构

### 可选托管 sink 能力

在 block transform V1/V2 之外新增可选 task 侧接口；既有 provider/task ABI 不修改：

```cpp
struct BlockTransformManagedSinkBindingV1 {
    uint32_t struct_size;
    uint32_t contract_version;
    IChannel* sink_channel;  // borrowed; Scheduler lease lives through ReleaseTask
    const char* target;      // exact SQL target, copied by BindManagedSink
    const char* category;    // normalized database type
    const char* name;        // database channel name
    const char* relation;    // empty for managed namespace
};

interface IBlockTransformManagedSinkTaskV1 {
    virtual ~IBlockTransformManagedSinkTaskV1() = default;
    virtual int BindManagedSink(const BlockTransformManagedSinkBindingV1&) = 0;
    virtual std::string ManagedSinkResultJson() const = 0;
};
```

- Scheduler 先解析目标并取得通道，再在 `Open()` 前至多绑定一次；task 复制字符串但不拥有 channel。绑定后仍按既有
  Process/Flush/Cancel 生命周期执行，Scheduler 只排空并丢弃其前台批次，不把批次写往另一个具名 sink。
- 不实现该接口的 task 遇到非 DataFrame block-transform 目标时保持现有拒绝语义。NPM task 只接受可写的
  `IDatabaseChannel`、空 `relation` 和精确两段式目标；绑定失败不得创建 runtime 或可见 run。
- 创建 run 后的成功或失败终态，`ManagedSinkResultJson()` 至少含 `run_id`、`run_status`、`metadata_status`、
  `rows_written` 和 `entities[]`；每个实体包含 `entity_id`、`schema_version`、三类关系名及 `rows_written`。
  Scheduler 在释放 task 前读取它：成功时作为结构化 `result`，失败时附入结构化错误，使已提交前缀可定位；
  run 创建前的 Open 失败可以没有摘要，状态落库失败时 `metadata_status=unknown`，不得伪报 incomplete 已持久化。

NPM runtime 在生成唯一 `run_id`、冻结全部实体描述之后、发布 runtime 之前创建附加消费者：

```cpp
struct NpmResultFailureV1 {
    int32_t code;
    const char* stage;    // borrowed for FailRun call
    const char* message;  // borrowed for FailRun call
};

interface INpmManagedResultConsumerV1 : public INpmResultConsumerV1 {
    virtual int FailRun(const NpmResultFailureV1&) = 0;
    virtual std::string ResultJson() const = 0;
};

interface INpmResultConsumerFactoryV1 {
    virtual ~INpmResultConsumerFactoryV1() = default;
    virtual int Create(const NpmResultContextV1&,
                       const std::vector<NpmEntityDescriptorV1>&,
                       std::shared_ptr<INpmTaskBudget>,
                       std::unique_ptr<INpmManagedResultConsumerV1>* consumer) = 0;
    virtual std::string LastError() const = 0;
};
```

Factory 是 task 私有拥有型对象。`Create` 一次性验证全部 Schema、命名空间和目标能力；任一失败不发布 runtime。
已有 `INpmResultConsumerV1` 的同步 `Consume/Finish/Cancel` 契约保持不变，所有 enabled 实体仍由统一 router 投递。
`Cancel()` 只负责并发、非阻塞地唤醒数据库调用；模块/写入失败或取消后，runtime 等所有在途回调退出，再恰好一次
调用 `FailRun()` 尝试标记 incomplete。`FailRun()` 与成功 `Finish()` 互斥，失败不得覆盖原始任务错误。

### 托管元数据与结果关系

托管命名空间保留 `npm_` 表/视图前缀；发现同名非兼容对象时失败，不覆盖、删除或猜测迁移用户对象。

| 关系/记录 | 最小字段与保证 |
| --- | --- |
| `npm_result_runs` | `run_id` 主身份、`task_id`、`input_namespace`、`status`、开始/完成时间、可空 `expires_at`、错误摘要。一次 task Open 对应一个 run；task_id 可重复，run_id 不复用。 |
| `npm_result_entities` | `(entity_id, schema_version)`、module ID、revision 语义、四个语义列名、规范 Arrow Schema/指纹、内部数据表及三类公开关系名；同版本不同指纹拒绝。 |
| 内部 data 表 | `__npm_run_id`、`__npm_task_id` 加原始业务 Schema 全部字段；逻辑键为 `(run_id, identity_column, revision_column)`，不作为稳定用户查询入口。 |
| history/latest/final 关系 | 从内部 data 表与 run 元数据导出的只读关系，补充 `__npm_run_status`；history 保留全部 revision，latest 取最大 revision，final 只取终态，不复制或改写业务字段。 |

- 可持久化 `entity_id` 限定为 `[a-z][a-z0-9_]{0,31}`；实体业务列不得使用保留前缀 `__npm_`。不满足时
  task Open 失败并指出实体/字段，不通过未转义标识符拼接 SQL。
- Schema 指纹覆盖字段顺序、名称、Arrow 类型、nullability 和 metadata。新 `schema_version` 创建独立 history/
  latest/final 关系；不 ALTER 旧版本，也不提供漂移的“当前版本”别名或自动跨版本 UNION。
- 每个不同 Schema 必须使用不同版本：现有 Basic/Session 无标签 Schema 固定为 v1，含
  `primary_label_id` 的标签 Schema 固定为 v2；T0 先修正并测试描述符版本，再允许创建托管关系。后续模块的任何
  字段、类型、nullability 或 metadata 变化同样必须递增版本，不得只靠指纹在运行时制造永久冲突。
- 原始 identity/revision/observed_at/is_final 列继续由 `NpmEntityDescriptorV1` 映射；存储层验证结果 router 已冻结
  的 Schema，不改写身份、revision 或终态。所有值参数化绑定，标识符由后端适配器校验并引用。
- SQLite、MySQL、PostgreSQL、ClickHouse 对相同逻辑模型给出相同可观察查询语义。字段值与逻辑类型必须无损，
  尤其 `uint64` 全域不得窄化为 `int64`；后端不能映射时在 Open 拒绝该实体，不得静默转字符串或退化为
  JSON/BLOB。规范 Arrow Schema/metadata 完整保存在目录中并参与指纹；通用数据库查询不承诺把 Arrow metadata
  重新附着到返回 Schema，业务值、nullability 和可查询语义仍必须一致。

### 写入、失败与资源边界

- `Consume` 同步写入当前 RecordBatch；首版不设后台队列。封装列/序列化和驱动缓冲计入 task 的
  `kPendingOutput`，单次调用结束即释放，因此最多保留当前批次且数据库慢会自然反压分析。
- 每批使用后端可提供的最小原子写入单元；不同批次/实体不承诺同一事务。成功前缀持久保留，后续写入失败
  立即终止 task，不丢弃、不自动重试，也不回滚已提交前缀。
- run 状态为 `opening`、`writing`、`completed`、`incomplete`、`purging`。只有 consumer `Finish()` 成功并提交
  行数/完成时间后才是 completed；Cancel、模块错误或写入错误在回调静默后调用 `FailRun()`，能成功更新时成为
  incomplete 并记录原因，更新本身失败则响应明确报告 metadata status unknown。
- 网络错误可能发生在数据库已提交但客户端未收到确认之后；此时仍返回失败并标记 incomplete/结果未知，禁止
  自动重试制造重复。逻辑键和 run_id 供人工核查，但本 Feature 不宣称 exactly-once。
- 初始化 DDL/实体目录并发由数据库级互斥或“创建后重读验证”收敛；同一命名空间的并发 run 以 run_id 隔离。
  手工篡改托管 Schema、关系或指纹时新任务明确失败，不自动 DROP/ALTER 修复。
- 数据库操作遵守通道/驱动超时；Cancel 只发出非阻塞取消信号，不在 NPM 生命周期锁内等待外部数据库调用。

### 保留策略

- `parameters.core.result.retention_days` 是托管模式专用的可选整数 `1..3650`，只决定本次 run 的 `expires_at`；
  省略表示不自动过期，不修改其他 run 或命名空间全局策略；非托管任务携带该节点时在 Open 明确拒绝。
- 清理仅处理到期的 completed/incomplete run：先原子标记 purging，使三类查询关系立即排除它，再按实体关系
  删除数据，最后删除 run 元数据；中断后重复执行同一清理必须幂等。
- writing run 不自动删除；异常进程遗留状态保持可见，交由明确的运维动作处理。清理不得与当前 run 共用无界
  内存，也不得阻塞其结果正确性；每次扫描/删除按固定批量上限执行。`expires_at` 是查询可见性的截止时间，
  公开关系到期即排除该 run；物理删除由后续托管任务 Open 时执行的有界维护推进，因此无新任务时允许延迟回收，
  但不得让已过期数据重新可见。

## 主链路

1. **分析与多实体持久化**：Scheduler 解析两段式数据库目标并持有通道 → 创建 task、绑定托管 sink → NPM Open
   冻结 features/observing、run_id 和全部实体描述 → factory 原子校验/建立目录与关系 → router 将每个 enabled
   实体同步投递消费者 → 正常 EOF Finish 标记 completed 并返回 run_id/关系摘要；错误保留可识别前缀并失败。
2. **查询与清理**：客户端从分析响应取得 run_id/显式版本关系 → 以现有三段式 database source 查询 history、
   latest 或 final → 可选 `INTO dataframe.<name>` 物化；清理器只将显式到期的终态 run 标记 purging、分批删除并收口。

## Feature Tasks 与测试锚点

- `[x]` T0：交付可编译的托管 sink/factory 契约和 SQL 计划校验，使两段式 NPM 命名空间具有唯一、无双写的执行边界。
  锚点：两段式/三段式分流，非能力 task 拒绝，Bind 仅一次且先于 Open，`SELECT *`/过滤限制，Basic/Session
  标签 Schema v1/v2、通道租约与旧 DataFrame/普通数据库 SQL 回归。
- `[x]` T1：交付 SQLite 上 Schema 版本化的多实体消费者与 run/catalog/history 存储，使 Basic/Session 全部结果
  不受 observing 影响且失败前缀可审计。锚点：双实体四种 observing 组合、重复 task 新 run、版本/指纹冲突、
  保留列冲突、同步反压、Consume/Finish/Cancel/不确定提交和预算归还。
- `[x]` T2：交付显式版本的 history/latest/final 关系及结构化运行摘要，使用户可由 run_id 经既有数据库链路准确
  回查并物化 DataFrame。锚点：cumulative/event、多实体同 identity 不串扰、latest 最大 revision、final 子集、
  incomplete 可见标记、无 latest-run/latest-schema 漂移、database → DataFrame E2E。
- `[x]` T3：交付 MySQL、PostgreSQL、ClickHouse 与 SQLite 的同契约适配，使四种现有数据库通道在类型映射、并发
  初始化和查询结果上保持一致。锚点：Basic/Session Schema 全字段往返、参数化值/安全标识符、并发首次 Open、
  不兼容对象拒绝；四后端官方集成环境均验收，未运行的可选测试不作为 Feature 完成证据。
- `[x]` T4：交付显式 run 保留、清理恢复和组合回归，使长期留存有可控退出路径且不破坏现有 NPM/数据库能力。
  锚点：无策略不删除、到期终态 purging/幂等分批清理、writing 保留、清理中查询隔离、失败注入/并发 run、
  定向 Sanitizer、完整构建/CTest、格式和数据库/NPM 既有 SQL 回归。

依赖顺序：T0 → T1 → T2；T3 基于 T1/T2，T4 在四后端契约完成后组合验收。每次只载入一个 Atomic Slice。

## 完成出口

- 一条 `npm.basic ... INTO <db_type>.<db_name>` SQL 保存全部 enabled 实体，切换 observing 不改变持久化行集；
  完成响应可直接构造显式 run_id、实体和 Schema 版本查询。
- 四种数据库后端的 history/latest/final 与失败/保留语义一致，普通三段式数据库写入及现有 DataFrame 前台路径
  无回归；协议模块仅声明实体并 Emit，不包含数据库代码。
- T0～T4 的测试锚点、对应 CMake targets、定向 Sanitizer、全量构建和完整 CTest 全绿，C++ 格式与 Diff 检查通过；
  随后才归档规格并把 Backlog 标记完成。

## 完成证据

- T0（2026-09-24）：托管 sink、NPM consumer factory 与拥有型 database lease 契约已编译；Scheduler 两段式目标正向和拒绝路径、NPM Bind/Schema 版本、数据库 `Release/Remove/Update` 租约保护均有测试锚点。定向 CTest 3/3、database manager 21/21、格式与 Diff 检查通过；T1 未启动。
- T1（2026-09-25）：SQLite task-private consumer 已建立 `npm_result_runs`、`npm_result_entities` 和按实体/Schema 版本隔离的内部 data 表；Basic/Session enabled 实体均经统一 router 同步参数化写入，`uint64` 全域以可逆十进制 TEXT 保存，Finish/FailRun/Cancel 分别收口 completed/incomplete，临时参数预算在调用结束归还。专项测试覆盖四种 feature/observing 组合、重复 task 新 run、batch 内原子回滚、提交失败连接清理、metadata unknown、失败前缀、命名/保留列/类型/指纹/catalog/data 表结构冲突及预算拒绝；定向 CTest 4/4、database manager 21/21、database E2E 可用项及格式/Diff 检查通过。T2 未启动，公开 history/latest/final 关系仍仅有目录名称。
- T2（2026-09-25）：SQLite 为每个实体/Schema 版本创建并逐次重读校验只读 `history/latest/final` 视图；history 保留可见 revision，latest 在 `(run_id,identity)` 内按无符号十进制长度和值选择最大 revision，final 只保留终态，三者补充 `__npm_run_status` 并排除 purging/到期 run。专项测试覆盖 cumulative/event、多实体同 identity、跨 run 隔离、`UINT64_MAX` latest、final 子集、incomplete 标记、v1/v2 隔离、无漂移别名及同名对象冲突；Scheduler E2E 从结构化响应取得 `run_id`/版本关系名，经三段式 database source 成功物化 DataFrame。定向构建与 CTest 2/2、格式和 Diff 检查通过；T3 未启动。
- T3（2026-09-25）：托管 consumer/factory 已泛化为 SQLite、MySQL 8、PostgreSQL 16、ClickHouse 24.3 同一契约；后端方言分别提供无损 `uint64`、typed/null/blob 参数绑定、批次最小原子写入、版本化目录/数据表/只读关系、终态更新和创建后重读验证。专项非跳过集成测试在四个官方环境覆盖 Basic/Session 全字段及 nullable 往返、`UINT64_MAX`、引号/反斜线/Unicode、history/latest/final、completed/incomplete、跨 run 隔离、v1/v2 隔离、结构化摘要、并发首次 Open、batch 失败无部分行、catalog/data/关系冲突拒绝；Scheduler E2E 保持三段式物化链路。最终定向 CTest 3/3；MySQL 19/19、PostgreSQL 7/7、ClickHouse 21/21 且 0 skip；clang-format diff、`git diff --check` 与范围审计通过。T4 未启动。
- T4（2026-09-26）：`parameters.core.result.retention_days` 仅在托管目标生效，接受 1～3650 天；无策略保留 `expires_at_ns=NULL`。到期的 completed/incomplete run 在后续 Open 中先标记 purging，再每次最多处理一个 run、一张目录表和 64 行，持久游标支持中断后幂等续清；writing 保留，三类公开关系在到期和 purging 时隔离。T3 目录经精确结构校验后追加游标列并重读验证，四后端旧目录迁移均通过。SQLite 专项验证无策略、到期、writing、失败注入、跨表分批恢复；MySQL 8、PostgreSQL 16、ClickHouse 24.3 专项集成验证到期清理和旧目录迁移。定向 ASan/UBSan SQLite 与三后端专项通过（关闭该环境不支持的 LeakSanitizer）；全量构建、CTest 19/19、MySQL 19/19、PostgreSQL 7/7、ClickHouse 21/21 均通过；格式和 Diff 检查通过。未 commit/push。
