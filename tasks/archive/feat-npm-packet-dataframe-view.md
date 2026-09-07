# Feature: NPM 数据包 DataFrame 查看

状态：`[x]` 已完成
优先级：P0
前置 Feature：`npm-packet-contract`、`npm-offline-import`、`npm-offline-import-web`（均已完成）

## 业务意图

用户上传 pcap/pcapng 并创建有限 `pcapfile` source 后，可执行：

```sql
SELECT * FROM pcapfile.<name> INTO dataframe.<name>
```

Scheduler 完整消费数据包并创建或覆盖命名 DataFrame。DataFrame 内部保留既有
`PacketSchema()` 及全部原始字节；Web 预览按 packet Arrow 类型安全显示字段，`raw_data`
只显示前 64 字节的小写 hex，并明确原始长度与截断状态。

## Non-Goals

- 不支持 block source 的 `WHERE`、列投影、多 source 或数据库 sink。
- 不新增协议识别或分析算子，不改变 pcapfile 已生成的 packet/layer 内容。
- 不改变 `IBlockStreamChannel`、`IDataFrame`、`DataType` 或 packet Schema ABI。
- 不改变 pcapfile 的有限、一次性消费语义，不增加 reset 或重复回放入口。
- 不在任务响应中返回完整 capture；命名 DataFrame 的内容通过预览接口读取。
- 不允许客户端配置 hex 截断长度；首版固定为 64 字节。

## 数据与预览契约

- DataFrame 内部 Arrow `RecordBatch` 的 schema 必须与 `packet::PacketSchema()` 完全一致，
  包括 `uint8`、`uint16`、fixed-size list/binary 和 schema metadata。
- 多个 packet batch 按输入顺序拼接；行数、`sequence` 和 `raw_data` 不变。
- 每个成功取得的 block 在写入成功或失败后都必须 exactly-once `ReleaseBlock()`。
- packet 预览按 Arrow 实际类型序列化：小整数为 JSON number、fixed-size list 为数组、
  null 为 JSON null、MAC/其他 fixed-size binary 为 hex。
- `raw_data` 单元格返回 `{ "hex", "byte_length", "truncated" }`；`hex` 为小写、无分隔符，
  最多对应前 64 字节，`byte_length` 为完整报文字节数，只有被截断时 `truncated` 为 true。
- packet 预览响应增加 `entity: "packet"`；`page` 默认 1，`page_size` 默认且最大为 100，
  `rows` 始终为总行数，`data` 只包含请求页。普通 DataFrame 响应保持兼容。

## 主链路

1. Scheduler 解析单个 `pcapfile` source 和 `dataframe` sink，建立 source 使用租约。
2. 逐个 Poll packet batch，按 Arrow 原生 schema 顺序追加到临时命名 DataFrame，并释放 block。
3. EOF 后才覆盖注册目标 DataFrame，任务响应只返回状态、行数和 `result_target`。
4. Channels 页面调用 DataFrame preview；Catalog 仅转换当前页 Arrow 值，浏览器不接收完整 raw packet。

## 原子任务

- `[x]` T1：以真实双 batch pcap 锚定无算子 SQL、命名 DataFrame 注册、完整 PacketSchema、行序、
  raw bytes 和 exactly-once release；将 DataFrame append 收敛为 Arrow 原生批拼接。
- `[x]` T2：实现 packet-aware preview 序列化；`raw_data` 使用固定 64 字节小写 hex 截断契约，
  覆盖空包、恰好 64 字节、65 字节、null、小整数和 fixed-size list/binary。
- `[x]` T3：前端渲染 hex/truncated 信息，完成上传 → SQL → DataFrame 分页预览 E2E，并执行相关回归。
- `[x]` T4：消除 Scheduler/Catalog 重复 DataFrame preview 路由，以生产插件顺序验证 Web 始终使用
  Catalog packet-aware serializer。
- `[x]` T5：`INTO dataframe.*` 执行响应只返回状态、行数和目标，不调用全量 `DataFrame::ToJson()`；
  packet 仅在分页 preview 时进行 JSON/hex 序列化。

## Feature 完成出口

1. 精确 SQL 可将真实 pcap/pcapng 的全部 packet 行写入命名 DataFrame。
2. 多 batch 后仍保留完整 PacketSchema、顺序和 raw bytes，block 全部 exactly-once 释放。
3. Web 可安全查看 packet 字段；任意 raw bytes 不作为 UTF-8 输出，单行最多显示 64 字节 hex。
4. 生产插件顺序下 `POST:/channels/dataframe/preview` 只有 Catalog 一个 provider。
5. 相关 Scheduler、DataFrame、Catalog、Web E2E 及 `git diff --check` 全部通过。
6. `INTO dataframe.*` 响应不包含 `data`；无 `INTO dataframe.*` 查询响应保持兼容。
