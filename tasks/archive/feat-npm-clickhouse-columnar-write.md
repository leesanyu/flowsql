# NPM ClickHouse 列式批量写入优化

状态：2026-10-13 已完成并归档；当前验收和观测限制见完成证据。

## 业务意图

让使用 ClickHouse 留存 NPM 分析结果的用户，避免列式结果被重新展开为逐字段 URL 参数而产生高额写入开销；交付预算受控、同步可见的 ArrowStream 批量写入与复用连接，并用同配置结果行/秒及字节/秒证明收益。

前置：npm-ip-family-throughput 已完成；其四后端测量是旧适配路径的证据，不是 ClickHouse 容量上限。本 Feature 单独保存实施前基线及新证据。

## Non-Goals

- 不改 MergeTree、排序键、Schema、结果内容、revision、history/latest/final 或其他数据库写入路径。
- 不新增第三方依赖、线程、通用连接池；不使用异步 INSERT 确认、自动重试、跨批次 exactly-once 或零拷贝承诺。
- 不扩大跨 ProcessBlock 的结果缓存、默认容量和预算；不以增加可见延迟换取吞吐。
- 不重写前序归档规格/性能样本，不将本次单机结果推广为数据库选型排名或 SLA。

## 核心契约

### 冻结接口

保留既有 IDatabaseChannel::WriteArrowBatches。新增独立可选能力 IDatabaseArrowWriteV1，不改变既有接口虚表；调用方从通道查询该能力，DatabaseChannel 转发给支持该能力的 session。

```cpp
interface IDatabaseArrowWriteV1 {
    virtual ~IDatabaseArrowWriteV1() = default;
    virtual int WriteArrowBatchesWithPool(
        const char* table,
        const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
        arrow::MemoryPool* pool) = 0;
};
```

- table 为单一物理表名；批次字段按该表顺序，类型和值完整；0 为成功，非 0 为失败。
- 调用同步结束：成功时所有批次请求已得到成功响应；返回后不保留 batches、pool 或网络 provider。
- pool 为非空借用对象；本次新增的系统列和 Arrow IPC 分配通过预算池计入 pending，原结果列共享所有者，不重复计费。
- IPC buffer 直接作为 HTTP content provider 的拥有型数据源，不再复制为完整 std::string 请求正文。HTTP 实现私有小型 scratch 不宣称受 Arrow pool 硬预算覆盖。
- ClickHouse 托管分析结果必须具有该能力；缺失则在 Open 前明确失败，不静默退回逐字段数据写入。少量目录/运行状态命令继续参数化。

### 数据与生命周期

- 每实体保留原 RecordBatch 的业务列，补齐 __npm_run_id、__npm_task_id；无符号整数、NULL、二进制/IPv6、空字符串及特殊字符无损。
- 每次 INSERT 使用 FORMAT ArrowStream；序列化/预算失败发生在发送前，行计数只在请求成功后增加；提交结果未知沿用现有失败/已确认前缀语义，不自动重放。
- 共用现有 pending 预算池，分配拒绝、请求失败、成功和取消后新增缓冲均释放；保留原调用方 Array/Buffer 的生命周期。
- 驱动持有有界的单个写连接及锁，session 共享其所有权；连接复用不跨身份/数据库配置，不影响独立 snapshot 取消；并发写请求串行使用连接。
- 维持现有逐实体批次、行/字节阈值与 ProcessBlock/Flush/EOF/发布前排空；不引入新积压队列。

## 主链路

1. NPM 已编码列式结果 → 校验实体/Schema/周期 → 预算池构建系统列 → 预算池序列化 IPC → 同步 ArrowStream INSERT → 成功计行 → 释放本次缓冲。
2. 驱动 Connect 创建写连接 → 多个 session 共享并加锁复用 → 返回时释放请求 owner → Disconnect 释放驱动所有权；已有 session 持有有效所有者直至请求结束。

## Feature Tasks

- [x] **T1：交付列式写入与内存生命周期接口、关键回归锚点及可重建基线，使后续性能变化可与同一工作量核对。**
  - [x] T1.1 冻结最小新增能力与同步借用契约，保存当前源码/编译/依赖/二进制身份。
  - [x] T1.2 建立列数组共享、预算拒绝、无损值、失败计数与同步释放的可执行测试锚点。
- [x] **T2：交付预算受控的 NPM→ArrowStream 结果写入，消除逐字段 URL 数据参数并保持全部实体结果等价。**
  - [x] T2.1 用现有预算池构建系统列并通过新增能力写入，保留其他后端路径及结果契约。
  - [x] T2.2 让 IPC writer 和正文 buffer 受同一 pool 控制，并直接发送拥有型 IPC buffer，验证失败前缀/释放。
- [x] **T3：交付安全复用的 ClickHouse 写连接，使连续批次摊薄连接成本且排空、取消与并发语义不变。**
  - [x] T3.1 验证跨 session 连接复用、同驱动并发隔离和连接失效后明确失败，不增加自动重放。
  - [x] T3.2 完成十二实体及 progress/Flush/EOF、未知提交、预算和所有者回归。
- [x] **T4：交付可复核的列式吞吐报告与完整工程验收，证明收益及资源代价后完成归档。**
  - [x] T4.1 至少七次版本交替独立 Release 进程；相同 512 包 mixed RST Basic+Session 的结果行/秒中位数至少为基线 2 倍，结果全字段等价；另用独立列式写入探针比较 256/4096/16384 行批次，不改变生产排空。
  - [x] T4.2 当前完整构建/CTest/Sanitizer/严格编译/格式/Diff 全部通过；报告中包含 packet/s、结果 row/s、Arrow 数据 bytes/s、批量/请求数、CPU/pending/RSS、服务端 part/merge 观察及观测限制。

## 验收边界

- 同配置基线来自本 Feature 实施前的工作树；双方共用依赖、编译优化、CPU、报文、模块及数据库设置，源与二进制摘要可复核。
- 吞吐主计时包含 ProcessBlock、必要 Flush/EOF 与输出释放；解码、业务校验、读回独立报告，nested API 时间不相加。
- 每轮计时 512 input packets、1024 结果行；预热另外 512 packets/1024 rows，分子不包含预热或元数据。
- 服务器日志无法关联到具体 run 时仅作辅助观察，不拿服务器 query_duration 代替完整客户端请求时间。
- 差异只允许 run 标识及 Basic 生成时钟规范化；所有业务字段、实体内序、行数和保留/失败契约必须一致。

## 完成证据

- T1：2026-10-13 12:54 UTC；1048 文件初始摘要，523 生产 C++ 与独立 Release 基线一致；四个基线二进制身份保存；新增接口与两个回归锚点编译通过。实施前传输 CTest 因缺少能力按预期失败，仍须 T2/T3 变绿及 T4 完整验收。
- T2：2026-10-13 13:00 UTC；NPM→共享业务列/预算系统列→预算 IPC buffer→ArrowStream 同步写入已交付；真实三后端十二实体及新增预算/未知提交回归通过30.48秒；传输正文无param_、值等价、池使用已验证，连接复用留 T3。
- T3：2026-10-13 13:10 UTC；传输/并发/连接所有权与七项生命周期/四后端回归7/7，44.36秒；增加4096bytes IPC拒绝用例；默认预算/排空/Schema/MergeTree不变。
- T4：2026-10-13 14:01 UTC；正式七次交替独立Release，63成功进程及7旧16,384行受阻进程；同配置主链路814.44→44,123.39 row/s，中位数54.1765倍、全字段等价，pending峰值1,044,136→653,352bytes。三种Arrow探针逐值读回通过，旧大批次HTTP400空正文诊断及原始失败保留，不换算吞吐比值。
- 当前终端验收：完整构建退出0、完整CTest93/93（214.22秒，0跳过）、ASan/UBSan7/7（77.39秒）、ClickHouse原驱动21/21（0跳过）、8个严格编译单元及12个格式版权文件通过。本次Diff无新增告警；全量构建的既有测试/第三方头告警及观测限制明确写入报告。
- [本地完整报告，未入库](../../evidence/npm-clickhouse-columnar-write.md)、[本地测量，未入库](../../evidence/npm-clickhouse-columnar-write/measurements.json)、[本地验证，未入库](../../evidence/npm-clickhouse-columnar-write/verification.json)；逐轮原始输出、生产差异、源/二进制/依赖身份与摘要清单同目录保存。本次未提交/推送，保留前序工作树。
