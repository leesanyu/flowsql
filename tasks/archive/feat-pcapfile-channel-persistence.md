# Feature: PCAP 文件通道持久化

状态：`[x]` 已完成
优先级：P0
前置 Feature：`npm-offline-import-web`、`npm-offline-filter`（均已完成）

## 业务意图

用户上传并创建 `pcapfile.<name>` 后，服务重启仍能以相同逻辑名称、文件路径和回放参数查询并重复执行该
离线 source。SQL 任务结束只释放该任务的独占 reader，不删除基础通道或持久化配置；用户显式删除通道时，
持久化记录、运行期通道和受管文件按既有安全顺序一起退出。

本 Feature 在 Scheduler 进程内为 `PcapFilePlugin` 补齐配置持久化和启动恢复，不改变 Web 上传文件体只在
8081 接收、控制面只传小型 JSON、packet 数据面只走 `IBlockStreamChannel`/Arrow 的既有边界。

## Non-Goals

- 不持久化任务级 reader、读取偏移、EOF、过滤计划或执行进度；每次 SQL 仍从原文件创建独立 reader。
- 不扫描受管目录猜测逻辑通道名或自动导入孤立文件；没有持久记录的文件不构成通道。
- 不实现上传事务或删除事务的崩溃后孤立文件回收、数据库备份、跨主机复制或对象存储。
- 不扩展 `IBlockStreamChannel`、`IBlockStreamFactory`、`IBlockStreamManager`、reader/filter ABI、packet Schema
  或 SQL 语义。
- 不为其他 block stream provider 建立通用持久化框架，不改变普通 `StreamPlugin` 的存储表和生命周期。
- 不允许 Web 外部响应暴露持久化 option 中的绝对文件路径。

## 核心数据与接口契约

### 插件配置

`PcapFilePlugin::Option()` 新增解析可选项：

```text
db_path=<sqlite-file>
```

- `db_path` 非空时启用持久化；创建父目录，以 SQLite full-mutex 模式打开数据库，启用 WAL，并设置有限
  busy timeout。数据库错误必须作为插件或管理请求失败返回，不能降级为仅内存成功。
- `db_path` 为空时保留现有易失模式，供已有嵌入式调用和未配置部署兼容；正式原生、Guardian 和 Docker
  配置必须显式提供持久路径。
- 原生单进程和 Guardian 使用 `./meta/flowsql_meta.db`。Docker 使用现有 `pcap-uploads` 持久卷中的
  `/opt/flowsql/uploads/.meta/pcapfile.db`，不能把状态写入容器临时层。

### SQLite 表

使用独立表，禁止复用普通 `StreamPlugin` 的 `stream_channel_store`：

```sql
CREATE TABLE IF NOT EXISTS pcapfile_channel_store (
    type       TEXT NOT NULL CHECK(type = 'pcapfile'),
    name       TEXT NOT NULL,
    option     TEXT NOT NULL,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(type, name)
);
```

`option` 必须是 `ParsePcapFileSourceConfig()` 产生的规范化 JSON，完整保存 `path`、`format`、
`batch_packets`、`replay_mode` 和 `replay_speed_milli`。`path` 是 Scheduler 可访问的绝对规范文件路径。
运行状态由当前 channel/reader 派生，不写入数据库。

持久层是插件私有实现，不新增公共 IID 或 ABI。内部最小契约为：

```cpp
struct PcapFileChannelRecord {
    std::string type;
    std::string name;
    std::string option;
};

class PcapFileChannelStore {
 public:
    int Open(const std::string& db_path, std::string* error);
    int LoadAll(std::vector<PcapFileChannelRecord>* records, std::string* error);
    int Insert(const PcapFileChannelRecord& record, std::string* error);
    int Update(const PcapFileChannelRecord& record, std::string* error);
    int Erase(const std::string& type, const std::string& name, std::string* error);
    void Close();
};
```

`Insert` 对重复键返回 `EEXIST`，`Update`/`Erase` 对缺失键返回 `ENOENT`；SQLite 或 I/O 错误返回非零并
提供稳定诊断。所有 SQL 使用绑定参数，禁止拼接名称、路径或 option。

### 一致性与生命周期

- SQLite 记录是重启恢复的唯一真相；管理请求在插件 mutex 下串行执行，并在返回失败前补偿为请求开始时的
  数据库与内存状态。进程在管理请求中途被强制终止时，以已提交的 SQLite 状态恢复；由此产生的孤立文件回收
  属于 Non-Goals。
- `AddChannel` 先校验并打开新通道，再写入持久记录，最后发布到 `options_`/`channels_`；数据库失败时关闭
  新通道并保持内存和数据库均无该名称，使 Web 上传事务可以回滚最终文件。
- `ModifyChannel` 只有在新通道可打开且持久记录更新成功后才替换旧通道；失败时旧配置和旧通道保持可用。
- `RemoveChannel` 成功返回前必须同时移除持久记录和运行期通道；任一步失败都不能形成“返回成功但重启恢复”
  或“数据库已删但运行期仍可查询”的分裂状态。
- `Stop()` 只关闭运行期句柄，不删除持久记录；`Unload()` 清理内存并关闭数据库连接。
- `Start()` 取得 `IID_PROTOCOL` 后读取全部记录，在临时容器中逐一调用既有 `BuildChannel()`。全部成功后一次
  发布；任一记录的 option、文件或格式无效时关闭已构建临时通道、返回非零并记录具体 `pcapfile.<name>`，
  禁止部分恢复或静默跳过。

## 主链路

### 上传、执行与重启恢复

1. Web 将文件原子落入受管目录，并向 Scheduler 调用现有 `/channels/stream/add`。
2. `PcapFilePlugin` 解析/规范化 option、打开文件验证格式，将记录写入 SQLite 后发布基础通道；成功后 Web
   Commit 文件，失败则沿既有上传事务回滚文件。
3. SQL 从基础通道的已保存 option 创建任务独占 reader；任务完成只 `ReleaseReader()`，基础通道和数据库记录
   保留，同一 SQL 可再次从文件开始执行。
4. 服务重启后插件绑定 NPI、加载全部记录并重建基础通道；查询仍能看到原 `pcapfile.<name>`，SQL 可再次执行。

### 显式删除与失败恢复

1. Web 先从 Scheduler 内部查询保存该通道的受管绝对路径，再调用现有 `/channels/stream/remove`。
2. `PcapFilePlugin` 在无活跃 reader/batch 时原子收敛数据库记录与运行期通道；失败返回明确错误且 Web 保留文件。
3. Scheduler 删除成功后 Web 按既有 canonical-path 校验删除受管文件；此后服务重启不得恢复该名称。
4. 若启动恢复时持久记录指向的文件缺失、不是普通非符号链接文件或格式损坏，插件启动明确失败，不删除记录或
   扫描其他文件代替；管理员修复文件/记录后再启动。

## 原子任务

- `[x]` T0：建立 Feature 入口，冻结 SQLite Schema、插件配置、所有权、一致性、严格恢复和任务拆分；不修改代码。
- `[x]` T1：先实现并测试私有 `PcapFileChannelStore`，覆盖建表、绑定参数、Insert/Update/Erase、重复/缺失键、
  重开数据库和 SQLite 失败；只提供持久层，不接入插件生命周期。
- `[x]` T2：将存储接入 `PcapFilePlugin` 的 Option/Start/Stop/Unload 与 Add/Modify/Remove，测试易失兼容、数据库
  失败回滚、全量或零恢复、缺失/损坏文件启动失败、无部分发布以及 SQL reader 释放不删除基础记录。
  - `[x]` T2.1：解析可选 `db_path`，接入 Start/Stop/Unload，测试易失兼容、数据库重开、全量或零恢复以及
    缺失/损坏记录启动失败；不持久化管理写请求。
  - `[x]` T2.2：持久化 Add/Modify，测试规范化 option、成功一致性，以及数据库 Insert/Update 失败时不发布或
    不替换运行期通道。
  - `[x]` T2.3：持久化 Remove，测试数据库/运行期补偿、active reader/batch 保护、SQL reader 释放不删除基础
    记录，以及显式删除后重启不恢复。
- `[x]` T3：更新原生/Guardian/Docker 持久路径和文档，扩展真实 Web E2E 覆盖“上传 → SQL completed →
  Stop/Unload → 重新加载 → 原名再次执行 → 显式删除 → 再重启不恢复”，并完成部署与相关全量回归。
  - `[x]` T3.1：为单进程、Guardian 和 Docker 的 pcapfile 插件配置持久 `db_path`，用部署契约测试冻结路径、
    named volume 所有权和运行目录语义，并在 README 说明重启/删除行为。
  - `[x]` T3.2：扩展真实 Web E2E，覆盖上传后 SQL completed、插件 Stop/Unload/重新加载、原名再次执行、
    显式删除以及再次加载不恢复，并确认外部响应不泄露绝对路径。
  - `[x]` T3.3：执行标准配置、全量构建与完整 CTest，审查 Feature 全部 diff，完成 backlog、规格归档和工作台
    收口。

## 测试锚点与验收

| 验收面 | 必测断言 |
| --- | --- |
| 存储契约 | 独立表；规范化 option 精确往返；重复/缺失键错误稳定；数据库重开后记录仍在；SQL 全部绑定参数 |
| 管理原子性 | Add/Modify/Remove 成功时数据库和内存一致；数据库失败保持原状态；重名与 active reader/batch 保护不回归 |
| 启动恢复 | 原名称和完整 option 恢复；每次 SQL 从新 reader 开始；缺失/损坏文件使 Start 失败且不部分发布 |
| 删除语义 | SQL completed 不删记录或文件；显式删除后记录、通道和受管文件退出；后续重启不再恢复 |
| 部署 | 原生、Guardian、Docker 均配置持久 `db_path`；Docker 状态位于 named volume；外部查询继续隐藏路径 |
| 回归 | `test_pcapfile_import`、`test_pcap_upload_contract`、`test_pcap_web_e2e`、部署配置测试及完整 CTest 通过 |

Feature 完成时至少执行：

```bash
cmake -B build src
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
git diff --check
```

## 完成出口

1. 所有 T0～T3 均完成，生产部署启用持久化且易失模式只作为明确的兼容路径保留。
2. 上传后的 `pcapfile` 在 SQL 完成和服务重启后仍以原名可查询、可重复执行；任务 reader 状态不被恢复。
3. 显式删除后数据库记录、运行期通道和受管文件按既有所有权顺序退出，再次重启不恢复。
4. 持久记录损坏或目标文件缺失时启动明确失败且无部分通道可见；Web 外部响应仍不泄露绝对路径。
5. 前端构建、标准 CMake 全量构建、完整 CTest、部署契约和 diff 检查全部通过后归档规格并完成 backlog。
