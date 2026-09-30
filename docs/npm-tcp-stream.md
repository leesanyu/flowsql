# NPM 共享有界 TCP 字节流接入说明

共享流是 `npm.basic` 任务内部供协议模块使用的被动捕获字节视图。生产模块目录目前仍只有 Basic 和 Session；配置流参数不会单独启用重组，也不会让 SQL 中出现 `tcp_stream` feature 或新的结果实体。现有离线 Basic/Session SQL 无需变更。

## 模块接入

协议模块继续实现 `INpmProtocolModuleV1`，同一对象额外实现 `INpmTcpStreamConsumerV1`。准备模块时声明 TCP input mask、`requires_labeling=true`、`requires_tcp_stream=true` 及非空且有效的 `primary_label_ids`；工厂返回的 `NpmModuleInstanceV1::tcp_stream_consumer` 指向该 `protocol` 对象。Open 校验声明、标签目录及对象归属后冻结订阅。生产 SQL catalog 当前不注册测试消费者，后续协议模块须在自己的 Feature 中注册并定义类型化结果。

每个会话首次分类得到的主标签决定消费者集合；标签 0、未命中订阅和没有订阅时不创建流状态。命中后，只有出现有效 TCP 输入的方向才建立状态。流身份为任务内的 `(session_id, direction)`，A→B/B→A 只是规范化方向，不代表客户端/服务端。观测域由现有会话身份隔离，tuple reuse 先终结旧实例再接收新实例。

模块原有 `OnInput(kTcpPacket)` 仍接收 segment facts；共享 provider 在该包的全部模块 packet 回调结束后只摄取一次。模块通过 `OnTcpStreamReadable(context, cursor, emitter)` 读取 Data、Gap、End。每次回调可循环 `Peek`/`Consume`：Data 消费 1 到当前字节数，Gap/End 用 `Consume(0)` 确认；普通通知可暂缓读取，`final_drain=true` 必须读到并确认 End。游标、会话视图及 Data 字节只在当前同步回调内有效；跨回调需要保留的解析片段由模块自行复制并计入自己的有界状态。Data 分块不表示应用消息边界，模块自行完成消息定界和事务关联。

## 捕获视图与资源边界

首个序号锚点确定方向偏移 0；有 SYN 时从 ISN+1 起算，无 SYN 时按首个声明 payload 或 FIN 起算，不伪造未知前缀。重传和重叠采用首次捕获可见的字节，乱序区间在补齐或确认缺口后才越过。Gap 表示本捕获视图中可证明的缺失范围，可能有捕获截断依据，不等于网络丢包。End 结束该方向的视图，不保证应用消息完整。已发布的 Gap 和字节不会被晚到包改写。

`parameters.core.tcp_stream` 可省略；仅在已启用合法消费模块时才会创建 provider。V1 参数如下：

| 字段 | 默认值 | 范围 | 作用 |
| --- | ---: | ---: | --- |
| `max_buffered_bytes_per_direction` | 1048576 | 65536～67108864 | 单方向共享缓存、事件和游标的逻辑计费上限 |
| `gap_timeout_ns` | 1000000000 | 0～60000000000 | 首次证明缺口后等待 capture watermark 的时长 |

配置仍放在 `parameters` 的 V1 JSON 信封内，例如 `{"schema_version":1,"core":{"tcp_stream":{"gap_timeout_ns":500000000}}}`。缺口由 capture watermark 到期或会话终结提交，不由墙钟触发；`gap_timeout_ns=0` 也需要水位到达缺口的证明时间。共享 payload 只保存一份，每个消费者有独立游标，所有消费者越过的前缀才可回收。方向限额、任务预算或分配失败会明确终止任务；不会静默丢字节或驱逐慢消费者。逻辑任务预算不等于进程 RSS。

终结包先完成 packet 与流分发，再进行流 final drain 和模块 `OnSessionEnd`；EOF 先排空全部流，再依次完成模块及结果消费者 Finish。水位推进先提交到期 Gap 和可读通知，再调用模块 `OnTime`。失败/取消只执行 Abort/Cancel，不伪造正常 End/Finish。流事件可通过模块的 `INpmResultEmitterV1` 发射类型化实体，由现有结果 router 同时送往已配置结果消费者；`observing` 只选择前台 DataFrame 实体。

## 验证入口

```bash
cmake -B build src
cmake --build build --target test_npm_basic test_npm_tcp_stream test_npm_tcp_stream_shared test_npm_protocol_contract -j8
ctest --test-dir build -R '^test_npm_(basic|tcp_stream_runtime|tcp_stream_shared|tcp_stream|protocol_contract)$' --output-on-failure
```

测试目录中的 `stream_probe` 仅验证跨 RecordBatch、Data/Gap/End 类型化路由、输入 owner 释放、Basic/Session 共存及前台 observing 行为；它不是生产 SQL 模块。协议模块接入前应阅读 `src/operators/npm_basic/npm_tcp_stream_contract.h` 和 `src/operators/npm_basic/npm_protocol_contract.h` 的借用生命周期与结果 Schema 契约。
