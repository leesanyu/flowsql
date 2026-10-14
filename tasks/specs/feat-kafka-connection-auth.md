# Feature: Kafka 连接与认证及通道 Web 管理

标识：`kafka-connection-auth`
状态：`[ ]` 待实施；本文件为规格，全部 Feature Task 尚未执行。
优先级：P1
前置 Feature：`web-console`、`routing-services`、`operator-catalog`（均已完成）。
后续消费者：`kafka-log-source`、`kafka-result-sink`、`kafka-packet-source`、`kafka-packet-sink`。

## 业务意图

让接入不同安全策略 Kafka 集群的用户，在现有 Web“通道管理”中创建、查看、编辑、验证和删除具名 Kafka
连接及 source/sink 通道，复用认证配置并明确知道配置已保存、连接已验证以及数据读写能力是否可用。
后续四类数据能力按同一契约绑定连接、凭证和任务独占客户端，无需各自实现认证与管理页面。

## Non-Goals

- 不实现消息编解码、实体 Schema 映射、packet 转换、消费位点提交、重放或 SQL 数据搬运；由后续四个 Feature 交付。
- 不实现 NPM 多实体直接持续发布、有状态任务迁移、跨崩溃分析恢复或端到端 Exactly-once。
- 不创建 Kafka 集群、Topic、用户、ACL、KDC 或身份提供方；不包含云厂商专用身份适配、Schema Registry。
- 不建设通用密钥管理平台，不把普通 Config Channel 用于密码、私钥或 token，不支持浏览器任意服务器路径或命令。
- 不支持连接/通道重命名、历史配置回退、活动任务热换密码或证书；配置变更在无活动租约时生效。
- 不承诺连接验证等同于实际读写授权或数据能力可用，不发布空 reader/writer 伪装已完成数据通道。

## 核心数据与接口契约

### 连接与具名通道

一个连接可被多个通道引用；连接承担 broker、安全与凭证配置，通道承担角色、Topic 和数据类别。
两类名称均匹配 `[a-z][a-z0-9_-]{0,63}`；连接名不用于 SQL，通道规范引用为 `kafka.<name>`。

| 对象/字段 | 契约 |
| --- | --- |
| `connection.name / revision` | 名称唯一；系统 revision 从 1 递增，修改必须携带 `expected_revision`，不保存配置历史。 |
| `bootstrap_servers` | 1～32 个无重复的 host:port；支持 DNS、IPv4 和带方括号 IPv6；同时验证元数据返回地址的连接行为。 |
| `security_protocol / sasl_mechanism` | 四类协议；每个连接最多一个 SASL 机制，非 SASL 协议禁止 SASL 字段；无隐式回退。 |
| `tls / sasl / credential_bindings` | 严格类型化配置与下一节认证矩阵一致；不接收任意 librdkafka 参数、JAAS 原文或 shell 命令。 |
| `channel.name / revision / connection_name` | 类别固定为 kafka、名称唯一；引用已存在连接，revision 与连接分别管理，修改以 expected 值防覆盖。 |
| `channel.role` | 必填 `source\|sink\|both`；source 仅允许 FROM、sink 仅允许 INTO，both 每个方向独立绑定客户端。 |
| `channel.topic / consumer_group` | 单个非空 Topic；source/both 必填消费组，sink 不携带消费组；消费和发布权限分别判断。 |
| `channel.payload_kind` | 必填 `structured\|packet`，为后续数据适配器选择依据；本 Feature 不填写实体字段、编码或位点策略。 |
| `channel.payload_options` | 创建时允许空对象；后续适配器声明并校验其有界数据配置，公共管理层不猜测格式或接受任意配置。 |

请求 JSON 最大 64 KiB，列表单页最多 100 条、按名称稳定分页；非法类型、重复键、未知字段或不存在引用拒绝。
Kafka Topic 名按 Kafka 规则校验且不通过本 Feature 自动创建；保存配置不要求网络在线，不在提交事务内访问 broker。
后续数据适配器未安装时仍能创建、管理和验证通道配置，SQL 绑定明确返回 `KAFKA_DATA_ADAPTER_UNAVAILABLE`。
数据适配器安装后负责 payload_options 完整性及字段表单；未配置完整时显示 incomplete，并在任务打开前拒绝。
payload_options 按 `source`、`sink` 分别配置；both 的两个方向独立校验与展示，不强制共享编码、Schema 或运行状态。
只允许角色包含的方向；该方向适配器未安装时只接受空对象，避免保存未经契约校验的数据配置。

### 认证矩阵与凭证生命周期

首期支持 Linux 原生和 Docker 部署，使用 librdkafka；实际版本及所需 TLS/SASL/OIDC/GSSAPI 构建特性在 T1 冻结。
服务端 definitions 返回实际支持矩阵和不可用原因；某机制不可用时禁止保存该配置，不切换协议或关闭证书校验。

| 模式 | 配置与保证 |
| --- | --- |
| PLAINTEXT | 无 TLS/SASL；显式选择，无密码或证书字段。 |
| SSL/TLS 服务端认证 | 系统 CA 或 `ca_ref`；强制证书链和主机名校验，元数据中的每个 broker 按其实际地址校验。 |
| SSL/TLS 双向认证 | 上述校验加成对的 `certificate_ref / private_key_ref`，可配置私钥口令；证书/私钥为 PEM。 |
| SASL_PLAINTEXT / SASL_SSL | 均支持下列 SASL 机制；后者叠加 TLS，允许按 broker 要求叠加 mTLS；页面明确传输是否加密。 |
| PLAIN / SCRAM-SHA-256 / SCRAM-SHA-512 | `username / password`；密码使用写入字段或凭证引用，机制严格匹配。 |
| GSSAPI/Kerberos | `principal / service_name / keytab_ref`；部署方提供 krb5/KDC，客户端获取并续期票据，固定 argv 调用 kinit，不提供自定义命令。 |
| OAUTHBEARER | 首期为 OIDC client_credentials：HTTPS token endpoint、client_id、client_secret、可选 scope 与 HTTPS CA；自动获取/刷新 token。 |

OAuth 的身份提供方 HTTPS 信任与 Kafka broker TLS 信任独立；不支持开发用未签名 JWT、仅一次静态 token 或浏览器交互登录。
Kerberos keytab、证书和 CA 使用部署方预置的别名；别名映射到 Scheduler 可读的受控文件，Web 不接受原始路径或文件上传。
密码、私钥口令和 OAuth client_secret 可写入或引用部署凭证；查询只返回 `configured` 和引用标识，不回显值。
引用语法为 `env:<alias>` 或 `file:<alias>`，由部署映射解析；文件秘密限 64 KiB，证书/keytab 限 1 MiB，拒绝越界路径。
写入值存于独立秘密表，以部署方提供的 32 字节主密钥进行 AES-256-GCM 加密；禁止内置密钥回退，主密钥不入库。
编辑秘密采用 `keep\|replace\|clear` 操作；未填写默认 keep，遮罩文本不能作为新密码；所选机制必需秘密不可 clear。
列表、详情、错误、日志和原始客户端回调均不得泄露密码、token、私钥、带秘密的 URL 或秘密摘要。
会话打开时解析引用并拥有凭证内容；密码/证书/client_secret 在该会话内冻结，OAuth token 和 Kerberos 票据按期限续期。
Kerberos 每个会话使用独立 ticket cache，续期子进程使用会话环境；禁止修改进程全局 KRB5CCNAME 或覆盖其他连接票据。
GSSAPI 握手也须绑定同一私有凭证上下文，不能仅隔离 kinit 输出；T1 冻结客户端前验证不同 principal 的并行隔离。
禁用客户端默认 shell kinit 路径，由 Provider 管理固定 argv 的票据获取与续期。
外部文件轮换只在后续新会话重新读取；环境变量更新需重启进程；活动任务不热换配置，不把续期 token 持久化。
认证失败、续期失败及连接断开返回分阶段脱敏状态；退避和总等待有界、取消可唤醒，不降级为明文或正常 EOF。

### 公共绑定与客户端所有权

Provider 为独立 `.so`，在 Scheduler 进程按 `Option → Load → Start` 批次加载，通过新 IID 暴露能力。
持久定义不直接暴露 librdkafka 指针；调用方经 IQuerier 取得服务，以租约绑定任务独占客户端。

```cpp
struct KafkaBindingSnapshotV1 {
    uint32_t contract_version = 1;
    std::string channel_name, connection_name, topic, consumer_group, payload_kind;
    uint64_t channel_revision = 0, connection_revision = 0;
    std::string usage;  // source | sink；无秘密值
};
interface IKafkaClientSessionV1 {
    virtual ~IKafkaClientSessionV1() = default;
    virtual const KafkaBindingSnapshotV1& Snapshot() const = 0;
    virtual int Probe(uint32_t timeout_ms, std::string* redacted_result_json) = 0;
    virtual void Cancel() = 0;
    virtual int QueryInterface(const Guid& iid, void** capability) = 0;
};
interface IKafkaConnectionServiceV1 {
    virtual ~IKafkaConnectionServiceV1() = default;
    virtual int OpenSession(const char* channel_name, const char* usage,
                            IKafkaClientSessionV1** session, std::string* redacted_error) = 0;
    virtual void ReleaseSession(IKafkaClientSessionV1* session) = 0;
};
```

失败返回空 session；Snapshot 借用至 ReleaseSession，跨线程使用须复制；Release 只在方法调用结束后执行。
OpenSession 原子校验角色、冻结两类 revision 并取得租约，在锁外解析凭证和构造客户端；失败归还租约，不等待网络认证。
网络认证由 Probe 或后续数据操作驱动，取消通过已返回会话生效；查询/管理锁不覆盖网络、续期或方法排空。
Cancel 与轮询/认证等待可并发，Release 销毁客户端并归还租约；Provider 停止先取消并排空会话，再释放库和配置存储。
每个 source 会话独立拥有消费组/分区状态，每个 sink 会话独立拥有在途发送状态；both 不共享取消或位点。
后续数据 Feature 通过 QueryInterface 获取独立版本化能力；返回指针借用至 Release，不支持时返回 ENOTSUP 和空值。
新增 reader/writer 能力在后续规格冻结，不改变本接口、不暴露 librdkafka 指针，不让数据走控制面 HTTP。

### 持久化、状态与并发

Scheduler 侧 SQLite 为连接、通道和加密秘密的唯一持久真相；一次修改的引用、revision 和秘密更新在同一事务提交。
连接和通道使用独立表；秘密表只保存 opaque ID、nonce、密文和认证 tag；删除连接同时删除其内部秘密，不删除外部文件。
连接被任一通道引用时禁止删除；连接或通道有活动任务/验证租约时禁止修改和删除；冲突返回 409 并保持原配置。
连接修改使所有引用通道的历史验证结果失效；失败提交不增加 revision，启动恢复失败不能降级成内存成功。
网络不可用或单个外部凭证失效不删除配置、不阻止其他连接恢复；数据库/主密钥无法加载时 Provider 启动明确失败。
公开查询分别返回 `config_status`、`adapter_status`、`verification`、`in_use`；不以单一 connected 掩盖不同就绪条件。
验证结果带连接/通道 revision、时间、阶段和超时预算，不持久化为保证；重启、配置变更或引用内容变化后须重新验证。
`adapter_status` 按方向区分 available/not_installed/incomplete；后续任务打开重做角色、能力与连接检查。

## 第三方组件与部署契约

组件管理沿用仓库 thirdparts 和系统包机制；依赖构建与认证能力属于本 Feature 的交付边界。

| 组件 | 来源与职责 |
| --- | --- |
| librdkafka | 固定上游源码版本、下载 URL 与 SHA256；提供 Kafka 协议和认证集成，由 thirdparts 构建。 |
| OpenSSL | 复用系统开发包和共享运行库，负责 TLS 与凭证 AES-GCM 加密，与同进程现有插件使用一致的系统库。 |
| libcurl | 使用系统开发包和共享运行库，提供 OIDC HTTPS；检查 TLS 后端与 CA，不能以 curl 命令存在代替库能力。 |
| Cyrus SASL、GSSAPI/Kerberos 库及 kinit | 使用系统包，分别检查链接库、动态加载的 SASL GSSAPI 插件和票据工具。 |
| zlib、lz4、zstd 等压缩库 | 按冻结的客户端选项确定来源与版本，明确实际启用的 codec，不依赖上游自动探测静默改变能力。 |
| SQLite、现有 JSON 库 | 复用仓库已有依赖，承载持久定义与管理数据。 |
| Kafka、KDC、OIDC 服务 | 固定版本的测试容器或外部联验服务，由部署方提供生产端，不进入客户端安装包。 |

依赖清单记录名称、来源、版本、源码 SHA256、许可证、构建选项、链接方式及实际能力；系统组件记录包版本和已验证 ABI 范围。
T1 冻结具体版本、平台与 TLS/SASL/GSSAPI/OAUTHBEARER/OIDC 及压缩选项，不使用浮动 latest。
librdkafka 的可复现构建入口为 `thirdparts/librdkafka/librdkafka-config.cmake`，下载校验失败明确停止。
先在 Git 主工作区完成下载/构建/安装预热，再进入 worktree；共用 `.thirdparts_prefix` 和 `.thirdparts_installed`。
缓存指纹覆盖源码/版本、平台、编译器、构建选项及关联系统库 ABI；匹配才复用，不能仅凭库文件存在判断。
匹配缓存的重复构建不重新下载；离线缺缓存明确失败；缓存指纹变化需显式重建，避免多个 worktree 同时写同一缓存。

librdkafka 构建为 PIC 静态库，仅以 PRIVATE 依赖链接 Kafka Provider；OpenSSL、libcurl、SASL、Kerberos 使用系统共享库。
CMake 以 target 表达头文件、构建依赖与完整传递链接需求；公共头文件不引入客户端类型，后续四个 Feature 通过 Provider 复用。
构建时检查所需特性，缺失明确失败；Provider 启动检查实际版本、机制、认证插件与工具，definitions 向 Web 报告可用性及原因。
启动发现认证机制不可用时按既有能力契约禁止保存相应配置；完整 Feature 验收仍要求全部声明机制通过。

原生安装说明与 Docker 运行镜像声明同一能力所需运行包、CA 证书、SASL 插件及 Kerberos 工具，显式提供受控凭证挂载说明。
开发包和源码缓存留在构建环境；运行产物携带必要组件版本与许可证记录，不把缓存或构建机路径作为部署依赖。
部署检查共享库解析、认证插件加载、kinit 可用及 CA 信任，再执行真实认证；ldd 通过不能替代插件、工具与认证验收。
本 Feature 按上述静态客户端方案交付；若调整为动态客户端，须重新冻结契约并完整交付 SONAME、符号链接和库搜索路径。
依赖升级须更新清单与缓存指纹，重跑受影响认证矩阵及原生/Docker 回归后才接受，不能只凭客户端编译成功。

## 控制面与 Web 契约

在现有“通道管理”增加独立“Kafka 通道”类别，提供“通道”和“连接配置”两页签，复用已有页面与 HTTP 路由框架。
不把认证字段塞入只支持基本类型的通用 Stream 表单，也不以现有只发布 source 的块通道管理器承载 sink。
Scheduler 提供 `/channels/kafka/*`，Web 代理为 `/api/channels/kafka/*`；本 Feature 的管理元数据不依赖数据适配器安装。

| 操作 | 请求与返回 |
| --- | --- |
| `definitions/query` | 实际协议/机制能力、角色、字段条件、引用别名及按方向的数据适配器状态；不返回秘密。 |
| `connections/{query,add,modify,remove}` | 分页脱敏连接配置、创建、携带 expected_revision 的修改/删除；检查引用与租约。 |
| `channels/{query,add,modify,remove}` | 分页通道、创建、携带 expected_revision 的修改/删除；保存角色、Topic、组和连接引用。 |
| `tests/{start,query,cancel}` | 验证目标为 connection/channel，可选已保存 revision 或草稿；start 返回 202 与 test_id，query 返回阶段与结果，cancel 幂等。 |

错误采用 HTTP 状态加稳定 code/stage：400 字段、404 未找到、409 revision/引用/使用中、413 超限、
429 验证并发超限、503 Provider/构建能力不可用、500 持久化失败；认证/证书/网络失败或超时在验证结果中逐阶段表达。
验证只进行网络、TLS/SASL 和指定 Topic 元数据检查；不订阅消费组、不拉取消息、不提交位点、不发送测试消息。
客户端关闭 Topic 自动创建；Topic 不存在明确失败。返回实际已检查的 broker 与阶段，未检查完不得标记完整验证成功。
broker 协议无法保证禁止自动创建时返回能力不支持，不回退到可能创建 Topic 的元数据请求。
返回 `read_permission=unverified / write_permission=unverified`，不得声称读写 ACL 已通过；实际权限由后续数据任务验证。
总验证预算 1～30 秒、默认 10 秒，覆盖元数据、全部声明检查及令牌获取；同时最多 8 个验证，完成结果仅驻内存保留 5 分钟。
草稿不入库、不占 revision，保存无需先验证；已保存配置验证使用租约，终态前释放临时客户端、票据缓存与租约。
页面离开/显式取消调用 cancel；客户端掉线时服务端仍按总截止时间清理，不依赖 HTTP 断开信号穿透内部路由。

Web 列表显示名称、角色、Topic、连接、协议/机制、配置 revision、验证时间/阶段、按方向的数据能力和使用中状态。
创建通道选择连接（可就地创建连接）、角色、Topic、数据类别；source/both 显示消费组，sink 隐藏并不提交消费组。
连接表单按协议/机制显示字段；秘密默认空白且标记已配置，支持显式替换/清除；切换机制不提交旧隐藏字段。
连接页面提供新建、详情、编辑、验证、删除及引用通道列表；通道页面提供新建、详情、编辑、验证、删除和引用复制。
删除需页面确认；使用中/有引用时禁用相应操作并展示原因，服务端仍独立校验；409 提示刷新，不自动覆盖。
未保存/保存失败的草稿保留；离开页面清理秘密值，不缓存至 localStorage、URL 或浏览器控制台。
页面明确“配置已保存”“连接验证通过”“数据能力未安装/配置不完整”；此 Feature 完成不展示已可执行数据 SQL。

## 主链路

### Web 创建、验证与管理

1. 页面读取 definitions；用户创建连接并填写认证配置，再创建引用该连接的 source/sink/both 通道。
2. Provider 校验字段、能力及引用，事务保存定义和加密秘密，返回脱敏 revision；网络离线仍可保存合法配置。
3. 用户启动草稿或已存配置验证；页面轮询 test_id，独立客户端分阶段检查并释放，显示结果与未验证的读写权限。
4. 用户编辑/删除携带 expected_revision；有引用、活动租约或并发更新时返回 409，原状态与草稿保持。

### 后续数据任务绑定与认证维护

1. 后续适配器检查 `kafka.<name>` 的角色、payload_options 与数据能力，通过新 IID 打开指定方向的任务会话。
2. Provider 冻结定义和凭证、建立客户端；后续 adapter 执行其数据接口，Provider 维护认证刷新和错误诊断。
3. 取消/停止先结束在途回调与续期工作，再释放客户端及租约；重启按持久定义重新绑定，不继承旧验证成功状态。

## Feature Task

- `[ ]` T1：交付冻结的依赖清单、可复现构建与共享缓存、连接/通道/租约公共契约及持久化，确认实际能力与 GSSAPI 多身份隔离，保证角色隔离、原子配置和重启恢复。
- `[ ]` T2：交付基于系统认证组件的 TLS、PLAIN/SCRAM、Kerberos 和 OAuth 客户端及凭证生命周期，保证标准认证、刷新和失败行为可验证。
- `[ ]` T3：交付脱敏 CRUD、独立连接验证及管理发现控制面，保证保存、验证、使用中保护和数据就绪状态有明确结果。
- `[ ]` T4：交付 Kafka 连接与 source/sink/both 通道的完整 Web 管理，使用户无需手写服务端配置即可完成日常操作。
- `[ ]` T5：交付完整运行时依赖、真实认证矩阵、Web 联验及原生/Docker 部署回归，使标准机制、管理流程和既有通道在最终运行环境中可用。

## 测试锚点与完成出口

| 验收面 | 可执行断言 |
| --- | --- |
| 角色与复用 | source 拒绝 sink 绑定、反向同理；both 双客户端独立；同连接多通道租约、Topic 和组不串用。 |
| 并发与持久化 | 相同 expected revision 的并发修改仅一个成功；有引用不能删连接，租约下不能改删；回滚/重启保持一致。 |
| TLS/认证 | 正确 CA/主机名/mTLS 成功；错误 CA、地址、证书、密码、机制明确失败；两种 SCRAM 分别实测。 |
| 长期凭证 | 两个不同 principal 的 Kerberos 会话不串用票据；keytab 续期及 OAuth HTTPS 到期刷新成功；失败/取消有界退出。 |
| 秘密与能力 | keep/replace/clear 正确；密文损坏或缺主密钥失败；响应/日志无秘密；缺构建特性不回退。 |
| 构建与缓存 | 源码校验失败拒绝；匹配指纹复用且不下载，不匹配不复用；离线缺缓存和缺必要构建特性明确失败。 |
| 依赖与打包 | 客户端私有链接且公共接口无第三方类型；最终环境共享库、SASL 插件、kinit 与 CA 检查及真实认证通过。 |
| 验证副作用 | 草稿不持久化；不创建 Topic、不改变消息/组成员/位点；授权 unverified；取消/超时释放租约和客户端。 |
| Web | 条件字段、角色、引用、脱敏、草稿、409 和 busy 行为正确；保存/验证不冒充数据适配器已可用。 |
| 联验与部署 | 真 HTTP CRUD/验证、同 IID 会话、停止清理；原生/Docker 持久目录和秘密挂载正确，旧通道无回归。 |

T1～T5 全部达到后才完成 Feature；真实环境覆盖四类安全协议、mTLS、PLAIN、两种 SCRAM、GSSAPI 和 OAUTHBEARER。
实现阶段计划目标：`test_kafka_connection_auth`、`test_kafka_channel_admin`、`test_kafka_web_e2e`；新增必要前端测试。
按标准入口 `cmake -B build src` 配置、编译对应 target；完成时全量构建/CTest、clang-format、前端测试和 Vite build 全通过。
真实认证不得只用 mock 代替；无法提供某环境时记录未验收，不能勾选对应任务或宣布 Feature 完成。
第三方依赖先在主工作区预热缓存；运行证据置于 `evidence/kafka-connection-auth/`，用户说明统一为 `docs/kafka.md` 并由 README 链接。

## 完成证据

本 Feature 尚未实施；本节只在实际验证后记录证据，不将规格撰写或客户端文档列为通过的认证结果。
