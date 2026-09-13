# Active Task

Feature：NPM 基础分析与模块组合（`npm-basic-analysis`）
原子任务：T1.4 冻结并实现 TCP 生命周期、五元组复用与模块结束通知
状态：已完成

## 业务意图

- 在任务内会话表中用最小 TCP 控制事实区分裸 SYN 重传与新连接证据：已有初始裸 SYN 时，同方向且相同
  初始序列号的裸 SYN 复用原实例；中途抓包后首次出现裸 SYN，或裸 SYN 的方向/初始序列号变化时，旧实例
  以 `tuple_reuse` 结束，当前 SYN 作为新实例的首包并获得新 `session_id`。
- 跟踪双向 FIN 半关闭与 RST：单向 FIN（含同方向重传）不结束会话，双向 FIN 或任意方向 RST 的当前 packet
  先计入基础统计，再以 `closed` 结束并返回拥有型快照；SYN+ACK 和 UDP 不触发 TCP 新建/关闭判据。
- 观察结果显式区分活跃会话借用视图与拥有型结束快照；结束通知按结束快照顺序及模块注册顺序同步调用
  `OnSessionEnd()`，任意模块/Writer 非零结果原样返回并停止后续通知，不向模块暴露已删除表项的悬空借用。

## Non-Goals

- 不实现 NPI `Identify`、协议标签更新、TCP/IP 重组、序列窗口、重传统计、RTT、丢包算法或 client/server 推断。
- 不要求从握手开始，不实现完整 RFC TCP 状态机，不把 SYN+ACK 单独视为新连接证据。
- 不实现 EOF Flush、Cancel/错误清理、周期快照、runtime tick 来源或公共 runtime ABI。
- 不接入算子、Scheduler/runtime、预算服务或 Arrow 输出，不新增模块 IID。
- 不修改 T1.3 事件水位/idle 判据；不进入 T2，不执行 Feature 全量回归、commit 或 push。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/specs/feat-npm-basic-analysis.md`
- `src/plugins/npm_basic/npm_session_key.h`
- `src/plugins/npm_basic/npm_session_key.cpp`
- `src/plugins/npm_basic/npm_session_table.h`
- `src/plugins/npm_basic/npm_session_table.cpp`
- `src/tests/test_npm_basic/test_npm_basic.cpp`

## 验收标准与命令

- packet binding 只在已完成 TCP 边界校验后输出主机序 sequence 与 SYN/ACK/FIN/RST 控制事实；非 TCP 控制事实
  无效，失败路径不修改输出。
- 同方向、相同初始 sequence 的裸 SYN 重传保持原 `session_id`；中途会话后的裸 SYN、方向变化或 sequence
  变化的裸 SYN 结束旧实例并建立新实例；SYN+ACK 不触发 reuse。
- tuple reuse 的旧快照不含新 SYN，新实例以该 SYN 为首包；结束快照原因是 `tuple_reuse`，新 ID 不复用。
- 单向 FIN 与同方向 FIN 重传保持活跃；观察到双向 FIN 或任意 RST 时，结束 packet 先计数再以 `closed`
  快照退役；UDP 不进入 TCP 生命周期判断。
- 普通观察返回活跃借用视图；FIN/RST 关闭只返回拥有型结束快照；所有错误保持表与输出不变。
- 模块通知按结束快照顺序、每个会话按注册顺序执行；回调只在调用期间借用快照自身，传入快照原因；任意
  非零返回值原样传播并停止后续模块。
- `cmake --build build --target test_npm_basic -j$(nproc)`
- `ctest --test-dir build -R '^test_npm_basic$' --output-on-failure`
- `git diff --check`
- `git diff --name-only`
- `git status --short --untracked-files=all`

## 时间盒与停止条件

- 时间盒：30 分钟。
- SYN 重传/reuse、FIN/RST、结束 packet 计数和模块通知断言全部通过后，只勾选 T1.4 与父任务 T1，并立即
  停止；T2 保持未完成。
- 若必须实现 NPI、EOF Flush、runtime ABI、预算或算子接入才可完成，则记录阻塞并停止。
- 若发现 P0/P1 级 ABI、内存或并发缺陷，先记录证据并重新切片；其他问题归属后续任务。

## 执行前基线

- 基线提交：`bf49c60`（`docs: clarify NPM feature spec ownership`）。
- T0.1～T0.4、T1.1～T1.3 已完成但尚未提交；本任务保留累计改动，不撤销或扩大这些已验证改动。
- 事实核查：TCP flags、sequence/ack 来自已解析 `TcpHeader`，sequence 必须从网络序转换；会话表不得重新定位
  未校验的原始 TCP 头。原 `Observe(..., NpmSessionView*)` 无法安全表达删除后的结束会话，因此本任务以
  “活跃借用视图 + 拥有型结束快照”的单一观察结果取代旧入口，不保留绕过生命周期的旧重载。
- T1.3 idle 已在删除活跃表项后返回拥有型快照；T1.4 沿用同一所有权模型，模块通知期间借用快照，通知完成后
  才释放快照对象，语义上不会访问已删除 map 节点。

## 完成证据

- 测试先行红灯成立：公共头文件与 T1.4 断言先落地后，定向目标只因旧 `MakeSnapshot()`/`Observe()` 实现
  尚未匹配新签名而编译失败；补齐最小生命周期实现后，同一目标转绿。
- `BuildNpmSessionPacketBinding()` 只在 TCP 头长度、offset、payload 边界全部通过后复制 SYN/ACK/FIN/RST，
  sequence 经 `ntohl()` 转为主机序；UDP 的 TCP 控制事实保持无效，失败输出哨兵保持不变。
- 中途 packet 后的裸 SYN 将旧 ID 1 以 `tuple_reuse` 退役并建立 ID 2；同方向/同 sequence SYN 重传保持
  ID 2，SYN+ACK 不触发 reuse；sequence 变化与方向变化依次建立 ID 3、ID 4，旧快照均不包含当前新 SYN。
- 单向 FIN 与同方向 FIN 重传保持 ID 1 活跃；反向 FIN 先计入第 4 个 packet 再输出 `closed` 快照。RST 同样
  先计入第 2 个 packet 再立即关闭；UDP 连续 packet 保持活跃且不产生 TCP 结束快照。
- `NpmSessionObserveResult` 对普通/reuse/关闭分别表达“活跃”“旧快照+新活跃”“仅结束快照”；错误路径保持
  表和观察结果哨兵不变。reuse/关闭均删除对应旧 deadline，结束视图只借用拥有型快照自身的键。
- `NotifyNpmSessionEnd()` 按两个快照 × 两个模块验证调用顺序为 11、12、21、22，并传递各自结束原因；模块
  返回 `EBUSY` 或 writer 返回 `EIO` 时原样返回，后续模块未被调用。
- 规格中的 T1.1～T1.4 保持在父任务 T1 下；本次只勾选 T1.4 与全部完成的父任务 T1，T2 保持未完成。
- `cmake --build build --target test_npm_basic -j$(nproc)` 通过；
  `ctest --test-dir build -R '^test_npm_basic$' --output-on-failure` 精确执行 1 个用例，1/1 通过。
- `git diff --check` 无输出；累计新增 C++ 无尾随空白、无超过 120 列的行。环境无 `clang-format`，未执行
  Feature 全量构建/CTest、commit 或 push；最终状态仅包含 T0～T1.4 的累计允许范围文件。
