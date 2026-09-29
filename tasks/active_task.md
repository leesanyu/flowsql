# 即时工作台

事项：NPM 共享有界 TCP 字节流（`npm-shared-tcp-stream`）T2 实施。
关联 Feature Task：T2 — 共享缓存、独立游标与统一计费；多消费者内容一致，慢消费者和预算不足可控。
当前 Atomic Slice：共享方向组件、独立消费/终结排空和 A7/A8 验证闭环。
状态：已完成；WIP=0。

## 业务意图

让同一方向的协议消费者共享一份确定性字节，各自推进且不互相丢数据，全部保留状态受方向和任务预算共同约束。

## Non-Goals

- 不实现 T3 标签准入、session map、runtime 分发、时间调度和 Cancel 操作门；组件由调用方串行调用。
- 不改变 SQL、生产目录、stream provider 不可用闸门、Basic/Session 算法或既有公共 V1 虚表。
- 不实现应用协议解析，不提交或推送，不提前实施 T3/T4。

## 冻结契约与步骤

- 新增内部 `NpmTcpStreamSharedDirection`，固定消费者集合，事件节点共享，游标独立；回调借用上下文/字节。
- 每次输入或实际水位推进最多通知各可读消费者一次；final drain 必须确认 End，非法 Consume/emitter 错误即使被忽略仍锁存失败。
- T1 sink 增加内部可选拥有权转交路径；共享缓存接管 payload 及已有预算，不产生第二份字节副本；原借用 sink 行为兼容。
- 方向预算适配器将核心未决区间、共享事件、订阅/游标/固定对象合计限制并转记任务 kModuleState；所有堆分配前 Reserve。
- 只回收所有消费者确认的事件前缀；部分 Data 的原始容量保留到整个事件被所有消费者读完。
- 错误诊断含限额类别、session_id、方向、首个保留最早缓存的消费者与失败消费者。消费者名借用任务冻结目录，生命周期覆盖组件及诊断。
- 组件不保存借用 session 指针，输入/水位/End 调用显式提供当前 session；T3 provider 负责拥有时间路径元数据。
- Abort 不发送 End；重入锁存错误，在当前回调退出后清理，避免释放在用游标。跨线程取消仍由 T3 原有操作门协调。
- 先头文件和测试，再实现；测试真实 new/new[] 分配失败、成功输出前缀保留与所有释放路径。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/specs/feat-npm-shared-tcp-stream.md`（仅 T2 状态/证据）
- `src/operators/npm_basic/core/npm_tcp_stream_direction.h`
- `src/operators/npm_basic/core/npm_tcp_stream_direction.cpp`
- `src/operators/npm_basic/core/npm_tcp_stream_shared.h`（新增）
- `src/operators/npm_basic/core/npm_tcp_stream_shared.cpp`（新增）
- `src/operators/npm_basic/CMakeLists.txt`
- `src/tests/test_npm_basic/test_npm_tcp_stream_shared.cpp`（新增）
- `src/tests/test_npm_basic/CMakeLists.txt`

T0/T1/规格和产品报告的已有改动按 `/tmp/flowsql-t2-baseline.json` 保留，副本位于 `/tmp/flowsql-t2-before`。
每次 patch 后检查 `git diff --name-only` 与新增文件；清单外文件不得变化。

## 验收命令

```bash
cmake -B build src
cmake --build build --target test_npm_tcp_stream_shared test_npm_tcp_stream test_npm_basic test_npm_protocol_contract -j8
ctest --test-dir build -R '^test_npm_(tcp_stream_shared|tcp_stream|basic|protocol_contract)$' --output-on-failure
clang-format-18 --dry-run --Werror src/operators/npm_basic/core/npm_tcp_stream_{direction,shared}.{h,cpp} src/tests/test_npm_basic/test_npm_tcp_stream_shared.cpp
git diff --check
git diff --name-only
```

复用 `/tmp/flowsql-t1-sanitizer`，标准 src 入口、`FLOWSQL_NPM_TCP_STREAM_SANITIZERS=ON`；运行两个流目标 ASan/UBSan 和泄漏检测。
全量 CTest、生产 runtime 与跨 batch 集成验证留 T4。

## 时间盒与停止条件

- 2026-09-29 20:55（Asia/Shanghai）开始，以 10～30 分钟为检查点。
- 用户要求持续实施 T2 至完成；检查点后继续同一范围，自主修复本轮编译/测试错误。
- A7/A8、相关回归、核心/共享流 Sanitizer、格式/diff/基线检查通过后勾选 T2，WIP=0 并停止。

## 完成证据

- T2 的 A7/A8 已完成，规格仅新增勾选 T2；T3/T4 未开始。
- 新增共享方向组件及 7 组共享流测试；核心仅增加内部 payload/计费转交和嵌入式预算使用，既有 sink 兼容。
- 快慢消费者使用独立事件/字节游标；共享 payload 单份，所有消费者确认后回收事件前缀，部分读取时原始容量继续计费。
- 方向和任务统一预算覆盖未决核心/共享缓存/对象/订阅游标；实际分配失败遍历、已转交部分事件后失败、两方向任务预算隔离均通过。
- 非法 Consume、消费者/emitter 返回错误和异常锁存；final drain 未确认 End 明确失败；失败/Abort/析构预算归零且不回滚已输出前缀。
- 标准配置与 `test_npm_tcp_stream_shared test_npm_tcp_stream test_npm_basic test_npm_protocol_contract` 四目标构建成功，日志无 Warning/Error。
- 相关 CTest 4/4 通过，0 失败，0.79 秒。首轮任务预算用例同时超过两级限额，改为单独耗尽任务预算后通过；实现的限额优先级保持不变。
- `/tmp/flowsql-t1-sanitizer` 的两个流目标 ASan/UBSan+LSan 2/2 通过，0.07 秒。沙箱 ptrace 限制使首次 LSan 无法执行，获准沙箱外运行后通过。
- 五个修改/新增 C++ 文件 clang-format-18、版权头及 diff 审查通过；清单外已有改动哈希与 `/tmp/flowsql-t2-baseline.json` 一致。
- 2026-09-29 21:24（Asia/Shanghai）完成本轮实现和验收，WIP=0；未接入生产 runtime、未执行全量 Feature 回归、未提交或推送。
