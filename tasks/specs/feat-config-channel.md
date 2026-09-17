# Feature: 配置资源通道

状态：`[-]` 进行中
优先级：P1
前置 Feature：`web-console`、`routing-services`（均已完成）

## 业务意图

需要跨任务复用静态规则、映射和字典的用户，可以在现有“通道管理”页面上传或编辑 JSON、YAML、XML，
并将其发布为具名、永久保存、重启可恢复的配置通道。消费者使用
`config.<name>@<revision>` 精确取得带格式、业务 Schema 和内容摘要的不可变只读快照，不再依赖每个算子约定
自己的本地配置目录，也不会因后续发布新版本而改变已绑定任务的配置。

配置资源是体积有界、低频变更的控制面数据；独立 Config Channel Provider 在 Scheduler 进程持久化并通过
版本化 IID 暴露快照，不复用普通 DataFrame/Stream 数据通道。`npm.basic`、Application Scope 等消费者的
参数绑定和业务解释由各自 Feature 完成。

## Non-Goals

- 不允许配置通道参与 SQL `FROM`/`INTO`，不改变 `IChannelRegistry`、DataFrame、Stream 或 Block Stream ABI。
- 不把本 Feature 作为密钥、证书或密码管理系统；配置页面及管理接口按普通非秘密资源处理。
- 不实现用户自定义 revision、`latest` 持久任务引用、可变别名、语义版本标签或版本审批流。
- 不实现多人协同草稿、自动保存计数、文本差异合并、运行中任务热更新或内容变更通知。
- 不实现通道重命名、历史 revision 覆盖/物理删除、保留策略、垃圾回收或跨集群复制。
- 不定义 Application Scope 等具体业务 Schema，不替消费者校验 CIDR、端口或领域规则冲突。

## 核心契约

### 标识与不可变快照

- 通道名匹配 `[a-z][a-z0-9_-]{0,63}`；规范引用为 `config.<name>@<revision>`，`revision` 是从 1 开始的
  十进制正整数。DataFrame、Stream 与 Config 可以使用相同 `<name>`，类别互不冲突。
- 消费者只能解析精确 revision；缺少 `@revision`、`@latest`、revision 0、未知通道或未知 revision 均明确失败。
- 每个 revision 保存当次 `format`、`schema_id`、原始 UTF-8 内容字节、SHA-256、长度和发布时间；摘要基于
  实际保存字节，不重排字段，也不判断 JSON/YAML/XML 的跨格式语义等价。
- 旧 revision 永不修改。“编辑”或“基于旧版本恢复”只读取旧快照作为副本，成功发布后生成当前通道的下一
  revision；已经取得旧快照的任务继续使用自己的不可变内容。

公共消费接口独立于当前只按裸 `name` 注册 `IChannel` 的 `IChannelRegistry`：

```cpp
struct ConfigChannelSnapshot {
    std::string channel_name;
    uint64_t revision;
    std::string format;       // json | yaml | xml
    std::string schema_id;
    std::string sha256_hex;
    uint64_t content_bytes;
    int64_t created_at_unix_ms;
    std::shared_ptr<const std::string> content;
};

interface IConfigChannelRegistryV1 {
    virtual ~IConfigChannelRegistryV1() = default;
    virtual int Resolve(const char* exact_reference,
                        ConfigChannelSnapshot* snapshot,
                        std::string* error) = 0;
};
```

Provider 通过新 IID `IID_CONFIG_CHANNEL_REGISTRY_V1` 注册该接口；`Resolve()` 线程安全，返回拥有内容生命周期的
快照，不暴露 SQLite 句柄或具体插件类。消费者在任务打开阶段 Resolve、检查 `schema_id` 并编译为任务私有配置，
逐包或逐事件路径不得查询数据库、调用 HTTP 或重复解析原文。

### 发布与系统 revision

发布命令至少包含 `name`、`expected_current_revision`、`format`、`schema_id` 和 `content`，并可包含
`base_revision`、`original_filename` 和 `change_note`。创建新通道时 expected 值为 0；更新时必须等于客户端
读取到的 current revision。

revision 由系统自动分配，不由用户填写，也不按编辑器敲字、自动保存或上传请求次数计数：

1. 完成名称、大小、UTF-8、格式语法及元数据校验后，在同一 SQLite 写事务内读取 current revision。
2. 待发布内容的 `format + schema_id + content SHA-256` 与当前快照完全相同时，幂等返回当前 revision，
   `created_revision=false`；重试不制造空版本。
3. 内容不同时检查 `expected_current_revision`；不匹配返回版本冲突，不覆盖他人的发布，也不分配 revision。
4. 匹配时以 `current + 1` 插入不可变快照并更新 current 指针；两者全部提交后才返回新 revision。

首个成功发布得到 revision 1。草稿、取消、语法错误、超限、冲突、SQLite/磁盘失败和事务回滚均不产生可见
revision。基于旧 revision 3 恢复时，只要其内容不同于当前 revision 7，就发布为 revision 8，并记录
`base_revision=3`；不能把 7 改回 3，也不能覆盖 revision 3。可选 `change_note` 表达人的版本意图，但不参与引用。

### 持久化模型

Provider 使用部署持久目录中的 SQLite 数据库和独立表，配置内容因受 512 KiB 上限约束而直接保存为 BLOB：

```sql
config_channel(
    name TEXT PRIMARY KEY,
    current_revision INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
)

config_channel_revision(
    name TEXT NOT NULL,
    revision INTEGER NOT NULL,
    format TEXT NOT NULL,
    schema_id TEXT NOT NULL,
    content BLOB NOT NULL,
    content_bytes INTEGER NOT NULL,
    content_sha256 BLOB NOT NULL,
    base_revision INTEGER,
    original_filename TEXT,
    change_note TEXT,
    created_at INTEGER NOT NULL,
    PRIMARY KEY(name, revision)
)
```

- 通道行、revision 行和 current 指针在同一事务收敛；禁止无锁 `MAX(revision)+1`。写事务串行化同一通道的
  发布，并以 `expected_current_revision` 阻止丢失更新。
- SQLite 是重启恢复的唯一真相；打开、建表或读取失败必须使 Provider 启动失败，不能降级成仅内存成功。
- `content_bytes`、摘要和原文必须在一次提交中一致。数据库满或写入失败返回明确错误，旧 current 和全部历史
  保持可解析。

### 内容与安全边界

- `format` 仅允许 `json|yaml|xml`；`.yml` 文件映射为 `yaml`。`schema_id` 是 1～255 字节的必填 UTF-8 文本，
  具体消费者决定是否接受该 Schema 及其字段含义；`original_filename` 最多 255 字节，`change_note` 最多
  1024 字节。
- 内容必须是非空 UTF-8 文本，原始内容最大 512 KiB，解析嵌套深度最大 64；列表、详情和历史接口不默认返回
  原文，只有预览/下载或精确 Resolve 才返回内容。
- JSON/YAML 映射拒绝重复键；YAML 使用安全解析模式并拒绝自定义对象标签和 alias；XML 禁止 DTD、外部实体、
  外部资源和 XInclude。校验只证明基础语法安全，不进行业务 Schema 转换或重写。
- Web 预览必须以纯文本转义显示，不能把 XML/HTML 内容注入 DOM。文件名仅作有界展示元数据，不参与服务器路径。

## 控制面与页面契约

Scheduler Provider 提供列表、发布、revision 历史和精确解析/下载四类 `/channels/config/*` 控制操作，Web 以
`/api/channels/config/*` 对外代理。内容以 Base64 放入有界 JSON 控制请求；512 KiB 原文编码后仍小于现有
RouterAgency 1 MiB 请求上限。Web 与 Provider 都校验解码长度和 UTF-8，配置内容不进入共享内存包数据面。
列表和历史只返回元数据，采用稳定游标分页且单页最多 100 条；错误固定映射为 400（字段/语法）、404（通道或
revision 不存在）、409（版本冲突）、413（内容超限）、503（Provider 不可用）和 500（持久化失败）。

“通道管理”左侧新增“配置通道”类别，保持 DataFrame、数据库和 Stream 页面行为不变：

- 列表显示名称、current revision、格式、`schema_id`、内容大小、SHA-256 短摘要和更新时间；顶部提供
  “新建配置通道”。
- 创建表单提供名称、格式、`schema_id`、可选变更说明，以及“选择文件/文本编辑器”二选一输入；不显示或接收
  revision，首次成功后展示 `config.<name>@1`。
- 行操作提供预览、复制 current 精确引用、上传新版本、编辑并发布和查看版本历史；首版不提供删除或重命名。
- 编辑默认载入 current revision，revision 只读展示；发布携带 `expected_current_revision`，HTTP 409 时提示
  内容已更新并要求刷新，不自动覆盖或合并。
- 历史抽屉显示 revision、格式、`schema_id`、摘要、大小、发布时间、`base_revision` 和变更说明，并提供预览、
  下载、复制精确引用及“基于此版本创建新版本”。

## 主链路

### 创建或发布新版本

1. 用户在配置通道页面选择文件或输入文本；Web 形成有界发布命令，revision 始终只读且由系统管理。
2. Provider 校验公共格式与安全边界，计算实际字节摘要，在事务内执行幂等检查和 expected revision 比较。
3. 提交新快照及 current 指针后返回 revision、摘要和长度；页面刷新列表并提供精确引用。任何失败都保留原
   current，且不消耗 revision。

### 消费精确快照

1. 消费者在任务打开阶段从自身参数取得例如 `config.corp-apps@7`，通过
   `IConfigChannelRegistryV1::Resolve()` 取得拥有生命周期的只读快照。
2. 消费者校验 `schema_id` 并解析为任务私有结构，同时在任务诊断中保留引用和 SHA-256；之后只访问内存对象。
3. 即使用户发布 revision 8 或服务重启，绑定 revision 7 的任务和恢复流程仍解析到相同格式、Schema、摘要和内容。

## Feature Task

- `[ ]` T1：交付独立配置快照接口、SQLite 不可变持久化和事务自动 revision，使精确引用在并发发布与进程重启后
  始终解析为同一内容。
- `[ ]` T2：交付 JSON/YAML/XML 的有界安全校验及列表、发布、历史、预览/下载控制面，使非法内容、重复请求和
  版本冲突在提交前后都有明确且不损坏历史的结果。
- `[ ]` T3：在通道管理页面交付配置通道列表、文件/编辑器发布、版本历史和精确引用复制，使用户无需接触服务器
  目录或维护版本号即可管理配置。
- `[ ]` T4：交付重启恢复、并发发布、不可变历史、精确快照消费和部署配置的端到端验收，使新版本发布不改变
  已绑定任务并确保现有三类通道无回归。

## 测试锚点

| 验收面 | 必测断言 |
| --- | --- |
| 系统 revision | 首次/再次成功发布为 1/2；非法内容、超限、冲突和持久化失败不消耗 revision |
| 并发与幂等 | 两个客户端基于同一 current 发布不同内容仅一个成功；重复提交当前相同内容返回当前 revision |
| 不可变历史 | 旧 revision 不能覆盖；基于旧版恢复产生下一 revision；重启后 current 与全部历史一致 |
| 精确解析 | 仅接受 `config.<name>@<revision>`；未知或 latest 明确失败；新发布不改变已持有的旧快照 |
| 格式安全 | 三种合法格式可发布；重复键、YAML tag/alias、XML DTD/外部实体/XInclude 和过深内容被拒绝 |
| 类别隔离 | Config 与 DataFrame/Stream 同名不冲突，配置内容不进入普通 SQL 数据面 |
| 页面行为 | 提供配置通道类别和约定操作；无 revision 输入框；冲突不覆盖；预览按纯文本转义 |
| 分页与部署 | 列表/历史单页不超过 100 条；Provider 使用持久路径恢复；路由、1 MiB 上限和现有页面无回归 |

## 完成出口

1. T1～T4 全部完成；成功发布由系统产生连续可见 revision，旧快照不可变，失败和冲突不产生空版本。
2. JSON/YAML/XML 内容通过有界安全校验并永久保存，服务重启后列表、历史和精确引用保持一致。
3. 通道管理页面可完成创建、上传/编辑发布、历史查看、预览/下载和精确引用复制，且不要求用户维护版本号。
4. 消费接口只解析精确引用并返回拥有生命周期的快照；运行任务不受后续发布影响，逐包热路径不访问控制面。
5. 相关单元、接口、前端、重启/并发端到端测试及完整 CTest 全部通过，文档和部署配置与实现一致。

## 完成证据

待实施。
