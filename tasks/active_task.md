# 即时工作台

事项：实施 `npm-protocol-analysis` T2 独立实体时间与终结管理
关联 Feature Task：`tasks/specs/feat-npm-protocol-analysis.md` T2。
当前 Atomic Slice：把模块事件 deadline、单调 watermark 通知、EOF Finish 和失败/取消 Abort 接入 T1 runtime。
状态：已完成

## 业务意图

- 使协议模块维护的事务能在端口会话仍存活或根本没有端口会话时按采集事件时间到期。
- 让正常 EOF、tuple reuse、处理失败和取消具有互斥且可验证的终结语义，不泄漏模块状态或重放终态。
- 用户本轮明确授权 T2 直至完成；本工作台只承载 T2，不进入 T3～T4。

## Non-Goals

- 不实现 T3 的统一 emitter/router、附加 consumer、run_id、Arrow owner 或前台/存储结果交付。
- 不实现具体协议解析、TCP 重组、持久化或生产实时采集适配器，不修改模块配置与输入订阅语义。
- 不把模块实体状态提升为 runtime 通用容器；entity ID、revision、finality 和对应预算仍由所属模块维护。
- 不修改 T0 契约或已交付的 Basic/Session Schema；不运行全量 CTest，不 commit/push。

## 冻结接口与测试契约

- `NpmProtocolModuleAdapter` 串行转发 `OnTime`、`Finish`、幂等 `Abort`，正常 Finish 后析构不再 Abort；
  callback 内取消在当前回调返回后终止处理，不再分发后续输入或启动正常 Finish。
- packet/control 成功分发并推进唯一 capture progress 后，只有单调 watermark 实际前进才通知所有协议模块；
  realtime backlog 未知/存在、idle 未确认及 watermark 不变/回退均不触发事务时间。
- EOF 顺序固定为剩余 session end → 每个协议模块一次 Finish → 前台 drain；任一步失败只进入失败清理，
  未 Finish 的模块 Abort。重复 Flush/Cancel 不重放模块回调。
- runtime 提供任务内 `NpmMaintenancePlanV1` 快照：聚合 session 与模块最早事件 deadline；周期输出仅使用
  初始化后的 monotonic snapshot deadline，不把事件 epoch 时间映射为单调时钟。
- 测试模块生成同 session 多实体及 control 无 session 实体，自己维护 deadline、revision/final 状态并计入
  `kModuleState`；测试只观察生命周期轨迹，不调用 T3 尚未交付的 Emit。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/specs/feat-npm-protocol-analysis.md`
- `src/operators/npm_basic/core/npm_module_catalog.h`
- `src/operators/npm_basic/core/npm_module_catalog.cpp`
- `src/operators/npm_basic/core/npm_basic_task_runtime.h`
- `src/operators/npm_basic/core/npm_basic_task_runtime.cpp`
- `src/operators/npm_basic/core/npm_eof_flusher.h`
- `src/operators/npm_basic/core/npm_eof_flusher.cpp`
- `src/operators/npm_basic/core/npm_packet_processor.h`
- `src/operators/npm_basic/core/npm_packet_processor.cpp`
- `src/operators/npm_basic/core/npm_session_table.h`
- `src/tests/test_npm_basic/test_npm_basic.cpp`

本轮修改跨越既有唯一输入链与终结器，是 T2 生命周期保证的必要接线；不新增生产源文件或 CMake 接线。
工作区已有 T0/T1 和 Backlog 未提交差异均保留；每次 patch 后以 `git diff --name-only` 检查不得越界。

## 验收命令

```bash
cmake -B build src
cmake --build build --target test_npm_basic test_npm_protocol_contract -j8
ctest --test-dir build -R '^(test_npm_basic|test_npm_protocol_contract)$' --output-on-failure
# 对本轮 C++ 文件运行 clang-format-18 --dry-run --Werror；既有大测试文件只检查新增区域。
git diff --check
git diff --name-only
git status --short --untracked-files=all
```

## 时间盒与停止条件

- 时间盒：30 分钟；开始于 2026-09-23 14:08（Asia/Shanghai）。
- T2 全部锚点通过构建、测试、格式与 Diff 审查后，记录证据并勾选 T2，立即停止，不进入 T3。
- 到期仍保留 T2 编号，只报告可验证检查点或明确错误，不增加规格层级或扩大范围。

## 完成证据

- T2 于 2026-09-23 15:14（Asia/Shanghai）完成；实际执行约 66 分钟，超过冻结的 30 分钟时间盒。期间未扩大
  到 T3、未新增生产文件或规格层级；偏差来自生命周期/取消/失败路径测试及最终 Diff 收敛，按实际证据记录。
- 已在协议 adapter 接入事件 deadline、单调 watermark `OnTime`、正常 EOF 单次 `Finish` 与幂等 `Abort`；
  callback 内取消由原子信号在当前回调返回后终止处理，重复取消或终态入口不重放回调。
- 离线 packet/control 仅在 capture watermark 实际前进时通知模块；实时 backlog 未知/存在、idle 未确认以及
  水位不变/回退不触发事务时间。`MaintenancePlan()` 分别聚合事件时间和单调 snapshot deadline，不跨时钟换算。
- 新增生命周期测试模块自行维护并计费同 session 多实体和 control 无 session 实体；覆盖 deadline 等号边界、
  活动会话内事务到期、终结 packet 顺序、tuple reuse、observing 独立、正常 EOF、Finish 失败、重复取消、
  回调内取消和预算部分预留失败后的完整归还，最终 module state 预算均归零。
- `cmake -B build src` 通过，`FLOWSQL_FLOW_LABELING=ON`；生产库、`test_npm_basic` 和
  `test_npm_protocol_contract` 构建通过，无编译 Error。
- 最终定向 CTest 2/2、0 失败（0.80 秒）；本轮生产 C++ 文件及既有测试文件新增区域通过
  `clang-format-18 --dry-run --Werror`，`git diff --check` 通过。
- 已审查 T2 Diff 并检查允许文件；T0/T1 与 Backlog 既有未提交差异保留。T3 统一结果路由尚未实施，
  协议 Emit 继续返回 ENOTSUP；未运行全量 CTest、未 commit/push。本 Atomic Slice 完成后停止。
